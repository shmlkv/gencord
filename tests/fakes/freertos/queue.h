#pragma once
#include <cstddef>
#include <cstdint>
using TickType_t=uint32_t;
using TaskHandle_t=void*;
struct FakeQueue;
using QueueHandle_t=FakeQueue*;
constexpr int pdTRUE=1,pdPASS=1,pdFALSE=0;
#define pdMS_TO_TICKS(x) (x)
int xQueueSend(QueueHandle_t,const void*,TickType_t);
int xQueueReceive(QueueHandle_t,void*,TickType_t);
QueueHandle_t xQueueCreate(size_t,size_t);
void vTaskDelay(TickType_t);
void vTaskDelete(void*);
int xTaskCreatePinnedToCore(void (*)(void*),const char*,unsigned,void*,unsigned,TaskHandle_t*,int);
