#pragma once

#include <stdint.h>

namespace PovSync {

constexpr uint64_t STALE_US = 5000000;
constexpr uint32_t MAX_ROUND_TRIP_US = 20000;

constexpr uint32_t intervalUs(uint8_t speed) {
  return (4U + (uint32_t(speed) * 256U + 127U) / 255U) * 1000U;
}

constexpr uint32_t columnAt(uint64_t now, uint64_t start, uint32_t interval, uint32_t width) {
  return width && interval && now >= start ? uint32_t(((now - start) / interval) % width) : 0;
}

class Clock {
public:
  #if __cplusplus >= 201402L
  constexpr
  #endif
  bool sample(uint64_t f1, uint64_t l2, uint64_t l3, uint64_t f4) {
    if (f4 < f1 || l3 < l2 || f4 > INT64_MAX / 2 || l3 > INT64_MAX / 2 || (_count && f4 <= _last)) return false;
    const int64_t delay = int64_t(f4 - f1) - int64_t(l3 - l2);
    if (delay < 0 || delay > MAX_ROUND_TRIP_US) return false;
    if (_count && f4 - _last > STALE_US) *this = Clock();
    _samples[_next] = Sample(uint32_t(delay), ((int64_t(l2) - int64_t(f1)) + (int64_t(l3) - int64_t(f4))) / 2, f4);
    _next = (_next + 1U) % 8U;
    if (_count < 8) ++_count;
    uint8_t best = 0;
    for (uint8_t i = 1; i < _count; ++i) {
      if (_samples[i].delay < _samples[best].delay
          || (_samples[i].delay == _samples[best].delay && _samples[i].received > _samples[best].received)) best = i;
    }
    const int64_t error = _samples[best].offset - _offset;
    if (_count == 1 || error > 20000 || error < -20000) {
      _offset = _samples[best].offset;
    } else {
      const int64_t correction = error / 8;
      _offset += correction > 250 ? 250 : (correction < -250 ? -250 : correction);
    }
    _roundTrip = _samples[best].delay;
    _last = f4;
    return true;
  }

  constexpr bool acquired() const { return _count >= 3; }
  constexpr bool fresh(uint64_t local) const { return _count && local >= _last && local - _last <= STALE_US; }
  constexpr uint64_t now(uint64_t local) const {
    return int64_t(local) + _offset > 0 ? uint64_t(int64_t(local) + _offset) : 0;
  }
  constexpr int64_t offset() const { return _offset; }
  constexpr uint32_t roundTrip() const { return _roundTrip; }
  constexpr uint8_t count() const { return _count; }

private:
  struct Sample {
    uint32_t delay;
    int64_t offset;
    uint64_t received;
    constexpr Sample(uint32_t d = 0, int64_t o = 0, uint64_t r = 0) : delay(d), offset(o), received(r) {}
  };
  Sample _samples[8] = {};
  uint8_t _next = 0;
  uint8_t _count = 0;
  int64_t _offset = 0;
  uint64_t _last = 0;
  uint32_t _roundTrip = 0;
};

enum class Status : uint8_t { Local, Leader, Acquiring, Synced, Stale, ImageNotReady };

struct Settings {
  uint8_t image = 0; // Sequence anchor, not the automatically changing displayed image.
  uint8_t speed = 0;
  uint8_t repeats = 1;
  bool cycling = false;
  uint32_t playlist = 0;
  uint8_t skipped[32] = {};
};

struct Playback {
  bool follower = false;
  bool active = false;
  bool ready = false;
  Settings settings;
  uint64_t startUs = 0;
  uint64_t nowUs = 0;
  Status status = Status::Local;
};

} // namespace PovSync

bool pairSyncPovPlayback(const PovSync::Settings& settings, bool active, uint64_t startUs,
                         PovSync::Playback& playback) __attribute__((weak));
bool pairSyncPovImageReady(uint8_t image, const uint8_t* hash) __attribute__((weak));
