// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 AMD Corporation. All rights reserved. */

#include <linux/aer.h>
#include <linux/atomic.h>
#include <linux/cleanup.h>
#include <linux/init.h>
#include <linux/kfifo.h>
#include <linux/lockdep.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/wait_bit.h>
#include <linux/workqueue.h>
#include "../pci.h"
#include "portdrv.h"
#include "aer_cxl.h"

#define CXL_ERROR_SOURCES_MAX          128

static struct cxl_proto_err_kfifo {
	struct work_struct *work;
	void (*flush)(void);
	struct rw_semaphore rwsem;
	spinlock_t fifo_lock;           /* Serializes kfifo writers */
	atomic_t flush_inflight;
	DECLARE_KFIFO(fifo, struct cxl_proto_err_work_data,
		      CXL_ERROR_SOURCES_MAX);
} cxl_proto_err_kfifo = {
	.rwsem = __RWSEM_INITIALIZER(cxl_proto_err_kfifo.rwsem),
	.fifo_lock = __SPIN_LOCK_UNLOCKED(cxl_proto_err_kfifo.fifo_lock),
	.flush_inflight = ATOMIC_INIT(0),
};

static int __init cxl_proto_err_kfifo_init(void)
{
	INIT_KFIFO(cxl_proto_err_kfifo.fifo);
	return 0;
}
subsys_initcall(cxl_proto_err_kfifo_init);

static bool is_aer_internal_error(struct aer_err_info *info)
{
	u32 status = info->status & ~info->mask;

	if (info->severity == AER_CORRECTABLE)
		return status & PCI_ERR_COR_INTERNAL;

	return status & PCI_ERR_UNC_INTN;
}

/**
 * is_aer_cxl_only_error - Check if Internal Error is the only unmasked error
 * @info: AER error info
 *
 * Only the status register matching @info->severity is checked (Correctable
 * or Uncorrectable), and masked bits are ignored.
 *
 * Return: true if no unmasked status bit other than Internal Error is set,
 * false otherwise.
 */
bool is_aer_cxl_only_error(struct aer_err_info *info)
{
	u32 status = info->status & ~info->mask;

	if (info->severity == AER_CORRECTABLE)
		return !(status & ~PCI_ERR_COR_INTERNAL);

	return !(status & ~PCI_ERR_UNC_INTN);
}

bool is_cxl_error(struct pci_dev *pdev, struct aer_err_info *info)
{
	if (!info)
		return false;

	/* RCEC lacks Flex Bus DVSEC; unmasked internal error marks CXL RCH */
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_RC_EC)
		return is_aer_internal_error(info);

	if (!info->is_cxl)
		return false;

	switch (pci_pcie_type(pdev)) {
	case PCI_EXP_TYPE_ENDPOINT:
	case PCI_EXP_TYPE_ROOT_PORT:
	case PCI_EXP_TYPE_UPSTREAM:
	case PCI_EXP_TYPE_DOWNSTREAM:
		break;
	default:
		return false;
	}

	return is_aer_internal_error(info);
}

/**
 * cxl_forward_error - Forward a CXL protocol error to the CXL subsystem via kfifo
 * @pdev: PCI device that reported the AER error
 * @info: AER error info containing severity and status
 *
 * Producer side of the AER-CXL kfifo. Enqueues a CXL protocol error work
 * item and schedules the consumer workqueue. An uncorrectable error also
 * waits for the kfifo to drain so that CXL RAS handling completes before
 * AER recovery tears the device down; a correctable error is handled
 * asynchronously. Takes a reference on @pdev that the consumer releases
 * after handling.
 *
 * If no consumer is registered the error is logged and dropped. If the
 * kfifo is full, a correctable error is logged and dropped, while an
 * uncorrectable error panics because it cannot be safely discarded.
 */
