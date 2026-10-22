#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "platform/affinity.hpp"

namespace me::platform {
bool pin_current_thread(int cpu) noexcept {
  // Single processor group only: the machines used here have <= 64 logical CPUs.
  if (cpu < 0 || cpu >= 64 || cpu >= logical_cpus()) return false;
  return SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu) != 0;
}
int logical_cpus() noexcept {
  return static_cast<int>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
}
}  // namespace me::platform
