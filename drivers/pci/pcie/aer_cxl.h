/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Interface between the PCIe AER core (aer.c) and the CXL protocol error
 * handlers (aer_cxl_vh.c).
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 */

#ifndef _AER_CXL_H_
#define _AER_CXL_H_

#include <linux/pci.h>

struct aer_err_info;

#ifdef CONFIG_CXL_RAS
bool is_aer_cxl_only_error(struct aer_err_info *info);
bool is_cxl_error(struct pci_dev *pdev, struct aer_err_info *info);
void cxl_forward_error(struct pci_dev *pdev, struct aer_err_info *info);
#else
static inline bool is_aer_cxl_only_error(struct aer_err_info *info) { return false; }
static inline bool is_cxl_error(struct pci_dev *pdev, struct aer_err_info *info) { return false; }
static inline void cxl_forward_error(struct pci_dev *pdev, struct aer_err_info *info) { }
#endif /* CONFIG_CXL_RAS */
#endif /* _AER_CXL_H_ */
