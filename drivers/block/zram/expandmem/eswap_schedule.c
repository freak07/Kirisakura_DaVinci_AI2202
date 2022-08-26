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
#include <linux/kref.h>
#include <linux/bio.h>
#include <linux/seq_file.h>
#include <linux/sched/task.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/random.h>
#include <linux/delay.h>
#include <linux/zsmalloc.h>

#include "eswap_common.h"
#include "eswap.h"

#define ESWAP_KEY_SIZE 64
#define ESWAP_MAX_INFILGHT_NUM 256

#define ESWAP_SECTOR_SHIFT 9
#define ESWAP_PAGE_SIZE_SECTOR (PAGE_SIZE >> ESWAP_SECTOR_SHIFT)

#define ESWAP_READ_TIME 10
#define ESWAP_WRITE_TIME 100
#define ESWAP_MAX_FAULTOUT_TIMEOUT (5 * 1000)
#define ESWAP_FAULTOUT_TIMEOUT 800

static u8 eswap_io_key[ESWAP_KEY_SIZE];
static struct workqueue_struct *eswap_proc_read_workqueue;
static struct workqueue_struct *eswap_proc_write_workqueue;
bool eswap_schedule_init_flag;

struct eswap_segment_time {
	ktime_t submit_bio;
	ktime_t end_io;
};

struct eswap_segment {
	sector_t segment_sector;
	int extent_cnt;
	int page_cnt;
	struct list_head io_entries;
	struct eswap_entry *io_entries_fifo[BIO_MAX_PAGES];
	struct work_struct endio_work;
	struct eswap_io_req *req;
	struct eswap_segment_time time;
	u32 bio_result;
};

struct eswap_io_req {
	struct eswap_io io_para;
	struct kref refcount;
	struct mutex refmutex;
	struct wait_queue_head io_wait;
	atomic_t extent_inflight;
	struct completion io_end_flag;
	struct eswap_segment *segment;
	bool limit_inflight_flag;
	bool wait_io_finish_flag;
	int page_cnt;
	int segment_cnt;
	int nice;
};

static bool eswap_check_entry_err(
	struct eswap_entry *io_entry)
{
	int i;

	if (unlikely(!io_entry)) {
		eswap_print(LEVEL_ERR, "io_entry is null\n");
		return true;
	}

	if (unlikely((!io_entry->dest_pages) ||
		(io_entry->ext_id < 0) ||
		(io_entry->pages_sz > BIO_MAX_PAGES) ||
		(io_entry->pages_sz <= 0))) {
		eswap_print(LEVEL_DEBUG, "ext_id %d, page_sz %d\n", io_entry->ext_id,
			io_entry->pages_sz);
		return true;
	}

	for (i = 0; i < io_entry->pages_sz; ++i) {
		if (!io_entry->dest_pages[i]) {
			eswap_print(LEVEL_ERR, "dest_pages[%d] is null\n", i);
			return true;
		}
	}

	return false;
}

static bool eswap_ext_merge_back(
	struct eswap_segment *segment,
	struct eswap_entry *io_entry)
{
	struct eswap_entry *tail_io_entry =
		list_last_entry(&segment->io_entries,
			struct eswap_entry, list);

	return ((tail_io_entry->addr +
		tail_io_entry->pages_sz * ESWAP_PAGE_SIZE_SECTOR) ==
		io_entry->addr);
}

static bool eswap_ext_merge_front(
	struct eswap_segment *segment,
	struct eswap_entry *io_entry)
{
	struct eswap_entry *head_io_entry =
		list_first_entry(&segment->io_entries,
			struct eswap_entry, list);

	return (head_io_entry->addr ==
		(io_entry->addr +
		io_entry->pages_sz * ESWAP_PAGE_SIZE_SECTOR));
}

static bool eswap_ext_merge(struct eswap_io_req *req,
	struct eswap_entry *io_entry)
{
	struct eswap_segment *segment = req->segment;

	if (segment == NULL)
		return false;

	if ((segment->page_cnt + io_entry->pages_sz) > BIO_MAX_PAGES)
		return false;

	if (eswap_ext_merge_front(segment, io_entry)) {
		list_add(&io_entry->list, &segment->io_entries);
		segment->io_entries_fifo[segment->extent_cnt++] = io_entry;
		segment->segment_sector = io_entry->addr;
		segment->page_cnt += io_entry->pages_sz;
		return true;
	}

