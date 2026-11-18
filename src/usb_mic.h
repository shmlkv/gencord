// ADV USB-only capture. No built-in microphone fallback, no channel duplication.
#pragma once
#include <Arduino.h>
#include <freertos/queue.h>
#include "pcm_pipeline.h"
struct UsbMicInfo {
  uint16_t vid = 0, pid = 0;
  uint32_t rate = 0;
  uint8_t channels = 0, bits = 0;
  char product[33] = "";
};
// freeQ contains int16_t*. filledQ contains AudioBlock, including the final short block.
struct AudioBlock { int16_t* data; size_t samples; };
bool usbMicStart(QueueHandle_t freeQ, QueueHandle_t filledQ, size_t bufSamples, uint32_t waitMs);
void usbMicStop();
bool usbMicActive();
bool usbMicLost(); // any terminal stream error, not just disconnect
uint32_t usbMicDrops();
const char* usbMicError();
const UsbMicInfo& usbMicInfo();
