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
#include <linux/blkdev.h>
#include <linux/spinlock.h>

#include "../zram_drv.h"
#include "expandmem.h"
#include "eswap.h"
#include "eswap_common.h"
#include "eswap_area.h"

#define ESWAP_WDT_EXPIRE_DEFAULT 3600
/* Warning.Consumed 80% of reserved blocks */
#define PRE_EOL_INFO_OVER_VAL 2
/* 70% - 80% device life time used */
#define LIFE_TIME_EST_OVER_VAL 8

struct eswap_settings {
	atomic_t enable;
	atomic_t reclaim_in_enable;
	int log_level;
	atomic_t watchdog_protect;
	struct timer_list wdt_timer;
	unsigned long wdt_expire_s;
	struct eswap_stat *stat;
	struct workqueue_struct *reclaim_wq;
	struct zram *zram;
};
static struct eswap_settings global_settings;

void *eswap_malloc(size_t size, bool fast, bool nofail)
{
	void *mem = NULL;

	if (likely(fast)) {
		mem = kzalloc(size, GFP_ATOMIC);
		if (likely(mem || !nofail))
			return mem;
	}

	mem = kzalloc(size, GFP_NOIO);

	return mem;
}

void eswap_free(const void *mem)
{
	kfree(mem);
}

void eswap_page_recycle(struct page *page,
				struct eswap_page_pool *pool)
{
	if (pool) {
		spin_lock(&pool->page_pool_lock);
		list_add(&page->lru, &pool->page_pool_list);
		spin_unlock(&pool->page_pool_lock);
	} else {
		__free_page(page);
	}
}

struct page *eswap_alloc_page(
		struct eswap_page_pool *pool, gfp_t gfp,
		bool fast, bool nofail)
{
	struct page *page = NULL;

	if (pool) {
		spin_lock(&pool->page_pool_lock);
		if (!list_empty(&pool->page_pool_list)) {
			page = list_first_entry(
				&pool->page_pool_list,
				struct page, lru);
			list_del(&page->lru);
		}
		spin_unlock(&pool->page_pool_lock);
	}

	if (!page) {
		if (fast) {
			page = alloc_page(GFP_ATOMIC);
			if (likely(page))
				goto out;
		}
		if (nofail)
			page = alloc_page(GFP_NOIO);
		else
			page = alloc_page(gfp);
	}
out:
	return page;
}

struct eswap_stat *eswap_get_stat_obj(void)
{
	return global_settings.stat;
}

struct workqueue_struct *eswap_get_reclaim_workqueue(void)
{
	return global_settings.reclaim_wq;
}

static void eswap_set_wdt_expire(unsigned long expire)
{
	global_settings.wdt_expire_s = expire;
}

static void eswap_set_wdt_enable(bool en)
{
	atomic_set(&global_settings.watchdog_protect, en ? 1 : 0);
}

static bool eswap_get_wdt_enable(void)
{
	return !!atomic_read(&global_settings.watchdog_protect);
}

bool eswap_get_reclaim_in_enable(void)
{
	return !!atomic_read(&global_settings.reclaim_in_enable);
}

static void eswap_set_reclaim_in_disable(void)
{
	atomic_set(&global_settings.reclaim_in_enable, false);
}

static void eswap_set_reclaim_in_enable(bool en)
{
	del_timer_sync(&global_settings.wdt_timer);
	atomic_set(&global_settings.reclaim_in_enable, en ? 1 : 0);
	if (en && eswap_get_wdt_enable())
		mod_timer(&global_settings.wdt_timer,
			jiffies + msecs_to_jiffies(
			global_settings.wdt_expire_s * MSEC_PER_SEC));
}

bool eswap_get_enable(void)
{
	return !!atomic_read(&global_settings.enable);
}

void eswap_set_enable(bool enable)
{
	eswap_set_reclaim_in_enable(enable);
	if (!eswap_get_enable()) {
		atomic_set(&global_settings.enable, enable ? 1 : 0);
	}
}

static void eswap_wdt_timeout(struct timer_list *data)
{
	eswap_print(LEVEL_ERR, "wdt is triggered! reclaim in is disabled!\n");
	eswap_set_reclaim_in_disable();
}

static void eswap_stat_init(struct eswap_stat *stat)
{
	atomic64_set(&stat->reclaimin_cnt, 0);
	atomic64_set(&stat->reclaimin_bytes, 0);
	atomic64_set(&stat->reclaimin_pages, 0);
	atomic64_set(&stat->reclaimin_infight, 0);
	atomic64_set(&stat->faultout_cnt, 0);
	atomic64_set(&stat->faultout_bytes, 0);
	atomic64_set(&stat->faultout_pages, 0);
	atomic64_set(&stat->fault_check_cnt, 0);
	atomic64_set(&stat->eswap_fault_cnt, 0);
	atomic64_set(&stat->reout_pages, 0);
	atomic64_set(&stat->reout_bytes, 0);
	atomic64_set(&stat->zram_stored_pages, 0);
	atomic64_set(&stat->zram_stored_size, 0);
	atomic64_set(&stat->stored_pages, 0);
	atomic64_set(&stat->stored_size, 0);
	atomic64_set(&stat->notify_free, 0);
	atomic64_set(&stat->frag_cnt, 0);
	atomic64_set(&stat->mcg_cnt, 0);
	atomic64_set(&stat->ext_cnt, 0);
}

