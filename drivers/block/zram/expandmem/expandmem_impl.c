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
#include <linux/cpu.h>
#include <linux/swap.h>
#include <linux/gfp.h>
#include <linux/freezer.h>
#include <linux/wait.h>
#include <uapi/linux/sched/types.h>

#include "expandmem.h"
#include "expandmem_vendor_hooks.h"
#include "eswap.h"

static wait_queue_head_t snapshotd_wait;
static atomic_t snapshotd_wait_flag;
static atomic_t snapshotd_init_flag = ATOMIC_LONG_INIT(0);
static struct task_struct *snapshotd_task;

static pid_t zswapd_pid = -1;
static u64 area_last_anon_pagefault;
static unsigned long last_anon_snapshot_time;
u64 global_anon_refault_ratio;
u64 zswapd_skip_interval;
bool last_round_is_empty;
unsigned long last_zswapd_time;

typedef struct oem_pglist_data {
	wait_queue_head_t zswapd_wait;
	atomic_t zswapd_wait_flag;
	struct task_struct *zswapd;

} oem_pg_data_t;

static int get_total_ram(void)
{
	u64 nr_total;
	nr_total = (u64) totalram_pages() >> (30 - PAGE_SHIFT); /* Pages to GB */
	return nr_total;
}

bool is_support_eswap(void)
{
	if (get_total_ram() > 6) {
		return false;
	}
	return true;
}

static unsigned int calc_sys_cur_avail_buffers(void)
{
	long buffers;
	buffers = si_mem_available() >> (20 - PAGE_SHIFT); /* Pages to MB */
	return buffers;
}

void zswapd_status_show(struct seq_file *m)
{
	unsigned int buffers = calc_sys_cur_avail_buffers();

	seq_printf(m, "buffer size: %u MB\n", buffers);
	seq_printf(m, "recent refault: %lu\n", global_anon_refault_ratio);
}

pid_t get_zswapd_pid(void)
{
	return zswapd_pid;
}

static bool min_buffer_is_suitable(void)
{
	u32 curr_buffers = calc_sys_cur_avail_buffers();

	if (curr_buffers >= get_min_avail_buffers_value())
		return true;

	return false;
}

static bool buffer_is_suitable(void)
{
	u32 curr_buffers = calc_sys_cur_avail_buffers();

	if (curr_buffers >= get_avail_buffers_value())
		return true;

	return false;
}

static bool high_buffer_is_suitable(void)
{
	u32 curr_buffers = calc_sys_cur_avail_buffers();

	if (curr_buffers >= get_high_avail_buffers_value())
		return true;

	return false;
}

static void snapshot_anon_refaults(void)
{
	struct mem_cgroup *memcg = NULL;
	struct oem_mem_cgroup *oem_memcg = NULL;

	while ((memcg = get_next_memcg(memcg))) {
		oem_memcg = get_oem_mem_cgroup(memcg);
		if (oem_memcg)
			oem_memcg->reclaimed_pagefault =
				eswap_read_mcg_stats(memcg, MCG_ANON_FAULT_CNT);
	}

	area_last_anon_pagefault = eswap_get_zram_pagefault();
	last_anon_snapshot_time = jiffies;
}

static struct pglist_data *expandmem_lruvec_pgdat(struct lruvec *lruvec)
{
	return lruvec->pgdat;
}

static unsigned long expandmem_mem_cgroup_get_zone_lru_size(struct lruvec *lruvec,
		enum lru_list lru, int zone_idx)
{
	struct mem_cgroup_per_node *mz;

	mz = container_of(lruvec, struct mem_cgroup_per_node, lruvec);
	return READ_ONCE(mz->lru_zone_size[zone_idx][lru]);
}

static unsigned long expandmem_zone_page_state(struct zone *zone,
					enum zone_stat_item item)
{
	long x = atomic_long_read(&zone->vm_stat[item]);
	if (x < 0)
		x = 0;
	return x;
}

