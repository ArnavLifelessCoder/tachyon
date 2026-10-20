#include "parser_cli.hpp"

#include <array>
#include <charconv>
#include <concepts>
#include <iostream>
#include <sstream>
#include <string_view>

#include "me/itch_parser.hpp"
#include "platform/mapped_file.hpp"

namespace {
constexpr char hex[] = "0123456789abcdef";
class JsonBuffer {
 public:
  JsonBuffer& operator<<(char value) {
    data_.push_back(value);
    return *this;
  }
  JsonBuffer& operator<<(std::string_view value) {
    data_.append(value);
    return *this;
  }
  template <std::integral T>
  JsonBuffer& operator<<(T value) {
    static_assert(sizeof(T) <= sizeof(std::uint64_t));
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    data_.append(buffer, static_cast<std::size_t>(result.ptr - buffer));
    return *this;
  }
  const std::string& str() const { return data_; }

 private:
  std::string data_;
};
template <class Output>
void quote(Output& out, std::string_view text) {
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
  JsonBuffer out;
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
int parse_file(const std::filesystem::path& path, bool events, std::ostream& out) {
  try {
    me::platform::MappedFile file(path);
    JsonHandler handler{events, true, {}};
    me::itch::Stats stats;
    me::itch::parse(file.bytes(), handler, &stats);
    out << "{\"messages\":" << stats.messages << ",\"counts\":";
    counts(out, stats.counts);
    out << ",\"skipped\":";
    counts(out, stats.skipped);
    out << ",\"bytes\":" << stats.bytes
        << ",\"terminated\":" << (stats.terminated ? "true" : "false");
    if (handler.enabled) out << ",\"events\":[" << handler.out.str() << ']';
    out << '}';
    return 0;
  } catch (const me::itch::ParseError& error) {
    out << "{\"error\":\"" << error.what() << "\",\"offset\":" << error.offset << '}';
    return 2;
  }
}
}  // namespace

int parser_cli(std::span<const std::filesystem::path> args) {
  if (args.empty() || args.size() > 2 ||
      (args.size() == 2 && args[1] != std::filesystem::path("--events"))) {
    std::cerr << "usage: itch_parse INPUT [--events] | --batch [--events]\n";
    return 1;
  }
  try {
    const bool events = args.size() == 2;
    if (args[0] != std::filesystem::path("--batch")) {
      const int status = parse_file(args[0], events, std::cout);
      std::cout << '\n' << std::flush;
      return std::cout ? status : 1;
    }
    std::string line;
    std::size_t index = 0;
    while (std::getline(std::cin, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      std::cerr << "itch_parse: input index=" << index++ << '\n' << std::flush;
      std::ostringstream result;
      int status = 1;
      try {
        // The batch protocol uses one UTF-8 path per line on every platform.
        const std::u8string utf8(line.begin(), line.end());
        status = parse_file(std::filesystem::path(utf8), events, result);
      } catch (const std::exception& error) {
        result.str("");
        result.clear();
        result << "{\"error\":\"io_error\",\"message\":";
        quote(result, error.what());
        result << '}';
      }
      std::cout << "{\"status\":" << status << ",\"result\":" << result.str() << "}\n"
                << std::flush;
      if (!std::cout) return 1;
    }
    return std::cin.bad() ? 1 : 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
