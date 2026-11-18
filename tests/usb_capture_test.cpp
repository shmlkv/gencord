// Runs the actual usb_mic.cpp producer/stop/format-selection with synthetic USB
// APIs and real host threads/queues. This is NOT a USB/hardware compatibility test.
#include "usb_mic.h"
#include "usb/uac_host.h"
#include "esp_private/usb_phy.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
using namespace std::chrono;
struct FakeQueue {size_t cap,item;std::mutex mutex;std::deque<std::vector<uint8_t>> items;};
static std::vector<std::unique_ptr<FakeQueue>> queues;
static std::vector<std::thread> tasks;
static std::mutex inputMutex;
static std::vector<uint8_t> input;
static std::atomic<size_t> consumed{0};
static unsigned channels=2;
static DeviceCb callback;
static int starts=0;
uint32_t millis() { static const auto t=steady_clock::now();return duration_cast<milliseconds>(steady_clock::now()-t).count(); }
void vTaskDelay(unsigned t) {std::this_thread::sleep_for(milliseconds(t));}
void vTaskDelete(void*) {}
int xTaskCreatePinnedToCore(void (*fn)(void*),const char*,unsigned,void*,unsigned,TaskHandle_t*,int) {
  tasks.emplace_back([fn]{fn(nullptr);});return pdPASS;
}
QueueHandle_t xQueueCreate(size_t cap,size_t item) {
  auto q=std::make_unique<FakeQueue>();q->cap=cap;q->item=item;auto p=q.get();queues.push_back(std::move(q));return p;
}
int xQueueSend(QueueHandle_t q,const void* p,unsigned) {
  std::lock_guard<std::mutex> l(q->mutex);
  if(q->items.size()==q->cap) return pdFALSE;
  const auto b=static_cast<const uint8_t*>(p);q->items.emplace_back(b,b+q->item);return pdTRUE;
}
int xQueueReceive(QueueHandle_t q,void* p,unsigned) {
  std::lock_guard<std::mutex> l(q->mutex);if(q->items.empty()) return pdFALSE;
  std::memcpy(p,q->items.front().data(),q->item);q->items.pop_front();return pdTRUE;
}
esp_err_t usb_host_lib_handle_events(unsigned t,uint32_t* flags) {vTaskDelay(t);*flags=2;return ESP_OK;}
esp_err_t usb_host_device_free_all(){return ESP_OK;}
esp_err_t usb_host_install(const usb_host_config_t*){return ESP_OK;}
esp_err_t usb_host_uninstall(){return ESP_OK;}
esp_err_t usb_new_phy(const usb_phy_config_t*,void** p){*p=reinterpret_cast<void*>(1);return ESP_OK;}
esp_err_t usb_del_phy(void*){return ESP_OK;}
esp_err_t uac_host_install(const uac_host_driver_config_t* c){c->callback(1,2,UAC_HOST_DRIVER_EVENT_RX_CONNECTED,nullptr);return ESP_OK;}
esp_err_t uac_host_uninstall(){return ESP_OK;}
esp_err_t uac_host_device_open(const uac_host_device_config_t* c,void** p){callback=c->callback;*p=reinterpret_cast<void*>(2);return ESP_OK;}
esp_err_t uac_host_device_start(void*,const uac_host_stream_config_t* c){assert(c->channels==2);++starts;return ESP_OK;}
esp_err_t uac_host_device_stop(void*){return ESP_OK;}
esp_err_t uac_host_device_close(void*){return ESP_OK;}
esp_err_t uac_host_get_device_info(void*,uac_host_dev_info_t* d){*d={};d->iface_alt_num=1;return ESP_OK;}
esp_err_t uac_host_get_device_alt_param(void*,unsigned,uac_host_dev_alt_param_t* p){
  *p={};p->format=1;p->channels=channels;p->subframe_size=2;p->bit_resolution=16;p->sample_freq_type=1;p->sample_freq[0]=16000;return ESP_OK;
}
esp_err_t uac_host_device_read(void*,uint8_t* p,size_t,uint32_t* got,unsigned){
  std::lock_guard<std::mutex> l(inputMutex);
  const size_t n=consumed.load();if(n==input.size()){*got=0;return ESP_ERR_TIMEOUT;}
  // Every USB read fragments a stereo frame, even between sample bytes.
  *p=input[n];*got=1;consumed=n+1;return ESP_OK;
}
static void join(){for(auto& t:tasks)t.join();tasks.clear();}
static void waitUntil(bool (*predicate)()) {const auto end=steady_clock::now()+seconds(3);while(!predicate()){assert(steady_clock::now()<end);vTaskDelay(1);}}
static bool allConsumed(){return consumed==input.size();}
static bool failed(){return usbMicLost();}
static void prepare(unsigned frames) {
  std::lock_guard<std::mutex> l(inputMutex);input.clear();consumed=0;
  for(unsigned i=0;i<frames;++i) {
    for(int16_t value:{int16_t(i*100+10),int16_t(-int(i*100)-20)}) {
      input.push_back(uint16_t(value)&255);input.push_back(uint16_t(value)>>8);
    }
  }
}
static void stopCase(int mode) {
  prepare(mode==3 ? 20 : (mode==4 ? 0 : 5));
  auto free=xQueueCreate(2,sizeof(int16_t*)), filled=xQueueCreate(2,sizeof(AudioBlock));
  int16_t buffers[2][8];
  for(auto& b:buffers){int16_t* p=b;assert(xQueueSend(free,&p,0)==pdTRUE);}
  assert(usbMicStart(free,filled,8,100));
  if(mode==3 || mode==4) waitUntil(failed);
  else {waitUntil(allConsumed);vTaskDelay(10);}
  if(mode==1) callback(nullptr,UAC_HOST_DRIVER_EVENT_DISCONNECTED,nullptr);
  if(mode==2) callback(nullptr,UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR,nullptr);
  if(mode==1 || mode==2) waitUntil(failed);
  usbMicStop();join();
  assert(!usbMicActive());assert(usbMicLost()==(mode!=0));
  std::vector<int16_t> out;AudioBlock b;
  while(xQueueReceive(filled,&b,0)==pdTRUE) out.insert(out.end(),b.data,b.data+b.samples);
  assert(out.size()==(mode==3 ? 16 : (mode==4 ? 0 : 10)));
  for(size_t i=0;i<out.size()/2;++i){assert(out[i*2]==int16_t(i*100+10));assert(out[i*2+1]==-int(i*100)-20);}
  if(mode!=0) assert(usbMicLost()); // terminal fault remains visible after shutdown
}
int main(){
  channels=1;auto free=xQueueCreate(1,sizeof(int16_t*)),filled=xQueueCreate(1,sizeof(AudioBlock));
  assert(!usbMicStart(free,filled,8,100));join();assert(starts==0);assert(usbMicLost());
  channels=2;
  for(int mode=0;mode<5;++mode) stopCase(mode);
  puts("PASS actual USB producer: mono rejected before stream, fragmented reads, short final blocks, normal/disconnect/transfer/overflow/stall, restart");
}
