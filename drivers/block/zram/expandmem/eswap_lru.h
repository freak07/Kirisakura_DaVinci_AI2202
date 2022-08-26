/*
 * Expanded RAM block device
 * Description: expanded memory implement
 *
 * Released under the terms of GNU General Public License Version 2.0
 *
 */

#ifndef _ESWAP_LRU_H_
#define _ESWAP_LRU_H_

#define EXTENT_MAX_OBJ_CNT (30 * EXTENT_PG_CNT)
#define esentry_extid(e) ((e) >> EXTENT_SHIFT)

void zram_set_memcg(struct zram *zram, u32 index, struct mem_cgroup *mcg);
struct mem_cgroup *zram_get_memcg(struct zram *zram, u32 index);
void eswap_zram_lru_add(struct zram *zram,
			    u32 index,
			    struct mem_cgroup *mcg);
void zram_lru_add_tail(struct zram *zram, u32 index, struct mem_cgroup *mcg);
void zram_lru_del(struct zram *zram, u32 index);
void zram_rmap_insert(struct zram *zram, u32 index);
void zram_rmap_erase(struct zram *zram, u32 index);
int zram_rmap_get_extent_index(struct zram *zram,
			       int ext_id, int *index);
int zram_get_memcg_coldest_index(struct eswap_area *area,
				 struct mem_cgroup *mcg,
				 int *index, int max_cnt);

#endif
