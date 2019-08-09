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

#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>

#include "fsm_dp.h"
#include "fsm_dp_mhi.h"

static struct fsm_dp_drv *__pdrv;

static int __mhi_rx_replenish(
	struct fsm_dp_mhi *mhi,
	struct fsm_dp_mempool *mempool)
{
	struct mhi_device *mhi_dev = mhi->mhi_dev;
	int nr = mhi_get_no_free_descriptors(mhi_dev, DMA_FROM_DEVICE);
	void *buf;
	int ret, i, to_xfer;
	bool outofbuf;

	ret = 0;
	if (nr < mhi_get_total_descriptors(mhi_dev, DMA_FROM_DEVICE) / 8)
		return ret;
	for (; nr > 0;) {
		to_xfer = min(FSM_DP_MAX_IOV_SIZE, nr);
		outofbuf = false;
		for (i = 0; i < to_xfer; i++) {
			buf = fsm_dp_mempool_get_buf(mempool);
			if (buf == NULL) {
				mhi->stats.rx_out_of_buf++;
				FSM_DP_DEBUG("%s: out of rx buffer!\n", __func__);
				outofbuf = true;
				break;
			}
			mhi->buf_array[i] = buf;
			mhi->size_array[i] = mempool->mem.buf_sz;
		}
		if (i == 0)
			return 0;
		to_xfer = i;
		ret = mhi_queue_n_transfer(mhi_dev,
						DMA_FROM_DEVICE,
						mhi->buf_array,
						mhi->size_array,
						mhi->flag_array,
						to_xfer);
		if (ret) {
			for (i = 0; i < to_xfer; i++)
				fsm_dp_mempool_put_buf(mempool,
							mhi->buf_array[i]);
			mhi->stats.rx_replenish_err++;
			FSM_DP_ERROR("%s: failed to load rx buf!\n",
				  __func__);
			return ret;
		}
		mhi->stats.rx_replenish++;
		if (outofbuf) {
			ret = -ENOMEM;
			break;
		}
		nr -= to_xfer;
	}

	return ret;
}

static void __mhi_ul_xfer_cb(
	struct mhi_device *mhi_dev,
	struct mhi_result *result)
{
	struct fsm_dp_drv *drv = mhi_device_get_devdata(mhi_dev);
	struct fsm_dp_mhi *mhi = &drv->mhi;
	void *addr = result->buf_addr;
	struct fsm_dp_mempool *mempool;

	FSM_DP_DEBUG("%s: ul_xfer_result addr=%p dir=%u bytes=%lu status=%d\n",
		     __func__, result->buf_addr, result->dir,
		     result->bytes_xferd, result->transaction_status);

	mhi->stats.tx_acked++;

	/* Try DL mempool first */
	mempool = fsm_dp_find_mempool(drv, addr, true);

	/* Try UL mempool for loopback packet */
	if (mempool == NULL)
		mempool = fsm_dp_find_mempool(drv, addr, false);

	if (unlikely(mempool == NULL)) {
		FSM_DP_DEBUG("%s: cannot find mempool, addr=%p\n",
			  __func__, addr);
		return;
	}

	switch (mempool->type) {
	case FSM_DP_MEM_TYPE_DL_L1_DATA:
		/* For DL_L1_DATA, don't put buffer back to the ring */
		break;
	default:
		fsm_dp_mempool_put_buf(mempool, addr);
		break;
	}
}

static void __mhi_dl_xfer_cb(
	struct mhi_device *mhi_dev,
	struct mhi_result *result)
{
	struct fsm_dp_drv *drv = mhi_device_get_devdata(mhi_dev);
	struct fsm_dp_mhi *mhi = &drv->mhi;
	struct fsm_dp_mempool *mempool = drv->mempool[FSM_DP_MEM_TYPE_UL];

	FSM_DP_DEBUG("%s: dl_xfer_result addr=%p dir=%u bytes=%lu status=%d\n",
		  __func__, result->buf_addr, result->dir,
		  result->bytes_xferd, result->transaction_status);

