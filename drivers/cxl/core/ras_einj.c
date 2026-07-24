// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 AMD Corporation. All rights reserved. */

#include <linux/pci.h>
#include <linux/aer.h>
#include <linux/debugfs.h>
#include <cxl/event.h>
#include <cxlmem.h>
#include "core.h"

#define AER_REGISTER_SIZE (sizeof(struct aer_capability_regs) / sizeof(u32))
#define RAS_REGISTER_SIZE (CXL_RAS_CAPABILITY_LENGTH / sizeof(u32))

struct cxl_aer_einj {
	int correctable;
	bool is_rch;
	struct mutex *lock;
	struct device *dev;

	/*
	 * These arrays impersonate device MMIO register blocks and are
	 * consumed via readl()/writel(), which byte-swap to/from
	 * little-endian.  Store them as __le32 so injected values round-trip
	 * correctly on both little- and big-endian hosts.
	 */
	__le32 aer_registers[AER_REGISTER_SIZE];
	__le32 ras_registers[RAS_REGISTER_SIZE];
};

static DEFINE_MUTEX(cxl_aer_einj_mutex);

/*
 * Serializes access to the shared cxl_aer_einj state (dev, is_rch,
 * correctable, and the register arrays) between the debugfs writer that
 * arms an injection and the AER worker-thread consumers
 * (to_einj_ras_base()/to_einj_aer_base()) and the module exit path.  It
 * is a leaf lock: it is only ever acquired for these short critical
 * sections and never nested under another cxl_aer_einj lock, so it does
 * not participate in the port device_lock ordering of the error path.
 */
static DEFINE_MUTEX(cxl_aer_einj_state_lock);

static struct cxl_aer_einj cxl_aer_einj = {
	.lock = &cxl_aer_einj_mutex,
};

static const char cxl_aer_einj_usage[] =
	"ssss:bb:dd.f [UCE|CE] AER_STATUS RAS_STATUS [RCH]\n";

