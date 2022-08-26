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
#include <linux/bit_spinlock.h>

#include "../zram_drv.h"
#include "eswap_list.h"
#include "eswap_area.h"
#include "eswap_lru.h"
#include "eswap_common.h"
#include "eswap.h"
#include "expandmem.h"

#define esentry_pgid(e) (((e) & ((1 << EXTENT_SHIFT) - 1)) >> PAGE_SHIFT)
#define esentry_pgoff(e) ((e) & (PAGE_SIZE - 1))

struct io_extent {
	int ext_id;
	struct zram *zram;
	struct mem_cgroup *mcg;
	struct page *pages[EXTENT_PG_CNT];
	u32 index[EXTENT_MAX_OBJ_CNT];
	int cnt;
	struct eswap_page_pool *pool;
};

static struct io_extent *alloc_io_extent(struct eswap_page_pool *pool,
				  bool fast, bool nofail)
{
	int i;
	struct io_extent *io_ext = eswap_malloc(sizeof(struct io_extent),
						     fast, nofail);

	if (!io_ext) {
		eswap_print(LEVEL_WARN, "alloc io_ext failed\n");
		return NULL;
	}

	io_ext->ext_id = -EINVAL;
	io_ext->pool = pool;
	for (i = 0; i < (int)EXTENT_PG_CNT; i++) {
		io_ext->pages[i] = eswap_alloc_page(pool, GFP_ATOMIC,
							fast, nofail);
		if (!io_ext->pages[i]) {
			eswap_print(LEVEL_WARN, "alloc page[%d] failed\n", i);
			goto page_free;
		}
	}
	return io_ext;
page_free:
	for (i = 0; i < (int)EXTENT_PG_CNT; i++)
		if (io_ext->pages[i])
			eswap_page_recycle(io_ext->pages[i], pool);
	eswap_free(io_ext);

	return NULL;
}

/*
 * The function will be called when eswap execute fault out.
 */
int eswap_find_extent_by_idx(unsigned long eswapentry,
				 struct eswap_buffer *buf,
				 void **private)
{
	int ext_id;
	struct io_extent *io_ext = NULL;
	struct zram *zram = NULL;
	struct oem_zram_data *oem_zram = NULL;

	if (!buf) {
		eswap_print(LEVEL_WARN, "buf is null\n");
		return -EINVAL;
	}
	if (!private) {
		eswap_print(LEVEL_WARN, "private is null\n");
		return -EINVAL;
	}

	zram = buf->zram;
	oem_zram = get_oem_zram(zram);
	ext_id = get_extent(oem_zram->area, esentry_extid(eswapentry));
	if (ext_id < 0)
		return ext_id;
	io_ext = alloc_io_extent(buf->pool, true, true);
	if (!io_ext) {
		eswap_print(LEVEL_WARN, "io_ext alloc failed\n");
		put_extent(oem_zram->area, ext_id);
		return -ENOMEM;
	}

	io_ext->ext_id = ext_id;
	io_ext->zram = zram;
	io_ext->mcg = get_mem_cgroup(
				eswap_list_get_mcgid(ext_idx(oem_zram->area, ext_id),
						  oem_zram->area->ext_table));
	if (io_ext->mcg)
		css_get(&io_ext->mcg->css);
	buf->dest_pages = io_ext->pages;
	(*private) = io_ext;
	eswap_print(LEVEL_DEBUG, "get entry = %lx ext = %d\n", eswapentry, ext_id);

	return ext_id;
}

