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
#include <linux/slab.h>
#include <linux/bit_spinlock.h>

#include "../zram_drv.h"
#include "eswap.h"
#include "eswap_common.h"
#include "eswap_list.h"

unsigned short eswap_list_get_mcgid(int idx, struct eswap_list_table *table)
{
	struct eswap_list_head *node = idx_node(idx, table);
	int mcg_id;

	if (!node) {
		eswap_print(LEVEL_WARN, "idx = %d, table = %pK\n", idx, table);
		return 0;
	}

	eswap_lock_list(idx, table);
	mcg_id = (node->mcg_hi << ESWAP_LIST_MCG_SHIFT_HALF) | node->mcg_lo;
	eswap_unlock_list(idx, table);

	return mcg_id;
}

void eswap_list_set_mcgid(int idx, struct eswap_list_table *table, int mcg_id)
{
	struct eswap_list_head *node = idx_node(idx, table);

	if (!node) {
		eswap_print(LEVEL_WARN, "idx = %d, table = %pK, mcg = %d\n",
			 idx, table, mcg_id);
		return;
	}

	eswap_lock_list(idx, table);
	node->mcg_hi = (u32)mcg_id >> ESWAP_LIST_MCG_SHIFT_HALF;
	node->mcg_lo = (u32)mcg_id & ((1 << ESWAP_LIST_MCG_SHIFT_HALF) - 1);
	eswap_unlock_list(idx, table);
}

void eswap_lock_list(int idx, struct eswap_list_table *table)
{
	struct eswap_list_head *node = idx_node(idx, table);

	if (!node) {
		eswap_print(LEVEL_DEBUG, "idx = %d, table = %pK\n", idx, table);
		return;
	}
	bit_spin_lock(ESWAP_LIST_LOCK_BIT, (unsigned long *)node);
}

void eswap_unlock_list(int idx, struct eswap_list_table *table)
{
	struct eswap_list_head *node = idx_node(idx, table);

	if (!node) {
		eswap_print(LEVEL_DEBUG, "idx = %d, table = %pK\n", idx, table);
		return;
	}
	bit_spin_unlock(ESWAP_LIST_LOCK_BIT, (unsigned long *)node);
}

void eswap_list_del_nolock(int idx, int hidx, struct eswap_list_table *table)
{
	struct eswap_list_head *node = NULL;
	struct eswap_list_head *prev = NULL;
	struct eswap_list_head *next = NULL;
	int pidx, nidx;

	node = idx_node(idx, table);
	if (!node) {
		eswap_print(LEVEL_WARN, "node is null, idx = %d, hidx = %d, table = %pK\n",
			 idx, hidx, table);
		return;
	}
	prev = idx_node(node->prev, table);
	if (!prev) {
		eswap_print(LEVEL_WARN, "prev is null, idx = %d, hidx = %d, table = %pK\n",
			 idx, hidx, table);
		return;
	}
	next = idx_node(node->next, table);
	if (!next) {
		eswap_print(LEVEL_WARN, "next is null, idx = %d, hidx = %d, table = %pK\n",
			 idx, hidx, table);
		return;
	}

	if (idx != hidx)
		eswap_lock_list(idx, table);
	pidx = node->prev;
	nidx = node->next;
	node->prev = idx;
	node->next = idx;
	if (idx != hidx)
		eswap_unlock_list(idx, table);
	if (pidx != hidx)
		eswap_lock_list(pidx, table);
	prev->next = nidx;
	if (pidx != hidx)
		eswap_unlock_list(pidx, table);
	if (nidx != hidx)
		eswap_lock_list(nidx, table);
	next->prev = pidx;
	if (nidx != hidx)
		eswap_unlock_list(nidx, table);
}

void eswap_list_del(int idx, int hidx, struct eswap_list_table *table)
{
	eswap_lock_list(hidx, table);
	eswap_list_del_nolock(idx, hidx, table);
	eswap_unlock_list(hidx, table);
}

