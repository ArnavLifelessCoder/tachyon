#include "pipeline.hpp"

#include <chrono>
#include <exception>
#include <memory>
#include <thread>
#include <vector>

#include "me/spsc_ring.hpp"
#include "me/tsc.hpp"
#include "platform/affinity.hpp"

namespace me::pipeline {
namespace {

std::uint64_t mix(std::uint64_t hash, std::uint64_t word) noexcept {
  // splitmix64 finalizer on the word, then a rotate-multiply chain.
  word += 0x9E3779B97F4A7C15ULL;
  word = (word ^ (word >> 30)) * 0xBF58476D1CE4E5B9ULL;
  word = (word ^ (word >> 27)) * 0x94D049BB133111EBULL;
  word ^= word >> 31;
  hash ^= word;
  return ((hash << 27) | (hash >> 37)) * 0x100000001B3ULL;
}

// Book stage: apply one message and describe the touched symbol's top of book.
class BookStage {
 public:
  BookStage(const Config& config, Result& result)
      : builder_(config.expected_orders), every_(config.checkpoint_every), result_(result) {}

  Update apply(const Event& event) {
    const Update update =
        std::visit([&](const auto& m) { return apply(event.parse_tsc, m); }, event.message);
    if (every_ && ++applied_ % every_ == 0 && result_.checkpoint_count < max_checkpoints) {
      result_.checkpoints[result_.checkpoint_count++] = book_digest(builder_);
    }
    return update;
  }
  const BookBuilder& builder() const noexcept { return builder_; }

 private:
  Update apply(std::uint64_t, const Unknown&) noexcept { return {}; }
  Update apply(std::uint64_t, const End&) noexcept { return {}; }
  Update apply(std::uint64_t, const itch::SystemEvent& m) {
    builder_.on(m);
    return {};
  }
  Update apply(std::uint64_t, const itch::Directory& m) {
    builder_.on(m);
    return {};
  }
  template <class M>
  Update apply(std::uint64_t parse_tsc, const M& m) {
    builder_.on(m);
    const std::uint64_t book_tsc = tsc::now();
    Update update{parse_tsc, book_tsc, 0, 0, 0, 0, m.header.stock_locate, kind_of(m), false};
    if (const OrderBook* book = builder_.book(m.header.stock_locate)) {
      if (const Level* bid = book->bids().best()) {
        update.bid = bid->price;
        update.bid_size = bid->total;
      }
      if (const Level* ask = book->asks().best()) {
        update.ask = ask->price;
        update.ask_size = ask->total;
      }
    }
    return update;
  }
  static char kind_of(const itch::Add&) { return 'A'; }
  static char kind_of(const itch::AddMpid&) { return 'F'; }
  static char kind_of(const itch::Execute&) { return 'E'; }
  static char kind_of(const itch::ExecutePrice&) { return 'C'; }
  static char kind_of(const itch::Cancel&) { return 'X'; }
  static char kind_of(const itch::Delete&) { return 'D'; }
  static char kind_of(const itch::Replace&) { return 'U'; }
  static char kind_of(const itch::Trade&) { return 'P'; }

  BookBuilder builder_;
  std::uint64_t every_;
  std::uint64_t applied_ = 0;
  Result& result_;  // checkpoints only; written by the book stage's thread
};

// Publisher stage: keeps the last published BBO per symbol, hashes every change,
// and records parse-to-book latency. Never allocates after construction.
class Publisher {
 public:
  Publisher(Result& result, double ticks_per_ns)
      : result_(result), ns_per_tick_(1.0 / ticks_per_ns), last_(BookBuilder::max_locates) {}

  void consume(const Update& u) noexcept {
    ++result_.updates;
    const std::uint64_t ticks = u.book_tsc >= u.parse_tsc ? u.book_tsc - u.parse_tsc : 0;
    result_.latency_ns.record(static_cast<std::uint64_t>(static_cast<double>(ticks) * ns_per_tick_));
    Bbo& last = last_[u.locate];
    const Bbo now{u.bid, u.ask, u.bid_size, u.ask_size};
    if (now == last) return;
    last = now;
    ++result_.bbo_changes;
    std::uint64_t h = mix(result_.bbo_digest, u.locate);
    h = mix(h, (std::uint64_t{u.bid} << 32) | u.ask);
    h = mix(h, u.bid_size);
    result_.bbo_digest = mix(h, u.ask_size);
  }

