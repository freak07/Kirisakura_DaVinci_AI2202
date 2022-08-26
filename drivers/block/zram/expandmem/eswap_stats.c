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
#include <linux/atomic.h>
#include <linux/memcontrol.h>
#include <linux/zsmalloc.h>

#include "eswap_common.h"
#include "eswap.h"
#include "expandmem.h"

#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
#define MBYTE_SHIFT 20
#endif

bool eswap_reclaim_work_running(void)
{
	struct eswap_stat *stat = NULL;

	if (!eswap_get_enable())
		return false;

	stat = eswap_get_stat_obj();
	if (unlikely(!stat)) {
		eswap_print(LEVEL_WARN, "can't get stat obj\n");
		return false;
	}

	return atomic64_read(&stat->reclaimin_infight) ? true : false;
}

u64 eswap_read_mcg_stats(struct mem_cgroup *mcg,
				enum eswap_mcg_member mcg_member)
{
	u64 val = 0;
	struct oem_mem_cgroup *oem_memcg;

	if (!eswap_get_enable()) {
		return 0;
	}

	oem_memcg = get_oem_mem_cgroup(mcg);
	if (!oem_memcg) {
		return 0;
	}

	switch (mcg_member) {
	case MCG_ZRAM_STORED_SZ:
		val = atomic64_read(&oem_memcg->zram_stored_size);
		break;
	case MCG_ZRAM_PG_SZ:
		val = atomic64_read(&oem_memcg->zram_page_size);
		break;
	case MCG_DISK_STORED_SZ:
		val = atomic64_read(&oem_memcg->eswap_stored_size);
		break;
	case MCG_DISK_STORED_PG_SZ:
		val = atomic64_read(&oem_memcg->eswap_stored_pages);
		break;
	case MCG_ANON_FAULT_CNT:
		val = atomic64_read(&oem_memcg->eswap_allfaultcnt);
		break;
	case MCG_DISK_FAULT_CNT:
		val = atomic64_read(&oem_memcg->eswap_faultcnt);
		break;
	case MCG_SWAPOUT_CNT:
		val = atomic64_read(&oem_memcg->eswap_outcnt);
		break;
	case MCG_SWAPOUT_SZ:
		val = atomic64_read(&oem_memcg->eswap_outextcnt) << EXTENT_SHIFT;
		break;
	default:
		break;
	}

	return val;
}

unsigned long eswap_get_zram_used_pages(void)
{
	struct eswap_stat *stat = NULL;

	if (!eswap_get_enable())
		return 0;

	stat = eswap_get_stat_obj();
	if (unlikely(!stat)) {
		eswap_print(LEVEL_WARN, "can't get stat obj\n");
		return 0;
	}

	return atomic64_read(&stat->zram_stored_pages);
}

u64 eswap_get_zram_pagefault(void)
{
	struct eswap_stat *stat = NULL;

	if (!eswap_get_enable())
		return 0;

	stat = eswap_get_stat_obj();
	if (unlikely(!stat)) {
		eswap_print(LEVEL_WARN, "can't get stat obj\n");
		return 0;
	}

	return atomic64_read(&stat->fault_check_cnt);
}

#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
static void eswap_stats_show(struct seq_file *m,
	struct eswap_stat *stat)
{
	seq_printf(m, "swapout_times %lld\n",
		atomic64_read(&stat->reclaimin_cnt));
	seq_printf(m, "swapout_comp_size %lld MB\n",
		atomic64_read(&stat->reclaimin_bytes) >> MBYTE_SHIFT);
	if (PAGE_SHIFT < MBYTE_SHIFT)
		seq_printf(m, "swapout_ori_size %lld MB\n",
			atomic64_read(&stat->reclaimin_pages) >>
				(MBYTE_SHIFT - PAGE_SHIFT));
	seq_printf(m, "swapin_times %lld\n",
		atomic64_read(&stat->faultout_cnt));
	seq_printf(m, "swapin_comp_size %lld MB\n",
		atomic64_read(&stat->faultout_bytes) >> MBYTE_SHIFT);
	if (PAGE_SHIFT < MBYTE_SHIFT)
		seq_printf(m, "swapin_ori_size %lld MB\n",
			atomic64_read(&stat->faultout_pages) >>
				(MBYTE_SHIFT - PAGE_SHIFT));
	seq_printf(m, "eswap_all_fault %lld\n",
		atomic64_read(&stat->fault_check_cnt));
	seq_printf(m, "eswap_fault %lld\n",
		atomic64_read(&stat->eswap_fault_cnt));
}

static void eswap_area_info_show(struct seq_file *m,
	struct eswap_stat *stat)
{
	seq_printf(m, "eswap_reout_ori_size %lld MB\n",
		atomic64_read(&stat->reout_pages) >>
			(MBYTE_SHIFT - PAGE_SHIFT));
	seq_printf(m, "eswap_reout_comp_size %lld MB\n",
		atomic64_read(&stat->reout_bytes) >> MBYTE_SHIFT);
	seq_printf(m, "eswap_store_comp_size %lld MB\n",
		atomic64_read(&stat->stored_size) >> MBYTE_SHIFT);
	seq_printf(m, "eswap_store_ori_size %lld MB\n",
		atomic64_read(&stat->stored_pages) >>
			(MBYTE_SHIFT - PAGE_SHIFT));
	seq_printf(m, "eswap_notify_free_size %lld MB\n",
		atomic64_read(&stat->notify_free) >>
			(MBYTE_SHIFT - EXTENT_SHIFT));
	seq_printf(m, "eswap_store_memcg_cnt %lld\n",
		atomic64_read(&stat->mcg_cnt));
	seq_printf(m, "eswap_store_extent_cnt %lld\n",
		atomic64_read(&stat->ext_cnt));
	seq_printf(m, "eswap_store_fragment_cnt %lld\n",
		atomic64_read(&stat->frag_cnt));
}

void eswap_psi_show(struct seq_file *m)
{
	struct eswap_stat *stat = NULL;

	if (!eswap_get_enable())
		return;

	stat = eswap_get_stat_obj();
	if (unlikely(!stat)) {
		eswap_print(LEVEL_WARN, "can't get stat obj!\n");
		return;
	}

	eswap_stats_show(m, stat);
	eswap_area_info_show(m, stat);
}
#endif

unsigned long eswap_get_reclaimin_bytes(void)
{
	struct eswap_stat *stat = NULL;

	if (!eswap_get_enable())
		return 0;

	stat = eswap_get_stat_obj();
	if (unlikely(!stat)) {
		eswap_print(LEVEL_WARN, "can't get stat obj\n");
		return 0;
	}

	return atomic64_read(&stat->reclaimin_bytes);
}