	if (eswap_ext_merge_back(segment, io_entry)) {
		list_add_tail(&io_entry->list, &segment->io_entries);
		segment->io_entries_fifo[segment->extent_cnt++] = io_entry;
		segment->page_cnt += io_entry->pages_sz;

		return true;
	}

	return false;
}

static void eswap_io_req_release(struct kref *ref)
{
	struct eswap_io_req *req =
		container_of(ref, struct eswap_io_req, refcount);

	if (req->io_para.complete_notify && req->io_para.private)
		req->io_para.complete_notify(req->io_para.private);

	kfree(req);
}

static void eswap_segment_free(struct eswap_io_req *req,
	struct eswap_segment *segment)
{
	int i;

	for (i = 0; i < segment->extent_cnt; ++i) {
		INIT_LIST_HEAD(&segment->io_entries_fifo[i]->list);
		req->io_para.done_callback(
				segment->io_entries_fifo[i], -EIO);
	}
	kfree(segment);
}

static void eswap_limit_inflight(struct eswap_io_req *req)
{
	int ret;

	if (!req->limit_inflight_flag)
		return;

	if (atomic_read(&req->extent_inflight) >= ESWAP_MAX_INFILGHT_NUM)
		do {
			eswap_print(LEVEL_DEBUG, "wait inflight start\n");
			ret = wait_event_timeout(req->io_wait,
				atomic_read(&req->extent_inflight) <
				ESWAP_MAX_INFILGHT_NUM,
				msecs_to_jiffies(100));
		} while (!ret);
}

static struct bio *eswap_bio_alloc(enum eswap_scenario scenario)
{
	gfp_t gfp = (scenario != ESWAP_RECLAIM_IN) ? GFP_ATOMIC : GFP_NOIO;
	struct bio *bio = bio_alloc(gfp, BIO_MAX_PAGES);

	if (!bio && (scenario == ESWAP_FAULT_OUT))
		bio = bio_alloc(GFP_NOIO, BIO_MAX_PAGES);

	return bio;
}

static int eswap_bio_add_page(struct bio *bio,
	struct eswap_segment *segment)
{
	int i;
	int k = 0;
	struct eswap_entry *io_entry = NULL;
	struct eswap_entry *tmp = NULL;

	list_for_each_entry_safe(io_entry, tmp, &segment->io_entries, list)  {
		for (i = 0; i < io_entry->pages_sz; i++) {
			io_entry->dest_pages[i]->index =
				bio->bi_iter.bi_sector + k;
			if (unlikely(!bio_add_page(bio,
				io_entry->dest_pages[i], PAGE_SIZE, 0))) {

				return -EIO;
			}
			k += ESWAP_PAGE_SIZE_SECTOR;
		}
	}

	return 0;
}

static void eswap_set_bio_opf(struct bio *bio,
	struct eswap_segment *segment)
{
	if (segment->req->io_para.scenario == ESWAP_RECLAIM_IN) {
		bio->bi_opf |= REQ_BACKGROUND;
		return;
	}

	bio->bi_opf |= REQ_SYNC;
}

static void eswap_inflight_inc(struct eswap_segment *segment)
{
	mutex_lock(&segment->req->refmutex);
	kref_get(&segment->req->refcount);
	mutex_unlock(&segment->req->refmutex);
	atomic_add(segment->page_cnt, &segment->req->extent_inflight);
}

static void eswap_inflight_dec(struct eswap_io_req *req,
	int num)
{
	if ((atomic_sub_return(num, &req->extent_inflight) <
		ESWAP_MAX_INFILGHT_NUM) && req->limit_inflight_flag &&
		wq_has_sleeper(&req->io_wait))
		wake_up(&req->io_wait);
}

static void eswap_io_end_wake_up(struct eswap_io_req *req)
{
	if (req->io_para.scenario == ESWAP_FAULT_OUT) {
		complete(&req->io_end_flag);
		return;
	}

	if (wq_has_sleeper(&req->io_wait))
		wake_up(&req->io_wait);
}

