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
#include "eswap.h"
#include "eswap_common.h"
#include "eswap_list.h"
#include "eswap_lru.h"
#include "expandmem.h"
#include "eswap_area.h"

void zram_set_memcg(struct zram *zram, u32 index, struct mem_cgroup *mcg)
{
	struct oem_zram_data *oem_zram = get_oem_zram(zram);
	unsigned short mcg_id = mcg ? mcg->id.id : 0;
	eswap_list_set_mcgid(obj_idx(oem_zram->area, index),
				oem_zram->area->obj_table, mcg_id);
}

struct mem_cgroup *zram_get_memcg(struct zram *zram, u32 index)
{
	unsigned short mcg_id;
	struct oem_zram_data *oem_zram = get_oem_zram(zram);
	mcg_id = eswap_list_get_mcgid(obj_idx(oem_zram->area, index),
				oem_zram->area->obj_table);

	return get_mem_cgroup(mcg_id);
}

void zram_lru_del(struct zram *zram, u32 index)
{
	struct mem_cgroup *mcg = NULL;
	unsigned long size;
	struct eswap_stat *stat = eswap_get_stat_obj();
	struct oem_mem_cgroup *oem_memcg = NULL;
	struct oem_zram_data *oem_zram = get_oem_zram(zram);
	struct oem_zram_data *memcg_oem_zram = NULL;

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
	if (zram_test_flag(zram, index, ZRAM_WB)) {
		eswap_print(LEVEL_WARN, "WB object, index = %d\n", index);
		return;
	}

	mcg = zram_get_memcg(zram, index);
	if (!mcg)
		return;

	oem_memcg = get_oem_mem_cgroup(mcg);
	if (!oem_memcg  || !oem_memcg->zram) {
		eswap_print(LEVEL_ERR, "invalid mcg data\n");
		return;
	}
	memcg_oem_zram = get_oem_zram(oem_memcg->zram);
	if (!memcg_oem_zram || !memcg_oem_zram->area) {
		eswap_print(LEVEL_ERR, "invalid mcg zram data\n");
		return;
	}

	if (zram_test_flag(zram, index, ZRAM_SAME))
		return;

	size = zram_get_obj_size(zram, index);
	eswap_list_del(obj_idx(oem_zram->area, index),
			mcg_idx(oem_zram->area, mcg->id.id),
			oem_zram->area->obj_table);
	zram_set_memcg(zram, index, NULL);

	atomic64_sub(size, &oem_memcg->zram_stored_size);
	atomic64_dec(&oem_memcg->zram_page_size);
	atomic64_sub(size, &stat->zram_stored_size);
	atomic64_dec(&stat->zram_stored_pages);
}

void zram_lru_add(struct zram *zram, u32 index, struct mem_cgroup *mcg)
{
	unsigned long size;
	struct eswap_stat *stat = eswap_get_stat_obj();
	struct oem_mem_cgroup *oem_memcg = get_oem_mem_cgroup(mcg);
	struct oem_zram_data *oem_zram = get_oem_zram(zram);
	struct oem_zram_data *memcg_oem_zram;

	if (!stat) {
		eswap_print(LEVEL_WARN, "stat is null\n");
		return;
	}
	if (!zram || !oem_zram) {
		eswap_print(LEVEL_WARN, "zram is null\n");
		return;
	}
	if (!mcg || !oem_memcg || !oem_memcg->zram) {
		eswap_print(LEVEL_WARN, "invalid mcg\n");
		return;
	}
	memcg_oem_zram = get_oem_zram(oem_memcg->zram);
	if (!memcg_oem_zram || !memcg_oem_zram->area) {
		eswap_print(LEVEL_ERR, "invalid mcg zram data\n");
		return;
	}

	if (index >= (u32)oem_zram->area->nr_objs) {
		eswap_print(LEVEL_WARN, "index = %d invalid\n", index);
		return;
	}
	if (zram_test_flag(zram, index, ZRAM_WB)) {
		eswap_print(LEVEL_WARN, "WB object, index = %d\n", index);
		return;
	}
	if (zram_test_flag(zram, index, ZRAM_SAME))
		return;

	zram_set_memcg(zram, index, mcg);
	eswap_list_add(obj_idx(oem_zram->area, index),
			mcg_idx(oem_zram->area, mcg->id.id),
			oem_zram->area->obj_table);
	size = zram_get_obj_size(zram, index);
	atomic64_add(size, &oem_memcg->zram_stored_size);
	atomic64_inc(&oem_memcg->zram_page_size);
	atomic64_add(size, &stat->zram_stored_size);
	atomic64_inc(&stat->zram_stored_pages);
}