static void discard_io_extent(struct io_extent *io_ext, unsigned int op)
{
	struct zram *zram = NULL;
	struct oem_mem_cgroup *oem_memcg = NULL;
	int i;
	struct oem_zram_data *oem_zram = NULL;

	if (!io_ext) {
		eswap_print(LEVEL_WARN, "io_ext is null\n");
		return;
	}
	if (!io_ext->mcg) {
		zram = io_ext->zram;
	} else {
		oem_memcg = get_oem_mem_cgroup(io_ext->mcg);
		if (oem_memcg) {
			zram = oem_memcg->zram;
		}
	}

	oem_zram = get_oem_zram(zram);
	if (!oem_zram) {
		eswap_print(LEVEL_WARN, "zram data is null\n");
		return;
	}

	for (i = 0; i < (int)EXTENT_PG_CNT; i++)
		if (io_ext->pages[i])
			eswap_page_recycle(io_ext->pages[i], io_ext->pool);
	if (io_ext->ext_id < 0)
		goto out;
	eswap_print(LEVEL_DEBUG, "ext = %d, op = %d\n", io_ext->ext_id, op);
	if (op == REQ_OP_READ) {
		put_extent(oem_zram->area, io_ext->ext_id);
		goto out;
	}
	for (i = 0; i < io_ext->cnt; i++) {
		u32 index = io_ext->index[i];

		zram_slot_lock(zram, index);
		if (io_ext->mcg)
			zram_lru_add_tail(zram, index, io_ext->mcg);
		zram_clear_flag(zram, index, ZRAM_UNDER_WB);
		zram_slot_unlock(zram, index);
	}
	eswap_free_extent(oem_zram->area, io_ext->ext_id);
out:
	eswap_free(io_ext);
}

/*
 * The function will be called
 * when schedule meets exception proceeding extent
 */
void eswap_extent_exception(enum eswap_scenario scenario,
			       void *private)
{
	struct io_extent *io_ext = private;
	struct mem_cgroup *mcg = NULL;
	unsigned int op = (scenario == ESWAP_RECLAIM_IN) ?
			  REQ_OP_WRITE : REQ_OP_READ;

	if (!io_ext) {
		eswap_print(LEVEL_WARN, "io_ext is null\n");
		return;
	}

	eswap_print(LEVEL_DEBUG, "ext_id = %d, op = %d\n", io_ext->ext_id, op);
	mcg = io_ext->mcg;
	discard_io_extent(io_ext, op);
	if (mcg)
		css_put(&mcg->css);
}

static void copy_from_pages(u8 *dst, struct page *pages[],
		     unsigned long eswapentry, int size)
{
	u8 *src = NULL;
	int pg_id = esentry_pgid(eswapentry);
	int offset = esentry_pgoff(eswapentry);

	if (!dst) {
		eswap_print(LEVEL_WARN, "dst is null\n");
		return;
	}
	if (!pages) {
		eswap_print(LEVEL_WARN, "pages is null\n");
		return;
	}
	if (size < 0 || size > (int)PAGE_SIZE) {
		eswap_print(LEVEL_WARN, "size = %d invalid\n", size);
		return;
	}

	src = page_to_virt(pages[pg_id]);
	if (offset + size <= (int)PAGE_SIZE) {
		memcpy(dst, src + offset, size);
		return;
	}
	if (pg_id == EXTENT_PG_CNT - 1) {
		eswap_print(LEVEL_WARN, "ext overflow, addr = %lx, size = %d\n",
			 eswapentry, size);
		return;
	}
	memcpy(dst, src + offset, PAGE_SIZE - offset);
	src = page_to_virt(pages[pg_id + 1]);
	memcpy(dst + PAGE_SIZE - offset, src, offset + size - PAGE_SIZE);
}

static void copy_to_pages(u8 *src, struct page *pages[],
		   unsigned long eswapentry, int size)
{
	u8 *dst = NULL;
	int pg_id = esentry_pgid(eswapentry);
	int offset = esentry_pgoff(eswapentry);

	if (!src) {
		eswap_print(LEVEL_WARN, "src is null\n");
		return;
	}
	if (!pages) {
		eswap_print(LEVEL_WARN, "pages is null\n");
		return;
	}
	if (size < 0 || size > (int)PAGE_SIZE) {
		eswap_print(LEVEL_WARN, "size = %d invalid\n", size);
		return;
	}
	dst = page_to_virt(pages[pg_id]);
	if (offset + size <= (int)PAGE_SIZE) {
		memcpy(dst + offset, src, size);
		return;
	}
	if (pg_id == EXTENT_PG_CNT - 1) {
		eswap_print(LEVEL_WARN, "ext overflow, addr = %lx, size = %d\n",
			 eswapentry, size);
		return;
	}
	memcpy(dst + offset, src, PAGE_SIZE - offset);
	dst = page_to_virt(pages[pg_id + 1]);
	memcpy(dst, src + PAGE_SIZE - offset, offset + size - PAGE_SIZE);
}

