#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>

#include "me/itch_parser.hpp"
#include "platform/mapped_file.hpp"

namespace {
bool recording = false;
std::size_t allocations = 0;
struct Handler {
  std::uint64_t callbacks = 0;
  template <class T>
  void on(const T&) {
    ++callbacks;
  }
  void on_unknown(unsigned char, std::span<const std::byte>) { ++callbacks; }
};
}  // namespace

void* operator new(std::size_t size) {
  if (recording) ++allocations;
  if (auto* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const me::platform::MappedFile input{std::filesystem::path(argv[1])};
  Handler handler;
  me::itch::Stats stats;
  recording = true;
  for (int i = 0; i < 10; ++i) me::itch::parse(input.bytes(), handler, &stats);
  recording = false;
  std::cout << "allocations=" << allocations << " callbacks=" << handler.callbacks << '\n';
  return allocations == 0 && stats.messages == 149213 && handler.callbacks == 1492130 ? 0 : 1;
}
