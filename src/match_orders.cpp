// Reads order streams in the text protocol from stdin (see me/order_protocol.hpp),
// runs each through the matching engine, and writes the events to stdout. One
// process handles any number of streams, so the differential test launches once.

#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#include "me/matching_engine.hpp"
#include "me/order_protocol.hpp"
#include "platform/main_trace.hpp"

namespace {
int run() {
  namespace p = me::protocol;
  me::MatchingEngine engine;
  std::string line, out;
  std::size_t line_number = 0;
  auto sink = [&](const me::ExecEvent& event) {
    out += p::format(event);
    out += '\n';
  };
  auto fail = [&] {
    std::cerr << "match_orders: bad input at line " << line_number << ": " << line << '\n';
    return 2;
  };
  while (std::getline(std::cin, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string_view rest(line);
    const auto kind = p::field(rest);
    if (kind.empty()) continue;
    if (kind == "S") {
      engine.reset();
      out += "S\n";
    } else if (kind == "N") {
      const auto id = p::number<std::uint64_t>(p::field(rest));
      const auto side = p::field(rest);
      const auto price = p::number<std::uint32_t>(p::field(rest));
      const auto qty = p::number<std::uint32_t>(p::field(rest));
      const auto type = p::field(rest);
      if (!id || !price || !qty || (side != "B" && side != "S") ||
          (type != "L" && type != "M" && type != "I")) {
        return fail();
      }
      const auto order_type = type == "L"   ? me::OrderType::limit
                              : type == "M" ? me::OrderType::market
                                            : me::OrderType::ioc;
      engine.submit(
          me::NewOrder{*id, side == "B" ? me::Side::buy : me::Side::sell, *price, *qty, order_type},
          sink);
    } else if (kind == "C") {
      const auto id = p::number<std::uint64_t>(p::field(rest));
      if (!id) return fail();
      engine.submit(me::CancelOrder{*id}, sink);
    } else if (kind == "M") {
      const auto id = p::number<std::uint64_t>(p::field(rest));
      const auto price = p::number<std::uint32_t>(p::field(rest));
      const auto qty = p::number<std::uint32_t>(p::field(rest));
      if (!id || !price || !qty) return fail();
      engine.submit(me::ModifyOrder{*id, *price, *qty}, sink);
    } else {
      return fail();
    }
    if (out.size() > (1 << 16)) {
      std::cout << out;
      out.clear();
    }
  }
  std::cout << out << std::flush;
  return std::cout ? 0 : 1;
}
}  // namespace

int main() {
  return me::platform::traced_main("match_orders", [] {
    try {
      return run();
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  });
}
