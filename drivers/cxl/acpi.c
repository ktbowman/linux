// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2021 Intel Corporation. All rights reserved. */
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/acpi.h>
#include <linux/pci.h>
#include "cxlpci.h"
#include "cxl.h"

/* Encode defined in CXL 2.0 8.2.5.12.7 HDM Decoder Control Register */
#define CFMWS_INTERLEAVE_WAYS(x)	(1 << (x)->interleave_ways)
#define CFMWS_INTERLEAVE_GRANULARITY(x)	((x)->granularity + 8)

static unsigned long cfmws_to_decoder_flags(int restrictions)
{
	unsigned long flags = CXL_DECODER_F_ENABLE;

	if (restrictions & ACPI_CEDT_CFMWS_RESTRICT_TYPE2)
		flags |= CXL_DECODER_F_TYPE2;
	if (restrictions & ACPI_CEDT_CFMWS_RESTRICT_TYPE3)
		flags |= CXL_DECODER_F_TYPE3;
	if (restrictions & ACPI_CEDT_CFMWS_RESTRICT_VOLATILE)
		flags |= CXL_DECODER_F_RAM;
	if (restrictions & ACPI_CEDT_CFMWS_RESTRICT_PMEM)
		flags |= CXL_DECODER_F_PMEM;
	if (restrictions & ACPI_CEDT_CFMWS_RESTRICT_FIXED)
		flags |= CXL_DECODER_F_LOCK;

	return flags;
}

static int cxl_acpi_cfmws_verify(struct device *dev,
				 struct acpi_cedt_cfmws *cfmws)
{
	int expected_len;

	if (cfmws->interleave_arithmetic != ACPI_CEDT_CFMWS_ARITHMETIC_MODULO) {
		dev_err(dev, "CFMWS Unsupported Interleave Arithmetic\n");
		return -EINVAL;
	}

	if (!IS_ALIGNED(cfmws->base_hpa, SZ_256M)) {
		dev_err(dev, "CFMWS Base HPA not 256MB aligned\n");
		return -EINVAL;
	}

	if (!IS_ALIGNED(cfmws->window_size, SZ_256M)) {
		dev_err(dev, "CFMWS Window Size not 256MB aligned\n");
		return -EINVAL;
	}

	if (CFMWS_INTERLEAVE_WAYS(cfmws) > CXL_DECODER_MAX_INTERLEAVE) {
		dev_err(dev, "CFMWS Interleave Ways (%d) too large\n",
			CFMWS_INTERLEAVE_WAYS(cfmws));
		return -EINVAL;
	}

	expected_len = struct_size((cfmws), interleave_targets,
				   CFMWS_INTERLEAVE_WAYS(cfmws));

	if (cfmws->header.length < expected_len) {
		dev_err(dev, "CFMWS length %d less than expected %d\n",
			cfmws->header.length, expected_len);
		return -EINVAL;
	}

	if (cfmws->header.length > expected_len)
		dev_dbg(dev, "CFMWS length %d greater than expected %d\n",
			cfmws->header.length, expected_len);

	return 0;
}

struct cxl_cfmws_context {
	struct device *dev;
	struct cxl_port *root_port;
};

static int cxl_parse_cfmws(union acpi_subtable_headers *header, void *arg,
			   const unsigned long end)
{
	int target_map[CXL_DECODER_MAX_INTERLEAVE];
	struct cxl_cfmws_context *ctx = arg;
	struct cxl_port *root_port = ctx->root_port;
	struct device *dev = ctx->dev;
	struct acpi_cedt_cfmws *cfmws;
	struct cxl_decoder *cxld;
	int rc, i;

	cfmws = (struct acpi_cedt_cfmws *) header;

	rc = cxl_acpi_cfmws_verify(dev, cfmws);
	if (rc) {
		dev_err(dev, "CFMWS range %#llx-%#llx not registered\n",
			cfmws->base_hpa,
			cfmws->base_hpa + cfmws->window_size - 1);
		return 0;
	}

	for (i = 0; i < CFMWS_INTERLEAVE_WAYS(cfmws); i++)
		target_map[i] = cfmws->interleave_targets[i];

	cxld = cxl_root_decoder_alloc(root_port, CFMWS_INTERLEAVE_WAYS(cfmws));
	if (IS_ERR(cxld))
		return 0;

	cxld->flags = cfmws_to_decoder_flags(cfmws->restrictions);
	cxld->target_type = CXL_DECODER_EXPANDER;
	cxld->platform_res = (struct resource)DEFINE_RES_MEM(cfmws->base_hpa,
							     cfmws->window_size);
	cxld->interleave_ways = CFMWS_INTERLEAVE_WAYS(cfmws);
	cxld->interleave_granularity = CFMWS_INTERLEAVE_GRANULARITY(cfmws);

	rc = cxl_decoder_add(cxld, target_map);
	if (rc)
		put_device(&cxld->dev);
	else
		rc = cxl_decoder_autoremove(dev, cxld);
	if (rc) {
		dev_err(dev, "Failed to add decoder for %pr\n",
			&cxld->platform_res);
		return 0;
	}
	dev_dbg(dev, "add: %s node: %d range %pr\n", dev_name(&cxld->dev),
		phys_to_target_node(cxld->platform_res.start),
		&cxld->platform_res);

	return 0;
}

