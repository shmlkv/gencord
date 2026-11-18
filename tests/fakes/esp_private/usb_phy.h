#pragma once
#include "usb/usb_host.h"
using usb_phy_handle_t=void*;
struct usb_phy_config_t {int controller,target;};
constexpr int USB_PHY_CTRL_SERIAL_JTAG=1,USB_PHY_TARGET_INT=1;
esp_err_t usb_new_phy(const usb_phy_config_t*,void**);
esp_err_t usb_del_phy(void*);
