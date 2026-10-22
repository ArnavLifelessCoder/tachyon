// Whole-system benchmarks. Each invocation runs one mode once and prints one JSON
// object; bench/run_benchmarks.py repeats runs and records machine info.
//
//   parse    FILE                  parse only, single thread
//   book     FILE                  parse + book build, single thread
//   ops      FILE                  per-message book update latency by ITCH type
//   pipeline FILE RATE P B U       threaded pipeline, RATE msgs/s (0 = unpaced), CPUs P B U
//   match    ORDERS                matching engine on a synthetic order stream

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "me/book_builder.hpp"
#include "me/histogram.hpp"
#include "me/itch_parser.hpp"
#include "me/matching_engine.hpp"
#include "me/tsc.hpp"
#include "pipeline.hpp"
#include "platform/affinity.hpp"
#include "platform/mapped_file.hpp"

namespace {
using clock_type = std::chrono::steady_clock;

double seconds_since(clock_type::time_point start) {
  return std::chrono::duration<double>(clock_type::now() - start).count();
}

void histogram_json(std::ostream& out, const me::Histogram& h) {
  out << "{\"count\":" << h.count() << ",\"min\":" << h.min() << ",\"p50\":" << h.percentile(0.5)
      << ",\"p90\":" << h.percentile(0.9) << ",\"p99\":" << h.percentile(0.99)
      << ",\"p999\":" << h.percentile(0.999) << ",\"max\":" << h.max() << ",\"mean\":" << h.mean()
      << ",\"buckets\":[";
  bool first = true;
  h.for_each_bucket([&](std::uint64_t upper, std::uint64_t count) {
    out << (first ? "" : ",") << '[' << upper << ',' << count << ']';
    first = false;
  });
  out << "]}";
}

// Touches every decoded field that matters so the parse cannot be optimized away.
struct Checksum {
  std::uint64_t sum = 0;
  template <class M>
  void on(const M& m) {
    sum += m.header.timestamp ^ m.header.stock_locate;
  }
  void on_unknown(unsigned char kind, std::span<const std::byte> payload) {
    sum += kind + payload.size();
  }
};

int parse_only(const std::filesystem::path& path) {
  const me::platform::MappedFile file(path);
  Checksum handler;
  me::itch::Stats stats;
  const auto start = clock_type::now();
  me::itch::parse(file.bytes(), handler, &stats);
  const double seconds = seconds_since(start);
  std::cout << "{\"mode\":\"parse\",\"messages\":" << stats.messages << ",\"seconds\":" << seconds
            << ",\"messages_per_second\":" << static_cast<double>(stats.messages) / seconds
            << ",\"checksum\":" << handler.sum << "}\n";
  return 0;
}

int book_build(const std::filesystem::path& path) {
  const me::platform::MappedFile file(path);
  me::BookBuilder builder(1 << 22);
  me::itch::Stats stats;
  const auto start = clock_type::now();
  me::itch::parse(file.bytes(), builder, &stats);
  const double seconds = seconds_since(start);
  std::cout << "{\"mode\":\"book\",\"messages\":" << stats.messages << ",\"seconds\":" << seconds
            << ",\"messages_per_second\":" << static_cast<double>(stats.messages) / seconds
            << ",\"unknown_refs\":" << builder.stats().unknown_refs << "}\n";
  return builder.stats().unknown_refs == 0 ? 0 : 3;
}

// Times builder.on(message) for each book-changing message with rdtsc on both sides.
struct OpTimer {
  me::BookBuilder& builder;
  double ns_per_tick;
  std::array<me::Histogram, 256>& by_type;