__mock struct acpi_device *to_cxl_host_bridge(struct device *host,
					      struct device *dev)
{
	struct acpi_device *adev = to_acpi_device(dev);

	if (!acpi_pci_find_root(adev->handle))
		return NULL;

	if (strcmp(acpi_device_hid(adev), "ACPI0016") == 0)
		return adev;
	return NULL;
}

/*
 * A host bridge is a dport to a CFMWS decode and it is a uport to the
 * dport (PCIe Root Ports) in the host bridge.
 */
static int add_host_bridge_uport(struct device *match, void *arg)
{
	struct cxl_port *root_port = arg;
	struct device *host = root_port->dev.parent;
	struct acpi_device *bridge = to_cxl_host_bridge(host, match);
	struct acpi_pci_root *pci_root;
	struct cxl_dport *dport;
	struct cxl_port *port;
	int rc;

	if (!bridge)
		return 0;

	dport = cxl_find_dport_by_dev(root_port, match);
	if (!dport) {
		dev_dbg(host, "host bridge expected and not found\n");
		return 0;
	}

	/*
	 * Note that this lookup already succeeded in
	 * to_cxl_host_bridge(), so no need to check for failure here
	 */
	pci_root = acpi_pci_find_root(bridge->handle);
	rc = devm_cxl_register_pci_bus(host, match, pci_root->bus);
	if (rc)
		return rc;

	port = devm_cxl_add_port(host, match, dport->component_reg_phys,
				 root_port);
	if (IS_ERR(port))
		return PTR_ERR(port);

	return 0;
}

struct cxl_chbs_context {
	struct device *dev;
	unsigned long long uid;
	resource_size_t chbcr;
};

static int cxl_get_chbcr(union acpi_subtable_headers *header, void *arg,
			 const unsigned long end)
{
	struct cxl_chbs_context *ctx = arg;
	struct acpi_cedt_chbs *chbs;

	if (ctx->chbcr)
		return 0;

	chbs = (struct acpi_cedt_chbs *) header;

	if (ctx->uid != chbs->uid)
		return 0;
	ctx->chbcr = chbs->base;

	return 0;
}

static int add_host_bridge_dport(struct device *match, void *arg)
{
	acpi_status status;
	unsigned long long uid;
	struct cxl_dport *dport;
	struct cxl_chbs_context ctx;
	struct cxl_port *root_port = arg;
	struct device *host = root_port->dev.parent;
	struct acpi_device *bridge = to_cxl_host_bridge(host, match);

	if (!bridge)
		return 0;

	status = acpi_evaluate_integer(bridge->handle, METHOD_NAME__UID, NULL,
				       &uid);
	if (status != AE_OK) {
		dev_err(host, "unable to retrieve _UID of %s\n",
			dev_name(match));
		return -ENODEV;
	}

	ctx = (struct cxl_chbs_context) {
		.dev = host,
		.uid = uid,
	};
	acpi_table_parse_cedt(ACPI_CEDT_TYPE_CHBS, cxl_get_chbcr, &ctx);

	if (ctx.chbcr == 0) {
		dev_warn(host, "No CHBS found for Host Bridge: %s\n",
			 dev_name(match));
		return 0;
	}

	dport = devm_cxl_add_dport(root_port, match, uid, ctx.chbcr);
	if (IS_ERR(dport))
		return PTR_ERR(dport);

	return 0;
}

static int add_root_nvdimm_bridge(struct device *match, void *data)
{
	struct cxl_decoder *cxld;
	struct cxl_port *root_port = data;
	struct cxl_nvdimm_bridge *cxl_nvb;
	struct device *host = root_port->dev.parent;

	if (!is_root_decoder(match))
		return 0;

	cxld = to_cxl_decoder(match);
	if (!(cxld->flags & CXL_DECODER_F_PMEM))
		return 0;

	cxl_nvb = devm_cxl_add_nvdimm_bridge(host, root_port);
	if (IS_ERR(cxl_nvb)) {
		dev_dbg(host, "failed to register pmem\n");
		return PTR_ERR(cxl_nvb);
	}
	dev_dbg(host, "%s: add: %s\n", dev_name(&root_port->dev),
		dev_name(&cxl_nvb->dev));
	return 1;
}