void eswap_list_add_nolock(int idx, int hidx, struct eswap_list_table *table)
{
	struct eswap_list_head *node = NULL;
	struct eswap_list_head *head = NULL;
	struct eswap_list_head *next = NULL;
	int nidx;

	node = idx_node(idx, table);
	if (!node) {
		eswap_print(LEVEL_WARN, "node is null, idx = %d, hidx = %d, table = %pK\n",
			 idx, hidx, table);
		return;
	}
	head = idx_node(hidx, table);
	if (!head) {
		eswap_print(LEVEL_WARN, "head is null, idx = %d, hidx = %d, table = %pK\n",
			 idx, hidx, table);
		return;
	}
	next = idx_node(head->next, table);
	if (!next) {
		eswap_print(LEVEL_WARN, "next is null, idx = %d, hidx = %d, table = %pK\n",
			 idx, hidx, table);
		return;
	}

	nidx = head->next;
	if (idx != hidx)
		eswap_lock_list(idx, table);
	node->prev = hidx;
	node->next = nidx;
	if (idx != hidx)
		eswap_unlock_list(idx, table);
	head->next = idx;
	if (nidx != hidx)
		eswap_lock_list(nidx, table);
	next->prev = idx;
	if (nidx != hidx)
		eswap_unlock_list(nidx, table);
}

void eswap_list_init(int idx, struct eswap_list_table *table)
{
	struct eswap_list_head *node = idx_node(idx, table);

	if (!node) {
		eswap_print(LEVEL_WARN, "idx = %d, table = %pK\n", idx, table);
		return;
	}
	memset(node, 0, sizeof(struct eswap_list_head));
	node->prev = idx;
	node->next = idx;
}

void eswap_list_add(int idx, int hidx, struct eswap_list_table *table)
{
	eswap_lock_list(hidx, table);
	eswap_list_add_nolock(idx, hidx, table);
	eswap_unlock_list(hidx, table);
}

void eswap_list_add_tail_nolock(int idx, int hidx, struct eswap_list_table *table)
{
	struct eswap_list_head *node = NULL;
	struct eswap_list_head *head = NULL;
	struct eswap_list_head *tail = NULL;
	int tidx;

	node = idx_node(idx, table);
	if (!node) {
		eswap_print(LEVEL_WARN, "node is null, idx = %d, hidx = %d, table = %pK\n",
			 idx, hidx, table);
		return;
	}
	head = idx_node(hidx, table);
	if (!head) {
		eswap_print(LEVEL_WARN, "head is null, idx = %d, hidx = %d, table = %pK\n",
			 idx, hidx, table);
		return;
	}
	tail = idx_node(head->prev, table);
	if (!tail) {
		eswap_print(LEVEL_WARN, "tail is null, idx = %d, hidx = %d, table = %pK\n",
			 idx, hidx, table);
		return;
	}

	tidx = head->prev;
	if (idx != hidx)
		eswap_lock_list(idx, table);
	node->prev = tidx;
	node->next = hidx;
	if (idx != hidx)
		eswap_unlock_list(idx, table);
	head->prev = idx;
	if (tidx != hidx)
		eswap_lock_list(tidx, table);
	tail->next = idx;
	if (tidx != hidx)
		eswap_unlock_list(tidx, table);
}

void eswap_list_add_tail(int idx, int hidx, struct eswap_list_table *table)
{
	eswap_lock_list(hidx, table);
	eswap_list_add_tail_nolock(idx, hidx, table);
	eswap_unlock_list(hidx, table);
}

bool eswap_list_set_priv(int idx, struct eswap_list_table *table)
{
	struct eswap_list_head *node = idx_node(idx, table);
	bool ret = false;

	if (!node) {
		eswap_print(LEVEL_WARN, "idx = %d, table = %pK\n", idx, table);
		return false;
	}
	eswap_lock_list(idx, table);
	ret = !test_and_set_bit(ESWAP_LIST_PRIV_BIT, (unsigned long *)node);
	eswap_unlock_list(idx, table);

	return ret;
}

bool eswap_list_clear_priv(int idx, struct eswap_list_table *table)
{
	struct eswap_list_head *node = idx_node(idx, table);
	bool ret = false;

	if (!node) {
		eswap_print(LEVEL_ERR, "idx = %d, table = %pK invalid\n", idx, table);
		return false;
	}

	eswap_lock_list(idx, table);
	ret = test_and_clear_bit(ESWAP_LIST_PRIV_BIT, (unsigned long *)node);
	eswap_unlock_list(idx, table);

	return ret;
}

static struct eswap_list_head *get_node_default(int idx, void *private)
{
	struct eswap_list_head *table = private;

	return &table[idx];
}

struct eswap_list_table *alloc_table(struct eswap_list_head *(*get_node)(int, void *),
				  void *private, gfp_t gfp)
{
	struct eswap_list_table *table =
				kmalloc(sizeof(struct eswap_list_table), gfp);

	if (!table)
		return NULL;
	table->get_node = get_node ? get_node : get_node_default;
	table->private = private;

	return table;
}