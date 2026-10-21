#pragma once

#include <stdint.h>

namespace NumberedPov {

#if __cplusplus >= 201402L
#define POV_PLAYLIST_CHECKABLE constexpr
#else
#define POV_PLAYLIST_CHECKABLE
#endif

constexpr uint8_t repeatCount(uint8_t value) { return value < 1 ? 1 : (value > 20 ? 20 : value); }
constexpr bool excluded(const uint8_t* mask, uint8_t id) { return mask && (mask[id >> 3] & (1U << (id & 7))); }

struct Position {
  uint8_t image = 0;
  uint8_t next = 0;
  uint32_t column = 0;
  uint64_t batchStartUs = 0;
};

class Playlist {
public:
  POV_PLAYLIST_CHECKABLE void clear() { _count = 0; _total = 0; }
  POV_PLAYLIST_CHECKABLE bool add(uint8_t id, uint32_t width) {
    if (!id || !width || _count == 255) return false;
    uint16_t i = 0;
    while (i < _count && _ids[i] < id) ++i;
    if (i < _count && _ids[i] == id) return false;
    for (uint16_t j = _count; j > i; --j) { _ids[j] = _ids[j - 1]; _widths[j] = _widths[j - 1]; }
    _ids[i] = id;
    _widths[i] = width;
    ++_count;
    return true;
  }
  POV_PLAYLIST_CHECKABLE void configure(const uint8_t* skipped) {
    _total = 0;
    for (uint16_t i = 0; i < _count; ++i) {
      if (!excluded(skipped, _ids[i])) _total += _widths[i];
      _ends[i] = _total;
    }
  }
  POV_PLAYLIST_CHECKABLE uint32_t signature() const {
    uint32_t hash = 2166136261UL;
    for (uint16_t i = 0; i < _count; ++i) {
      hash = (hash ^ _ids[i]) * 16777619UL;
      for (uint8_t shift = 0; shift < 32; shift += 8) hash = (hash ^ uint8_t(_widths[i] >> shift)) * 16777619UL;
    }
    return hash ? hash : 1;
  }
  POV_PLAYLIST_CHECKABLE uint16_t count() const { return _count; }
  POV_PLAYLIST_CHECKABLE uint8_t id(uint16_t i) const { return i < _count ? _ids[i] : 0; }
  POV_PLAYLIST_CHECKABLE bool contains(uint8_t image) const { return index(image) < _count; }
  POV_PLAYLIST_CHECKABLE uint8_t next(uint8_t image, const uint8_t* failed = nullptr) const {
    if (!_count) return 0;
    const uint16_t found = index(image);
    const uint16_t current = found < _count ? found : _count - 1;
    for (uint16_t n = 1; n <= _count; ++n) {
      const uint16_t i = (current + n) % _count;
      if (enabled(i) && !excluded(failed, _ids[i])) return _ids[i];
    }
    return 0;
  }
  POV_PLAYLIST_CHECKABLE Position at(uint64_t now, uint64_t start, uint32_t interval, uint8_t repeats, bool cycling, uint8_t anchor) const {
    Position result;
    if (!_count || !interval) return result;
    uint16_t first = index(anchor);
    if (first == _count) first = 0;
    const uint64_t steps = now >= start ? (now - start) / interval : 0;
    if (!cycling) {
      result.image = _ids[first];
      result.next = result.image;
      result.column = steps % _widths[first];
      result.batchStartUs = start;
      return result;
    }
    if (!_total) return result;
    while (!enabled(first)) first = (first + 1U) % _count;
    const uint8_t passes = repeatCount(repeats);
    const uint64_t before = first ? _ends[first - 1] : 0;
    const uint64_t phase = (steps % (_total * passes) + before * passes) % (_total * passes);
    uint16_t low = 0, high = _count;
    while (low < high) {
      const uint16_t middle = (low + high) / 2;
      if (_ends[middle] * passes <= phase) low = middle + 1;
      else high = middle;
    }
    const uint64_t inside = phase - (low ? _ends[low - 1] * passes : 0);
    result.image = _ids[low];
    result.next = next(result.image);
    result.column = inside % _widths[low];
    result.batchStartUs = start + (steps - inside) * interval;
    return result;
  }

private:
  POV_PLAYLIST_CHECKABLE uint16_t index(uint8_t image) const {
    for (uint16_t i = 0; i < _count; ++i) if (_ids[i] == image) return i;
    return _count;
  }
  POV_PLAYLIST_CHECKABLE bool enabled(uint16_t i) const { return _ends[i] > (i ? _ends[i - 1] : 0); }
  uint8_t _ids[255] = {};
  uint32_t _widths[255] = {};
  uint64_t _ends[255] = {};
  uint16_t _count = 0;
  uint64_t _total = 0;
};

} // namespace NumberedPov

#undef POV_PLAYLIST_CHECKABLE
