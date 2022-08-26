/*
 * Expanded RAM block device
 * Description: expanded memory implement
 *
 * Released under the terms of GNU General Public License Version 2.0
 *
 */

#ifndef _ESWAP_LIST_H_
#define _ESWAP_LIST_H_

#define ESWAP_LIST_PTR_SHIFT 23
#define ESWAP_LIST_MCG_SHIFT_HALF 8
#define ESWAP_LIST_LOCK_BIT ESWAP_LIST_MCG_SHIFT_HALF
#define ESWAP_LIST_PRIV_BIT (ESWAP_LIST_PTR_SHIFT + ESWAP_LIST_MCG_SHIFT_HALF + \
				ESWAP_LIST_MCG_SHIFT_HALF + 1)

struct eswap_list_head {
	unsigned int mcg_hi : ESWAP_LIST_MCG_SHIFT_HALF;
	unsigned int lock : 1;
	unsigned int prev : ESWAP_LIST_PTR_SHIFT;
	unsigned int mcg_lo : ESWAP_LIST_MCG_SHIFT_HALF;
	unsigned int priv : 1;
	unsigned int next : ESWAP_LIST_PTR_SHIFT;
};

struct eswap_list_table {
	struct eswap_list_head *(*get_node)(int, void *);
	void *private;
};

static inline struct eswap_list_head *idx_node(int idx, struct eswap_list_table *tab)
{
	return (tab && tab->get_node) ? tab->get_node(idx, tab->private) : NULL;
}

static inline int next_idx(int idx, struct eswap_list_table *tab)
{
	struct eswap_list_head *node = idx_node(idx, tab);
	return node ? node->next : -EINVAL;
}

static inline int prev_idx(int idx, struct eswap_list_table *tab)
{
	struct eswap_list_head *node = idx_node(idx, tab);
	return node ? node->prev : -EINVAL;
}

static inline int is_first_idx(int idx, int hidx, struct eswap_list_table *tab)
{
	return prev_idx(idx, tab) == hidx;
}

static inline int is_last_idx(int idx, int hidx, struct eswap_list_table *tab)
{
	return next_idx(idx, tab) == hidx;
}

#define eswap_list_for_each_entry(idx, hidx, tab) \
	for ((idx) = next_idx((hidx), (tab)); \
	     (idx) >= 0 && (idx) != (hidx); (idx) = next_idx((idx), (tab)))
#define eswap_list_for_each_entry_reverse(idx, hidx, tab) \
	for ((idx) = prev_idx((hidx), (tab)); \
	     (idx) >= 0 && (idx) != (hidx); (idx) = prev_idx((idx), (tab)))

unsigned short eswap_list_get_mcgid(int idx, struct eswap_list_table *table);
void eswap_list_set_mcgid(int idx, struct eswap_list_table *table, int mcg_id);
void eswap_lock_list(int idx, struct eswap_list_table *table);
void eswap_unlock_list(int idx, struct eswap_list_table *table);
void eswap_list_del(int idx, int hidx, struct eswap_list_table *table);
void eswap_list_init(int idx, struct eswap_list_table *table);
void eswap_list_add(int idx, int hidx, struct eswap_list_table *table);
void eswap_list_add_tail(int idx, int hidx, struct eswap_list_table *table);
bool eswap_list_set_priv(int idx, struct eswap_list_table *table);
bool eswap_list_clear_priv(int idx, struct eswap_list_table *table);
void eswap_list_add_nolock(int idx, int hidx, struct eswap_list_table *table);
void eswap_list_del_nolock(int idx, int hidx, struct eswap_list_table *table);
struct eswap_list_table *alloc_table(struct eswap_list_head *(*get_node)(int, void *),
				  void *private, gfp_t gfp);
void eswap_list_init(int idx, struct eswap_list_table *table);

#endif