static bool zram_test_skip(struct zram *zram, u32 index, struct mem_cgroup *mcg)
{
	if (zram_test_flag(zram, index, ZRAM_WB))
		return true;
	if (zram_test_flag(zram, index, ZRAM_UNDER_WB))
		return true;
	if (zram_test_flag(zram, index, ZRAM_UNDER_FAULTOUT))
		return true;
	if (zram_test_flag(zram, index, ZRAM_SAME))
		return true;
	if (mcg != zram_get_memcg(zram, index))
		return true;
	if (!zram_get_obj_size(zram, index))
		return true;

	return false;
}

bool zram_test_overwrite(struct zram *zram, u32 index, int ext_id)
{
	int ext_id_new;
	if (!zram_test_flag(zram, index, ZRAM_WB)) {
		eswap_print(LEVEL_DEBUG, "overwrite not ZRAM_WB, index = %d ext_id = %d\n", index, ext_id);
		return true;
	}

	ext_id_new = esentry_extid(zram_get_handle(zram, index));
	if (ext_id != ext_id_new) {
		eswap_print(LEVEL_DEBUG, "overwrite ext_id change, index = %d ext_id = %d new ext_id = %d\n",
			index, ext_id, ext_id_new);
		return true;
	}

	return false;
}

static void move_to_eswap(struct zram *zram, u32 index,
		       unsigned long eswapentry, struct mem_cgroup *mcg)
{
	int size;
	struct eswap_stat *stat = eswap_get_stat_obj();
	struct oem_mem_cgroup *oem_memcg = get_oem_mem_cgroup(mcg);
	struct oem_zram_data *oem_zram = get_oem_zram(zram);

	if (!stat) {
		eswap_print(LEVEL_WARN, "stat is null\n");
		return;
	}
	if (!zram || !oem_zram) {
		eswap_print(LEVEL_WARN, "zram is null\n");
		return;
	}
	if (index >= (u32)oem_zram->area->nr_objs) {
		eswap_print(LEVEL_WARN, "index = %d invalid\n", index);
		return;
	}
	if (!mcg || !oem_memcg) {
		eswap_print(LEVEL_WARN, "mcg is null\n");
		return;
	}

	size = zram_get_obj_size(zram, index);

	zram_clear_flag(zram, index, ZRAM_UNDER_WB);

	zs_free(zram->mem_pool, zram_get_handle(zram, index));
	atomic64_sub(size, &zram->stats.compr_data_size);
	atomic64_dec(&zram->stats.pages_stored);

	zram_set_memcg(zram, index, mcg);
	zram_set_flag(zram, index, ZRAM_WB);
	zram_set_obj_size(zram, index, size);
	if (size == PAGE_SIZE)
		zram_set_flag(zram, index, ZRAM_HUGE);
	zram_set_handle(zram, index, eswapentry);
	zram_rmap_insert(zram, index);

	atomic64_add(size, &stat->stored_size);
	atomic64_add(size, &oem_memcg->eswap_stored_size);
	atomic64_inc(&stat->stored_pages);
	atomic_inc(&oem_zram->area->ext_stored_pages[esentry_extid(eswapentry)]);
	atomic64_inc(&oem_memcg->eswap_stored_pages);
}

static void __move_to_zram(struct zram *zram, u32 index, unsigned long handle,
			struct io_extent *io_ext)
{
	struct eswap_stat *stat = eswap_get_stat_obj();
	struct mem_cgroup *mcg = io_ext->mcg;
	int size = zram_get_obj_size(zram, index);
	struct oem_mem_cgroup *oem_memcg = get_oem_mem_cgroup(mcg);
	struct oem_zram_data *oem_zram = get_oem_zram(zram);

	if (!stat) {
		eswap_print(LEVEL_ERR, "stat is null\n");
		return;
	}

	zram_slot_lock(zram, index);
	if (zram_test_overwrite(zram, index, io_ext->ext_id)) {
		eswap_print(LEVEL_WARN, "overwrite, index = %d ext_id = %d\n", index, io_ext->ext_id);
		zram_slot_unlock(zram, index);
		zs_free(zram->mem_pool, handle);
		return;
	}
	zram_rmap_erase(zram, index);
	zram_set_handle(zram, index, handle);
	zram_clear_flag(zram, index, ZRAM_WB);
	if (mcg)
		zram_lru_add_tail(zram, index, mcg);
	zram_set_flag(zram, index, ZRAM_FROM_ESWAP);
	zram_slot_unlock(zram, index);

