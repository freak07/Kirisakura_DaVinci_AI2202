/*
 * Expanded RAM block device
 * Description: expanded memory implement
 *
 * Released under the terms of GNU General Public License Version 2.0
 *
 */

#define KMSG_COMPONENT "expandmem"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/blkdev.h>
#include <linux/sched/task.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/memcontrol.h>
#include <linux/swap.h>

#include "../zram_drv.h"
#include "expandmem.h"
#include "eswap.h"
#include "eswap_common.h"
#include "eswap_lru.h"
#include "eswap_area.h"

struct async_req {
	struct mem_cgroup *mcg;
	unsigned long size;
	struct work_struct work;
	int nice;
	bool preload;
};

struct io_priv {
	struct zram *zram;
	enum eswap_scenario scenario;
	struct eswap_page_pool page_pool;
};

struct schedule_para {
	void *io_handler;
	struct eswap_entry *io_entry;
	struct eswap_buffer io_buf;
	struct io_priv priv;
};

#define GET_EXTENT_MAX_TIMES (100 * 1000)

static void eswap_memcg_iter(
	int (*iter)(struct mem_cgroup *, void *), void *data)
{
	struct mem_cgroup *mcg = get_next_memcg(NULL);

	while (mcg) {
		if (iter(mcg, data)) {
			get_next_memcg_break(mcg);
			return;
		}
		mcg = get_next_memcg(mcg);
	}
}

struct oem_zram_data *get_oem_zram(struct zram *zram)
{
	struct oem_zram_data *oem_zram;

	if (!zram) {
		eswap_print(LEVEL_WARN, "zram invalid\n");
		return NULL;
	}
	oem_zram = (struct oem_zram_data *)zram->android_oem_data1;
	if (unlikely(!oem_zram)) {
		eswap_print(LEVEL_ERR, "zram data invalid\n");
		return NULL;
	}
	return oem_zram;
}

static unsigned long memcg_reclaim_size(struct mem_cgroup *memcg)
{
	struct oem_mem_cgroup *oem_memcg = get_oem_mem_cgroup(memcg);

	if (oem_memcg) {
		unsigned long zram_size = atomic64_read(&oem_memcg->zram_stored_size);
		unsigned long cur_size = atomic64_read(&oem_memcg->eswap_stored_size);
		unsigned long new_size = (zram_size + cur_size) *
				atomic_read(&oem_memcg->ub_zram2ufs_ratio) / 100;

		return (new_size > cur_size) ? (new_size - cur_size) : 0;
	}
	return 0;
}

static int eswap_permcg_sz(struct mem_cgroup *memcg, void *data)
{
	unsigned long *out_size = (unsigned long *)data;

	*out_size += memcg_reclaim_size(memcg);
	return 0;
}

static void eswap_reclaimin_inc(void)
{
	struct eswap_stat *stat;

	stat = eswap_get_stat_obj();
	if (unlikely(!stat))
		return;
	atomic64_inc(&stat->reclaimin_infight);
}

static void eswap_reclaimin_dec(void)
{
	struct eswap_stat *stat;

	stat = eswap_get_stat_obj();
	if (unlikely(!stat))
		return;
	atomic64_dec(&stat->reclaimin_infight);
}

static int eswap_update_reclaim_sz(unsigned long *require_size,
						unsigned long *mcg_reclaim_sz,
						unsigned long reclaim_size)
{
	if (*require_size > reclaim_size) {
		*require_size -= reclaim_size;
	} else {
		*require_size = 0;
	}

	if (*mcg_reclaim_sz > reclaim_size) {
		*mcg_reclaim_sz -= reclaim_size;
	} else {
		return -EINVAL;
	}

	if (*mcg_reclaim_sz < MIN_RECLAIM_ZRAM_SZ)
		return -EAGAIN;
	return 0;
}

static void eswap_fill_entry(struct eswap_entry *io_entry,
					struct eswap_buffer *io_buf,
					void *private)
{
	io_entry->addr = io_entry->ext_id * EXTENT_SECTOR_SIZE;
	io_entry->dest_pages = io_buf->dest_pages;
	io_entry->pages_sz = EXTENT_PG_CNT;
	io_entry->private = private;
}

