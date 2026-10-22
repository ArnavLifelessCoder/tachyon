// Component micro-benchmarks (Google Benchmark): the order id map against
// std::unordered_map, the SPSC ring with and without cache-line padding, and
// sorted-vector price levels against std::map.

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "me/order_book.hpp"
#include "me/order_map.hpp"
#include "me/spsc_ring.hpp"
#include "me/tsc.hpp"
#include "platform/affinity.hpp"

namespace {

// ITCH-like ids: increasing, with irregular gaps.
std::vector<std::uint64_t> make_ids(std::size_t n, std::uint64_t seed) {
  std::mt19937_64 random(seed);
  std::vector<std::uint64_t> ids(n);
  std::uint64_t id = 1'000'000;
  for (auto& x : ids) x = id += 1 + random() % 8;
  return ids;
}

constexpr std::size_t kLive = 1 << 20;  // about the peak live order count of a real day
int dummy = 0;

// ---------------------------------------------------------------- id maps

template <class Map>
struct MapOps;
template <>
struct MapOps<me::OrderMap<int>> {
  static auto make() { return std::make_unique<me::OrderMap<int>>(kLive); }
  static void insert(me::OrderMap<int>& m, std::uint64_t k) { m.insert(k, &dummy); }
  static int* find(me::OrderMap<int>& m, std::uint64_t k) { return m.find(k); }
  static void erase(me::OrderMap<int>& m, std::uint64_t k) { m.erase(k); }
};
template <>
struct MapOps<std::unordered_map<std::uint64_t, int*>> {
  using M = std::unordered_map<std::uint64_t, int*>;
  static auto make() {
    auto m = std::make_unique<M>();
    m->reserve(kLive);
    return m;
  }
  static void insert(M& m, std::uint64_t k) { m.emplace(k, &dummy); }
  static int* find(M& m, std::uint64_t k) {
    auto it = m.find(k);
    return it == m.end() ? nullptr : it->second;
  }
  static void erase(M& m, std::uint64_t k) { m.erase(k); }
};

template <class Map>
void BM_MapInsert(benchmark::State& state) {
  const auto ids = make_ids(kLive, 1);
  for (auto _ : state) {
    state.PauseTiming();
    auto map = MapOps<Map>::make();
    state.ResumeTiming();
    for (auto id : ids) MapOps<Map>::insert(*map, id);
    benchmark::DoNotOptimize(map.get());
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * ids.size()));
}

template <class Map>
void BM_MapLookupHit(benchmark::State& state) {
  const auto ids = make_ids(kLive, 1);
  auto map = MapOps<Map>::make();
  for (auto id : ids) MapOps<Map>::insert(*map, id);
  std::vector<std::uint64_t> probes(1 << 16);
  std::mt19937_64 random(2);
  for (auto& p : probes) p = ids[random() % ids.size()];
  std::size_t i = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(MapOps<Map>::find(*map, probes[i++ & (probes.size() - 1)]));
  }
  state.SetItemsProcessed(state.iterations());
}

template <class Map>
void BM_MapLookupMiss(benchmark::State& state) {
  const auto ids = make_ids(kLive, 1);
  auto map = MapOps<Map>::make();
  for (auto id : ids) MapOps<Map>::insert(*map, id);
  std::vector<std::uint64_t> probes(1 << 16);
  std::mt19937_64 random(3);
  for (auto& p : probes) p = (random() | (1ULL << 62));  // far outside the id range
  std::size_t i = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(MapOps<Map>::find(*map, probes[i++ & (probes.size() - 1)]));
  }
  state.SetItemsProcessed(state.iterations());
}

// Steady state of a trading day: kLive orders resting, each step adds a new id
// and deletes a random resting one (ITCH deletes are spread across the book).
template <class Map>
void BM_MapChurn(benchmark::State& state) {
  auto ids = make_ids(kLive, 1);
  auto map = MapOps<Map>::make();
  for (auto id : ids) MapOps<Map>::insert(*map, id);
  std::mt19937_64 random(4);
  std::uint64_t next = ids.back() + 1;
  for (auto _ : state) {
    const std::size_t victim = random() % ids.size();
    MapOps<Map>::erase(*map, ids[victim]);
    ids[victim] = next += 1 + random() % 8;
    MapOps<Map>::insert(*map, ids[victim]);
  }
  state.SetItemsProcessed(state.iterations());
}

using Custom = me::OrderMap<int>;
using Std = std::unordered_map<std::uint64_t, int*>;
BENCHMARK_TEMPLATE(BM_MapInsert, Custom)->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_MapInsert, Std)->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_MapLookupHit, Custom);
BENCHMARK_TEMPLATE(BM_MapLookupHit, Std);
BENCHMARK_TEMPLATE(BM_MapLookupMiss, Custom);
BENCHMARK_TEMPLATE(BM_MapLookupMiss, Std);
BENCHMARK_TEMPLATE(BM_MapChurn, Custom);
BENCHMARK_TEMPLATE(BM_MapChurn, Std);

