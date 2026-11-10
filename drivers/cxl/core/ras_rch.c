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
static bool cxl_get_rch_aer_info(void __iomem *aer_base,
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

static void cxl_handle_rdport_errors(struct pci_dev *pdev,
				       struct cxl_port *port,
				       struct cxl_dport *dport,
				       struct aer_capability_regs *aer_regs)
{
	struct aer_capability_regs uncor_regs;
	u32 cor_status, cor_mask, raw_uncor, print_uncor;
	u32 cor_proto, uncor_proto;

	/* pci_print_aer() modifies aer_regs status. Make copy for UCE */
	uncor_regs = *aer_regs;
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
		pci_print_aer(pdev, AER_CORRECTABLE, aer_regs);

		if (cor_proto)
			cxl_handle_proto_error(pdev, port, dport,
					       AER_CORRECTABLE);

		if (cor_status & ~cor_mask & PCI_ERR_COR_ADV_NFAT) {
			/*
			 * The RCRB AER mapping does not cover the PCIe
			 * Capability where Device Status lives, so pass
			 * a synthetic devsta with CED set and NFED
			 * clear.  This is correct: the outer condition
			 * already proves only ERR_COR was signalled, so
			 * aer_compute_anfe_status() classifies the
			 * AER_POSSIBLE_ANFE bits as Advisory and they
			 * are excluded from the logged uncorrectable
			 * status below.
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

/**
 * cxl_get_rcd_aer_info - Read RCD config-space AER registers
 * @pdev: the RCD PCI device
 * @aer_regs: output — populated with AER register snapshot on success
 *
 * Pure reader: snapshots the AER capability registers from PCI config
 * space and checks whether any unmasked internal errors are present.
 *
 * Returns: true if any unmasked internal error is present, false otherwise.
 */
static bool cxl_get_rcd_aer_info(struct pci_dev *pdev,
				  struct aer_capability_regs *aer_regs)
{
	int aer = pdev->aer_cap;
	u32 cor_internal, uncor_internal;
	unsigned int i, tlp_len;
	bool flit;

	if (!aer)
		return false;

	memset(aer_regs, 0, sizeof(*aer_regs));

	pci_read_config_dword(pdev, aer + PCI_ERR_UNCOR_STATUS,
			      &aer_regs->uncor_status);
	pci_read_config_dword(pdev, aer + PCI_ERR_UNCOR_MASK,
			      &aer_regs->uncor_mask);
	pci_read_config_dword(pdev, aer + PCI_ERR_UNCOR_SEVER,
			      &aer_regs->uncor_severity);
	pci_read_config_dword(pdev, aer + PCI_ERR_COR_STATUS,
			      &aer_regs->cor_status);
	pci_read_config_dword(pdev, aer + PCI_ERR_COR_MASK,
			      &aer_regs->cor_mask);

	pci_read_config_dword(pdev, aer + PCI_ERR_CAP, &aer_regs->cap_control);

	flit = FIELD_GET(PCI_ERR_CAP_TLP_LOG_FLIT, aer_regs->cap_control);
	if (flit)
		tlp_len = FIELD_GET(PCI_ERR_CAP_TLP_LOG_SIZE,
				    aer_regs->cap_control);
	else
		tlp_len = PCIE_STD_NUM_TLP_HEADERLOG + PCIE_STD_MAX_TLP_PREFIXLOG;

	tlp_len = min(tlp_len, ARRAY_SIZE(aer_regs->header_log.dw));

	for (i = 0; i < tlp_len; i++) {
		unsigned int off;

		if (i < PCIE_STD_NUM_TLP_HEADERLOG)
			off = PCI_ERR_HEADER_LOG + i * sizeof(u32);
		else
			off = PCI_ERR_PREFIX_LOG +
			      (i - PCIE_STD_NUM_TLP_HEADERLOG) * sizeof(u32);

		pci_read_config_dword(pdev, aer + off,
				      &aer_regs->header_log.dw[i]);
	}

	aer_regs->header_log.header_len = flit ? i :
					   min(i, PCIE_STD_NUM_TLP_HEADERLOG);
	aer_regs->header_log.flit = flit;

	cor_internal = aer_regs->cor_status & ~aer_regs->cor_mask &
		       PCI_ERR_COR_INTERNAL;
	uncor_internal = aer_regs->uncor_status & ~aer_regs->uncor_mask &
			 PCI_ERR_UNC_INTN;

	return cor_internal || uncor_internal;
}

/**
 * cxl_handle_rcd_aer - Log and clear RCD internal AER errors
 * @pdev: the RCD PCI device
 * @aer_regs: AER register snapshot from cxl_get_rcd_aer_info()
 * @has_cor: output — true if correctable internal error was reported
 * @has_uncor: output — true if uncorrectable internal error was reported
 * @severity: output — AER_FATAL or AER_NONFATAL, valid only when @has_uncor
 *
 * Logs correctable and uncorrectable internal errors independently (so
 * neither is dropped when both are present simultaneously), and W1C-clears
 * the full latched status of each class that was consumed.
 *
 * pci_print_aer() mutates the struct it is given: on the Advisory Non-Fatal
 * recursion path it overwrites uncor_status and zeroes uncor_mask. Print
 * from a copy so the uncorrectable W1C below still writes the status that
 * was actually latched.
 */
static void cxl_handle_rcd_aer(struct pci_dev *pdev,
				struct aer_capability_regs *aer_regs,
				bool *has_cor, bool *has_uncor,
				int *severity)
{
	int aer = pdev->aer_cap;
	u32 cor_internal, uncor_internal;

	*has_cor = false;
	*has_uncor = false;

	cor_internal = aer_regs->cor_status & ~aer_regs->cor_mask &
		       PCI_ERR_COR_INTERNAL;
	uncor_internal = aer_regs->uncor_status & ~aer_regs->uncor_mask &
			 PCI_ERR_UNC_INTN;

	/* Log CE and UCE independently — never drop one when both present */
	if (cor_internal) {
		struct aer_capability_regs cor_regs = *aer_regs;
		u32 anfe;

		pci_print_aer(pdev, AER_CORRECTABLE, &cor_regs);

		/*
		 * pci_print_aer() for correctable recurses on
		 * PCI_ERR_COR_ADV_NFAT and logs those bits as
		 * AER_NONFATAL. Exclude them from the uncorrectable
		 * print below so they are not logged twice.
		 */
		anfe = aer_compute_anfe_status(PCI_EXP_DEVSTA_CED,
					       aer_regs->uncor_status,
					       aer_regs->uncor_mask,
					       aer_regs->uncor_severity);
		pci_write_config_dword(pdev, aer + PCI_ERR_COR_STATUS,
				       aer_regs->cor_status);
		aer_regs->uncor_status &= ~anfe;
		*has_cor = true;
	}

	if (uncor_internal) {
		struct aer_capability_regs uncor_regs = *aer_regs;

		*severity = (uncor_internal & aer_regs->uncor_severity) ?
			    AER_FATAL : AER_NONFATAL;

		pci_print_aer(pdev, *severity, &uncor_regs);
		pci_write_config_dword(pdev, aer + PCI_ERR_UNCOR_STATUS,
				       aer_regs->uncor_status);
		*has_uncor = true;
	}
}

int cxl_handle_rch_rcd_errors(struct pci_dev *pdev, void *data)
{
	struct aer_capability_regs rdport_aer_regs;
	struct aer_capability_regs rcd_aer_regs;
	struct cxl_dport *dport;
	bool has_cor, has_uncor;
	int severity;

	if (!is_cxl_mem_dev(pdev))
		return 0;

	struct cxl_port *port __free(put_cxl_port) = cxl_pci_find_port(pdev);
	if (!port)
		return 0;

	/* Endpoint Port — owns the RCD's CXL RAS registers */
	struct cxl_port *ep __free(put_cxl_port) = find_cxl_port_by_uport(&pdev->dev);

	if (!ep)
		return 0;

	/*
	 * Hold the Endpoint Port host lock across the whole handler so the
	 * dport and the RCD RAS mapping cannot be torn down mid-handling.
	 * The status is W1C, so the driver-bound check must also precede
	 * any read or clear, or the error is consumed and dropped.
	 */
	struct device *host = cxl_port_dport_host(ep);

	guard(device)(host);
	if (!host->driver) {
		dev_warn(&pdev->dev,
			 "%s: port host unbound, abort error handling\n",
			 dev_name(host));
		return 0;
	}

	guard(device)(cxl_port_dport_host(port));
	dport = cxl_pci_find_dport(pdev, port);
	if (dport && cxl_get_rch_aer_info(dport->regs.dport_aer,
					  &rdport_aer_regs))
		cxl_handle_rdport_errors(pdev, port, dport, &rdport_aer_regs);

	if (!cxl_get_rcd_aer_info(pdev, &rcd_aer_regs))
		return 0;

	/* RCD Endpoint — read config-space AER, handle internal errors */
	cxl_handle_rcd_aer(pdev, &rcd_aer_regs, &has_cor, &has_uncor, &severity);

	if (has_cor)
		cxl_handle_proto_error(pdev, ep, NULL, AER_CORRECTABLE);
	if (has_uncor)
		cxl_handle_proto_error(pdev, ep, NULL, severity);

	return 0;
}
