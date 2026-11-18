#pragma once
#include <cstdint>
constexpr int ESP_OK=0,ESP_ERR_TIMEOUT=1,ESP_FAIL=2,ESP_INTR_FLAG_LEVEL1=1;
using esp_err_t=int;
constexpr unsigned USB_HOST_LIB_EVENT_FLAGS_ALL_FREE=2;
struct usb_host_config_t {int intr_flags=0;};
esp_err_t usb_host_lib_handle_events(unsigned,uint32_t*);
esp_err_t usb_host_device_free_all();
esp_err_t usb_host_install(const usb_host_config_t*);
esp_err_t usb_host_uninstall();
