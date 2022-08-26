/*
 * Copyright (c) 2019-2020, The ASUS Company. All rights reserved.
 */

#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/soc/qcom/pmic_glink.h>
#include <linux/soc/qcom/battery_charger.h>
#include "asus_battery_charger_AI2202.h"

#include <linux/of_gpio.h>
#include <drm/drm_panel.h>
#include <linux/soc/qcom/panel_event_notifier.h>
#include <linux/delay.h>
#include <linux/reboot.h>

#include <linux/extcon.h>
#include <../../extcon-asus/extcon-asus.h>
#include <linux/iio/consumer.h>

//[+++] Add debug log
#define CHARGER_TAG "[BAT][CHG]"
#define ERROR_TAG "[ERR]"
#define CHG_DBG(...)  printk(KERN_INFO CHARGER_TAG __VA_ARGS__)
#define CHG_DBG_E(...)  printk(KERN_ERR CHARGER_TAG ERROR_TAG __VA_ARGS__)
//[---] Add debug log

enum {
    QTI_POWER_SUPPLY_USB_TYPE_HVDCP = 0x80,
    QTI_POWER_SUPPLY_USB_TYPE_HVDCP_3,
    QTI_POWER_SUPPLY_USB_TYPE_HVDCP_3P5,
};

static const char * const power_supply_usb_type_text[] = {
    "Unknown", "USB", "DCP", "CDP", "ACA", "C",
    "PD", "PD_DRP", "PD_PPS", "BrickID"
};
/* Custom usb_type definitions */
static const char * const qc_power_supply_usb_type_text[] = {
    "HVDCP", "HVDCP_3", "HVDCP_3P5"
};

bool g_asuslib_init = false;
bool g_once_usb_thermal = false;
bool g_Charger_mode = false;
EXPORT_SYMBOL(g_Charger_mode);
static int asus_usb_online = 0;
bool g_cos_over_full_flag = false;
bool first_cos_48h_protect = false;
volatile int g_ultra_cos_spec_time = 2880;
int  g_charger_mode_full_time = 0;
static struct wakeup_source *chg_ws;

struct extcon_asus_dev   *bat_extcon;
struct extcon_asus_dev   *bat_id_extcon;
struct extcon_asus_dev   *quickchg_extcon;
struct extcon_asus_dev   *thermal_extcon;
struct extcon_asus_dev   *adaptervid_extcon;

#define BC_WAIT_TIME_MS         1000

struct power_supply *qti_phy_usb;
struct power_supply *qti_phy_bat;
struct iio_channel *usb_conn_temp_vadc_chan;

static int battery_chg_write(struct battery_chg_dev *bcdev, void *data, int len);
static int read_property_id(struct battery_chg_dev *bcdev, struct psy_state *pst, u32 prop_id);
ssize_t oem_prop_read(enum battman_oem_property prop, size_t count);
ssize_t oem_prop_write(enum battman_oem_property prop, u32 *buf, size_t count);

//Panel Check +++
static struct drm_panel *active_panel;
struct delayed_work asus_set_panelonoff_current_work;

extern void qti_charge_notify_device_charge(void);
extern void qti_charge_notify_device_not_charge(void);

extern int asus_extcon_set_state_sync(struct extcon_asus_dev *edev, int cable_state);
extern struct extcon_asus_dev *extcon_asus_dev_allocate(void);
extern int extcon_asus_dev_register(struct extcon_asus_dev *edev);
extern int asus_extcon_get_state(struct extcon_asus_dev *edev);
extern unsigned long asus_qpnp_rtc_read_time(void);
extern bool rtc_probe_done;

static int drm_check_dt(struct device_node *np)
{
    int i = 0;
    int count = 0;
    struct device_node *node = NULL;
    struct drm_panel *panel = NULL;

    count = of_count_phandle_with_args(np, "panel", NULL);
    if (count <= 0) {
        CHG_DBG_E("find drm_panel count(%d) fail", count);
        return -ENODEV;
    }

    for (i = 0; i < count; i++) {
        node = of_parse_phandle(np, "panel", i);
        panel = of_drm_find_panel(node);
        of_node_put(node);
        if (!IS_ERR(panel)) {
            CHG_DBG_E("find drm_panel successfully");
            active_panel = panel;
            return 0;
        }
    }

    CHG_DBG_E("no find drm_panel");

    return -ENODEV;
}

void asus_set_panelonoff_current_worker(struct work_struct *work)
{
    int rc;
    u32 tmp = ChgPD_Info.panel_status;

    CHG_DBG_E("panelOn= 0x%x\n", tmp);
    rc = oem_prop_write(BATTMAN_OEM_Panel_Check, &tmp, 1);
    if (rc < 0) {
        pr_err("Failed to set BATTMAN_OEM_Panel_Check rc=%d\n", rc);
        return;
    }
}

static void fts_ts_panel_notifier_callback(enum panel_event_notifier_tag tag,
         struct panel_event_notification *notification, void *client_data)
{
    switch (notification->notif_type) {
    case DRM_PANEL_EVENT_UNBLANK:
        CHG_DBG("Display on");
        ChgPD_Info.panel_status = true;
        schedule_delayed_work(&asus_set_panelonoff_current_work, 0);
        break;
    case DRM_PANEL_EVENT_BLANK:
        CHG_DBG("Display off");
        ChgPD_Info.panel_status = false;
        schedule_delayed_work(&asus_set_panelonoff_current_work, 0);
        break;
    case DRM_PANEL_EVENT_BLANK_LP:
        CHG_DBG("DRM_PANEL_EVENT_BLANK_LP,Display resume into LP1/LP2");
        break;
    case DRM_PANEL_EVENT_FPS_CHANGE:
        break;
    default:
        break;
    }

    return;
}

void RegisterDRMCallback(void)
{
    int ret = 0;
    void *cookie = NULL;
    int ts_data;

    pr_err("[BAT][CHG] RegisterDRMCallback");
    ret = drm_check_dt(g_bcdev->dev->of_node);
    if (ret) {
        pr_err("[BAT][CHG] parse drm-panel fail");
    }

    if (active_panel) {
        pr_err("[BAT][CHG] RegisterDRMCallback: registering fb notification");
        cookie = panel_event_notifier_register(PANEL_EVENT_NOTIFICATION_PRIMARY,
                PANEL_EVENT_NOTIFIER_CLIENT_POWER, active_panel,
                &fts_ts_panel_notifier_callback, &ts_data);
        if (!cookie) {
            pr_err("Failed to register for panel events\n");
            return;
        }
    }

    return;
}
//Panel Check ---

static ssize_t pm8350b_icl_store(struct class *c,
                    struct class_attribute *attr,
                    const char *buf, size_t count)
{
    int rc;
    u32 tmp;
    tmp = simple_strtol(buf, NULL, 10);

    CHG_DBG("%s. set BATTMAN_OEM_PM8350B_ICL : %d", __func__, tmp);
    rc = oem_prop_write(BATTMAN_OEM_PM8350B_ICL, &tmp, 1);
    if (rc < 0) {
        pr_err("Failed to set BATTMAN_OEM_PM8350B_ICL rc=%d\n", rc);
        return rc;
    }

    return count;
}

static ssize_t pm8350b_icl_show(struct class *c,
                    struct class_attribute *attr, char *buf)
{
    int rc;

    rc = oem_prop_read(BATTMAN_OEM_PM8350B_ICL, 1);
    if (rc < 0) {
        pr_err("Failed to get BATTMAN_OEM_PM8350B_ICL rc=%d\n", rc);
        return rc;
    }

    return scnprintf(buf, PAGE_SIZE, "%d\n", ChgPD_Info.pm8350b_icl);
}
static CLASS_ATTR_RW(pm8350b_icl);

static ssize_t smb1396_icl_store(struct class *c,
                    struct class_attribute *attr,
                    const char *buf, size_t count)
{
    int rc;
    u32 tmp;
    tmp = simple_strtol(buf, NULL, 10);

    CHG_DBG("%s. set BATTMAN_OEM_SMB1396_ICL : %d", __func__, tmp);
    rc = oem_prop_write(BATTMAN_OEM_SMB1396_ICL, &tmp, 1);
    if (rc < 0) {
        pr_err("Failed to set BATTMAN_OEM_SMB1396_ICL rc=%d\n", rc);
        return rc;
    }

    return count;
}

static ssize_t smb1396_icl_show(struct class *c,
                    struct class_attribute *attr, char *buf)
{
    int rc;

    rc = oem_prop_read(BATTMAN_OEM_SMB1396_ICL, 1);
    if (rc < 0) {
        pr_err("Failed to get BATTMAN_OEM_SMB1396_ICL rc=%d\n", rc);
        return rc;
    }

    return scnprintf(buf, PAGE_SIZE, "%d\n", ChgPD_Info.smb1396_icl);
}
static CLASS_ATTR_RW(smb1396_icl);