	if (result->transaction_status == -ENOTCONN) {
		mhi->stats.rx_err++;
		fsm_dp_mempool_put_buf(mempool, result->buf_addr);
	} else {
		mhi->stats.rx_cnt++;
		fsm_dp_rx(drv, result->buf_addr, result->bytes_xferd);
	}
}

static void __mhi_status_cb(struct mhi_device *mhi_dev, enum MHI_CB mhi_cb)
{

	struct fsm_dp_drv *pdrv = mhi_device_get_devdata(mhi_dev);

	if (mhi_cb != MHI_CB_PENDING_DATA)
		return;
	if (napi_schedule_prep(&pdrv->napi)) {
		__napi_schedule(&pdrv->napi);
		pdrv->stats.rx_int++;
		return;
	}
}

int fsm_dp_mhi_rx_replenish(struct fsm_dp_drv *drv)
{
	struct fsm_dp_mhi *mhi = &drv->mhi;
	struct fsm_dp_mempool *mempool = drv->mempool[FSM_DP_MEM_TYPE_UL];
	int ret;

	spin_lock_bh(&mhi->rx_lock);
	ret = __mhi_rx_replenish(mhi, mempool);
	spin_unlock_bh(&mhi->rx_lock);
	return ret;
}


static int fsm_dp_mhi_probe(
	struct mhi_device *mhi_dev,
	const struct mhi_device_id *id)
{
	struct fsm_dp_drv *pdrv = __pdrv;
	int ret;
	int i;

	FSM_DP_DEBUG("%s: probing mhi\n", __func__);

	if (__pdrv == NULL)
		return -ENODEV;

	mhi_device_set_devdata(mhi_dev, __pdrv);
	ret = mhi_prepare_for_transfer(mhi_dev);
	if (ret) {
		FSM_DP_ERROR("%s: mhi_prepare_for_transfer failed\n", __func__);
		return ret;
	}

	pdrv->mhi.mhi_dev = mhi_dev;
	for (i = 0; i < FSM_DP_MAX_IOV_SIZE; i++)
		pdrv->mhi.flag_array[i] = MHI_EOT;
	spin_lock_init(&pdrv->mhi.rx_lock);
	spin_lock_init(&pdrv->mhi.tx_lock);
	ret = fsm_dp_mhi_rx_replenish(pdrv);
	if (ret) {
		FSM_DP_ERROR("%s: fsm_dp_mhi_rx_replenish failed\n", __func__);
		return ret;
	}

	FSM_DP_DEBUG("%s: mhi_probed\n", __func__);
	return 0;
}

static void fsm_dp_mhi_remove(struct mhi_device *mhi_dev)
{
	mhi_unprepare_from_transfer(mhi_dev);
}

static struct mhi_device_id fsm_dp_mhi_match_table[] = {
	{ .chan = "IP_HW0" },
	{},
};

static struct mhi_driver __fsm_dp_mhi_drv = {
	.id_table = fsm_dp_mhi_match_table,
	.remove = fsm_dp_mhi_remove,
	.probe = fsm_dp_mhi_probe,
	.ul_xfer_cb = __mhi_ul_xfer_cb,
	.dl_xfer_cb = __mhi_dl_xfer_cb,
	.status_cb = __mhi_status_cb,
	.driver = {
		.name = FSM_DP_MHI_NAME,
		.owner = THIS_MODULE,
	},
};


int fsm_dp_mhi_init(struct fsm_dp_drv *pdrv)
{
	int ret = -EBUSY;

	if (__pdrv == NULL) {
		__pdrv = pdrv;
		ret = mhi_driver_register(&__fsm_dp_mhi_drv);
		if (ret) {
			__pdrv = NULL;
			pr_err("FSM-DP: mhi registration failed!\n");
			return ret;
		}

		pr_info("FSM-DP: Register MHI driver!\n");
	}
	return ret;
}

void fsm_dp_mhi_cleanup(struct fsm_dp_drv *pdrv)
{
	if (__pdrv) {
		mhi_driver_unregister(&__fsm_dp_mhi_drv);
		__pdrv = NULL;
		pr_info("FSM-DP: Unregister MHI driver\n");
	}
}
