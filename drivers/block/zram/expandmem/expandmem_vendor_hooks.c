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
#include <linux/string.h>
#include <linux/memcontrol.h>
#include <linux/cgroup.h>
#include <trace/hooks/mm.h>
#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/time.h>
#include <linux/jiffies.h>
#include <linux/eventfd.h>
#include <linux/file.h>
#include "../../../scsi/ufs/ufshcd.h"
#include <scsi/scsi_proto.h>
#include <asm/byteorder.h>
#include <trace/hooks/ufshcd.h>
#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
#include <linux/rtc.h>
#endif

#include "expandmem.h"
#include "expandmem_impl.h"
#include "expandmem_vendor_hooks.h"
#include "eswap.h"

struct list_head score_head;
static bool score_head_inited;
static DEFINE_MUTEX(reclaim_para_lock);
static DEFINE_SPINLOCK(score_list_lock);

static DEFINE_MUTEX(zswapd_pressure_event_lock);
struct eventfd_ctx *zswapd_press_efd[ZSWAPD_PRESSURE_NUM_LEVELS];

#define MAX_APP_SCORE 1000
#define MAX_RATIO 100
atomic_t avail_buffers = ATOMIC_INIT(0);
atomic_t min_avail_buffers = ATOMIC_INIT(0);
atomic_t high_avail_buffers = ATOMIC_INIT(0);
atomic64_t swap_free_low = ATOMIC64_INIT(0); //pages
atomic64_t zram_critical_threshold = ATOMIC_LONG_INIT(0);
atomic_t max_reclaim_size = ATOMIC_INIT(100);

#define ZRAM_WM_RATIO 37
#define COMPRESS_RATIO 30
atomic64_t zram_wm_ratio = ATOMIC_LONG_INIT(ZRAM_WM_RATIO);
atomic64_t compress_ratio = ATOMIC_LONG_INIT(COMPRESS_RATIO);

#define AREA_ANON_REFAULT_THRESHOLD 22000
#define ANON_REFAULT_SNAPSHOT_MIN_INTERVAL 200
#define EMPTY_ROUND_SKIP_INTERNVAL 20
#define MAX_SKIP_INTERVAL 1000
#define EMPTY_ROUND_CHECK_THRESHOLD 10
atomic64_t area_anon_refault_threshold =
	ATOMIC_LONG_INIT(AREA_ANON_REFAULT_THRESHOLD);
atomic64_t anon_refault_snapshot_min_interval =
	ATOMIC_LONG_INIT(ANON_REFAULT_SNAPSHOT_MIN_INTERVAL);
atomic64_t empty_round_skip_interval =
	ATOMIC_LONG_INIT(EMPTY_ROUND_SKIP_INTERNVAL);
atomic64_t max_skip_interval =
	ATOMIC_LONG_INIT(MAX_SKIP_INTERVAL);
atomic64_t empty_round_check_threshold =
	ATOMIC_LONG_INIT(EMPTY_ROUND_CHECK_THRESHOLD);

#define MB_SHIFT 20
atomic64_t ufs_write_bytes = ATOMIC_LONG_INIT(0);
#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
static int EMEM_UFS_OBSERVE_DELAY_TIME = 30 * 60 * 1000;
static struct workqueue_struct *expandmem_monitor_wq = NULL;
static struct delayed_work expandmem_monitor_task;
static int task_initialized;
#endif

struct oem_mem_cgroup *get_oem_mem_cgroup(struct mem_cgroup *memcg)
{
	struct oem_mem_cgroup *oem_memcg;

	if (!memcg) {
		eswap_print(LEVEL_WARN, "mcd invalid\n");
		return NULL;
	}
	oem_memcg = (struct oem_mem_cgroup *)memcg->android_oem_data1;
	if (IS_ERR_OR_NULL(oem_memcg)) {
		eswap_print(LEVEL_WARN, "mcg data invalid\n");
		return NULL;
	}
	return oem_memcg;
}

/**
 * get_next_memcg - iterate over memory cgroup score_list
 * @prev: previously returned memcg, NULL on first invocation
 *
 * Returns references to the next memg on score_list of @prev,
 * or %NULL after a full round-trip.
 *
 * Caller must pass the return value in @prev on subsequent
 * invocations for reference counting, or use get_next_memcg_break()
 * to cancel a walk before the round-trip is complete.
 */
struct mem_cgroup *get_next_memcg(struct mem_cgroup *prev)
{
	struct mem_cgroup *memcg = NULL;
	struct oem_mem_cgroup *oem_memcg = NULL;
	struct oem_mem_cgroup *oem_prev = NULL;
	struct list_head *pos = NULL;
	unsigned long flags;

	if (!eswap_get_enable()) {
		return NULL;
	}

	if (unlikely(!score_head_inited))
		return NULL;

	spin_lock_irqsave(&score_list_lock, flags);

	if (unlikely(!prev))
		pos = &score_head;
	else {
		oem_prev = get_oem_mem_cgroup(prev);
		if (oem_prev)
			pos = &oem_prev->score_node;
	}

	if (list_empty(pos)) /* deleted node */
		goto unlock;

	if (pos->next == &score_head)
		goto unlock;

	oem_memcg = list_entry(pos->next,
			struct oem_mem_cgroup, score_node);
	if (unlikely(!oem_memcg))
		goto unlock;

