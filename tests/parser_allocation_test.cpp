#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <new>
#include <string_view>

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
  if (argc != 3) return 2;
  constexpr std::uint64_t replays = 10;
  std::uint64_t expected = 0;
  const std::string_view count(argv[2]);
  const auto converted = std::from_chars(count.data(), count.data() + count.size(), expected);
  if (converted.ec != std::errc{} || converted.ptr != count.data() + count.size() ||
      expected > std::numeric_limits<std::uint64_t>::max() / replays)
    return 2;
  const me::platform::MappedFile input{std::filesystem::path(argv[1])};
  Handler handler;
  me::itch::Stats stats;
  recording = true;
  for (std::uint64_t i = 0; i < replays; ++i) me::itch::parse(input.bytes(), handler, &stats);
  recording = false;
  std::cout << "allocations=" << allocations << " callbacks=" << handler.callbacks << '\n'
            << std::flush;
  return std::cout && allocations == 0 && stats.messages == expected &&
                 handler.callbacks == expected * replays
             ? 0
             : 1;
}
