#pragma once

namespace me::platform {
// Pins the calling thread to one logical CPU. Returns false (and leaves the thread
// unpinned) if the CPU does not exist or the OS refuses.
bool pin_current_thread(int cpu) noexcept;
// Number of logical CPUs the process may run on.
int logical_cpus() noexcept;
}  // namespace me::platform
