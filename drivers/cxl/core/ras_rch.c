// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 AMD Corporation. All rights reserved. */

#include <linux/aer.h>
#include "cxl.h"
#include "cxlpci.h"
#include "core.h"
#include "cxlmem.h"

static bool is_cxl_mem_dev(struct pci_dev *dev)
{
	/*
	 * The capability, status, and control fields in Device 0,
	 * Function 0 DVSEC control the CXL functionality of the
	 * entire device (CXL 3.0, 8.1.3).
	 */
	if (dev->devfn != PCI_DEVFN(0, 0))
		return false;

	/*
	 * CXL Memory Devices must have the 502h class code set (CXL
	 * 3.0, 8.1.12.1).
	 */
	if ((dev->class >> 8) != PCI_CLASS_MEMORY_CXL)
		return false;

	return true;
}

void cxl_dport_map_rch_aer(struct cxl_dport *dport)
{
	resource_size_t aer_phys;
	struct device *host;
	u16 aer_cap;

	aer_cap = cxl_rcrb_to_aer(dport->dport_dev, dport->rcrb.base);
	if (aer_cap) {
		host = dport->reg_map.host;
		aer_phys = aer_cap + dport->rcrb.base;
		dport->regs.dport_aer =
			devm_cxl_iomap_block(host, aer_phys,
					     PCIE_AER_CAP_HW_SIZE);
	}
}

void cxl_disable_rch_root_ints(struct cxl_dport *dport)
{
	void __iomem *aer_base = dport->regs.dport_aer;
	u32 aer_cmd_mask, aer_cmd;

	if (!aer_base)
		return;

	/*
	 * Disable RCH root port command interrupts.
	 * CXL 3.0 12.2.1.1 - RCH Downstream Port-detected Errors
	 *
	 * This sequence may not be necessary. CXL spec states disabling
	 * the root cmd register's interrupts is required. But, PCI spec
	 * shows these are disabled by default on reset.
	 */
	aer_cmd_mask = (PCI_ERR_ROOT_CMD_COR_EN |
			PCI_ERR_ROOT_CMD_NONFATAL_EN |
			PCI_ERR_ROOT_CMD_FATAL_EN);
	aer_cmd = readl(aer_base + PCI_ERR_ROOT_COMMAND);
	aer_cmd &= ~aer_cmd_mask;
	writel(aer_cmd, aer_base + PCI_ERR_ROOT_COMMAND);
}

void cxl_rch_enable_rcec(struct cxl_port *endpoint)
{
	struct pci_dev *pdev;

	if (!dev_is_pci(endpoint->uport_dev->parent))
		return;

	pdev = to_pci_dev(endpoint->uport_dev->parent);
	if (pci_pcie_type(pdev) != PCI_EXP_TYPE_RC_END)
		return;

	struct pci_dev *rcec __free(pci_dev_put) = pci_rciep_to_rcec(pdev);
	if (!rcec) {
		pci_warn(pdev,
			 "CXL: No associated RCEC found, internal errors not unmasked\n");
		return;
	}

	if (!pcie_aer_is_native(rcec))
		return;

	pci_aer_unmask_internal_errors(rcec);
	pci_info(rcec, "CXL: Internal errors unmasked\n");
}
EXPORT_SYMBOL_NS_GPL(cxl_rch_enable_rcec, "CXL");

/*
 * RCH RAS setup lives here alongside the AER mapping helpers it drives. The
 * RCRB is not mapped until the cxl_pci driver attaches to the RCIeP, so this
 * runs from the Endpoint Port probe rather than at dport add time.
 */
void devm_cxl_dport_rch_ras_setup(struct cxl_dport *dport)
{
	struct pci_host_bridge *host_bridge;

	devm_cxl_dport_ras_setup(dport);

	host_bridge = to_pci_host_bridge(dport->dport_dev);
	if (!host_bridge->native_aer)
		return;

	cxl_dport_map_rch_aer(dport);
	cxl_disable_rch_root_ints(dport);
}
EXPORT_SYMBOL_NS_GPL(devm_cxl_dport_rch_ras_setup, "CXL");

/*
 * Copy the AER capability registers using 32 bit read accesses.
 * This is necessary because RCRB AER capability is MMIO mapped. Clear the
 * status after copying.
 *
 * @aer_base: base address of AER capability block in RCRB
 * @aer_regs: destination for copying AER capability
 */
static bool cxl_rch_get_aer_info(void __iomem *aer_base,
				 struct aer_capability_regs *aer_regs)
{
	/* A flat copy cannot fill the struct; aer_cap_regs_unpack() places it. */
	__le32 raw[PCIE_AER_CAP_HW_SIZE / sizeof(__le32)];

