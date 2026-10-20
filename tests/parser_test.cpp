#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "me/itch_parser.hpp"
#include "platform/mapped_file.hpp"

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

struct Corpus {
  std::vector<std::byte> data;
  std::vector<std::size_t> boundaries{0};
  explicit Corpus(const std::filesystem::path& path) {
    const me::platform::MappedFile file(path);
    data.assign(file.bytes().begin(), file.bytes().end());
    std::size_t offset = 0;
    while (offset < data.size()) {
      if (data.size() - offset < 2) throw std::runtime_error("truncated seed prefix");
      const auto length = (std::to_integer<unsigned int>(data[offset]) << 8) |
                          std::to_integer<unsigned int>(data[offset + 1]);
      if (length == 0) {
        if (offset + 2 != data.size()) throw std::runtime_error("nonfinal seed terminator");
        data.resize(offset);
        break;
      }
      if (length > data.size() - offset - 2) throw std::runtime_error("truncated seed payload");
      offset += length + 2;
      boundaries.push_back(offset);
    }
  }
};

TEST(Parser, MutatedCorpusExactAllocationAndUnalignedInput) {
  std::mt19937_64 random(seed);
  SCOPED_TRACE("seed=" + std::to_string(seed));
  const std::filesystem::path fixtures(ME_FIXTURE_DIR);
  const Corpus synthetic(fixtures / "synthetic.itch");
  const Corpus real(fixtures / "01302019.AAPL-MSFT-SPY.itch");
  ASSERT_GT(synthetic.boundaries.size(), 1U);
  ASSERT_GT(real.boundaries.size(), 65U);
  std::array<std::uint64_t, 256> decoded{};
  std::array<std::size_t, 6> mutations{};
  std::array<std::size_t, 5> length_mutations{};
  std::size_t trials_with_frames = 0;
  for (std::size_t trial = 0; trial < 10000; ++trial) {
    SCOPED_TRACE(trial);
    // Alternate synthetic-only, real-only, and concatenated seed streams. All
    // mutation locations include the first frame; no prefix is protected.
    std::vector<std::byte> mutated;
    if (trial % 4 != 1) mutated = synthetic.data;
    if (trial % 4 != 0) {
      const auto count = static_cast<std::size_t>(8 + random() % 57);
      const auto first = static_cast<std::size_t>(random() % (real.boundaries.size() - count));
      mutated.insert(mutated.end(), real.data.begin() + real.boundaries[first],
                     real.data.begin() + real.boundaries[first + count]);
    }
    std::vector<std::size_t> frames;
    for (std::size_t offset = 0; offset < mutated.size();) {
      frames.push_back(offset);
      const auto length = (std::to_integer<unsigned int>(mutated[offset]) << 8) |
                          std::to_integer<unsigned int>(mutated[offset + 1]);
      offset += length + 2;
    }
    const auto frame = frames[random() % frames.size()];
    const auto position = static_cast<std::size_t>(random() % mutated.size());
    const auto mutation = trial % mutations.size();
    ++mutations[mutation];
    switch (mutation) {
      case 0:
        for (std::size_t flip = 0, n = 1 + random() % 4; flip < n; ++flip) {
          mutated[random() % mutated.size()] ^= static_cast<std::byte>(1U << (random() % 8));
        }
        break;
      case 1: {
        const auto original = (std::to_integer<unsigned int>(mutated[frame]) << 8) |
                              std::to_integer<unsigned int>(mutated[frame + 1]);
        const auto variant = (trial / mutations.size()) % length_mutations.size();
        ++length_mutations[variant];
        const std::array<unsigned int, 5> replacements{original - 1, original + 1, 0, 65535,
                                                       static_cast<unsigned int>(random() & 65535)};
        auto length = replacements[variant];
        if (length == original) length ^= 1;
        mutated[frame] = static_cast<std::byte>((length >> 8) & 255);
        mutated[frame + 1] = static_cast<std::byte>(length & 255);
        break;
      }
      case 2:
        mutated[frame + 2] ^= static_cast<std::byte>(1 + random() % 255);
        break;
      case 3:
        mutated.resize(position);
        break;
      case 4: {
        std::vector<std::byte> inserted(1 + random() % 8);
        for (auto& byte : inserted) byte = static_cast<std::byte>(random() & 255);
        mutated.insert(mutated.begin() + position, inserted.begin(), inserted.end());
        break;
      }
      case 5: {
        const auto count = std::min<std::size_t>(1 + random() % 8, mutated.size() - position);
        mutated.erase(mutated.begin() + position, mutated.begin() + position + count);
        break;
      }
    }
    const auto size = mutated.size();
    // End exactly at the allocation boundary so ASan catches any lookahead.
    auto storage = std::make_unique<std::byte[]>(size + 1);
    std::copy(mutated.begin(), mutated.end(), storage.get() + 1);
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
    trials_with_frames += stats.messages != 0;
    for (unsigned char kind : std::string("SRAFECXDUP")) decoded[kind] += stats.counts[kind];
  }
  std::cout << "seed=" << seed << " trials_with_frames=" << trials_with_frames << "/10000\n";
  EXPECT_GT(trials_with_frames, 7500U);
  for (unsigned char kind : std::string("SRAFECXDUP")) {
    std::cout << "decoded." << kind << '=' << decoded[kind] << '\n';
    EXPECT_GT(decoded[kind], 0U) << "type=" << kind;
  }
  for (auto count : mutations) EXPECT_GT(count, 0U);
  for (auto count : length_mutations) EXPECT_GT(count, 0U);
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