static int eswap_reclaim_check(struct mem_cgroup *memcg,
					unsigned long *require_size,
					unsigned long *mcg_reclaim_sz)
{
	struct oem_mem_cgroup *oem_memcg = get_oem_mem_cgroup(memcg);
	if (unlikely(!oem_memcg || !oem_memcg->zram)) {
		return -EAGAIN;
	}
	if (*require_size < MIN_RECLAIM_ZRAM_SZ) {
		return -EIO;
	}

	*mcg_reclaim_sz = memcg_reclaim_size(memcg);

	if (*mcg_reclaim_sz < MIN_RECLAIM_ZRAM_SZ)
		return -EAGAIN;

	return 0;
}

static void eswap_flush_cb(enum eswap_scenario scenario, void *pri)
{
	switch (scenario) {
	case ESWAP_FAULT_OUT:
		eswap_extent_destroy(pri, scenario);
		break;
	case ESWAP_RECLAIM_IN:
		eswap_extent_register(pri);
		break;
	default:
		break;
	}
}

static void eswap_free_pagepool(struct schedule_para *sched)
{
	struct page *free_page = NULL;

	spin_lock(&sched->priv.page_pool.page_pool_lock);
	while (!list_empty(&sched->priv.page_pool.page_pool_list)) {
		free_page = list_first_entry(
			&sched->priv.page_pool.page_pool_list,
				struct page, lru);
		list_del_init(&free_page->lru);
		__free_page(free_page);
	}
	spin_unlock(&sched->priv.page_pool.page_pool_lock);
}

static void eswap_flush_done(struct eswap_entry *io_entry, int err)
{
	struct io_priv *priv = (struct io_priv *)(io_entry->private);

	if (likely(!err)) {
		eswap_flush_cb(priv->scenario,
				io_entry->manager_private);

		if (!zram_watermark_ok()) {
			wake_all_zswapd();
		}

	} else {
		eswap_extent_exception(priv->scenario,
				io_entry->manager_private);
	}
	eswap_free(io_entry);
}

static void eswap_plug_complete(void *data)
{
	struct schedule_para *sched  = (struct schedule_para *)data;
	eswap_free_pagepool(sched);
	eswap_free(sched);
}

static void *eswap_init_plug(struct zram *zram,
				enum eswap_scenario scenario,
				struct schedule_para *sched)
{
	struct eswap_io io_para;
	struct oem_zram_data *oem_zram = get_oem_zram(zram);

	io_para.bdev = oem_zram->bdev;
	io_para.scenario = scenario;
	io_para.private = (void *)sched;
	INIT_LIST_HEAD(&sched->priv.page_pool.page_pool_list);
	spin_lock_init(&sched->priv.page_pool.page_pool_lock);
	io_para.done_callback = eswap_flush_done;
	switch (io_para.scenario) {
	case ESWAP_RECLAIM_IN:
		io_para.complete_notify = eswap_plug_complete;
		sched->io_buf.pool = NULL;
		break;
	case ESWAP_FAULT_OUT:
		io_para.complete_notify = NULL;
		sched->io_buf.pool = NULL;
		break;
	default:
		break;
	}
	sched->io_buf.zram = zram;
	sched->priv.zram = zram;
	sched->priv.scenario = io_para.scenario;
	return eswap_plug_start(&io_para);
}

static int eswap_reclaim_extent(struct mem_cgroup *memcg,
					struct schedule_para *sched,
					unsigned long *require_size,
					unsigned long *mcg_reclaim_sz,
					int *io_err)
{
	int ret;
	unsigned long reclaim_size;

	sched->io_entry = eswap_malloc(
		sizeof(struct eswap_entry), false, true);
	if (unlikely(!sched->io_entry)) {
		eswap_print(LEVEL_WARN, "alloc io entry failed\n");
		*require_size = 0;
		*io_err = -ENOMEM;
		return *io_err;
	}

	reclaim_size = eswap_extent_create(
			memcg, &sched->io_entry->ext_id,
			&sched->io_buf,
			&sched->io_entry->manager_private);
	if (unlikely(!reclaim_size)) {
		if (sched->io_entry->ext_id != -ENOENT)
			*require_size = 0;
		eswap_free(sched->io_entry);
		return -EAGAIN;
	}

	eswap_fill_entry(sched->io_entry, &sched->io_buf,
				(void *)(&sched->priv));

	ret = eswap_write_extent(sched->io_handler,
						sched->io_entry);
	if (unlikely(ret)) {
		eswap_print(LEVEL_WARN, "eswap write failed, %d\n", ret);
		*require_size = 0;
		*io_err = ret;
		return *io_err;
	}

	return eswap_update_reclaim_sz(require_size,
				mcg_reclaim_sz, reclaim_size);
}

