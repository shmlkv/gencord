#include "pcm_pipeline.h"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace gencord {
bool supportedFormat(unsigned ch, unsigned sf, unsigned bits, uint32_t rate) {
  // Packed little-endian signed PCM only. Do not reinterpret 24-in-32 slots.
  return ch==2 && sf>=2 && sf<=4 && bits==sf*8 && rate>=8000 && rate<=96000;
}
static void put16(uint8_t* p, uint16_t v) {p[0]=uint8_t(v); p[1]=uint8_t(v>>8);}
static void put32(uint8_t* p, uint32_t v) {for(unsigned i=0;i<4;++i) p[i]=uint8_t(v>>(8*i));}
bool wavHeader(uint8_t* h, uint32_t bytes) {
  if (bytes%FrameBytes || bytes>UINT32_MAX-36) return false;
  std::memcpy(h,"RIFF",4); put32(h+4,36+bytes); std::memcpy(h+8,"WAVEfmt ",8);
  put32(h+16,16); put16(h+20,1); put16(h+22,2); put32(h+24,OutputRate);
  put32(h+28,OutputRate*FrameBytes); put16(h+32,FrameBytes); put16(h+34,16);
  std::memcpy(h+36,"data",4); put32(h+40,bytes); return true;
}
uint32_t durationMs(uint32_t bytes) {return uint32_t(uint64_t(bytes/FrameBytes)*1000/OutputRate);}
bool StereoPcm::begin(unsigned ch, unsigned sf, unsigned bits, uint32_t rate, Emit emit, void* context) {
  *this = StereoPcm{};
  if (!emit || !supportedFormat(ch,sf,bits,rate)) {end_=End::Format; return false;}
  sf_=sf; emit_=emit; context_=context; end_=End::Running;
  step_=(uint64_t(rate)<<32)/OutputRate; filter_=rate>OutputRate;
  if (filter_) {
    constexpr float pi=3.14159265358979323846f;
    const float fc=7200.0f/rate;
    float sum=0;
    for (unsigned k=0;k<Taps;++k) {
      const float m=float(k)-(Taps-1)/2.0f;
      const float s=m==0 ? 2*fc : std::sin(2*pi*fc*m)/(pi*m);
      taps_[k]=s*(0.54f-0.46f*std::cos(2*pi*k/(Taps-1))); sum+=taps_[k];
    }
    for (float& t:taps_) t/=sum;
  }
  return true;
}
float StereoPcm::filtered(unsigned ch, uint32_t n) const {
  if (!filter_) return hist_[ch][n&(Hist-1)];
  float y=0;
  for(unsigned k=0;k<Taps;++k) y+=taps_[k]*hist_[ch][(n-k)&(Hist-1)];
  return y;
}
bool StereoPcm::frame() {
  for(unsigned ch=0;ch<2;++ch) {
    const uint8_t* p=partial_+ch*sf_;
    // The high 16 bits of packed 16/24/32-bit signed PCM. No mixing or AGC.
    const uint16_t u=uint16_t(p[sf_-2]) | uint16_t(p[sf_-1])<<8;
    const int32_t signedValue = u>=32768 ? int32_t(u)-65536 : u;
    hist_[ch][inCount_&(Hist-1)]=float(signedValue);
  }
  for (;;) {
    const uint32_t i=uint32_t(outPos_>>32), frac=uint32_t(outPos_);
    if (i>inCount_ || (frac && i==inCount_)) break;
    int16_t out[2];
    for(unsigned ch=0;ch<2;++ch) {
      float v=filtered(ch,i);
      if(frac) v+=(frac*(1.0f/4294967296.0f))*(filtered(ch,i+1)-v);
      out[ch]=int16_t(std::max(-32768.0f,std::min(32767.0f,v)));
    }
    if(!emit_(context_,out[0],out[1])) {end_=End::Overflow; return false;}
    outPos_+=step_;
  }
  ++inCount_;
  if(inCount_>=0x40000000u) {
    const uint32_t shift=(inCount_-2*Hist)&~uint32_t(Hist-1);
    inCount_-=shift; outPos_-=uint64_t(shift)<<32;
  }
  return true;
}
bool StereoPcm::feed(const uint8_t* bytes, size_t count) {
  if(end_!=End::Running) return false;
  while(count) {
    const size_t n=std::min(count,size_t(sf_*2-have_));
    std::memcpy(partial_+have_,bytes,n); have_+=unsigned(n); bytes+=n; count-=n;
    if(have_==sf_*2) {have_=0; if(!frame()) return false;}
  }
  return true;
}
void StereoPcm::finish(End reason) {
  if(end_!=End::Running) return;
  end_=(reason==End::Stopped && have_) ? End::Truncated : reason;
  have_=0; // incomplete input frame never becomes a fabricated/duplicated channel
}
} // namespace gencord