	atomic64_inc(&stat->faultout_pages);
	atomic64_sub(size, &stat->stored_size);
	atomic64_dec(&stat->stored_pages);
	atomic_dec(&oem_zram->area->ext_stored_pages[io_ext->ext_id]);
	if (oem_memcg) {
		atomic64_sub(size, &oem_memcg->eswap_stored_size);
		atomic64_dec(&oem_memcg->eswap_stored_pages);
	}
}

static int move_to_zram(struct zram *zram, u32 index, struct io_extent *io_ext)
{
	unsigned long handle, eswapentry;
	struct mem_cgroup *mcg = NULL;
	int size, i;
	u8 *dst = NULL;
	struct oem_zram_data *oem_zram = get_oem_zram(zram);

	if (!zram || !oem_zram) {
		eswap_print(LEVEL_ERR, "zram is null\n");
		return -EINVAL;
	}
	if (index >= (u32)oem_zram->area->nr_objs) {
		eswap_print(LEVEL_ERR, "index = %d invalid\n", index);
		return -EINVAL;
	}
	if (!io_ext) {
		eswap_print(LEVEL_ERR, "io_ext is null\n");
		return -EINVAL;
	}

	mcg = io_ext->mcg;
	zram_slot_lock(zram, index);
	eswapentry = zram_get_handle(zram, index);
	if (zram_test_overwrite(zram, index, io_ext->ext_id)) {
		eswap_print(LEVEL_WARN, "overwrite, index = %d ext_id = %d\n", index, io_ext->ext_id);
		zram_slot_unlock(zram, index);
		return 0;
	}
	size = zram_get_obj_size(zram, index);
	zram_slot_unlock(zram, index);

	for (i = esentry_pgid(eswapentry) - 1; i >= 0 && io_ext->pages[i]; i--) {
		eswap_page_recycle(io_ext->pages[i], io_ext->pool);
		io_ext->pages[i] = NULL;
	}
	handle = zs_malloc(zram->mem_pool, size, __GFP_DIRECT_RECLAIM | __GFP_KSWAPD_RECLAIM |
		__GFP_NOWARN | __GFP_HIGHMEM |	__GFP_MOVABLE);
	if (!handle) {
		eswap_print(LEVEL_ERR, "alloc handle failed, size = %d\n", size);
		return -ENOMEM;
	}
	dst = zs_map_object(zram->mem_pool, handle, ZS_MM_WO);
	copy_from_pages(dst, io_ext->pages, eswapentry, size);
	zs_unmap_object(zram->mem_pool, handle);
	__move_to_zram(zram, index, handle, io_ext);

	return 0;
}

static void extent_unlock(struct io_extent *io_ext)
{
	int ext_id;
	struct mem_cgroup *mcg = NULL;
	struct zram *zram = NULL;
	int k;
	unsigned long eswapentry;
	struct oem_mem_cgroup *oem_memcg = NULL;
	struct oem_zram_data *oem_zram = NULL;

	if (!io_ext) {
		eswap_print(LEVEL_WARN, "io_ext is null\n");
		goto out;
	}

	mcg = io_ext->mcg;
	if (!mcg) {
		eswap_print(LEVEL_WARN, "mcg is null\n");
		goto out;
	}
	oem_memcg = get_oem_mem_cgroup(mcg);
	if (!oem_memcg) {
		eswap_print(LEVEL_WARN, "mcg data is null\n");
		goto out;
	}

	zram = oem_memcg->zram;
	if (!zram) {
		eswap_print(LEVEL_WARN, "zram is null\n");
		goto out;
	}
	oem_zram = get_oem_zram(zram);
	if (!oem_zram) {
		eswap_print(LEVEL_WARN, "zram data is null\n");
		goto out;
	}
	ext_id = io_ext->ext_id;
	if (ext_id < 0)
		goto out;

	eswap_print(LEVEL_DEBUG, "add ext_id = %d, cnt = %d\n",
			ext_id, io_ext->cnt);
	eswapentry = ((unsigned long)ext_id) << EXTENT_SHIFT;

	for (k = 0; k < io_ext->cnt; k++) {
		zram_slot_lock(zram, io_ext->index[k]);
		move_to_eswap(zram, io_ext->index[k], eswapentry, mcg);
		eswapentry += zram_get_obj_size(zram, io_ext->index[k]);
		zram_slot_unlock(zram, io_ext->index[k]);
	}
	put_extent(oem_zram->area, ext_id);
	io_ext->ext_id = -EINVAL;

	eswap_print(LEVEL_DEBUG, "add extent OK\n");
out:
	discard_io_extent(io_ext, REQ_OP_WRITE);
	if (mcg)
		css_put(&mcg->css);
}