static int cxl_aer_inject_error(struct pci_dev *pdev, bool correctable,
				u32 aer_status, u32 ras_status)
{
	/* RCD errors are signaled as internal errors on the associated RCEC */
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_RC_END) {
		if (!pdev->rcec)
			return -ENODEV;
		pdev = pdev->rcec;
	}

	struct aer_error_inj einj = {
		.bus = pdev->bus->number,
		.dev = PCI_SLOT(pdev->devfn),
		.fn = PCI_FUNC(pdev->devfn),
		.domain = pci_domain_nr(pdev->bus),
	};
	int ret;
	int aer_offset;
	int ras_offset;

	if (correctable) {
		einj.cor_status = aer_status | PCI_ERR_COR_INTERNAL;
		aer_offset = PCI_ERR_COR_STATUS / sizeof(u32);
		ras_offset = CXL_RAS_CORRECTABLE_STATUS_OFFSET / sizeof(u32);
	} else {
		einj.uncor_status = aer_status | PCI_ERR_UNC_INTN;
		aer_offset = PCI_ERR_UNCOR_STATUS / sizeof(u32);
		ras_offset = CXL_RAS_UNCORRECTABLE_STATUS_OFFSET / sizeof(u32);
	}

	cxl_aer_einj.correctable = correctable;

	/*
	 * Clear stale register state from a prior injection so that, e.g., a
	 * UCE injection's uncor_status/uncor_severity is not re-read as a
	 * spurious uncorrectable error during a subsequent CE injection.
	 */
	memset(cxl_aer_einj.aer_registers, 0, sizeof(cxl_aer_einj.aer_registers));
	memset(cxl_aer_einj.ras_registers, 0, sizeof(cxl_aer_einj.ras_registers));
	cxl_aer_einj.aer_registers[aer_offset] = cpu_to_le32(aer_status);

	/*
	 * Mark the injected uncorrectable error as fatal so
	 * cxl_handle_rdport_errors() decodes it as AER_FATAL.
	 */
	if (!correctable)
		cxl_aer_einj.aer_registers[PCI_ERR_UNCOR_SEVER / sizeof(u32)] =
			cpu_to_le32(aer_status | PCI_ERR_UNC_INTN);
	cxl_aer_einj.ras_registers[ras_offset] = cpu_to_le32(ras_status);

	ret = aer_inject(&einj);
	if (ret) {
		pr_err("cxl-einj: aer_inject failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static ssize_t cxl_aer_einj_write(struct file *file,
				    const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	char sbdf[16], severity[4], topology[4] = "";
	unsigned int domain, bus, dev, fn;
	u32 aer_status, ras_status;
	struct cxl_dport *dport;
	char buf[128];
	int nargs;
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (count >= sizeof(buf)) {
		pr_err("cxl-einj: input too long (%zu bytes, max %zu)\n", count, sizeof(buf) - 1);
		return -EINVAL;
	}

	if (copy_from_user(buf, ubuf, count)) {
		pr_err("cxl-einj: copy_from_user failed\n");
		return -EFAULT;
	}
	buf[count] = '\0';

	nargs = sscanf(buf, "%15s %3s %x %x %3s", sbdf, severity,
		       &aer_status, &ras_status, topology);
	if (nargs < 4) {
		pr_err("cxl-einj: expected format: <SBDF> <UCE|CE> <aer_status> <ras_status>\n");
		return -EINVAL;
	}

	if (nargs == 5 && strcmp(topology, "RCH") != 0)
		return -EINVAL;

	if (strcmp(severity, "UCE") != 0 && strcmp(severity, "CE") != 0) {
		pr_err("cxl-einj: expected 'UCE' or 'CE', got '%s'\n", severity);
		return -EINVAL;
	}

	if (sscanf(sbdf, "%x:%x:%x.%x", &domain, &bus, &dev, &fn) != 4) {
		pr_err("cxl-einj: invalid SBDF format '%s', expected DDDD:BB:DD.F\n", sbdf);
		return -EINVAL;
	}

	struct pci_dev *pdev __free(pci_dev_put) =
		pci_get_domain_bus_and_slot(domain, bus, PCI_DEVFN(dev, fn));
	if (!pdev) {
		pr_err("cxl-einj: device %s not found\n", sbdf);
		return -ENODEV;
	}

	guard(mutex)(cxl_aer_einj.lock);

	/*
	 * Drop any reference left armed by a prior injection that was never
	 * consumed before overwriting the pending state below.
	 */
	scoped_guard(mutex, &cxl_aer_einj_state_lock) {
		if (cxl_aer_einj.dev && !cxl_aer_einj.is_rch)
			pci_dev_put(to_pci_dev(cxl_aer_einj.dev));
		cxl_aer_einj.dev = NULL;
	}

	struct cxl_port *port __free(put_cxl_port) = find_cxl_port_by_dev(&pdev->dev, &dport);
	if (!port) {
		dev_err(&pdev->dev, "cxl-einj: Failed to find CXL Port.\n");
		return -ENODEV;
	}

	if (!to_ras_base(port, dport)) {
		dev_err(&pdev->dev, "cxl-einj: RAS not initialized.\n");
		return -ENODEV;
	}

	scoped_guard(mutex, &cxl_aer_einj_state_lock) {
		cxl_aer_einj.is_rch = (nargs == 5 && strcmp(topology, "RCH") == 0);
		if (!cxl_aer_einj.is_rch)
			pci_dev_get(pdev);
		cxl_aer_einj.dev = cxl_aer_einj.is_rch ? pdev->dev.parent : &pdev->dev;
	}

	ret = cxl_aer_inject_error(pdev, strcmp(severity, "CE") == 0,
				   aer_status, ras_status);
	if (ret) {
		scoped_guard(mutex, &cxl_aer_einj_state_lock) {
			if (cxl_aer_einj.dev && !cxl_aer_einj.is_rch)
				pci_dev_put(pdev);
			cxl_aer_einj.dev = NULL;
		}
		pr_err("cxl-einj: injection failed for %s: %d\n", sbdf, ret);
		return ret;
	}

	return count;
}

static ssize_t cxl_aer_einj_read(struct file *file, char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	return simple_read_from_buffer(ubuf, count, ppos,
				       cxl_aer_einj_usage,
				       sizeof(cxl_aer_einj_usage) - 1);
}

static const struct file_operations cxl_ras_error_fops = {
	.owner		= THIS_MODULE,
	.read		= cxl_aer_einj_read,
	.write		= cxl_aer_einj_write,
	.llseek		= default_llseek,
};

void __iomem *to_einj_aer_base(struct cxl_dport *dport)
{
	guard(mutex)(&cxl_aer_einj_state_lock);

	if (!cxl_aer_einj.is_rch || !cxl_aer_einj.dev)
		return NULL;

	/*
	 * For RCH, cxl_aer_einj.dev is the CXL host bridge device (the
	 * RCD's parent), which is also the RCH Downstream Port's
	 * dport_dev.
	 */
	if (!dport || cxl_aer_einj.dev != dport->dport_dev)
		return NULL;

	return (__force void __iomem *)cxl_aer_einj.aer_registers;
}

void __iomem *to_einj_ras_base(struct cxl_port *port, struct cxl_dport *dport)
{
	guard(mutex)(&cxl_aer_einj_state_lock);

	if (!cxl_aer_einj.dev)
		return NULL;

	if (dport) {
		if (cxl_aer_einj.is_rch) {
			if (cxl_aer_einj.dev == dport->dport_dev) {
				cxl_aer_einj.dev = NULL;
				return (__force void __iomem *)cxl_aer_einj.ras_registers;
			}
		} else {
			if (cxl_aer_einj.dev == dport->dport_dev) {
				pci_dev_put(to_pci_dev(cxl_aer_einj.dev));
				cxl_aer_einj.dev = NULL;
				return (__force void __iomem *)cxl_aer_einj.ras_registers;
			}
		}
	} else if (!cxl_aer_einj.is_rch) {
		struct device *dev = is_cxl_endpoint(port) ?
			port->uport_dev->parent : port->uport_dev;

		if (dev_is_pci(dev) && cxl_aer_einj.dev == dev) {
			pci_dev_put(to_pci_dev(cxl_aer_einj.dev));
			cxl_aer_einj.dev = NULL;
			return (__force void __iomem *)cxl_aer_einj.ras_registers;
		}
	}

	return NULL;
}

void cxl_ras_einj_init(void)
{
	debugfs_create_file("aer_einj_inject", 0600, cxl_debugfs, NULL,
			    &cxl_ras_error_fops);
}

void cxl_ras_einj_exit(void)
{
	guard(mutex)(&cxl_aer_einj_state_lock);

	if (cxl_aer_einj.dev) {
		if (!cxl_aer_einj.is_rch)
			pci_dev_put(to_pci_dev(cxl_aer_einj.dev));
		cxl_aer_einj.dev = NULL;
	}
}
