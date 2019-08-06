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
#include <linux/list.h>

#include "fsm_dp.h"
#include "fsm_dp_mem.h"

static inline struct fsm_dp_mempool *fsm_dp_mem_to_mempool(
	struct fsm_dp_mem *mem)
{
	struct fsm_dp_mempool *mempool = container_of(mem,
						   struct fsm_dp_mempool, mem);
	return mempool;
}

static inline void fsm_dp_mem_loc_set(
	struct fsm_dp_mem_loc *loc,
	void *base,
	size_t size,
	dma_addr_t addr,
	unsigned int mmap_cookie)
{
	loc->base = base;
	loc->size = size;
	loc->addr = addr;
	loc->page_base = (void *)((unsigned long)base & PAGE_MASK);
	loc->page_off = (unsigned int)((unsigned long)base & (PAGE_SIZE - 1));
	loc->cookie = mmap_cookie;
}

static inline int __alloc(
	size_t size,
	unsigned int mmap_cookie,
	struct fsm_dp_mem_loc *loc)
{
	void *p = kzalloc(size, GFP_KERNEL);
	dma_addr_t addr;

	if (p) {
		addr = virt_to_phys(p);
		fsm_dp_mem_loc_set(loc, p, size, addr, mmap_cookie);
		return 0;
	}
	return -ENOMEM;
}

static inline void __free(struct fsm_dp_mem_loc *loc)
{
	if (loc && loc->base && loc->size) {
		kfree(loc->base);
		memset(loc, 0, sizeof(*loc));
	}
}

static inline int __dma_alloc(struct device *dev,
			      size_t size,
			      unsigned int mmap_cookie,
			      struct fsm_dp_mem_loc *loc)
{
	dma_addr_t dma_hdl;
	void *p = dma_alloc_coherent(dev, size, &dma_hdl, GFP_KERNEL);

	if (p) {
		fsm_dp_mem_loc_set(loc, p, size, dma_hdl, mmap_cookie);
		return 0;
	}
	return -ENOMEM;
}

static inline void __dma_free(struct device *dev, struct fsm_dp_mem_loc *loc)
{
	if (loc && loc->base && loc->size) {
		dma_free_coherent(dev, loc->size, loc->base, loc->addr);
		memset(loc, 0, sizeof(*loc));
	}
}

int fsm_dp_ring_init(
	struct fsm_dp_ring *ring,
	unsigned int ringsz,
	unsigned int mmap_cookie)
{
	unsigned int allocsz = ringsz * sizeof(*ring->element);
	char *aligned_ptr;
	fsm_dp_ring_element_t *elem_p;
	int i;

	/* cons and prod index space, aligned to cache line */
	allocsz += 4 * cache_line_size();
	allocsz += cache_line_size() - 1;

	if (__alloc(allocsz, mmap_cookie, &ring->loc)) {
		FSM_DP_ERROR("%s: failed to allocate ring memory\n", __func__);
		return -ENOMEM;
	}

	aligned_ptr = (char *)ALIGN((unsigned long)ring->loc.base,
				    cache_line_size());
	ring->prod_head = (fsm_dp_ring_index_t *)aligned_ptr;
	aligned_ptr += cache_line_size();
	ring->prod_tail = (fsm_dp_ring_index_t *)aligned_ptr;
	aligned_ptr += cache_line_size();
	ring->cons_head = (fsm_dp_ring_index_t *)aligned_ptr;
	aligned_ptr += cache_line_size();
	ring->cons_tail = (fsm_dp_ring_index_t *)aligned_ptr;
	aligned_ptr += cache_line_size();
	ring->element = elem_p = (fsm_dp_ring_element_t *)aligned_ptr;

	for (i = 0; i < ringsz; i++, elem_p++)
		elem_p->element_ctrl = 1; /* not valid */
	ring->size = ringsz;
	return 0;
}

void fsm_dp_ring_cleanup(struct fsm_dp_ring *ring)
{
	if (ring) {
		__free(&ring->loc);
		memset(ring, 0, sizeof(*ring));
	}
}

