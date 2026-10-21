#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include "me/itch_parser.hpp"
#include "me/order_book.hpp"
#include "me/order_map.hpp"

// Per-event invariant checks are on in debug builds and compiled out of release
// builds so benchmarks measure the book, not the checks.
#ifndef ME_BOOK_CHECKS
#ifdef NDEBUG
#define ME_BOOK_CHECKS 0
#else
#define ME_BOOK_CHECKS 1
#endif
#endif

namespace me {

struct BookStats {
  std::uint64_t adds{}, executes{}, cancels{}, deletes{}, replaces{}, trades{};
  // Execute/cancel/delete/replace naming an id that is not resting. A correct
  // replay of a complete day must report zero.
  std::uint64_t unknown_refs{};
  std::uint64_t duplicate_adds{};
  std::uint64_t locate_mismatches{};  // reference whose message locate differs from the order's
  std::uint64_t over_executions{};    // shares removed exceed shares resting
  // After a book-changing message: best bid > best ask (crossed) or == (locked).
  std::uint64_t crossed_events{}, locked_events{};
  std::uint64_t crossed_episodes{};  // transitions from neither into crossed or locked
  std::array<std::uint64_t, 24> crossed_events_by_hour{};
  std::uint64_t live_orders{}, peak_live_orders{};
};

// ITCH handler that rebuilds one OrderBook per stock locate. Replaying the feed
// reconstructs the exchange's published book; it does not match anything.
class BookBuilder {
 public:
  static constexpr std::size_t max_locates = 65536;
  static constexpr std::uint32_t no_change = 0x10000;

  explicit BookBuilder(std::size_t expected_orders = 1 << 20)
      : books_(max_locates),
        symbols_(max_locates),
        crossed_(max_locates, 0),
        crossed_by_locate_(max_locates, 0) {
    order_ids_.reserve(expected_orders);
    orders_.reserve(expected_orders);
    levels_.reserve(expected_orders / 8 + Pool<Level>::chunk_size);
  }

  void on(const itch::SystemEvent&) noexcept {}
  void on(const itch::Directory& m) { symbols_[m.header.stock_locate] = m.stock; }
  void on(const itch::Add& m) {
    ++stats_.adds;
    changed(m.header, insert(m.header, m.order_id, side_of(m.side), m.price, m.shares));
  }
  void on(const itch::AddMpid& m) {
    ++stats_.adds;
    changed(m.header, insert(m.header, m.order_id, side_of(m.side), m.price, m.shares));
  }
  void on(const itch::Execute& m) {
    ++stats_.executes;
    changed(m.header, reduce(m.header, m.order_id, m.shares));
  }
  void on(const itch::ExecutePrice& m) {
    ++stats_.executes;
    changed(m.header, reduce(m.header, m.order_id, m.shares));
  }
  void on(const itch::Cancel& m) {
    ++stats_.cancels;
    changed(m.header, reduce(m.header, m.order_id, m.shares));
  }
  void on(const itch::Delete& m) {
    ++stats_.deletes;
    Order* order = lookup(m.header, m.order_id);
    changed(m.header, order ? remove(order) : no_change);
  }
  void on(const itch::Replace& m) {
    ++stats_.replaces;
    // U gives a new id: delete the old order and add the new one, keeping side
    // and symbol from the old order. The new order loses time priority.
    Order* order = lookup(m.header, m.order_id);
    if (!order) return;
    const Side side = order->side;
    itch::Header owner = m.header;
    owner.stock_locate = order->locate;
    const std::uint32_t removed = remove(order);
    const std::uint32_t added = insert(owner, m.new_order_id, side, m.price, m.shares);
    changed(m.header, added != no_change ? added : removed);
  }
  void on(const itch::Trade&) noexcept { ++stats_.trades; }  // non-cross trade: no book effect
  void on_unknown(unsigned char, std::span<const std::byte>) noexcept {}

  const OrderBook* book(std::uint16_t locate) const noexcept { return books_[locate].get(); }
  const std::array<char, 8>& symbol(std::uint16_t locate) const noexcept {
    return symbols_[locate];
  }
  const Order* find(std::uint64_t id) const noexcept { return order_ids_.find(id); }
  const BookStats& stats() const noexcept { return stats_; }
  std::uint64_t crossed_events_for(std::uint16_t locate) const noexcept {
    return crossed_by_locate_[locate];
  }
  // Whether the book was crossed or locked after its most recent change.
  bool crossed_now(std::uint16_t locate) const noexcept { return crossed_[locate] != 0; }