	memcg = mem_cgroup_from_css(oem_memcg->css);

	if (IS_ERR_OR_NULL(memcg))
		goto unlock;

	if (!css_tryget(&memcg->css))
		memcg = NULL;

unlock:
	spin_unlock_irqrestore(&score_list_lock, flags);

	if (prev)
		css_put(&prev->css);

	return memcg;
}

void get_next_memcg_break(struct mem_cgroup *memcg)
{
	if (memcg)
		css_put(&memcg->css);
}

static void oem_mem_cgroup_init(struct mem_cgroup *memcg)
{
	long error = -ENOMEM;
	struct oem_mem_cgroup *oem_memcg = NULL;

	if (IS_ERR_OR_NULL(memcg))
		return;

	if (unlikely(!score_head_inited)) {
		INIT_LIST_HEAD(&score_head);
		score_head_inited = true;
	}

	oem_memcg = get_oem_mem_cgroup(memcg);
	if (!oem_memcg) {
		memset(&memcg->android_oem_data1, 0, sizeof(memcg->android_oem_data1));
		memcg->android_oem_data1 = (u64)kzalloc(sizeof(struct oem_mem_cgroup), GFP_KERNEL);
		oem_memcg = get_oem_mem_cgroup(memcg);
		if (!oem_memcg) {
			eswap_print(LEVEL_ERR, "alloc error %d\n", ERR_PTR(error));
			return;
		}

		INIT_LIST_HEAD(&oem_memcg->score_node);
		spin_lock_init(&oem_memcg->zram_init_lock);

		atomic64_set(&oem_memcg->app_score, -1000);
		atomic_set(&oem_memcg->ub_mem2zram_ratio, 60);
		atomic_set(&oem_memcg->ub_zram2ufs_ratio, 10);
		atomic_set(&oem_memcg->refault_threshold, 50);
		oem_memcg->css = &memcg->css;
	}

}

static void memcg_app_score_update(struct mem_cgroup *target)
{
	struct list_head *pos = NULL;
	unsigned long flags;
	struct oem_mem_cgroup *target_oem_memcg = get_oem_mem_cgroup(target);
	if (unlikely(!target_oem_memcg)) {
		return;
	}

	spin_lock_irqsave(&score_list_lock, flags);
	list_for_each(pos, &score_head) {
		struct oem_mem_cgroup *oem_memcg = list_entry(pos,
				struct oem_mem_cgroup, score_node);
		if (atomic64_read(&oem_memcg->app_score) <
				atomic64_read(&target_oem_memcg->app_score))
			break;
	}
	list_move_tail(&target_oem_memcg->score_node, pos);
	spin_unlock_irqrestore(&score_list_lock, flags);
}

static int mem_cgroup_app_score_write(struct cgroup_subsys_state *css,
				struct cftype *cft, s64 val)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);
	struct oem_mem_cgroup *oem_memcg = get_oem_mem_cgroup(memcg);
	if (!oem_memcg)
		return 0;

	if (val > MAX_APP_SCORE)
		return -EINVAL;

	if (atomic64_read(&oem_memcg->app_score) != val) {
		atomic64_set(&oem_memcg->app_score, val);
		memcg_app_score_update(memcg);
	}

	return 0;
}

static int memcg_total_info_per_app_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = NULL;
	struct oem_mem_cgroup *oem_memcg = NULL;
	struct mem_cgroup_per_node *mz = NULL;
	struct lruvec *lruvec = NULL;
	u64 anon_size;
	u64 zram_compress_size;
	u64 eswap_compress_size;

	while ((memcg = get_next_memcg(memcg))) {
		mz = mem_cgroup_nodeinfo(memcg, 0);
		if (!mz) {
			get_next_memcg_break(memcg);
			return 0;
		}
		oem_memcg = get_oem_mem_cgroup(memcg);
		if (!oem_memcg) {
			get_next_memcg_break(memcg);
			return 0;
		}

		lruvec = &mz->lruvec;
		if (!lruvec) {
			get_next_memcg_break(memcg);
			return 0;
		}

		anon_size = expandmem_lruvec_lru_size(lruvec, LRU_ACTIVE_ANON,
			MAX_NR_ZONES) + expandmem_lruvec_lru_size(lruvec,
			LRU_INACTIVE_ANON, MAX_NR_ZONES);
		zram_compress_size = eswap_read_mcg_stats(memcg,
				MCG_ZRAM_STORED_SZ);
		eswap_compress_size = eswap_read_mcg_stats(memcg,
				MCG_DISK_STORED_SZ);

		anon_size *= (PAGE_SIZE / SZ_1K);
		zram_compress_size /= SZ_1K;
		eswap_compress_size /= SZ_1K;

		seq_printf(m, "%s %d %lld %llu %llu %llu\n", oem_memcg->name, memcg->id.id, oem_memcg->app_score,
			anon_size, zram_compress_size, eswap_compress_size);
	}

	return 0;
}

static ssize_t mem_cgroup_name_write(struct kernfs_open_file *of, char *buf,
				size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	struct oem_mem_cgroup *oem_memcg = NULL;
	const unsigned int buf_max_size = 100;

	buf = strstrip(buf);
	if (nbytes >= buf_max_size)
		return -EINVAL;
	mutex_lock(&reclaim_para_lock);
	if (memcg) {
		oem_memcg = get_oem_mem_cgroup(memcg);
		if (oem_memcg) {
			strcpy(oem_memcg->name, buf);
		}
	}
	mutex_unlock(&reclaim_para_lock);

	return nbytes;
}