static ssize_t batt_FCC_store(struct class *c,
                    struct class_attribute *attr,
                    const char *buf, size_t count)
{
    int rc;
    u32 tmp;
    tmp = simple_strtol(buf, NULL, 10);

    CHG_DBG("%s. set BATTMAN_OEM_FCC : %d", __func__, tmp);
    rc = oem_prop_write(BATTMAN_OEM_FCC, &tmp, 1);
    if (rc < 0) {
        pr_err("Failed to set BATTMAN_OEM_FCC rc=%d\n", rc);
        return rc;
    }

    return count;
}

static ssize_t batt_FCC_show(struct class *c,
                    struct class_attribute *attr, char *buf)
{
    int rc;

    rc = oem_prop_read(BATTMAN_OEM_FCC, 1);
    if (rc < 0) {
        pr_err("Failed to get BATTMAN_OEM_FCC rc=%d\n", rc);
        return rc;
    }

    return scnprintf(buf, PAGE_SIZE, "%d\n", ChgPD_Info.batt_fcc);
}
static CLASS_ATTR_RW(batt_FCC);

static ssize_t set_debugmask_store(struct class *c,
                    struct class_attribute *attr,
                    const char *buf, size_t count)
{
    int rc;
    u32 tmp;
    tmp = (u32) simple_strtol(buf, NULL, 16);

    CHG_DBG("%s. set BATTMAN_OEM_DEBUG_MASK : 0x%x", __func__, tmp);
    rc = oem_prop_write(BATTMAN_OEM_DEBUG_MASK, &tmp, 1);
    if (rc < 0) {
        pr_err("Failed to set BATTMAN_OEM_DEBUG_MASK rc=%d\n", rc);
        return rc;
    }

    return count;
}
static CLASS_ATTR_WO(set_debugmask);

static ssize_t usbin_suspend_en_store(struct class *c,
                    struct class_attribute *attr,
                    const char *buf, size_t count)
{
    int rc;
    u32 tmp;
    tmp = simple_strtol(buf, NULL, 10);

    rc = oem_prop_write(BATTMAN_OEM_USBIN_SUSPEND, &tmp, 1);

    pr_err("%s. enable : %d", __func__, tmp);
    if (rc < 0) {
        pr_err("Failed to set USBIN_SUSPEND_EN rc=%d\n", rc);
        return rc;
    }

    return count;
}

static ssize_t usbin_suspend_en_show(struct class *c,
                    struct class_attribute *attr, char *buf)
{
    int rc;

    rc = oem_prop_read(BATTMAN_OEM_USBIN_SUSPEND, 1);
    if (rc < 0) {
        pr_err("Failed to get USBIN_SUSPEND_EN rc=%d\n", rc);
        return rc;
    }

    return scnprintf(buf, PAGE_SIZE, "%d\n", ChgPD_Info.usbin_suspend_en);
}
static CLASS_ATTR_RW(usbin_suspend_en);

static ssize_t charging_suspend_en_store(struct class *c,
                    struct class_attribute *attr,
                    const char *buf, size_t count)
{
    int rc;
    u32 tmp;
    tmp = simple_strtol(buf, NULL, 10);

    CHG_DBG("%s. enable : %d", __func__, tmp);
    rc = oem_prop_write(BATTMAN_OEM_CHARGING_SUSPNED, &tmp, 1);
    if (rc < 0) {
        pr_err("Failed to set CHARGING_SUSPEND_EN rc=%d\n", rc);
        return rc;
    }

    return count;
}

static ssize_t charging_suspend_en_show(struct class *c,
                    struct class_attribute *attr, char *buf)
{
    int rc;

    rc = oem_prop_read(BATTMAN_OEM_CHARGING_SUSPNED, 1);
    if (rc < 0) {
        pr_err("Failed to get CHARGING_SUSPEND_EN rc=%d\n", rc);
        return rc;
    }

    return scnprintf(buf, PAGE_SIZE, "%d\n", ChgPD_Info.charging_suspend_en);
}
static CLASS_ATTR_RW(charging_suspend_en);

static ssize_t write_pm8350b_register_store(struct class *c,
                    struct class_attribute *attr,
                    const char *buf, size_t count)
{
    int tmp[2];
    int rc;

    sscanf(buf, "%X %X", &tmp[0], &tmp[1]);

    pr_err("address %x , data %x \n", tmp[0] , tmp[1]);

    rc = oem_prop_write(BATTMAN_OEM_Write_PM8350B_Register, tmp, 2);
    if (rc < 0) {
            pr_err("Failed to set BATTMAN_OEM_Write_PM8350B_Register rc=%d\n", rc);
            return rc;
    }
    return count;
}
static ssize_t write_pm8350b_register_show(struct class *c,
                    struct class_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "No function\n");
}
static CLASS_ATTR_RW(write_pm8350b_register);

static ssize_t set_charger_mode_store(struct class *c,
                    struct class_attribute *attr,
                    const char *buf, size_t count)
{
    int rc;
    u32 tmp;
    tmp = simple_strtol(buf, NULL, 10);
    ChgPD_Info.charger_mode = tmp;

    schedule_delayed_work(&asus_set_qc_state_work, msecs_to_jiffies(0));

    if (tmp == 0) {
	g_Charger_mode = 0;
    } else if (tmp == 1) {
	g_Charger_mode = 1;
    }

    CHG_DBG("%s. set BATTMAN_OEM_CHG_MODE : 0x%x", __func__, tmp);
    rc = oem_prop_write(BATTMAN_OEM_CHG_MODE, &tmp, 1);
    if (rc < 0) {
        pr_err("Failed to set BATTMAN_OEM_CHG_MODE rc=%d\n", rc);
        return rc;
    }

    return count;
}
static CLASS_ATTR_WO(set_charger_mode);

static ssize_t in_call_store(struct class *c,
                    struct class_attribute *attr,
                    const char *buf, size_t count)
{
    int rc;
    u32 tmp;
    tmp = simple_strtol(buf, NULL, 10);
    ChgPD_Info.in_call = tmp;

    CHG_DBG("%s. set BATTMAN_OEM_In_Call : %d", __func__, tmp);
    rc = oem_prop_write(BATTMAN_OEM_In_Call, &tmp, 1);
    if (rc < 0) {
        pr_err("Failed to set BATTMAN_OEM_In_Call rc=%d\n", rc);
        return rc;
    }

    return count;
}

static ssize_t in_call_show(struct class *c,
                    struct class_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%d\n", ChgPD_Info.in_call);
}
static CLASS_ATTR_RW(in_call);

//ATD +++
static ssize_t asus_get_PlatformID_show(struct class *c,
                    struct class_attribute *attr, char *buf)
{
    int rc;

    rc = oem_prop_read(BATTMAN_OEM_ADSP_PLATFORM_ID, 1);
    if (rc < 0) {
        pr_err("Failed to get PlatformID rc=%d\n", rc);
        return rc;
    }

    return scnprintf(buf, PAGE_SIZE, "%d\n", ChgPD_Info.PlatformID);
}
static CLASS_ATTR_RO(asus_get_PlatformID);

int asus_get_Batt_ID(void)
{
    int rc;

    rc = oem_prop_read(BATTMAN_OEM_BATT_ID, 1);
    if (rc < 0) {
        pr_err("Failed to get BattID rc=%d\n", rc);
        return rc;
    }
    return 0;
}

static ssize_t asus_get_BattID_show(struct class *c,
                    struct class_attribute *attr, char *buf)
{
    int rc;

    rc = asus_get_Batt_ID();
    if (rc < 0) {
        pr_err("Failed to get BattID rc=%d\n", rc);
        return rc;
    }

    return scnprintf(buf, PAGE_SIZE, "%d\n", ChgPD_Info.BATT_ID);
}
static CLASS_ATTR_RO(asus_get_BattID);

static ssize_t charger_limit_en_store(struct class *c,
                    struct class_attribute *attr,
                    const char *buf, size_t count)
{
    int rc;
    u32 tmp;

    tmp = simple_strtol(buf, NULL, 10);

    CHG_DBG("%s. enable : %d", __func__, tmp);
    rc = oem_prop_write(BATTMAN_OEM_CHG_LIMIT_EN, &tmp, 1);
    if (rc < 0) {
        pr_err("Failed to set CHG_LIMIT_EN rc=%d\n", rc);
        return rc;
    }

    return count;
}

static ssize_t charger_limit_en_show(struct class *c,
                    struct class_attribute *attr, char *buf)
{
    int rc;

    rc = oem_prop_read(BATTMAN_OEM_CHG_LIMIT_EN, 1);
    if (rc < 0) {
        pr_err("Failed to get CHG_LIMIT_EN rc=%d\n", rc);
        return rc;
    }

    return scnprintf(buf, PAGE_SIZE, "%d\n", ChgPD_Info.chg_limit_en);
}
static CLASS_ATTR_RW(charger_limit_en);

