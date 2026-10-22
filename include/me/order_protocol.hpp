#pragma once

// Plain-text order/event protocol shared by the match_orders tool and the Python
// reference matcher, one record per line:
//
//   input:  S                       start a new stream (empty book)
//           N <id> <B|S> <price> <qty> <L|M|I>   new limit / market / IOC order
//           C <id>                  cancel
//           M <id> <price> <qty>    modify to a new price and open quantity
//   output: S                       stream start, echoed
//           A <id> <price> <qty>    ack (echoes the request's price and quantity)
//           T <taker> <maker> <price> <qty>
//           X <id> <price> <qty>    cancelled quantity
//           R <id> <reason>

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "me/matching_engine.hpp"

namespace me::protocol {

inline const char* reason_name(RejectReason reason) noexcept {
  switch (reason) {
    case RejectReason::none:
      return "none";
    case RejectReason::invalid_quantity:
      return "invalid_quantity";
    case RejectReason::invalid_price:
      return "invalid_price";
    case RejectReason::duplicate_id:
      return "duplicate_id";
    case RejectReason::unknown_order:
      return "unknown_order";
    case RejectReason::no_liquidity:
      return "no_liquidity";
  }
  return "unknown";
}

inline std::string format(const ExecEvent& e) {
  const auto n = [](std::uint64_t v) { return std::to_string(v); };
  switch (e.type) {
    case EventType::ack:
      return "A " + n(e.id) + ' ' + n(e.price) + ' ' + n(e.qty);
    case EventType::trade:
      return "T " + n(e.id) + ' ' + n(e.maker_id) + ' ' + n(e.price) + ' ' + n(e.qty);
    case EventType::cancelled:
      return "X " + n(e.id) + ' ' + n(e.price) + ' ' + n(e.qty);
    case EventType::rejected:
      return std::string("R ") + n(e.id) + ' ' + reason_name(e.reason);
  }
  return "?";
}

// Splits off the next space-separated field.
inline std::string_view field(std::string_view& rest) {
  const auto start = rest.find_first_not_of(' ');
  if (start == std::string_view::npos) return rest = {};
  rest.remove_prefix(start);
  const auto end = rest.find(' ');
  const auto token = rest.substr(0, end);
  rest.remove_prefix(end == std::string_view::npos ? rest.size() : end);
  return token;
}

template <class T>
std::optional<T> number(std::string_view text) {
  T value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return std::nullopt;
  return value;
}

}  // namespace me::protocol