inline bool oem_mem_cgroup_disabled(void)
{
	return !(atomic_read(&((struct static_key *)(&(&memory_cgrp_subsys_enabled_key)->key))->enabled)  > 0);
}

/**
 * expandmem_lruvec_lru_size -  Returns the number of pages on the given LRU list.
 * @lruvec: lru vector
 * @lru: lru to use
 * @zone_idx: zones to consider (use MAX_NR_ZONES for the whole LRU list)
 */
unsigned long expandmem_lruvec_lru_size(struct lruvec *lruvec, enum lru_list lru, int zone_idx)
{
	unsigned long size = 0;
	int zid;

	for (zid = 0; zid <= zone_idx && zid < MAX_NR_ZONES; zid++) {
		struct zone *zone = &expandmem_lruvec_pgdat(lruvec)->node_zones[zid];

		if (!((unsigned long)atomic_long_read(&zone->managed_pages)))
			continue;

		if (!oem_mem_cgroup_disabled())
			size += expandmem_mem_cgroup_get_zone_lru_size(lruvec, lru, zid);
		else
			size += expandmem_zone_page_state(zone, NR_ZONE_LRU_BASE + lru);
	}
	return size;
}

/*
 * Return true if refault changes between two read operations.
 */
static bool get_memcg_anon_refault_status(struct mem_cgroup *memcg)
{
	const unsigned int percent_constant = 100;
	u64 cur_anon_pagefault;
	unsigned long anon_total;
	u64 ratio;
	struct oem_mem_cgroup *oem_memcg;

	struct mem_cgroup_per_node *mz = NULL;
	struct lruvec *lruvec = NULL;

	if (!memcg)
		return false;

	oem_memcg = get_oem_mem_cgroup(memcg);
	if (!oem_memcg)
		return false;

	cur_anon_pagefault = eswap_read_mcg_stats(memcg, MCG_ANON_FAULT_CNT);

	if (cur_anon_pagefault == oem_memcg->reclaimed_pagefault)
		return false;

	mz = mem_cgroup_nodeinfo(memcg, 0);
	if (!mz)
		return false;
	lruvec = &mz->lruvec;
	if (!lruvec)
		return false;
	anon_total = expandmem_lruvec_lru_size(lruvec, LRU_ACTIVE_ANON, MAX_NR_ZONES) +
		expandmem_lruvec_lru_size(lruvec, LRU_INACTIVE_ANON, MAX_NR_ZONES) +
		eswap_read_mcg_stats(memcg, MCG_DISK_STORED_PG_SZ) +
		eswap_read_mcg_stats(memcg, MCG_ZRAM_PG_SZ);

	ratio = (cur_anon_pagefault - oem_memcg->reclaimed_pagefault) *
		percent_constant / (anon_total + 1);

	if (ratio > atomic_read(&oem_memcg->refault_threshold))
		return true;

	return false;
}

static bool get_area_anon_refault_status(void)
{
	const unsigned int percent_constant = 1000;
	u64 cur_anon_pagefault;
	u64 cur_time;
	u64 ratio;

	cur_anon_pagefault = eswap_get_zram_pagefault();
	cur_time = jiffies;

	if (cur_anon_pagefault == area_last_anon_pagefault
		|| cur_time == last_anon_snapshot_time)
		return false;

	ratio = (cur_anon_pagefault - area_last_anon_pagefault) *
		percent_constant / (jiffies_to_msecs(cur_time -
					last_anon_snapshot_time) + 1);

	global_anon_refault_ratio = ratio;

	if (ratio > get_area_anon_refault_threshold_value())
		return true;

	return false;
}

static void wakeup_snapshotd(void)
{
	unsigned long curr_snapshot_interval;
	curr_snapshot_interval =
		jiffies_to_msecs(jiffies - last_anon_snapshot_time);

	if (curr_snapshot_interval >=
		get_anon_refault_snapshot_min_interval_value()) {
		atomic_set(&snapshotd_wait_flag, 1);
		wake_up_interruptible(&snapshotd_wait);
	}
}

