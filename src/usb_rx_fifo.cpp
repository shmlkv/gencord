// IDF 5.4 Arduino prebuilt USB library selects Periodic-OUT bias (128-byte IN MPS).
// Public usb_host_config_t has no FIFO selector in this SDK. At the existing HAL
// configuration boundary, force RX bias for this input-only application. The real
// HAL still configures hardware and publishes matching MPS limits. GNU ld --wrap
// redirects every HCD initialization/reset call, without patching SDK archives.
#include <esp_idf_version.h>
#include <hal/usb_dwc_hal.h>
#if ESP_IDF_VERSION_MAJOR != 5 || ESP_IDF_VERSION_MINOR != 4
#error "RX FIFO wrapper audited only for pinned ESP-IDF 5.4"
#endif
extern "C" void __real_usb_dwc_hal_set_fifo_bias(usb_dwc_hal_context_t*, usb_hal_fifo_bias_t);
extern "C" void __wrap_usb_dwc_hal_set_fifo_bias(usb_dwc_hal_context_t* hal, usb_hal_fifo_bias_t) {
  __real_usb_dwc_hal_set_fifo_bias(hal, USB_HAL_FIFO_BIAS_RX);
}
