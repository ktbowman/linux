// SPDX-License-Identifier: GPL-2.0
/*
 * Root Complex Event Collector Support
 *
 * Authors:
 *  Sean V Kelley <sean.v.kelley@intel.com>
 *  Qiuxu Zhuo <qiuxu.zhuo@intel.com>
 *
 * Copyright (C) 2020 Intel Corp.
 */

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>

#include "../pci.h"

struct walk_rcec_data {
	struct pci_dev *rcec;
	int (*user_callback)(struct pci_dev *dev, void *data);
	void *user_data;
};

static bool rcec_assoc_rciep(struct pci_dev *rcec, struct pci_dev *rciep)
{
	unsigned long bitmap = rcec->rcec_ea->bitmap;
	unsigned int devn;

	/* An RCiEP found on a different bus in range */
	if (rcec->bus->number != rciep->bus->number)
		return true;

	/* Same bus, so check bitmap */
	for_each_set_bit(devn, &bitmap, 32)
		if (devn == PCI_SLOT(rciep->devfn))
			return true;

	return false;
}

static int link_rcec_helper(struct pci_dev *dev, void *data)
{
	struct walk_rcec_data *rcec_data = data;
	struct pci_dev *rcec = rcec_data->rcec;

	if ((pci_pcie_type(dev) == PCI_EXP_TYPE_RC_END) &&
	    rcec_assoc_rciep(rcec, dev)) {
		dev->rcec = rcec;
		pci_dbg(dev, "PME & error events signaled via %s\n",
			pci_name(rcec));
	}

	return 0;
}

static int walk_rcec_helper(struct pci_dev *dev, void *data)
{
	struct walk_rcec_data *rcec_data = data;
	struct pci_dev *rcec = rcec_data->rcec;

	if ((pci_pcie_type(dev) == PCI_EXP_TYPE_RC_END) &&
	    rcec_assoc_rciep(rcec, dev))
		rcec_data->user_callback(dev, rcec_data->user_data);

	return 0;
}

static void walk_rcec(int (*cb)(struct pci_dev *dev, void *data),
		      void *userdata)
{
	struct walk_rcec_data *rcec_data = userdata;
	struct pci_dev *rcec = rcec_data->rcec;
	u8 nextbusn, lastbusn;
	struct pci_bus *bus;
	unsigned int bnr;

	if (!rcec->rcec_ea)
		return;

	/* Walk own bus for bitmap based association */
	pci_walk_bus(rcec->bus, cb, rcec_data);

	nextbusn = rcec->rcec_ea->nextbusn;
	lastbusn = rcec->rcec_ea->lastbusn;

	/* All RCiEP devices are on the same bus as the RCEC */
	if (nextbusn == 0xff && lastbusn == 0x00)
		return;

	for (bnr = nextbusn; bnr <= lastbusn; bnr++) {
		/* No association indicated (PCIe 5.0-1, 7.9.10.3) */
		if (bnr == rcec->bus->number)
			continue;

		bus = pci_find_bus(pci_domain_nr(rcec->bus), bnr);
		if (!bus)
			continue;

		/* Find RCiEP devices on the given bus ranges */
		pci_walk_bus(bus, cb, rcec_data);
	}
}

/**
 * pcie_link_rcec - Link RCiEP devices associated with RCEC.
 * @rcec: RCEC whose RCiEP devices should be linked.
 *
 * Link the given RCEC to each RCiEP device found.
 */
void pcie_link_rcec(struct pci_dev *rcec)
{
	struct walk_rcec_data rcec_data;

	if (!rcec->rcec_ea)
		return;

	rcec_data.rcec = rcec;
	rcec_data.user_callback = NULL;
	rcec_data.user_data = NULL;

	walk_rcec(link_rcec_helper, &rcec_data);
}

/**
 * pcie_walk_rcec - Walk RCiEP devices associating with RCEC and call callback.
 * @rcec:	RCEC whose RCiEP devices should be walked
 * @cb:		Callback to be called for each RCiEP device found
 * @userdata:	Arbitrary pointer to be passed to callback
 *
 * Walk the given RCEC. Call the callback on each RCiEP found.
 *
 * If @cb returns anything other than 0, break out.
 */
void pcie_walk_rcec(struct pci_dev *rcec, int (*cb)(struct pci_dev *, void *),
		    void *userdata)
{
	struct walk_rcec_data rcec_data;

	if (!rcec->rcec_ea)
		return;

	rcec_data.rcec = rcec;
	rcec_data.user_callback = cb;
	rcec_data.user_data = userdata;

	walk_rcec(walk_rcec_helper, &rcec_data);
}

struct find_rcec_data {
	struct pci_dev *rciep;	/* RCiEP we are resolving */
	struct pci_dev *rcec;	/* resolved RCEC, or NULL */
};

