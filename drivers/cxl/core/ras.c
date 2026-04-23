// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 AMD Corporation. All rights reserved. */

#include <linux/pci.h>
#include <linux/aer.h>
#include <cxl/event.h>
#include <cxlmem.h>
#include <cxlpci.h>
#include "trace.h"

/* Check that UCE header definition is maintained to keep ABI intact  */
static_assert(CXL_HEADERLOG_TRACE_SIZE_U32 == 128,
	      "rasdaemon ABI requires exactly 128 u32s");

static void cxl_cper_trace_uncorr_prot_err(struct cxl_port *port, struct cxl_dport *dport,
					   u64 serial, struct cxl_ras_capability_regs *ras_cap)
{
	u32 hl[CXL_HEADERLOG_TRACE_SIZE_U32] = {};
	u32 status = ras_cap->uncor_status & ~ras_cap->uncor_mask;
	u32 fe;

	if (hweight32(status) > 1)
		fe = BIT(FIELD_GET(CXL_RAS_CAP_CONTROL_FE_MASK,
				   ras_cap->cap_control));
	else
		fe = status;

	/*
	 * ras_cap->header_log[] holds CXL_HEADERLOG_SIZE_U32 (16) hardware
	 * dwords. Copy them into the front of a zero-filled
	 * CXL_HEADERLOG_TRACE_SIZE_U32 (128) u32 staging buffer so the trace
	 * event memcpy sees a full 512-byte source and the userspace ABI
	 * (rasdaemon) is preserved.
	 */
	memcpy(hl, ras_cap->header_log, CXL_HEADERLOG_SIZE);
	trace_cxl_aer_uncorrectable_error(port, dport, status, fe,
					  hl, serial);
}

static void cxl_cper_trace_corr_prot_err(struct cxl_port *port, struct cxl_dport *dport,
					 u64 serial, struct cxl_ras_capability_regs *ras_cap)
{
	u32 status = ras_cap->cor_status & ~ras_cap->cor_mask;

	trace_cxl_aer_correctable_error(port, dport, status, serial);
}

/**
 * find_cxl_port_by_dev - Use @dev as hint to do a _by_dport or _by_uport lookup
 * @dev: generic device that may either be a companion of port or target dport
 * @dport: optional output; if non-NULL, set to the matched dport for
 * Root Port and Downstream Port lookups, NULL for all other types.
 *
 * Return a 'struct cxl_port' with an elevated reference if found. Use
 * __free(put_cxl_port) to release.
 */
static struct cxl_port *find_cxl_port_by_dev(struct device *dev, struct cxl_dport **dport)
{
	if (dport)
		*dport = NULL;
	if (!dev_is_pci(dev))
		return NULL;

	switch (pci_pcie_type(to_pci_dev(dev))) {
	case PCI_EXP_TYPE_ROOT_PORT:
	case PCI_EXP_TYPE_DOWNSTREAM:
		return find_cxl_port_by_dport(dev, dport);
	case PCI_EXP_TYPE_UPSTREAM:
	case PCI_EXP_TYPE_ENDPOINT:
	case PCI_EXP_TYPE_RC_END:
		return find_cxl_port_by_uport(dev);
	}

	return NULL;
}

void cxl_cper_handle_prot_err(struct cxl_cper_prot_err_work_data *data)
{
	struct cxl_dport *dport = NULL;
	struct device *host;
	unsigned int devfn = PCI_DEVFN(data->prot_err.agent_addr.device,
				       data->prot_err.agent_addr.function);
	struct pci_dev *pdev __free(pci_dev_put) =
		pci_get_domain_bus_and_slot(data->prot_err.agent_addr.segment,
					    data->prot_err.agent_addr.bus,
					    devfn);
	if (!pdev) {
		pr_err_ratelimited("Failed to find CPER device in CXL topology\n");
		return;
	}

	struct cxl_port *port __free(put_cxl_port) = find_cxl_port_by_dev(&pdev->dev, NULL);
	if (port) {
		host = is_cxl_root(port) ? port->uport_dev : &port->dev;
		guard(device)(host);
		if (is_cxl_endpoint(port))
			dport = port->parent_dport;
		else
			dport = cxl_find_dport_by_dev(port, &pdev->dev);
	}

	if (data->severity == AER_CORRECTABLE)
		cxl_cper_trace_corr_prot_err(port, dport, pdev->dsn,
					     &data->ras_cap);
	else
		cxl_cper_trace_uncorr_prot_err(port, dport, pdev->dsn,
					       &data->ras_cap);
}
EXPORT_SYMBOL_GPL(cxl_cper_handle_prot_err);

