#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <span>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace me::itch {

struct Header {
  std::uint16_t stock_locate;
  std::uint16_t tracking_number;
  std::uint64_t timestamp;
};
struct SystemEvent {
  Header header;
  char event_code;
};
struct Directory {
  Header header;
  std::array<char, 8> stock;
  char market_category, financial_status;
  std::uint32_t round_lot_size;
  char round_lots_only, issue_classification;
  std::array<char, 2> issue_subtype;
  char authenticity, short_sale_threshold, ipo_flag, luld_tier, etp_flag;
  std::uint32_t etp_leverage_factor;
  char inverse_indicator;
};
struct Add {
  Header header;
  std::uint64_t order_id;
  char side;
  std::uint32_t shares;
  std::array<char, 8> stock;
  std::uint32_t price;
};
struct AddMpid {
  Header header;
  std::uint64_t order_id;
  char side;
  std::uint32_t shares;
  std::array<char, 8> stock;
  std::uint32_t price;
  std::array<char, 4> attribution;
};
struct Execute {
  Header header;
  std::uint64_t order_id;
  std::uint32_t shares;
  std::uint64_t match_number;
};
struct ExecutePrice {
  Header header;
  std::uint64_t order_id;
  std::uint32_t shares;
  std::uint64_t match_number;
  char printable;
  std::uint32_t price;
};
struct Cancel {
  Header header;
  std::uint64_t order_id;
  std::uint32_t shares;
};
struct Delete {
  Header header;
  std::uint64_t order_id;
};
struct Replace {
  Header header;
  std::uint64_t order_id, new_order_id;
  std::uint32_t shares, price;
};
struct Trade {
  Header header;
  std::uint64_t order_id;
  char side;
  std::uint32_t shares;
  std::array<char, 8> stock;
  std::uint32_t price;
  std::uint64_t match_number;
};

enum class Error { truncated_prefix, truncated_payload, invalid_length, trailing_data };
class ParseError final : public std::exception {
 public:
  ParseError(Error code, std::size_t offset) noexcept : code(code), offset(offset) {}
  const char* what() const noexcept override {
    switch (code) {
      case Error::truncated_prefix:
        return "truncated_prefix";
      case Error::truncated_payload:
        return "truncated_payload";
      case Error::invalid_length:
        return "invalid_length";
      case Error::trailing_data:
        return "trailing_data";
    }
    return "unknown_error";
  }
  Error code;
  std::size_t offset;
};

struct Stats {
  std::array<std::uint64_t, 256> counts{};
  std::array<std::uint64_t, 256> skipped{};
  std::uint64_t messages{};
  std::size_t bytes{};
  bool terminated{};
};

namespace detail {
template <class T>
T be(const std::byte* p) noexcept {
  static_assert(sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);
  static_assert(std::endian::native == std::endian::little ||
                std::endian::native == std::endian::big);
  T value;
  std::memcpy(&value, p, sizeof(T));
  if constexpr (std::endian::native == std::endian::little) {
#if defined(_MSC_VER)
    if constexpr (sizeof(T) == 2) return _byteswap_ushort(value);
    if constexpr (sizeof(T) == 4) return _byteswap_ulong(value);
    if constexpr (sizeof(T) == 8) return _byteswap_uint64(value);
#else
    if constexpr (sizeof(T) == 2) return __builtin_bswap16(value);
    if constexpr (sizeof(T) == 4) return __builtin_bswap32(value);
    if constexpr (sizeof(T) == 8) return __builtin_bswap64(value);
#endif
  } else {
    return value;
  }
}
inline std::uint16_t u16(const std::byte* p) noexcept { return be<std::uint16_t>(p); }
inline std::uint32_t u32(const std::byte* p) noexcept { return be<std::uint32_t>(p); }
inline std::uint64_t u64(const std::byte* p) noexcept { return be<std::uint64_t>(p); }
inline char ch(const std::byte* p) noexcept { return static_cast<char>(*p); }
template <std::size_t N>
std::array<char, N> text(const std::byte* p) noexcept {
  std::array<char, N> out;
  std::memcpy(out.data(), p, N);
  return out;
}
inline Header header(const std::byte* p) noexcept {
  return {u16(p + 1), u16(p + 3), (static_cast<std::uint64_t>(u16(p + 5)) << 32) | u32(p + 7)};
}
constexpr std::size_t known_length(unsigned char kind) noexcept {
  switch (kind) {
    case 'S':
      return 12;
    case 'R':
      return 39;
    case 'A':
      return 36;
    case 'F':
      return 40;
    case 'E':
      return 31;
    case 'C':
      return 36;
    case 'X':
      return 23;
    case 'D':
      return 19;
    case 'U':
      return 35;
    case 'P':
      return 44;
    default:
      return 0;
  }
}
}  // namespace detail