static void extent_add(struct io_extent *io_ext,
		       enum eswap_scenario scenario)
{
	struct mem_cgroup *mcg = NULL;
	struct zram *zram = NULL;
	int ext_id;
	int k;
	struct oem_mem_cgroup *oem_memcg = NULL;
	struct oem_zram_data *oem_zram = NULL;

	if (!io_ext) {
		eswap_print(LEVEL_WARN, "io_ext is null\n");
		return;
	}

	mcg = io_ext->mcg;
	if (!mcg)
		zram = io_ext->zram;
	else {
		oem_memcg = get_oem_mem_cgroup(mcg);
		if (oem_memcg) {
			zram = oem_memcg->zram;
		}
	}
	if (!zram) {
		eswap_print(LEVEL_WARN, "zram is null\n");
		goto out;
	}
	oem_zram = get_oem_zram(zram);
	if (!oem_zram) {
		eswap_print(LEVEL_WARN, "zram dtat is null\n");
		goto out;
	}
	ext_id = io_ext->ext_id;
	if (ext_id < 0)
		goto out;

	io_ext->cnt = zram_rmap_get_extent_index(zram,
						 ext_id,
						 io_ext->index);
	eswap_print(LEVEL_DEBUG, "ext_id = %d, cnt = %d\n", ext_id, io_ext->cnt);
	for (k = 0; k < io_ext->cnt; k++) {
		int ret = move_to_zram(zram, io_ext->index[k], io_ext);
		if (ret)
			goto out;
	}
	eswap_print(LEVEL_DEBUG, "extent add OK, free ext_id = %d\n", ext_id);
	eswap_free_extent(oem_zram->area, io_ext->ext_id);
	io_ext->ext_id = -EINVAL;
out:
	discard_io_extent(io_ext, REQ_OP_READ);
	if (mcg)
		css_put(&mcg->css);
}


static void extent_clear(struct zram *zram, int ext_id)
{
	int *index = NULL;
	int cnt;
	int k;
	struct eswap_stat *stat = eswap_get_stat_obj();

	if (!stat) {
		eswap_print(LEVEL_WARN, "stat is null\n");
		return;
	}

	index = kzalloc(sizeof(int) * EXTENT_MAX_OBJ_CNT, GFP_NOIO);
	if (!index)
		index = kzalloc(sizeof(int) * EXTENT_MAX_OBJ_CNT,
				GFP_NOIO | __GFP_NOFAIL);

	eswap_print(LEVEL_WARN, "ext_id = %d\n", ext_id);
	cnt = zram_rmap_get_extent_index(zram, ext_id, index);

	for (k = 0; k < cnt; k++) {
		zram_slot_lock(zram, index[k]);
		if (zram_test_overwrite(zram, index[k], ext_id)) {
			zram_slot_unlock(zram, index[k]);
			continue;
		}
		zram_set_memcg(zram, index[k], NULL);
		zram_set_flag(zram, index[k], ZRAM_MCGID_CLEAR);
		zram_slot_unlock(zram, index[k]);
	}

	kfree(index);
}

