#pragma once

#include <stdint.h>

namespace NumberedPov {

struct JokerBlend {
  uint8_t first;
  uint8_t second;
  uint8_t amount;
};

#if __cplusplus >= 201402L
constexpr
#else
inline
#endif
JokerBlend jokerBlend(uint32_t column, uint32_t width, uint8_t colors) {
  if (width <= 1 || colors <= 1) return {0, 0, 0};
  if (colors > 3) colors = 3;
  if (column >= width) column = width - 1;
  const uint32_t position = uint64_t(column) * (colors - 1U) * 255U / (width - 1U);
  const uint8_t first = position > 255 ? 1 : 0;
  return {first, uint8_t(first + 1), uint8_t(position - first * 255U)};
}

constexpr uint32_t jokerPixel(uint32_t pixel, uint32_t replacement) {
  return pixel == 0x00FFFFFFU ? replacement : pixel;
}

} // namespace NumberedPov