static ssize_t charger_limit_cap_store(struct class *c,
                    struct class_attribute *attr,
                    const char *buf, size_t count)
{
    int rc;
    u32 tmp;

    tmp = simple_strtol(buf, NULL, 10);

    CHG_DBG("%s. cap : %d", __func__, tmp);
    rc = oem_prop_write(BATTMAN_OEM_CHG_LIMIT_CAP, &tmp, 1);
    if (rc < 0) {
        pr_err("Failed to set CHG_LIMIT_CAP rc=%d\n", rc);
        return rc;
    }

    return count;
}

static ssize_t charger_limit_cap_show(struct class *c,
                    struct class_attribute *attr, char *buf)
{
    int rc;

    rc = oem_prop_read(BATTMAN_OEM_CHG_LIMIT_CAP, 1);
    if (rc < 0) {
        pr_err("Failed to get CHG_LIMIT_CAP rc=%d\n", rc);
        return rc;
    }

    return scnprintf(buf, PAGE_SIZE, "%d\n", ChgPD_Info.chg_limit_cap);
}
static CLASS_ATTR_RW(charger_limit_cap);

static ssize_t enter_ship_mode_store(struct class *c,
                    struct class_attribute *attr,
                    const char *buf, size_t count)
{
    int tmp;
    bool ship_en;
    tmp = simple_strtol(buf, NULL, 10);
    ship_en = tmp;

    if (ship_en == 0) {
        CHG_DBG_E("%s. NO action for SHIP mode\n", __func__);
        return count;
    }

    CHG_DBG_E("%s. write enter_ship_mode %d \n", __func__,ship_en);
    schedule_delayed_work(&enter_ship_work, 0);

    return count;
}

static ssize_t enter_ship_mode_show(struct class *c,
                    struct class_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "No function\n");
}
static CLASS_ATTR_RW(enter_ship_mode);

static const char *get_usb_type_name(u32 usb_type)
{
    u32 i;

    if (usb_type >= QTI_POWER_SUPPLY_USB_TYPE_HVDCP &&
        usb_type <= QTI_POWER_SUPPLY_USB_TYPE_HVDCP_3P5) {
        for (i = 0; i < ARRAY_SIZE(qc_power_supply_usb_type_text);
             i++) {
            if (i == (usb_type - QTI_POWER_SUPPLY_USB_TYPE_HVDCP))
                return qc_power_supply_usb_type_text[i];
        }
        return "Unknown";
    }

    for (i = 0; i < ARRAY_SIZE(power_supply_usb_type_text); i++) {
        if (i == usb_type)
            return power_supply_usb_type_text[i];
    }

    return "Unknown";
}

static ssize_t get_usb_type_show(struct class *c,
                    struct class_attribute *attr, char *buf)
{
    //struct power_supply *psy;
    struct psy_state *pst;
    int rc = 0 , val = 0;

    if(ChgPD_Info.AdapterVID == 0xB05){
        return scnprintf(buf, PAGE_SIZE, "PD_ASUS_PPS_30W_2A\n");
    }

    if (g_bcdev == NULL)
        return -1;
    CHG_DBG("%s\n", __func__);
    pst = &g_bcdev->psy_list[PSY_TYPE_USB];
    rc = read_property_id(g_bcdev, pst, 11);//11:USB_REAL_TYPE
    if (!rc) {
        val = pst->prop[11];//11:USB_REAL_TYPE
        CHG_DBG("%s. val : %d\n", __func__, val);
    }

    return scnprintf(buf, PAGE_SIZE, "%s\n", get_usb_type_name(val));
}
static CLASS_ATTR_RO(get_usb_type);
//ATD ---

static ssize_t demo_app_status_store(struct class *c,
                    struct class_attribute *attr,
                    const char *buf, size_t count)
{
    int rc;
    u32 tmp;
    tmp = simple_strtol(buf, NULL, 10);
    ChgPD_Info.demo_app_status = tmp;

    tmp = ChgPD_Info.demo_app_status;
    CHG_DBG("%s. set BATTMAN_OEM_DEMOAPP : %d", __func__, tmp);
    rc = oem_prop_write(BATTMAN_OEM_DEMOAPP, &tmp, 1);
    if (rc < 0) {
        pr_err("Failed to set BATTMAN_OEM_DEMOAPP rc=%d\n", rc);
        return rc;
    }

    return count;
}

static ssize_t demo_app_status_show(struct class *c,
                    struct class_attribute *attr, char *buf)
{
    return sprintf(buf, "%d\n", ChgPD_Info.demo_app_status);
}
static CLASS_ATTR_RW(demo_app_status);

static ssize_t ultra_bat_life_store(struct class *c,
                    struct class_attribute *attr,
                    const char *buf, size_t count)
{
    int rc;
    u32 tmp;
    tmp = simple_strtol(buf, NULL, 10);
    ChgPD_Info.ultra_bat_life = tmp;

    tmp = ChgPD_Info.ultra_bat_life;
    CHG_DBG("%s. set BATTMAN_OEM_Batt_Protection : %d", __func__, tmp);
    rc = oem_prop_write(BATTMAN_OEM_Batt_Protection, &tmp, 1);
    if (rc < 0) {
        pr_err("Failed to set BATTMAN_OEM_Batt_Protection rc=%d\n", rc);
        return rc;
    }

    return count;
}

static ssize_t ultra_bat_life_show(struct class *c,
                    struct class_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%d\n", ChgPD_Info.ultra_bat_life);
}
static CLASS_ATTR_RW(ultra_bat_life);

static ssize_t set_virtualthermal_store(struct class *c,
                    struct class_attribute *attr,
                    const char *buf, size_t count)
{
    int mask;
    mask = simple_strtol(buf, NULL, 16);
    ChgPD_Info.thermal_threshold = mask;
    CHG_DBG_E("%s thermal threshold=%d", __func__, mask);

    schedule_delayed_work(&asus_thermal_policy_work, 0);

    return count;
}

static ssize_t set_virtualthermal_show(struct class *c,
                    struct class_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "No function\n");
}
static CLASS_ATTR_RW(set_virtualthermal);

static ssize_t smartchg_slow_charging_store(struct class *c,
                    struct class_attribute *attr,
                    const char *buf, size_t count)
{
    u32 tmp;

    sscanf(buf, "%d", &tmp);
    ChgPD_Info.slow_chglimit = tmp;

    CHG_DBG("%s. slow charging : %d", __func__, tmp);
    if(asus_usb_online){
        cancel_delayed_work_sync(&asus_slow_charging_work);
        schedule_delayed_work(&asus_slow_charging_work, 0 * HZ);
    }

    return count;
}

static ssize_t smartchg_slow_charging_show(struct class *c,
                    struct class_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%d\n", ChgPD_Info.slow_chglimit);
}
static CLASS_ATTR_RW(smartchg_slow_charging);

static ssize_t boot_completed_store(struct class *c,
                    struct class_attribute *attr,
                    const char *buf, size_t count)
{
    u32 tmp;
    tmp = simple_strtol(buf, NULL, 10);

    ChgPD_Info.boot_completed = tmp;

    cancel_delayed_work_sync(&asus_set_qc_state_work);
    schedule_delayed_work(&asus_set_qc_state_work, msecs_to_jiffies(100));

    return count;
}

static ssize_t boot_completed_show(struct class *c,
                    struct class_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%d\n", ChgPD_Info.boot_completed);
}
static CLASS_ATTR_RW(boot_completed);

static ssize_t chg_disable_jeita_store(struct class *c,
                    struct class_attribute *attr,
                    const char *buf, size_t count)
{
    int rc;
    u32 tmp;
    tmp = simple_strtol(buf, NULL, 10);
    ChgPD_Info.chg_disable_jeita = tmp;

    CHG_DBG_E("%s. set BATTMAN_OEM_CHG_Disable_Jeita : %d", __func__, tmp);
    rc = oem_prop_write(BATTMAN_OEM_CHG_Disable_Jeita, &tmp, 1);
    if (rc < 0) {
        pr_err("Failed to set BATTMAN_OEM_CHG_Disable_Jeita rc=%d\n", rc);
        return rc;
    }

    return count;
}

static ssize_t chg_disable_jeita_show(struct class *c,
                    struct class_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%d\n", ChgPD_Info.chg_disable_jeita);
}
static CLASS_ATTR_RW(chg_disable_jeita);

static struct attribute *asuslib_class_attrs[] = {
    &class_attr_pm8350b_icl.attr,
    &class_attr_smb1396_icl.attr,
    &class_attr_batt_FCC.attr,
    &class_attr_set_debugmask.attr,
    &class_attr_usbin_suspend_en.attr,
    &class_attr_charging_suspend_en.attr,
    &class_attr_write_pm8350b_register.attr,
    &class_attr_set_charger_mode.attr,
    &class_attr_in_call.attr,
    &class_attr_asus_get_PlatformID.attr,
    &class_attr_asus_get_BattID.attr,
    &class_attr_charger_limit_en.attr,
    &class_attr_charger_limit_cap.attr,
    &class_attr_enter_ship_mode.attr,
    &class_attr_get_usb_type.attr,
    &class_attr_demo_app_status.attr,
    &class_attr_ultra_bat_life.attr,
    &class_attr_set_virtualthermal.attr,
    &class_attr_smartchg_slow_charging.attr,
    &class_attr_boot_completed.attr,
    &class_attr_chg_disable_jeita.attr,
    NULL,
};
ATTRIBUTE_GROUPS(asuslib_class);