static int shrink_entry(struct zram *zram, u32 index, struct io_extent *io_ext,
		 unsigned long ext_off)
{
	unsigned long handle;
	int size;
	u8 *src = NULL;
	struct eswap_stat *stat = eswap_get_stat_obj();
	struct oem_zram_data *oem_zram = get_oem_zram(zram);

	if (!stat) {
		eswap_print(LEVEL_WARN, "stat is null\n");
		return -EINVAL;
	}
	if (!zram || !oem_zram) {
		eswap_print(LEVEL_WARN, "zram is null\n");
		return -EINVAL;
	}
	if (index >= (u32)oem_zram->area->nr_objs) {
		eswap_print(LEVEL_WARN, "index = %d invalid\n", index);
		return -EINVAL;
	}

	zram_slot_lock(zram, index);
	handle = zram_get_handle(zram, index);
	if (!handle || zram_test_skip(zram, index, io_ext->mcg)) {
		zram_slot_unlock(zram, index);
		return 0;
	}
	size = zram_get_obj_size(zram, index);
	if (ext_off + size > EXTENT_SIZE) {
		zram_slot_unlock(zram, index);
		return -ENOSPC;
	}

	src = zs_map_object(zram->mem_pool, handle, ZS_MM_RO);
	copy_to_pages(src, io_ext->pages, ext_off, size);
	zs_unmap_object(zram->mem_pool, handle);
	io_ext->index[io_ext->cnt++] = index;
	zram_lru_del(zram, index);
	zram_set_flag(zram, index, ZRAM_UNDER_WB);
	if (zram_test_flag(zram, index, ZRAM_FROM_ESWAP)) {
		atomic64_inc(&stat->reout_pages);
		atomic64_add(size, &stat->reout_bytes);
	}
	zram_slot_unlock(zram, index);
	atomic64_inc(&stat->reclaimin_pages);

	return size;
}

static int shrink_entry_list(struct io_extent *io_ext)
{
	struct mem_cgroup *mcg = NULL;
	struct zram *zram = NULL;
	unsigned long stored_size;
	int *swap_index = NULL;
	int swap_cnt, k;
	int swap_size = 0;
	struct oem_mem_cgroup *oem_memcg = NULL;
	struct oem_zram_data *oem_zram = NULL;

	if (!io_ext) {
		eswap_print(LEVEL_WARN, "io_ext is null\n");
		return -EINVAL;
	}

	mcg = io_ext->mcg;
	oem_memcg = get_oem_mem_cgroup(mcg);
	if (!oem_memcg) {
		eswap_print(LEVEL_WARN, "memcg data is null\n");
		return -EINVAL;
	}
	zram = oem_memcg->zram;
	oem_zram = get_oem_zram(zram);
	if (!oem_zram) {
		eswap_print(LEVEL_WARN, "zram data is null\n");
		return -EINVAL;
	}

	stored_size = atomic64_read(&oem_memcg->zram_stored_size);
	if (stored_size < EXTENT_SIZE)
		return -ENOENT;

	swap_index = kzalloc(sizeof(int) * EXTENT_MAX_OBJ_CNT, GFP_NOIO);
	if (!swap_index)
		return -ENOMEM;
	io_ext->ext_id = eswap_alloc_extent(oem_zram->area, mcg);
	if (io_ext->ext_id < 0) {
		kfree(swap_index);
		return io_ext->ext_id;
	}
	swap_cnt = zram_get_memcg_coldest_index(oem_zram->area, mcg, swap_index,
						EXTENT_MAX_OBJ_CNT);
	io_ext->cnt = 0;
	for (k = 0; k < swap_cnt && swap_size < (int)EXTENT_SIZE; k++) {
		int size = shrink_entry(zram, swap_index[k], io_ext, swap_size);

		if (size < 0)
			break;
		swap_size += size;
	}
	kfree(swap_index);
	eswap_print(LEVEL_DEBUG, "fill extent = %d, cnt = %d, overhead = %ld\n",
		 io_ext->ext_id, io_ext->cnt, EXTENT_SIZE - swap_size);
	if (swap_size == 0) {
		eswap_print(LEVEL_DEBUG, "swap_size = 0, free ext_id = %d\n",
				io_ext->ext_id);
		eswap_free_extent(oem_zram->area, io_ext->ext_id);
		io_ext->ext_id = -EINVAL;
		return -ENOENT;
	}

	return swap_size;
}

/*
 * The function will be called when eswap read done.
 * The function should extra the extent to ZRAM, then destroy
 */
void eswap_extent_destroy(void *private, enum eswap_scenario scenario)
{
	struct io_extent *io_ext = private;

	if (!io_ext) {
		eswap_print(LEVEL_WARN, "io_ext is null\n");
		return;
	}

	eswap_print(LEVEL_DEBUG, "ext_id = %d\n", io_ext->ext_id);
	extent_add(io_ext, scenario);
}

