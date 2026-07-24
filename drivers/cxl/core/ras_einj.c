// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 AMD Corporation. All rights reserved. */

#include <linux/pci.h>
#include <linux/aer.h>
#include <linux/debugfs.h>
#include <linux/wait.h>
#include <cxl/event.h>
#include <cxlmem.h>
#include "core.h"

#define AER_REGISTER_SIZE (sizeof(struct aer_capability_regs) / sizeof(u32))
#define RAS_REGISTER_SIZE (CXL_RAS_CAPABILITY_LENGTH / sizeof(u32))

/* Max wait for a prior injection to be consumed before arming a new one. */
#define CXL_AER_EINJ_CONSUME_TIMEOUT (5 * HZ)

struct cxl_aer_einj {
	bool is_rch;
	struct mutex *lock;

	/*
	 * The armed injection target. Used only as an opaque token: it is
	 * stored but usage is limited to comparing for equality against the
	 * device in the error path. Not for deref against the device.
	 */
	struct device *dev;

	/*
	 * These arrays impersonate device MMIO register blocks and are
	 * consumed via readl()/writel(), which byte-swap to/from little-endian.
	 *
	 * NOTE: single in-flight injection only. The arrays are a single
	 * shared slot: the producer (cxl_aer_inject_error()) memsets and
	 * repopulates them under cxl_aer_einj_state_lock, while the consumer
	 * fetches the pointer under the same lock but performs its readl()/
	 * writel() loop after the lock is dropped (the loop lives in the
	 * generic RAS handlers, e.g. cxl_handle_ras()/cxl_rch_get_aer_info()).
	 * A new injection is therefore held off until the prior one is
	 * consumed or times out (see the wait in cxl_aer_einj_write()).
	 *
	 * The consumer disarms (and wakes the next writer) inside
	 * to_einj_*_base() while it still holds the pointer, before its
	 * readl()/writel() loop runs. On SMP a woken writer could in principle
	 * repopulate the slot while that loop is still in flight. This is an
	 * accepted limitation of the single-slot design: it requires an admin
	 * (CAP_SYS_ADMIN) to issue back-to-back injections against the same
	 * target faster than the first is consumed, on a debug-only interface.
	 * Fully closing it would need a completion signal from the generic RAS
	 * consumer path (or per-injection buffers), which is out of scope here.
	 *
	 * Supporting multiple in-flight injections would require per-injection
	 * buffers (so a new producer cannot memset a slot a consumer is still
	 * reading) and a completion signal from the consume path; that is out
	 * of scope for this single-slot debug/test interface.
	 */
	__le32 aer_registers[AER_REGISTER_SIZE];
	__le32 ras_registers[RAS_REGISTER_SIZE];
};

static DEFINE_MUTEX(cxl_aer_einj_mutex);

/*
 * Serializes access to the shared cxl_aer_einj state (producer) and
 * to_einj_ras_base()/to_einj_aer_base() (consumers).
 */
static DEFINE_MUTEX(cxl_aer_einj_state_lock);

static struct cxl_aer_einj cxl_aer_einj = {
	.lock = &cxl_aer_einj_mutex,
};

/*
 * Woken when a consumer (or teardown) disarms the pending injection by
 * clearing cxl_aer_einj.dev.
 */
static DECLARE_WAIT_QUEUE_HEAD(cxl_aer_einj_consumed);

/* Disarm the pending injection and wake a writer waiting to re-arm. */
/* Caller must hold cxl_aer_einj_state_lock. */
static void cxl_aer_einj_disarm(void)
{
	put_device(cxl_aer_einj.dev);
	cxl_aer_einj.dev = NULL;
	wake_up(&cxl_aer_einj_consumed);
}

static const char cxl_aer_einj_usage[] =
	"ssss:bb:dd.f [UCE|CE] AER_STATUS RAS_STATUS [RCH]\n";