struct pci_host_bridge *cxl_find_next_rch(struct pci_host_bridge *host)
{
	struct pci_bus *bus = host ? host->bus : NULL;

	while ((bus = pci_find_next_bus(bus)) != NULL) {
		host = bus ? to_pci_host_bridge(bus->bridge) : NULL;
		if (!host)
			continue;

		dev_dbg(&host->dev, "PCI bridge found\n");

		/* Must be a root bridge */
		if (host->bus->parent)
			continue;

		dev_dbg(&host->dev, "PCI bridge is root bridge\n");

		return host;
	}

	return NULL;
}

static const struct acpi_device_id cxl_host_ids[] = {
	{ "ACPI0016", 0 },
	{ "PNP0A08", 0 },
	{ },
};

static int __init cxl_restricted_host_probe(struct platform_device *pdev)
{
	struct pci_host_bridge *host = NULL;
	struct acpi_device *adev;

	while ((host = cxl_find_next_rch(host)) != NULL) {
		adev = ACPI_COMPANION(&host->dev);
		if (acpi_match_device_ids(adev, cxl_host_ids))
			continue;

		dev_dbg(&host->dev, "PCI ACPI host found: %s\n",
			acpi_dev_name(adev));
	}

	return 0;
}

static int cxl_acpi_probe(struct platform_device *pdev)
{
	int rc;
	struct cxl_port *root_port;
	struct device *host = &pdev->dev;
	struct acpi_device *adev = ACPI_COMPANION(host);
	struct cxl_cfmws_context ctx;

	/*
	 * For RCH (CXL 1.1 hosts) the probe is triggered by a plain
	 * platform dev which does not have an acpi companion.
	 */
	if (!adev)
		return cxl_restricted_host_probe(pdev);

	root_port = devm_cxl_add_port(host, host, CXL_RESOURCE_NONE, NULL);
	if (IS_ERR(root_port))
		return PTR_ERR(root_port);

	rc = bus_for_each_dev(adev->dev.bus, NULL, root_port,
			      add_host_bridge_dport);
	if (rc < 0)
		return rc;

	ctx = (struct cxl_cfmws_context) {
		.dev = host,
		.root_port = root_port,
	};
	acpi_table_parse_cedt(ACPI_CEDT_TYPE_CFMWS, cxl_parse_cfmws, &ctx);

	/*
	 * Root level scanned with host-bridge as dports, now scan host-bridges
	 * for their role as CXL uports to their CXL-capable PCIe Root Ports.
	 */
	rc = bus_for_each_dev(adev->dev.bus, NULL, root_port,
			      add_host_bridge_uport);
	if (rc < 0)
		return rc;

	if (IS_ENABLED(CONFIG_CXL_PMEM))
		rc = device_for_each_child(&root_port->dev, root_port,
					   add_root_nvdimm_bridge);
	if (rc < 0)
		return rc;

	/* In case PCI is scanned before ACPI re-trigger memdev attach */
	return cxl_bus_rescan();
}

static const struct acpi_device_id cxl_acpi_ids[] = {
	{ "ACPI0017" },
	{ },
};
MODULE_DEVICE_TABLE(acpi, cxl_acpi_ids);

static struct platform_driver cxl_acpi_driver = {
	.probe = cxl_acpi_probe,
	.driver = {
		.name = "cxl_root",
		.acpi_match_table = cxl_acpi_ids,
	},
};

static void cxl_acpi_device_release(struct device *dev) { }

static struct platform_device cxl_acpi_device = {
	.name = "cxl_root",
	.id = PLATFORM_DEVID_NONE,
	.dev = {
		.release = cxl_acpi_device_release,
	}
};

static int __init cxl_host_init(void)
{
	int rc;

	/* Kick off restricted host (CXL 1.1) detection */
	rc = platform_device_register(&cxl_acpi_device);
	if (rc) {
		platform_device_put(&cxl_acpi_device);
		return rc;
	}
	rc = platform_driver_register(&cxl_acpi_driver);
	if (rc)
		platform_device_unregister(&cxl_acpi_device);
	return rc;
}

static void __exit cxl_host_exit(void)
{
	platform_driver_unregister(&cxl_acpi_driver);
	platform_device_unregister(&cxl_acpi_device);
}

module_init(cxl_host_init);
module_exit(cxl_host_exit);

MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS(CXL);
MODULE_IMPORT_NS(ACPI);
