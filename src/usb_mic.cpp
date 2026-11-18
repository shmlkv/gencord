#include "usb_mic.h"
#include <atomic>
#include <initializer_list>
#include <usb/usb_host.h>
#include <usb/uac_host.h>
#include <esp_private/usb_phy.h>
using gencord::End;
static UsbMicInfo info;
static QueueHandle_t qFree, qFilled;
static size_t bufLen;
static int16_t* cur = nullptr;
static size_t curFill = 0;
static gencord::StereoPcm pcm;
static std::atomic<bool> hostRun{false}, hostQuit{false}, hostAlive{false};
static std::atomic<bool> readRun{false}, readAlive{false}, active{false};
static std::atomic<End> failure{End::Stopped};
static std::atomic<uint8_t> newAddr{0};
static uint8_t newIface;
static std::atomic<uint32_t> drops{0};
static uac_host_device_handle_t dev = nullptr;
static usb_phy_handle_t jtagPhy = nullptr;
static bool hostInstalled = false, driverInstalled = false, cleanupOk = true;
static void fail(End reason) {
  End expected=End::Running;
  failure.compare_exchange_strong(expected,reason);
}
static void hostTaskFn(void*) {
  while(hostRun) {
    uint32_t flags=0;
    const esp_err_t e=usb_host_lib_handle_events(pdMS_TO_TICKS(20),&flags);
    if(e!=ESP_OK && e!=ESP_ERR_TIMEOUT) fail(End::Transfer);
    if(hostQuit && ((flags&USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) || usb_host_device_free_all()==ESP_OK)) break;
  }
  hostAlive=false;
  vTaskDelete(nullptr);
}
static void driverCb(uint8_t addr,uint8_t iface,const uac_host_driver_event_t ev,void*) {
  if(ev==UAC_HOST_DRIVER_EVENT_RX_CONNECTED && !newAddr.load()) { newIface=iface; newAddr=addr; }
}
static void deviceCb(uac_host_device_handle_t,const uac_host_device_event_t ev,void*) {
  if(ev==UAC_HOST_DRIVER_EVENT_DISCONNECTED) fail(End::Disconnected);
  if(ev==UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR) fail(End::Transfer);
}
static bool publish() {
  if(!cur || !curFill) return true;
  const AudioBlock block{cur,curFill};
  if(xQueueSend(qFilled,&block,0)!=pdTRUE) { fail(End::Overflow); ++drops; return false; }
  cur=nullptr; curFill=0; return true;
}
static bool emit(void*,int16_t l,int16_t r) {
  if(!cur) {
    if(xQueueReceive(qFree,&cur,0)!=pdTRUE) {++drops; return false;}
    curFill=0;
  }
  cur[curFill++]=l; cur[curFill++]=r;
  return curFill<bufLen || publish();
}
static void readTaskFn(void*) {
  uint8_t raw[1536];
  uint32_t lastData=millis();
  while(readRun && failure==End::Running) {
    uint32_t got=0;
    const esp_err_t e=uac_host_device_read(dev,raw,sizeof(raw),&got,0);
    if(e!=ESP_OK && e!=ESP_ERR_TIMEOUT) {fail(End::Transfer); break;}
    if(got) {
      lastData=millis();
      if(!pcm.feed(raw,got)) {fail(pcm.end()); break;}
    } else {
      if(millis()-lastData>2000) {fail(End::Timeout); break;}
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  const End reason=failure.load();
  pcm.finish(reason==End::Running ? End::Stopped : reason);
  if(pcm.end()!=End::Stopped) fail(pcm.end());
  publish(); // preserve every completed frame, even a short final block
  readAlive=false;
  vTaskDelete(nullptr);
}
static int rateScore(uint32_t r) {
  switch(r) {
    case 16000:return 100; case 48000:return 80; case 32000:return 70;
    case 96000:return 50; case 44100:return 40; default:return r>=8000 && r<=96000 ? 20:-1000;
  }
}
static bool pickFormat(uac_host_dev_alt_param_t& best,uint32_t& bestRate) {
  uac_host_dev_info_t di{};
  if(uac_host_get_device_info(dev,&di)!=ESP_OK) return false;
  info.vid=di.VID; info.pid=di.PID;
  unsigned i=0;
  for(;i<UAC_STR_DESC_MAX_LENGTH && di.iProduct[i];++i) info.product[i]=di.iProduct[i]<128 ? char(di.iProduct[i]):'?';
  info.product[i]=0;
  int bestScore=-1;
  for(unsigned alt=1;alt<=di.iface_alt_num;++alt) {
    uac_host_dev_alt_param_t p{};
    if(uac_host_get_device_alt_param(dev,alt,&p)!=ESP_OK || p.format!=1) continue;
    uint32_t rate=0; int rs=-1000;
    if(p.sample_freq_type==0) {
      for(uint32_t r:{16000u,48000u,32000u,44100u,96000u,8000u})
        if(r>=p.sample_freq_lower && r<=p.sample_freq_upper && rateScore(r)>rs) {rs=rateScore(r);rate=r;}
    } else {
      for(unsigned k=0;k<p.sample_freq_type && k<UAC_FREQ_NUM_MAX;++k)
        if(rateScore(p.sample_freq[k])>rs) {rs=rateScore(p.sample_freq[k]);rate=p.sample_freq[k];}
    }
    if(!gencord::supportedFormat(p.channels,p.subframe_size,p.bit_resolution,rate)) continue;
    const int score=rs*100+(p.bit_resolution==16 ? 10:5);
    if(score>bestScore) {bestScore=score;best=p;bestRate=rate;}
  }
  return bestScore>=0;
}
static void restoreSerial() {
  usb_phy_config_t pc{};
  pc.controller=USB_PHY_CTRL_SERIAL_JTAG; pc.target=USB_PHY_TARGET_INT;
  if(usb_new_phy(&pc,&jtagPhy)!=ESP_OK) jtagPhy=nullptr;
}
static void hostDown() {
  if(dev) {
    // stop may report invalid-state after disconnect; close still owns the open handle.
    uac_host_device_stop(dev);
    if(uac_host_device_close(dev)!=ESP_OK) {cleanupOk=false;return;}
    dev=nullptr;
  }
  if(driverInstalled) {
    if(uac_host_uninstall()!=ESP_OK) {cleanupOk=false;return;}
    driverInstalled=false;
  }
  hostQuit=true;
  for(int i=0;i<200 && hostAlive;++i) vTaskDelay(pdMS_TO_TICKS(10));
  hostRun=false;
  for(int i=0;i<50 && hostAlive;++i) vTaskDelay(pdMS_TO_TICKS(10));
  if(hostAlive) {cleanupOk=false;return;}
  if(hostInstalled) {
    if(usb_host_uninstall()!=ESP_OK) {cleanupOk=false;return;}
    hostInstalled=false;
  }
  restoreSerial();
}
bool usbMicStart(QueueHandle_t freeQ,QueueHandle_t filledQ,size_t samples,uint32_t waitMs) {
  if(active) return usbMicActive();
  if(!cleanupOk || !freeQ || !filledQ || samples<2 || samples%2) return false;
  qFree=freeQ;qFilled=filledQ;bufLen=samples;cur=nullptr;curFill=0;
  info=UsbMicInfo{};drops=0;newAddr=0;failure=End::Running;
  if(jtagPhy) {usb_del_phy(jtagPhy);jtagPhy=nullptr;}
  usb_host_config_t hc{};hc.intr_flags=ESP_INTR_FLAG_LEVEL1;
  if(usb_host_install(&hc)!=ESP_OK) {fail(End::Transfer);restoreSerial();return false;}
  hostInstalled=true;hostRun=true;hostQuit=false;hostAlive=true;
  if(xTaskCreatePinnedToCore(hostTaskFn,"usbhost",4096,nullptr,6,nullptr,0)!=pdPASS) {
    hostAlive=false;fail(End::Overflow);hostDown();return false;
  }
  uac_host_driver_config_t dc{};
  dc.create_background_task=true;dc.task_priority=6;dc.stack_size=4096;dc.core_id=0;dc.callback=driverCb;
  if(uac_host_install(&dc)!=ESP_OK) {fail(End::Transfer);hostDown();return false;}
  driverInstalled=true;
  const uint32_t t0=millis();
  while(!newAddr && failure==End::Running && millis()-t0<waitMs) vTaskDelay(pdMS_TO_TICKS(10));
  if(!newAddr || failure!=End::Running) {fail(End::Timeout);hostDown();return false;}
  uac_host_device_config_t oc{};
  oc.addr=newAddr;oc.iface_num=newIface;oc.buffer_size=24576;oc.buffer_threshold=3072;oc.callback=deviceCb;
  uac_host_dev_alt_param_t fmt{};uint32_t rate=0;
  if(uac_host_device_open(&oc,&dev)!=ESP_OK) {fail(End::Transfer);hostDown();return false;}
  if(!pickFormat(fmt,rate)) {fail(End::Format);hostDown();return false;}
  if(!pcm.begin(fmt.channels,fmt.subframe_size,fmt.bit_resolution,rate,emit,nullptr)) {
    fail(End::Format);hostDown();return false;
  }
  info.rate=rate;info.channels=fmt.channels;info.bits=fmt.bit_resolution;
  uac_host_stream_config_t sc{};sc.channels=2;sc.bit_resolution=fmt.bit_resolution;sc.sample_freq=rate;
  if(uac_host_device_start(dev,&sc)!=ESP_OK || failure!=End::Running) {
    fail(End::Transfer);hostDown();return false;
  }
  readRun=true;readAlive=true;
  if(xTaskCreatePinnedToCore(readTaskFn,"usbmic",4096,nullptr,5,nullptr,0)!=pdPASS) {
    readAlive=false;readRun=false;fail(End::Overflow);hostDown();return false;
  }
  active=true;return true;
}
void usbMicStop() {
  if(!active) return;
  readRun=false;
  while(readAlive) vTaskDelay(pdMS_TO_TICKS(5));
  hostDown();active=false;
}
bool usbMicActive() {return active && failure==End::Running;}
bool usbMicLost() {return !cleanupOk || (failure!=End::Running && failure!=End::Stopped);}
uint32_t usbMicDrops() {return drops;}
const UsbMicInfo& usbMicInfo() {return info;}
const char* usbMicError() {
  if(!cleanupOk) return "USB cleanup failed: reset";
  switch(failure.load()) {
    case End::Format:return "Need exactly 2ch PCM";
    case End::Disconnected:return "USB disconnected";
    case End::Transfer:return "USB transfer error";
    case End::Overflow:return "Audio buffer overflow";
    case End::Truncated:return "Incomplete USB frame";
    case End::Timeout:return "USB not ready / stalled";
    default:return "USB stereo required";
  }
}