 private:
  struct Bbo {
    std::uint32_t bid = 0, ask = 0;
    std::uint64_t bid_size = 0, ask_size = 0;
    friend bool operator==(const Bbo&, const Bbo&) = default;
  };
  Result& result_;
  double ns_per_tick_;
  std::vector<Bbo> last_;
};

// Parser-side handler: stamps each decoded message and hands it to `sink`.
template <class Sink>
struct Stamp {
  Sink& sink;
  template <class M>
  void on(const M& m) {
    sink(Event{tsc::now(), m});
  }
  void on_unknown(unsigned char kind, std::span<const std::byte> payload) {
    const auto locate =
        payload.size() >= 3 ? static_cast<std::uint16_t>((std::to_integer<unsigned>(payload[1]) << 8) |
                                                         std::to_integer<unsigned>(payload[2]))
                            : std::uint16_t{0};
    sink(Event{tsc::now(), Unknown{kind, locate}});
  }
};

void prepare(Result& result) {
  result.invariant_tsc = tsc::invariant();
  result.ticks_per_ns = tsc::ticks_per_ns();
}

}  // namespace

std::uint64_t book_digest(const BookBuilder& builder) {
  std::uint64_t hash = 0;
  for (std::size_t l = 0; l < BookBuilder::max_locates; ++l) {
    const auto* book = builder.book(static_cast<std::uint16_t>(l));
    if (!book) continue;
    for (const BookSide* side : {&book->bids(), &book->asks()}) {
      const auto levels = side->levels();
      for (std::size_t i = levels.size(); i-- > 0;) {
        hash = mix(mix(hash, l), (std::uint64_t{static_cast<std::uint8_t>(side->side())} << 32) |
                                     levels[i]->price);
        for (const Order* order = levels[i]->head; order; order = order->next) {
          hash = mix(mix(hash, order->id), order->qty);
        }
      }
    }
  }
  return hash;
}

Result run_single(std::span<const std::byte> input, const Config& config) {
  auto result = std::make_unique<Result>();
  prepare(*result);
  BookStage book(config, *result);
  Publisher publisher(*result, result->ticks_per_ns);
  auto sink = [&](const Event& event) {
    const Update update = book.apply(event);
    if (update.kind) publisher.consume(update);
  };
  Stamp<decltype(sink)> stamp{sink};
  const auto start = std::chrono::steady_clock::now();
  itch::parse(input, stamp, &result->parse);
  result->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  result->book = book.builder().stats();
  result->book_digest = book_digest(book.builder());
  return *result;
}

Result run_threaded(std::span<const std::byte> input, const Config& config) {
  auto result = std::make_unique<Result>();
  prepare(*result);
  SpscRing<Event> parsed(config.ring_capacity);
  SpscRing<Update> updates(config.ring_capacity);
  BookStage book(config, *result);
  Publisher publisher(*result, result->ticks_per_ns);
  std::exception_ptr parse_error;

  const auto start = std::chrono::steady_clock::now();
  std::thread parser([&] {
    result->pinned[0] = platform::pin_current_thread(config.parser_cpu);
    auto push = [&](const Event& event) {
      while (!parsed.try_push(event)) tsc::cpu_relax();
    };
    Stamp<decltype(push)> stamp{push};
    try {
      itch::parse(input, stamp, &result->parse);
    } catch (...) {
      parse_error = std::current_exception();
    }
    push(Event{tsc::now(), End{}});  // always sent, so the other stages finish
  });
  std::thread book_thread([&] {
    result->pinned[1] = platform::pin_current_thread(config.book_cpu);
    Event event;
    for (;;) {
      while (!parsed.try_pop(event)) tsc::cpu_relax();
      if (std::holds_alternative<End>(event.message)) break;
      const Update update = book.apply(event);
      if (!update.kind) continue;
      while (!updates.try_push(update)) tsc::cpu_relax();
    }
    Update end{};
    end.end = true;
    while (!updates.try_push(end)) tsc::cpu_relax();
  });
  std::thread publisher_thread([&] {
    result->pinned[2] = platform::pin_current_thread(config.publisher_cpu);
    Update update;
    for (;;) {
      while (!updates.try_pop(update)) tsc::cpu_relax();
      if (update.end) break;
      publisher.consume(update);
    }
  });
  parser.join();
  book_thread.join();
  publisher_thread.join();
  result->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (parse_error) std::rethrow_exception(parse_error);
  result->book = book.builder().stats();
  result->book_digest = book_digest(book.builder());
  return *result;
}

}  // namespace me::pipeline
