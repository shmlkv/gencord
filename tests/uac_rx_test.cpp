// Synthetic host harness for the ACTUAL vendored stream_rx_xfer_done body.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
constexpr int UAC_INTERFACE_STATE_ACTIVE=1,USB_TRANSFER_STATUS_COMPLETED=0,USB_TRANSFER_STATUS_CANCELED=1,USB_TRANSFER_STATUS_NO_DEVICE=2;
constexpr int UAC_HOST_DEVICE_EVENT_RX_DONE=0,UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR=1,ESP_OK=0;
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
struct Packet {int status,num_bytes,actual_num_bytes;};
struct uac_iface_t;
struct usb_transfer_t {uac_iface_t* context;int status,actual_num_bytes,num_isoc_packets;Packet isoc_packet_desc[2];uint8_t data_buffer[64];};
struct uac_iface_t {int state;void* ringbuf;unsigned ringbuf_size,ringbuf_threshold,packet_frame_size;};
static unsigned ringLen=0,pushes=0;static int errors=0;static bool pushFail=false,submitFail=false;
static unsigned _ring_buffer_get_len(void*) {return ringLen;}
static int _ring_buffer_push(void*,uint8_t*,unsigned n,int) {if(pushFail)return 1;ringLen+=n;++pushes;return ESP_OK;}
static void uac_host_user_interface_callback(uac_iface_t*,int e) {if(e==UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR)++errors;}
static int usb_host_transfer_submit(usb_transfer_t*) {return submitFail ? 1:ESP_OK;}
// ACTUAL_CALLBACK is populated from lib/usb_host_uac/src/uac_host.c by run_host.py.
#include "actual_uac_rx.inc"
int main() {
  for(int mode=0;mode<6;++mode) {
    ringLen=pushes=errors=0;pushFail=submitFail=false;
    uac_iface_t iface{1,reinterpret_cast<void*>(1),32,8,4};
    usb_transfer_t x{};x.context=&iface;x.num_isoc_packets=1;x.actual_num_bytes=8;x.isoc_packet_desc[0]={0,8,8};
    if(mode==1) ringLen=32; // entire URB would overflow
    if(mode==2) x.isoc_packet_desc[0].status=7;
    if(mode==3) pushFail=true;
    if(mode==4) submitFail=true;
    if(mode==5) {x.actual_num_bytes=7;x.isoc_packet_desc[0].actual_num_bytes=7;} // broken frame alignment
    stream_rx_xfer_done(&x);
    assert(errors==(mode==0 ? 0:1));
    if(mode==0) assert(pushes==1 && ringLen==8);
    if(mode==1 || mode==2 || mode==5) assert(pushes==0);
  }
  puts("PASS actual vendored UAC callback: valid RX, ring overflow, bad packet, push/submit failure, malformed stereo frame");
}