static bool eswap_setting_init(struct zram *zram)
{
	struct oem_zram_data *oem_zram = NULL;
	if (unlikely(global_settings.stat))
		return false;

	global_settings.log_level = LEVEL_ERR;
	eswap_set_enable(false);

	eswap_set_wdt_enable(false);
	init_timer_key(&global_settings.wdt_timer, eswap_wdt_timeout, 0, NULL, NULL);
	eswap_set_wdt_expire(ESWAP_WDT_EXPIRE_DEFAULT);

	zram->android_oem_data1 = (u64)eswap_malloc(
				sizeof(struct oem_zram_data), false, true);
	oem_zram = (struct oem_zram_data *)zram->android_oem_data1;
	if (unlikely(!oem_zram)) {
		eswap_print(LEVEL_ERR, "zram data allocation failed\n");
		goto err_out;
	}
	global_settings.zram = zram;

	global_settings.stat = eswap_malloc(
				sizeof(struct eswap_stat), false, true);
	if (unlikely(!global_settings.stat)) {
		eswap_print(LEVEL_ERR, "global stat allocation failed\n");
		goto err_release_zram;
	}
	eswap_stat_init(global_settings.stat);

	global_settings.reclaim_wq = alloc_workqueue("eswap_reclaim",
							WQ_CPU_INTENSIVE, 0);
	if (unlikely(!global_settings.reclaim_wq)) {
		eswap_print(LEVEL_ERR, "reclaim workqueue allocation failed\n");
		goto err_release_stat;
	}

	return true;

err_release_stat:
	eswap_free(global_settings.stat);
	global_settings.stat = NULL;
err_release_zram:
	eswap_free((struct oem_zram_data *)zram->android_oem_data1);
	zram->android_oem_data1 = 0;
	global_settings.zram = NULL;
err_out:
	return false;
}

static void eswap_setting_deinit(void)
{
	destroy_workqueue(global_settings.reclaim_wq);
	eswap_free(global_settings.stat);
	global_settings.stat = NULL;

	eswap_free((struct oem_zram_data *)global_settings.zram->android_oem_data1);
	global_settings.zram->android_oem_data1 = 0;
	global_settings.zram = NULL;
}

static int eswap_backing_dev_init(struct zram *zram)
{
	struct file *backing_dev = NULL;
	struct inode *inode = NULL;
	unsigned long nr_pages;
	struct block_device *bdev = NULL;
	int err;
	struct oem_zram_data *oem_zram = get_oem_zram(zram);

    const char *file_name = "/dev/block/by-name/expandmem";

	if (!oem_zram) {
		eswap_print(LEVEL_ERR, "zram data is null\n");
		err = -ENXIO;
		goto out;
	}

	down_write(&zram->init_lock);
	backing_dev = filp_open_block(file_name, O_RDWR|O_LARGEFILE, 0);
	if (unlikely(IS_ERR(backing_dev))) {
		err = PTR_ERR(backing_dev);
		eswap_print(LEVEL_ERR, "open the storage device %s failed, err = %lld\n",
					file_name, err);
		backing_dev = NULL;
		goto out;
	}

	inode = backing_dev->f_mapping->host;
	/* Support only block device in this moment */
	if (unlikely(!S_ISBLK(inode->i_mode))) {
		eswap_print(LEVEL_ERR, "%s isn't a blk device\n", file_name);
		err = -ENOTBLK;
		goto out;
	}

	bdev = blkdev_get_by_dev(inode->i_rdev, FMODE_READ | FMODE_WRITE | FMODE_EXCL, zram);
	if (IS_ERR(bdev)) {
		err = PTR_ERR(bdev);
		eswap_print(LEVEL_ERR, "%s blkdev_get failed, err = %d\n",
					file_name, err);
		bdev = NULL;
		goto out;
	}

	nr_pages = (unsigned long)i_size_read(inode) >> PAGE_SHIFT;

	err = set_blocksize(bdev, PAGE_SIZE);
	if (unlikely(err)) {
		eswap_print(LEVEL_ERR, "%s set blocksize failed, err = %d\n", file_name, err);
		goto out;
	}

	oem_zram->bdev = bdev;
	zram->backing_dev = backing_dev;
	oem_zram->nr_pages = nr_pages;
	up_write(&zram->init_lock);

	eswap_print(LEVEL_DEBUG, "setup backing device %s\n", file_name);

	return 0;
out:
	if (bdev) {
		blkdev_put(bdev, FMODE_READ | FMODE_WRITE | FMODE_EXCL);
	}

	if (backing_dev) {
		filp_close(backing_dev, NULL);
	}

	up_write(&zram->init_lock);

	return err;
}

/*
 * This will be called when set the ZRAM size
 */