struct class asuslib_class = {
    .name = "asuslib",
    .class_groups = asuslib_class_groups,
};

int asus_init_power_supply_prop(void) {

    // Initialize the power supply for usb properties
    if (!qti_phy_usb)
        qti_phy_usb = power_supply_get_by_name("usb");

    if (!qti_phy_usb) {
        pr_err("Failed to get usb power supply, rc=%d\n");
        return -ENODEV;
    }

    // Initialize the power supply for battery properties
    if (!qti_phy_bat)
        qti_phy_bat = power_supply_get_by_name("battery");

    if (!qti_phy_bat) {
        pr_err("Failed to get battery power supply, rc=%d\n");
        return -ENODEV;
    }
    return 0;
};

static void handle_notification(struct battery_chg_dev *bcdev, void *data,
                size_t len)
{
    struct evtlog_context_resp_msg3 *evtlog_msg;
    struct oem_enable_change_msg *enable_change_msg;
    struct asus_notify_work_event_msg *work_event_msg;
    struct oem_asus_adaptervid_msg *adaptervid_msg;
    struct oem_jeita_cc_state_msg *jeita_cc_state_msg;
    struct oem_update_charger_type_msg *update_charger_type_msg;
    struct oem_cc_orientation_msg *cc_orientation_msg;
    static int pre_chg_type = 0;
    static u32 pre_adapter_id = 0;

    struct pmic_glink_hdr *hdr = data;
    int rc;

    switch(hdr->opcode) {
    case OEM_ASUS_EVTLOG_IND:
        if (len == sizeof(*evtlog_msg)) {
            evtlog_msg = data;
            pr_err("[adsp] evtlog= %s\n", evtlog_msg->buf);
        }
        break;
    case OEM_SET_OTG_WA:
	if (len == sizeof(*enable_change_msg)) {
	    enable_change_msg = data;
	    ChgPD_Info.otg_enable = enable_change_msg->enable;

            if (gpio_is_valid(OTG_LOAD_SWITCH_GPIO)) {
                rc = gpio_direction_output(OTG_LOAD_SWITCH_GPIO, enable_change_msg->enable);
                if (rc)
                    pr_err("%s. Failed to control OTG_Load_Switch\n", __func__);
            } else {
                CHG_DBG_E("%s. OTG_LOAD_SWITCH_GPIO is invalid\n", __func__);
            }    
        } else {
            pr_err("Incorrect response length %zu for OEM_SET_OTG_WA\n",
                len);
        }
        break;
    case OEM_ASUS_WORK_EVENT_REQ:
        if (len == sizeof(*work_event_msg)) {
            work_event_msg = data;
            if(work_event_msg->work == WORK_JEITA_CC){
                CHG_DBG_E("%s OEM_ASUS_WORK_EVENT_REQ work=%d, enable=%d\n", __func__, work_event_msg->work, work_event_msg->data_buffer[0]);
                if(work_event_msg->data_buffer[0] == 1){
                    cancel_delayed_work_sync(&asus_jeita_cc_work);
                    schedule_delayed_work(&asus_jeita_cc_work, 0 * HZ);
                }else{
                    cancel_delayed_work_sync(&asus_jeita_cc_work);
                }
            }else{
                CHG_DBG_E("%s OEM_ASUS_WORK_EVENT_REQ work=%d Error Work\n", __func__, work_event_msg->work);
            }
        } else {
            pr_err("Incorrect response length %zu for OEM_ASUS_WORK_EVENT_REQ\n",
                len);
        }
        break;
    case OEM_ASUS_AdapterVID_REQ:
        if (len == sizeof(*adaptervid_msg)) {
            adaptervid_msg = data;
            CHG_DBG("%s AdapterVID enable : 0x%X\n", __func__, adaptervid_msg->VID);
            ChgPD_Info.AdapterVID = adaptervid_msg->VID;
            asus_extcon_set_state_sync(adaptervid_extcon, ChgPD_Info.AdapterVID);

            CHG_DBG("%s OEM_ASUS_AdapterVID_REQ. new type : 0x%x, old type : 0x%x\n", __func__, adaptervid_msg->VID, pre_adapter_id);
            if (adaptervid_msg->VID != pre_adapter_id) {
                if (adaptervid_msg->VID == 0x0b05) {
                    CHG_DBG("%s. Set VID quickchg_extcon state: 101\n", __func__);
                    if (ChgPD_Info.boot_completed == 1){
                        asus_extcon_set_state_sync(quickchg_extcon, 101);
                    }
                }

                if (IS_ENABLED(CONFIG_QTI_PMIC_GLINK_CLIENT_DEBUG) && qti_phy_bat)
                    power_supply_changed(qti_phy_bat);
            }
            pre_adapter_id = adaptervid_msg->VID;
        } else {
            pr_err("Incorrect response length %zu for OEM_ASUS_AdapterVID_REQ\n",
                len);
        }
        break;
    case OEM_JEITA_CC_STATE_REQ:
        if (len == sizeof(*jeita_cc_state_msg)) {
            jeita_cc_state_msg = data;
            CHG_DBG("%s jeita cc state : %d\n", __func__, jeita_cc_state_msg->state);
            ChgPD_Info.jeita_cc_state = jeita_cc_state_msg->state;
        } else {
            pr_err("Incorrect response length %zu for OEM_JEITA_CC_STATE_REQ\n",
                len);
        }
        break;
    case OEM_SET_CHARGER_TYPE_CHANGE:
        if (len == sizeof(*update_charger_type_msg)) {
            update_charger_type_msg = data;
            CHG_DBG("%s OEM_SET_CHARGER_TYPE_CHANGE. new type : %d, old type : %d\n", __func__, update_charger_type_msg->charger_type, pre_chg_type);
            if (update_charger_type_msg->charger_type != pre_chg_type) {
                switch (update_charger_type_msg->charger_type) {
                case ASUS_CHARGER_TYPE_LEVEL0:
                    g_SWITCH_LEVEL = SWITCH_LEVEL0_DEFAULT;
                break;
                case ASUS_CHARGER_TYPE_LEVEL1:
                    g_SWITCH_LEVEL = SWITCH_LEVEL2_QUICK_CHARGING;
                break;
                case ASUS_CHARGER_TYPE_LEVEL2:
                    g_SWITCH_LEVEL = SWITCH_LEVEL3_QUICK_CHARGING;
                break;
                case ASUS_CHARGER_TYPE_LEVEL3:
                    g_SWITCH_LEVEL = SWITCH_LEVEL4_QUICK_CHARGING;
                break;
                default:
                    g_SWITCH_LEVEL = SWITCH_LEVEL0_DEFAULT;
                break;
                }
                if (IS_ENABLED(CONFIG_QTI_PMIC_GLINK_CLIENT_DEBUG) && qti_phy_bat)
                    power_supply_changed(qti_phy_bat);
                cancel_delayed_work_sync(&asus_set_qc_state_work);
                if(ChgPD_Info.charger_mode){
                    schedule_delayed_work(&asus_set_qc_state_work, msecs_to_jiffies(0));
                }else{
                    schedule_delayed_work(&asus_set_qc_state_work, msecs_to_jiffies(100));
                }
            }
            pre_chg_type = update_charger_type_msg->charger_type;
        } else {
            pr_err("Incorrect response length %zu for OEM_SET_CHARGER_TYPE_CHANGE\n",
                len);
        }
        break;
    case OEM_CC_ORIENTATION:
        if (len == sizeof(*cc_orientation_msg)) {
            cc_orientation_msg = data;
            CHG_DBG("%s OEM_CC_ORIENTATION cc_orientation : %d\n", __func__, cc_orientation_msg->cc_orientation);
            ChgPD_Info.cc_orientation = cc_orientation_msg->cc_orientation;
        } else {
            pr_err("Incorrect response length %zu for OEM_SET_OTG_WA\n",
                len);
        }
        break;
    default:
        pr_err("Unknown opcode: %u\n", hdr->opcode);
        break;
    }

}

