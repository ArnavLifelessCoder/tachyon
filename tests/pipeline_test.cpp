#include "pipeline.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "me/histogram.hpp"
#include "me/spsc_ring.hpp"
#include "me/tsc.hpp"
#include "platform/mapped_file.hpp"

namespace {
std::uint64_t items = 100'000'000;
std::uint64_t seed = 20261021;

// ---------------------------------------------------------------- SPSC ring

TEST(SpscRing, RejectsBadCapacity) {
  EXPECT_THROW(me::SpscRing<int>(0), std::invalid_argument);
  EXPECT_THROW(me::SpscRing<int>(1), std::invalid_argument);
  EXPECT_THROW(me::SpscRing<int>(6), std::invalid_argument);
}

TEST(SpscRing, FullEmptyAndWrapAround) {
  me::SpscRing<int> ring(4);
  int out = 0;
  EXPECT_FALSE(ring.try_pop(out));
  for (int round = 0; round < 10; ++round) {  // indices pass the capacity many times
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(ring.try_push(round * 10 + i));
    EXPECT_FALSE(ring.try_push(-1));
    for (int i = 0; i < 4; ++i) {
      ASSERT_TRUE(ring.try_pop(out));
      EXPECT_EQ(out, round * 10 + i);
    }
    EXPECT_FALSE(ring.try_pop(out));
  }
  // Interleaved: never more than three in flight.
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(ring.try_push(i));
    if (i % 2) {
      ASSERT_TRUE(ring.try_pop(out));
      ASSERT_TRUE(ring.try_pop(out));
    }
  }
}

TEST(SpscRing, IndexLinesDoNotShareCacheLines) {
  // The producer and consumer indices must sit on different 64-byte lines.
  EXPECT_GE(alignof(me::SpscRing<int>), me::cache_line);
  EXPECT_GE(sizeof(me::SpscRing<int>), 3 * me::cache_line);
}

// Two threads, a small ring so both the full and the empty paths are hit
// constantly; every item must arrive exactly once and in order.
TEST(SpscRing, StressOrderAndNoLoss) {
  me::SpscRing<std::uint64_t> ring(1024);
  std::uint64_t mismatches = 0, received = 0;
  std::thread consumer([&] {
    std::uint64_t value = 0;
    for (std::uint64_t expected = 0; expected < items; ++expected) {
      while (!ring.try_pop(value)) me::tsc::cpu_relax();
      mismatches += value != expected;
      ++received;
    }
  });
  for (std::uint64_t i = 0; i < items; ++i) {
    while (!ring.try_push(i)) me::tsc::cpu_relax();
  }
  consumer.join();
  std::uint64_t leftover = 0;
  EXPECT_FALSE(ring.try_pop(leftover));
  EXPECT_EQ(received, items);
  EXPECT_EQ(mismatches, 0U);
  std::cout << "items=" << items << '\n';
}

// Items wider than one machine word: a torn read would show up as an item
// whose fields disagree.
TEST(SpscRing, StressWideItemsAreNeverTorn) {
  struct Wide {
    std::uint64_t a, b, c, d;
  };
  me::SpscRing<Wide> ring(64);
  const std::uint64_t n = std::min<std::uint64_t>(items / 10, 10'000'000);
  std::uint64_t bad = 0;
  std::thread consumer([&] {
    Wide w{};
    for (std::uint64_t i = 0; i < n; ++i) {
      while (!ring.try_pop(w)) me::tsc::cpu_relax();
      bad += !(w.a == i && w.b == ~i && w.c == i * 3 && w.d == (i ^ 0x5555));
    }
  });
  for (std::uint64_t i = 0; i < n; ++i) {
    while (!ring.try_push(Wide{i, ~i, i * 3, i ^ 0x5555})) me::tsc::cpu_relax();
  }
  consumer.join();
  EXPECT_EQ(bad, 0U);
}

// ---------------------------------------------------------------- histogram

TEST(Histogram, ExactBelowSubBucketRangeAndBucketBounds) {
  me::Histogram h;
  for (std::uint64_t v = 0; v < 64; ++v) h.record(v);
  EXPECT_EQ(h.count(), 64U);
  EXPECT_EQ(h.min(), 0U);
  EXPECT_EQ(h.max(), 63U);
  EXPECT_EQ(h.percentile(0.5), 31U);
  EXPECT_EQ(h.percentile(1.0), 63U);
  for (std::uint64_t v : {64ULL, 65ULL, 127ULL, 128ULL, 1000ULL, 123456789ULL, ~0ULL}) {
    const auto i = me::Histogram::index(v);
    ASSERT_LT(i, me::Histogram::bucket_count);
    EXPECT_GE(me::Histogram::upper(i), v);
    if (i > 0) {
      EXPECT_LT(me::Histogram::upper(i - 1), v);
    }
  }
}