	if (!aer_base)
		return false;

	/*
	 * Use readl() to guarantee 32-bit accesses; it returns host order, so
	 * put the registers back little-endian for aer_cap_regs_unpack().
	 */
	for (int n = 0; n < ARRAY_SIZE(raw); n++)
		raw[n] = cpu_to_le32(readl(aer_base + n * sizeof(u32)));

	aer_cap_regs_unpack(aer_regs, raw, sizeof(raw));

	if (!aer_regs->uncor_status && !aer_regs->cor_status)
		return false;

	writel(aer_regs->uncor_status, aer_base + PCI_ERR_UNCOR_STATUS);
	writel(aer_regs->cor_status, aer_base + PCI_ERR_COR_STATUS);

	return true;
}

void cxl_handle_rdport_errors(struct pci_dev *pdev)
{
	struct aer_capability_regs aer_regs, uncor_regs;
	u32 cor_status, cor_mask, raw_uncor, print_uncor;
	u32 cor_proto, uncor_proto;
	struct cxl_dport *dport;

	struct cxl_port *port __free(put_cxl_port) =
		cxl_pci_find_port(pdev);
	if (!port)
		return;

	/*
	 * The dport may have been freed since the lookup above, so take the
	 * lock that pins it and find it again.
	 */
	guard(device)(cxl_port_dport_host(port));

	dport = cxl_pci_find_dport(pdev, port);
	if (!dport)
		return;

	if (!cxl_rch_get_aer_info(dport->regs.dport_aer, &aer_regs))
		return;

	/* pci_print_aer() modifies aer_regs status. Make copy for UCE */
	uncor_regs = aer_regs;
	cor_status = uncor_regs.cor_status;
	cor_mask = uncor_regs.cor_mask;
	raw_uncor = uncor_regs.uncor_status & ~uncor_regs.uncor_mask;
	print_uncor = raw_uncor;

	/*
	 * Only the internal-error bits carry a CXL protocol error (CXL
	 * r4.0 12.2.1.1). The RCRB AER block holds the Downstream Port's
	 * full PCIe status, so a PCIe uncorrectable error on the RCH link
	 * is not CXL's to handle and must not reach the CXL panic policy.
	 * Every bit is still read, logged and W1C-cleared; only the
	 * protocol-error dispatch is narrowed.
	 */
	cor_proto = cor_status & ~cor_mask & PCI_ERR_COR_INTERNAL;
	uncor_proto = raw_uncor & PCI_ERR_UNC_INTN;

	/* Handle CE and UCE errors independently, in the same pass */
	if (cor_status & ~cor_mask) {
		pci_print_aer(pdev, AER_CORRECTABLE, &aer_regs);

		if (cor_proto)
			cxl_handle_proto_error(pdev, port, dport,
					       AER_CORRECTABLE);

		if (cor_status & ~cor_mask & PCI_ERR_COR_ADV_NFAT) {
			/*
			 * Pass CED set and NFED clear: no ERR_NONFATAL was
			 * signalled alongside this correctable event, so
			 * aer_compute_anfe_status() also folds in the
			 * AER_POSSIBLE_ANFE bits. Exclude those from the
			 * uncorrectable status logged below.
			 */
			u32 anfe = aer_compute_anfe_status(PCI_EXP_DEVSTA_CED,
							   uncor_regs.uncor_status,
							   uncor_regs.uncor_mask,
							   uncor_regs.uncor_severity);

			print_uncor &= ~anfe;
		}
	}

	if (print_uncor) {
		int severity = (print_uncor & uncor_regs.uncor_severity) ?
			       AER_FATAL : AER_NONFATAL;

		uncor_regs.uncor_status = print_uncor;
		uncor_regs.uncor_mask = 0;
		pci_print_aer(pdev, severity, &uncor_regs);
	}

	if (uncor_proto) {
		int severity = (uncor_proto & uncor_regs.uncor_severity) ?
			       AER_FATAL : AER_NONFATAL;

		cxl_handle_proto_error(pdev, port, dport, severity);
	}
}

int cxl_rch_handle_error_iter(struct pci_dev *pdev, void *data)
{
	struct cxl_proto_err_work_data *wd = data;

	if (!is_cxl_mem_dev(pdev))
		return 0;

	/* RCH Downstream Port, located through the RCD */
	cxl_handle_rdport_errors(pdev);

	/* RCD Endpoint */
	cxl_handle_pdev_proto_error(pdev, wd->severity);

	return 0;
}