static void handle_message(struct battery_chg_dev *bcdev, void *data,
                size_t len)
{
    struct battman_oem_read_buffer_resp_msg *oem_read_buffer_resp_msg;
    struct battman_oem_write_buffer_resp_msg *oem_write_buffer_resp_msg;

    struct pmic_glink_hdr *hdr = data;
    bool ack_set = false;

    switch (hdr->opcode) {
    case OEM_OPCODE_READ_BUFFER:
        if (len == sizeof(*oem_read_buffer_resp_msg)) {
            oem_read_buffer_resp_msg = data;
            switch (oem_read_buffer_resp_msg->oem_property_id) {
            case BATTMAN_OEM_PM8350B_ICL:
                ChgPD_Info.pm8350b_icl = oem_read_buffer_resp_msg->data_buffer[0];
                ack_set = true;
                break;
            case BATTMAN_OEM_SMB1396_ICL:
                ChgPD_Info.smb1396_icl = oem_read_buffer_resp_msg->data_buffer[0];
                ack_set = true;
                break;
            case BATTMAN_OEM_FCC:
                ChgPD_Info.batt_fcc = oem_read_buffer_resp_msg->data_buffer[0];
                ack_set = true;
                break;
            case BATTMAN_OEM_USBIN_SUSPEND:
                ChgPD_Info.usbin_suspend_en = oem_read_buffer_resp_msg->data_buffer[0];
                ack_set = true;
                break;
            case BATTMAN_OEM_CHARGING_SUSPNED:
                CHG_DBG("%s BATTMAN_OEM_CHARGING_SUSPNED successfully\n", __func__);
                ChgPD_Info.charging_suspend_en = oem_read_buffer_resp_msg->data_buffer[0];
                ack_set = true;
                break;
            //ATD +++
            case BATTMAN_OEM_ADSP_PLATFORM_ID:
                ChgPD_Info.PlatformID = oem_read_buffer_resp_msg->data_buffer[0];
                ack_set = true;
                break;
            case BATTMAN_OEM_BATT_ID:
                ChgPD_Info.BATT_ID = oem_read_buffer_resp_msg->data_buffer[0];
                ack_set = true;
                if(ChgPD_Info.BATT_ID < 51000*1.15 && ChgPD_Info.BATT_ID > 51000*0.85)
                    asus_extcon_set_state_sync(bat_id_extcon, 1);
                else if(ChgPD_Info.BATT_ID < 100000*1.15 && ChgPD_Info.BATT_ID > 100000*0.85)
                    asus_extcon_set_state_sync(bat_id_extcon, 1);
                else
                    asus_extcon_set_state_sync(bat_id_extcon, 0);
                break;
                break;
            case BATTMAN_OEM_CHG_LIMIT_EN:
                CHG_DBG("%s BATTMAN_OEM_CHG_LIMIT_EN successfully\n", __func__);
                ChgPD_Info.chg_limit_en = oem_read_buffer_resp_msg->data_buffer[0];
                ack_set = true;
                break;
            case BATTMAN_OEM_CHG_LIMIT_CAP:
                CHG_DBG("%s BATTMAN_OEM_CHG_LIMIT_CAP successfully\n", __func__);
                ChgPD_Info.chg_limit_cap = oem_read_buffer_resp_msg->data_buffer[0];
                ack_set = true;
                break;
            //ATD ---
            default:
                ack_set = true;
                pr_err("Unknown property_id: %u\n", oem_read_buffer_resp_msg->oem_property_id);
            }
        } else {
            pr_err("Incorrect response length %zu for OEM_OPCODE_READ_BUFFER\n", len);
        }
        break;
    case OEM_OPCODE_WRITE_BUFFER:
        if (len == sizeof(*oem_write_buffer_resp_msg)) {
            oem_write_buffer_resp_msg = data;
            switch (oem_write_buffer_resp_msg->oem_property_id) {
            case BATTMAN_OEM_PM8350B_ICL:
            case BATTMAN_OEM_SMB1396_ICL:
            case BATTMAN_OEM_FCC:
            case BATTMAN_OEM_DEBUG_MASK:
            case BATTMAN_OEM_USBIN_SUSPEND:
            case BATTMAN_OEM_CHARGING_SUSPNED:
            case BATTMAN_OEM_Panel_Check:
            case BATTMAN_OEM_Write_PM8350B_Register:
            case BATTMAN_OEM_CHG_MODE:
            case BATTMAN_OEM_FV:
            case BATTMAN_OEM_In_Call:
            case BATTMAN_OEM_WORK_EVENT:
            case BATTMAN_OEM_CHG_LIMIT_EN:
            case BATTMAN_OEM_CHG_LIMIT_CAP:
            case BATTMAN_OEM_Batt_Protection:
            case BATTMAN_OEM_DEMOAPP:
            case BATTMAN_OEM_THERMAL_THRESHOLD:
            case BATTMAN_OEM_SLOW_CHG:
            case BATTMAN_OEM_THERMAL_ALERT:
            case BATTMAN_OEM_CHG_Disable_Jeita:
                CHG_DBG("%s set property:%d successfully\n", __func__, oem_write_buffer_resp_msg->oem_property_id);
                ack_set = true;
                break;
            default:
                ack_set = true;
                pr_err("Unknown property_id: %u\n", oem_write_buffer_resp_msg->oem_property_id);
            }
        } else {
            pr_err("Incorrect response length %zu for OEM_OPCODE_READ_BUFFER\n", len);
        }
        break;
    default:
        pr_err("Unknown opcode: %u\n", hdr->opcode);
        ack_set = true;
        break;
    }

    if (ack_set)
        complete(&bcdev->ack);
}

static int asusBC_msg_cb(void *priv, void *data, size_t len)
{
    struct pmic_glink_hdr *hdr = data;

    // pr_err("owner: %u type: %u opcode: %u len: %zu\n", hdr->owner, hdr->type, hdr->opcode, len);

    if (hdr->owner == PMIC_GLINK_MSG_OWNER_OEM) {
        if (hdr->type == MSG_TYPE_NOTIFY)
            handle_notification(g_bcdev, data, len);
        else
            handle_message(g_bcdev, data, len);
    }
    return 0;
}

static void asusBC_state_cb(void *priv, enum pmic_glink_state state)
{
    pr_err("Enter asusBC_state_cb\n");
}

int asus_chg_resume(struct device *dev)
{
	CHG_DBG_E("Enter asus_chg_resume\n");

	if(g_Charger_mode) {
		CHG_DBG_E("Charger mode , start asus timer monitor\n");
		schedule_delayed_work(&asus_min_check_work, msecs_to_jiffies(0));
	}
	
	return 0;
}
EXPORT_SYMBOL(asus_chg_resume);

//ASUS BSP : Show "+" on charging icon +++
void asus_set_qc_state_worker(struct work_struct *work)
{
    CHG_DBG_E("%s: VID=%d, level=%d, boot=%d\n", __func__, ChgPD_Info.AdapterVID, g_SWITCH_LEVEL, ChgPD_Info.boot_completed);
    if(ChgPD_Info.AdapterVID == 0xB05 &&
        g_SWITCH_LEVEL == SWITCH_LEVEL3_QUICK_CHARGING && ChgPD_Info.boot_completed){
        CHG_DBG_E("%s:  report 30W pps \n", __func__);
        asus_extcon_set_state_sync(quickchg_extcon, 101);//ASUS 30W
        mdelay(10);
    }

    if(ChgPD_Info.boot_completed || ChgPD_Info.charger_mode){
        asus_extcon_set_state_sync(quickchg_extcon, g_SWITCH_LEVEL);
    }

    CHG_DBG_E("%s: switch : %d\n", __func__, asus_extcon_get_state(quickchg_extcon));
}

static int pre_batt_status = -1;
void set_qc_stat(int status)
{
    if(!g_asuslib_init) return;
    if(status == pre_batt_status) return;

    CHG_DBG("%s: status: %d\n", __func__, status);
    pre_batt_status = status;
    switch (status) {
    //"qc" stat happends in charger mode only, refer to smblib_get_prop_batt_status
    case POWER_SUPPLY_STATUS_NOT_CHARGING:
	CHG_DBG_E("POWER_SUPPLY_STATUS_NOT_CHARGING \n");
	g_cos_over_full_flag = false;
	first_cos_48h_protect = false;
        cancel_delayed_work_sync(&asus_set_qc_state_work);
        schedule_delayed_work(&asus_set_qc_state_work, msecs_to_jiffies(150));
        break;
    case POWER_SUPPLY_STATUS_CHARGING:
    case POWER_SUPPLY_STATUS_FULL:
        cancel_delayed_work_sync(&asus_set_qc_state_work);
        schedule_delayed_work(&asus_set_qc_state_work, msecs_to_jiffies(150));
        break;
    default:
        break;
    }
}
EXPORT_SYMBOL(set_qc_stat);
//ASUS BSP : Show "+" on charging icon ---

int g_temp_Triger = 70000;
int g_temp_Release = 60000;