/*
 * rcec_covers_rciep_bus - Does @rcec's Endpoint Association range cover
 * @rciep's bus?
 *
 * rcec_assoc_rciep()'s "different bus => associated" shortcut is only valid
 * for callers (walk_rcec()) that have already restricted the candidate set to
 * the RCEC's [nextbusn, lastbusn] range. find_rcec_for_rciep() walks the whole
 * host bridge, so it must apply the range gate itself before trusting that
 * shortcut; otherwise, with multiple RCECs under one host bridge, an RCiEP
 * could be matched to an RCEC whose EA range does not cover the RCiEP's bus.
 */
static bool rcec_covers_rciep_bus(struct pci_dev *rcec, struct pci_dev *rciep)
{
	u8 nextbusn = rcec->rcec_ea->nextbusn;
	u8 lastbusn = rcec->rcec_ea->lastbusn;
	u8 busn = rciep->bus->number;

	/* Same bus is resolved by the EA bitmap, not the bus range. */
	if (rcec->bus->number == busn)
		return true;

	/* All RCiEPs on the RCEC's own bus (PCIe 5.0-1, 7.9.10.3). */
	if (nextbusn == 0xff && lastbusn == 0x00)
		return false;

	return busn >= nextbusn && busn <= lastbusn;
}

/*
 * Callback for pci_walk_bus(): for each candidate RC_EC, apply the same
 * RCEC Endpoint Association rule used by the core walk (rcec_assoc_rciep())
 * and record the first RCEC that covers @rciep.
 *
 * This runs with pci_bus_sem read-held by pci_walk_bus(). It must not call
 * into pci_walk_bus() or pci_find_bus() again (both re-acquire pci_bus_sem),
 * so the match is performed inline against the cached rcec_ea rather than by
 * nesting pcie_walk_rcec(). A reference is taken on the matched RCEC while
 * the lock is still held so the returned pointer remains valid after
 * pci_walk_bus() drops pci_bus_sem.
 */
static int find_rcec_for_rciep(struct pci_dev *dev, void *data)
{
	struct find_rcec_data *fd = data;

	if (pci_pcie_type(dev) != PCI_EXP_TYPE_RC_EC)
		return 0;

	if (!dev->rcec_ea)
		return 0;

	if (!rcec_covers_rciep_bus(dev, fd->rciep))
		return 0;

	if (rcec_assoc_rciep(dev, fd->rciep)) {
		fd->rcec = pci_dev_get(dev);
		return 1;	/* stop the host-bridge walk */
	}
	return 0;
}

/**
 * pci_rciep_to_rcec - Find the RCEC associated with an RCiEP.
 * @rciep: Root Complex Integrated Endpoint (PCI_EXP_TYPE_RC_END).
 *
 * Return the RCEC whose RCEC Endpoint Association (cached at enumeration in
 * rcec_ea) covers @rciep, or NULL if none is found. This is read-only and
 * does not depend on pcie_link_rcec() having populated pdev->rcec, so it is
 * valid regardless of enumeration or driver-bind ordering.
 *
 * The returned pci_dev has its reference count incremented; the caller must
 * drop it with pci_dev_put().
 */
struct pci_dev *pci_rciep_to_rcec(struct pci_dev *rciep)
{
	struct find_rcec_data fd = { .rciep = rciep };
	struct pci_host_bridge *hb;

	if (pci_pcie_type(rciep) != PCI_EXP_TYPE_RC_END)
		return NULL;

	hb = pci_find_host_bridge(rciep->bus);
	if (!hb)
		return NULL;

	pci_walk_bus(hb->bus, find_rcec_for_rciep, &fd);

	return fd.rcec;
}
EXPORT_SYMBOL_FOR_MODULES(pci_rciep_to_rcec, "cxl_core");

void pci_rcec_init(struct pci_dev *dev)
{
	struct rcec_ea *rcec_ea;
	u32 rcec, hdr, busn;
	u8 ver;

	/* Only for Root Complex Event Collectors */
	if (pci_pcie_type(dev) != PCI_EXP_TYPE_RC_EC)
		return;

	rcec = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_RCEC);
	if (!rcec)
		return;

	rcec_ea = kzalloc_obj(*rcec_ea);
	if (!rcec_ea)
		return;

	pci_read_config_dword(dev, rcec + PCI_RCEC_RCIEP_BITMAP,
			      &rcec_ea->bitmap);

	/* Check whether RCEC BUSN register is present */
	pci_read_config_dword(dev, rcec, &hdr);
	ver = PCI_EXT_CAP_VER(hdr);
	if (ver >= PCI_RCEC_BUSN_REG_VER) {
		pci_read_config_dword(dev, rcec + PCI_RCEC_BUSN, &busn);
		rcec_ea->nextbusn = PCI_RCEC_BUSN_NEXT(busn);
		rcec_ea->lastbusn = PCI_RCEC_BUSN_LAST(busn);
	} else {
		/* Avoid later ver check by setting nextbusn */
		rcec_ea->nextbusn = 0xff;
		rcec_ea->lastbusn = 0x00;
	}

	dev->rcec_ea = rcec_ea;
}

void pci_rcec_exit(struct pci_dev *dev)
{
	kfree(dev->rcec_ea);
	dev->rcec_ea = NULL;
}
