/*
 * Expanded RAM block device
 * Description: expanded memory implement
 *
 * Released under the terms of GNU General Public License Version 2.0
 *
 */

#ifndef _ESWAP_COMMON_H_
#define _ESWAP_COMMON_H_
#include <linux/zsmalloc.h>
#include <linux/sched.h>
#include <linux/ktime.h>
#include <linux/memcontrol.h>

#define MIN_RECLAIM_ZRAM_SZ	(1024 * 1024)

#define EXTENT_SHIFT		14
#define EXTENT_SIZE		(1UL << EXTENT_SHIFT)
#define EXTENT_PG_CNT		(EXTENT_SIZE >> PAGE_SHIFT)
#define EXTENT_SECTOR_SIZE	(EXTENT_PG_CNT << 3)

struct oem_zram_data {
	struct block_device *bdev;
    unsigned long nr_pages;
	struct eswap_area *area;
};

enum eswap_scenario {
	ESWAP_RECLAIM_IN = 0,
	ESWAP_FAULT_OUT,
	ESWAP_SCENARIO_BUTT
};

struct eswap_lat_stat {
	atomic64_t total_lat;
	atomic64_t max_lat;
	atomic64_t timeout_cnt;
};

struct eswap_stat {
	atomic64_t reclaimin_cnt;
	atomic64_t reclaimin_bytes;
	atomic64_t reclaimin_pages;
	atomic64_t reclaimin_infight;
	atomic64_t faultout_cnt;
	atomic64_t faultout_bytes;
	atomic64_t faultout_pages;
	atomic64_t fault_check_cnt;
	atomic64_t eswap_fault_cnt;
	atomic64_t reout_pages;
	atomic64_t reout_bytes;
	atomic64_t zram_stored_pages;
	atomic64_t zram_stored_size;
	atomic64_t stored_pages;
	atomic64_t stored_size;
	atomic64_t notify_free;
	atomic64_t frag_cnt;
	atomic64_t mcg_cnt;
	atomic64_t ext_cnt;
};

struct eswap_page_pool {
	struct list_head page_pool_list;
	spinlock_t page_pool_lock;
};

struct eswap_buffer {
	struct zram *zram;
	struct eswap_page_pool *pool;
	struct page **dest_pages;
};

struct eswap_entry {
	int ext_id;
	sector_t addr;
	struct page **dest_pages;
	int pages_sz;
	struct list_head list;
	void *private;
	void *manager_private;
};

struct eswap_io {
	struct block_device *bdev;
	enum eswap_scenario scenario;
	void (*done_callback)(struct eswap_entry *, int);
	void (*complete_notify)(void *);
	void *private;
};

void eswap_init(struct zram *zram);
void *eswap_malloc(size_t size, bool fast, bool nofail);
void eswap_free(const void *mem);
struct eswap_stat *eswap_get_stat_obj(void);
bool eswap_get_reclaim_in_enable(void);
struct workqueue_struct *eswap_get_reclaim_workqueue(void);
void eswap_page_recycle(struct page *page,
				struct eswap_page_pool *pool);
struct page *eswap_alloc_page(
		struct eswap_page_pool *pool, gfp_t gfp,
		bool fast, bool nofail);

bool zram_test_overwrite(struct zram *zram, u32 index, int ext_id);
int eswap_find_extent_by_idx(
	unsigned long eswapentry, struct eswap_buffer *buf, void **private);
void eswap_manager_memcg_init(struct mem_cgroup *mcg, struct zram *zram);
void eswap_manager_memcg_deinit(struct mem_cgroup *mcg);
void eswap_extent_exception(enum eswap_scenario scenario,
					void *private);
void eswap_extent_destroy(void *private, enum eswap_scenario scenario);
void eswap_extent_register(void *private);
void eswap_extent_objs_del(struct zram *zram, u32 index);
unsigned long eswap_extent_create(struct mem_cgroup *mcg,
				      int *ext_id,
				      struct eswap_buffer *buf,
				      void **private);
void eswap_manager_deinit(struct zram *zram);
int eswap_manager_init(struct zram *zram);

int eswap_plug_finish(void *io_handler);
int eswap_read_extent(void *io_handler,
				struct eswap_entry *io_entry);
int eswap_write_extent(void *io_handler,
	struct eswap_entry *io_entry);
void *eswap_plug_start(struct eswap_io *io_para);
int eswap_schedule_init(void);
struct oem_zram_data *get_oem_zram(struct zram *zram);
#endif
