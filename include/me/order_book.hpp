#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace me {

enum class Side : std::uint8_t { buy, sell };

struct Level;

struct Order {
  std::uint64_t id;
  std::uint32_t price;  // ITCH units of 0.0001; never converted to floating point
  std::uint32_t qty;
  Side side;
  std::uint16_t locate;  // owning book, so references can be checked against the message
  Order* prev;
  Order* next;
  Level* level;
};

// One price level: an intrusive FIFO of resting orders plus their total size.
struct Level {
  std::uint32_t price;
  std::uint32_t count;
  std::uint64_t total;
  Order* head;
  Order* tail;
};

struct LevelView {
  std::uint32_t price;
  std::uint32_t count;
  std::uint64_t total;
  friend bool operator==(const LevelView&, const LevelView&) = default;
};

// Fixed-address object pool. Objects are carved from 64K-element chunks and
// recycled through a free list, so steady-state allocate/release never touches
// the heap. A new chunk is allocated only when every object is live; reserve()
// moves that cost out of the hot path.
template <class T>
class Pool {
 public:
  static constexpr std::size_t chunk_size = 1 << 16;

  T* allocate() {
    if (free_.empty()) grow();
    T* object = free_.back();
    free_.pop_back();
    return object;
  }
  void release(T* object) noexcept { free_.push_back(object); }  // capacity reserved in grow()
  void reserve(std::size_t objects) {
    while (chunks_.size() * chunk_size < objects) grow();
  }
  std::size_t capacity() const noexcept { return chunks_.size() * chunk_size; }
  std::size_t live() const noexcept { return capacity() - free_.size(); }

 private:
  void grow() {
    chunks_.push_back(std::make_unique<T[]>(chunk_size));
    free_.reserve(capacity());
    T* chunk = chunks_.back().get();
    for (std::size_t i = chunk_size; i-- > 0;) free_.push_back(chunk + i);
  }

  std::vector<std::unique_ptr<T[]>> chunks_;
  std::vector<T*> free_;
};

// Levels of one side, sorted worst to best so the best price is at the back.
// Most ITCH activity is near the touch, so searches start at the back and an
// insert or erase near the top moves only a few pointers. A level far from the
// touch costs O(distance), which is the trade-off against a tree.
class BookSide {
 public:
  explicit BookSide(Side side) : side_(side) { levels_.reserve(256); }

  // a is strictly better than b for this side.
  bool better(std::uint32_t a, std::uint32_t b) const noexcept {
    return side_ == Side::buy ? a > b : a < b;
  }

  Level* find_or_insert(std::uint32_t price, Pool<Level>& pool) {
    std::size_t i = levels_.size();
    while (i > 0 && better(levels_[i - 1]->price, price)) --i;
    if (i > 0 && levels_[i - 1]->price == price) return levels_[i - 1];
    Level* level = pool.allocate();
    *level = Level{price, 0, 0, nullptr, nullptr};
    levels_.insert(levels_.begin() + static_cast<std::ptrdiff_t>(i), level);
    return level;
  }

  void erase(Level* level, Pool<Level>& pool) noexcept {
    std::size_t i = levels_.size();
    while (i > 0 && levels_[i - 1] != level) --i;
    // The level is always present; a miss would mean a corrupted book.
    levels_.erase(levels_.begin() + static_cast<std::ptrdiff_t>(i - 1));
    pool.release(level);
  }

  const Level* best() const noexcept { return levels_.empty() ? nullptr : levels_.back(); }
  std::size_t size() const noexcept { return levels_.size(); }
  Side side() const noexcept { return side_; }

  // Writes up to out.size() levels, best first; returns how many were written.
  std::size_t depth(std::span<LevelView> out) const noexcept {
    std::size_t n = 0;
    for (std::size_t i = levels_.size(); i > 0 && n < out.size(); --i, ++n) {
      const Level* level = levels_[i - 1];
      out[n] = {level->price, level->count, level->total};
    }
    return n;
  }

  // Levels in worst-to-best order, for invariant checks and full snapshots.
  std::span<Level* const> levels() const noexcept { return levels_; }

