/*
 * Expanded RAM block device
 * Description: expanded memory implement
 *
 * Released under the terms of GNU General Public License Version 2.0
 *
 */

#ifndef _ESWAP_AREA_H_
#define _ESWAP_AREA_H_
#include <linux/memcontrol.h>

struct eswap_area {
	unsigned long size;
	int nr_objs;
	int nr_exts;
	int nr_mcgs;

	unsigned long *bitmap;
	atomic_t last_alloc_bit;

	struct eswap_list_table *ext_table;
	struct eswap_list_head *ext;

	struct eswap_list_table *obj_table;
	struct eswap_list_head *rmap;
	struct eswap_list_head *lru;

	atomic_t stored_exts;
	atomic_t *ext_stored_pages;

	unsigned int mcg_id_cnt[MEM_CGROUP_ID_MAX + 1];
};

struct mem_cgroup *get_mem_cgroup(unsigned short mcg_id);

int obj_idx(struct eswap_area *area, int idx);
int ext_idx(struct eswap_area *area, int idx);
int mcg_idx(struct eswap_area *area, int idx);

int get_extent(struct eswap_area *area, int ext_id);
void put_extent(struct eswap_area *area, int ext_id);
void eswap_free_extent(struct eswap_area *area, int ext_id);
int eswap_alloc_extent(struct eswap_area *area, struct mem_cgroup *mcg);
int get_memcg_zram_entry(struct eswap_area *area, struct mem_cgroup *mcg);
int get_memcg_extent(struct eswap_area *area, struct mem_cgroup *mcg);
void free_eswap_area(struct eswap_area *area);
struct eswap_area *alloc_eswap_area(unsigned long ori_size,
					    unsigned long comp_size);

#endif
