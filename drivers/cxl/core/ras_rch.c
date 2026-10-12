// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 AMD Corporation. All rights reserved. */

#include <linux/types.h>
#include <linux/aer.h>
#include "cxl.h"
#include "cxlpci.h"
#include "core.h"
#include "cxlmem.h"

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
					     PCI_ERR_ROOT_ERR_SRC + sizeof(u32));
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
	struct device *dev = endpoint->uport_dev->parent;
	struct pci_dev *pdev;

	if (!dev_is_pci(dev))
		return;

	pdev = to_pci_dev(dev);
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

	if (!dev_is_pci(dport->dport_dev))
		return;

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
	/*
	 * Bound the copy to the physically-defined AER registers (header
	 * through the 16-byte Header Log). struct aer_capability_regs is a
	 * software layout whose embedded struct pcie_tlp_log is larger than
	 * the on-wire AER capability; copying sizeof(*aer_regs) would
	 * over-read the RCRB-mapped MMIO block.
	 */
	int read_cnt = (PCI_ERR_HEADER_LOG + 16) / sizeof(u32);
	u32 *aer_regs_buf = (u32 *)aer_regs;
	int n;

	if (!aer_base)
		return false;

	/*
	 * Zero the destination so the software-only tail fields
	 * (e.g. header_log.header_len) are deterministic rather than
	 * left as uninitialized stack, which could drive a bogus loop
	 * length in pcie_print_tlp_log().
	 */
	memset(aer_regs, 0, sizeof(*aer_regs));

	/* Use readl() to guarantee 32-bit accesses */
	for (n = 0; n < read_cnt; n++)
		aer_regs_buf[n] = readl(aer_base + n * sizeof(u32));

	writel(aer_regs->uncor_status, aer_base + PCI_ERR_UNCOR_STATUS);
	writel(aer_regs->cor_status, aer_base + PCI_ERR_COR_STATUS);

	return true;
}

void cxl_handle_rdport_errors(struct pci_dev *pdev)
{
	struct cxl_dev_state *cxlds = pci_get_drvdata(pdev);
	struct aer_capability_regs aer_regs, uncor_regs;
	u32 cor_status, cor_mask, raw_uncor, print_uncor;
	struct cxl_dport *dport;

	struct cxl_port *port __free(put_cxl_port) =
		cxl_pci_find_port(pdev, NULL);
	if (!port)
		return;

	guard(device)(&port->dev);
	dport = cxl_find_dport_by_dev(port, pdev->dev.parent);
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

	/* Handle CE and UCE errors independently, in the same pass */
	if (cor_status & ~cor_mask) {
		pci_print_aer(pdev, AER_CORRECTABLE, &aer_regs);
		cxl_handle_cor_ras(&cxlds->cxlmd->dev, dport->regs.ras);

		if (cor_status & ~cor_mask & PCI_ERR_COR_ADV_NFAT) {
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
		pci_print_aer(pdev, severity, &uncor_regs);
	}

	if (raw_uncor)
		cxl_handle_ras(&cxlds->cxlmd->dev, dport->regs.ras);
}