TEST(Histogram, PercentilesWithinRelativeErrorOfExact) {
  std::mt19937_64 random(seed);
  std::lognormal_distribution<double> latency(5.0, 1.2);  // a long right tail, like real latency
  std::vector<std::uint64_t> values;
  me::Histogram h;
  for (int i = 0; i < 200000; ++i) {
    const auto v = static_cast<std::uint64_t>(latency(random));
    values.push_back(v);
    h.record(v);
  }
  std::sort(values.begin(), values.end());
  for (double q : {0.5, 0.9, 0.99, 0.999, 1.0}) {
    const auto rank = std::clamp<std::size_t>(
        static_cast<std::size_t>(q * static_cast<double>(values.size()) + 0.5), 1, values.size());
    const double exact = static_cast<double>(values[rank - 1]);
    const double reported = static_cast<double>(h.percentile(q));
    EXPECT_GE(reported, exact) << "q=" << q;
    EXPECT_LE(reported, exact * (1.0 + 1.0 / 64) + 1) << "q=" << q;
  }
  EXPECT_EQ(h.max(), values.back());
}

TEST(Histogram, MergeEqualsRecordingEverything) {
  me::Histogram a, b, all;
  for (std::uint64_t v = 0; v < 5000; v += 7) {
    (v % 2 ? a : b).record(v * v);
    all.record(v * v);
  }
  a.merge(b);
  for (double q : {0.1, 0.5, 0.99}) EXPECT_EQ(a.percentile(q), all.percentile(q));
  EXPECT_EQ(a.count(), all.count());
  EXPECT_EQ(a.min(), all.min());
  EXPECT_EQ(a.max(), all.max());
}

// ---------------------------------------------------------------- pipeline

void expect_same(const me::pipeline::Result& single, const me::pipeline::Result& threaded) {
  EXPECT_EQ(threaded.book_digest, single.book_digest);
  ASSERT_EQ(threaded.checkpoint_count, single.checkpoint_count);
  for (std::size_t i = 0; i < single.checkpoint_count; ++i) {
    EXPECT_EQ(threaded.checkpoints[i], single.checkpoints[i]) << "checkpoint " << i;
  }
  EXPECT_EQ(threaded.bbo_digest, single.bbo_digest);
  EXPECT_EQ(threaded.updates, single.updates);
  EXPECT_EQ(threaded.bbo_changes, single.bbo_changes);
  EXPECT_EQ(threaded.parse.messages, single.parse.messages);
  EXPECT_EQ(threaded.parse.counts, single.parse.counts);
  EXPECT_EQ(threaded.book.adds, single.book.adds);
  EXPECT_EQ(threaded.book.live_orders, single.book.live_orders);
  EXPECT_EQ(threaded.book.unknown_refs, single.book.unknown_refs);
  EXPECT_EQ(threaded.latency_ns.count(), threaded.updates);
}

TEST(Pipeline, ThreadedMatchesSingleThreadedOnRealFixture) {
  const me::platform::MappedFile file(std::filesystem::path(ME_FIXTURE_DIR) /
                                      "01302019.AAPL-MSFT-SPY.itch");
  me::pipeline::Config config;
  config.expected_orders = 1 << 16;
  config.checkpoint_every = 10000;
  const auto single = me::pipeline::run_single(file.bytes(), config);
  EXPECT_EQ(single.parse.messages, 149213U);
  ASSERT_EQ(single.checkpoint_count, 14U);
  EXPECT_NE(single.checkpoints[13], 0U);  // books are non-empty at the last checkpoint
  EXPECT_NE(single.checkpoints[13], single.checkpoints[12]);
  EXPECT_EQ(single.book.unknown_refs, 0U);
  EXPECT_GT(single.bbo_changes, 0U);
  for (std::size_t capacity :
       {std::size_t{4}, std::size_t{1} << 16}) {  // tiny ring: constant backpressure
    SCOPED_TRACE("ring_capacity=" + std::to_string(capacity));
    config.ring_capacity = capacity;
    expect_same(single, me::pipeline::run_threaded(file.bytes(), config));
  }
}

TEST(Pipeline, DigestDetectsADifferentBook) {
  const me::platform::MappedFile file(std::filesystem::path(ME_FIXTURE_DIR) / "synthetic.itch");
  const auto full = me::pipeline::run_single(file.bytes(), {});
  // Drop the final two messages (D, then the S end-of-session): the book differs.
  const auto bytes = file.bytes();
  const auto prefix = me::pipeline::run_single(bytes.first(bytes.size() - 2 - 14 - 21), {});
  EXPECT_NE(full.book_digest, prefix.book_digest);
}

TEST(Pipeline, ParseErrorReachesTheCallerAndThreadsFinish) {
  const std::array<std::byte, 3> bad{std::byte{0}, std::byte{5}, std::byte{'S'}};
  EXPECT_THROW(me::pipeline::run_threaded(bad, {}), me::itch::ParseError);
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    std::uint64_t* target = arg.starts_with("--items=")  ? &items
                            : arg.starts_with("--seed=") ? &seed
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