void asus_usb_thermal_worker(struct work_struct *work)
{
    int rc;
    u32 tmp;
    int conn_temp;

    rc = iio_read_channel_processed(usb_conn_temp_vadc_chan, &conn_temp);
    if (rc < 0)
        CHG_DBG_E("%s: iio_read_channel_processed fail\n", __func__);
    else
        CHG_DBG_E("%s: usb_conn_temp = %d\n", __func__, conn_temp);

    if (conn_temp > g_temp_Triger) {
        if(ChgPD_Info.cc_orientation){
            tmp = THERMAL_ALERT_WITH_AC;
            CHG_DBG("%s. set BATTMAN_OEM_THERMAL_ALERT : %d", __func__, tmp);
            rc = oem_prop_write(BATTMAN_OEM_THERMAL_ALERT, &tmp, 1);
            if (rc < 0) {
                pr_err("Failed to set BATTMAN_OEM_THERMAL_ALERT rc=%d\n", rc);
            }
            asus_extcon_set_state_sync(thermal_extcon, THERMAL_ALERT_WITH_AC);
        }else{
            tmp = THERMAL_ALERT_NO_AC;
            CHG_DBG("%s. set BATTMAN_OEM_THERMAL_ALERT : %d", __func__, tmp);
            rc = oem_prop_write(BATTMAN_OEM_THERMAL_ALERT, &tmp, 1);
            if (rc < 0) {
                pr_err("Failed to set BATTMAN_OEM_THERMAL_ALERT rc=%d\n", rc);
            }
            asus_extcon_set_state_sync(thermal_extcon, THERMAL_ALERT_NO_AC);
        }

        g_once_usb_thermal = true;
        CHG_DBG("conn_temp(%d) >= 70000, usb thermal alert\n", conn_temp);
    }else if(!ChgPD_Info.cc_orientation && conn_temp < g_temp_Release){
        tmp = THERMAL_ALERT_NONE;
        CHG_DBG("%s. set BATTMAN_OEM_THERMAL_ALERT : %d", __func__, tmp);
        rc = oem_prop_write(BATTMAN_OEM_THERMAL_ALERT, &tmp, 1);
        if (rc < 0) {
            pr_err("Failed to set BATTMAN_OEM_THERMAL_ALERT rc=%d\n", rc);
        }
        g_once_usb_thermal = false;

        asus_extcon_set_state_sync(thermal_extcon, THERMAL_ALERT_NONE);
        CHG_DBG("conn_temp(%d) <= 60000, disable usb suspend\n", conn_temp);
    }

    CHG_DBG("conn_temp(%d), usb(%d), otg(%d), usb_connector = %d\n",
            conn_temp, ChgPD_Info.cc_orientation, ChgPD_Info.otg_enable, asus_extcon_get_state(thermal_extcon));

    schedule_delayed_work(&asus_usb_thermal_work, msecs_to_jiffies(60000));
}

void static enter_ship_mode_worker(struct work_struct *dat)
{
    struct battery_charger_ship_mode_req_msg msg = { { 0 } };
    int rc;
    // int rc,usb_online;
    // union power_supply_propval prop = {};

    msg.hdr.owner = MSG_OWNER_BC;
    msg.hdr.type = MSG_TYPE_REQ_RESP;
    msg.hdr.opcode = 0x36;// = BC_SHIP_MODE_REQ_SET
    msg.ship_mode_type = 0;// = SHIP_MODE_PMIC

    // while(1)
    // {
    //     rc = power_supply_get_property(qti_phy_usb,
    //         POWER_SUPPLY_PROP_ONLINE, &prop);
    //     if (rc < 0) {
    //         pr_err("Failed to get usb  online, rc=%d\n", rc);
    //         return;
    //     }
    //     usb_online = prop.intval;
    //     if (usb_online == 0 )
    //         break;
    //     mdelay(1000);
    // }

    CHG_DBG_E("%s. usb plug out ,begin to set shoip mode\n", __func__);
    rc = battery_chg_write(g_bcdev, &msg, sizeof(msg));
    if (rc < 0)
        pr_err("%s. Failed to write SHIP mode: %d\n", rc);
}

static void asus_jeita_rule_worker(struct work_struct *dat){
    int rc;
    u32 tmp;
    int bat_cap;
    union power_supply_propval prop = {};

    tmp = WORK_JEITA_RULE;
    printk(KERN_ERR "[BAT][CHG]%s set BATTMAN_OEM_WORK_EVENT : WORK_JEITA_RULE", __func__);
    rc = oem_prop_write(BATTMAN_OEM_WORK_EVENT, &tmp, 1);
    if (rc < 0) {
        pr_err("Failed to set BATTMAN_OEM_WORK_EVENT WORK_JEITA_RULE rc=%d\n", rc);
    }

    schedule_delayed_work(&asus_jeita_rule_work, 60 * HZ);

    rc = power_supply_get_property(qti_phy_bat,
        POWER_SUPPLY_PROP_CAPACITY, &prop);
    if (rc < 0) {
        pr_err("Failed to get battery soc, rc=%d\n", rc);
        return;
    }
    bat_cap = prop.intval;

    if(ChgPD_Info.jeita_cc_state > S_JEITA_CC_STBY && bat_cap < 100){
        __pm_wakeup_event(chg_ws, 65 * 1000);
    }
}

static void asus_jeita_cc_worker(struct work_struct *dat){
    int rc;
    int tmp[2];

    tmp[0] = WORK_JEITA_CC;
    tmp[1] = 0;
    printk(KERN_ERR "[BAT][CHG]%s set BATTMAN_OEM_WORK_EVENT : WORK_JEITA_CC", __func__);
    rc = oem_prop_write(BATTMAN_OEM_WORK_EVENT, tmp, 2);
    if (rc < 0) {
        pr_err("Failed to set BATTMAN_OEM_WORK_EVENT WORK_JEITA_CC rc=%d\n", rc);
    }

    if(ChgPD_Info.jeita_cc_state == S_JEITA_CC_PRE_CHG){
        schedule_delayed_work(&asus_jeita_cc_work, HZ);
    }else{
        schedule_delayed_work(&asus_jeita_cc_work, 5 * HZ);
    }
}

//print_battery_status +++
static char *charging_stats[] = {
    "UNKNOWN",
    "CHARGING",
    "DISCHARGING",
    "NOT_CHARGING",
    "FULL"
};
char *health_type[] = {
    "UNKNOWN",
    "GOOD",
    "OVERHEAT",
    "DEAD",
    "OVERVOLTAGE",
    "UNSPEC_FAILURE",
    "COLD",
    "WATCHDOG_TIMER_EXPIRE",
    "SAFETY_TIMER_EXPIRE",
    "OVERCURRENT",
    "CALIBRATION_REQUIRED",
    "WARM",
    "COOL",
    "HOT"
};
static void print_battery_status(void) {
    union power_supply_propval prop = {};
    char battInfo[256];
    int bat_cap, fcc,bat_vol,bat_cur,bat_temp,charge_status,bat_health,rc = 0;

    rc = power_supply_get_property(qti_phy_bat,
        POWER_SUPPLY_PROP_CAPACITY, &prop);
    if (rc < 0) {
        pr_err("Failed to get battery SOC, rc=%d\n", rc);
        return;
    }
    bat_cap = prop.intval;

    rc = power_supply_get_property(qti_phy_bat,
        POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN, &prop);
    if (rc < 0) {
        pr_err("Failed to get battery full design, rc=%d\n", rc);
        return;
    }
    fcc = prop.intval;

    rc = power_supply_get_property(qti_phy_bat,
        POWER_SUPPLY_PROP_VOLTAGE_NOW, &prop);
    if (rc < 0) {
        pr_err("Failed to get battery vol , rc=%d\n", rc);
        return;
    }
    bat_vol = prop.intval;

    rc = power_supply_get_property(qti_phy_bat,
        POWER_SUPPLY_PROP_CURRENT_NOW, &prop);
    if (rc < 0) {
        pr_err("Failed to get battery current , rc=%d\n", rc);
        return;
    }
    bat_cur= prop.intval;

    rc = power_supply_get_property(qti_phy_bat,
        POWER_SUPPLY_PROP_TEMP, &prop);
    if (rc < 0) {
        pr_err("Failed to get battery temp , rc=%d\n", rc);
        return;
    }
    bat_temp= prop.intval;

    rc = power_supply_get_property(qti_phy_bat,
        POWER_SUPPLY_PROP_STATUS, &prop);
    if (rc < 0) {
        pr_err("Failed to get battery status , rc=%d\n", rc);
        return;
    }
    charge_status= prop.intval;

    rc = power_supply_get_property(qti_phy_bat,
        POWER_SUPPLY_PROP_HEALTH, &prop);
    if (rc < 0) {
        pr_err("Failed to get battery health , rc=%d\n", rc);
        return;
    }
    bat_health= prop.intval;

    snprintf(battInfo, sizeof(battInfo), "report Capacity ==>%d, FCC:%dmAh, BMS:%d, V:%dmV, Cur:%dmA, ",
            bat_cap,
            fcc/1000,
            bat_cap,
            bat_vol/1000,
            bat_cur/1000);
    snprintf(battInfo, sizeof(battInfo), "%sTemp:%d.%dC, BATID:%d, CHG_Status:%d(%s), BAT_HEALTH:%s \n",
            battInfo,
            bat_temp/10,
            bat_temp%10,
            ChgPD_Info.BATT_ID,
            charge_status,
            charging_stats[charge_status],
            health_type[bat_health]);

    // ASUSEvtlog("[BAT][Ser]%s", battInfo);
    pr_err("[BAT][Ser]%s", battInfo);

    schedule_delayed_work(&update_gauge_status_work, 60*HZ);
}