void cxl_forward_error(struct pci_dev *pdev, struct aer_err_info *info)
{
	bool uncorrectable = info->severity != AER_CORRECTABLE;
	struct work_struct *work;
	void (*flush)(void) = NULL;

	struct cxl_proto_err_work_data wd = {
		.severity = info->severity,
		.pdev = pdev,
	};

	scoped_guard(rwsem_read, &cxl_proto_err_kfifo.rwsem) {
		work = cxl_proto_err_kfifo.work;
		if (!work) {
			dev_err_ratelimited(&pdev->dev, "AER-CXL kfifo reader not registered\n");
			return;
		}

		/*
		 * Reference discipline: the AER caller (handle_error_source()) holds
		 * a ref on @pdev for the duration of this call and releases it on
		 * return. Take a fresh ref here so the pdev stays live while queued
		 * in the kfifo; the corresponding consumer is for_each_cxl_proto_err()
		 * and will drop that ref after handling. On enqueue failure below,
		 * drop the ref we just took to avoid a leak.
		 */
		pci_dev_get(pdev);

		/* Serialize concurrent kfifo writers: multiple AER threaded IRQs */
		if (!kfifo_in_spinlocked(&cxl_proto_err_kfifo.fifo, &wd, 1,
					 &cxl_proto_err_kfifo.fifo_lock)) {
			pci_dev_put(pdev);

			/*
			 * Unlike PCIe AER, a dropped CXL.mem uncorrectable error
			 * cannot be treated as device-local: it may signal lost cache
			 * coherency over HDM memory in active use. The error can no
			 * longer be confirmed via CXL RAS, and reaching here means the
			 * fifo is saturated with pending protocol errors. Collapse the
			 * unknown state to the same conservative outcome as a confirmed
			 * UCE.
			 */
			if (uncorrectable)
				panic("CXL: dropped uncorrectable protocol error\n");

			/*
			 * A correctable error is device-local and recoverable, so a
			 * dropped CE is safe to log and discard. Never panic on CE
			 * pressure: a CE storm must not fill the fifo and escalate a
			 * later event.
			 */
			dev_err_ratelimited(&pdev->dev,
					    "AER-CXL kfifo full, CE dropped\n");
			return;
		}

		/*
		 * Only an uncorrectable error has to be finished before AER
		 * recovery runs. A correctable error is logged and cleared by
		 * the consumer on its own schedule; draining here would
		 * serialize every correctable error behind the worker.
		 */
		if (uncorrectable) {
			flush = cxl_proto_err_kfifo.flush;
			if (flush)
				atomic_inc(&cxl_proto_err_kfifo.flush_inflight);
		}

		schedule_work(work);
	}

	if (flush) {
		flush();
		if (atomic_dec_and_test(&cxl_proto_err_kfifo.flush_inflight))
			wake_up_var(&cxl_proto_err_kfifo.flush_inflight);
	}
}

void cxl_register_proto_err_work(struct work_struct *work,
				 void (*flush)(void))
{
	guard(rwsem_write)(&cxl_proto_err_kfifo.rwsem);

	/*
	 * Warn on double-registration to surface driver bugs (e.g. missing
	 * cxl_unregister_proto_err_work() on module exit)
	 */
	if (WARN(cxl_proto_err_kfifo.work,
		 "AER-CXL kfifo consumer already registered\n"))
		return;
	cxl_proto_err_kfifo.work = work;
	cxl_proto_err_kfifo.flush = flush;
}
EXPORT_SYMBOL_FOR_MODULES(cxl_register_proto_err_work, "cxl_core");

static struct work_struct *cancel_cxl_proto_err(void)
{
	struct work_struct *work;
	struct cxl_proto_err_work_data wd;

	guard(rwsem_write)(&cxl_proto_err_kfifo.rwsem);
	work = cxl_proto_err_kfifo.work;
	cxl_proto_err_kfifo.work = NULL;
	cxl_proto_err_kfifo.flush = NULL;

	/* rwsem_write excludes all producers; fifo_lock not needed */
	while (kfifo_get(&cxl_proto_err_kfifo.fifo, &wd)) {
		dev_err_ratelimited(&wd.pdev->dev,
				    "AER-CXL error report canceled\n");
		pci_dev_put(wd.pdev);
	}
	return work;
}

void cxl_unregister_proto_err_work(void)
{
	struct work_struct *work;

	lockdep_assert_not_held(&cxl_proto_err_kfifo.rwsem);

	work = cancel_cxl_proto_err();

	/*
	 * Producers snapshot flush under the read lock and drain outside it,
	 * so wait for any such drain to finish before cancelling the work.
	 */
	wait_var_event(&cxl_proto_err_kfifo.flush_inflight,
		       atomic_read(&cxl_proto_err_kfifo.flush_inflight) == 0);

	if (work)
		cancel_work_sync(work);
}
EXPORT_SYMBOL_FOR_MODULES(cxl_unregister_proto_err_work, "cxl_core");

/**
 * for_each_cxl_proto_err - Call a function for each kfifo work item
 * @wd: caller-provided work-data scratch buffer, populated per kfifo entry
 * @fn: callback invoked for each dequeued &struct cxl_proto_err_work_data
 *
 * Single-consumer invariant: this function is only called from
 * cxl_proto_err_work_fn() via a single DECLARE_WORK.
 *
 * Holds rwsem_read internally; fn() must not call
 * cxl_register_proto_err_work() or cxl_unregister_proto_err_work() (both take
 * the same rwsem for write), nor the producer flush/drain path
 * (cxl_forward_error()'s flush callback), which would re-enter this drain and
 * can block a pending rwsem writer.
 */
void for_each_cxl_proto_err(struct cxl_proto_err_work_data *wd,
			    cxl_proto_err_fn_t fn)
{
	guard(rwsem_read)(&cxl_proto_err_kfifo.rwsem);
	while (kfifo_get(&cxl_proto_err_kfifo.fifo, wd)) {
		fn(wd);

		/* Corresponding ref incr taken in cxl_forward_error() */
		pci_dev_put(wd->pdev);
	}
}
EXPORT_SYMBOL_FOR_MODULES(for_each_cxl_proto_err, "cxl_core");