/*
 * The function is used to add ZS object to ZRAM LRU list
 */
void eswap_zram_lru_add(struct zram *zram,
			    u32 index,
			    struct mem_cgroup *mcg)
{
	struct oem_zram_data *oem_zram = get_oem_zram(zram);
	if (!zram || !oem_zram) {
		eswap_print(LEVEL_WARN, "zram is null\n");
		return;
	}
	if (index >= (u32)oem_zram->area->nr_objs) {
		eswap_print(LEVEL_WARN, "index = %d invalid\n", index);
		return;
	}
	if (!mcg) {
		eswap_print(LEVEL_WARN, "mcg is null\n");
		return;
	}

	zram_lru_add(zram, index, mcg);
}

void zram_lru_add_tail(struct zram *zram, u32 index, struct mem_cgroup *mcg)
{
	unsigned long size;
	struct eswap_stat *stat = eswap_get_stat_obj();
	struct oem_mem_cgroup *oem_memcg = get_oem_mem_cgroup(mcg);
	struct oem_zram_data *oem_zram = get_oem_zram(zram);
	struct oem_zram_data *memcg_oem_zram;

	if (!stat) {
		eswap_print(LEVEL_WARN, "stat is null\n");
		return;
	}
	if (!zram || !oem_zram) {
		eswap_print(LEVEL_WARN, "zram is null\n");
		return;
	}
	if (!mcg || !oem_memcg || !oem_memcg->zram) {
		eswap_print(LEVEL_WARN, "invalid mcg\n");
		return;
	}
	memcg_oem_zram = get_oem_zram(oem_memcg->zram );
	if (!memcg_oem_zram || !memcg_oem_zram->area) {
		eswap_print(LEVEL_WARN, "invalid mcg\n");
		return;
	}

	if (index >= (u32)oem_zram->area->nr_objs) {
		eswap_print(LEVEL_WARN, "index = %d invalid\n", index);
		return;
	}
	if (zram_test_flag(zram, index, ZRAM_WB)) {
		eswap_print(LEVEL_WARN, "WB object, index = %d\n", index);
		return;
	}
	if (zram_test_flag(zram, index, ZRAM_SAME))
		return;

	zram_set_memcg(zram, index, mcg);
	eswap_list_add_tail(obj_idx(oem_zram->area, index),
			mcg_idx(oem_zram->area, mcg->id.id),
			oem_zram->area->obj_table);

	size = zram_get_obj_size(zram, index);

	atomic64_add(size, &oem_memcg->zram_stored_size);
	atomic64_inc(&oem_memcg->zram_page_size);
	atomic64_add(size, &stat->zram_stored_size);
	atomic64_inc(&stat->zram_stored_pages);
}

