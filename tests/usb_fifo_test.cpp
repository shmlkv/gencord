#include "hal/usb_dwc_hal.h"
#include <cassert>
#include <initializer_list>
#include <cstdio>
static int calls=0;
extern "C" void __wrap_usb_dwc_hal_set_fifo_bias(usb_dwc_hal_context_t*,usb_hal_fifo_bias_t);
extern "C" void __real_usb_dwc_hal_set_fifo_bias(usb_dwc_hal_context_t* h,usb_hal_fifo_bias_t b) {
  assert(h->test_marker==42); assert(b==USB_HAL_FIFO_BIAS_RX);++calls;
}
int main() {
  usb_dwc_hal_context_t h{42};
  for(auto bias:{USB_HAL_FIFO_BIAS_DEFAULT,USB_HAL_FIFO_BIAS_PTX,USB_HAL_FIFO_BIAS_RX})
    __wrap_usb_dwc_hal_set_fifo_bias(&h,bias);
  assert(calls==3); puts("PASS IDF54 RX FIFO override delegates init/reset to real HAL with RX bias");
}
