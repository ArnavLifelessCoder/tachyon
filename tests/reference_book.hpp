#pragma once

// Deliberately simple reference book for differential tests: std::map levels
// holding std::list FIFOs, and a std::unordered_map from id to list position.
// It shares no code with me::OrderBook and applies the same ITCH semantics.

#include <cstdint>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <span>
#include <unordered_map>
#include <vector>

#include "me/itch_parser.hpp"
#include "me/order_book.hpp"

namespace ref {

struct RefOrder {
  std::uint64_t id;
  std::uint32_t qty;
};

struct RefBook {
  std::map<std::uint32_t, std::list<RefOrder>, std::greater<>> bids;  // best (highest) first
  std::map<std::uint32_t, std::list<RefOrder>> asks;                  // best (lowest) first

  std::vector<me::LevelView> depth(me::Side side, std::size_t n) const {
    std::vector<me::LevelView> out;
    auto collect = [&](const auto& levels) {
      for (const auto& [price, orders] : levels) {
        if (out.size() == n) break;
        std::uint64_t total = 0;
        for (const auto& order : orders) total += order.qty;
        out.push_back({price, static_cast<std::uint32_t>(orders.size()), total});
      }
    };
    if (side == me::Side::buy) {
      collect(bids);
    } else {
      collect(asks);
    }
    return out;
  }
};

struct Where {
  std::uint16_t locate;
  me::Side side;
  std::uint32_t price;
  std::list<RefOrder>::iterator position;
};

class RefBuilder {
 public:
  std::uint64_t unknown_refs = 0;
  std::uint64_t duplicate_adds = 0;

  void on(const me::itch::SystemEvent&) {}
  void on(const me::itch::Directory&) {}
  void on(const me::itch::Add& m) {
    add(m.header.stock_locate, m.order_id, m.side, m.price, m.shares);
  }
  void on(const me::itch::AddMpid& m) {
    add(m.header.stock_locate, m.order_id, m.side, m.price, m.shares);
  }
  void on(const me::itch::Execute& m) { reduce(m.order_id, m.shares); }
  void on(const me::itch::ExecutePrice& m) { reduce(m.order_id, m.shares); }
  void on(const me::itch::Cancel& m) { reduce(m.order_id, m.shares); }
  void on(const me::itch::Delete& m) {
    auto it = orders_.find(m.order_id);
    if (it == orders_.end()) {
      ++unknown_refs;
      return;
    }
    erase(it);
  }
  void on(const me::itch::Replace& m) {
    auto it = orders_.find(m.order_id);
    if (it == orders_.end()) {
      ++unknown_refs;
      return;
    }
    const Where old = it->second;
    erase(it);
    add(old.locate, m.new_order_id, old.side == me::Side::buy ? 'B' : 'S', m.price, m.shares);
  }
  void on(const me::itch::Trade&) {}
  void on_unknown(unsigned char, std::span<const std::byte>) {}

  const RefBook& book(std::uint16_t locate) { return books_[locate]; }
  std::size_t live() const { return orders_.size(); }

 private:
  void add(std::uint16_t locate, std::uint64_t id, char side, std::uint32_t price,
           std::uint32_t shares) {
    if (orders_.count(id)) {
      ++duplicate_adds;
      return;
    }
    RefBook& book = books_[locate];
    const me::Side s = side == 'S' ? me::Side::sell : me::Side::buy;
    auto& fifo = s == me::Side::buy ? book.bids[price] : book.asks[price];
    fifo.push_back({id, shares});
    orders_[id] = {locate, s, price, std::prev(fifo.end())};
  }

  void reduce(std::uint64_t id, std::uint32_t shares) {
    auto it = orders_.find(id);
    if (it == orders_.end()) {
      ++unknown_refs;
      return;
    }
    if (shares >= it->second.position->qty) {
      erase(it);
    } else {
      it->second.position->qty -= shares;
    }
  }

  void erase(std::unordered_map<std::uint64_t, Where>::iterator it) {
    const Where w = it->second;
    RefBook& book = books_[w.locate];
    if (w.side == me::Side::buy) {
      auto level = book.bids.find(w.price);
      level->second.erase(w.position);
      if (level->second.empty()) book.bids.erase(level);
    } else {
      auto level = book.asks.find(w.price);
      level->second.erase(w.position);
      if (level->second.empty()) book.asks.erase(level);
    }
    orders_.erase(it);
  }

  std::unordered_map<std::uint16_t, RefBook> books_;
  std::unordered_map<std::uint64_t, Where> orders_;
};

}  // namespace ref