static int snapshotd(void *p)
{
	int ret;

	while (!kthread_should_stop()) {

		ret = wait_event_interruptible(snapshotd_wait,
				atomic_read(&snapshotd_wait_flag));
		if (ret)
			continue;

		atomic_set(&snapshotd_wait_flag, 0);

		snapshot_anon_refaults();
	}

	return 0;
}

void set_snapshotd_init_flag(unsigned int val)
{
	atomic_set(&snapshotd_init_flag, val);
}

/*
 * This snapshotd start function will be called by init.
 */
static int snapshotd_run(void)
{
	atomic_set(&snapshotd_wait_flag, 0);
	init_waitqueue_head(&snapshotd_wait);
	snapshotd_task = kthread_run(snapshotd, NULL, "snapshotd");
	if (IS_ERR(snapshotd_task)) {
		eswap_print(LEVEL_ERR, "expandmem failed to start snapshotd\n");
		return PTR_ERR(snapshotd_task);
	}

	return 0;
}

int snapshotd_init(void)
{
	snapshotd_run();
	return 0;
}

int get_zram_current_watermark(void)
{
	long long diff_buffers;
	const unsigned int percent_constant = 100;
	u64 nr_total;

	nr_total = (u64) totalram_pages();
	diff_buffers = get_avail_buffers_value() -
		calc_sys_cur_avail_buffers(); /* M_target - M_current */
	diff_buffers *= SZ_1M / PAGE_SIZE; /* MB to page */
	diff_buffers *= percent_constant / get_compress_ratio_value(); /* after_comp to before_comp */
	diff_buffers = diff_buffers * percent_constant /
		nr_total; /* page to ratio */
	return min(get_zram_wm_ratio_value(),
		get_zram_wm_ratio_value() - diff_buffers);
}

bool zram_watermark_ok(void)
{
	const unsigned int percent_constant = 100;
	u64 curr_ratio;
	u64 nr_zram_used;
	u64 nr_wm;

	curr_ratio = get_zram_current_watermark();
	nr_zram_used = eswap_get_zram_used_pages();
	nr_wm = (u64) totalram_pages() * curr_ratio / percent_constant;
	if (nr_zram_used > nr_wm)
		return true;
	return false;
}

static bool zram_watermark_exceed(void)
{
	u64 nr_zram_used;
	u64 nr_wm = get_zram_critical_threshold_value() << (20 - PAGE_SHIFT);

	if (!nr_wm)
		return false;

	nr_zram_used = eswap_get_zram_used_pages();
	if (nr_zram_used > nr_wm)
		return true;
	return false;
}

static bool free_swap_is_low(void)
{
	u64 freeswap = 0;

	struct sysinfo val;
	si_swapinfo(&val);
	freeswap = val.freeswap; /* Pages*/
	return (freeswap < get_swap_free_low_value());
}