void static update_gauge_status_worker(struct work_struct *dat)
{
    print_battery_status();
}
//print_battery_status ---

static void asus_18W_workaround_worker(struct work_struct *dat){
    int rc;
    u32 tmp;

    tmp = WORK_18W_WORKAROUND;
    printk(KERN_ERR "[BAT][CHG]%s set BATTMAN_OEM_WORK_EVENT : WORK_18W_WORKAROUND", __func__);
    rc = oem_prop_write(BATTMAN_OEM_WORK_EVENT, &tmp, 1);
    if (rc < 0) {
        pr_err("Failed to set BATTMAN_OEM_WORK_EVENT WORK_18W_WORKAROUND rc=%d\n", rc);
    }
}

void asus_thermal_policy_worker(struct work_struct *work)
{
    int rc;
    u32 tmp;

    tmp = ChgPD_Info.thermal_threshold;
    CHG_DBG("%s. set BATTMAN_OEM_THERMAL_THRESHOLD : %d", __func__, tmp);
    rc = oem_prop_write(BATTMAN_OEM_THERMAL_THRESHOLD, &tmp, 1);
    if (rc < 0) {
        pr_err("Failed to set BATTMAN_OEM_THERMAL_THRESHOLD rc=%d\n", rc);
    }
}

static void asus_panel_check_worker(struct work_struct *dat){
    int rc;
    u32 tmp;

    tmp = WORK_PANEL_CHECK;
    printk(KERN_ERR "[BAT][CHG]%s set BATTMAN_OEM_WORK_EVENT : WORK_PANEL_CHECK", __func__);
    rc = oem_prop_write(BATTMAN_OEM_WORK_EVENT, &tmp, 1);
    if (rc < 0) {
        pr_err("Failed to set BATTMAN_OEM_WORK_EVENT WORK_PANEL_CHECK rc=%d\n", rc);
    }

    schedule_delayed_work(&asus_panel_check_work, 10 * HZ);
}

static void asus_slow_charging_worker(struct work_struct *dat){
    int rc;
    u32 tmp;

    tmp = ChgPD_Info.slow_chglimit;
    CHG_DBG("%s. set BATTMAN_OEM_SLOW_CHG : %d", __func__, tmp);
    rc = oem_prop_write(BATTMAN_OEM_SLOW_CHG, &tmp, 1);
    if (rc < 0) {
        pr_err("Failed to set BATTMAN_OEM_SLOW_CHG rc=%d\n", rc);
    }
}

static void asus_long_full_cap_monitor_worker(struct work_struct *dat){
    int rc;
    u32 tmp;

    tmp = WORK_LONG_FULL_CAP;
    CHG_DBG("%s set BATTMAN_OEM_WORK_EVENT : WORK_LONG_FULL_CAP", __func__);
    rc = oem_prop_write(BATTMAN_OEM_WORK_EVENT, &tmp, 1);
    if (rc < 0) {
        pr_err("Failed to set BATTMAN_OEM_WORK_EVENT WORK_LONG_FULL_CAP rc=%d\n", rc);
    }

    cancel_delayed_work(&asus_long_full_cap_monitor_work);
    schedule_delayed_work(&asus_long_full_cap_monitor_work, 30 * HZ);
}

//start plugin work +++
void asus_monitor_start(int status){
    if(!g_asuslib_init) return;
    if (asus_usb_online == status) return;

    asus_usb_online = status;
    printk(KERN_ERR "[BAT][CHG] asus_monitor_start %d\n", asus_usb_online);
    if(asus_usb_online){
        cancel_delayed_work_sync(&asus_jeita_rule_work);
        schedule_delayed_work(&asus_jeita_rule_work, 0);

        cancel_delayed_work_sync(&asus_18W_workaround_work);
        schedule_delayed_work(&asus_18W_workaround_work, 21 * HZ);

        if(!ChgPD_Info.charger_mode){
            cancel_delayed_work_sync(&asus_panel_check_work);
            schedule_delayed_work(&asus_panel_check_work, 61 * HZ);
        }

        cancel_delayed_work_sync(&asus_slow_charging_work);
        schedule_delayed_work(&asus_slow_charging_work, 0 * HZ);

        qti_charge_notify_device_charge();
        __pm_wakeup_event(chg_ws, 60 * 1000);
    }else{
        cancel_delayed_work_sync(&asus_jeita_rule_work);
        cancel_delayed_work_sync(&asus_jeita_cc_work);
        cancel_delayed_work_sync(&asus_18W_workaround_work);
        cancel_delayed_work_sync(&asus_thermal_policy_work);
        cancel_delayed_work_sync(&asus_panel_check_work);
        cancel_delayed_work_sync(&asus_slow_charging_work);

        qti_charge_notify_device_not_charge();
    }
}
EXPORT_SYMBOL(asus_monitor_start);
//start plugin work ---

unsigned long full_cap_start_time;

void monitor_charging_enable(void)
{
    union power_supply_propval prop = {};
    int bat_capacity;
    unsigned long  temp_time;
    u32 tmp = 0;
    int rc;


    if (g_Charger_mode) {
        rc = power_supply_get_property(qti_phy_bat, POWER_SUPPLY_PROP_CAPACITY, &prop);
        if (rc < 0)
	    pr_err("Failed to get battery SOC, rc=%d\n", rc);
        bat_capacity = prop.intval;

       if (bat_capacity == 100 && !g_cos_over_full_flag) {
            full_cap_start_time = asus_qpnp_rtc_read_time();
	    g_cos_over_full_flag = true;
       }

       if((bat_capacity == 100) && g_cos_over_full_flag && (!first_cos_48h_protect)) {
	    CHG_DBG_E("Start 48h full cap detect time is %ld\n",full_cap_start_time);
	    temp_time = asus_qpnp_rtc_read_time();
	    if((temp_time - full_cap_start_time) >= 172800) {
		CHG_DBG_E("Detect 48h full cap has over 48h, start to charger suspend !\n");
		tmp = 1;
                ChgPD_Info.ultra_bat_life = tmp;
                tmp = ChgPD_Info.ultra_bat_life;
		printk("%s. set BATTMAN_OEM_Batt_Protection : %d", __func__, tmp);
		rc = oem_prop_write(BATTMAN_OEM_Batt_Protection, &tmp, 1);
		if (rc < 0) {
			printk("Failed to set BATTMAN_OEM_Batt_Protection rc=%d\n", rc);
		}

		first_cos_48h_protect = true;
	    }
       }

    }
}

void asus_min_check_worker(struct work_struct *work)
{
    monitor_charging_enable();//Always TRUE, JEITA is decided on ADSP
}

static int battery_chg_write(struct battery_chg_dev *bcdev, void *data,
                int len)
{
    int rc;

    /*
     * When the subsystem goes down, it's better to return the last
     * known values until it comes back up. Hence, return 0 so that
     * pmic_glink_write() is not attempted until pmic glink is up.
     */
    if (atomic_read(&bcdev->state) == PMIC_GLINK_STATE_DOWN) {
        pr_debug("glink state is down\n");
        return 0;
    }

    if (bcdev->debug_battery_detected && bcdev->block_tx)
        return 0;

    mutex_lock(&bcdev->rw_lock);
    reinit_completion(&bcdev->ack);
    rc = pmic_glink_write(bcdev->client, data, len);
    if (!rc) {
        rc = wait_for_completion_timeout(&bcdev->ack,
                    msecs_to_jiffies(BC_WAIT_TIME_MS));
        if (!rc) {
            pr_err("Error, timed out sending message\n");
            mutex_unlock(&bcdev->rw_lock);
            return -ETIMEDOUT;
        }

        rc = 0;
    }
    mutex_unlock(&bcdev->rw_lock);

    return rc;
}

static int read_property_id(struct battery_chg_dev *bcdev,
            struct psy_state *pst, u32 prop_id)
{
    struct battery_charger_req_msg req_msg = { { 0 } };

    req_msg.property_id = prop_id;
    req_msg.battery_id = 0;
    req_msg.value = 0;
    req_msg.hdr.owner = MSG_OWNER_BC;
    req_msg.hdr.type = MSG_TYPE_REQ_RESP;
    req_msg.hdr.opcode = pst->opcode_get;

    pr_debug("psy: %s prop_id: %u\n", pst->psy->desc->name,
        req_msg.property_id);

    return battery_chg_write(bcdev, &req_msg, sizeof(req_msg));
}