int fsm_dp_ring_get_cfg(struct fsm_dp_ring *ring, struct fsm_dp_ring_cfg *cfg)
{
	if (unlikely(ring == NULL || cfg == NULL))
		return -EINVAL;

	cfg->mmap.length = ring->loc.size;
	cfg->mmap.offset = ring->loc.page_off;
	cfg->mmap.cookie = ring->loc.cookie;

	cfg->size = ring->size;
	cfg->prod_head_off = ring->loc.page_off +
		vaddr_offset((void *)ring->prod_head, ring->loc.base);
	cfg->prod_tail_off = ring->loc.page_off +
		vaddr_offset((void *)ring->prod_tail, ring->loc.base);
	cfg->cons_head_off = ring->loc.page_off +
		vaddr_offset((void *)ring->cons_head, ring->loc.base);
	cfg->cons_tail_off = ring->loc.page_off +
		vaddr_offset((void *)ring->cons_tail, ring->loc.base);
	cfg->ringbuf_off = ring->loc.page_off +
		vaddr_offset((void *)ring->element, ring->loc.base);
	return 0;
}

/* Read from ring */
int fsm_dp_ring_read(
	struct fsm_dp_ring *ring,
	fsm_dp_ring_element_data_t *element_ptr, unsigned int *flag)
{
	register fsm_dp_ring_index_t cons_head, cons_next, cons_tail;
	register fsm_dp_ring_index_t prod_tail, mask;
	fsm_dp_ring_element_data_t data;

	if (unlikely(ring == NULL))
		return -EINVAL;

	mask = ring->size - 1;

again:
	/* test to see if the ring is empty.
	 * If not, advance cons_head and read the data
	 */
	cons_head = *ring->cons_head;
	prod_tail = *ring->prod_tail;
	rmb();	/* Get current cons_head and prod_tail */
	if ((cons_head & mask) == (prod_tail & mask)) {
		ring->opstats.read_empty++;
		return -EAGAIN;
	}
	cons_next = cons_head + 1;
	if (atomic_cmpxchg((atomic_t *)ring->cons_head,
			   cons_head,
			   cons_next) != cons_head) {
		ring->opstats.cons_head_updt_retry++;
		goto again;
	}

	/* Read the ring */
	data = ring->element[(cons_head & mask)].element_data;
	if (flag)
		*flag = ring->element[(cons_head & mask)].element_ctrl >> 1;
	rmb();	/* Get current element */

	/* After read, write to ring with bit0 on */

	ring->element[(cons_head & mask)].element_ctrl = 1;
	wmb();	/* Ensure element is written */

	if (element_ptr)
		*element_ptr = data;

	/* Move the tail */
	cons_tail = *ring->cons_tail;
	rmb();	/* Get current cons_tail */

	/* If tail is behind, let other producer to update it */
	if (cons_head != cons_tail) {
		ring->opstats.cons_tail_no_updt++;
		return 0;
	}

repeat:
	/* Potential two consumer is updating */
	if (atomic_cmpxchg((atomic_t *)ring->cons_tail,
			   cons_tail,
			   cons_next) != cons_tail) {
		/* the other producer wins */
		ring->opstats.cons_tail_updt_backoff++;
		return 0;
	}

	ring->opstats.cons_tail_updt++;
	cons_tail = cons_next;
	cons_next++;

	/* This consumer win, read the cons_head */
	cons_head = *ring->cons_head;
	rmb();	/* Get current cons_head */

	if (cons_tail == cons_head)
		return 0;

	/* The reader has not cleared the bit0 */
	if (!(ring->element[(cons_tail & mask)].element_ctrl & 1)) {
		ring->opstats.cons_tail_updt_stop++;
		return 0;
	}

	goto repeat;
}