static int eswap_permcg_reclaim(
			struct mem_cgroup *memcg, void *data)
{
	/*
	 * 1. Recaim one extent from ZRAM in cur memcg
	 * 2. If extent exist, dispatch it to UFS
	 *
	 */
	int ret;
	int io_err = 0;
	unsigned long *require_size = (unsigned long *)data;
	unsigned long mcg_reclaim_sz;
	struct schedule_para *sched = NULL;
	struct oem_mem_cgroup *oem_memcg = get_oem_mem_cgroup(memcg);

	if (!oem_memcg) {
		return 0;
	}

	ret = eswap_reclaim_check(memcg, require_size, &mcg_reclaim_sz);
	if (ret) {
		return ret == -EAGAIN ? 0 : ret;
	}

	sched = eswap_malloc(sizeof(struct schedule_para), false, true);
	if (unlikely(!sched)) {
		eswap_print(LEVEL_WARN, "alloc sched failed\n");
		return -ENOMEM;
	}

	sched->io_handler = eswap_init_plug(oem_memcg->zram,
				ESWAP_RECLAIM_IN, sched);
	if (unlikely(!sched->io_handler)) {
		eswap_print(LEVEL_WARN, "plug start failed\n");
		eswap_free(sched);
		ret = -EIO;
		goto out;
	}

	while (*require_size) {
		if (eswap_reclaim_extent(memcg, sched,
			require_size, &mcg_reclaim_sz, &io_err))
			break;
		atomic64_inc(&oem_memcg->eswap_outextcnt);
	}
	ret = eswap_plug_finish(sched->io_handler);
	if (unlikely(ret)) {
		eswap_print(LEVEL_WARN, "write flush failed, %d\n", ret);
		*require_size = 0;
	} else {
		ret = io_err;
	}
	atomic64_inc(&oem_memcg->eswap_outcnt);
out:
	return ret;
}

static void eswap_reclaim_work(struct work_struct *work)
{
	/*
	 * 1. Set process priority
	 * 2. Reclaim object from each memcg
	 *
	 */
	struct async_req *rq = container_of(work, struct async_req, work);
	int old_nice = task_nice(current);

	set_user_nice(current, rq->nice);
	eswap_reclaimin_inc();

	eswap_memcg_iter(eswap_permcg_reclaim, &rq->size);

	eswap_reclaimin_dec();
	set_user_nice(current, old_nice);
	eswap_free(rq);
}

/*
 * This interface is for anon mem reclaim
 *
 */
unsigned long eswap_reclaim_in(unsigned long size)
{
	/*
	 * 1. Estimate push in size
	 * 2. Wakeup push in worker
	 *
	 */
	struct async_req *rq = NULL;
	unsigned long out_size = 0;

	if (!eswap_get_enable()  || !eswap_get_reclaim_in_enable()) {
		return 0;
	}

	eswap_memcg_iter(eswap_permcg_sz, &out_size);
	if (!out_size)
		return 0;

	rq = eswap_malloc(sizeof(struct async_req), false, true);
	if (unlikely(!rq)) {
		eswap_print(LEVEL_ERR, "alloc async req fail\n");
		return 0;
	}

	eswap_print(LEVEL_DEBUG, "size:%lu out_size:%lu \n", size, out_size);

	rq->size = size;
	rq->nice = task_nice(current);
	INIT_WORK(&rq->work, eswap_reclaim_work);
	queue_work(eswap_get_reclaim_workqueue(), &rq->work);

	return out_size > size ? size : out_size;
}