static void cxl_cper_prot_err_work_fn(struct work_struct *work)
{
	struct cxl_cper_prot_err_work_data wd;

	while (cxl_cper_prot_err_kfifo_get(&wd))
		cxl_cper_handle_prot_err(&wd);
}
static DECLARE_WORK(cxl_cper_prot_err_work, cxl_cper_prot_err_work_fn);

static void cxl_dport_map_ras(struct cxl_dport *dport)
{
	struct cxl_register_map *map = &dport->reg_map;
	struct device *dev = dport->dport_dev;

	if (!map->component_map.ras.valid)
		dev_dbg(dev, "RAS registers not found\n");
	else if (cxl_map_component_regs(map, &dport->regs.component,
					BIT(CXL_CM_CAP_CAP_ID_RAS)))
		dev_dbg(dev, "Failed to map RAS capability.\n");
}

/**
 * devm_cxl_dport_ras_setup - Setup CXL RAS report on this dport
 * @dport: the cxl_dport that needs to be initialized
 */
void devm_cxl_dport_ras_setup(struct cxl_dport *dport)
{
	dport->reg_map.host = dport_to_host(dport);
	cxl_dport_map_ras(dport);
}

void devm_cxl_port_ras_setup(struct cxl_port *port)
{
	struct cxl_register_map *map = &port->reg_map;

	if (!map->component_map.ras.valid) {
		dev_dbg(&port->dev, "RAS registers not found\n");
		return;
	}

	map->host = &port->dev;
	if (cxl_map_component_regs(map, &port->regs,
				   BIT(CXL_CM_CAP_CAP_ID_RAS)))
		dev_dbg(&port->dev, "Failed to map RAS capability\n");
}
EXPORT_SYMBOL_NS_GPL(devm_cxl_port_ras_setup, "CXL");

void __iomem *to_ras_base(struct cxl_port *port, struct cxl_dport *dport)
{
	if (!port)
		return NULL;

	if (dport)
		return dport->regs.ras;

	return port->regs.ras;
}

void cxl_do_recovery(struct pci_dev *pdev, struct cxl_port *port, struct cxl_dport *dport)
{
	struct device *ras_dev = dport ? dport->dport_dev :
					 (port ? port->uport_dev : &pdev->dev);
	void __iomem *ras_base = to_ras_base(port, dport);

	/*
	 * Name both devices: on the RCEC walk the reporter is the RCEC or the
	 * RCD, while the block that cannot be read belongs to the port or
	 * Downstream Port beneath it.
	 */
	if (!ras_base)
		panic("CXL: uncorrectable protocol error on %s with unmapped RAS (reported by %s)",
		      dev_name(ras_dev), dev_name(&pdev->dev));

	if (cxl_handle_ras(port, dport, ras_base, pdev->dsn))
		panic("CXL cachemem error");

	dev_dbg(&pdev->dev,
		"CXL UCE signaled but no CXL RAS status bits set\n");
}

void cxl_handle_cor_ras(struct cxl_port *port, struct cxl_dport *dport,
			void __iomem *ras_base, u64 serial)
{
	void __iomem *addr;
	u32 status, mask;

	if (!ras_base)
		return;

	addr = ras_base + CXL_RAS_CORRECTABLE_STATUS_OFFSET;
	status = readl(addr);
	if (!status)
		return;

	mask = readl(ras_base + CXL_RAS_CORRECTABLE_MASK_OFFSET);

	/*
	 * The mask gates logging when the error is detected, so a bit that is
	 * set while masked was recorded before the mask was raised and never
	 * signaled. Acknowledge everything hardware logged, but report only
	 * what the platform is currently asking to be told about.
	 */
	writel(status, addr);

	if (status & ~mask)
		trace_cxl_aer_correctable_error(port, dport, status & ~mask,
						serial);
}

