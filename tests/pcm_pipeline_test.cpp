#include "pcm_pipeline.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace gencord;
static bool collect(void* ctx, int16_t l, int16_t r) {
  auto& out = *static_cast<std::vector<int16_t>*>(ctx);
  out.push_back(l); out.push_back(r); return true;
}
static std::vector<uint8_t> pack(const std::vector<int16_t>& in, unsigned sf) {
  std::vector<uint8_t> out;
  for (auto x : in) {
    uint32_t v = uint32_t(uint16_t(x)) << (8 * (sf - 2));
    for (unsigned k = 0; k < sf; ++k) out.push_back(uint8_t(v >> (8*k)));
  }
  return out;
}
static std::vector<int16_t> run(const std::vector<int16_t>& in, unsigned rate, unsigned sf, size_t fragment) {
  std::vector<int16_t> out;
  StereoPcm p;
  assert(p.begin(2, sf, sf*8, rate, collect, &out));
  auto raw = pack(in, sf);
  for (size_t i=0; i<raw.size(); i+=fragment)
    assert(p.feed(raw.data()+i, std::min(fragment, raw.size()-i)));
  p.finish(End::Stopped);
  assert(p.end() == End::Stopped);
  return out;
}
static void formats_and_fragments() {
  std::vector<int16_t> input{32767,-32768,1,-1,1234,-8765,0,23,-13,0};
  for (unsigned sf : {2u,3u,4u})
    for (size_t n : {1u,2u,3u,5u,7u,1536u})
      assert(run(input,16000,sf,n) == input);
  std::vector<int16_t> out;
  StereoPcm p;
  for (unsigned channels : {0u,1u,3u,4u}) {
    assert(!p.begin(channels,2,16,48000,collect,&out));
    assert(p.end() == End::Format);
  }
  assert(!p.begin(2,4,24,48000,collect,&out));
  assert(!p.begin(2,2,16,0,collect,&out));
  assert(!p.begin(2,2,16,192000,collect,&out));
  puts("PASS formats, mono/multichannel rejection, packed 16/24/32, byte fragments");
}
static void resampling() {
  for (unsigned rate : {8000u,16000u,32000u,44100u,48000u,96000u}) {
    std::vector<int16_t> in;
    for (unsigned n=0; n<rate; ++n) {
      in.push_back(int16_t(12000*std::sin(2*3.141592653589793*440*n/rate)));
      in.push_back(0);
    }
    auto out = run(in,rate,2,7);
    assert(out == run(in,rate,2,1536));
    assert(out.size()/2 >= 15998 && out.size()/2 <= 16001);
    int peak = 0;
    for (size_t n=0; n<out.size(); n+=2) {
      peak = std::max(peak,std::abs(int(out[n]))); assert(out[n+1] == 0);
    }
    assert(peak > 10000 && peak < 13000);
    for (size_t n=0; n<in.size(); n+=2) std::swap(in[n],in[n+1]);
    auto right = run(in,rate,2,11);
    assert(out.size()==right.size());
    for (size_t n=0; n<out.size(); n+=2) {
      assert(right[n]==0); assert(right[n+1]==out[n]);
    }
  }
  // Antiphase transmitters must not disappear through averaging.
  std::vector<int16_t> opposite;
  for (int n=0;n<4800;++n) { int16_t x=int16_t(6000*std::sin(n*0.1)); opposite.push_back(x); opposite.push_back(-x); }
  auto out=run(opposite,48000,3,13);
  int peak=0;
  for(size_t n=0;n<out.size();n+=2) { assert(out[n]==-out[n+1]); peak=std::max(peak,std::abs(int(out[n]))); }
  assert(peak>5000);
  puts("PASS resampler rates/frame counts, packet invariance, L/R isolation, antiphase");
}
static void termination() {
  for (End reason : {End::Disconnected, End::Transfer, End::Overflow, End::Stopped}) {
    StereoPcm p; std::vector<int16_t> out;
    assert(p.begin(2,2,16,16000,collect,&out));
    auto raw=pack({100,-200,300,-400,500,-600},2);
    assert(p.feed(raw.data(),9)); // two frames plus one byte
    p.finish(reason);
    assert(out == std::vector<int16_t>({100,-200,300,-400}));
    assert(p.end()==(reason==End::Stopped ? End::Truncated : reason));
    assert(!p.feed(raw.data()+9,3));
    p.finish(End::Stopped); // error is sticky
    assert(p.end()==(reason==End::Stopped ? End::Truncated : reason));
  }
  StereoPcm p;
  assert(p.begin(2,2,16,16000,[](void*,int16_t,int16_t){return false;},nullptr));
  auto raw=pack({3,4,5,6},2);
  assert(!p.feed(raw.data(),raw.size())); assert(p.end()==End::Overflow);
  puts("PASS disconnect/error finalization, partial frame, no post-stop samples, sink overflow");
}
static uint32_t le32(const uint8_t* p) {return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
static void wav() {
  uint8_t h[44];
  assert(wavHeader(h,64000));
  assert(h[22]==2 && h[23]==0 && le32(h+24)==16000 && le32(h+28)==64000);
  assert(h[32]==4 && h[34]==16 && le32(h+40)==64000 && le32(h+4)==64036);
  assert(!wavHeader(h,3)); assert(!wavHeader(h,0xfffffffcu));
  assert(durationMs(64000)==1000);
  assert(wavHeader(h,MaxDataBytes));
  assert(durationMs(MaxDataBytes)==3600000);
  assert(MaxDataBytes+44 < 512u*1024u*1024u);
  puts("PASS stereo WAV header, block alignment/RIFF bounds, frame-based duration");
}
int main() { formats_and_fragments(); resampling(); termination(); wav(); puts("ALL PCM TESTS PASSED"); }