/*
 * The function is used to del ZS object from ZRAM LRU list
 */
static void eswap_zram_lru_del(struct zram *zram, u32 index)
{
	struct eswap_stat *stat = eswap_get_stat_obj();
	struct oem_zram_data *oem_zram = get_oem_zram(zram);

	if (!stat) {
		eswap_print(LEVEL_WARN, "stat is null\n");
		return;
	}
	if (!zram || !oem_zram || !oem_zram->area) {
		eswap_print(LEVEL_WARN, "zram is null\n");
		return;
	}
	if (index >= (u32)oem_zram->area->nr_objs) {
		eswap_print(LEVEL_WARN, "index = %d invalid\n", index);
		return;
	}

	zram_clear_flag(zram, index, ZRAM_FROM_ESWAP);
	if (zram_test_flag(zram, index, ZRAM_MCGID_CLEAR)) {
		zram_clear_flag(zram, index, ZRAM_MCGID_CLEAR);
	}
	if (zram_test_flag(zram, index, ZRAM_WB)) {
		zram_rmap_erase(zram, index);
		zram_clear_flag(zram, index, ZRAM_WB);
		zram_set_memcg(zram, index, NULL);
		zram_set_handle(zram, index, 0);
	} else {
		zram_lru_del(zram, index);
	}
}

/*
 * This interface will be called when anon page is added to ZRAM
 * Expandmem should trace this ZRAM in memcg LRU list
 *
 */
static void eswap_track(struct zram *zram, u32 index,
				struct mem_cgroup *memcg)
{
	struct oem_mem_cgroup *oem_memcg;

	if (!eswap_get_enable()) {
		return;
	}

	oem_memcg = get_oem_mem_cgroup(memcg);

	/*
	 * 1. Add ZRAM obj into LRU
	 * 2. Updata the stored size in memcg and ZRAM
	 *
	 */
	if (unlikely(!memcg || !oem_memcg)) {
		return;
	}

	if (unlikely(!oem_memcg->zram)) {
		spin_lock(&oem_memcg->zram_init_lock);
		if (!oem_memcg->zram)
			eswap_manager_memcg_init(memcg, zram);
		spin_unlock(&oem_memcg->zram_init_lock);
	}

	eswap_zram_lru_add(zram, index, memcg);
}

/*
 * This interface will be called when anon page in ZRAM is freed
 *
 */
static void eswap_untrack(struct zram *zram, u32 index)
{
	/*
	 * 1. When the ZS object in the writeback or swapin, it can't be untrack
	 * 2. Updata the stored size in memcg and ZRAM
	 * 3. Remove ZRAM obj from LRU
	 *
	 */
	if (!eswap_get_enable())
		return;

	while (zram_test_flag(zram, index, ZRAM_UNDER_WB) ||
		zram_test_flag(zram, index, ZRAM_UNDER_FAULTOUT)) {
		zram_slot_unlock(zram, index);
		udelay(50);
		zram_slot_lock(zram, index);
	}

	eswap_zram_lru_del(zram, index);
}

struct mem_cgroup *eswap_zram_get_memcg(struct zram *zram, u32 index)
{
	return zram_get_memcg(zram, index);
}

static void eswap_faultcheck_stat(struct zram *zram, u32 index)
{
	struct mem_cgroup *mcg = NULL;
	struct eswap_stat *stat = eswap_get_stat_obj();
	struct oem_mem_cgroup *oem_memcg = NULL;

	if (unlikely(!stat))
		return;

	atomic64_inc(&stat->fault_check_cnt);

	mcg = eswap_zram_get_memcg(zram, index);
	if (mcg) {
		oem_memcg = get_oem_mem_cgroup(mcg);
		if (oem_memcg) {
			atomic64_inc(&oem_memcg->eswap_allfaultcnt);
		}
	}
}

static bool eswap_fault_out_check(struct zram *zram,
					u32 index, unsigned long *zentry)
{
	if (!eswap_get_enable())
		return false;

	eswap_faultcheck_stat(zram, index);

	if (!zram_test_flag(zram, index, ZRAM_WB))
		return false;

	zram_set_flag(zram, index, ZRAM_UNDER_FAULTOUT);
	*zentry = zram_get_handle(zram, index);
	zram_slot_unlock(zram, index);
	return true;
}

