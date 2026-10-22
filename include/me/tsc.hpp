#pragma once

#include <chrono>
#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#include <x86intrin.h>
#endif

namespace me::tsc {

// Time-stamp counter read. Not serializing: it measures elapsed time between two
// points on the hot path without forcing the pipeline to drain.
inline std::uint64_t now() noexcept { return __rdtsc(); }

inline void cpu_relax() noexcept { _mm_pause(); }

// CPUID 0x80000007 EDX bit 8: the TSC ticks at a constant rate in every power state
// and is synchronized across cores (Linux reports this as constant_tsc plus
// nonstop_tsc). Without it, TSC differences across cores or sleeps are meaningless.
inline bool invariant() noexcept {
#if defined(_MSC_VER)
  int regs[4]{};
  __cpuid(regs, static_cast<int>(0x80000000));
  if (static_cast<unsigned>(regs[0]) < 0x80000007U) return false;
  __cpuid(regs, static_cast<int>(0x80000007));
  return (static_cast<unsigned>(regs[3]) >> 8) & 1U;
#else
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (!__get_cpuid(0x80000007U, &a, &b, &c, &d)) return false;
  return (d >> 8) & 1U;
#endif
}

// Measures TSC ticks per nanosecond against steady_clock by spinning for the given
// interval. Called once at startup, off the hot path.
inline double ticks_per_ns(std::chrono::milliseconds interval = std::chrono::milliseconds(50)) {
  using clock = std::chrono::steady_clock;
  const auto start_time = clock::now();
  const std::uint64_t start_ticks = now();
  while (clock::now() - start_time < interval) cpu_relax();
  const std::uint64_t ticks = now() - start_ticks;
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start_time);
  return static_cast<double>(ticks) / static_cast<double>(elapsed.count());
}

}  // namespace me::tsc