static void memcg_eswap_info_show(struct seq_file *m)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	struct oem_mem_cgroup *oem_memcg = NULL;
	struct mem_cgroup_per_node *mz = NULL;
	struct lruvec *lruvec = NULL;
	u64 anon;
	u64 file;
	u64 zram;
	u64 eswap;
	int id = 0;
	s64 score = -1001;

	mz = mem_cgroup_nodeinfo(memcg, 0);
	if (!mz)
		return;

	oem_memcg = get_oem_mem_cgroup(memcg);
	if (!oem_memcg)
		return;

	lruvec = &mz->lruvec;
	if (!lruvec)
		return;

	if(memcg) {
		id = memcg->id.id;
		score = atomic64_read(&oem_memcg->app_score);
	}
	anon = expandmem_lruvec_lru_size(lruvec, LRU_ACTIVE_ANON, MAX_NR_ZONES) +
		expandmem_lruvec_lru_size(lruvec, LRU_INACTIVE_ANON, MAX_NR_ZONES);
	file = expandmem_lruvec_lru_size(lruvec, LRU_ACTIVE_FILE, MAX_NR_ZONES) +
		expandmem_lruvec_lru_size(lruvec, LRU_INACTIVE_FILE, MAX_NR_ZONES);
	zram = eswap_read_mcg_stats(memcg, MCG_ZRAM_PG_SZ);
	eswap = eswap_read_mcg_stats(memcg, MCG_DISK_STORED_PG_SZ);
	anon *= (PAGE_SIZE / SZ_1K);
	file *= (PAGE_SIZE / SZ_1K);
	zram *= (PAGE_SIZE / SZ_1K);
	eswap *= (PAGE_SIZE / SZ_1K);

	seq_printf(m,
		"anon %llu KB\n"
		"file %llu KB\n"
		"zram %llu KB\n"
		"eswap %llu KB\n"
		"mcgid %d\n"
		"score %lld\n",
		anon, file, zram, eswap, id, score);
}

static int memcg_eswap_stat_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = NULL;
	unsigned long swap_out_cnt;
	unsigned long swap_out_size;
	unsigned long page_fault_cnt;

	memcg = mem_cgroup_from_css(seq_css(m));
	swap_out_cnt = eswap_read_mcg_stats(memcg, MCG_SWAPOUT_CNT);
	swap_out_size = eswap_read_mcg_stats(memcg, MCG_SWAPOUT_SZ);
	page_fault_cnt = eswap_read_mcg_stats(memcg, MCG_DISK_FAULT_CNT);

	seq_printf(m, "swapout_total %lu \n", swap_out_cnt);
	seq_printf(m, "swapout_size %lu MB\n", swap_out_size / SZ_1M);
	seq_printf(m, "swapin_total %lu\n", page_fault_cnt);

	memcg_eswap_info_show(m);
	return 0;
}

inline u64 get_zram_wm_ratio_value(void)
{
	return atomic64_read(&zram_wm_ratio);
}

inline u64 get_compress_ratio_value(void)
{
	return atomic64_read(&compress_ratio);
}

inline unsigned int get_avail_buffers_value(void)
{
	return atomic_read(&avail_buffers);
}

inline unsigned int get_min_avail_buffers_value(void)
{
	return atomic_read(&min_avail_buffers);
}

inline unsigned int get_high_avail_buffers_value(void)
{
	return atomic_read(&high_avail_buffers);
}

inline u64 get_zswapd_max_reclaim_size(void)
{
	return atomic_read(&max_reclaim_size);
}

inline u64 get_swap_free_low_value(void)
{
	return atomic64_read(&swap_free_low);
}

inline u64 get_area_anon_refault_threshold_value(void)
{
	return atomic64_read(&area_anon_refault_threshold);
}

inline unsigned long get_anon_refault_snapshot_min_interval_value(void)
{
	return atomic64_read(&anon_refault_snapshot_min_interval);
}
inline u64 get_empty_round_skip_interval_value(void)
{
	return atomic64_read(&empty_round_skip_interval);
}
inline u64 get_max_skip_interval_value(void)
{
	return atomic64_read(&max_skip_interval);
}
inline u64 get_empty_round_check_threshold_value(void)
{
	return atomic64_read(&empty_round_check_threshold);
}

inline u64 get_zram_critical_threshold_value(void)
{
	return atomic64_read(&zram_critical_threshold);
}

static ssize_t avail_buffers_params_write(struct kernfs_open_file *of,
				char *buf, size_t nbytes, loff_t off)
{
	const unsigned int params_num = 4;
	unsigned int avail_buffers_value;
	unsigned int min_avail_buffers_value;
	unsigned int high_avail_buffers_value;
	u64 free_swap_low_value;

	buf = strstrip(buf);

	if (sscanf(buf, "%u %u %u %llu",
		&avail_buffers_value,
		&min_avail_buffers_value,
		&high_avail_buffers_value,
		&free_swap_low_value) != params_num)
		return -EINVAL;
	atomic_set(&avail_buffers, avail_buffers_value);
	atomic_set(&min_avail_buffers, min_avail_buffers_value);
	atomic_set(&high_avail_buffers, high_avail_buffers_value);
	atomic64_set(&swap_free_low, free_swap_low_value << (20 - PAGE_SHIFT));

	if (atomic_read(&min_avail_buffers) == 0)
		set_snapshotd_init_flag(0);
	else
		set_snapshotd_init_flag(1);

	wake_all_zswapd();

	return nbytes;
}