static void eswap_fault_stat(struct zram *zram, u32 index)
{
	struct mem_cgroup *mcg = NULL;
	struct eswap_stat *stat = eswap_get_stat_obj();
	struct oem_mem_cgroup *oem_memcg = NULL;

	if (unlikely(!stat))
		return;

	atomic64_inc(&stat->eswap_fault_cnt);

	mcg = eswap_zram_get_memcg(zram, index);
	if (mcg) {
		oem_memcg = get_oem_mem_cgroup(mcg);
		if (oem_memcg) {
			atomic64_inc(&oem_memcg->eswap_faultcnt);
		}
	}
}

static int eswap_fault_out_get_extent(struct zram *zram,
						struct schedule_para *sched,
						unsigned long zentry,
						u32 index)
{
	int i = 0;
	sched->io_buf.zram = zram;
	sched->priv.zram = zram;
	sched->io_buf.pool = NULL;
	sched->io_entry->ext_id = eswap_find_extent_by_idx(zentry,
			&sched->io_buf, &sched->io_entry->manager_private);
	if (unlikely(sched->io_entry->ext_id == -EBUSY)) {
		/* The extent maybe in unexpected case, wait here */
		for (i = 0; i < GET_EXTENT_MAX_TIMES; i++) {
			/* The extent doesn't exist in eswap */
			zram_slot_lock(zram, index);
			if (!zram_test_flag(zram, index, ZRAM_WB)) {
				zram_slot_unlock(zram, index);
				eswap_free(sched->io_entry);
				return -EAGAIN;
			}
			zram_slot_unlock(zram, index);

			/* Get extent again */
			sched->io_entry->ext_id =
			eswap_find_extent_by_idx(zentry,
				&sched->io_buf,
				&sched->io_entry->manager_private);
			if (likely(sched->io_entry->ext_id != -EBUSY))
				break;
			udelay(50);
		}
	}

	if (i >= 1000) {
		eswap_print(LEVEL_ERR, "get extent ext_id = %d index = %d num = %d\n", esentry_extid(zentry), index, i);
	}

	if (sched->io_entry->ext_id < 0) {
		return sched->io_entry->ext_id;
	}
	eswap_fault_stat(zram, index);
	eswap_fill_entry(sched->io_entry, &sched->io_buf,
					(void *)(&sched->priv));
	return 0;
}

static int eswap_fault_out_exit_check(struct zram *zram,
						u32 index, int ret)
{
	zram_slot_lock(zram, index);
	if (likely(!ret)) {
		if (unlikely(zram_test_flag(zram, index, ZRAM_WB))) {
			eswap_print(LEVEL_WARN, "still in WB status\n");
			ret = -EIO;
		}
	}
	zram_clear_flag(zram, index, ZRAM_UNDER_FAULTOUT);
	return ret;
}

static int eswap_fault_out_extent(struct zram *zram, u32 index,
	struct schedule_para *sched, unsigned long zentry)
{
	int ret;

	sched->io_entry = eswap_malloc(sizeof(struct eswap_entry),
						true, true);
	if (unlikely(!sched->io_entry)) {
		eswap_print(LEVEL_ERR, "alloc io entry failed\n");
		return -ENOMEM;
	}

	ret = eswap_fault_out_get_extent(zram, sched, zentry, index);
	if (ret)
		return ret;

	ret = eswap_read_extent(sched->io_handler, sched->io_entry);
	if (unlikely(ret)) {
		eswap_print(LEVEL_ERR, "eswap read failed, %d\n", ret);
	}

	return ret;
}

/*
 * This interface will be called when ZRAM is read
 * Expandmem should be searched before ZRAM is read
 * This function require ZRAM slot lock being held
 *
 */
