#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <variant>

#include "me/book_builder.hpp"
#include "me/histogram.hpp"
#include "me/itch_parser.hpp"

namespace me::pipeline {

// Message types the book does not need; carried so per-type counts stay complete.
struct Unknown {
  unsigned char kind;
  std::uint16_t locate;
};
struct End {};

using Message = std::variant<itch::SystemEvent, itch::Directory, itch::Add, itch::AddMpid,
                             itch::Execute, itch::ExecutePrice, itch::Cancel, itch::Delete,
                             itch::Replace, itch::Trade, Unknown, End>;

// Parser -> book ring item.
struct Event {
  std::uint64_t parse_tsc;
  Message message;
};

// Book -> publisher ring item: the touched symbol's best bid and offer after the
// update, plus both timestamps for the parse-to-book latency.
struct Update {
  std::uint64_t parse_tsc;
  std::uint64_t book_tsc;
  std::uint64_t bid_size;
  std::uint64_t ask_size;
  std::uint32_t bid;  // 0 when the side is empty
  std::uint32_t ask;
  std::uint16_t locate;
  char kind;  // ITCH type of the message; 0 means no update to publish
  bool end;
};

static_assert(std::is_trivially_copyable_v<Event>);
static_assert(std::is_trivially_copyable_v<Update>);

struct Config {
  std::size_t ring_capacity = 1 << 16;
  std::size_t expected_orders = 1 << 20;
  // Logical CPU for each stage; -1 leaves the thread unpinned.
  int parser_cpu = -1;
  int book_cpu = -1;
  int publisher_cpu = -1;
  // Every this many messages the book stage records a digest of all books, so
  // runs can be compared mid-day (a full day ends with every book empty).
  std::uint64_t checkpoint_every = 0;  // 0 disables
  // Threaded run only: release messages at this rate instead of as fast as the
  // file can be parsed (0 = unpaced). Latency is only meaningful below capacity.
  double messages_per_second = 0;
};

inline constexpr std::size_t max_checkpoints = 64;

struct Result {
  itch::Stats parse;
  BookStats book;
  std::uint64_t updates = 0;      // updates delivered to the publisher
  std::uint64_t bbo_changes = 0;  // updates that changed a symbol's best bid or offer
  std::uint64_t book_digest = 0;  // every resting order of every book, in priority order
  std::uint64_t bbo_digest = 0;   // the sequence of published BBO changes
  std::uint64_t checkpoints[max_checkpoints] = {};  // book digests every checkpoint_every
  std::size_t checkpoint_count = 0;
  Histogram latency_ns;  // parse timestamp to book-updated timestamp
  double seconds = 0;
  double ticks_per_ns = 0;
  bool invariant_tsc = false;
  bool pinned[3] = {false, false, false};  // parser, book, publisher
};

// All three stages on the calling thread, no rings: the reference output.
Result run_single(std::span<const std::byte> input, const Config& config);
// Parser, book and publisher threads connected by SPSC rings, each busy-spinning.
Result run_threaded(std::span<const std::byte> input, const Config& config);

// Hash of every resting order of every book: locate, side, price, then id and
// remaining shares in time priority. Equal books give equal digests.
std::uint64_t book_digest(const BookBuilder& builder);

}  // namespace me::pipeline
