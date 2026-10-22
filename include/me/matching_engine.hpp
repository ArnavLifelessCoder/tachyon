#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "me/order_book.hpp"
#include "me/order_map.hpp"

namespace me {

enum class OrderType : std::uint8_t { limit, market, ioc };

struct NewOrder {
  std::uint64_t id;
  Side side;
  std::uint32_t price;  // ignored for market orders
  std::uint32_t qty;
  OrderType type;
};
struct CancelOrder {
  std::uint64_t id;
};
// new_qty is the new open (unfilled) quantity of the resting order.
struct ModifyOrder {
  std::uint64_t id;
  std::uint32_t new_price;
  std::uint32_t new_qty;
};

enum class EventType : std::uint8_t { ack, trade, cancelled, rejected };
enum class RejectReason : std::uint8_t {
  none,
  invalid_quantity,
  invalid_price,
  duplicate_id,
  unknown_order,
  no_liquidity,
};

// One output event. For a trade, id is the taker and maker_id the resting order;
// price is the maker's price. For cancelled, qty is the quantity removed.
struct ExecEvent {
  EventType type;
  RejectReason reason;
  std::uint64_t id;
  std::uint64_t maker_id;
  std::uint32_t price;
  std::uint32_t qty;
  friend bool operator==(const ExecEvent&, const ExecEvent&) = default;
};

// Single-symbol price-time priority matching engine. It reuses the book builder's
// structures (pooled orders, intrusive level FIFOs, sorted level vectors and the
// open-addressing id map), so resting, cancelling and filling never allocate once
// the pools are warm. Events go to a caller-supplied sink, called as sink(event),
// in the order they happen.
//
// Rules:
//  - A trade executes at the resting order's price; within a level, earliest first.
//  - New order: Ack, then each Trade, then Cancelled for any IOC or market remainder.
//    A limit remainder rests.
//  - Market order: matches at any price. Rejected (no_liquidity), with no Ack, if
//    the opposite side is empty.
//  - Modify: lowering the quantity at the same price keeps time priority. Any price
//    change or quantity increase is a cancel plus re-add: the order goes to the back
//    of its new level and can match on re-entry. A successful modify emits Ack
//    (then any trades from the re-add).
//  - Rejected: zero quantity, zero limit/IOC price, an id already resting, and
//    cancel/modify of an id that is not resting.
//  - No self-trade prevention (out of scope).
class MatchingEngine {
 public:
  explicit MatchingEngine(std::size_t expected_orders = 1 << 16)
      : ids_(expected_orders), book_(orders_, levels_) {
    orders_.reserve(expected_orders);
    levels_.reserve(Pool<Level>::chunk_size);
  }

  template <class Sink>
  void submit(const NewOrder& o, Sink&& sink) {
    if (o.qty == 0) return reject(o.id, RejectReason::invalid_quantity, sink);
    if (o.type != OrderType::market && o.price == 0) {
      return reject(o.id, RejectReason::invalid_price, sink);
    }
    if (ids_.find(o.id)) return reject(o.id, RejectReason::duplicate_id, sink);
    if (o.type == OrderType::market && !opposite(o.side).best()) {
      return reject(o.id, RejectReason::no_liquidity, sink);
    }
    sink(ExecEvent{EventType::ack, RejectReason::none, o.id, 0, o.price, o.qty});
    enter(o.id, o.side, o.price, o.qty, o.type, sink);
  }

  template <class Sink>
  void submit(const CancelOrder& c, Sink&& sink) {
    Order* order = ids_.find(c.id);
    if (!order) return reject(c.id, RejectReason::unknown_order, sink);
    const std::uint32_t qty = order->qty;
    const std::uint32_t price = order->price;
    remove(order);
    sink(ExecEvent{EventType::cancelled, RejectReason::none, c.id, 0, price, qty});
  }

  template <class Sink>
  void submit(const ModifyOrder& m, Sink&& sink) {
    Order* order = ids_.find(m.id);
    if (!order) return reject(m.id, RejectReason::unknown_order, sink);
    if (m.new_qty == 0) return reject(m.id, RejectReason::invalid_quantity, sink);
    if (m.new_price == 0) return reject(m.id, RejectReason::invalid_price, sink);
    sink(ExecEvent{EventType::ack, RejectReason::none, m.id, 0, m.new_price, m.new_qty});
    if (m.new_price == order->price && m.new_qty <= order->qty) {
      if (m.new_qty < order->qty) book_.reduce(order, order->qty - m.new_qty);  // keeps priority
      return;
    }
    const Side side = order->side;
    remove(order);
    enter(m.id, side, m.new_price, m.new_qty, OrderType::limit, sink);
  }

  const OrderBook& book() const noexcept { return book_; }
  const Order* find(std::uint64_t id) const noexcept { return ids_.find(id); }
  std::size_t resting() const noexcept { return ids_.size(); }

  // Empties the book; pool and map capacity is kept.
  void reset() noexcept {
    book_.clear();
    ids_.clear();
  }

 private:
  const BookSide& opposite(Side side) const noexcept {
    return side == Side::buy ? book_.asks() : book_.bids();
  }

  static bool crosses(Side side, std::uint32_t limit, std::uint32_t resting) noexcept {
    return side == Side::buy ? limit >= resting : limit <= resting;
  }

  template <class Sink>
  void enter(std::uint64_t id, Side side, std::uint32_t price, std::uint32_t qty, OrderType type,
             Sink& sink) {
    // Take liquidity from the opposite side, best level first, oldest order first.
    while (qty > 0) {
      const Level* best = opposite(side).best();
      if (!best || (type != OrderType::market && !crosses(side, price, best->price))) break;
      Order* maker = best->head;
      const std::uint32_t fill = std::min(qty, maker->qty);
      sink(ExecEvent{EventType::trade, RejectReason::none, id, maker->id, best->price, fill});
      qty -= fill;
      if (fill == maker->qty) {
        remove(maker);
      } else {
        book_.reduce(maker, fill);
      }
    }
    if (qty == 0) return;
    if (type == OrderType::limit) {
      ids_.insert(id, book_.add(id, side, price, qty, 0));
    } else {
      sink(ExecEvent{EventType::cancelled, RejectReason::none, id, 0, price, qty});
    }
  }

  void remove(Order* order) noexcept {
    ids_.erase(order->id);
    book_.remove(order);
  }

  template <class Sink>
  static void reject(std::uint64_t id, RejectReason reason, Sink& sink) {
    sink(ExecEvent{EventType::rejected, reason, id, 0, 0, 0});
  }

  Pool<Order> orders_;
  Pool<Level> levels_;
  OrderMap<Order> ids_;
  OrderBook book_;
};

}  // namespace me