  template <class M>
  void time(char kind, const M& m) {
    const std::uint64_t t0 = me::tsc::now();
    builder.on(m);
    const std::uint64_t t1 = me::tsc::now();
    by_type[static_cast<unsigned char>(kind)].record(
        static_cast<std::uint64_t>(static_cast<double>(t1 - t0) * ns_per_tick));
  }
  void on(const me::itch::Add& m) { time('A', m); }
  void on(const me::itch::AddMpid& m) { time('F', m); }
  void on(const me::itch::Execute& m) { time('E', m); }
  void on(const me::itch::ExecutePrice& m) { time('C', m); }
  void on(const me::itch::Cancel& m) { time('X', m); }
  void on(const me::itch::Delete& m) { time('D', m); }
  void on(const me::itch::Replace& m) { time('U', m); }
  template <class M>
  void on(const M& m) {
    builder.on(m);
  }
  void on_unknown(unsigned char kind, std::span<const std::byte> payload) {
    builder.on_unknown(kind, payload);
  }
};

int op_latency(const std::filesystem::path& path) {
  const me::platform::MappedFile file(path);
  me::BookBuilder builder(1 << 22);
  const double ns_per_tick = 1.0 / me::tsc::ticks_per_ns();
  static std::array<me::Histogram, 256> by_type{};  // 256 x 30 KB: keep off the stack
  // Cost of the measurement itself: two back-to-back timestamp reads.
  me::Histogram overhead;
  for (int i = 0; i < 1'000'000; ++i) {
    const std::uint64_t t0 = me::tsc::now();
    const std::uint64_t t1 = me::tsc::now();
    overhead.record(static_cast<std::uint64_t>(static_cast<double>(t1 - t0) * ns_per_tick));
  }
  OpTimer timer{builder, ns_per_tick, by_type};
  me::itch::parse(file.bytes(), timer);
  std::cout << "{\"mode\":\"ops\",\"timer_overhead_ns\":";
  histogram_json(std::cout, overhead);
  std::cout << ",\"ops\":{";
  bool first = true;
  for (char kind : std::string_view("AFECXDU")) {
    std::cout << (first ? "" : ",") << '"' << kind << "\":";
    histogram_json(std::cout, by_type[static_cast<unsigned char>(kind)]);
    first = false;
  }
  std::cout << "}}\n";
  return 0;
}

int pipeline(const std::filesystem::path& path, double rate, int p, int b, int u) {
  const me::platform::MappedFile file(path);
  me::pipeline::Config config;
  config.expected_orders = 1 << 22;
  config.messages_per_second = rate;
  config.parser_cpu = p;
  config.book_cpu = b;
  config.publisher_cpu = u;
  const auto r = me::pipeline::run_threaded(file.bytes(), config);
  std::cout << "{\"mode\":\"pipeline\",\"target_messages_per_second\":" << rate
            << ",\"messages\":" << r.parse.messages << ",\"seconds\":" << r.seconds
            << ",\"messages_per_second\":" << static_cast<double>(r.parse.messages) / r.seconds
            << ",\"ring_capacity\":" << config.ring_capacity << ",\"pinned\":[" << r.pinned[0]
            << ',' << r.pinned[1] << ',' << r.pinned[2]
            << "],\"invariant_tsc\":" << (r.invariant_tsc ? "true" : "false") << ",\"latency_ns\":";
  histogram_json(std::cout, r.latency_ns);
  std::cout << "}\n";
  return 0;
}

// Synthetic stream around a mid price: mostly limits near the touch, some IOC and
// market orders, cancels of recent orders, and modifies. Deterministic.
int match(std::uint64_t orders) {
  me::MatchingEngine engine(1 << 20);
  const double ns_per_tick = 1.0 / me::tsc::ticks_per_ns();
  me::Histogram latency;
  std::uint64_t trades = 0, events = 0;
  auto sink = [&](const me::ExecEvent& e) {
    ++events;
    trades += e.type == me::EventType::trade;
  };
  std::uint64_t state = 20261022;
  auto next = [&] {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<std::uint32_t>(state >> 33);
  };
  const auto start = clock_type::now();
  for (std::uint64_t id = 1; id <= orders; ++id) {
    const std::uint32_t r = next();
    const std::uint32_t pick = r % 100;
    const std::uint64_t t0 = me::tsc::now();
    if (pick < 15 && id > 100) {
      engine.submit(me::CancelOrder{id - 1 - r % 100}, sink);
    } else if (pick < 20 && id > 100) {
      engine.submit(me::ModifyOrder{id - 1 - r % 100, 9990 + next() % 20, 1 + next() % 200}, sink);
    } else {
      const auto type = pick < 23   ? me::OrderType::market
                        : pick < 30 ? me::OrderType::ioc
                                    : me::OrderType::limit;
      const auto side = (r >> 8) % 2 ? me::Side::buy : me::Side::sell;
      // Buys rest below the mid and sells above it, with some crossing the spread.
      const std::uint32_t offset = next() % 12;
      const std::uint32_t price = side == me::Side::buy ? 9998 - offset + 2 : 10002 + offset - 2;
      engine.submit(me::NewOrder{id, side, price, 1 + next() % 300, type}, sink);
    }
    const std::uint64_t t1 = me::tsc::now();
    latency.record(static_cast<std::uint64_t>(static_cast<double>(t1 - t0) * ns_per_tick));
  }
  const double seconds = seconds_since(start);
  std::cout << "{\"mode\":\"match\",\"orders\":" << orders << ",\"seconds\":" << seconds
            << ",\"orders_per_second\":" << static_cast<double>(orders) / seconds
            << ",\"trades\":" << trades << ",\"events\":" << events
            << ",\"resting_at_end\":" << engine.resting() << ",\"latency_ns\":";
  histogram_json(std::cout, latency);
  std::cout << "}\n";
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const std::string_view mode = argc > 1 ? argv[1] : "";
    // Single-threaded modes run pinned to one core (the pipeline pins its own threads).
    if (mode != "pipeline") me::platform::pin_current_thread(2);
    if (mode == "parse" && argc == 3) return parse_only(argv[2]);
    if (mode == "book" && argc == 3) return book_build(argv[2]);
    if (mode == "ops" && argc == 3) return op_latency(argv[2]);
    if (mode == "pipeline" && argc == 7) {
      return pipeline(argv[2], std::atof(argv[3]), std::atoi(argv[4]), std::atoi(argv[5]),
                      std::atoi(argv[6]));
    }
    if (mode == "match" && argc == 3) return match(std::strtoull(argv[2], nullptr, 10));
    std::cerr << "usage: bench_replay parse|book|ops FILE | pipeline FILE RATE P B U | match N\n";
    return 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