void eswap_init(struct zram *zram)
{
	int ret;

	if (!eswap_setting_init(zram)) {
		return;
	}

	ret = eswap_backing_dev_init(zram);
	if (unlikely(ret)) {
		eswap_setting_deinit();
		return;
	}
}

static int eswap_set_enable_init(bool en)
{
	int ret;

	if (eswap_get_enable() || !en)
		return 0;

	if (!global_settings.stat) {
		eswap_print(LEVEL_ERR, "global_settings.stat is null\n");
		return -EINVAL;
	}

	ret = eswap_manager_init(global_settings.zram);
	if (unlikely(ret)) {
		eswap_print(LEVEL_ERR, "init manager failed, %d\n", ret);
		return -EINVAL;
	}

	ret = eswap_schedule_init();
	if (unlikely(ret)) {
		eswap_print(LEVEL_ERR, "init schedule failed, %d\n", ret);
		eswap_manager_deinit(global_settings.zram);
		return -EINVAL;
	}

	return 0;
}

extern int eswap_ufshcd_get_health_info(struct block_device *bdev,
		u8 *pre_eol_info, u8 *life_time_est_a, u8 *life_time_est_b);
static int eswap_health_check(void)
{
	int ret;
	u8 pre_eol_info = PRE_EOL_INFO_OVER_VAL;
	u8 life_time_est_a = LIFE_TIME_EST_OVER_VAL;
	u8 life_time_est_b = LIFE_TIME_EST_OVER_VAL;
	struct oem_zram_data *oem_data = get_oem_zram(global_settings.zram);

	if (unlikely(!oem_data)) {
		eswap_print(LEVEL_ERR, "zram data is null\n");
		return -EFAULT;
	}

	if (unlikely(!oem_data->bdev)) {
		eswap_print(LEVEL_ERR, "device is null\n");
		return -EFAULT;
	}

	ret = eswap_ufshcd_get_health_info(oem_data->bdev, &pre_eol_info,
			&life_time_est_a, &life_time_est_b);
	if (ret) {
		eswap_print(LEVEL_ERR, "query health err %d\n", ret);
		return ret;
	}

	if ((pre_eol_info >= PRE_EOL_INFO_OVER_VAL) ||
			(life_time_est_a >= LIFE_TIME_EST_OVER_VAL) ||
			(life_time_est_b >= LIFE_TIME_EST_OVER_VAL)) {
		eswap_print(LEVEL_ERR, "over life time uesd %u %u %u\n",
				pre_eol_info, life_time_est_a, life_time_est_b);
		return -EPERM;
	}

	eswap_print(LEVEL_DEBUG, "life time uesd %u %u %u\n",
			pre_eol_info, life_time_est_a, life_time_est_b);

	return 0;
}

ssize_t eswap_enable_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t len)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (unlikely(ret)) {
		eswap_print(LEVEL_ERR, "val is error\n");
		return -EINVAL;
	}

	/* eswap must be close when over 70% life time uesd */
	if (!!val) {
		if (eswap_health_check()) {
			val = false;
		}
	}

	if (eswap_set_enable_init(!!val))
		return -EINVAL;

	eswap_set_enable(!!val);

	return len;
}

ssize_t eswap_enable_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n",
		eswap_get_enable() ? "1" : "0");
}

ssize_t eswap_reclaimin_enable_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t len)
{
	int ret;
	unsigned long val;

	if (!eswap_get_enable())
		return len;

	ret = kstrtoul(buf, 0, &val);
	if (unlikely(ret)) {
		eswap_print(LEVEL_ERR, "val is error\n");
		return -EINVAL;
	}

    eswap_set_reclaim_in_enable(!!val);
	return len;
}

ssize_t eswap_reclaimin_enable_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n",
		eswap_get_reclaim_in_enable() ? "1" : "0");
}

ssize_t eswap_wdt_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t len)
{
	int ret;
	unsigned long val;

	if (!eswap_get_enable())
		return len;

	ret = kstrtoul(buf, 0, &val);
	if (unlikely(ret)) {
		eswap_print(LEVEL_ERR, "val is error\n");
		return -EINVAL;
	}

	if (val) {
		eswap_set_wdt_expire(val);
	}
	eswap_set_wdt_enable(!!val);
	return len;
}

ssize_t eswap_wdt_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "watchdog enable %s, watchdog expire(s) %lu\n",
		eswap_get_wdt_enable() ? "1" : "0",
		global_settings.wdt_expire_s);
}

int eswap_get_loglevel(void)
{
	return global_settings.log_level;
}

static void eswap_set_loglevel(int level)
{
	global_settings.log_level = level;
}

ssize_t eswap_loglevel_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t len)
{
	int ret;
	unsigned long val;

	if (!eswap_get_enable())
		return len;

	ret = kstrtoul(buf, 0, &val);
	if (unlikely(ret)) {
		eswap_print(LEVEL_ERR, "val is error\n");
		return -EINVAL;
	}

	eswap_set_loglevel((int)val);
	return len;
}

ssize_t eswap_loglevel_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n",
		eswap_get_loglevel());
}