/* CXL spec rev3.0 8.2.4.16.1 */
static void header_log_copy(void __iomem *ras_base, u32 *log)
{
	void __iomem *addr;
	u32 *log_addr;
	int i;

	addr = ras_base + CXL_RAS_HEADER_LOG_OFFSET;
	log_addr = log;

	for (i = 0; i < CXL_HEADERLOG_SIZE_U32; i++) {
		*log_addr = readl(addr);
		log_addr++;
		addr += sizeof(u32);
	}
}

/*
 * Log the state of the RAS status registers and prepare them to log the
 * next error status. Return 1 if reset needed.
 */
bool cxl_handle_ras(struct cxl_port *port, struct cxl_dport *dport,
		    void __iomem *ras_base, u64 serial)
{
	u32 hl[CXL_HEADERLOG_TRACE_SIZE_U32] = {};
	void __iomem *addr;
	u32 status, mask, err;
	bool reported = false;

	if (!ras_base)
		return false;

	addr = ras_base + CXL_RAS_UNCORRECTABLE_STATUS_OFFSET;
	status = readl(addr);
	mask = readl(ras_base + CXL_RAS_UNCORRECTABLE_MASK_OFFSET);
	/* A dead link returns all 1s from both registers, treat as unmasked */
	if (status == ~0U)
		mask = 0;
	err = status & ~mask;

	/*
	 * The mask gates logging when the error is detected, so a bit that is
	 * set while masked was recorded before the mask was raised and never
	 * signaled. Report only what the platform is currently asking to be
	 * told about, but acknowledge everything hardware logged below so a
	 * suppressed bit cannot resurface as a current error later.
	 *
	 * The acknowledge has to come after the header log copy: clearing the
	 * bit named by First Error Pointer releases the recorded header.
	 */
	if (err) {
		u32 fe;

		/* If multiple errors, log header points to first error from ctrl reg */
		if (hweight32(err) > 1) {
			void __iomem *rcc_addr =
				ras_base + CXL_RAS_CAP_CONTROL_OFFSET;

			fe = BIT(FIELD_GET(CXL_RAS_CAP_CONTROL_FE_MASK,
					   readl(rcc_addr)));
		} else {
			fe = err;
		}

		header_log_copy(ras_base, hl);
		trace_cxl_aer_uncorrectable_error(port, dport, err, fe, hl,
						  serial);
		reported = true;
	}

	writel(status, addr);

	return reported;
}

pci_ers_result_t cxl_pci_error_detected(struct pci_dev *pdev,
					pci_channel_state_t state)
{
	struct cxl_port *port __free(put_cxl_port) = find_cxl_port_by_uport(&pdev->dev);
	bool ue = false;

	if (!port)
		return PCI_ERS_RESULT_DISCONNECT;

	if (is_cxl_restricted(pdev))
		cxl_handle_rdport_errors(pdev);

	scoped_guard(device, &port->dev) {
		if (!port->dev.driver) {
			dev_warn(&pdev->dev,
				 "%s: port disabled, abort error handling\n",
				 dev_name(&port->dev));
			return PCI_ERS_RESULT_DISCONNECT;
		}

		/*
		 * The CXL RAS uncorrectable status is the only signal here
		 * that the error is a CXL internal (protocol) error. A set
		 * UCE bit confirms it and triggers the panic below. On a dead
		 * link readl() returns 0xFFFFFFFF, which sets all UCE bits and
		 * triggers the panic intentionally.
		 *
		 * If RAS is not mapped the read is skipped. Unlike
		 * cxl_do_recovery(), which is reached only after
		 * is_aer_internal_error() has already confirmed a CXL internal
		 * UCE, this path has no such confirmation, so an unmapped RAS
		 * block cannot attribute the error to CXL and must not panic.
		 * The switch cases below then handle AER recovery.
		 */
		ue = cxl_handle_ras(port, NULL, to_ras_base(port, NULL),
				    pdev->dsn);
	}

	/*
	 * CXL.mem UCE means cache coherency is lost. Continuing risks
	 * silent data corruption.
	 */
	if (ue)
		panic("CXL cachemem error");

	switch (state) {
	case pci_channel_io_normal:
		return PCI_ERS_RESULT_CAN_RECOVER;
	case pci_channel_io_frozen:
		dev_warn(&pdev->dev,
			 "%s: frozen state error detected, disable CXL.mem\n",
			 dev_name(port->uport_dev));
		device_release_driver(port->uport_dev);
		return PCI_ERS_RESULT_NEED_RESET;
	case pci_channel_io_perm_failure:
		dev_warn(&pdev->dev,
			 "failure state error detected, request disconnect\n");
		return PCI_ERS_RESULT_DISCONNECT;
	}
	return PCI_ERS_RESULT_NEED_RESET;
}
EXPORT_SYMBOL_NS_GPL(cxl_pci_error_detected, "CXL");