/*
 * The function is used to alloc a new extent,
 * then reclaim ZRAM by LRU list to the new extent
 */
unsigned long eswap_extent_create(struct mem_cgroup *mcg,
				      int *ext_id,
				      struct eswap_buffer *buf,
				      void **private)
{
	struct io_extent *io_ext = NULL;
	int reclaim_size;

	if (!mcg) {
		eswap_print(LEVEL_WARN, "mcg is null\n");
		return 0;
	}
	if (!ext_id) {
		eswap_print(LEVEL_WARN, "ext_id is null\n");
		return 0;
	}
	(*ext_id) = -EINVAL;
	if (!buf) {
		eswap_print(LEVEL_WARN, "buf is null\n");
		return 0;
	}
	if (!private) {
		eswap_print(LEVEL_WARN, "private is null\n");
		return 0;
	}

	io_ext = alloc_io_extent(buf->pool, false, true);
	if (!io_ext)
		return 0;
	io_ext->mcg = mcg;
	reclaim_size = shrink_entry_list(io_ext);
	if (reclaim_size < 0) {
		discard_io_extent(io_ext, REQ_OP_WRITE);
		(*ext_id) = reclaim_size;
		return 0;
	}
	css_get(&mcg->css);
	(*ext_id) = io_ext->ext_id;
	buf->dest_pages = io_ext->pages;
	(*private) = io_ext;
	eswap_print(LEVEL_DEBUG, "mcg = %d, ext_id = %d\n",
		 mcg->id.id, io_ext->ext_id);

	return reclaim_size;
}

/*
 * The function will be called when eswap write done.
 * The function should register extent to eswap manager.
 */
void eswap_extent_register(void *private)
{
	struct io_extent *io_ext = private;

	if (!io_ext) {
		eswap_print(LEVEL_WARN, "io_ext is null\n");
		return;
	}
	eswap_print(LEVEL_DEBUG, "ext_id = %d\n", io_ext->ext_id);
	extent_unlock(io_ext);
}

void eswap_extent_objs_del(struct zram *zram, u32 index)
{
	int ext_id;
	struct mem_cgroup *mcg = NULL;
	unsigned long eswapentry;
	int size;
	struct eswap_stat *stat = eswap_get_stat_obj();
	struct oem_mem_cgroup *oem_memcg = NULL;
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
	if (!zram_test_flag(zram, index, ZRAM_WB)) {
		eswap_print(LEVEL_WARN, "not WB object\n");
		return;
	}

	eswapentry = zram_get_handle(zram, index);
	size = zram_get_obj_size(zram, index);
	atomic64_sub(size, &stat->stored_size);
	atomic64_dec(&stat->stored_pages);
	mcg = zram_get_memcg(zram, index);
	oem_memcg = get_oem_mem_cgroup(mcg);
	if (oem_memcg) {
		atomic64_sub(size, &oem_memcg->eswap_stored_size);
		atomic64_dec(&oem_memcg->eswap_stored_pages);
	}
	if (!atomic_dec_and_test(
		&oem_zram->area->ext_stored_pages[esentry_extid(eswapentry)]))
		return;
	ext_id = get_extent(oem_zram->area, esentry_extid(eswapentry));
	if (ext_id < 0)
		return;

	atomic64_inc(&stat->notify_free);
	eswap_print(LEVEL_DEBUG, "free ext_id = %d\n", ext_id);
	eswap_free_extent(oem_zram->area, ext_id);
}

/*
 * The function is used to initialize private member in memcg
 */
