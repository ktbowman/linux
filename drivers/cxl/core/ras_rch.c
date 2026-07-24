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
		/*
		 * Map the physical AER capability, header through the Error
		 * Source Identification register. struct aer_capability_regs
		 * is a software layout whose embedded struct pcie_tlp_log is
		 * larger than the on-wire capability.
		 */
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


static void cxl_rch_enable_rcec(struct cxl_port *endpoint)
{
	struct device *dev = endpoint->uport_dev->parent;
	struct pci_dev *pdev, *rcec;

	if (!dev_is_pci(dev))
		return;

	pdev = to_pci_dev(dev);
	if (pci_pcie_type(pdev) != PCI_EXP_TYPE_RC_END)
		return;

	rcec = pdev->rcec;
	if (!rcec || !pcie_aer_is_native(rcec))
		return;

	pci_aer_unmask_internal_errors(rcec);
	pci_info(rcec, "CXL: Internal errors unmasked\n");
}

/*
 * RCH RAS setup lives here alongside the AER mapping and RCEC helpers it
 * drives. The RCRB is not mapped until the cxl_pci driver attaches to the
 * RCIeP, so this runs from the Endpoint Port probe rather than at dport add
 * time. Take the Endpoint Port and recover its parent dport internally so
 * the RCEC enablement can stay local to this file.
 */
void devm_cxl_dport_rch_ras_setup(struct cxl_port *endpoint)
{
	struct cxl_dport *dport = endpoint->parent_dport;
	struct pci_host_bridge *host_bridge;

	devm_cxl_dport_ras_setup(dport);

	host_bridge = to_pci_host_bridge(dport->dport_dev);
	if (host_bridge->native_aer) {
		cxl_dport_map_rch_aer(dport);
		cxl_disable_rch_root_ints(dport);
	}

	cxl_rch_enable_rcec(endpoint);
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
	struct aer_capability_regs aer_regs;
	struct cxl_dport *dport;

	struct cxl_port *port __free(put_cxl_port) =
		cxl_pci_find_port(pdev, &dport);
	if (!port)
		return;

	guard(device)(port->uport_dev);
	dport = cxl_find_dport_by_dev(port, pdev->dev.parent);
	if (!dport)
		return;

	if (!cxl_rch_get_aer_info(dport->regs.dport_aer, &aer_regs))
		return;

	/*
	 * Snapshot aer_regs before handling the correctable error:
	 * pci_print_aer() takes it by pointer and rewrites uncor_status/
	 * uncor_mask in place on the Advisory Non-Fatal Error path. Use the
	 * copy for the uncorrectable dispatch and logging below so neither is
	 * corrupted by the correctable pci_print_aer() call.
	 */
	struct aer_capability_regs uncor_regs = aer_regs;
	u32 uncor_status = uncor_regs.uncor_status & ~uncor_regs.uncor_mask;

	/*
	 * Handle correctable and uncorrectable errors independently; both
	 * may be set in the same pass and cxl_rch_get_aer_info() has already
	 * cleared both status registers.
	 */
	if (aer_regs.cor_status & ~aer_regs.cor_mask) {
		pci_print_aer(pdev, AER_CORRECTABLE, &aer_regs);
		cxl_handle_cor_ras(dport->dport_dev, to_ras_base(port, dport));
	}

	if (uncor_status) {
		int severity = (uncor_status & uncor_regs.uncor_severity) ?
			       AER_FATAL : AER_NONFATAL;

		/*
		 * Log unconditionally. The correctable pci_print_aer() only
		 * logs this via ANFE recursion when aer_compute_anfe_status()
		 * is non-zero. Testing PCI_ERR_COR_ADV_NFAT alone cannot tell
		 * whether it fired, and the HW status is already cleared. A
		 * duplicate line beats a lost error.
		 */
		pci_print_aer(pdev, severity, &uncor_regs);

		cxl_do_recovery(pdev, dport->port, dport);
	}
}

int cxl_rch_handle_error_iter(struct pci_dev *pdev, void *data)
{
	if (!is_cxl_mem_dev(pdev))
		return 0;

	cxl_handle_rdport_errors(pdev);

	return 0;
}