// ---------------------------------------------------------------- ring padding

// Same algorithm as me::SpscRing, but every field packed together, so the
// producer's and consumer's indices share cache lines.
template <class T>
class UnpaddedRing {
 public:
  explicit UnpaddedRing(std::size_t capacity)
      : mask_(capacity - 1), slots_(std::make_unique<T[]>(capacity)) {}
  bool try_push(const T& item) noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - cached_head_ > mask_) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (tail - cached_head_ > mask_) return false;
    }
    slots_[tail & mask_] = item;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }
  bool try_pop(T& item) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == cached_tail_) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (head == cached_tail_) return false;
    }
    item = slots_[head & mask_];
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

 private:
  std::atomic<std::size_t> tail_{0};
  std::size_t cached_head_ = 0;
  std::atomic<std::size_t> head_{0};
  std::size_t cached_tail_ = 0;
  const std::size_t mask_;
  std::unique_ptr<T[]> slots_;
};

template <class Ring>
void BM_RingTransfer(benchmark::State& state) {
  constexpr std::uint64_t items = 10'000'000;
  for (auto _ : state) {
    Ring ring(1024);
    std::uint64_t sum = 0;
    // Producer on logical CPU 2; consumer on the CPU given as the argument. Unpinned,
    // the scheduler sometimes places both on one core's hyperthreads, which share L1
    // and hide false sharing. On the recorded machine (two 8-core CCDs) CPU 4 is
    // another core on the same CCD and CPU 18 is a core on the other CCD.
    const int consumer_cpu = static_cast<int>(state.range(0));
    std::thread consumer([&] {
      me::platform::pin_current_thread(consumer_cpu);
      std::uint64_t value = 0;
      for (std::uint64_t i = 0; i < items; ++i) {
        while (!ring.try_pop(value)) me::tsc::cpu_relax();
        sum += value;
      }
    });
    me::platform::pin_current_thread(2);
    for (std::uint64_t i = 0; i < items; ++i) {
      while (!ring.try_push(i)) me::tsc::cpu_relax();
    }
    consumer.join();
    benchmark::DoNotOptimize(sum);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * items));
}
BENCHMARK_TEMPLATE(BM_RingTransfer, me::SpscRing<std::uint64_t>)
    ->ArgName("consumer_cpu")
    ->Arg(4)
    ->Arg(18)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
BENCHMARK_TEMPLATE(BM_RingTransfer, UnpaddedRing<std::uint64_t>)
    ->ArgName("consumer_cpu")
    ->Arg(4)
    ->Arg(18)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

// ---------------------------------------------------------------- price levels

// Level churn shaped like ITCH: most new levels within a few ticks of the touch,
// some deep. Each step creates (or finds) a level and removes one.
std::vector<std::uint32_t> level_prices(std::size_t n) {
  std::mt19937_64 random(5);
  std::vector<std::uint32_t> prices(n);
  for (auto& p : prices) {
    const std::uint32_t depth = random() % 10 < 8 ? random() % 5 : random() % 200;
    p = 100'000 - depth * 100;  // bids: best is highest
  }
  return prices;
}

void BM_LevelsSortedVector(benchmark::State& state) {
  const auto prices = level_prices(1 << 16);
  me::Pool<me::Level> pool;
  me::BookSide side(me::Side::buy);
  std::vector<me::Level*> live;
  std::size_t i = 0;
  for (auto _ : state) {
    me::Level* level = side.find_or_insert(prices[i++ & 0xFFFF], pool);
    level->count += 1;
    live.push_back(level);
    if (live.size() > 64) {  // keep a realistic number of levels resting
      me::Level* victim = live[i % live.size()];
      live[i % live.size()] = live.back();
      live.pop_back();
      if (--victim->count == 0) side.erase(victim, pool);
    }
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_LevelsStdMap(benchmark::State& state) {
  const auto prices = level_prices(1 << 16);
  std::map<std::uint32_t, std::uint32_t, std::greater<>> side;  // price -> order count
  std::vector<std::uint32_t> live;
  std::size_t i = 0;
  for (auto _ : state) {
    const std::uint32_t price = prices[i++ & 0xFFFF];
    side[price] += 1;
    live.push_back(price);
    if (live.size() > 64) {
      const std::uint32_t victim = live[i % live.size()];
      live[i % live.size()] = live.back();
      live.pop_back();
      auto it = side.find(victim);
      if (--it->second == 0) side.erase(it);
    }
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_LevelsSortedVector);
BENCHMARK(BM_LevelsStdMap);

}  // namespace

BENCHMARK_MAIN();
