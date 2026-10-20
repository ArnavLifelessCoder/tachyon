#include "parser_cli.hpp"

#include <array>
#include <concepts>
#include <iostream>
#include <sstream>
#include <string_view>

#include "me/itch_parser.hpp"
#include "platform/mapped_file.hpp"

namespace {
constexpr char hex[] = "0123456789abcdef";
void quote(std::ostream& out, std::string_view text) {
  out << '"';
  for (unsigned char c : text) {
    if (c < 32 || c >= 127 || c == '"' || c == '\\') {
      out << "\\u00" << hex[c >> 4] << hex[c & 15];
    } else {
      out << static_cast<char>(c);
    }
  }
  out << '"';
}
struct JsonHandler {
  bool enabled;
  bool first = true;
  std::ostringstream out;
  void begin(char type) {
    if (!first) out << ',';
    first = false;
    out << "{\"type\":";
    quote(out, {&type, 1});
  }
  void key(std::string_view name) {
    out << ',';
    quote(out, name);
    out << ':';
  }
  template <std::integral T>
  void field(std::string_view name, T value) {
    key(name);
    out << value;
  }
  void field(std::string_view name, char value) {
    key(name);
    quote(out, {&value, 1});
  }
  template <std::size_t N>
  void field(std::string_view name, const std::array<char, N>& value) {
    key(name);
    quote(out, {value.data(), N});
  }
  void begin(char type, const me::itch::Header& h) {
    begin(type);
    field("stock_locate", h.stock_locate);
    field("tracking_number", h.tracking_number);
    field("timestamp", h.timestamp);
  }
#define FIELD(name) field(#name, m.name)
#define START(type)     \
  if (!enabled) return; \
  begin(type, m.header)
  void on(const me::itch::SystemEvent& m) {
    START('S');
    FIELD(event_code);
    out << '}';
  }
  void on(const me::itch::Directory& m) {
    START('R');
    FIELD(stock);
    FIELD(market_category);
    FIELD(financial_status);
    FIELD(round_lot_size);
    FIELD(round_lots_only);
    FIELD(issue_classification);
    FIELD(issue_subtype);
    FIELD(authenticity);
    FIELD(short_sale_threshold);
    FIELD(ipo_flag);
    FIELD(luld_tier);
    FIELD(etp_flag);
    FIELD(etp_leverage_factor);
    FIELD(inverse_indicator);
    out << '}';
  }
  template <class T>
  void add_fields(const T& m) {
    FIELD(order_id);
    FIELD(side);
    FIELD(shares);
    FIELD(stock);
    FIELD(price);
  }
  void on(const me::itch::Add& m) {
    START('A');
    add_fields(m);
    out << '}';
  }
  void on(const me::itch::AddMpid& m) {
    START('F');
    add_fields(m);
    FIELD(attribution);
    out << '}';
  }
  void on(const me::itch::Execute& m) {
    START('E');
    FIELD(order_id);
    FIELD(shares);
    FIELD(match_number);
    out << '}';
  }
  void on(const me::itch::ExecutePrice& m) {
    START('C');
    FIELD(order_id);
    FIELD(shares);
    FIELD(match_number);
    FIELD(printable);
    FIELD(price);
    out << '}';
  }
  void on(const me::itch::Cancel& m) {
    START('X');
    FIELD(order_id);
    FIELD(shares);
    out << '}';
  }
  void on(const me::itch::Delete& m) {
    START('D');
    FIELD(order_id);
    out << '}';
  }
  void on(const me::itch::Replace& m) {
    START('U');
    FIELD(order_id);
    FIELD(new_order_id);
    FIELD(shares);
    FIELD(price);
    out << '}';
  }
  void on(const me::itch::Trade& m) {
    START('P');
    add_fields(m);
    FIELD(match_number);
    out << '}';
  }
#undef START
#undef FIELD
  void on_unknown(unsigned char type, std::span<const std::byte> payload) {
    if (!enabled) return;
    begin(static_cast<char>(type));
    out << ",\"payload\":\"";
    for (auto byte : payload) {
      const auto value = std::to_integer<unsigned char>(byte);
      out << hex[value >> 4] << hex[value & 15];
    }
    out << "\"}";
  }
};
void counts(std::ostream& out, const std::array<std::uint64_t, 256>& values) {
  out << '{';
  bool first = true;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (!values[i]) continue;
    if (!first) out << ',';
    first = false;
    const char kind = static_cast<char>(i);
    quote(out, {&kind, 1});
    out << ':' << values[i];
  }
  out << '}';
}
}  // namespace

int parser_cli(std::span<const std::filesystem::path> args) {
  if (args.empty() || args.size() > 2 ||
      (args.size() == 2 && args[1] != std::filesystem::path("--events"))) {
    std::cerr << "usage: itch_parse INPUT [--events]\n";
    return 1;
  }
  try {
    me::platform::MappedFile file(args[0]);
    JsonHandler handler{args.size() == 2, true, {}};
    me::itch::Stats stats;
    me::itch::parse(file.bytes(), handler, &stats);
    std::cout << "{\"messages\":" << stats.messages << ",\"counts\":";
    counts(std::cout, stats.counts);
    std::cout << ",\"skipped\":";
    counts(std::cout, stats.skipped);
    std::cout << ",\"bytes\":" << stats.bytes
              << ",\"terminated\":" << (stats.terminated ? "true" : "false");
    if (handler.enabled) std::cout << ",\"events\":[" << handler.out.str() << ']';
    std::cout << "}\n" << std::flush;
    return std::cout ? 0 : 1;
  } catch (const me::itch::ParseError& error) {
    std::cout << "{\"error\":\"" << error.what() << "\",\"offset\":" << error.offset << "}\n"
              << std::flush;
    return std::cout ? 2 : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
