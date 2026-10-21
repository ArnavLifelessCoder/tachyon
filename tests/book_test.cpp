#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "me/book_builder.hpp"
#include "me/itch_parser.hpp"
#include "me/order_book.hpp"
#include "me/order_map.hpp"
#include "platform/mapped_file.hpp"
#include "reference_book.hpp"

namespace {
std::uint64_t seed = 20261021;
std::uint64_t seeds = 10000;

using me::Side;
using me::itch::Header;

std::vector<me::LevelView> depth(const me::OrderBook* book, Side side, std::size_t n) {
  if (!book) return {};
  std::vector<me::LevelView> out(std::min(n, book->side(side).size()));
  out.resize(book->side(side).depth(out));
  return out;
}

// ---------------------------------------------------------------- OrderMap

TEST(OrderMap, InsertFindEraseAndDuplicates) {
  me::OrderMap<int> map(4);
  int a = 1, b = 2;
  EXPECT_TRUE(map.insert(0, &a));  // id 0 is a valid key
  EXPECT_TRUE(map.insert(7, &b));
  EXPECT_FALSE(map.insert(7, &a));
  EXPECT_EQ(map.find(0), &a);
  EXPECT_EQ(map.find(7), &b);
  EXPECT_EQ(map.find(8), nullptr);
  EXPECT_EQ(map.erase(7), &b);
  EXPECT_EQ(map.erase(7), nullptr);
  EXPECT_EQ(map.size(), 1U);
}

TEST(OrderMap, RandomizedAgainstUnorderedMap) {
  std::mt19937_64 random(seed);
  SCOPED_TRACE("seed=" + std::to_string(seed));
  me::OrderMap<std::uint64_t> map(8);  // small start forces many rehashes
  std::unordered_map<std::uint64_t, std::uint64_t*> expected;
  std::vector<std::uint64_t> storage(4096);
  for (int step = 0; step < 200000; ++step) {
    // A small key space makes long probe runs and wrap-around deletions common.
    const std::uint64_t key = random() % 3000;
    std::uint64_t* value = &storage[random() % storage.size()];
    switch (random() % 3) {
      case 0:
        ASSERT_EQ(map.insert(key, value), expected.emplace(key, value).second);
        break;
      case 1: {
        auto it = expected.find(key);
        ASSERT_EQ(map.erase(key), it == expected.end() ? nullptr : it->second);
        if (it != expected.end()) expected.erase(it);
        break;
      }
      default: {
        auto it = expected.find(key);
        ASSERT_EQ(map.find(key), it == expected.end() ? nullptr : it->second);
      }
    }
    ASSERT_EQ(map.size(), expected.size());
  }
  for (const auto& [key, value] : expected) ASSERT_EQ(map.find(key), value);
}

TEST(OrderMap, ReserveAvoidsGrowthAndClearKeepsCapacity) {
  me::OrderMap<int> map(1000);
  const auto capacity = map.capacity();
  int value = 0;
  for (std::uint64_t i = 0; i < 1000; ++i) ASSERT_TRUE(map.insert(i * 4096, &value));
  EXPECT_EQ(map.capacity(), capacity);
  map.clear();
  EXPECT_EQ(map.size(), 0U);
  EXPECT_EQ(map.capacity(), capacity);
  EXPECT_EQ(map.find(4096), nullptr);
}

// ---------------------------------------------------------------- Pool

TEST(Pool, RecyclesAddressesAndGrowsByChunk) {
  me::Pool<me::Order> pool;
  me::Order* first = pool.allocate();
  EXPECT_EQ(pool.capacity(), me::Pool<me::Order>::chunk_size);
  EXPECT_EQ(pool.live(), 1U);
  pool.release(first);
  EXPECT_EQ(pool.allocate(), first);
  std::vector<me::Order*> held;
  for (std::size_t i = 0; i < me::Pool<me::Order>::chunk_size; ++i) held.push_back(pool.allocate());
  EXPECT_EQ(pool.capacity(), 2 * me::Pool<me::Order>::chunk_size);
  std::sort(held.begin(), held.end());
  EXPECT_EQ(std::adjacent_find(held.begin(), held.end()), held.end());
}

// ---------------------------------------------------------------- OrderBook

class Book : public ::testing::Test {
 protected:
  me::Pool<me::Order> orders;
  me::Pool<me::Level> levels;
  me::OrderBook book{orders, levels};
};

TEST_F(Book, FifoTotalsAndUnlinkFromEveryPosition) {
  auto* a = book.add(1, Side::buy, 100, 10, 0);
  auto* b = book.add(2, Side::buy, 100, 20, 0);
  auto* c = book.add(3, Side::buy, 100, 30, 0);
  const me::Level* level = book.bids().best();
  EXPECT_EQ(level->head, a);
  EXPECT_EQ(level->tail, c);
  EXPECT_EQ(level->total, 60U);
  book.remove(b);  // middle
  EXPECT_EQ(a->next, c);
  EXPECT_EQ(c->prev, a);
  book.remove(a);  // head
  EXPECT_EQ(level->head, c);
  EXPECT_FALSE(book.reduce(c, 5));
  EXPECT_EQ(level->total, 25U);
  EXPECT_TRUE(book.valid());
  EXPECT_TRUE(book.reduce(c, 25));  // tail and last order: level removed
  EXPECT_EQ(book.bids().size(), 0U);
  EXPECT_EQ(orders.live(), 0U);
  EXPECT_EQ(levels.live(), 0U);
}

TEST_F(Book, LevelsSortedBestFirstOnBothSides) {
  for (std::uint32_t price : {100U, 104U, 99U, 102U, 104U}) book.add(price, Side::buy, price, 1, 0);
  for (std::uint32_t price : {110U, 106U, 120U, 106U})
    book.add(price + 1000, Side::sell, price, 1, 0);
  std::array<me::LevelView, 8> out{};
  ASSERT_EQ(book.bids().depth(out), 4U);
  EXPECT_EQ(out[0], (me::LevelView{104, 2, 2}));
  EXPECT_EQ(out[3].price, 99U);
  ASSERT_EQ(book.asks().depth(out), 3U);
  EXPECT_EQ(out[0], (me::LevelView{106, 2, 2}));
  EXPECT_EQ(out[2].price, 120U);
  EXPECT_FALSE(book.crossed());
  book.add(50, Side::buy, 106, 1, 0);
  EXPECT_TRUE(book.crossed());  // locked counts as crossed at this level
  EXPECT_TRUE(book.valid());
}

TEST_F(Book, ClearReturnsEverythingToThePools) {
  for (std::uint64_t i = 0; i < 100; ++i)
    book.add(i, i % 2 ? Side::buy : Side::sell, 100 + i % 7, 5, 0);
  book.clear();
  EXPECT_EQ(orders.live(), 0U);
  EXPECT_EQ(levels.live(), 0U);
  EXPECT_EQ(book.bids().size() + book.asks().size(), 0U);
}

// ---------------------------------------------------------------- BookBuilder per message type

Header h(std::uint16_t locate, std::uint64_t ts = 0) { return {locate, 0, ts}; }
me::itch::Add add(std::uint16_t locate, std::uint64_t id, char side, std::uint32_t qty,
                  std::uint32_t price) {
  return {h(locate), id, side, qty, {}, price};
}

TEST(BookBuilder, EveryMessageType) {
  me::BookBuilder b(64);
  b.on(add(1, 10, 'B', 100, 5000));
  b.on(me::itch::AddMpid{h(1), 11, 'S', 50, {}, 5100, {}});
  b.on(me::itch::Execute{h(1), 10, 30, 1});
  EXPECT_EQ(b.find(10)->qty, 70U);
  b.on(me::itch::ExecutePrice{h(1), 10, 20, 2, 'Y', 4999});  // price may differ; book uses shares
  EXPECT_EQ(b.find(10)->qty, 50U);
  b.on(me::itch::Cancel{h(1), 11, 10});
  EXPECT_EQ(b.find(11)->qty, 40U);
  b.on(me::itch::Replace{h(1), 11, 12, 25, 5200});
  EXPECT_EQ(b.find(11), nullptr);
  ASSERT_NE(b.find(12), nullptr);
  EXPECT_EQ(b.find(12)->side, Side::sell);  // side kept from the old order
  EXPECT_EQ(b.find(12)->price, 5200U);
  b.on(me::itch::Trade{h(1), 0, 'B', 5, {}, 5000, 3});  // no book effect
  b.on(me::itch::Execute{h(1), 10, 50, 4});             // full execution removes
  EXPECT_EQ(b.find(10), nullptr);
  b.on(me::itch::Delete{h(1), 12});
  EXPECT_EQ(b.book(1)->bids().size() + b.book(1)->asks().size(), 0U);
  const auto& s = b.stats();
  EXPECT_EQ(s.adds, 2U);
  EXPECT_EQ(s.executes, 3U);
  EXPECT_EQ(s.cancels, 1U);
  EXPECT_EQ(s.replaces, 1U);
  EXPECT_EQ(s.deletes, 1U);
  EXPECT_EQ(s.trades, 1U);
  EXPECT_EQ(s.unknown_refs, 0U);
  EXPECT_EQ(s.live_orders, 0U);
  EXPECT_EQ(s.peak_live_orders, 2U);
}

TEST(BookBuilder, UnknownDuplicateOverExecutionAndLocateMismatch) {
  me::BookBuilder b(64);
  b.on(me::itch::Delete{h(1), 99});
  b.on(me::itch::Execute{h(1), 99, 1, 1});
  b.on(me::itch::Replace{h(1), 99, 100, 1, 1});
  EXPECT_EQ(b.stats().unknown_refs, 3U);
  b.on(add(1, 1, 'B', 10, 100));
  b.on(add(1, 1, 'B', 10, 100));
  EXPECT_EQ(b.stats().duplicate_adds, 1U);
  b.on(me::itch::Cancel{h(2), 1, 1});
  EXPECT_EQ(b.stats().locate_mismatches, 1U);
  EXPECT_EQ(b.find(1)->qty, 9U);  // applied to the order's own book
  b.on(me::itch::Execute{h(1), 1, 50, 2});
  EXPECT_EQ(b.stats().over_executions, 1U);
  EXPECT_EQ(b.find(1), nullptr);
}

TEST(BookBuilder, CrossedLockedAndEpisodes) {
  me::BookBuilder b(64);
  const std::uint64_t four_am = 4ULL * 3'600'000'000'000ULL;
  b.on(me::itch::Add{h(3, four_am), 1, 'B', 10, {}, 100});
  b.on(me::itch::Add{h(3, four_am), 2, 'S', 10, {}, 100});  // locked
  b.on(me::itch::Add{h(3, four_am), 3, 'S', 10, {}, 99});   // crossed, same episode
  b.on(me::itch::Delete{h(3, four_am), 3});                 // still locked
  b.on(me::itch::Delete{h(3, four_am), 2});                 // uncrossed
  b.on(me::itch::Add{h(3, four_am), 4, 'S', 10, {}, 90});   // second episode
  const auto& s = b.stats();
  EXPECT_EQ(s.locked_events, 2U);
  EXPECT_EQ(s.crossed_events, 2U);
  EXPECT_EQ(s.crossed_episodes, 2U);
  EXPECT_EQ(s.crossed_events_by_hour[4], 4U);
  EXPECT_EQ(b.crossed_events_for(3), 4U);
}

// ---------------------------------------------------------------- differential test

// Random ITCH-shaped event streams applied to both the fast builder and the
// reference. After every event, top-5 depth on both sides of the touched book
// must match exactly; every 64 events and at the end the full books are
// compared and the fast book's structure is checked.
struct Generator {
  std::mt19937_64 random;
  std::vector<std::pair<std::uint64_t, std::uint16_t>> live;  // id, locate
  std::uint64_t next_id = 1;
  std::uint16_t locates;