static ssize_t zswapd_max_reclaim_size_write(struct kernfs_open_file *of,
				char *buf, size_t nbytes, loff_t off)
{
	const unsigned int base = 10;
	u32 max_reclaim_size_value;
	int ret;

	buf = strstrip(buf);
	ret = kstrtouint(buf, base, &max_reclaim_size_value);
	if (ret)
		return -EINVAL;

	atomic_set(&max_reclaim_size, max_reclaim_size_value);

	return nbytes;
}

static int area_anon_refault_threshold_write(struct cgroup_subsys_state *css,
				struct cftype *cft, u64 val)
{
	atomic64_set(&area_anon_refault_threshold, val);
	return 0;
}

static int empty_round_skip_interval_write(struct cgroup_subsys_state *css,
				struct cftype *cft, u64 val)
{
	atomic64_set(&empty_round_skip_interval, val);
	return 0;
}

static int max_skip_interval_write(struct cgroup_subsys_state *css,
				struct cftype *cft, u64 val)
{
	atomic64_set(&max_skip_interval, val);
	return 0;
}

static int empty_round_check_threshold_write(struct cgroup_subsys_state *css,
				struct cftype *cft, u64 val)
{
	atomic64_set(&empty_round_check_threshold, val);
	return 0;
}

static int anon_refault_snapshot_min_interval_write(
	struct cgroup_subsys_state *css, struct cftype *cft, u64 val)
{
	atomic64_set(&anon_refault_snapshot_min_interval, val);
	return 0;
}

static int zram_critical_threshold_write(struct cgroup_subsys_state *css,
				struct cftype *cft, u64 val)
{
	atomic64_set(&zram_critical_threshold, val);
	return 0;
}

static ssize_t zswapd_pressure_event_control(struct kernfs_open_file *of,
				char *buf, size_t nbytes, loff_t off)
{
	const unsigned int params_num = 2;
	unsigned int efd;
	unsigned int level;
	struct fd efile;
	int ret;

	buf = strstrip(buf);
	if (sscanf(buf, "%u %u", &efd, &level) != params_num)
		return -EINVAL;

	if (level >= ZSWAPD_PRESSURE_NUM_LEVELS)
		return -EINVAL;

	mutex_lock(&zswapd_pressure_event_lock);
	efile = fdget(efd);
	if (!efile.file) {
		ret = -EBADF;
		goto out;
	}
	zswapd_press_efd[level] = eventfd_ctx_fileget(efile.file);
	if (IS_ERR(zswapd_press_efd[level])) {
		ret = PTR_ERR(zswapd_press_efd[level]);
		goto out_put_efile;
	}
	fdput(efile);
	mutex_unlock(&zswapd_pressure_event_lock);
	return nbytes;

out_put_efile:
	fdput(efile);
out:
	mutex_unlock(&zswapd_pressure_event_lock);

	return ret;
}

void zswapd_pressure_report(enum zswapd_pressure_levels level)
{
	int ret;

	if (zswapd_press_efd[level] == NULL)
		return;

	ret = eventfd_signal(zswapd_press_efd[level], 1);
	if (ret < 0) {
		eswap_print(LEVEL_ERR, "level %u, ret %d ", level, ret);
	}

}

static u64 zswapd_pid_read(struct cgroup_subsys_state *css,
				struct cftype *cft)
{
	return get_zswapd_pid();
}

static ssize_t zswapd_single_memcg_param_write(struct kernfs_open_file *of,
				char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	struct oem_mem_cgroup *oem_memcg = NULL;
	unsigned int ub_mem2zram_ratio;
	unsigned int ub_zram2ufs_ratio;
	unsigned int refault_threshold;
	const unsigned int params_num = 3;

	if (!memcg)
		return nbytes;

	oem_memcg = get_oem_mem_cgroup(memcg);
	if (!oem_memcg) {
		return nbytes;
	}

	buf = strstrip(buf);

	if (sscanf(buf, "%u %u %u",
		&ub_mem2zram_ratio,
		&ub_zram2ufs_ratio,
		&refault_threshold) != params_num)
		return -EINVAL;

	if (ub_mem2zram_ratio > MAX_RATIO || ub_zram2ufs_ratio > MAX_RATIO)
		return -EINVAL;

	atomic_set(&oem_memcg->ub_mem2zram_ratio, ub_mem2zram_ratio);
	atomic_set(&oem_memcg->ub_zram2ufs_ratio, ub_zram2ufs_ratio);
	atomic_set(&oem_memcg->refault_threshold, refault_threshold);

	return nbytes;
}

static int mem_cgroup_zram_wm_ratio_write(struct cgroup_subsys_state *css,
				struct cftype *cft, u64 val)
{
	if (val > MAX_RATIO)
		return -EINVAL;

	atomic64_set(&zram_wm_ratio, val);

	return 0;
}