/* Write to ring */
int fsm_dp_ring_write(struct fsm_dp_ring *ring, fsm_dp_ring_element_data_t data,
		unsigned int flag)
{
	register fsm_dp_ring_index_t prod_head, prod_next, prod_tail;
	register fsm_dp_ring_index_t cons_tail, mask;

	if (unlikely(ring == NULL))
		return -EINVAL;

	mask = ring->size - 1;

again:
	/* test to see if the ring is full.
	 * If not, advance prod_head and write the data
	 */
	prod_head = *ring->prod_head;
	cons_tail = *ring->cons_tail;
	rmb();	/* Get current prod_head and cons_tail */
	prod_next = prod_head + 1;
	if ((prod_next & mask) == (cons_tail & mask)) {
		ring->opstats.write_full++;
		return -EAGAIN;
	}
	if (atomic_cmpxchg((atomic_t *)ring->prod_head,
			   prod_head,
			   prod_next) != prod_head) {
		ring->opstats.prod_head_updt_retry++;
		goto again;
	}

#ifdef CONFIG_FSM_DP_TEST
	if (data == TEST_RING_WRITE_MAGIC_VALUE)
		data = prod_head << 1;
#endif
	/* Write to ring buffer with bit0 off */
	ring->element[(prod_head & mask)].element_data = data;
	ring->element[(prod_head & mask)].element_ctrl = flag << 1;
	wmb();	/* Ensure element is written */

	ring->opstats.write_ok++;
	/* Move the tail */
	prod_tail = *ring->prod_tail;
	rmb();	/* Get current prod_tail */

	/* If tail is behind, let other producer to update it */
	if (prod_head != prod_tail) {
		ring->opstats.prod_tail_no_updt++;
		return 0;
	}

repeat:
	/* Potential two producer is updating */
	if (atomic_cmpxchg((atomic_t *)ring->prod_tail,
			   prod_tail,
			   prod_next) != prod_tail) {
		/* the other producer wins */
		ring->opstats.prod_tail_updt_backoff++;
		return 0;
	}

	ring->opstats.prod_tail_updt++;
	prod_tail = prod_next;
	prod_next++;

	/* This producer win, read the prod_head */
	prod_head = *ring->prod_head;
	rmb();	/* Get current prod_head */

	if (prod_tail == prod_head)
		return 0;

	/* The writer has not written the data yet */
	if (ring->element[(prod_tail & mask)].element_ctrl & 1) {
		ring->opstats.prod_tail_updt_stop++;
		return 0;
	}

	goto repeat;
}

bool fsm_dp_ring_is_empty(struct fsm_dp_ring *ring)
{
	fsm_dp_ring_index_t prod_tail, cons_tail;

	prod_tail = *ring->prod_tail;
	cons_tail = *ring->cons_tail;
	if (prod_tail == cons_tail)
		return true;
	return false;
}

static int fsm_dp_mem_init(
	struct fsm_dp_mem *mem,
	unsigned int bufcnt,
	unsigned int bufsz,
	unsigned int cookie)
{
	struct fsm_dp_mempool *mempool = fsm_dp_mem_to_mempool(mem);
	struct fsm_dp_drv *pdrv = mempool->drv;

	if (__dma_alloc(pdrv->dev, bufcnt * bufsz, cookie, &mem->loc)) {
		FSM_DP_ERROR("%s: failed to allocate DMA memory\n", __func__);
		return -ENOMEM;
	}

	mem->buf_cnt = bufcnt;
	mem->buf_sz = bufsz;
	return 0;
}

static void fsm_dp_mem_cleanup(struct fsm_dp_mem *mem)
{
	struct fsm_dp_mempool *mempool = fsm_dp_mem_to_mempool(mem);
	struct fsm_dp_drv *pdrv = mempool->drv;

	__dma_free(pdrv->dev, &mem->loc);
	memset(mem, 0, sizeof(*mem));
}

static int fsm_dp_mem_get_cfg(
	struct fsm_dp_mem *mem,
	struct fsm_dp_mem_cfg *cfg)
{
	cfg->mmap.length = mem->loc.size;
	cfg->mmap.offset = mem->loc.page_off;
	cfg->mmap.cookie = mem->loc.cookie;

	cfg->buf_sz = mem->buf_sz;
	cfg->buf_cnt = mem->buf_cnt;
	return 0;
}

static struct fsm_dp_mempool *__try_and_hold_drv_mempool(
	struct fsm_dp_drv *pdrv,
	enum fsm_dp_mem_type type,
	unsigned int buf_sz,
	unsigned int buf_cnt)
{
	struct fsm_dp_mempool *mempool;

	mempool = pdrv->mempool[type];
	if (mempool == NULL)
		return NULL;

	if (buf_sz > mempool->mem.buf_sz || buf_cnt > mempool->mem.buf_cnt) {
		FSM_DP_ERROR("%s: can't use existing mempool, type=%u\n",
			  __func__, type);
		return NULL;
	}

	__fsm_dp_mempool_hold(mempool);
	FSM_DP_DEBUG("%s: use existing mempool, type=%u\n", __func__, type);
	return mempool;
}

static inline struct fsm_dp_mempool *try_and_hold_drv_mempool(
	struct fsm_dp_drv *pdrv,
	enum fsm_dp_mem_type type,
	unsigned int buf_sz,
	unsigned int buf_cnt)
{
	struct fsm_dp_mempool *mempool;
	unsigned long flag;

	spin_lock_irqsave(&pdrv->mempool_lock, flag);
	mempool = __try_and_hold_drv_mempool(pdrv, type, buf_sz, buf_cnt);
	spin_unlock_irqrestore(&pdrv->mempool_lock, flag);

	return mempool;
}

