#pragma once
#include <cwchar>
#include "usb_host.h"
constexpr unsigned UAC_FREQ_NUM_MAX=8,UAC_STR_DESC_MAX_LENGTH=32;
using uac_host_device_handle_t=void*;
enum uac_host_driver_event_t {UAC_HOST_DRIVER_EVENT_RX_CONNECTED};
enum uac_host_device_event_t {UAC_HOST_DEVICE_EVENT_RX_DONE,UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR,UAC_HOST_DRIVER_EVENT_DISCONNECTED};
using DriverCb=void (*)(uint8_t,uint8_t,uac_host_driver_event_t,void*);
using DeviceCb=void (*)(void*,uac_host_device_event_t,void*);
struct uac_host_driver_config_t {bool create_background_task;unsigned task_priority,stack_size;int core_id;DriverCb callback;};
struct uac_host_device_config_t {uint8_t addr,iface_num;unsigned buffer_size,buffer_threshold;DeviceCb callback;};
struct uac_host_dev_info_t {uint16_t VID,PID;wchar_t iProduct[32];uint8_t iface_alt_num;};
struct uac_host_dev_alt_param_t {uint8_t format,channels,subframe_size,bit_resolution,sample_freq_type;uint32_t sample_freq_lower,sample_freq_upper,sample_freq[8];};
struct uac_host_stream_config_t {uint8_t channels,bit_resolution;uint32_t sample_freq;};
esp_err_t uac_host_install(const uac_host_driver_config_t*);
esp_err_t uac_host_uninstall();
esp_err_t uac_host_device_open(const uac_host_device_config_t*,void**);
esp_err_t uac_host_device_start(void*,const uac_host_stream_config_t*);
esp_err_t uac_host_device_stop(void*);
esp_err_t uac_host_device_close(void*);
esp_err_t uac_host_device_read(void*,uint8_t*,size_t,uint32_t*,unsigned);
esp_err_t uac_host_get_device_info(void*,uac_host_dev_info_t*);
esp_err_t uac_host_get_device_alt_param(void*,unsigned,uac_host_dev_alt_param_t*);
