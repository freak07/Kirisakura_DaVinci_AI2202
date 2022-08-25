LOCAL_PATH := $(call my-dir)

$(shell mkdir -p $(TARGET_OUT_VENDOR)/firmware/wlan/qca_cld/qca6490;)

ifeq ($(TARGET_SKU),CN)
$(shell cp $(LOCAL_PATH)/WCNSS_qcom_cfg_CN.ini $(TARGET_OUT_VENDOR)/firmware/wlan/qca_cld/qca6490/WCNSS_qcom_cfg.ini)
else
$(shell cp $(LOCAL_PATH)/WCNSS_qcom_cfg.ini $(TARGET_OUT_VENDOR)/firmware/wlan/qca_cld/qca6490/WCNSS_qcom_cfg.ini)
endif

ifeq ($(ASUS_BUILD_PROJECT),AI2201)
$(shell mkdir -p $(TARGET_OUT_VENDOR)/firmware_dallas/qca6490;)

$(shell cp $(LOCAL_PATH)/bdwlan_rog6.elf $(TARGET_OUT_VENDOR)/firmware/bdwlan.elf)
$(shell cp $(LOCAL_PATH)/bdwlang_rog6.elf $(TARGET_OUT_VENDOR)/firmware/bdwlang.elf)
$(shell cp $(LOCAL_PATH)/bdwlan_rog6_8475.elf $(TARGET_OUT_VENDOR)/firmware_dallas/qca6490/bdwlan.elf)
$(shell cp $(LOCAL_PATH)/bdwlang_rog6_8475.elf $(TARGET_OUT_VENDOR)/firmware_dallas/qca6490/bdwlang.elf)

$(shell cp $(LOCAL_PATH)/amss20.bin $(TARGET_OUT_VENDOR)/firmware_dallas/qca6490/)
$(shell cp $(LOCAL_PATH)/m3.bin $(TARGET_OUT_VENDOR)/firmware_dallas/qca6490/)
$(shell cp $(LOCAL_PATH)/Data20.msc $(TARGET_OUT_VENDOR)/firmware_dallas/qca6490/)
$(shell cp $(LOCAL_PATH)/regdb.bin $(TARGET_OUT_VENDOR)/firmware_dallas/qca6490/)
endif

ifeq ($(ASUS_BUILD_PROJECT),AI2202)
$(shell mkdir -p $(TARGET_OUT_VENDOR)/firmware_waipio/qca6490;)
$(shell mkdir -p $(TARGET_OUT_VENDOR)/firmware/qca6490;)

$(shell cp $(LOCAL_PATH)/bdwlan_zf9_8475.elf $(TARGET_OUT_VENDOR)/firmware/bdwlan.elf)
$(shell cp $(LOCAL_PATH)/bdwlang_zf9_8475.elf $(TARGET_OUT_VENDOR)/firmware/bdwlang.elf)
$(shell cp $(LOCAL_PATH)/bdwlan_zf9.elf $(TARGET_OUT_VENDOR)/firmware_waipio/bdwlan.elf)
$(shell cp $(LOCAL_PATH)/bdwlang_zf9.elf $(TARGET_OUT_VENDOR)/firmware_waipio/bdwlang.elf)
$(shell cp $(LOCAL_PATH)/bdwlan_zf9_8475.elf $(TARGET_OUT_VENDOR)/firmware/qca6490/bdwlan.elf)
$(shell cp $(LOCAL_PATH)/bdwlang_zf9_8475.elf $(TARGET_OUT_VENDOR)/firmware/qca6490/bdwlang.elf)
$(shell cp $(LOCAL_PATH)/bdwlan_zf9.elf $(TARGET_OUT_VENDOR)/firmware_waipio/qca6490/bdwlan.elf)
$(shell cp $(LOCAL_PATH)/bdwlang_zf9.elf $(TARGET_OUT_VENDOR)/firmware_waipio/qca6490/bdwlang.elf)

$(shell cp $(LOCAL_PATH)/amss20.bin $(TARGET_OUT_VENDOR)/firmware/qca6490/)
$(shell cp $(LOCAL_PATH)/m3.bin $(TARGET_OUT_VENDOR)/firmware/qca6490/)
$(shell cp $(LOCAL_PATH)/Data20.msc $(TARGET_OUT_VENDOR)/firmware/qca6490/)
$(shell cp $(LOCAL_PATH)/regdb.bin $(TARGET_OUT_VENDOR)/firmware/qca6490/)
endif