static int mem_cgroup_compress_ratio_write(struct cgroup_subsys_state *css,
				struct cftype *cft, u64 val)
{
	if (val > MAX_RATIO)
		return -EINVAL;

	atomic64_set(&compress_ratio, val);

	return 0;
}

static int zswapd_presure_show(struct seq_file *m, void *v)
{
	zswapd_status_show(m);
	return 0;
}

static int memcg_active_app_info_list_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = NULL;
	struct oem_mem_cgroup *oem_memcg = NULL;
	struct mem_cgroup_per_node *mz = NULL;
	struct lruvec *lruvec = NULL;
	u64 anon_size;
	u64 zram_size;
	u64 eswap_size;
	u64 total_anon_size = 0;
	u64 total_zram_size = 0;
	u64 total_eswap_size = 0;

	while ((memcg = get_next_memcg(memcg))) {
		s64 score = -1001;
		oem_memcg = get_oem_mem_cgroup(memcg);
		if (!oem_memcg) {
			get_next_memcg_break(memcg);
			return 0;
		}

		mz = mem_cgroup_nodeinfo(memcg, 0);
		if (!mz) {
			get_next_memcg_break(memcg);
			return 0;
		}

		score = atomic64_read(&oem_memcg->app_score);

		lruvec = &mz->lruvec;
		if (!lruvec) {
			get_next_memcg_break(memcg);
			return 0;
		}

		anon_size = expandmem_lruvec_lru_size(lruvec, LRU_ACTIVE_ANON,
				MAX_NR_ZONES) + expandmem_lruvec_lru_size(lruvec,
					LRU_INACTIVE_ANON, MAX_NR_ZONES);
		eswap_size = eswap_read_mcg_stats(memcg,
				MCG_DISK_STORED_PG_SZ);
		zram_size = eswap_read_mcg_stats(memcg,
				MCG_ZRAM_PG_SZ);

		if (anon_size + zram_size + eswap_size == 0)
			continue;

		anon_size *= (PAGE_SIZE / SZ_1K);
		zram_size *= (PAGE_SIZE / SZ_1K);
		eswap_size *= (PAGE_SIZE / SZ_1K);

		total_anon_size += anon_size;
		total_zram_size += zram_size;
		total_eswap_size += eswap_size;

		seq_printf(m, "%s %d %lld %llu %llu %llu %llu\n", oem_memcg->name, memcg->id.id,
			score, anon_size, zram_size, eswap_size,
			oem_memcg->reclaimed_pagefault);
	}

	seq_printf(m, "total_anon_size %llu KB\n", total_anon_size);
	seq_printf(m, "total_zram_size %llu KB\n", total_zram_size);
	seq_printf(m, "total_eswap_size %llu KB\n", total_eswap_size);
	return 0;
}

#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
static int memcg_name_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	struct oem_mem_cgroup *oem_memcg = get_oem_mem_cgroup(memcg);
	if (oem_memcg)
		seq_printf(m, "%s\n", oem_memcg->name);
	return 0;
}

static s64 mem_cgroup_app_score_read(struct cgroup_subsys_state *css,
				struct cftype *cft)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);
	struct oem_mem_cgroup *oem_memcg = get_oem_mem_cgroup(memcg);
	if (!oem_memcg)
		return MAX_APP_SCORE + 1;
	return atomic64_read(&oem_memcg->app_score);
}

static u64 mem_cgroup_zram_wm_ratio_read(struct cgroup_subsys_state *css,
				struct cftype *cft)
{
	return atomic64_read(&zram_wm_ratio);
}

static u64 mem_cgroup_compress_ratio_read(struct cgroup_subsys_state *css,
				struct cftype *cft)
{
	return atomic64_read(&compress_ratio);
}

static int avail_buffers_params_show(struct seq_file *m, void *v)
{
	seq_printf(m, "avail_buffers: %u\n",
		atomic_read(&avail_buffers));
	seq_printf(m, "min_avail_buffers: %u\n",
		atomic_read(&min_avail_buffers));
	seq_printf(m, "high_avail_buffers: %u\n",
		atomic_read(&high_avail_buffers));
	seq_printf(m, "free_swap_low_threshold: %llu\n",
		(atomic64_read(&swap_free_low) >> (20 - PAGE_SHIFT)));

	return 0;
}

static int zswapd_max_reclaim_size_show(struct seq_file *m, void *v)
{
	seq_printf(m, "zswapd_max_reclaim_size: %u\n",
		atomic_read(&max_reclaim_size));
	return 0;
}

static u64 area_anon_refault_threshold_read(struct cgroup_subsys_state *css,
				struct cftype *cft)
{
	return atomic64_read(&area_anon_refault_threshold);
}

static u64 empty_round_skip_interval_read(struct cgroup_subsys_state *css,
				struct cftype *cft)
{
	return atomic64_read(&empty_round_skip_interval);
}

static u64 max_skip_interval_read(struct cgroup_subsys_state *css,
				struct cftype *cft)
{
	return atomic64_read(&max_skip_interval);
}

static u64 empty_round_check_threshold_read(struct cgroup_subsys_state *css,
				struct cftype *cft)
{
	return atomic64_read(&empty_round_check_threshold);
}

static u64 anon_refault_snapshot_min_interval_read(
	struct cgroup_subsys_state *css, struct cftype *cft)
{
	return atomic64_read(&anon_refault_snapshot_min_interval);
}

