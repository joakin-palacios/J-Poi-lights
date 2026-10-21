// Compile-only self-check: xtensa-esp32s3-elf-g++ -std=c++14 -fsyntax-only test/pov_joker.cpp
#include "../usermods/numbered_pov/joker.h"

using namespace NumberedPov;

constexpr bool blendIs(uint32_t column, uint32_t width, uint8_t colors, uint8_t first, uint8_t second, uint8_t amount) {
  const auto blend = jokerBlend(column, width, colors);
  return blend.first == first && blend.second == second && blend.amount == amount;
}

static_assert(blendIs(0, 101, 1, 0, 0, 0) && blendIs(100, 101, 1, 0, 0, 0), "One swatch is constant");
static_assert(blendIs(0, 101, 2, 0, 1, 0) && blendIs(100, 101, 2, 0, 1, 255), "Two-swatch endpoints");
static_assert(blendIs(50, 101, 2, 0, 1, 127), "Two-swatch midpoint");
static_assert(blendIs(0, 101, 3, 0, 1, 0) && blendIs(50, 101, 3, 0, 1, 255)
  && blendIs(100, 101, 3, 1, 2, 255), "Three-swatch stops");
static_assert(blendIs(25, 101, 3, 0, 1, 127) && blendIs(75, 101, 3, 1, 2, 127), "Three-swatch interpolation");
static_assert(blendIs(1, 4, 3, 0, 1, 170) && blendIs(2, 4, 3, 1, 2, 85), "Even image widths");
static_assert(blendIs(0, 0, 3, 0, 0, 0) && blendIs(0, 1, 3, 0, 0, 0), "Single column uses the first swatch");
static_assert(blendIs(1, 3, 0, 0, 0, 0) && blendIs(1, 3, 255, 0, 1, 255), "Clamp color count");
static_assert(blendIs(UINT32_MAX, UINT32_MAX, 3, 1, 2, 255), "Large widths and column clamping do not overflow");
static_assert(jokerPixel(0xFFFFFF, 0x123456) == 0x123456, "Exact white is replaced");
static_assert(jokerPixel(0xFFFFFE, 0x123456) == 0xFFFFFE && jokerPixel(0xFEFFFF, 0x123456) == 0xFEFFFF,
  "Near-white stays unchanged");
static_assert(jokerPixel(0, 0x123456) == 0 && jokerPixel(0xFF00FF, 0x123456) == 0xFF00FF, "Other pixels stay unchanged");
static_assert(jokerPixel(0xFFFFFF, 0) == 0 && jokerPixel(0xFFFFFF, 0xFFFFFF) == 0xFFFFFF, "Black and white swatches are valid");
static_assert(jokerPixel(0xFFFFFF, 0x80123456) == 0x80123456, "RGBW replacement is preserved");
