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
	int rc, expected_len;
	unsigned int ways;

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

	rc = cxl_to_ways(cfmws->interleave_ways, &ways);
	if (rc) {
		dev_err(dev, "CFMWS Interleave Ways (%d) invalid\n",
			cfmws->interleave_ways);
		return -EINVAL;
	}

	expected_len = struct_size(cfmws, interleave_targets, ways);

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
	struct resource *cxl_res;
	int id;
};

static int cxl_parse_cfmws(union acpi_subtable_headers *header, void *arg,
			   const unsigned long end)
{
	int target_map[CXL_DECODER_MAX_INTERLEAVE];
	struct cxl_cfmws_context *ctx = arg;
	struct cxl_port *root_port = ctx->root_port;
	struct resource *cxl_res = ctx->cxl_res;
	struct cxl_root_decoder *cxlrd;
	struct device *dev = ctx->dev;
	struct acpi_cedt_cfmws *cfmws;
	struct cxl_decoder *cxld;
	unsigned int ways, i, ig;
	struct resource *res;
	int rc;

	cfmws = (struct acpi_cedt_cfmws *) header;

	rc = cxl_acpi_cfmws_verify(dev, cfmws);
	if (rc) {
		dev_err(dev, "CFMWS range %#llx-%#llx not registered\n",
			cfmws->base_hpa,
			cfmws->base_hpa + cfmws->window_size - 1);
		return 0;
	}

	rc = cxl_to_ways(cfmws->interleave_ways, &ways);
	if (rc)
		return rc;
	rc = cxl_to_granularity(cfmws->granularity, &ig);
	if (rc)
		return rc;
	for (i = 0; i < ways; i++)
		target_map[i] = cfmws->interleave_targets[i];

	res = kzalloc(sizeof(*res), GFP_KERNEL);
	if (!res)
		return -ENOMEM;

	res->name = kasprintf(GFP_KERNEL, "CXL Window %d", ctx->id++);
	if (!res->name)
		goto err_name;

	res->start = cfmws->base_hpa;
	res->end = cfmws->base_hpa + cfmws->window_size - 1;
	res->flags = IORESOURCE_MEM;

	/* add to the local resource tracking to establish a sort order */
	rc = insert_resource(cxl_res, res);
	if (rc)
		goto err_insert;

	cxlrd = cxl_root_decoder_alloc(root_port, ways);
	if (IS_ERR(cxlrd))
		return 0;

	cxld = &cxlrd->cxlsd.cxld;
	cxld->flags = cfmws_to_decoder_flags(cfmws->restrictions);
	cxld->target_type = CXL_DECODER_EXPANDER;
	cxld->hpa_range = (struct range) {
		.start = res->start,
		.end = res->end,
	};
	cxld->interleave_ways = ways;
	/*
	 * Minimize the x1 granularity to advertise support for any
	 * valid region granularity
	 */
	if (ways == 1)
		ig = CXL_DECODER_MIN_GRANULARITY;
	cxld->interleave_granularity = ig;

	rc = cxl_decoder_add(cxld, target_map);
	if (rc)
		put_device(&cxld->dev);
	else
		rc = cxl_decoder_autoremove(dev, cxld);
	if (rc) {
		dev_err(dev, "Failed to add decode range [%#llx - %#llx]\n",
			cxld->hpa_range.start, cxld->hpa_range.end);
		return 0;
	}
	dev_dbg(dev, "add: %s node: %d range [%#llx - %#llx]\n",
		dev_name(&cxld->dev),
		phys_to_target_node(cxld->hpa_range.start),
		cxld->hpa_range.start, cxld->hpa_range.end);

	return 0;

err_insert:
	kfree(res->name);
err_name:
	kfree(res);
	return -ENOMEM;
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

	port = devm_cxl_add_port(host, match, dport->component_reg_phys, dport);
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

static const struct acpi_device_id cxl_host_ids[] = {
	{ "ACPI0016", 0 },
	{ "PNP0A08", 0 },
	{ },
};

struct pci_host_bridge *cxl_find_next_rch(struct pci_host_bridge *host)
{
	struct pci_bus *bus = host ? host->bus : NULL;
	struct acpi_device *adev;
	struct pci_dev *pdev;
	bool is_restricted_host;

	while ((bus = pci_find_next_bus(bus)) != NULL) {
		host = bus ? to_pci_host_bridge(bus->bridge) : NULL;
		if (!host)
			continue;

		dev_dbg(&host->dev, "PCI bridge found\n");

		/* Must be a root bridge */
		if (host->bus->parent)
			continue;

		dev_dbg(&host->dev, "PCI bridge is root bridge\n");

		adev = ACPI_COMPANION(&host->dev);
		if (acpi_match_device_ids(adev, cxl_host_ids))
			continue;

		dev_dbg(&host->dev, "PCI ACPI host found: %s\n",
			acpi_dev_name(adev));

		/* Check CXL DVSEC of dev 0 func 0 */
		pdev = pci_get_slot(bus, PCI_DEVFN(0, 0));
		is_restricted_host = pdev
			&& (pci_pcie_type(pdev) == PCI_EXP_TYPE_RC_END)
			&& pci_find_dvsec_capability(pdev,
						PCI_DVSEC_VENDOR_ID_CXL,
						CXL_DVSEC_PCIE_DEVICE);
		pci_dev_put(pdev);

		if (!is_restricted_host)
			continue;

		dev_dbg(&host->dev, "CXL restricted host found\n");

		return host;
	}

	return NULL;
}

static int __cxl_get_rcrb(union acpi_subtable_headers *header, void *arg,
			  const unsigned long end)
{
	struct cxl_chbs_context *ctx = arg;
	struct acpi_cedt_chbs *chbs;

	if (ctx->chbcr)
		return 0;

	chbs = (struct acpi_cedt_chbs *)header;

	if (ctx->uid != chbs->uid)
		return 0;

	if (chbs->cxl_version != ACPI_CEDT_CHBS_VERSION_CXL11)
		return 0;

	if (chbs->length != SZ_8K)
		return 0;

	ctx->chbcr = chbs->base;

	return 0;
}

static resource_size_t cxl_get_rcrb(u32 uid)
{
	struct cxl_chbs_context ctx = {
		.uid = uid,
	};

	acpi_table_parse_cedt(ACPI_CEDT_TYPE_CHBS, __cxl_get_rcrb, &ctx);

	return ctx.chbcr;
}

static resource_size_t cxl_get_component_reg_phys(resource_size_t rcrb)
{
	resource_size_t component_reg_phys;
	u32 bar0, bar1;
	void *addr;

	/*
	 * RCRB's BAR[0..1] point to component block containing CXL subsystem
	 * component registers.
	 * CXL 8.2.4 - Component Register Layout Definition.
	 *
	 * Also, RCRB accesses must use MMIO readl()/readq() to guarantee
	 * 32/64-bit access.
	 * CXL 8.2.2 - CXL 1.1 Upstream and Downstream Port Subsystem Component
	 * Registers
	 */
	addr = ioremap(rcrb, PCI_BASE_ADDRESS_0 + SZ_8);
	bar0 = readl(addr + PCI_BASE_ADDRESS_0);
	bar1 = readl(addr + PCI_BASE_ADDRESS_1);
	iounmap(addr);

	/* sanity check */
	if (bar0 & (PCI_BASE_ADDRESS_MEM_TYPE_1M | PCI_BASE_ADDRESS_SPACE_IO))
		return CXL_RESOURCE_NONE;

	component_reg_phys = bar0 & PCI_BASE_ADDRESS_MEM_MASK;
	if (bar0 & PCI_BASE_ADDRESS_MEM_TYPE_64)
		component_reg_phys |= ((u64)bar1) << 32;

	if (!component_reg_phys)
		return CXL_RESOURCE_NONE;

	/*
	 * Must be 8k aligned (size of combined CXL 1.1 Downstream and
	 * Upstream Port RCRBs).
	 */
	if (component_reg_phys & (SZ_8K - 1))
		return CXL_RESOURCE_NONE;

	return component_reg_phys;
}

static int cxl_setup_component_reg(struct device *parent,
				   resource_size_t component_reg_phys)
{
	struct cxl_component_reg_map comp_map;
	void __iomem *base;

	if (component_reg_phys == CXL_RESOURCE_NONE)
		return -EINVAL;

	base = ioremap(component_reg_phys, SZ_64K);
	if (!base) {
		dev_err(parent, "failed to map registers\n");
		return -ENOMEM;
	}

	cxl_probe_component_regs(parent, base, &comp_map);
	iounmap(base);

	if (!comp_map.hdm_decoder.valid) {
		dev_err(parent, "HDM decoder registers not found\n");
		return -ENXIO;
	}

	dev_dbg(parent, "Set up component registers\n");

	return 0;
}

static int cxl_enumerate_rch_ports(struct device *root_dev,
				   struct cxl_port *cxl_root,
				   struct pci_host_bridge *host,
				   resource_size_t component_reg_phys,
				   int port_id)
{
	struct cxl_dport *dport;
	struct cxl_port *port;
	struct pci_dev *pdev;

	dport = devm_cxl_add_dport(cxl_root, &host->dev, port_id,
				   component_reg_phys);
	if (IS_ERR(dport))
		return PTR_ERR(dport);

	port = devm_cxl_add_port(root_dev, &host->dev,
				 component_reg_phys, dport);
	if (IS_ERR(port))
		return PTR_ERR(port);

	pdev = pci_get_slot(host->bus, PCI_DEVFN(0, 0));
	if (!pdev)
		return -ENXIO;

	/* Note: The endpoint provides the component reg base. */
	dport = devm_cxl_add_dport(port, &pdev->dev, 0,
				   CXL_RESOURCE_NONE);

	pci_dev_put(pdev);

	if (IS_ERR(dport))
		return PTR_ERR(dport);

	return 0;
}

static int __init cxl_restricted_host_probe(struct platform_device *pdev)
{
	struct device *root_dev = &pdev->dev;
	struct pci_host_bridge *host = NULL;
	struct acpi_device *adev;
	struct cxl_port *cxl_root = NULL;
	unsigned long long uid = ~0;
	resource_size_t rcrb;
	resource_size_t component_reg_phys;
	int port_id = 0;
	int rc;

	while ((host = cxl_find_next_rch(host)) != NULL) {
		adev = ACPI_COMPANION(&host->dev);
		if (!adev || !adev->pnp.unique_id ||
			(kstrtoull(adev->pnp.unique_id, 10, &uid) < 0))
			continue;

		dev_dbg(&adev->dev, "host uid: %llu\n", uid);

		if (uid > U32_MAX)
			continue;

		rcrb = cxl_get_rcrb(uid);

		/*
		 * workaround if no cedt is present:
		 *
		 * base: 0xb8200000	(from bios log)
		 * size: SZ_8K		(from spec)
		 * busnr: 0x7f		(from lspci/bios log)
		 */
		if (!rcrb && host->busnr == 0x7f) {
			rcrb = 0xb8200000;
			dev_info(&host->dev, "Applying CEDT workaround\n");
		}

		if (!rcrb)
			continue;

		dev_dbg(&host->dev, "RCRB found: 0x%08llx\n", (u64)rcrb);

		/*
		 * For CXL 1.1 hosts we create a root device other
		 * than the ACPI0017 device to hold the devm data and
		 * the uport ref.
		 */
		if (!cxl_root) {
			cxl_root = devm_cxl_add_port(root_dev, root_dev,
						     CXL_RESOURCE_NONE, NULL);
			if (IS_ERR(cxl_root)) {
				rc = PTR_ERR(cxl_root);
				goto fail;
			}
		}

		component_reg_phys = cxl_get_component_reg_phys(rcrb);
		rc = cxl_setup_component_reg(&host->dev, component_reg_phys);
		if (rc)
			goto fail;

		rc = cxl_enumerate_rch_ports(root_dev, cxl_root, host,
					     component_reg_phys, port_id++);
		if (rc)
			goto fail;

		dev_info(&host->dev, "host supports CXL\n");
	}

	return 0;
fail:
	dev_err(&host->dev, "failed to initialize CXL host: %d\n", rc);
	return rc;
}

static struct lock_class_key cxl_root_key;

static void cxl_acpi_lock_reset_class(void *dev)
{
	device_lock_reset_class(dev);
}

static void del_cxl_resource(struct resource *res)
{
	kfree(res->name);
	kfree(res);
}

static void cxl_set_public_resource(struct resource *priv, struct resource *pub)
{
	priv->desc = (unsigned long) pub;
}

static struct resource *cxl_get_public_resource(struct resource *priv)
{
	return (struct resource *) priv->desc;
}

static void remove_cxl_resources(void *data)
{
	struct resource *res, *next, *cxl = data;

	for (res = cxl->child; res; res = next) {
		struct resource *victim = cxl_get_public_resource(res);

		next = res->sibling;
		remove_resource(res);

		if (victim) {
			remove_resource(victim);
			kfree(victim);
		}

		del_cxl_resource(res);
	}
}

/**
 * add_cxl_resources() - reflect CXL fixed memory windows in iomem_resource
 * @cxl_res: A standalone resource tree where each CXL window is a sibling
 *
 * Walk each CXL window in @cxl_res and add it to iomem_resource potentially
 * expanding its boundaries to ensure that any conflicting resources become
 * children. If a window is expanded it may then conflict with a another window
 * entry and require the window to be truncated or trimmed. Consider this
 * situation:
 *
 * |-- "CXL Window 0" --||----- "CXL Window 1" -----|
 * |--------------- "System RAM" -------------|
 *
 * ...where platform firmware has established as System RAM resource across 2
 * windows, but has left some portion of window 1 for dynamic CXL region
 * provisioning. In this case "Window 0" will span the entirety of the "System
 * RAM" span, and "CXL Window 1" is truncated to the remaining tail past the end
 * of that "System RAM" resource.
 */
static int add_cxl_resources(struct resource *cxl_res)
{
	struct resource *res, *new, *next;

	for (res = cxl_res->child; res; res = next) {
		new = kzalloc(sizeof(*new), GFP_KERNEL);
		if (!new)
			return -ENOMEM;
		new->name = res->name;
		new->start = res->start;
		new->end = res->end;
		new->flags = IORESOURCE_MEM;
		new->desc = IORES_DESC_CXL;

		/*
		 * Record the public resource in the private cxl_res tree for
		 * later removal.
		 */
		cxl_set_public_resource(res, new);

		insert_resource_expand_to_fit(&iomem_resource, new);

		next = res->sibling;
		while (next && resource_overlaps(new, next)) {
			if (resource_contains(new, next)) {
				struct resource *_next = next->sibling;

				remove_resource(next);
				del_cxl_resource(next);
				next = _next;
			} else
				next->start = new->end + 1;
		}
	}
	return 0;
}

static int pair_cxl_resource(struct device *dev, void *data)
{
	struct resource *cxl_res = data;
	struct resource *p;

	if (!is_root_decoder(dev))
		return 0;

	for (p = cxl_res->child; p; p = p->sibling) {
		struct cxl_root_decoder *cxlrd = to_cxl_root_decoder(dev);
		struct cxl_decoder *cxld = &cxlrd->cxlsd.cxld;
		struct resource res = {
			.start = cxld->hpa_range.start,
			.end = cxld->hpa_range.end,
			.flags = IORESOURCE_MEM,
		};

		if (resource_contains(p, &res)) {
			cxlrd->res = cxl_get_public_resource(p);
			break;
		}
	}

	return 0;
}

static int cxl_acpi_probe(struct platform_device *pdev)
{
	int rc;
	struct resource *cxl_res;
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

	device_lock_set_class(&pdev->dev, &cxl_root_key);
	rc = devm_add_action_or_reset(&pdev->dev, cxl_acpi_lock_reset_class,
				      &pdev->dev);
	if (rc)
		return rc;

	cxl_res = devm_kzalloc(host, sizeof(*cxl_res), GFP_KERNEL);
	if (!cxl_res)
		return -ENOMEM;
	cxl_res->name = "CXL mem";
	cxl_res->start = 0;
	cxl_res->end = -1;
	cxl_res->flags = IORESOURCE_MEM;

	root_port = devm_cxl_add_port(host, host, CXL_RESOURCE_NONE, NULL);
	if (IS_ERR(root_port))
		return PTR_ERR(root_port);

	rc = bus_for_each_dev(adev->dev.bus, NULL, root_port,
			      add_host_bridge_dport);
	if (rc < 0)
		return rc;

	rc = devm_add_action_or_reset(host, remove_cxl_resources, cxl_res);
	if (rc)
		return rc;

	ctx = (struct cxl_cfmws_context) {
		.dev = host,
		.root_port = root_port,
		.cxl_res = cxl_res,
	};
	rc = acpi_table_parse_cedt(ACPI_CEDT_TYPE_CFMWS, cxl_parse_cfmws, &ctx);
	if (rc < 0)
		return -ENXIO;

	rc = add_cxl_resources(cxl_res);
	if (rc)
		return rc;

	/*
	 * Populate the root decoders with their related iomem resource,
	 * if present
	 */
	device_for_each_child(&root_port->dev, cxl_res, pair_cxl_resource);

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

static const struct platform_device_id cxl_test_ids[] = {
	{ "cxl_acpi" },
	{ "cxl_root" },
	{ },
};
MODULE_DEVICE_TABLE(platform, cxl_test_ids);

static struct platform_driver cxl_acpi_driver = {
	.probe = cxl_acpi_probe,
	.driver = {
		.name = KBUILD_MODNAME,
		.acpi_match_table = cxl_acpi_ids,
	},
	.id_table = cxl_test_ids,
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
MODULE_SOFTDEP("pre: cxl_port");
