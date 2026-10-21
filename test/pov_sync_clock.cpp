// Compile-only self-check: xtensa-esp32s3-elf-g++ -std=c++14 -fsyntax-only test/pov_sync_clock.cpp
#include "../include/pov_sync.h"

using namespace PovSync;

constexpr bool clockChecks() {
  Clock clock;
  // Symmetric 1 ms transit, 100 us leader processing, leader 10 seconds ahead.
  if (!clock.sample(1000000, 11001000, 11001100, 1002100)) return false;
  if (clock.offset() != 10000000 || clock.roundTrip() != 2000 || clock.acquired()) return false;
  if (!clock.sample(1100000, 11101000, 11101100, 1102100)) return false;
  if (!clock.sample(1200000, 11201000, 11201100, 1202100) || !clock.acquired()) return false;
  // Reject delayed replies and impossible timestamps without changing the estimate.
  if (clock.sample(1300000, 11301000, 11301100, 1400000)) return false;
  if (clock.sample(1300000, 11301000, 11300000, 1302100)) return false;
  if (clock.sample(1400000, 11401000, 11401100, 1302100)) return false;
  if (clock.offset() != 10000000 || !clock.fresh(2000000) || clock.fresh(7000000)) return false;
  if (clock.sample(1100000, 11101000, 11101100, 1102100)) return false;
  if (clock.sample(1200000, 11201000, 11201100, 1202100)) return false;
  // Packet loss: the clock continues. On return, reacquire instead of reusing old samples.
  if (clock.now(6000000) != 16000000) return false;
  if (!clock.sample(8000000, 18002000, 18002100, 8002100) || clock.acquired()) return false;
  if (clock.offset() != 10001000) return false;
  // A later-booted leader can have a negative offset.
  Clock behind;
  return behind.sample(11000000, 1001000, 1001100, 11002100)
    && behind.offset() == -10000000 && behind.now(12000000) == 2000000;
}

constexpr bool filterChecks() {
  Clock clock;
  if (!clock.sample(1000000, 11001000, 11001100, 1002100)) return false;
  // A higher-delay asymmetric sample must not displace the low-delay estimate.
  if (!clock.sample(2000000, 12009000, 12009100, 2010100) || clock.offset() != 10000000) return false;
  // Equal-delay recent samples track drift, with a bounded correction.
  if (!clock.sample(3000000, 13002000, 13002100, 3002100) || clock.offset() != 10000125) return false;
  for (uint64_t i = 4; i <= 20; ++i) {
    const uint64_t f1 = i * 1000000;
    const int64_t before = clock.offset();
    if (!clock.sample(f1, f1 + 10002000, f1 + 10002100, f1 + 2100)) return false;
    if (clock.offset() < before || clock.offset() - before > 250 || clock.offset() > 10001000) return false;
  }
  // Reboot clears old estimates, including when the leader's uptime becomes smaller.
  clock = Clock();
  return clock.sample(0x100000000ULL, 1001000, 1001100, 0x100000000ULL + 2100)
    && clock.now(0x100000000ULL + 4000) == 1004000 && !clock.acquired();
}

static_assert(clockChecks(), "Clock sampling, rejection, holdover and reacquisition");
static_assert(filterChecks(), "Delay filtering, bounded drift correction, history rotation and reboot");
static_assert(intervalUs(0) == 4000 && intervalUs(255) == 260000, "Speed endpoints");
static_assert(columnAt(100000, 200000, 4000, 200) == 0, "Future start");
static_assert(columnAt(12000000, 0, 4000, 200) == 0, "Column wraps at image width");
static_assert(columnAt(12012000, 0, 4000, 200) == 3, "Late join skips to current column");
static_assert(columnAt(0x100000000ULL + 12000, 0x100000000ULL, 4000, 200) == 3, "32-bit micros rollover");
static_assert(columnAt(0x100000000ULL * 1000 + 12000, 0x100000000ULL * 1000, 4000, 200) == 3, "32-bit millis rollover");
static_assert(columnAt(12000, 0, 0, 200) == 0 && columnAt(12000, 0, 4000, 0) == 0, "Unavailable image");