static void fsm_dp_mempool_init(struct fsm_dp_mempool *mempool)
{
	struct fsm_dp_mem *mem = &mempool->mem;
	struct fsm_dp_ring *ring = &mempool->ring;
	fsm_dp_ring_element_data_t element_data;
	int i;

	switch (mempool->type) {
	case FSM_DP_MEM_TYPE_DL_L1_DATA:
	case FSM_DP_MEM_TYPE_DL_L1_CTL:
	case FSM_DP_MEM_TYPE_DL_RF:
	case FSM_DP_MEM_TYPE_UL:
		element_data = mem->loc.page_off;
		for (i = 0; i < mem->buf_cnt; i++) {
			ring->element[i].element_data = element_data;
			ring->element[i].element_ctrl = 0; /* entry valid */
			element_data += mem->buf_sz;
		}
		*ring->cons_head = 0;
		*ring->cons_tail = 0;
		*ring->prod_head = i - 1;
		*ring->prod_tail = i - 1;
		wmb();	/* Ensure all the data are written */
		break;
	default:
		break;
	}
}

static struct fsm_dp_mempool *__fsm_dp_mempool_alloc(
	struct fsm_dp_drv *pdrv,
	enum fsm_dp_mem_type type,
	unsigned int buf_sz,
	unsigned int buf_cnt,
	unsigned int ring_sz)
{
	struct fsm_dp_mempool *mempool;
	unsigned int cookie;

	mempool = kzalloc(sizeof(*mempool), GFP_KERNEL);
	if (IS_ERR(mempool)) {
		FSM_DP_ERROR("%s: failed to allocate mempool\n", __func__);
		return NULL;
	}

	mempool->drv = pdrv;
	mempool->type = type;

	cookie = MMAP_COOKIE(type, FSM_DP_MMAP_TYPE_MEM);
	if (fsm_dp_mem_init(&mempool->mem, buf_cnt, buf_sz, cookie)) {
		FSM_DP_ERROR("%s: failed to initialize memory\n", __func__);
		goto cleanup;
	}

	cookie = MMAP_COOKIE(type, FSM_DP_MMAP_TYPE_RING);
	if (fsm_dp_ring_init(&mempool->ring, ring_sz, cookie)) {
		FSM_DP_ERROR("%s: failed to initialize ring\n", __func__);
		goto cleanup_mem;
	}

	fsm_dp_mempool_init(mempool);

	FSM_DP_DEBUG("%s: mempool is created, type=%u bufsz=%u bufcnt=%u\n",
		  __func__, type, buf_sz, buf_cnt);

	return mempool;

cleanup_mem:
	fsm_dp_mem_cleanup(&mempool->mem);
cleanup:
	kfree(mempool);
	return NULL;
}

static void __fsm_dp_mempool_free(struct fsm_dp_mempool *mempool)
{
	if (mempool) {
		enum fsm_dp_mem_type type = mempool->type;

		fsm_dp_mem_cleanup(&mempool->mem);
		fsm_dp_ring_cleanup(&mempool->ring);
		kfree(mempool);

		FSM_DP_DEBUG("%s: mempool is freed, type=%u\n", __func__, type);
	}
}

struct fsm_dp_mempool *fsm_dp_mempool_alloc(
	struct fsm_dp_drv *pdrv,
	enum fsm_dp_mem_type type,
	unsigned int buf_sz,
	unsigned int buf_cnt)
{
	struct fsm_dp_mempool *mempool, *drv_mempool;
	unsigned int ring_sz;
	unsigned long flag;

	if (unlikely(!buf_sz || !buf_cnt || !fsm_dp_mem_type_is_valid(type)))
		return NULL;
	if (unlikely(((ULONG_MAX) / buf_sz) < buf_cnt))
		return NULL;

	ring_sz = calc_ring_size(buf_cnt);
	if (unlikely(!ring_sz))
		return NULL;

	drv_mempool = try_and_hold_drv_mempool(
		pdrv, type, buf_sz, buf_cnt);
	if (drv_mempool)
		return drv_mempool;

	mempool = __fsm_dp_mempool_alloc(pdrv, type, buf_sz, buf_cnt, ring_sz);
	if (mempool == NULL)
		return NULL;

	spin_lock_irqsave(&pdrv->mempool_lock, flag);
	drv_mempool = __try_and_hold_drv_mempool(
		pdrv, type, buf_sz, buf_cnt);
	if (likely(drv_mempool == NULL)) {
		pdrv->mempool[type] = mempool;
		__fsm_dp_mempool_hold(mempool);
	}
	spin_unlock_irqrestore(&pdrv->mempool_lock, flag);

	if (unlikely(drv_mempool)) {
		__fsm_dp_mempool_free(mempool);
		return drv_mempool;
	}

	return mempool;
}