static void eswap_io_entry_proc(struct eswap_segment *segment)
{
	int i;
	struct eswap_io_req *req = segment->req;
	int page_num;
	ktime_t callback_start;

	for (i = 0; i < segment->extent_cnt; ++i) {
		INIT_LIST_HEAD(&segment->io_entries_fifo[i]->list);
		page_num = segment->io_entries_fifo[i]->pages_sz;
		eswap_print(LEVEL_DEBUG, "extent_id[%d] %d page_num %d\n",
			i, segment->io_entries_fifo[i]->ext_id, page_num);
		callback_start = ktime_get();

		if (req->io_para.done_callback)
			req->io_para.done_callback(segment->io_entries_fifo[i],
				0);
		eswap_inflight_dec(req, page_num);
	}
}

static void eswap_io_err_proc(struct eswap_io_req *req,
	struct eswap_segment *segment)
{
	eswap_print(LEVEL_DEBUG, "segment sector 0x%llx, extent_cnt %d\n",
		segment->segment_sector, segment->extent_cnt);
	eswap_print(LEVEL_DEBUG, "scenario %u, bio_result %u\n",
		req->io_para.scenario, segment->bio_result);

	eswap_inflight_dec(req, segment->page_cnt);
	eswap_io_end_wake_up(req);
	eswap_segment_free(req, segment);
	kref_put_mutex(&req->refcount, eswap_io_req_release,
		&req->refmutex);
}

static void eswap_io_end_work(struct work_struct *work)
{
	struct eswap_segment *segment =
		container_of(work, struct eswap_segment, endio_work);
	struct eswap_io_req *req = segment->req;
	int old_nice = task_nice(current);

	if (unlikely(segment->bio_result)) {
		eswap_io_err_proc(req, segment);
		return;
	}

	set_user_nice(current, req->nice);
	eswap_io_entry_proc(segment);
	eswap_io_end_wake_up(req);

	kref_put_mutex(&req->refcount, eswap_io_req_release,
		&req->refmutex);
	kfree(segment);

	set_user_nice(current, old_nice);
}

static void eswap_end_io(struct bio *bio)
{
	struct eswap_segment *segment = bio->bi_private;
	struct eswap_io_req *req = NULL;
	struct workqueue_struct *workqueue = NULL;

	if (unlikely(!segment || !(segment->req))) {
		eswap_print(LEVEL_ERR, "segment or req is null\n");
		bio_put(bio);
		return;
	}

	req = segment->req;

	workqueue = (req->io_para.scenario == ESWAP_RECLAIM_IN) ?
		eswap_proc_write_workqueue : eswap_proc_read_workqueue;
	segment->time.end_io = ktime_get();
	segment->bio_result = bio->bi_status;

	queue_work(workqueue, &segment->endio_work);
	bio_put(bio);
}

static int eswap_submit_bio(struct eswap_segment *segment)
{
	unsigned int op =
		(segment->req->io_para.scenario == ESWAP_RECLAIM_IN) ?
		REQ_OP_WRITE : REQ_OP_READ;
	struct eswap_entry *head_io_entry =
		list_first_entry(&segment->io_entries,
			struct eswap_entry, list);
	struct bio *bio = NULL;

	bio = eswap_bio_alloc(segment->req->io_para.scenario);
	if (unlikely(!bio)) {
		eswap_print(LEVEL_ERR, "bio is null\n");
		return -ENOMEM;
	}

	bio->bi_iter.bi_sector = segment->segment_sector;
	bio_set_dev(bio, segment->req->io_para.bdev);
	bio->bi_private = segment;
	bio_set_op_attrs(bio, op, 0);
	bio->bi_end_io = eswap_end_io;
	eswap_set_bio_opf(bio, segment);

	if (unlikely(eswap_bio_add_page(bio, segment))) {
		bio_put(bio);
		eswap_print(LEVEL_ERR, "bio_add_page fail\n");
		return -EIO;
	}

	eswap_inflight_inc(segment);

	eswap_print(LEVEL_DEBUG, "submit bio sector %llu ext_id %d extent_cnt %d scenario %u\n",
		segment->segment_sector, head_io_entry->ext_id, segment->extent_cnt, segment->req->io_para.scenario);

	segment->req->page_cnt += segment->page_cnt;
	segment->req->segment_cnt++;
	segment->time.submit_bio = ktime_get();

	submit_bio(bio);
	return 0;
}

