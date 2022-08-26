/*
 * Expanded RAM block device
 * Description: expanded memory implement
 *
 * Released under the terms of GNU General Public License Version 2.0
 *
 */

#ifndef _ESWAP_H_
#define _ESWAP_H_

enum {
	LEVEL_ERR = 0,
	LEVEL_WARN,
	LEVEL_INFO,
	LEVEL_DEBUG,
	LEVEL_MAX
};

static inline void pr_none(void) {}
#define cur_lvl()	eswap_get_loglevel()
#define eswap_print(l, f, ...) \
	(l <= cur_lvl() ? pr_err("<%s>:"f, __func__, ##__VA_ARGS__) : pr_none())

enum eswap_mcg_member {
	MCG_ZRAM_STORED_SZ = 0,
	MCG_ZRAM_PG_SZ,
	MCG_DISK_STORED_SZ,
	MCG_DISK_STORED_PG_SZ,
	MCG_ANON_FAULT_CNT,
	MCG_DISK_FAULT_CNT,
	MCG_SWAPOUT_CNT,
	MCG_SWAPOUT_SZ,
};

bool eswap_get_enable(void);
int eswap_get_loglevel(void);
void eswap_zram_vendor_hooks_init(void);
void eswap_zram_vendor_hooks_remove(void);

bool eswap_reclaim_work_running(void);
unsigned long eswap_reclaim_in(unsigned long size);
void eswap_mem_cgroup_remove(struct mem_cgroup *memcg);

u64 eswap_read_mcg_stats(struct mem_cgroup *mcg,
				enum eswap_mcg_member mcg_member);
unsigned long eswap_get_zram_used_pages(void);
u64 eswap_get_zram_pagefault(void);

ssize_t eswap_enable_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t len);
ssize_t eswap_enable_show(struct device *dev,
	struct device_attribute *attr, char *buf);
ssize_t eswap_reclaimin_enable_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t len);
ssize_t eswap_reclaimin_enable_show(struct device *dev,
	struct device_attribute *attr, char *buf);
ssize_t eswap_loglevel_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t len);
ssize_t eswap_loglevel_show(struct device *dev,
	struct device_attribute *attr, char *buf);
ssize_t eswap_wdt_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t len);
ssize_t eswap_wdt_show(struct device *dev,
	struct device_attribute *attr, char *buf);

unsigned long eswap_get_reclaimin_bytes(void);
#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
void eswap_psi_show(struct seq_file *m);
#else
static inline void eswap_psi_show(struct seq_file *m) {}
#endif
#endif