int zram_get_memcg_coldest_index(struct eswap_area *area,
				 struct mem_cgroup *mcg,
				 int *index, int max_cnt)
{
	int cnt = 0;
	u32 i;
	u32 tmp;
	int invalid_cnt = 0;

	if (!area) {
		eswap_print(LEVEL_WARN, "area is null\n");
		return 0;
	}
	if (!area->obj_table) {
		eswap_print(LEVEL_WARN, "table is null\n");
		return 0;
	}
	if (!mcg) {
		eswap_print(LEVEL_WARN, "mcg is null\n");
		return 0;
	}
	if (!index) {
		eswap_print(LEVEL_WARN, "index is null\n");
		return 0;
	}

	eswap_lock_list(mcg_idx(area, mcg->id.id), area->obj_table);
	eswap_list_for_each_entry_reverse_safe(i, tmp,
		mcg_idx(area, mcg->id.id), area->obj_table) {
		if (i >= (u32)area->nr_objs) {
			invalid_cnt++;
			if (invalid_cnt >= max_cnt) {
				eswap_print(LEVEL_ERR, "the number of invalid index exceeds %d mcgid = %d\n", max_cnt, mcg->id.id);
			}
			continue;
		}
		index[cnt++] = i;
		if (cnt >= max_cnt)
			break;
	}
	eswap_unlock_list(mcg_idx(area, mcg->id.id), area->obj_table);

	return cnt;
}

void zram_rmap_insert(struct zram *zram, u32 index)
{
	unsigned long eswapentry;
	u32 ext_id;
	struct oem_zram_data *oem_zram = get_oem_zram(zram);

	if (!zram || !oem_zram) {
		eswap_print(LEVEL_WARN, "zram is null\n");
		return;
	}
	if (index >= (u32)oem_zram->area->nr_objs) {
		eswap_print(LEVEL_WARN, "index = %d invalid\n", index);
		return;
	}

	eswapentry = zram_get_handle(zram, index);
	ext_id = esentry_extid(eswapentry);
	eswap_list_add_tail(obj_idx(oem_zram->area, index),
			ext_idx(oem_zram->area, ext_id),
			oem_zram->area->obj_table);
}

void zram_rmap_erase(struct zram *zram, u32 index)
{
	unsigned long eswapentry;
	u32 ext_id;
	struct oem_zram_data *oem_zram = get_oem_zram(zram);

	if (!zram || !oem_zram) {
		eswap_print(LEVEL_WARN, "zram is null\n");
		return;
	}
	if (index >= (u32)oem_zram->area->nr_objs) {
		eswap_print(LEVEL_WARN, "index = %d invalid\n", index);
		return;
	}

	eswapentry = zram_get_handle(zram, index);
	ext_id = esentry_extid(eswapentry);
	eswap_list_del(obj_idx(oem_zram->area, index),
		ext_idx(oem_zram->area, ext_id),
		oem_zram->area->obj_table);
}

int zram_rmap_get_extent_index(struct zram *zram,
			       int ext_id, int *index)
{
	int cnt = 0;
	struct eswap_area *area;
	int i;
	struct oem_zram_data *oem_zram = get_oem_zram(zram);

	if (!oem_zram) {
		eswap_print(LEVEL_WARN, "zram data is null\n");
		return 0;
	}
	area = oem_zram->area;
	if (!area) {
		eswap_print(LEVEL_WARN, "area is null\n");
		return 0;
	}
	if (!area->obj_table) {
		eswap_print(LEVEL_WARN, "table is null\n");
		return 0;
	}
	if (!index) {
		eswap_print(LEVEL_WARN, "index is null\n");
		return 0;
	}
	if (ext_id < 0 || ext_id >= area->nr_exts) {
		eswap_print(LEVEL_WARN, "ext = %d invalid\n", ext_id);
		return 0;
	}

	eswap_lock_list(ext_idx(area, ext_id), area->obj_table);
	eswap_list_for_each_entry(i, ext_idx(area, ext_id), area->obj_table) {
		if (cnt >= (int)EXTENT_MAX_OBJ_CNT) {
			eswap_print(LEVEL_DEBUG, "the number of extent exceeds %d ext_id = %d\n", EXTENT_MAX_OBJ_CNT, ext_id);
			WARN_ON_ONCE(1);
			break;
		}

		if (i >= (u32)area->nr_objs) {
			eswap_print(LEVEL_DEBUG, "ext_id = %d index = %d invalid\n", ext_id, i);
			continue;
		}
		index[cnt++] = i;
	}
	eswap_unlock_list(ext_idx(area, ext_id), area->obj_table);

	return cnt;
}