static int zswapd_single_memcg_param_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	struct oem_mem_cgroup *oem_memcg = get_oem_mem_cgroup(memcg);
	if (!oem_memcg)
		return 0;
	seq_printf(m, "memcg score %lld\n", atomic64_read(&oem_memcg->app_score));
	seq_printf(m, "memcg ub_mem2zram_ratio %u\n",
			atomic_read(&oem_memcg->ub_mem2zram_ratio));
	seq_printf(m, "memcg ub_zram2ufs_ratio %u\n",
			atomic_read(&oem_memcg->ub_zram2ufs_ratio));
	seq_printf(m, "memcg refault_threshold %u\n",
			atomic_read(&oem_memcg->refault_threshold));

	return 0;
}

static u64 zram_critical_threshold_read(struct cgroup_subsys_state *css,
				struct cftype *cft)
{
	return atomic64_read(&zram_critical_threshold);
}

static int memcg_score_list_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = NULL;
	struct oem_mem_cgroup *oem_memcg = NULL;
	u64 zram;
	u64 eswap;

	while ((memcg = get_next_memcg(memcg))) {
		oem_memcg = get_oem_mem_cgroup(memcg);
		if (!oem_memcg) {
			get_next_memcg_break(memcg);
			return 0;
		}
		zram = eswap_read_mcg_stats(memcg, MCG_ZRAM_PG_SZ);
		eswap = eswap_read_mcg_stats(memcg, MCG_DISK_STORED_PG_SZ);
		zram *= (PAGE_SIZE / SZ_1K);
		eswap *= (PAGE_SIZE / SZ_1K);

		seq_printf(m, "%d %lld %lluKB %lluKB %s\n",
			memcg->id.id,
			atomic64_read(&oem_memcg->app_score),
			zram,
			eswap,
			oem_memcg->name);
	}

	return 0;
}

static int expandmem_psi_info_show(struct seq_file *m, void *v)
{
	eswap_psi_show(m);
	/* zswapd info */
	zswapd_status_show(m);

	seq_printf(m, "ufs total write %lld MB\n", atomic64_read(&ufs_write_bytes) >> MB_SHIFT);
	return 0;
}
#endif

static int expandmem_ufs_info_show(struct seq_file *m, void *v)
{
	unsigned long ufs_eswap;
	unsigned long ufs_total;
	ufs_total = atomic64_read(&ufs_write_bytes) >> MB_SHIFT;
	ufs_eswap = eswap_get_reclaimin_bytes() >> MB_SHIFT;
	seq_printf(m, "%lld;%lld\n", ufs_total, ufs_eswap);
	return 0;
}

static struct cftype vendor_mem_cgroup_legacy_files[] = {
	{
		.name = "total_info_per_app",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.seq_show = memcg_total_info_per_app_show,
	},
	{
		.name = "eswap_stat",
		.seq_show = memcg_eswap_stat_show,
	},
	{
		.name = "name",
		.write = mem_cgroup_name_write,
#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
		.seq_show = memcg_name_show,
#endif
	},
	{
		.name = "app_score",
		.write_s64 = mem_cgroup_app_score_write,
#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
		.read_s64 = mem_cgroup_app_score_read,
#endif
	},
	{
		.name = "active_app_info_list",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.seq_show = memcg_active_app_info_list_show,
	},
	{
		.name = "zram_wm_ratio",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_u64 = mem_cgroup_zram_wm_ratio_write,
#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
		.read_u64 = mem_cgroup_zram_wm_ratio_read,
#endif
	},
	{
		.name = "compress_ratio",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_u64 = mem_cgroup_compress_ratio_write,
#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
		.read_u64 = mem_cgroup_compress_ratio_read,
#endif
	},
	{
		.name = "zswapd_pressure_level",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write = zswapd_pressure_event_control,
	},
	{
		.name = "zswapd_pid",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.read_u64 = zswapd_pid_read,
	},
	{
		.name = "avail_buffers",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write = avail_buffers_params_write,
#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
		.seq_show = avail_buffers_params_show,
#endif
	},
	{
		.name = "zswapd_max_reclaim_size",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write = zswapd_max_reclaim_size_write,
#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
		.seq_show = zswapd_max_reclaim_size_show,
#endif
	},
	{
		.name = "area_anon_refault_threshold",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_u64 = area_anon_refault_threshold_write,
#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
		.read_u64 = area_anon_refault_threshold_read,
#endif
	},
	{
		.name = "empty_round_skip_interval",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_u64 = empty_round_skip_interval_write,
#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
		.read_u64 = empty_round_skip_interval_read,
#endif
	},
	{
		.name = "max_skip_interval",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_u64 = max_skip_interval_write,
#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
		.read_u64 = max_skip_interval_read,
#endif
	},
	{
		.name = "empty_round_check_threshold",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_u64 = empty_round_check_threshold_write,
#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
		.read_u64 = empty_round_check_threshold_read,
#endif
	},
	{
		.name = "anon_refault_snapshot_min_interval",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_u64 = anon_refault_snapshot_min_interval_write,
#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
		.read_u64 = anon_refault_snapshot_min_interval_read,
#endif
	},
	{
		.name = "zswapd_single_memcg_param",
		.write = zswapd_single_memcg_param_write,
#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
		.seq_show = zswapd_single_memcg_param_show,
#endif
	},
	{
		.name = "zswapd_presure_show",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.seq_show = zswapd_presure_show,
	},
	{
		.name = "zram_critical_threshold",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_u64 = zram_critical_threshold_write,
#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
		.read_u64 = zram_critical_threshold_read,
#endif
	},
#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
	{
		.name = "score_list",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.seq_show = memcg_score_list_show,
	},
	{
		.name = "expandmem_psi_info",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.seq_show = expandmem_psi_info_show,
	},
#endif
	{
		.name = "expandmem_ufs_info",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.seq_show = expandmem_ufs_info_show,
	},
	{ },	/* terminate */
};