void cxl_handle_proto_error(struct pci_dev *pdev, struct cxl_port *port,
			    struct cxl_dport *dport, int severity)
{
	if (severity == AER_CORRECTABLE)
		cxl_handle_cor_ras(port, dport, to_ras_base(port, dport), pdev->dsn);
	else
		cxl_do_recovery(pdev, port, dport);
}

/*
 * Find the CXL Port, and for a Root Port or Downstream Port the dport, of
 * @pdev. Hold the Port host lock with the driver bound across the RAS access.
 */
void cxl_handle_pdev_proto_error(struct pci_dev *pdev, int severity)
{
	struct cxl_dport *dport;
	struct device *host;

	struct cxl_port *port __free(put_cxl_port) = find_cxl_port_by_dev(&pdev->dev, NULL);
	if (!port) {
		dev_err_ratelimited(&pdev->dev,
				    "Failed to find parent port device in CXL topology\n");
		return;
	}

	host = is_cxl_root(port) ? port->uport_dev : &port->dev;

	guard(device)(host);
	if (!host->driver) {
		dev_err_ratelimited(host,
				    "Port host device is unbound, abort error handling\n");
		return;
	}

	dport = cxl_find_dport_by_dev(port, &pdev->dev);
	if (!dport && (pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT ||
		       pci_pcie_type(pdev) == PCI_EXP_TYPE_DOWNSTREAM)) {
		dev_err_ratelimited(&pdev->dev,
				    "Failed to find dport device in CXL topology\n");
		return;
	}

	cxl_handle_proto_error(pdev, port, dport, severity);
}

static void __cxl_proto_err_work_fn(struct cxl_proto_err_work_data *wd)
{
	/*
	 * An RCEC AER internal error indicates an error in an
	 * associated RCH Downstream Port or RCD device or both.
	 */
	if (pci_pcie_type(wd->pdev) == PCI_EXP_TYPE_RC_EC) {
		pcie_walk_rcec(wd->pdev, cxl_rch_handle_error_iter, wd);
		return;
	}

	cxl_handle_pdev_proto_error(wd->pdev, wd->severity);
}

static void cxl_proto_err_work_fn(struct work_struct *work)
{
	struct cxl_proto_err_work_data wd;

	for_each_cxl_proto_err(&wd, __cxl_proto_err_work_fn);
}

static DECLARE_WORK(cxl_proto_err_work, cxl_proto_err_work_fn);

static void cxl_proto_err_do_flush(void)
{
	flush_work(&cxl_proto_err_work);
}

void cxl_ras_init(void)
{
	cxl_cper_register_prot_err_work(&cxl_cper_prot_err_work);
	cxl_register_proto_err_work(&cxl_proto_err_work,
				   cxl_proto_err_do_flush);
}

void cxl_ras_exit(void)
{
	cxl_unregister_proto_err_work();
	cxl_cper_unregister_prot_err_work();
}