ssize_t oem_prop_read(enum battman_oem_property prop, size_t count)
{
    struct battman_oem_read_buffer_req_msg req_msg = { { 0 } };
    int rc;

    req_msg.hdr.owner = PMIC_GLINK_MSG_OWNER_OEM;
    req_msg.hdr.type = MSG_TYPE_REQ_RESP;
    req_msg.hdr.opcode = OEM_OPCODE_READ_BUFFER;
    req_msg.oem_property_id = prop;
    req_msg.data_size = count;

    rc = battery_chg_write(g_bcdev, &req_msg, sizeof(req_msg));
    if (rc < 0) {
        pr_err("Failed to read buffer rc=%d\n", rc);
        return rc;
    }

    return count;
}

ssize_t oem_prop_write(enum battman_oem_property prop,
                    u32 *buf, size_t count)
{
    struct battman_oem_write_buffer_req_msg req_msg = { { 0 } };
    int rc;

    req_msg.hdr.owner = PMIC_GLINK_MSG_OWNER_OEM;
    req_msg.hdr.type = MSG_TYPE_REQ_RESP;
    req_msg.hdr.opcode = OEM_OPCODE_WRITE_BUFFER;
    req_msg.oem_property_id = prop;
    memcpy(req_msg.data_buffer, buf, sizeof(u32)*count);
    req_msg.data_size = count;

    if (g_bcdev == NULL) {
        pr_err("g_bcdev is null\n");
        return -1;
    }
    rc = battery_chg_write(g_bcdev, &req_msg, sizeof(req_msg));
    if (rc < 0) {
        pr_err("Failed to write buffer rc=%d\n", rc);
        return rc;
    }

    return count;
}

int asuslib_init(void) {
    int rc = 0;
    struct pmic_glink_client_data client_data = { };
    struct pmic_glink_client    *client;

    printk(KERN_ERR "%s +++\n", __func__);
    // Initialize the necessary power supply
    rc = asus_init_power_supply_prop();
    if (rc < 0) {
        pr_err("Failed to init power_supply chains\n");
        return rc;
    }

    // Register the class node
    rc = class_register(&asuslib_class);
    if (rc) {
        pr_err("%s: Failed to register asuslib class\n", __func__);
        return -1;
    }

    OTG_LOAD_SWITCH_GPIO = of_get_named_gpio(g_bcdev->dev->of_node, "OTG_LOAD_SWITCH", 0);
    rc = gpio_request(OTG_LOAD_SWITCH_GPIO, "OTG_LOAD_SWITCH");
    if (rc) {
        pr_err("%s: Failed to initalize the OTG_LOAD_SWITCH\n", __func__);
        return -1;
    }

    if (gpio_is_valid(OTG_LOAD_SWITCH_GPIO)) {
        rc = gpio_direction_output(OTG_LOAD_SWITCH_GPIO, 0);
        if (rc)
            pr_err("%s. Failed to control OTG_Load_Switch\n", __func__);
    } else {
        CHG_DBG_E("%s. OTG_LOAD_SWITCH_GPIO is invalid\n", __func__);
    }

    //quickchg_extcon
    quickchg_extcon = extcon_asus_dev_allocate();
    if (IS_ERR(quickchg_extcon)) {
        rc = PTR_ERR(quickchg_extcon);
        printk(KERN_ERR "[BAT][CHG] failed to allocate ASUS quickchg extcon device rc=%d\n", rc);
    }
    quickchg_extcon->name = "quick_charging";
    rc = extcon_asus_dev_register(quickchg_extcon);
    if (rc < 0)
        printk(KERN_ERR "[BAT][CHG] failed to register ASUS quickchg extcon device rc=%d\n", rc);

    asus_extcon_set_state_sync(quickchg_extcon, SWITCH_LEVEL0_DEFAULT);

    INIT_DELAYED_WORK(&asus_set_qc_state_work, asus_set_qc_state_worker);

    chg_ws = wakeup_source_register(g_bcdev->dev, "charge_wakelock");

    //bat_extcon
    bat_extcon = extcon_asus_dev_allocate();
    if (IS_ERR(bat_extcon)) {
        rc = PTR_ERR(bat_extcon);
    }
    bat_extcon->name = "battery";
    rc = extcon_asus_dev_register(bat_extcon);
    bat_extcon->name = st_battery_name;

    //bat_id_extcon
    bat_id_extcon = extcon_asus_dev_allocate();
    if (IS_ERR(bat_id_extcon)) {
        rc = PTR_ERR(bat_id_extcon);
    }
    bat_id_extcon->name = "battery_id";
    rc = extcon_asus_dev_register(bat_id_extcon);
    if (rc < 0)
        printk(KERN_ERR "[BAT][CHG] failed to register bat_id_extcon device rc=%d\n", rc);

    //adaptervid_extcon
    adaptervid_extcon = extcon_asus_dev_allocate();
    if (IS_ERR(adaptervid_extcon)) {
        rc = PTR_ERR(adaptervid_extcon);
        printk(KERN_ERR "[BAT][CHG] failed to allocate ASUS adaptervid extcon device rc=%d\n", rc);
    }
    adaptervid_extcon->name = "adaptervid";
    rc = extcon_asus_dev_register(adaptervid_extcon);
    if (rc < 0)
        printk(KERN_ERR "[BAT][CHG] failed to register ASUS adaptervid extcon device rc=%d\n", rc);
    asus_extcon_set_state_sync(adaptervid_extcon, 0);

    //[+++]Register the extcon for thermal alert
    thermal_extcon = extcon_asus_dev_allocate();
    if (IS_ERR(thermal_extcon)) {
        rc = PTR_ERR(thermal_extcon);
        printk(KERN_ERR "[BAT][CHG] failed to allocate ASUS thermal alert extcon device rc=%d\n", rc);
    }
    thermal_extcon->name = "usb_connector";
    rc = extcon_asus_dev_register(thermal_extcon);
    if (rc < 0)
        printk(KERN_ERR "[BAT][CHG] failed to register ASUS thermal alert extcon device rc=%d\n", rc);

   //[+++] Init the PMIC-GLINK
    client_data.id = PMIC_GLINK_MSG_OWNER_OEM;
    client_data.name = "asus_BC";
    client_data.msg_cb = asusBC_msg_cb;
    client_data.priv = g_bcdev;
    client_data.state_cb = asusBC_state_cb;
    client = pmic_glink_register_client(g_bcdev->dev, &client_data);
    if (IS_ERR(client)) {
        rc = PTR_ERR(client);
        if (rc != -EPROBE_DEFER)
            dev_err(g_bcdev->dev, "Error in registering with pmic_glink %d\n",
                rc);
        return rc;
    }
    //[---] Init the PMIC-GLINK

    usb_conn_temp_vadc_chan = iio_channel_get(g_bcdev->dev, "pm8350b_amux_thm4");
    if (IS_ERR_OR_NULL(usb_conn_temp_vadc_chan)) {
        CHG_DBG_E("%s: usb_conn_temp iio_channel_get fail\n", __func__);
    }else{
        INIT_DELAYED_WORK(&asus_usb_thermal_work, asus_usb_thermal_worker);
        schedule_delayed_work(&asus_usb_thermal_work, msecs_to_jiffies(0));
    }
    //[---]Register the extcon for thermal alert

    asus_get_Batt_ID();

    //register drm notifier
    INIT_DELAYED_WORK(&asus_set_panelonoff_current_work, asus_set_panelonoff_current_worker);
    RegisterDRMCallback();

    //jeita rule work
    INIT_DELAYED_WORK(&asus_jeita_rule_work, asus_jeita_rule_worker);
    INIT_DELAYED_WORK(&asus_jeita_cc_work, asus_jeita_cc_worker);

    INIT_DELAYED_WORK(&enter_ship_work, enter_ship_mode_worker);

    INIT_DELAYED_WORK(&update_gauge_status_work, update_gauge_status_worker);
    schedule_delayed_work(&update_gauge_status_work, 0);

    //18W workaround work
    INIT_DELAYED_WORK(&asus_18W_workaround_work, asus_18W_workaround_worker);

    //thermal policy
    INIT_DELAYED_WORK(&asus_thermal_policy_work, asus_thermal_policy_worker);

    //panel check work
    INIT_DELAYED_WORK(&asus_panel_check_work, asus_panel_check_worker);

    //slow charging work
    INIT_DELAYED_WORK(&asus_slow_charging_work, asus_slow_charging_worker);

    //implement the asus owns algorithm of detection full capacity
    INIT_DELAYED_WORK(&asus_long_full_cap_monitor_work, asus_long_full_cap_monitor_worker);
    schedule_delayed_work(&asus_long_full_cap_monitor_work, msecs_to_jiffies(0));

    //cos battery 48hours protect
    INIT_DELAYED_WORK(&asus_min_check_work, asus_min_check_worker);

    CHG_DBG_E("Load the asuslib_init Succesfully\n");
    g_asuslib_init = true;
    return rc;
}
EXPORT_SYMBOL(asuslib_init);

int asuslib_deinit(void) {
    g_asuslib_init = false;
    class_unregister(&asuslib_class);
    wakeup_source_unregister(chg_ws);
    return 0;
}
EXPORT_SYMBOL(asuslib_deinit);

MODULE_LICENSE("GPL v2");
