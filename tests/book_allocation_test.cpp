// Hot-path allocation probe. Replays the real fixture into the book builder
// twice: the first pass warms pools, the id map, level vectors and per-locate
// books; after reset() the second pass must not allocate at all. The matching
// engine gets the same treatment on a deterministic order stream.

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>

#include "me/book_builder.hpp"
#include "me/itch_parser.hpp"
#include "me/matching_engine.hpp"
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
    const std::size_t book_allocations = allocations;

    // Matching engine: same warm/reset/replay pattern on a deterministic order stream.
    me::MatchingEngine engine(1 << 16);
    std::uint64_t trades = 0;
    auto sink = [&](const me::ExecEvent& e) { trades += e.type == me::EventType::trade; };
    auto drive = [&] {
      std::uint64_t state = 12345;
      for (std::uint64_t id = 1; id <= 200000; ++id) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const auto r = static_cast<std::uint32_t>(state >> 33);
        if (r % 10 < 2 && id > 10) {
          engine.submit(me::CancelOrder{id - 1 - r % 10}, sink);
        } else {
          const auto type = r % 20 == 0 ? me::OrderType::ioc : me::OrderType::limit;
          engine.submit(me::NewOrder{id, r % 2 ? me::Side::buy : me::Side::sell, 9950 + r % 100,
                                     1 + r % 300, type},
                        sink);
        }
      }
    };
    drive();
    const std::uint64_t warm_trades = trades;
    engine.reset();
    trades = 0;
    allocations = 0;
    recording = true;
    drive();
    recording = false;
    std::cout << "engine_allocations=" << allocations << " trades=" << trades << '\n' << std::flush;
    const bool engine_ok = allocations == 0 && trades == warm_trades && trades > 0;
    return book_allocations == 0 && same && replay.adds > 0 && engine_ok ? 0 : 1;
  });
}
