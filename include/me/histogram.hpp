#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace me {

// Log-linear latency histogram in the style of HdrHistogram. Values below 2^P are
// counted exactly; above that, each power of two is split into 2^P equal
// sub-buckets, so any recorded value is reported within a relative error of
// 2^-P (1.6% for P = 6). Storage is a fixed array, so record() never allocates.
class Histogram {
 public:
  static constexpr int precision_bits = 6;
  static constexpr std::uint64_t sub_buckets = 1ULL << precision_bits;
  static constexpr std::size_t bucket_count = (65 - precision_bits) * sub_buckets;

  void record(std::uint64_t value) noexcept {
    ++counts_[index(value)];
    ++count_;
    sum_ += value;
    min_ = std::min(min_, value);
    max_ = std::max(max_, value);
  }

  // Smallest bucket upper bound at or below which at least q of the samples lie
  // (q in [0, 1]); exact for values below 2^P, and never above max().
  std::uint64_t percentile(double q) const noexcept {
    if (count_ == 0) return 0;
    const auto rank = static_cast<std::uint64_t>(q * static_cast<double>(count_) + 0.5);
    const std::uint64_t target = std::clamp<std::uint64_t>(rank, 1, count_);
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < bucket_count; ++i) {
      seen += counts_[i];
      if (seen >= target) return std::min(upper(i), max_);
    }
    return max_;
  }

  void merge(const Histogram& other) noexcept {
    for (std::size_t i = 0; i < bucket_count; ++i) counts_[i] += other.counts_[i];
    count_ += other.count_;
    sum_ += other.sum_;
    min_ = std::min(min_, other.min_);
    max_ = std::max(max_, other.max_);
  }
  void reset() noexcept { *this = Histogram{}; }

  std::uint64_t count() const noexcept { return count_; }
  std::uint64_t min() const noexcept { return count_ ? min_ : 0; }
  std::uint64_t max() const noexcept { return max_; }
  double mean() const noexcept {
    return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0;
  }
  // Non-empty buckets as (upper bound, count) for plotting a CDF.
  template <class Visit>
  void for_each_bucket(Visit&& visit) const {
    for (std::size_t i = 0; i < bucket_count; ++i) {
      if (counts_[i]) visit(upper(i), counts_[i]);
    }
  }

  static std::size_t index(std::uint64_t value) noexcept {
    if (value < sub_buckets) return static_cast<std::size_t>(value);
    const int msb = 63 - std::countl_zero(value);
    const int shift = msb - precision_bits;
    const std::uint64_t low = (value >> shift) - sub_buckets;
    return static_cast<std::size_t>((static_cast<std::uint64_t>(shift) + 1) * sub_buckets + low);
  }
  // Largest value that maps to bucket i.
  static std::uint64_t upper(std::size_t i) noexcept {
    if (i < sub_buckets) return i;
    const auto shift = static_cast<int>(i / sub_buckets) - 1;
    const std::uint64_t low = i % sub_buckets;
    const std::uint64_t lower = (sub_buckets + low) << shift;
    return lower + ((1ULL << shift) - 1);
  }

 private:
  std::array<std::uint64_t, bucket_count> counts_{};
  std::uint64_t count_ = 0;
  std::uint64_t sum_ = 0;
  std::uint64_t min_ = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t max_ = 0;
};

}  // namespace me