static int eswap_new_segment_init(struct eswap_io_req *req,
	struct eswap_entry *io_entry)
{
	gfp_t gfp = (req->io_para.scenario != ESWAP_RECLAIM_IN) ?
		GFP_ATOMIC : GFP_NOIO;
	struct eswap_segment *segment = NULL;

	segment = kzalloc(sizeof(struct eswap_segment), gfp);
	if (!segment && (req->io_para.scenario == ESWAP_FAULT_OUT))
		segment = kzalloc(sizeof(struct eswap_segment), GFP_NOIO);

	if (unlikely(!segment)) {
		return -ENOMEM;
	}

	segment->req = req;
	INIT_LIST_HEAD(&segment->io_entries);
	list_add_tail(&io_entry->list, &segment->io_entries);
	segment->io_entries_fifo[segment->extent_cnt++] = io_entry;
	segment->page_cnt = io_entry->pages_sz;
	INIT_WORK(&segment->endio_work, eswap_io_end_work);
	segment->segment_sector = io_entry->addr;
	req->segment = segment;

	return 0;
}

static bool eswap_check_io_para_err(struct eswap_io *io_para)
{
	if (unlikely(!io_para)) {
		eswap_print(LEVEL_ERR, "io_para is null\n");
		return true;
	}

	if (unlikely(!io_para->bdev ||
		(io_para->scenario >= ESWAP_SCENARIO_BUTT))) {
		eswap_print(LEVEL_ERR, "io_para err, scenario %u\n",
			io_para->scenario);
		return true;
	}

	if (unlikely(!io_para->done_callback)) {
		eswap_print(LEVEL_ERR, "done_callback err\n");
		return true;
	}

	return false;
}

static int eswap_io_submit(struct eswap_io_req *req,
	bool merge_flag)
{
	int ret;
	struct eswap_segment *segment = req->segment;

	if (!segment || ((merge_flag) && (segment->page_cnt < BIO_MAX_PAGES)))
		return 0;

	eswap_limit_inflight(req);

	ret = eswap_submit_bio(segment);
	if (unlikely(ret))
		eswap_segment_free(req, segment);

	req->segment = NULL;

	return ret;
}

static int eswap_io_extent(void *io_handler,
	struct eswap_entry *io_entry)
{
	int ret;
	struct eswap_io_req *req = (struct eswap_io_req *)io_handler;

	if (unlikely(eswap_check_entry_err(io_entry))) {
		req->io_para.done_callback(io_entry, -EIO);
		return -EFAULT;
	}

	eswap_print(LEVEL_DEBUG, "ext id %d, pages_sz %d, addr %llx\n",
		io_entry->ext_id, io_entry->pages_sz,
		io_entry->addr);

	if (eswap_ext_merge(req, io_entry))
		return eswap_io_submit(req, true);

	ret = eswap_io_submit(req, false);
	if (unlikely(ret)) {
		eswap_print(LEVEL_ERR, "submit fail %d\n", ret);
		req->io_para.done_callback(io_entry, -EIO);
		return ret;
	}

	ret = eswap_new_segment_init(req, io_entry);
	if (unlikely(ret)) {
		eswap_print(LEVEL_ERR, "eswap_new_segment_init fail %d\n",
			ret);
		req->io_para.done_callback(io_entry, -EIO);

		return ret;
	}

	return 0;
}

/* io_handler validity guaranteed by the caller */
int eswap_read_extent(void *io_handler,
	struct eswap_entry *io_entry)
{
	return eswap_io_extent(io_handler, io_entry);
}

/* io_handler validity guaranteed by the caller */
int eswap_write_extent(void *io_handler,
	struct eswap_entry *io_entry)
{
	return eswap_io_extent(io_handler, io_entry);
}

