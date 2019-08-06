/* Copyright (c) 2019, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#ifndef __FSM_DP_IOCTL_H__
#define __FSM_DP_IOCTL_H__

#include <linux/types.h>
#ifdef __KERNEL__
#include <linux/uio.h>
#else
#include <sys/uio.h>
#endif

#define FSM_DP_MAX_IOV_SIZE	64
#define FSM_DP_MAX_SG_IOV_SIZE	8

#define FSM_DP_IOCTL_BASE			'f'

#define FSM_DP_IOCTL_MEMPOOL_ALLOC	\
		_IOWR(FSM_DP_IOCTL_BASE, 1, struct fsm_dp_ioctl_mempool_alloc)

#define FSM_DP_IOCTL_MEMPOOL_GET_CONFIG	\
		_IOWR(FSM_DP_IOCTL_BASE, 2, struct fsm_dp_ioctl_getcfg)

#define FSM_DP_IOCTL_RX_GET_CONFIG	\
		_IOWR(FSM_DP_IOCTL_BASE, 3, struct fsm_dp_ioctl_getcfg)

#define FSM_DP_IOCTL_TX			\
		_IOWR(FSM_DP_IOCTL_BASE, 4, struct iovec)

#define FSM_DP_IOCTL_SG_TX		\
		_IOWR(FSM_DP_IOCTL_BASE, 5, struct iovec)

#define FSM_DP_IOCTL_TX_MODE_CONFIG	\
		_IOWR(FSM_DP_IOCTL_BASE, 6, unsigned int)

/* ioctl command for testing */
#define FSM_DP_IOCTL_TEST_RING_WRITE	_IO(FSM_DP_IOCTL_BASE, 0x11)
#define FSM_DP_IOCTL_TEST_RING_GET_CONFIG	\
		_IOWR(FSM_DP_IOCTL_BASE, 0x12, struct fsm_dp_ioctl_getcfg)

/* special value to write for testing */
#define TEST_RING_WRITE_MAGIC_VALUE	0xFFFFFFFE

/* message header version */
#define FSM_DP_MSG_HDR_VERSION		0x1

enum fsm_dp_mem_type {
	FSM_DP_MEM_TYPE_DL_L1_DATA,
	FSM_DP_MEM_TYPE_DL_L1_CTL,
	FSM_DP_MEM_TYPE_DL_RF,
	FSM_DP_MEM_TYPE_UL,
	FSM_DP_MEM_TYPE_LAST,
};

enum fsm_dp_mmap_type {
	FSM_DP_MMAP_TYPE_MEM,
	FSM_DP_MMAP_TYPE_RING,
	FSM_DP_MMAP_TYPE_LAST,
};

enum fsm_dp_rx_type {
	FSM_DP_RX_TYPE_L1,
	FSM_DP_RX_TYPE_RF,
	FSM_DP_RX_TYPE_TA,
	FSM_DP_RX_TYPE_LPBK,
	FSM_DP_RX_TYPE_LAST,
};

enum fsm_dp_msg_type {
	FSM_DP_MSG_TYPE_L1		= 0,
	FSM_DP_MSG_TYPE_RF		= 1,
	FSM_DP_MSG_TYPE_TA		= 2,
	FSM_DP_MSG_TYPE_LPBK_REQ	= 0xFE,
	FSM_DP_MSG_TYPE_LPBK_RSP	= 0xFF,
};

struct fsm_dp_msghdr {
	uint32_t version : 8;
	uint32_t type : 8;
	uint32_t unused : 8;
	uint32_t reserved : 8;
	uint32_t length : 16;
	uint16_t sequence : 16;
} __attribute__((packed));

typedef unsigned long fsm_dp_ring_element_data_t;
typedef unsigned int fsm_dp_ring_index_t;

struct fsm_dp_ring_element {
	unsigned long element_ctrl; /* 1 entry not valid, 0 valid */
				    /* Other bits for control flags: tbd */
	fsm_dp_ring_element_data_t element_data;
};

typedef struct fsm_dp_ring_element fsm_dp_ring_element_t;

struct fsm_dp_mmap_cfg {
	__u32 length;	/* length parameter for mmap */
	__u32 offset;	/* page offset of memory starting address */
	__u32 cookie;	/* last parameter for mmap */
};

struct fsm_dp_ring_cfg {
	struct fsm_dp_mmap_cfg mmap;	/* mmap parameters */
	__u32 size;			/* ring size */
	__u32 prod_head_off;		/* page offset of prod_head */
	__u32 prod_tail_off;		/* page offset of prod_tail */
	__u32 cons_head_off;		/* page offset of cons_head */
	__u32 cons_tail_off;		/* page offset of cons_tail */
	__u32 ringbuf_off;		/* page offset of ring buffer */
};

struct fsm_dp_mem_cfg {
	struct fsm_dp_mmap_cfg mmap;	/* mmap parameters */
	__u32 buf_sz;			/* size of buffer */
	__u32 buf_cnt;			/* number of buffer */
};

struct fsm_dp_mempool_cfg {
	enum fsm_dp_mem_type type;
	struct fsm_dp_mem_cfg mem;
	struct fsm_dp_ring_cfg ring;
};

struct fsm_dp_ioctl_mempool_alloc {
	__u32 type;		/* type defined in enum fsm_dp_mem_type */
	__u32 buf_sz;		/* size of buffer */
	__u32 buf_num;		/* number of buffer */
	struct fsm_dp_mempool_cfg *cfg;	/* for kernel to return config info */
};

struct fsm_dp_ioctl_getcfg {
	__u32 type;
	void *cfg;
};

static inline int fsm_dp_mem_type_is_valid(enum fsm_dp_mem_type type)
{
	return (type >= 0 && type < FSM_DP_MEM_TYPE_LAST);
}

static inline const char *fsm_dp_mem_type_to_str(enum fsm_dp_mem_type type)
{
	switch (type) {
	case FSM_DP_MEM_TYPE_DL_L1_DATA: return "DL_L1_DATA";
	case FSM_DP_MEM_TYPE_DL_L1_CTL: return "DL_L1_CTRL";
	case FSM_DP_MEM_TYPE_DL_RF: return "DL_RF";
	case FSM_DP_MEM_TYPE_UL: return "UL";
	default: return "unknown";
	}
}

static inline int fsm_dp_mmap_type_is_valid(enum fsm_dp_mmap_type type)
{
	return (type >= 0 && type < FSM_DP_MMAP_TYPE_LAST);
}

static inline const char *fsm_dp_mmap_type_to_str(enum fsm_dp_mmap_type type)
{
	switch (type) {
	case FSM_DP_MMAP_TYPE_MEM: return "Memory";
	case FSM_DP_MMAP_TYPE_RING: return "Ring";
	default: return "unknown";
	}
}

static inline int fsm_dp_rx_type_is_valid(enum fsm_dp_rx_type type)
{
	return (type >= 0 && type < FSM_DP_RX_TYPE_LAST);
}

static inline const char *fsm_dp_rx_type_to_str(enum fsm_dp_rx_type type)
{
	switch (type) {
	case FSM_DP_RX_TYPE_L1: return "L1";
	case FSM_DP_RX_TYPE_RF: return "RF";
	case FSM_DP_RX_TYPE_TA: return "TA";
	case FSM_DP_RX_TYPE_LPBK: return "LOOPBACK";
	default: return "unknown";
	}
}

#endif /* __FSM_DP_IOCTL_H__ */