static int cxl_aer_inject_error(struct pci_dev *pdev, struct device *arm_dev,
				bool is_rch, bool correctable,
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

	/*
	 * Populate the fake register state AND arm the injection target in a
	 * single state-lock section, matching the consumers (to_einj_*_base)
	 * that read both under the same lock. Doing arm+populate atomically
	 * closes the window where a consumer could observe dev != NULL while
	 * the register arrays are still zeroed. Clear stale state first so a
	 * prior UCE's uncor_status/uncor_severity is not re-read as a spurious
	 * uncorrectable error during a later CE injection.
	 */
	scoped_guard(mutex, &cxl_aer_einj_state_lock) {
		memset(cxl_aer_einj.aer_registers, 0, sizeof(cxl_aer_einj.aer_registers));
		memset(cxl_aer_einj.ras_registers, 0, sizeof(cxl_aer_einj.ras_registers));
		cxl_aer_einj.aer_registers[aer_offset] =
			cpu_to_le32(correctable ? (aer_status | PCI_ERR_COR_INTERNAL)
						: (aer_status | PCI_ERR_UNC_INTN));

		/*
		 * Mark the injected uncorrectable error as fatal so
		 * cxl_handle_rdport_errors() decodes it as AER_FATAL.
		 */
		if (!correctable)
			cxl_aer_einj.aer_registers[PCI_ERR_UNCOR_SEVER / sizeof(u32)] =
				cpu_to_le32(aer_status | PCI_ERR_UNC_INTN);
		cxl_aer_einj.ras_registers[ras_offset] = cpu_to_le32(ras_status);

		cxl_aer_einj.is_rch = is_rch;
		cxl_aer_einj.dev = get_device(arm_dev);
	}

	ret = aer_inject(&einj);
	if (ret) {
		/* Injection failed: unwind the arm done above. */
		scoped_guard(mutex, &cxl_aer_einj_state_lock)
			cxl_aer_einj_disarm();
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
	 * A prior injection is consumed asynchronously by the AER worker.
	 * Wait for it to be consumed (cxl_aer_einj.dev cleared) before arming
	 * a new one, so a back-to-back write cannot overwrite the single-slot
	 * injection state before the previous injection has been simulated.
	 *
	 * If the prior injection is never delivered (e.g. a bad SBDF that the
	 * AER path never matches), the wait times out; reclaim the stale slot
	 * and proceed rather than wedge the interface.
	 */
	ret = wait_event_interruptible_timeout(cxl_aer_einj_consumed,
				({
					guard(mutex)(&cxl_aer_einj_state_lock);
					!cxl_aer_einj.dev;
				}),
				CXL_AER_EINJ_CONSUME_TIMEOUT);
	if (ret < 0)
		return ret;
	if (ret == 0) {
		scoped_guard(mutex, &cxl_aer_einj_state_lock)
			cxl_aer_einj_disarm();
	}

	bool is_rch = (nargs == 5 && strcmp(topology, "RCH") == 0);

	struct cxl_port *port __free(put_cxl_port) = find_cxl_port_by_dev(&pdev->dev, &dport);
	if (!port) {
		dev_err(&pdev->dev, "cxl-einj: Failed to find CXL Port.\n");
		return -ENODEV;
	}

	/*
	 * The RAS/AER register blocks are devm-mapped against the port host
	 * device and torn down on driver unbind. Hold the host device lock
	 * across both the mapping validation below and the arm step, so the
	 * target cannot be unbound (unmapping its registers) between the two.
	 * Without this an injection could be armed against a device whose real
	 * RAS base has since gone NULL, panicking cxl_do_recovery() when the
	 * injected AER is consumed. This mirrors the guard(device) taken by the
	 * consumers cxl_handle_rdport_errors()/cxl_pci_error_detected().
	 */
	struct cxl_dport *rch_dport = NULL;
	struct cxl_port *rch_port __free(put_cxl_port) =
		is_rch ? cxl_pci_find_port(pdev, &rch_dport) : NULL;
	struct device *host = is_rch ?
		(rch_port ? rch_port->uport_dev : NULL) :
		(is_cxl_root(port) ? port->uport_dev : &port->dev);

	if (!host) {
		dev_err(&pdev->dev, "cxl-einj: Failed to find CXL Port host.\n");
		return -ENODEV;
	}

	guard(device)(host);
	if (!host->driver) {
		dev_err(&pdev->dev, "cxl-einj: CXL port host unbound, abort.\n");
		return -ENODEV;
	}

	/* Validate RAS is mapped before arming */
	if (is_rch) {
		if (!rch_dport || !rch_dport->regs.dport_aer) {
			dev_err(&pdev->dev, "cxl-einj: RCH Downstream Port AER not initialized.\n");
			return -ENODEV;
		}
	} else if (!to_ras_base(port, dport)) {
		dev_err(&pdev->dev, "cxl-einj: RAS not initialized.\n");
		return -ENODEV;
	}

	/*
	 * Arm+populate happen atomically inside cxl_aer_inject_error() under
	 * the state lock, and it disarms on failure, so there is no separate
	 * arm section here. The arm target is derived from the original pdev
	 * (RCiEP for RCH); cxl_aer_inject_error() re-targets to the RCEC only
	 * for the aer_inject() call itself.
	 */
	ret = cxl_aer_inject_error(pdev, is_rch ? pdev->dev.parent : &pdev->dev,
				   is_rch, strcmp(severity, "CE") == 0,
				   aer_status, ras_status);
	if (ret) {
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
		if (cxl_aer_einj.dev == dport->dport_dev) {
			cxl_aer_einj_disarm();
			return (__force void __iomem *)cxl_aer_einj.ras_registers;
		}
	} else if (!cxl_aer_einj.is_rch) {
		struct device *dev = is_cxl_endpoint(port) ?
			port->uport_dev->parent : port->uport_dev;

		if (dev_is_pci(dev) && cxl_aer_einj.dev == dev) {
			cxl_aer_einj_disarm();
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

	if (cxl_aer_einj.dev)
		cxl_aer_einj_disarm();
}