  // Empties every book and the id map but keeps all capacity, so a second
  // replay of the same input allocates nothing. Only locates that have a book
  // are touched.
  void reset() noexcept {
    for (const std::uint16_t locate : active_) {
      books_[locate]->clear();
      crossed_[locate] = 0;
      crossed_by_locate_[locate] = 0;
    }
    order_ids_.clear();
    stats_ = {};
  }

 private:
  static Side side_of(char side) noexcept { return side == 'S' ? Side::sell : Side::buy; }

  // Each mutator returns the locate of the book it changed, or no_change.
  std::uint32_t insert(const itch::Header& h, std::uint64_t id, Side side, std::uint32_t price,
                       std::uint32_t shares) {
    if (order_ids_.find(id)) {
      ++stats_.duplicate_adds;
      return no_change;
    }
    Order* order = book_for(h.stock_locate).add(id, side, price, shares, h.stock_locate);
    order_ids_.insert(id, order);
    if (++stats_.live_orders > stats_.peak_live_orders)
      stats_.peak_live_orders = stats_.live_orders;
    check(order->level, side);
    return h.stock_locate;
  }

  std::uint32_t reduce(const itch::Header& h, std::uint64_t id, std::uint32_t shares) {
    Order* order = lookup(h, id);
    if (!order) return no_change;
    if (shares >= order->qty) {
      if (shares > order->qty) ++stats_.over_executions;
      return remove(order);
    }
    books_[order->locate]->reduce(order, shares);
    check(order->level, order->side);
    return order->locate;
  }

  std::uint32_t remove(Order* order) {
    const std::uint16_t locate = order->locate;
    Level* level = order->level;
    const Side side = order->side;
    const bool level_survives = level->count > 1;
    order_ids_.erase(order->id);
    --stats_.live_orders;
    books_[locate]->remove(order);
    if (level_survives) check(level, side);
    return locate;
  }

  Order* lookup(const itch::Header& h, std::uint64_t id) noexcept {
    Order* order = order_ids_.find(id);
    if (!order) {
      ++stats_.unknown_refs;
      return nullptr;
    }
    if (order->locate != h.stock_locate) ++stats_.locate_mismatches;
    return order;
  }

  OrderBook& book_for(std::uint16_t locate) {
    auto& book = books_[locate];
    if (!book) {
      book = std::make_unique<OrderBook>(orders_, levels_);
      active_.push_back(locate);
    }
    return *book;
  }

  // Called once per message, after the whole message has been applied.
  void changed(const itch::Header& h, std::uint32_t locate) noexcept {
    if (locate == no_change) return;
    const OrderBook& book = *books_[locate];
    const Level* bid = book.bids().best();
    const Level* ask = book.asks().best();
    const bool crossed = bid && ask && bid->price >= ask->price;
    if (crossed) {
      if (bid->price > ask->price) {
        ++stats_.crossed_events;
      } else {
        ++stats_.locked_events;
      }
      ++crossed_by_locate_[locate];
      const std::uint64_t hour = h.timestamp / 3'600'000'000'000ULL;
      ++stats_.crossed_events_by_hour[hour < 24 ? hour : 23];
      if (!crossed_[locate]) ++stats_.crossed_episodes;
    }
    crossed_[locate] = crossed ? 1 : 0;
  }

  static void check([[maybe_unused]] const Level* level, [[maybe_unused]] Side side) {
#if ME_BOOK_CHECKS
    if (!OrderBook::level_valid(*level, side)) throw std::logic_error("book invariant violated");
#endif
  }

  Pool<Order> orders_;
  Pool<Level> levels_;
  OrderMap<Order> order_ids_;
  std::vector<std::unique_ptr<OrderBook>> books_;
  std::vector<std::array<char, 8>> symbols_;
  std::vector<std::uint8_t> crossed_;
  std::vector<std::uint64_t> crossed_by_locate_;
  std::vector<std::uint16_t> active_;  // locates with a book, in creation order
  BookStats stats_;
};

}  // namespace me
