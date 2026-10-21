// Replays the real fixture into the book builder twice. The first pass warms
// pools, the id map, level vectors and per-locate books; after reset() the
// second pass must not allocate at all.

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>

#include "me/book_builder.hpp"
#include "me/itch_parser.hpp"
#include "platform/main_trace.hpp"
#include "platform/mapped_file.hpp"

namespace {
bool recording = false;
std::size_t allocations = 0;
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
  return me::platform::traced_main("book_allocation_test", [&] {
    if (argc != 2) return 2;
    const me::platform::MappedFile input{std::filesystem::path(argv[1])};
    me::BookBuilder builder(1 << 16);
    me::itch::parse(input.bytes(), builder);
    const auto warm = builder.stats();
    builder.reset();
    recording = true;
    me::itch::parse(input.bytes(), builder);
    recording = false;
    const auto& replay = builder.stats();
    std::cout << "allocations=" << allocations << " adds=" << replay.adds
              << " unknown_refs=" << replay.unknown_refs << " live=" << replay.live_orders << '\n'
              << std::flush;
    const bool same = replay.adds == warm.adds && replay.live_orders == warm.live_orders &&
                      replay.unknown_refs == warm.unknown_refs;
    return allocations == 0 && same && replay.adds > 0 ? 0 : 1;
  });
}
