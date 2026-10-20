#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "me/itch_parser.hpp"

namespace {
std::uint64_t seed = 0x123456789abcdef0ULL;

template <class T>
struct Capture {
  static_assert(std::is_trivial_v<T> && std::is_standard_layout_v<T>);
  std::optional<T> value;
  void on(const T& message) {
    EXPECT_FALSE(value.has_value());
    value = message;
  }
  template <class Other>
  void on(const Other&) {
    FAIL() << "Wrong callback";
  }
  void on_unknown(unsigned char, std::span<const std::byte>) { FAIL() << "Unexpected unknown"; }
};

struct Counter {
  std::uint64_t known{}, unknown{};
  template <class T>
  void on(const T&) {
    ++known;
  }
  void on_unknown(unsigned char, std::span<const std::byte>) { ++unknown; }
};

#include "parser_cases.inc"

TEST(Parser, UnknownTypesAndMaximumLength) {
  for (unsigned int type = 0; type < 256; ++type) {
    if (me::itch::detail::known_length(static_cast<unsigned char>(type))) continue;
    for (std::size_t length : {1U, 65535U}) {
      std::vector<std::byte> bytes(length + 2, std::byte{0});
      bytes[0] = static_cast<std::byte>(length >> 8);
      bytes[1] = static_cast<std::byte>(length & 255);
      bytes[2] = static_cast<std::byte>(type);
      Counter handler;
      me::itch::Stats stats;
      me::itch::parse(bytes, handler, &stats);
      EXPECT_EQ(stats.skipped[type], 1U);
      EXPECT_EQ(handler.unknown, 1U);
      EXPECT_EQ(handler.known, 0U);
    }
  }
}

TEST(Parser, EmptyTerminatorAndTrailingData) {
  Counter handler;
  me::itch::Stats stats;
  me::itch::parse({}, handler, &stats);
  EXPECT_EQ(stats.bytes, 0U);
  const std::array bytes{std::byte{0}, std::byte{0}, std::byte{1}};
  me::itch::parse(std::span(bytes).first(2), handler, &stats);
  EXPECT_TRUE(stats.terminated);
  EXPECT_EQ(stats.bytes, 2U);
  try {
    me::itch::parse(bytes, handler);
    FAIL();
  } catch (const me::itch::ParseError& error) {
    EXPECT_EQ(error.code, me::itch::Error::trailing_data);
    EXPECT_EQ(error.offset, 0U);
  }
}

TEST(Parser, RejectsKnownLengthMismatchAndPreservesPrefixStats) {
  for (unsigned char kind : std::string("SRAFECXDUP")) {
    for (int adjustment : {-1, 1}) {
      const auto length = static_cast<std::size_t>(
          static_cast<int>(me::itch::detail::known_length(kind)) + adjustment);
      std::vector<std::byte> bytes(length + 5);
      bytes[1] = std::byte{1};
      bytes[2] = std::byte{'?'};
      bytes[4] = static_cast<std::byte>(length);
      bytes[5] = static_cast<std::byte>(kind);
      Counter handler;
      me::itch::Stats stats;
      try {
        me::itch::parse(bytes, handler, &stats);
        FAIL();
      } catch (const me::itch::ParseError& error) {
        EXPECT_EQ(error.offset, 3U);
        EXPECT_EQ(error.code, me::itch::Error::invalid_length);
        EXPECT_EQ(stats.messages, 1U);
        EXPECT_EQ(stats.bytes, 3U);
        EXPECT_EQ(handler.unknown, 1U);
        EXPECT_EQ(handler.known, 0U);
      }
    }
  }
}

TEST(Parser, RandomBytesExactAllocationAndUnalignedInput) {
  std::mt19937_64 random(seed);
  SCOPED_TRACE("seed=" + std::to_string(seed));
  for (std::size_t trial = 0; trial < 10000; ++trial) {
    SCOPED_TRACE(trial);
    const auto size = static_cast<std::size_t>(random() % 2048);
    // End exactly at the allocation boundary so ASan catches any lookahead.
    auto storage = std::make_unique<std::byte[]>(size + 1);
    for (std::size_t i = 0; i < size + 1; ++i) storage[i] = static_cast<std::byte>(random() & 255);
    const std::span<const std::byte> bytes(storage.get() + 1, size);
    Counter handler;
    me::itch::Stats stats;
    try {
      me::itch::parse(bytes, handler, &stats);
      EXPECT_EQ(stats.bytes, size);
    } catch (const me::itch::ParseError& error) {
      EXPECT_LE(error.offset, size);
    }
    EXPECT_EQ(stats.messages, handler.known + handler.unknown);
    EXPECT_LE(stats.bytes, size);
  }
}
}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg.starts_with("--seed=")) {
      const auto value = arg.substr(7);
      const auto result = std::from_chars(value.data(), value.data() + value.size(), seed);
      if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return 2;
      for (int j = i; j < argc - 1; ++j) argv[j] = argv[j + 1];
      --argc;
      --i;
    }
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