static unsigned long zswapd_shrink_node_memcgs(pg_data_t *pgdat, unsigned long nr_to_reclaim)
{
	struct mem_cgroup *memcg = NULL;
	const u32 percent_constant = 100;
	unsigned long total_nr_reclaimed = 0;

	while ((memcg = get_next_memcg(memcg))) {
		struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);
		u64 nr_active, nr_inactive;
		u64 nr_zram, nr_expandmem;
		u64 zram_ratio;
		struct oem_mem_cgroup *oem_memcg;
		unsigned long reclaimed;

		oem_memcg = get_oem_mem_cgroup(memcg);
		if (!oem_memcg)
			continue;

		/* reclaim and try to meet the high buffer watermark */
		if (high_buffer_is_suitable()) {
			get_next_memcg_break(memcg);
			break;
		}

#ifdef CONFIG_MEMCG_PROTECT_LRU
		/* Skip if it is a protect memcg. */
		if (is_prot_memcg(memcg, false))
			continue;
#endif

		if (get_memcg_anon_refault_status(memcg))
			continue;

		nr_active = expandmem_lruvec_lru_size(lruvec, LRU_ACTIVE_ANON,
				MAX_NR_ZONES);
		nr_inactive = expandmem_lruvec_lru_size(lruvec,
				LRU_INACTIVE_ANON, MAX_NR_ZONES);
		nr_zram = eswap_read_mcg_stats(memcg, MCG_ZRAM_PG_SZ);
		nr_expandmem = eswap_read_mcg_stats(memcg, MCG_DISK_STORED_PG_SZ);

		zram_ratio = (nr_zram + nr_expandmem) * percent_constant /
			(nr_inactive + nr_active + nr_zram + nr_expandmem + 1);

		if (zram_ratio >= atomic_read(&oem_memcg->ub_mem2zram_ratio))
			continue;

		reclaimed = try_to_free_mem_cgroup_pages(memcg, nr_to_reclaim, GFP_KERNEL, true);
		if (!reclaimed)
			continue;
		total_nr_reclaimed += reclaimed;

		if (total_nr_reclaimed >= nr_to_reclaim) {
			get_next_memcg_break(memcg);
			break;
		}
	}

	return total_nr_reclaimed;
}

static u64 __calc_nr_to_reclaim(void)
{
	u32 curr_buffers;
	u64 high_buffers;
	u64 max_reclaim_size_value;
	u64 reclaim_size = 0;

	high_buffers = get_high_avail_buffers_value();
	curr_buffers = calc_sys_cur_avail_buffers();
	max_reclaim_size_value = get_zswapd_max_reclaim_size();
	if (curr_buffers < high_buffers)
		reclaim_size = high_buffers - curr_buffers;

	/* once max reclaim target is max_reclaim_size_value */
	reclaim_size = min(reclaim_size, max_reclaim_size_value);

	return reclaim_size << (20 - PAGE_SHIFT); /* MB to pages */
}

static void zswapd_shrink_node(pg_data_t *pgdat)
{
	const unsigned int increase_rate = 2;
	unsigned long nr_to_reclaim = 0;
	int priority = DEF_PRIORITY;
	unsigned long total_nr_reclaimed = 0;

	do {
		unsigned long nr_reclaimed = 0;

		/* reclaim and try to meet the high buffer watermark */
		if (high_buffer_is_suitable())
			break;

		nr_to_reclaim = __calc_nr_to_reclaim();
		if (!nr_to_reclaim)
			break;

		nr_reclaimed = zswapd_shrink_node_memcgs(pgdat, nr_to_reclaim);

		total_nr_reclaimed += nr_reclaimed;
		if (try_to_freeze() || kthread_should_stop())
			break;
	} while (--priority >= 0);

	/*
	 * When meets the first empty round, set the interval to t.
	 * If the following round is still empty, set the interval
	 * to 2t. If the round is always empty, then 4t, 8t, and so on.
	 * But make sure the interval is not more than the max_skip_interval.
	 * Once a non-empty round occurs, reset the interval to 20.
	 */
	if (total_nr_reclaimed < get_empty_round_check_threshold_value()) {
		if (last_round_is_empty)
			zswapd_skip_interval = min(zswapd_skip_interval *
					increase_rate,
					get_max_skip_interval_value());
		else
			zswapd_skip_interval =
				get_empty_round_skip_interval_value();
		last_round_is_empty = true;
	} else {
		zswapd_skip_interval = get_empty_round_skip_interval_value();
		last_round_is_empty = false;
	}
}

/*
 * The background pageout daemon, started as a kernel thread
 * from the init process.
 *
 * This basically trickles out pages so that we have _some_
 * free memory available even if there is no other activity
 * that frees anything up. This is needed for things like routing
 * etc, where we otherwise might have all activity going on in
 * asynchronous contexts that cannot page things out.
 *
 * If there are applications that are active memory-allocators
 * (most normal use), this basically shouldn't matter.
 */
