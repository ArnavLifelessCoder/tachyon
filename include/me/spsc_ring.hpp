#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace me {

// 64 bytes on every x86-64 part we target. std::hardware_destructive_interference_size
// is not used because GCC warns (-Winterference-size, fatal under -Werror) that its
// value depends on -mtune and so is not ABI-stable across translation units.
inline constexpr std::size_t cache_line = 64;

// Lock-free single-producer/single-consumer ring of trivially copyable items.
//
// Indices are free-running 64-bit counters (they never wrap in practice) masked by
// a power-of-two capacity. head_ is written only by the consumer and tail_ only by
// the producer; each sits on its own cache line, and each side keeps a private copy
// of the other side's index, refreshed only when the ring looks full or empty, so
// the common case touches no shared cache line except to publish its own index.
//
// Memory ordering:
//  - Producer writes the slot, then tail_.store(release). The consumer's
//    tail_.load(acquire) that observes the new tail therefore also observes the slot
//    contents (release/acquire pair on tail_).
//  - Consumer reads the slot, then head_.store(release). The producer's
//    head_.load(acquire) that observes the new head is ordered after that read, so
//    the producer never overwrites a slot that is still being read (release/acquire
//    pair on head_).
//  - Each side reads its own index with relaxed ordering: only that thread writes it.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // padding from alignas is the point: no false sharing
#endif
template <class T>
class SpscRing {
  static_assert(std::is_trivially_copyable_v<T>, "items are copied with plain stores");

 public:
  explicit SpscRing(std::size_t capacity)
      : mask_(capacity - 1), slots_(std::make_unique<T[]>(capacity)) {
    if (capacity < 2 || !std::has_single_bit(capacity)) {
      throw std::invalid_argument("SpscRing capacity must be a power of two >= 2");
    }
  }
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // Producer only. Returns false when full.
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

  // Consumer only. Returns false when empty.
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

  std::size_t capacity() const noexcept { return mask_ + 1; }

 private:
  // Producer-owned line: its index and its view of the consumer.
  alignas(cache_line) std::atomic<std::size_t> tail_{0};
  std::size_t cached_head_ = 0;
  // Consumer-owned line.
  alignas(cache_line) std::atomic<std::size_t> head_{0};
  std::size_t cached_tail_ = 0;
  // Read-only after construction; its own line so it is never invalidated.
  alignas(cache_line) const std::size_t mask_;
  std::unique_ptr<T[]> slots_;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace me