// Handlers provide on(const T&) for each POD and on_unknown(type, payload).
// No input ownership, heap allocation, packed/unaligned loads, or virtual dispatch.
// Stats describe this invocation; on error they contain only completed callbacks.
template <class Handler>
void parse(std::span<const std::byte> input, Handler& handler, Stats* stats = nullptr) {
  using namespace detail;
  if (stats) *stats = {};
  std::size_t offset = 0;
  while (offset < input.size()) {
    if (input.size() - offset < 2) throw ParseError(Error::truncated_prefix, offset);
    const auto length = u16(input.data() + offset);
    if (length == 0) {
      if (input.size() - offset != 2) throw ParseError(Error::trailing_data, offset);
      if (stats) {
        stats->bytes = offset + 2;
        stats->terminated = true;
      }
      return;
    }
    if (length > input.size() - offset - 2) throw ParseError(Error::truncated_payload, offset);
    const auto* p = input.data() + offset + 2;
    const auto kind = std::to_integer<unsigned char>(*p);
    const auto expected = known_length(kind);
    if (expected != 0 && length != expected) throw ParseError(Error::invalid_length, offset);
    switch (kind) {
      case 'S':
        handler.on(SystemEvent{header(p), ch(p + 11)});
        break;
      case 'R':
        handler.on(Directory{header(p), text<8>(p + 11), ch(p + 19), ch(p + 20), u32(p + 21),
                             ch(p + 25), ch(p + 26), text<2>(p + 27), ch(p + 29), ch(p + 30),
                             ch(p + 31), ch(p + 32), ch(p + 33), u32(p + 34), ch(p + 38)});
        break;
      case 'A':
        handler.on(
            Add{header(p), u64(p + 11), ch(p + 19), u32(p + 20), text<8>(p + 24), u32(p + 32)});
        break;
      case 'F':
        handler.on(AddMpid{header(p), u64(p + 11), ch(p + 19), u32(p + 20), text<8>(p + 24),
                           u32(p + 32), text<4>(p + 36)});
        break;
      case 'E':
        handler.on(Execute{header(p), u64(p + 11), u32(p + 19), u64(p + 23)});
        break;
      case 'C':
        handler.on(ExecutePrice{header(p), u64(p + 11), u32(p + 19), u64(p + 23), ch(p + 31),
                                u32(p + 32)});
        break;
      case 'X':
        handler.on(Cancel{header(p), u64(p + 11), u32(p + 19)});
        break;
      case 'D':
        handler.on(Delete{header(p), u64(p + 11)});
        break;
      case 'U':
        handler.on(Replace{header(p), u64(p + 11), u64(p + 19), u32(p + 27), u32(p + 31)});
        break;
      case 'P':
        handler.on(Trade{header(p), u64(p + 11), ch(p + 19), u32(p + 20), text<8>(p + 24),
                         u32(p + 32), u64(p + 36)});
        break;
      default:
        handler.on_unknown(kind, std::span<const std::byte>(p, length));
        break;
    }
    offset += 2 + length;
    if (stats) {
      ++stats->messages;
      ++stats->counts[kind];
      if (expected == 0) ++stats->skipped[kind];
      stats->bytes = offset;
    }
  }
}
}  // namespace me::itch