  void clear(Pool<Level>& pool) noexcept {
    for (Level* level : levels_) pool.release(level);
    levels_.clear();
  }

 private:
  Side side_;
  std::vector<Level*> levels_;
};

// One symbol's limit order book. Orders and levels come from pools shared by
// all books; the id map lives with the caller because ITCH ids are unique
// across the whole day, not per symbol.
//   add: O(1) at an existing level, O(distance from touch) for a new level
//   reduce/remove: O(1), plus O(distance from touch) when a level empties
//   best bid/offer: O(1)
class OrderBook {
 public:
  OrderBook(Pool<Order>& orders, Pool<Level>& levels)
      : orders_(&orders), levels_(&levels), bids_(Side::buy), asks_(Side::sell) {}
  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;

  Order* add(std::uint64_t id, Side side, std::uint32_t price, std::uint32_t qty,
             std::uint16_t locate) {
    Order* order = orders_->allocate();
    Level* level = side_of(side).find_or_insert(price, *levels_);
    *order = Order{id, price, qty, side, locate, level->tail, nullptr, level};
    if (level->tail) {
      level->tail->next = order;
    } else {
      level->head = order;
    }
    level->tail = order;
    ++level->count;
    level->total += qty;
    return order;
  }

  // Removes qty shares; the order is unlinked and released when none remain.
  // Returns true if the order was removed. qty must not exceed order->qty.
  bool reduce(Order* order, std::uint32_t qty) noexcept {
    if (qty >= order->qty) {
      remove(order);
      return true;
    }
    order->qty -= qty;
    order->level->total -= qty;
    return false;
  }

  void remove(Order* order) noexcept {
    Level* level = order->level;
    (order->prev ? order->prev->next : level->head) = order->next;
    (order->next ? order->next->prev : level->tail) = order->prev;
    --level->count;
    level->total -= order->qty;
    if (level->count == 0) side_of(order->side).erase(level, *levels_);
    orders_->release(order);
  }

  const BookSide& bids() const noexcept { return bids_; }
  const BookSide& asks() const noexcept { return asks_; }
  const BookSide& side(Side side) const noexcept { return side == Side::buy ? bids_ : asks_; }

  // Both sides present and best bid >= best ask (a locked book counts as crossed
  // here; callers that care distinguish equality).
  bool crossed() const noexcept {
    const Level* bid = bids_.best();
    const Level* ask = asks_.best();
    return bid && ask && bid->price >= ask->price;
  }

  // Releases every order and level back to the pools; capacity is kept.
  void clear() noexcept {
    for (BookSide* side : {&bids_, &asks_}) {
      for (Level* level : side->levels()) {
        for (Order* order = level->head; order;) {
          Order* next = order->next;
          orders_->release(order);
          order = next;
        }
      }
      side->clear(*levels_);
    }
  }

  // Full structural check: sorted strictly by price, no empty levels, counts and
  // totals equal the sum of their orders, links consistent, no zero-qty orders.
  bool valid() const noexcept {
    for (const BookSide* side : {&bids_, &asks_}) {
      const auto levels = side->levels();
      for (std::size_t i = 0; i < levels.size(); ++i) {
        if (i > 0 && !side->better(levels[i]->price, levels[i - 1]->price)) return false;
        if (!level_valid(*levels[i], side->side())) return false;
      }
    }
    return true;
  }

  static bool level_valid(const Level& level, Side side) noexcept {
    if (level.count == 0 || level.head == nullptr || level.head->prev != nullptr) return false;
    std::uint64_t total = 0;
    std::uint32_t count = 0;
    const Order* last = nullptr;
    for (const Order* order = level.head; order; last = order, order = order->next) {
      if (order->qty == 0 || order->level != &level || order->price != level.price ||
          order->side != side || order->prev != last) {
        return false;
      }
      total += order->qty;
      ++count;
    }
    return last == level.tail && count == level.count && total == level.total;
  }

 private:
  BookSide& side_of(Side side) noexcept { return side == Side::buy ? bids_ : asks_; }

  Pool<Order>* orders_;
  Pool<Level>* levels_;
  BookSide bids_;
  BookSide asks_;
};

}  // namespace me
