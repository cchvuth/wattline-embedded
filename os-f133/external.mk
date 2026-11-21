include $(sort $(wildcard $(BR2_EXTERNAL_F133_PATH)/package/*/*.mk))
# wpa_supplicant's AP mode (dash-aa's Android Auto AP) without Wi-Fi Direct: Buildroot's AP option turns CONFIG_P2P on
# too, 1.1 MB more in the root (the NOR's rootfs partition has no room for it) for nothing used here
WPA_SUPPLICANT_CONFIG_ENABLE := $(filter-out CONFIG_P2P,$(WPA_SUPPLICANT_CONFIG_ENABLE))
WPA_SUPPLICANT_CONFIG_DISABLE += CONFIG_P2P
