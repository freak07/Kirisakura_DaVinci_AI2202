/*
 * Expanded RAM block device
 * Description: expanded memory implement
 *
 * Released under the terms of GNU General Public License Version 2.0
 *
 */

#define KMSG_COMPONENT "expandmem"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/string.h>

#include "expandmem.h"
#include "expandmem_vendor_hooks.h"
#include "expandmem_impl.h"
#include "eswap.h"

static DEVICE_ATTR_RW(eswap_enable);
static DEVICE_ATTR_RW(eswap_reclaimin_enable);
static DEVICE_ATTR_RW(eswap_wdt);
static DEVICE_ATTR_RW(eswap_loglevel);

static struct attribute *expandmem_disk_attrs[] = {
	&dev_attr_eswap_enable.attr,
	&dev_attr_eswap_reclaimin_enable.attr,
	&dev_attr_eswap_wdt.attr,
	&dev_attr_eswap_loglevel.attr,
	NULL,
};

static const struct attribute_group expandmem_disk_attr_group = {
	.attrs = expandmem_disk_attrs,
};

static int expandmem_probe(struct platform_device *pdev)
{
	int ret;

	if (is_support_eswap()) {
		ret = expandmem_mem_vendor_hooks_init();
		if (ret) {
			eswap_print(LEVEL_ERR, "vendor hooks init failed\n");
			goto out_error;
		}

		expandmem_mem_vendor_cgroup_init();

		ret = sysfs_create_group(&pdev->dev.kobj, &expandmem_disk_attr_group);
		if (ret) {
			eswap_print(LEVEL_ERR, "sysfs_create_group ERROR\n");
			goto err_unregister;
		}

		expandmem_ufs_vendor_hooks_init();
#if IS_ENABLED(CONFIG_EXPANDMEM_DEBUG)
		expandmem_monitor_init();
#endif

		return 0;

	err_unregister:
		expandmem_mem_vendor_hooks_remove();
	out_error:
		return ret;
	}

	return 0;
}

static int expandmem_remove(struct platform_device *pdev)
{
	if (is_support_eswap()) {
		expandmem_mem_vendor_hooks_remove();
		sysfs_remove_group(&pdev->dev.kobj, &expandmem_disk_attr_group);

		expandmem_ufs_vendor_hooks_remove();
	}

	return 0;
}

static const struct of_device_id expandmem_match_table[] = {
	{ .compatible = "asus,expandmem" },
	{ }
};
MODULE_DEVICE_TABLE(of, expandmem_match_table);

static struct platform_driver expandmem_driver = {
	.driver = {
		.name = "expandmem",
		.owner = THIS_MODULE,
		.of_match_table = expandmem_match_table,
	},
	.probe = expandmem_probe,
	.remove = expandmem_remove,
};

static void destroy_devices(void)
{
	return platform_driver_unregister(&expandmem_driver);
}

static int __init expandmem_init(void)
{
	int ret = 0;

	ret = platform_driver_register(&expandmem_driver);
	if (ret) {
		return ret;
	}

	if (is_support_eswap()) {
		ret = zswapd_init();
		if (ret) {
			eswap_print(LEVEL_ERR, "zswapd init failed\n");
			destroy_devices();
			return ret;
		}

		snapshotd_init();
		return 0;
	}

	return 0;
}

static void __exit expandmem_exit(void)
{
	if (is_support_eswap()) {
		zswapd_exit();
	}
	destroy_devices();
}

module_init(expandmem_init);
module_exit(expandmem_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("Expanded RAM Block Device");
