#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include "platform/affinity.hpp"

namespace me::platform {
bool pin_current_thread(int cpu) noexcept {
  if (cpu < 0 || cpu >= CPU_SETSIZE || cpu >= logical_cpus()) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}
int logical_cpus() noexcept { return static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN)); }
}  // namespace me::platform