static void trace_mem_cgroup_alloc(void *unused, struct mem_cgroup *memcg)
{
	eswap_print(LEVEL_ERR, "android_vh_mem_cgroup_alloc hook\n");
	oem_mem_cgroup_init(memcg);
}

static void trace_mem_cgroup_free(void *unused, struct mem_cgroup *memcg)
{
	struct oem_mem_cgroup *oem_memcg;
	if (unlikely(!memcg)) {
		return;
	}

	oem_memcg = get_oem_mem_cgroup(memcg);
	if (unlikely(!oem_memcg)) {
		return;
	}

	kfree(oem_memcg);
	memcg->android_oem_data1 = 0;
}

static void trace_mem_cgroup_css_online(void *unused, struct cgroup_subsys_state *css, struct mem_cgroup *memcg)
{
	eswap_print(LEVEL_ERR, "android_vh_mem_cgroup_css_offline hook\n");
	memcg_app_score_update(memcg);
	css_get(css);
}

static void trace_mem_cgroup_css_offline(void *unused, struct cgroup_subsys_state *css, struct mem_cgroup *memcg)
{
	unsigned long flags;
	struct oem_mem_cgroup *oem_memcg;
	if (unlikely(!memcg)) {
		return;
	}

	oem_memcg = get_oem_mem_cgroup(memcg);

	spin_lock_irqsave(&score_list_lock, flags);
	list_del_init(&oem_memcg->score_node);
	spin_unlock_irqrestore(&score_list_lock, flags);
	css_put(css);
}

static void trace_mem_cgroup_id_remove(void *unused, struct mem_cgroup *memcg)
{
	eswap_mem_cgroup_remove(memcg);
}

static void trace_rmqueue(void *unused, struct zone *preferred_zone, struct zone *zone,
               unsigned int order, gfp_t gfp_flags,
               unsigned int alloc_flags, int migratetype)
{
	if (gfp_flags & __GFP_KSWAPD_RECLAIM) {
		wake_all_zswapd();
	}
}

int expandmem_mem_vendor_hooks_init(void)
{
	int ret;
	ret = register_trace_android_vh_rmqueue(trace_rmqueue, NULL);
	if (ret) {
		eswap_print(LEVEL_ERR, "Failed to register android_vh_rmqueue hook\n");
		goto err_out;
	}

	ret = register_trace_android_vh_mem_cgroup_alloc(trace_mem_cgroup_alloc, NULL);
	if (ret) {
		eswap_print(LEVEL_ERR, "Failed to register android_vh_mem_cgroup_alloc hook\n");
		goto err_unregister_rmqueue;
	}

	ret = register_trace_android_vh_mem_cgroup_free(trace_mem_cgroup_free, NULL);
	if (ret) {
		eswap_print(LEVEL_ERR, "Failed to register android_vh_mem_cgroup_free hook\n");
		goto err_unregister_cgroup_alloc;
	}

	ret = register_trace_android_vh_mem_cgroup_css_online(trace_mem_cgroup_css_online, NULL);
	if (ret) {
		eswap_print(LEVEL_ERR, "Failed to register android_vh_mem_cgroup_css_online hook\n");
		goto err_unregister_cgroup_free;
	}

	ret = register_trace_android_vh_mem_cgroup_css_offline(trace_mem_cgroup_css_offline, NULL);
	if (ret) {
		eswap_print(LEVEL_ERR, "Failed to register android_vh_mem_cgroup_css_offline hook\n");
		goto err_unregister_css_online;
	}

	ret = register_trace_android_vh_mem_cgroup_id_remove(trace_mem_cgroup_id_remove, NULL);
	if (ret) {
		eswap_print(LEVEL_ERR, "Failed to register android_vh_mem_cgroup_css_offline hook\n");
		goto err_unregister_css_offline;
	}

	eswap_zram_vendor_hooks_init();
	return 0;

err_unregister_css_offline:
	unregister_trace_android_vh_mem_cgroup_css_offline(trace_mem_cgroup_css_offline, NULL);
err_unregister_css_online:
	unregister_trace_android_vh_mem_cgroup_css_online(trace_mem_cgroup_css_online, NULL);
err_unregister_cgroup_free:
	unregister_trace_android_vh_mem_cgroup_free(trace_mem_cgroup_free, NULL);
err_unregister_cgroup_alloc:
	unregister_trace_android_vh_mem_cgroup_alloc(trace_mem_cgroup_alloc, NULL);
err_unregister_rmqueue:
	unregister_trace_android_vh_rmqueue(trace_rmqueue, NULL);
err_out:
	return ret;
}

