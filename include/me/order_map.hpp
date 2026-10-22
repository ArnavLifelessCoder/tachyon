#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace me {

// Order id -> pointer map with open addressing over a power-of-two table.
// Linear probing keeps lookups near the home slot; erase uses backward-shift
// deletion, so no tombstones accumulate over a trading day. A null value marks
// an empty slot, so every id (including 0) is a valid key.
// The table grows when it reaches half full. Call reserve() up front to keep
// insert allocation-free on the hot path.
template <class T>
class OrderMap {
 public:
  explicit OrderMap(std::size_t expected = 1024) { rehash(capacity_for(expected)); }

  T* find(std::uint64_t key) const noexcept {
    for (std::size_t i = home(key);; i = (i + 1) & mask_) {
      const Slot& slot = slots_[i];
      if (slot.value == nullptr) return nullptr;
      if (slot.key == key) return slot.value;
    }
  }

  // Returns false (and changes nothing) if key is already present.
  bool insert(std::uint64_t key, T* value) {
    if (2 * (size_ + 1) > capacity()) rehash(capacity() * 2);
    for (std::size_t i = home(key);; i = (i + 1) & mask_) {
      Slot& slot = slots_[i];
      if (slot.value == nullptr) {
        slot = {key, value};
        ++size_;
        return true;
      }
      if (slot.key == key) return false;
    }
  }

  // Returns the removed value, or nullptr if key was absent.
  T* erase(std::uint64_t key) noexcept {
    std::size_t i = home(key);
    for (;; i = (i + 1) & mask_) {
      if (slots_[i].value == nullptr) return nullptr;
      if (slots_[i].key == key) break;
    }
    T* removed = slots_[i].value;
    // Shift later entries of the probe run back so every remaining key stays
    // reachable from its home slot without tombstones.
    for (std::size_t j = (i + 1) & mask_; slots_[j].value != nullptr; j = (j + 1) & mask_) {
      const std::size_t k = home(slots_[j].key);
      if (((j - k) & mask_) >= ((j - i) & mask_)) {
        slots_[i] = slots_[j];
        i = j;
      }
    }
    slots_[i] = {};
    --size_;
    return removed;
  }

  void reserve(std::size_t expected) {
    const std::size_t wanted = capacity_for(expected);
    if (wanted > capacity()) rehash(wanted);
  }
  void clear() noexcept {
    for (std::size_t i = 0; i < capacity(); ++i) slots_[i] = {};
    size_ = 0;
  }
  std::size_t size() const noexcept { return size_; }
  std::size_t capacity() const noexcept { return mask_ + 1; }

 private:
  struct Slot {
    std::uint64_t key = 0;
    T* value = nullptr;
  };

  static std::size_t capacity_for(std::size_t expected) {
    return std::bit_ceil(expected < 8 ? std::size_t{16} : expected * 2);
  }
  std::size_t home(std::uint64_t key) const noexcept {
    // Fibonacci hashing: the multiply spreads sequential ITCH ids across the table.
    return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ULL) >> shift_);
  }
  void rehash(std::size_t capacity) {
    const std::size_t previous = slots_ ? mask_ + 1 : 0;
    auto old = std::move(slots_);
    slots_ = std::make_unique<Slot[]>(capacity);
    mask_ = capacity - 1;
    shift_ = 64 - std::countr_zero(capacity);
    size_ = 0;
    for (std::size_t i = 0; i < previous; ++i) {
      if (old[i].value != nullptr) insert(old[i].key, old[i].value);
    }
  }

  std::unique_ptr<Slot[]> slots_;
  std::size_t mask_ = 0;
  int shift_ = 64;
  std::size_t size_ = 0;
};

}  // namespace me
