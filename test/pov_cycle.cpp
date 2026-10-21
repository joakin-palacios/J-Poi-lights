// Compile-only self-check: xtensa-esp32s3-elf-g++ -std=c++14 -fsyntax-only test/pov_cycle.cpp
#include "../usermods/numbered_pov/playlist.h"
#include "../include/pov_bmp.h"

using namespace NumberedPov;

constexpr bool playlistChecks() {
  Playlist list;
  if (list.at(1000, 0, 4000, 1, true, 1).image || list.next(1)) return false;
  if (!list.add(255, 1) || !list.add(1, 2) || !list.add(3, 3) || list.add(3, 9) || list.add(0, 1)) return false;
  if (list.id(0) != 1 || list.id(1) != 3 || list.id(2) != 255) return false;
  list.configure(nullptr);
  if (list.next(0) != 1 || list.at(0, 1000000, 4000, 1, true, 3).image != 3
      || list.at(0, 1000000, 4000, 1, true, 3).column != 0) return false;
  const uint8_t ids[] = {1, 3, 255};
  const uint32_t widths[] = {2, 3, 1};
  // Compare binary-search scheduling against a simple traversal, for every repeat count and anchor.
  for (uint8_t repeats = 1; repeats <= 20; ++repeats) {
    for (uint8_t anchor = 0; anchor < 3; ++anchor) {
      for (uint32_t step = 0; step < 18U * repeats; ++step) {
        uint32_t remaining = step;
        uint8_t expected = anchor;
        while (remaining >= widths[expected] * repeats) {
          remaining -= widths[expected] * repeats;
          expected = (expected + 1U) % 3U;
        }
        const uint64_t start = 0x100000000ULL * 1000;
        const auto position = list.at(start + uint64_t(step) * 4000 + 3999, start, 4000, repeats, true, ids[anchor]);
        if (position.image != ids[expected] || position.column != remaining % widths[expected]
            || position.next != ids[(expected + 1U) % 3U]
            || position.batchStartUs != start + uint64_t(step - remaining) * 4000) return false;
      }
    }
  }
  const auto manual = list.at(1000000, 0, 4000, 20, false, 3);
  if (manual.image != 3 || manual.next != 3 || manual.column != (250 % 3)) return false;
  uint8_t skipped[32] = {};
  skipped[3 >> 3] |= 1U << (3 & 7);
  list.configure(skipped);
  if (list.at(0, 0, 4000, 2, true, 3).image != 255) return false;
  if (list.at(8000, 0, 4000, 2, true, 3).image != 1 || list.next(1) != 255) return false;
  skipped[1 >> 3] |= 1U << (1 & 7);
  list.configure(skipped);
  if (list.next(255) != 255 || list.at(999999, 0, 4000, 20, true, 255).image != 255) return false;
  skipped[255 >> 3] |= 1U << (255 & 7);
  list.configure(skipped);
  return !list.at(1000000, 0, 4000, 2, true, 1).image;
}

constexpr void put32(uint8_t* p, uint32_t value) {
  for (uint8_t i = 0; i < 4; ++i) p[i] = uint8_t(value >> (i * 8));
}

constexpr bool layoutChecks() {
  uint8_t header[54] = {};
  header[0] = 'B'; header[1] = 'M'; header[26] = 1; header[28] = 24;
  put32(header + 10, 54); put32(header + 14, 40);
  put32(header + 18, 300); put32(header + 22, 11);
  BmpLayout layout;
  if (!bmpLayout(header, 9954, layout) || layout.dataSize != 9900 || layout.rowSize != 900 || layout.topDown) return false;
  // Expected sizes are not dimension limits; retain BMP padding and top-down support.
  put32(header + 18, 301); put32(header + 22, uint32_t(-12));
  if (!bmpLayout(header, 54 + 904 * 12, layout) || layout.dataSize != 904 * 12 || !layout.topDown) return false;
  if (bmpLayout(header, 54 + 904 * 12 - 1, layout)) return false;
  put32(header + 14, UINT32_MAX);
  if (bmpLayout(header, UINT32_MAX, layout)) return false;
  put32(header + 14, 40); put32(header + 18, INT32_MAX); put32(header + 22, INT32_MAX);
  if (bmpLayout(header, UINT32_MAX, layout)) return false;
  put32(header + 18, 1); put32(header + 22, uint32_t(INT32_MIN));
  if (bmpLayout(header, UINT32_MAX, layout)) return false;
  put32(header + 22, 1); header[28] = 32;
  return !bmpLayout(header, 58, layout);
}

static_assert(playlistChecks(), "Sparse IDs, ordering, full repetitions, wrap, late joins, different widths and skipped images");
static_assert(layoutChecks(), "Dynamic image sizes, padding, top-down BMPs, truncation, overflow and format validation");
static_assert(repeatCount(0) == 1 && repeatCount(1) == 1 && repeatCount(20) == 20 && repeatCount(31) == 20, "Repeat bounds");