int expandmem_mem_vendor_hooks_remove(void)
{
	/* Reset all initialized global variables and unregister callbacks. */
	eswap_zram_vendor_hooks_remove();
	unregister_trace_android_vh_rmqueue(trace_rmqueue, NULL);
	unregister_trace_android_vh_mem_cgroup_alloc(trace_mem_cgroup_alloc, NULL);
	unregister_trace_android_vh_mem_cgroup_free(trace_mem_cgroup_free, NULL);
	unregister_trace_android_vh_mem_cgroup_css_online(trace_mem_cgroup_css_online, NULL);
	unregister_trace_android_vh_mem_cgroup_css_offline(trace_mem_cgroup_css_offline, NULL);
	unregister_trace_android_vh_mem_cgroup_id_remove(trace_mem_cgroup_id_remove, NULL);
	return 0;
}

extern struct cgroup_subsys memory_cgrp_subsys;
void expandmem_mem_vendor_cgroup_init(void)
{
	struct mem_cgroup *memcg;
	unsigned short initial_memcg_num = 3;
	unsigned short i;

	WARN_ON(cgroup_add_legacy_cftypes(&memory_cgrp_subsys, vendor_mem_cgroup_legacy_files));

	for (i = 1; i <= initial_memcg_num; i++) {
		memcg = mem_cgroup_from_id(i);
		oem_mem_cgroup_init(memcg);
		memcg_app_score_update(memcg);
	}
}

static void eswap_ufs_compl_command(struct ufs_hba *hba, struct ufshcd_lrb *lrbp)
{
	struct scsi_cmnd *cmd;
	int transfer_len = 0;
	u8 opcode = 0;

	if (lrbp) {
		cmd = lrbp->cmd;
		if (cmd) {
			opcode = cmd->cmnd[0];
			if ((opcode == WRITE_6)
					|| (opcode == WRITE_10)
					|| (opcode == WRITE_12)
					|| (opcode == WRITE_16)
					|| (opcode == WRITE_32)) {
				/*
				* only trace write commands
				*/
				transfer_len = be32_to_cpu(lrbp->ucd_req_ptr->sc.exp_data_transfer_len);
				if (transfer_len) {
					atomic64_add(transfer_len, &ufs_write_bytes);
				}
			}
		}
	}
}

static void trace_ufs_compl_command(void *unused, struct ufs_hba *hba, struct ufshcd_lrb *lrbp)
{
	eswap_ufs_compl_command(hba, lrbp);
}

int expandmem_ufs_vendor_hooks_init(void)
{
	int ret;

	ret = register_trace_android_vh_ufs_compl_command(trace_ufs_compl_command, NULL);
	if (ret) {
		eswap_print(LEVEL_ERR, "Failed to register android_vh_ufs_compl_command\n");
		return ret;
	}
	return 0;
}

int expandmem_ufs_vendor_hooks_remove(void)
{
	/* Reset all initialized global variables and unregister callbacks. */
	unregister_trace_android_vh_ufs_compl_command(trace_ufs_compl_command, NULL);
	return 0;
}

#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
static void expandmem_monitor_work_func(struct work_struct *work)
{
	struct timespec64 boottime;
	struct timespec64 ts;
	struct rtc_time tmboot;
	struct rtc_time tm;
	unsigned long ufs_total;
	unsigned long ufs_eswap;
	int ufs_radio;

	if (!eswap_get_enable())
        return;

	if (expandmem_monitor_wq == NULL)
		return;


	getboottime64(&boottime);
	ktime_get_real_ts64(&ts);

	rtc_time64_to_tm((unsigned long long)boottime.tv_sec - (sys_tz.tz_minuteswest * 60), &tmboot);
	rtc_time64_to_tm(ts.tv_sec - (sys_tz.tz_minuteswest * 60), &tm);

	ufs_total = atomic64_read(&ufs_write_bytes) >> MB_SHIFT;
	ufs_eswap = eswap_get_reclaimin_bytes() >> MB_SHIFT;
	ufs_radio = ufs_eswap * 100 / ufs_total;
	printk("[EMEM][UFS] statistic (%04d-%02d-%02d %02d:%02d:%02d ~ %04d-%02d-%02d %02d:%02d:%02d) total %lldMB emem %lldMB ratio %d\n",
			tmboot.tm_year + 1900, tmboot.tm_mon + 1, tmboot.tm_mday, tmboot.tm_hour, tmboot.tm_min, tmboot.tm_sec,
			tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
			ufs_total, ufs_eswap, ufs_radio);

	queue_delayed_work(expandmem_monitor_wq, &expandmem_monitor_task, msecs_to_jiffies(EMEM_UFS_OBSERVE_DELAY_TIME));
}

void expandmem_monitor_init(void)
{
	if (task_initialized == 1)
		return;

	if (expandmem_monitor_wq == NULL) {
		expandmem_monitor_wq = alloc_workqueue("expandmem_monitor_wq", WQ_UNBOUND | WQ_FREEZABLE, 0);
	}

	if (!expandmem_monitor_wq)
		return;

	INIT_DELAYED_WORK(&expandmem_monitor_task,
			expandmem_monitor_work_func);
	task_initialized = 1;
	queue_delayed_work(expandmem_monitor_wq, &expandmem_monitor_task,
			msecs_to_jiffies(EMEM_UFS_OBSERVE_DELAY_TIME));
}
#endif
