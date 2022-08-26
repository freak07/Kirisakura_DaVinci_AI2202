/*
 * Expanded RAM block device
 * Description: expanded memory implement
 *
 * Released under the terms of GNU General Public License Version 2.0
 *
 */

#ifndef _EXPANDMEM_IMPL_H_
#define _EXPANDMEM_IMPL_H_

int zswapd_init(void);
void zswapd_exit(void);
int snapshotd_init(void);
void set_snapshotd_init_flag(unsigned int val);
pid_t get_zswapd_pid(void);
void zswapd_status_show(struct seq_file *m);
unsigned long expandmem_lruvec_lru_size(struct lruvec *lruvec, enum lru_list lru, int zone_idx);
inline bool oem_mem_cgroup_disabled(void);
bool is_support_eswap(void);
#endif
