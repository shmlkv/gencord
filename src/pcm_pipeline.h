// Pure, fixed-memory USB PCM boundary. This same implementation is host-tested.
#pragma once
#include <cstddef>
#include <cstdint>

namespace gencord {
constexpr uint32_t OutputRate = 16000;
constexpr uint32_t FrameBytes = 4;
constexpr uint32_t MaxDataBytes = OutputRate * FrameBytes * 3600; // 60 min < receiver 512 MiB

enum class End { Running, Stopped, Format, Disconnected, Transfer, Overflow, Truncated, Timeout };
using Emit = bool (*)(void*, int16_t left, int16_t right);
bool supportedFormat(unsigned channels, unsigned subframe, unsigned bits, uint32_t rate);
bool wavHeader(uint8_t* header44, uint32_t dataBytes);
uint32_t durationMs(uint32_t dataBytes);

class StereoPcm {
 public:
  bool begin(unsigned channels, unsigned subframe, unsigned bits, uint32_t rate, Emit emit, void* context);
  bool feed(const uint8_t* bytes, size_t count);
  void finish(End reason);
  End end() const { return end_; }
 private:
  static constexpr unsigned Taps=31, Hist=64;
  float filtered(unsigned ch, uint32_t n) const;
  bool frame();
  End end_ = End::Stopped;
  Emit emit_ = nullptr;
  void* context_ = nullptr;
  unsigned sf_ = 0, have_ = 0;
  uint8_t partial_[8]{};
  float taps_[Taps]{};
  float hist_[2][Hist]{};
  uint32_t inCount_ = 0;
  uint64_t outPos_ = 0, step_ = 0;
  bool filter_ = false;
};
} // namespace gencord
