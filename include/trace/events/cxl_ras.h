/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM cxl_ras

#if !defined(_CXL_RAS_EVENTS_H) || defined(TRACE_HEADER_MULTI_READ)
#define _CXL_RAS_EVENTS_H

#include <linux/tracepoint.h>

#define CXL_RAS_UC_CACHE_DATA_PARITY	BIT(0)
#define CXL_RAS_UC_CACHE_ADDR_PARITY	BIT(1)
#define CXL_RAS_UC_CACHE_BE_PARITY	BIT(2)
#define CXL_RAS_UC_CACHE_DATA_ECC	BIT(3)
#define CXL_RAS_UC_MEM_DATA_PARITY	BIT(4)
#define CXL_RAS_UC_MEM_ADDR_PARITY	BIT(5)
#define CXL_RAS_UC_MEM_BE_PARITY	BIT(6)
#define CXL_RAS_UC_MEM_DATA_ECC		BIT(7)
#define CXL_RAS_UC_REINIT_THRESH	BIT(8)
#define CXL_RAS_UC_RSVD_ENCODE		BIT(9)
#define CXL_RAS_UC_POISON		BIT(10)
#define CXL_RAS_UC_RECV_OVERFLOW	BIT(11)
#define CXL_RAS_UC_INTERNAL_ERR		BIT(14)
#define CXL_RAS_UC_IDE_TX_ERR		BIT(15)
#define CXL_RAS_UC_IDE_RX_ERR		BIT(16)

#define show_uc_errs(status)	__print_flags(status, " | ",		  \
	{ CXL_RAS_UC_CACHE_DATA_PARITY, "Cache Data Parity Error" },	  \
	{ CXL_RAS_UC_CACHE_ADDR_PARITY, "Cache Address Parity Error" },	  \
	{ CXL_RAS_UC_CACHE_BE_PARITY, "Cache Byte Enable Parity Error" }, \
	{ CXL_RAS_UC_CACHE_DATA_ECC, "Cache Data ECC Error" },		  \
	{ CXL_RAS_UC_MEM_DATA_PARITY, "Memory Data Parity Error" },	  \
	{ CXL_RAS_UC_MEM_ADDR_PARITY, "Memory Address Parity Error" },	  \
	{ CXL_RAS_UC_MEM_BE_PARITY, "Memory Byte Enable Parity Error" },  \
	{ CXL_RAS_UC_MEM_DATA_ECC, "Memory Data ECC Error" },		  \
	{ CXL_RAS_UC_REINIT_THRESH, "REINIT Threshold Hit" },		  \
	{ CXL_RAS_UC_RSVD_ENCODE, "Received Unrecognized Encoding" },	  \
	{ CXL_RAS_UC_POISON, "Received Poison From Peer" },		  \
	{ CXL_RAS_UC_RECV_OVERFLOW, "Receiver Overflow" },		  \
	{ CXL_RAS_UC_INTERNAL_ERR, "Component Specific Error" },	  \
	{ CXL_RAS_UC_IDE_TX_ERR, "IDE Tx Error" },			  \
	{ CXL_RAS_UC_IDE_RX_ERR, "IDE Rx Error" }			  \
)

#define CXL_RAS_UC_OVFL_D2H_REQ		BIT(0)
#define CXL_RAS_UC_OVFL_D2H_RSP		BIT(1)
#define CXL_RAS_UC_OVFL_D2H_DATA	BIT(2)
#define CXL_RAS_UC_OVFL_S2M_NDR		BIT(3)
#define CXL_RAS_UC_OVFL_S2M_DRS		BIT(4)

#define show_uc_ovfl(hl)	__print_flags(hl, " | ",		\
	{ CXL_RAS_UC_OVFL_D2H_REQ, "Receiver Overflow D2H Req" },	\
	{ CXL_RAS_UC_OVFL_D2H_RSP, "Receiver Overflow D2H Rsp" },	\
	{ CXL_RAS_UC_OVFL_D2H_DATA, "Receiver Overflow D2H Data" },	\
	{ CXL_RAS_UC_OVFL_S2M_NDR, "Receiver Overflow S2M NDR" },	\
	{ CXL_RAS_UC_OVFL_S2M_DRS, "Receiver Overflow S2M DRS" }	\
)

TRACE_EVENT(cxl_ras_uc,
	TP_PROTO(const char *dev_name, u32 status, u32 fe, u8 *hl, int hl_len),
	TP_ARGS(dev_name, status, fe, hl, hl_len),
	TP_STRUCT__entry(
		__string(dev_name, dev_name)
		__field(u32, status)
		__field(u32, first_error)
		__dynamic_array(u8, header_log, hl_len)
		__field(int, header_log_len)
	),
	TP_fast_assign(
		__assign_str(dev_name, dev_name);
		__entry->status = status;
		__entry->first_error = fe;
		memcpy(__get_dynamic_array(header_log), hl, hl_len);
		__entry->header_log_len = hl_len;
	),
	TP_printk("%s: status: '%s' first_error: '%s' header log: %s",
		  __get_str(dev_name), show_uc_errs(__entry->status),
		  show_uc_errs(__entry->first_error),
		  __print_array(__get_dynamic_array(header_log), __entry->header_log_len, 1)
	)
);

#define CXL_RAS_CE_CACHE_DATA_ECC	BIT(0)
#define CXL_RAS_CE_MEM_DATA_ECC		BIT(1)
#define CXL_RAS_CE_CRC_THRESH		BIT(2)
#define CXL_RAS_CE_CACHE_POISON		BIT(3)
#define CXL_RAS_CE_MEM_POISON		BIT(4)
#define CXL_RAS_CE_PHYS_LAYER_ERR	BIT(5)

#define show_ce_errs(status)	__print_flags(status, " | ",			\
	{ CXL_RAS_CE_CACHE_DATA_ECC, "Cache Data ECC Error" },			\
	{ CXL_RAS_CE_MEM_DATA_ECC, "Memory Data Ecc Error" },			\
	{ CXL_RAS_CE_CRC_THRESH, "CRC Threshold Hit" },				\
	{ CXL_RAS_CE_CACHE_POISON, "Received Cache Poison From Peer" },		\
	{ CXL_RAS_CE_MEM_POISON, "Received Memory Poison From Peer" },		\
	{ CXL_RAS_CE_PHYS_LAYER_ERR, "Received Error From Physical Layer" }	\
)

TRACE_EVENT(cxl_ras_ce,
	TP_PROTO(const char *dev_name, u32 status),
	TP_ARGS(dev_name, status),
	TP_STRUCT__entry(
		__string(dev_name, dev_name)
		__field(u32, status)
	),
	TP_fast_assign(
		__assign_str(dev_name, dev_name);
		__entry->status = status;
	),
	TP_printk("%s: status: '%s'",
		  __get_str(dev_name), show_ce_errs(__entry->status)
	)
);

#endif /* _CXL_RAS_EVENTS_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