void fsm_dp_mempool_free(struct fsm_dp_mempool *mempool)
{
	if (mempool) {
		if (mempool->drv) {
			struct fsm_dp_drv *pdrv = mempool->drv;
			unsigned long flag;

			spin_lock_irqsave(&pdrv->mempool_lock, flag);
			pdrv->mempool[mempool->type] = NULL;
			spin_unlock_irqrestore(&pdrv->mempool_lock, flag);
		}

		__fsm_dp_mempool_free(mempool);
	}
}

int fsm_dp_mempool_get_cfg(
	struct fsm_dp_mempool *mempool,
	struct fsm_dp_mempool_cfg *cfg)
{
	if (unlikely(mempool == NULL || cfg == NULL))
		return -EINVAL;

	cfg->type = mempool->type;
	fsm_dp_mem_get_cfg(&mempool->mem, &cfg->mem);
	fsm_dp_ring_get_cfg(&mempool->ring, &cfg->ring);
	return 0;
}

int fsm_dp_mempool_put_buf(struct fsm_dp_mempool *mempool, void *vaddr)
{
	struct fsm_dp_mem *mem;
	unsigned long offset;
	int ret;

	if (unlikely(mempool == NULL || vaddr == NULL))
		return -EINVAL;

	mem = &mempool->mem;
	if (!vaddr_in_range(vaddr, mem->loc.base, mem->loc.size)) {
		mempool->stats.invalid_buf_put++;
		FSM_DP_DEBUG("%s: address(%p) not in range\n", __func__, vaddr);
		return -EINVAL;
	}

	offset = vaddr_offset(vaddr, mem->loc.base);
	/* align to buffer boundary */
	offset -= offset % mem->buf_sz;
	/* align to page boundary for mmap */
	offset += mem->loc.page_off;

	ret = fsm_dp_ring_write(&mempool->ring, (fsm_dp_ring_element_data_t)offset, 0);
	if (ret)
		mempool->stats.buf_put_err++;
	else
		mempool->stats.buf_put++;

	return ret;
}

void *fsm_dp_mempool_get_buf(struct fsm_dp_mempool *mempool)
{
	struct fsm_dp_mem *mem;
	fsm_dp_ring_element_data_t val;
	unsigned int flag;
	unsigned long offset;
	void *ptr;

	if (unlikely(mempool == NULL))
		return NULL;

	if (fsm_dp_ring_read(&mempool->ring, &val, &flag)) {
		mempool->stats.buf_get_err++;
		return NULL;
	}

	mem = &mempool->mem;
	ptr = (char *)mem->loc.page_base + val;
	offset = vaddr_offset(ptr, mem->loc.base);
	if (offset % mem->buf_sz) {
		mempool->stats.invalid_buf_get++;
		FSM_DP_ERROR("%s: get unaligned buffer from ring\n", __func__);
		return NULL;
	}
	mempool->stats.buf_get++;
	return ptr;
}

struct fsm_dp_mempool *fsm_dp_find_mempool(
	struct fsm_dp_drv *pdrv,
	void *addr,
	bool tx)
{
	struct fsm_dp_mempool *mempool = NULL;
	struct fsm_dp_mem *mem;
	unsigned int mem_type, mem_type_last;

	if (unlikely(pdrv == NULL || addr == NULL))
		return NULL;

	if (tx) {
		mem_type = 0;
		mem_type_last = FSM_DP_MEM_TYPE_UL;
	} else {
		mem_type = FSM_DP_MEM_TYPE_UL;
		mem_type_last = FSM_DP_MEM_TYPE_LAST;
	}

	for (; mem_type < mem_type_last; mem_type++) {
		mempool = pdrv->mempool[mem_type];
		if (mempool) {
			mem = &mempool->mem;
			if (vaddr_in_range(addr, mem->loc.base, mem->loc.size))
				return mempool;
		}
	}
	return NULL;
}
