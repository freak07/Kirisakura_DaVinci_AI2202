/*
 * Expanded RAM block device
 * Description: expanded memory implement
 *
 * Released under the terms of GNU General Public License Version 2.0
 *
 */

#ifndef _EXPANDMEM_VENDOR_HOOKS_H_
#define _EXPANDMEM_VENDOR_HOOKS_H_

enum zswapd_pressure_levels {
	ZSWAPD_PRESSURE_LOW = 0,
	ZSWAPD_PRESSURE_MEDIUM,
	ZSWAPD_PRESSURE_CRITICAL,
	ZSWAPD_PRESSURE_NUM_LEVELS,
};

int expandmem_mem_vendor_hooks_init(void);
int expandmem_mem_vendor_hooks_remove(void);
void expandmem_mem_vendor_cgroup_init(void);

inline u64 get_swap_free_low_value(void);
inline unsigned int get_avail_buffers_value(void);
inline unsigned int get_min_avail_buffers_value(void);
inline unsigned int get_high_avail_buffers_value(void);
inline u64 get_zram_critical_threshold_value(void);
inline u64 get_zram_wm_ratio_value(void);
inline u64 get_compress_ratio_value(void);
inline u64 get_area_anon_refault_threshold_value(void);
inline unsigned long get_anon_refault_snapshot_min_interval_value(void);
inline u64 get_empty_round_skip_interval_value(void);
inline u64 get_max_skip_interval_value(void);
inline u64 get_empty_round_check_threshold_value(void);
inline u64 get_zswapd_max_reclaim_size(void);
void zswapd_pressure_report(enum zswapd_pressure_levels level);

int expandmem_ufs_vendor_hooks_init(void);
int expandmem_ufs_vendor_hooks_remove(void);

#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
void expandmem_monitor_init(void);
#endif

#endif