static void eswap_wait_io_finish(struct eswap_io_req *req)
{
	int ret;
	unsigned int wait_time;
	int fault_out_wait_time = 0;

	if (!req->wait_io_finish_flag || !req->page_cnt)
		return;

	if (req->io_para.scenario == ESWAP_FAULT_OUT) {
		eswap_print(LEVEL_DEBUG, "fault out wait finish start\n");

		do {
			ret = wait_for_completion_io_timeout(&req->io_end_flag, msecs_to_jiffies(ESWAP_FAULTOUT_TIMEOUT));
			fault_out_wait_time += ESWAP_FAULTOUT_TIMEOUT;
			if (!ret) {
				eswap_print(LEVEL_ERR, "fault out wait time out %d\n", fault_out_wait_time);
			}
		} while (!ret && (fault_out_wait_time < ESWAP_MAX_FAULTOUT_TIMEOUT));

		return;
	}

	wait_time = (req->io_para.scenario == ESWAP_RECLAIM_IN) ?
		ESWAP_WRITE_TIME : ESWAP_READ_TIME;

	do {
		eswap_print(LEVEL_DEBUG, "wait finish start\n");
		ret = wait_event_timeout(req->io_wait,
			(!atomic_read(&req->extent_inflight)),
			msecs_to_jiffies(wait_time));
	} while (!ret);
}

static void eswap_stat_io_bytes(struct eswap_io_req *req)
{
	struct eswap_stat *stat = eswap_get_stat_obj();

	if (!stat || !req->page_cnt)
		return;

	if (req->io_para.scenario == ESWAP_RECLAIM_IN) {
		atomic64_add(req->page_cnt * PAGE_SIZE,
			&stat->reclaimin_bytes);
		atomic64_inc(&stat->reclaimin_cnt);
	} else {
		atomic64_add(req->page_cnt * PAGE_SIZE,
			&stat->faultout_bytes);
		atomic64_inc(&stat->faultout_cnt);
	}
}

/* io_handler validity guaranteed by the caller */
int eswap_plug_finish(void *io_handler)
{
	int ret;
	struct eswap_io_req *req = (struct eswap_io_req *)io_handler;

	ret = eswap_io_submit(req, false);
	if (unlikely(ret))
		eswap_print(LEVEL_ERR, "submit fail %d\n", ret);

	eswap_wait_io_finish(req);

	eswap_stat_io_bytes(req);

	eswap_print(LEVEL_DEBUG, "io schedule finish\n");
	return ret;
}

void *eswap_plug_start(struct eswap_io *io_para)
{
	gfp_t gfp;
	struct eswap_io_req *req = NULL;

	if (unlikely(eswap_check_io_para_err(io_para)))
		return NULL;

	gfp = (io_para->scenario != ESWAP_RECLAIM_IN) ?
		GFP_ATOMIC : GFP_NOIO;
	req = kzalloc(sizeof(struct eswap_io_req), gfp);
	if (!req && (io_para->scenario == ESWAP_FAULT_OUT))
		req = kzalloc(sizeof(struct eswap_io_req), GFP_NOIO);

	if (unlikely(!req)) {
		eswap_print(LEVEL_ERR, "io_req is null\n");
		return NULL;
	}

	kref_init(&req->refcount);
	mutex_init(&req->refmutex);
	atomic_set(&req->extent_inflight, 0);
	init_waitqueue_head(&req->io_wait);
	req->io_para.bdev = io_para->bdev;
	req->io_para.scenario = io_para->scenario;
	req->io_para.done_callback = io_para->done_callback;
	req->io_para.complete_notify = io_para->complete_notify;
	req->io_para.private = io_para->private;
	req->limit_inflight_flag =
		(io_para->scenario == ESWAP_RECLAIM_IN);
	req->wait_io_finish_flag =
		(io_para->scenario == ESWAP_RECLAIM_IN) ||
		(io_para->scenario == ESWAP_FAULT_OUT);
	req->nice = task_nice(current);
	init_completion(&req->io_end_flag);

	return (void *)req;
}

static void eswap_key_init(void)
{
	get_random_bytes(eswap_io_key, ESWAP_KEY_SIZE);
}

int eswap_schedule_init(void)
{
	if (eswap_schedule_init_flag)
		return 0;

	eswap_proc_read_workqueue = alloc_workqueue("proc_eswap_read",
		WQ_HIGHPRI | WQ_UNBOUND, 0);
	if (unlikely(!eswap_proc_read_workqueue))
		return -EFAULT;

	eswap_proc_write_workqueue = alloc_workqueue("proc_eswap_write",
		WQ_CPU_INTENSIVE, 0);
	if (unlikely(!eswap_proc_write_workqueue)) {
		destroy_workqueue(eswap_proc_read_workqueue);

		return -EFAULT;
	}

	eswap_key_init();

	eswap_schedule_init_flag = true;

	return 0;
}