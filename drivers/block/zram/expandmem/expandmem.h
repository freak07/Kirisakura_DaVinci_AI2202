/*
 * Expanded RAM block device
 * Description: expanded memory implement
 *
 * Released under the terms of GNU General Public License Version 2.0
 *
 */

#ifndef _EXPANDMEM_H_
#define _EXPANDMEM_H_

#define MEM_CGROUP_NAME_MAX_LEN 100
struct oem_mem_cgroup {
	struct cgroup_subsys_state *css;
	atomic_t ub_zram2ufs_ratio;
	atomic64_t app_score;
	struct list_head score_node;
	char name[MEM_CGROUP_NAME_MAX_LEN];

	atomic_t ub_mem2zram_ratio;
	atomic_t refault_threshold;
	/* anon refault */
	u64 reclaimed_pagefault;

	unsigned long zram_lru;
	unsigned long ext_lru;
	spinlock_t zram_init_lock;
	struct zram *zram;

	atomic64_t zram_stored_size;
	atomic64_t zram_page_size;

	atomic64_t eswap_stored_pages;
	atomic64_t eswap_stored_size;

	atomic64_t eswap_outcnt;
	atomic64_t eswap_outextcnt;
	atomic64_t eswap_faultcnt;
	atomic64_t eswap_allfaultcnt;
};

struct mem_cgroup *get_next_memcg(struct mem_cgroup *prev);
void get_next_memcg_break(struct mem_cgroup *memcg);
struct oem_mem_cgroup *get_oem_mem_cgroup(struct mem_cgroup *memcg);

void wake_all_zswapd(void);
bool zram_watermark_ok(void);
#endif