static int eswap_fault_out(struct zram *zram, u32 index)
{
	/*
	 * 1. Find the extent in UFS by the index
	 * 2. If extent exist, dispatch it to ZRAM
	 *
	 */
	int ret = 0;
	int io_err;
	struct schedule_para sched;
	unsigned long zentry;

	if (!eswap_fault_out_check(zram, index, &zentry))
		return ret;

	sched.io_handler = eswap_init_plug(zram,
				ESWAP_FAULT_OUT, &sched);
	if (unlikely(!sched.io_handler)) {
		eswap_print(LEVEL_ERR, "plug start failed\n");
		ret = -EIO;
		goto out;
	}

	io_err = eswap_fault_out_extent(zram, index, &sched, zentry);
	ret = eswap_plug_finish(sched.io_handler);
	if (unlikely(ret)) {
		eswap_print(LEVEL_ERR, "eswap flush failed, %d\n", ret);
	} else {
		ret = (io_err != -EAGAIN) ? io_err : 0;
	}
out:
	ret = eswap_fault_out_exit_check(zram, index, ret);
	return ret;
}

/*
 * This interface will be called when ZRAM is freed
 * Expandmem should be deleted before ZRAM is freed
 * If obj can be deleted from ZRAM, return true, otherwise return false
 *
 */
static bool eswap_delete(struct zram *zram, u32 index)
{
	/*
	 * 1. Find the extent in ZRAM by the index
	 * 2. Delete the zs Object in the extent
	 * 3. Free the extent
	 *
	 */

	if (!eswap_get_enable())
		return true;

	if (zram_test_flag(zram, index, ZRAM_UNDER_WB)
		|| zram_test_flag(zram, index, ZRAM_UNDER_FAULTOUT)) {
		return false;
	}

	if (!zram_test_flag(zram, index, ZRAM_WB))
		return true;

	eswap_extent_objs_del(zram, index);

	return true;
}

void eswap_mem_cgroup_remove(struct mem_cgroup *memcg)
{
	if (!eswap_get_enable())
		return;
	eswap_manager_memcg_deinit(memcg);
}

static void eswap_vh_zram_free_page(struct zram *zram, size_t index)
{
	eswap_untrack(zram, index);
}

static void eswap_vh__zram_bvec_read(struct bio *bio, struct zram *zram, size_t index, int *ret)
{
	*ret = 0;
	if (likely(!bio)) {
		*ret = eswap_fault_out(zram, index);
		if (unlikely(*ret)) {
			eswap_print(LEVEL_ERR, "search in eswap failed! err=%d, page=%u\n",
				*ret, index);
			zram_slot_unlock(zram, index);
			return;
		}
	}
}

static void eswap_vh__zram_bvec_write(struct zram *zram, size_t index, struct mem_cgroup *memcg)
{
	eswap_track(zram, index, memcg);
}

static void eswap_vh_zram_slot_free_notify(struct zram *zram, size_t index, int *ret)
{
	if (!eswap_delete(zram, index)) {
		zram_slot_unlock(zram, index);
		atomic64_inc(&zram->stats.miss_free);
		*ret = 1;
		return;
	}
	*ret = 0;
}

static void eswap_vh_disksize_store(struct zram *zram)
{
	eswap_init(zram);
}

extern void (*vh_zram_free_page)(struct zram *zram, size_t index);
extern void (*vh__zram_bvec_read)(struct bio *bio, struct zram *zram, size_t index, int *ret);
extern void (*vh__zram_bvec_write)(struct zram *zram, size_t index, struct mem_cgroup *memcg);
extern void (*vh_zram_slot_free_notify)(struct zram *zram, size_t index, int *ret);
extern void (*vh_disksize_store)(struct zram *zram);
void eswap_zram_vendor_hooks_init(void)
{
	vh_zram_free_page = eswap_vh_zram_free_page;
	vh__zram_bvec_read = eswap_vh__zram_bvec_read;
	vh__zram_bvec_write = eswap_vh__zram_bvec_write;
	vh_zram_slot_free_notify = eswap_vh_zram_slot_free_notify;
	vh_disksize_store = eswap_vh_disksize_store;
}

void eswap_zram_vendor_hooks_remove(void)
{
	vh_zram_free_page = NULL;
	vh__zram_bvec_read = NULL;
	vh__zram_bvec_write = NULL;
	vh_zram_slot_free_notify = NULL;
	vh_disksize_store = NULL;
}

