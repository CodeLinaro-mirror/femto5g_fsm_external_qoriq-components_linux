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
#ifndef __FSM_DP_MHI_H__
#define __FSM_DP_MHI_H__

#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/mhi.h>

#define FSM_DP_MHI_NAME	"fsm-l1rf-mhi"

struct fsm_dp_drv;

struct fsm_dp_mhi_stats {
	unsigned long tx_cnt;
	unsigned long tx_acked;
	unsigned long tx_err;
	unsigned long rx_cnt;
	unsigned long rx_err;
	unsigned long rx_out_of_buf;

	unsigned long rx_replenish;
	unsigned long rx_replenish_err;
};

struct fsm_dp_mhi {
	struct mhi_device *mhi_dev;
	struct fsm_dp_mhi_stats stats;
	spinlock_t rx_lock;
	spinlock_t tx_lock;
	/*
	 * the following are for needed storage
	 * for doing replenish at context of
	 * of fsm_dp_mhi_rx_replenish callback.
	 * No locking is necessary.
	 */
	void *buf_array[FSM_DP_MAX_IOV_SIZE];
	size_t size_array[FSM_DP_MAX_IOV_SIZE];
	enum MHI_FLAGS flag_array[FSM_DP_MAX_IOV_SIZE];
};

int fsm_dp_mhi_init(struct fsm_dp_drv *pdrv);
void fsm_dp_mhi_cleanup(struct fsm_dp_drv *pdrv);

int fsm_dp_mhi_rx_replenish(struct fsm_dp_drv *drv);

static inline int fsm_dp_mhi_n_tx(struct fsm_dp_mhi *mhi,
				void *msg_array[],
				size_t msglen_array[],
				enum MHI_FLAGS flag_array[], unsigned int num)
{
	int ret;

	ret = mhi_queue_n_transfer(mhi->mhi_dev,
				 DMA_TO_DEVICE,
				 msg_array, msglen_array,
				 flag_array, num);
	if (!ret)
		mhi->stats.tx_cnt += num;
	else
		mhi->stats.tx_err += num;
	return ret;
}

static inline int fsm_dp_mhi_tx(struct fsm_dp_mhi *mhi,
				void *msg,
				unsigned int msglen,
				enum MHI_FLAGS flag)
{
	int ret;

	ret = mhi_queue_transfer(mhi->mhi_dev,
				 DMA_TO_DEVICE,
				 msg, msglen,
				 flag);
	if (!ret)
		mhi->stats.tx_cnt++;
	else
		mhi->stats.tx_err++;
	return ret;
}

static inline bool fsm_dp_mhi_is_ready(struct fsm_dp_mhi *mhi)
{
	return ((mhi->mhi_dev) ? true : false);
}

#endif /* __FSM_DP_MHI_H__ */