  explicit Generator(std::uint64_t s) : random(s), locates(static_cast<std::uint16_t>(1 + s % 3)) {}

  std::uint64_t pick(std::uint64_t n) { return random() % n; }

  std::uint32_t price(char side) {
    // Mostly near a mid of 100.0000 in one-cent ticks, sometimes deep, sometimes
    // through the other side so crossed and locked books occur.
    const std::uint32_t mid = 1'000'000;
    std::int64_t ticks = static_cast<std::int64_t>(pick(8));
    if (pick(20) == 0) ticks = static_cast<std::int64_t>(pick(400));
    if (pick(25) == 0) ticks = -static_cast<std::int64_t>(pick(4));
    const std::int64_t offset = (side == 'B' ? -1 : 1) * (ticks + 1) * 100;
    return static_cast<std::uint32_t>(mid + offset);
  }

  std::uint32_t shares() { return static_cast<std::uint32_t>(1 + pick(pick(4) == 0 ? 5000 : 300)); }

  // Applies one random event to both builders; returns the locate it names.
  template <class Apply>
  std::uint16_t step(Apply&& apply, const me::BookBuilder& fast) {
    const auto locate = static_cast<std::uint16_t>(1 + pick(locates));
    const std::uint64_t ts = pick(24) * 3'600'000'000'000ULL;
    const auto r = pick(100);
    if (live.empty() || r < 35) {
      const char side = pick(2) ? 'B' : 'S';
      const auto id =
          (pick(100) == 0 && next_id > 1) ? pick(next_id) : next_id++;  // rare duplicate
      const bool already_live = fast.find(id) != nullptr;
      if (pick(4) == 0) {
        apply(me::itch::AddMpid{{locate, 0, ts}, id, side, shares(), {}, price(side), {}});
      } else {
        apply(me::itch::Add{{locate, 0, ts}, id, side, shares(), {}, price(side)});
      }
      if (!already_live && fast.find(id)) live.emplace_back(id, locate);
      return locate;
    }
    // Unknown references must be ignored identically by both books.
    if (r < 38) {
      apply(me::itch::Delete{{locate, 0, ts}, next_id + 1000000});
      return locate;
    }
    const auto index = pick(live.size());
    const auto [id, owner] = live[index];
    const me::Order* order = fast.find(id);
    const std::uint32_t resting = order ? order->qty : 1;
    std::uint32_t amount = static_cast<std::uint32_t>(1 + pick(resting));
    if (pick(50) == 0)
      amount = resting + 1 + static_cast<std::uint32_t>(pick(10));  // over-execution
    const Header header{owner, 0, ts};
    if (r < 55) {
      apply(me::itch::Execute{header, id, amount, 0});
    } else if (r < 62) {
      apply(me::itch::ExecutePrice{header, id, amount, 0, 'Y', 0});
    } else if (r < 77) {
      apply(me::itch::Cancel{header, id, amount});
    } else if (r < 90) {
      apply(me::itch::Delete{header, id});
    } else {
      const char side = order && order->side == Side::sell ? 'S' : 'B';
      apply(me::itch::Replace{header, id, next_id++, shares(), price(side)});
    }
    // Only the referenced id can have stopped resting; swap-remove it, then pick
    // up the replacement id.
    if (!fast.find(id)) {
      live[index] = live.back();
      live.pop_back();
    }
    if (r >= 90 && fast.find(next_id - 1)) live.emplace_back(next_id - 1, owner);
    return owner;
  }
};

void compare_full(const me::BookBuilder& fast, ref::RefBuilder& slow, std::uint16_t locate) {
  for (Side side : {Side::buy, Side::sell}) {
    ASSERT_EQ(depth(fast.book(locate), side, 1 << 20), slow.book(locate).depth(side, 1 << 20))
        << "locate=" << locate << " side=" << static_cast<int>(side);
  }
  if (fast.book(locate)) ASSERT_TRUE(fast.book(locate)->valid());
}

TEST(BookDifferential, TopFiveMatchesReferenceAfterEveryEvent) {
  std::uint64_t events = 0, crossed = 0;
  // One builder for all seeds: reset() keeps capacity, which also exercises
  // reuse of recycled orders and levels across streams.
  me::BookBuilder fast(64);
  for (std::uint64_t s = seed; s < seed + seeds; ++s) {
    SCOPED_TRACE("seed=" + std::to_string(s) + " (rerun with --seed=" + std::to_string(s) +
                 " --seeds=1)");
    fast.reset();
    ref::RefBuilder slow;
    Generator gen(s);
    const auto count = 50 + gen.pick(350);
    for (std::uint64_t e = 0; e < count; ++e) {
      const auto locate = gen.step(
          [&](const auto& message) {
            fast.on(message);
            slow.on(message);
          },
          fast);
      for (Side side : {Side::buy, Side::sell}) {
        ASSERT_EQ(depth(fast.book(locate), side, 5), slow.book(locate).depth(side, 5))
            << "event=" << e << " locate=" << locate << " side=" << static_cast<int>(side);
      }
      if (e % 64 == 63) {
        for (std::uint16_t l = 1; l <= gen.locates; ++l) compare_full(fast, slow, l);
      }
    }
    for (std::uint16_t l = 1; l <= gen.locates; ++l) compare_full(fast, slow, l);
    ASSERT_EQ(fast.stats().unknown_refs, slow.unknown_refs);
    ASSERT_EQ(fast.stats().duplicate_adds, slow.duplicate_adds);
    ASSERT_EQ(fast.stats().live_orders, slow.live());
    events += count;
    crossed += fast.stats().crossed_events + fast.stats().locked_events;
  }
  std::cout << "seeds=" << seeds << " base_seed=" << seed << " events=" << events
            << " crossed_or_locked_events=" << crossed << '\n';
  // Guard against a generator that stops producing the interesting cases.
  EXPECT_GE(events, seeds * 50);
  if (seeds >= 100) EXPECT_GT(crossed, 0U);
}

// ---------------------------------------------------------------- real fixture

TEST(BookFixture, RealDayPrefixHasNoUnknownReferencesAndMatchesReference) {
  const me::platform::MappedFile file(std::filesystem::path(ME_FIXTURE_DIR) /
                                      "01302019.AAPL-MSFT-SPY.itch");
  me::BookBuilder fast;
  ref::RefBuilder slow;
  me::itch::parse(file.bytes(), fast);
  me::itch::parse(file.bytes(), slow);
  const auto& s = fast.stats();
  EXPECT_EQ(s.unknown_refs, 0U);
  EXPECT_EQ(s.locate_mismatches, 0U);
  EXPECT_EQ(s.duplicate_adds, 0U);
  EXPECT_EQ(s.over_executions, 0U);
  EXPECT_EQ(s.adds, 85572U + 1719U);  // A + F counts from the fixture manifest
  EXPECT_EQ(s.live_orders, slow.live());
  for (const std::uint16_t locate : std::array<std::uint16_t, 3>{14, 5217, 7291}) {
    compare_full(fast, slow, locate);
  }
  EXPECT_EQ(std::string(fast.symbol(14).data(), 4), "AAPL");
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    std::uint64_t* target = arg.starts_with("--seed=")    ? &seed
                            : arg.starts_with("--seeds=") ? &seeds
                                                          : nullptr;
    if (!target) continue;
    const auto value = arg.substr(arg.find('=') + 1);
    const auto result = std::from_chars(value.data(), value.data() + value.size(), *target);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return 2;
    for (int j = i; j < argc - 1; ++j) argv[j] = argv[j + 1];
    --argc;
    --i;
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