static int zswapd(void *p)
{
	pg_data_t *pgdat = (pg_data_t*)p;
	oem_pg_data_t *oem_pgdat = (oem_pg_data_t *)pgdat->android_oem_data1;
	struct task_struct *tsk = current;
	const struct cpumask *cpumask = cpumask_of_node(pgdat->node_id);

	/* save zswapd pid for schedule strategy */
	zswapd_pid = tsk->pid;

	if (!cpumask_empty(cpumask))
		set_cpus_allowed_ptr(tsk, cpumask);

	set_freezable();

	while (!kthread_should_stop()) {
		bool refault = false;
		bool swaplow = false;
		u32 curr_buffers, avail;
		u64 size;

		wait_event_freezable(oem_pgdat->zswapd_wait,
				atomic_read(&oem_pgdat->zswapd_wait_flag));
		atomic_set(&oem_pgdat->zswapd_wait_flag, 0);
		zswapd_pressure_report(ZSWAPD_PRESSURE_LOW);

		if (free_swap_is_low() || zram_watermark_exceed()) {
			swaplow = true;
			goto do_eswap;
		}

		if (get_area_anon_refault_status()) {
			refault = true;
			goto do_eswap;
		}

		zswapd_shrink_node(pgdat);
		last_zswapd_time = jiffies;

do_eswap:
		if (!eswap_reclaim_work_running() &&
				(swaplow || refault || zram_watermark_ok())) {
			avail = get_high_avail_buffers_value();
			curr_buffers = calc_sys_cur_avail_buffers();
			size = (avail - curr_buffers) * SZ_1M;
			if (curr_buffers < avail)
				size = eswap_reclaim_in(size);
		}

		if (!buffer_is_suitable()) {
			if (free_swap_is_low() || zram_watermark_exceed()) {
				zswapd_pressure_report(ZSWAPD_PRESSURE_CRITICAL);
				//expandmem_set_enable(true);
			} else {
				zswapd_pressure_report(ZSWAPD_PRESSURE_MEDIUM);
			}
		}
	}

	return 0;
}

/*
 * A zone is low on free memory or too fragmented for high-order memory.  If
 * kswapd should reclaim (direct reclaim is deferred), wake it up for the zone's
 * pgdat.  It will wake up kcompactd after reclaiming memory.  If kswapd reclaim
 * has failed or is not needed, still wake up kcompactd if only compaction is
 * needed.
 */
static void wakeup_zswapd(pg_data_t *pgdat)
{
	unsigned long curr_interval;
	oem_pg_data_t *oem_pgdat = (oem_pg_data_t *)pgdat->android_oem_data1;

	if (IS_ERR(oem_pgdat))
		return;

	if (IS_ERR(oem_pgdat->zswapd))
		return;

	if (!wq_has_sleeper(&oem_pgdat->zswapd_wait))
		return;

	/* make anon pagefault snapshots */
	/* wake up snapshotd */
	if (atomic_read(&snapshotd_init_flag) == 1)
		wakeup_snapshotd();

	/* wake up when the buffer is lower than min_avail_buffer */
	if (min_buffer_is_suitable())
		return;

	curr_interval =
		jiffies_to_msecs(jiffies - last_zswapd_time);
	if (curr_interval < zswapd_skip_interval)
		return;

	atomic_set(&oem_pgdat->zswapd_wait_flag, 1);
	wake_up_interruptible(&oem_pgdat->zswapd_wait);
}

void wake_all_zswapd(void)
{
	pg_data_t *pgdat = NULL;
	int nid;

	if (!eswap_get_enable())
		return;

	for_each_online_node(nid) {
		pgdat = NODE_DATA(nid);
		wakeup_zswapd(pgdat);
	}
}

