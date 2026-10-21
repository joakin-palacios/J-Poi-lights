#pragma once

#include <stdint.h>

namespace NumberedPov {

constexpr uint32_t little32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

struct BmpLayout {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t rowSize = 0;
  uint32_t offset = 0;
  uint32_t dataSize = 0;
  bool topDown = false;
};

#if __cplusplus >= 201402L
constexpr
#else
inline
#endif
bool bmpLayout(const uint8_t* header, uint32_t fileSize, BmpLayout& out) {
  if (fileSize < 54 || header[0] != 'B' || header[1] != 'M') return false;
  const uint32_t dib = little32(header + 14);
  const int32_t width = int32_t(little32(header + 18));
  const int32_t height = int32_t(little32(header + 22));
  if (dib < 40 || width <= 0 || !height || height == INT32_MIN || header[26] != 1 || header[27]
      || header[28] != 24 || header[29] || little32(header + 30)) return false;
  const uint64_t row = (uint64_t(width) * 3U + 3U) & ~uint64_t(3);
  const uint32_t rows = height < 0 ? uint32_t(-height) : uint32_t(height);
  const uint64_t bytes = row * rows;
  const uint32_t offset = little32(header + 10);
  if (uint64_t(offset) < 14ULL + dib || offset > fileSize || bytes > fileSize - offset) return false;
  out.width = width;
  out.height = rows;
  out.rowSize = uint32_t(row);
  out.offset = offset;
  out.dataSize = uint32_t(bytes);
  out.topDown = height < 0;
  return true;
}

} // namespace NumberedPov