void eswap_manager_memcg_init(struct mem_cgroup *mcg, struct zram *zram)
{
	struct oem_mem_cgroup *oem_memcg = NULL;
	struct oem_zram_data *oem_zram = get_oem_zram(zram);

	if (!mcg || !zram || !oem_zram || !oem_zram->area) {
		eswap_print(LEVEL_WARN, "invalid mcg\n");
		return;
	}

	oem_memcg = get_oem_mem_cgroup(mcg);
	if (unlikely(!oem_memcg)) {
		eswap_print(LEVEL_ERR, "invalid mcg data\n");
		return;
	}

	eswap_list_init(mcg_idx(oem_zram->area, mcg->id.id), oem_zram->area->obj_table);
	eswap_list_init(mcg_idx(oem_zram->area, mcg->id.id), oem_zram->area->ext_table);

	atomic64_set(&oem_memcg->zram_stored_size, 0);
	atomic64_set(&oem_memcg->zram_page_size, 0);
	atomic64_set(&oem_memcg->eswap_stored_pages, 0);
	atomic64_set(&oem_memcg->eswap_stored_size, 0);
	atomic64_set(&oem_memcg->eswap_allfaultcnt, 0);

	atomic64_set(&oem_memcg->eswap_outcnt, 0);
	atomic64_set(&oem_memcg->eswap_faultcnt, 0);
	atomic64_set(&oem_memcg->eswap_outextcnt, 0);

	smp_wmb();
	oem_memcg->zram = zram;
}

void eswap_manager_memcg_deinit(struct mem_cgroup *mcg)
{
	struct zram *zram = NULL;
	struct eswap_area *area = NULL;
	struct eswap_stat *stat = eswap_get_stat_obj();
	int last_index = -1;
	struct oem_mem_cgroup *oem_memcg = get_oem_mem_cgroup(mcg);
	struct oem_zram_data *oem_zram = NULL;

	if (!stat) {
		eswap_print(LEVEL_WARN, "stat is null\n");
		return;
	}

	if (!mcg || !oem_memcg || !oem_memcg->zram) {
		eswap_print(LEVEL_DEBUG, "invalid mcg\n");
		return;
	}
	zram = oem_memcg->zram;
	oem_zram = get_oem_zram(zram);
	if (!oem_zram || !oem_zram->area) {
		eswap_print(LEVEL_DEBUG, "invalid zram data\n");
		return;
	}

	eswap_print(LEVEL_ERR, "deinit mcg %d\n", mcg->id.id);
	area = oem_zram->area;
	while (1) {
		int index = get_memcg_zram_entry(area, mcg);

		if (index == -ENOENT)
			break;
		if (index < 0) {
			eswap_print(LEVEL_WARN, "invalid index\n");
			return;
		}

		if (last_index == index) {
			eswap_print(LEVEL_WARN, "dup index %d\n", index);
		}

		zram_slot_lock(zram, index);
		if (index == last_index || mcg == zram_get_memcg(zram, index)) {
			eswap_list_del(obj_idx(area, index),
					mcg_idx(area, mcg->id.id),
					area->obj_table);
			zram_set_memcg(zram, index, NULL);
			zram_set_flag(zram, index, ZRAM_MCGID_CLEAR);
		}
		zram_slot_unlock(zram, index);
		last_index = index;
	}
	eswap_print(LEVEL_DEBUG, "deinit mcg %d, entry done\n", mcg->id.id);
	while (1) {
		int ext_id = get_memcg_extent(area, mcg);

		if (ext_id == -ENOENT)
			break;
		extent_clear(zram, ext_id);
		eswap_list_set_mcgid(ext_idx(area, ext_id), area->ext_table, 0);
		put_extent(area, ext_id);
	}
	eswap_print(LEVEL_DEBUG, "deinit mcg %d, extent done\n", mcg->id.id);
}

void eswap_manager_deinit(struct zram *zram)
{
	struct oem_zram_data *oem_zram = get_oem_zram(zram);
	if (!zram || !oem_zram) {
		eswap_print(LEVEL_WARN, "zram is null\n");
		return;
	}

	free_eswap_area(oem_zram->area);
	oem_zram->area = NULL;
}

/*
 * The function is used to initialize global settings
 */
int eswap_manager_init(struct zram *zram)
{
	int ret;
	struct oem_zram_data *oem_zram = get_oem_zram(zram);

	if (!zram || !oem_zram) {
		eswap_print(LEVEL_WARN, "zram is null\n");
		ret = -EINVAL;
		goto out;
	}

	oem_zram->area = alloc_eswap_area(zram->disksize,
					  oem_zram->nr_pages << PAGE_SHIFT);
	if (!oem_zram->area) {
		ret = -ENOMEM;
		goto out;
	}
	return 0;
out:
	eswap_manager_deinit(zram);

	return ret;
}