/* It's optimal to keep kswapds on the same CPUs as their memory, but
   not required for correctness.  So if the last cpu in a node goes
   away, we get changed to run anywhere: as the first one comes back,
   restore their cpu bindings. */
static int zswapd_cpu_online(unsigned int cpu)
{
	int nid;

	for_each_node_state(nid, N_MEMORY) {
		pg_data_t *pgdat = NODE_DATA(nid);
		oem_pg_data_t *oem_pgdat = (oem_pg_data_t *)pgdat->android_oem_data1;
		const struct cpumask *mask;

		if (!oem_pgdat)
			continue;

		mask = cpumask_of_node(pgdat->node_id);

		if (cpumask_any_and(cpu_online_mask, mask) < nr_cpu_ids) {
			/* One of our CPUs online: restore mask */
			set_cpus_allowed_ptr(oem_pgdat->zswapd, mask);
		}
	}
	return 0;
}

/*
 * This zswapd start function will be called by init and node-hot-add.
 * On node-hot-add, zswapd will moved to proper cpus if cpus are hot-added.
 */
static int zswapd_run(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);
	oem_pg_data_t *oem_pgdat = (oem_pg_data_t *)pgdat->android_oem_data1;
	int ret = 0;

	const unsigned int priority_less = 5;
	struct sched_param param = {
		.sched_priority = MAX_PRIO - priority_less,
	};

	if (!oem_pgdat) {
		pgdat->android_oem_data1 = (u64)kzalloc(sizeof(oem_pg_data_t), GFP_KERNEL);
		oem_pgdat = (oem_pg_data_t *)pgdat->android_oem_data1;
		if (!oem_pgdat) {
			eswap_print(LEVEL_ERR, "alloc error %d\n", ERR_PTR(-ENOMEM));
			return -ENOMEM;
		}
	}

	if (oem_pgdat->zswapd)
		return 0;

	atomic_set(&oem_pgdat->zswapd_wait_flag, 0);
	init_waitqueue_head(&oem_pgdat->zswapd_wait);

	oem_pgdat->zswapd = kthread_create(zswapd, pgdat, "zswapd%d", nid);
	if (IS_ERR(oem_pgdat->zswapd)) {
		eswap_print(LEVEL_ERR, "expandmem failed to start zswapd on node %d\n", nid);
		ret = PTR_ERR(oem_pgdat->zswapd);
		oem_pgdat->zswapd = NULL;
		return ret;
	}

	sched_setscheduler_nocheck(oem_pgdat->zswapd, SCHED_NORMAL, &param);
	set_user_nice(oem_pgdat->zswapd, PRIO_TO_NICE(param.sched_priority));
	wake_up_process(oem_pgdat->zswapd);
	return ret;
}

/*
 * Called by memory hotplug when all memory in a node is offlined.  Caller must
 * hold mem_hotplug_begin/end().
 */
static void zswapd_stop(int nid)
{
	struct task_struct *zswapd;
	pg_data_t *pgdat = NODE_DATA(nid);
	oem_pg_data_t *oem_pgdat = (oem_pg_data_t *)pgdat->android_oem_data1;
	if (oem_pgdat) {
		zswapd = oem_pgdat->zswapd;
		if (zswapd) {
			kthread_stop(zswapd);
			oem_pgdat->zswapd = NULL;
		}
		kfree(oem_pgdat);
	}

	zswapd_pid = -1;
}

void zswapd_exit(void)
{
	int nid;

	for_each_node_state(nid, N_MEMORY) {
		zswapd_stop(nid);
	}

	zswapd_pid = -1;
}

int zswapd_init(void)
{
	int nid, ret;

	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
					"mm/expandmem:online", zswapd_cpu_online,
					NULL);
	if (ret < 0) {
		eswap_print(LEVEL_ERR, "expandmem zswapd: failed to register hotplug callbacks.\n");
		return ret;
	}

	for_each_node_state(nid, N_MEMORY) {
		zswapd_run(nid);
	}

	return 0;
}
