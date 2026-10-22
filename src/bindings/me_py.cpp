// pybind11 module me_py: replay ITCH into books and query them, and run the
// matching engine on an order list. A thin layer over the C++ types.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "me/book_builder.hpp"
#include "me/itch_parser.hpp"
#include "me/matching_engine.hpp"
#include "platform/mapped_file.hpp"

namespace py = pybind11;

namespace {

std::string trim(const std::array<char, 8>& raw) {
  std::string out(raw.data(), raw.size());
  out.erase(out.find_last_not_of(' ') + 1);
  return out;
}

// Steps through a raw ITCH file message by message, so callers can stop at any
// timestamp and inspect the reconstructed books.
class Replay {
 public:
  explicit Replay(const std::string& path)
      : file_(std::make_unique<me::platform::MappedFile>(std::filesystem::path(path))) {}

  // Applies messages until the next one is later than timestamp_ns (nanoseconds
  // since midnight). Returns the number of messages applied by this call.
  std::uint64_t advance_to(std::uint64_t timestamp_ns) {
    const auto bytes = file_->bytes();
    std::uint64_t applied = 0;
    while (offset_ + 2 <= bytes.size()) {
      const auto length = (std::to_integer<std::size_t>(bytes[offset_]) << 8) |
                          std::to_integer<std::size_t>(bytes[offset_ + 1]);
      if (length == 0) {  // end-of-session marker
        offset_ = bytes.size();
        break;
      }
      if (offset_ + 2 + length > bytes.size()) throw std::runtime_error("truncated ITCH file");
      if (length >= 11) {
        std::uint64_t ts = 0;
        for (std::size_t i = 0; i < 6; ++i) {
          ts = (ts << 8) | std::to_integer<std::uint64_t>(bytes[offset_ + 2 + 5 + i]);
        }
        if (ts > timestamp_ns) break;
        now_ = ts;
      }
      const auto frame = bytes.subspan(offset_, 2 + length);
      me::itch::parse(frame, builder_);
      offset_ += 2 + length;
      ++applied;
      ++messages_;
    }
    return applied;
  }
  std::uint64_t run_to_end() { return advance_to(~0ULL); }

  std::optional<std::uint16_t> locate(const std::string& symbol) {
    if (symbols_.empty() || !symbols_.count(symbol)) {
      for (std::size_t l = 0; l < me::BookBuilder::max_locates; ++l) {
        const auto name = trim(builder_.symbol(static_cast<std::uint16_t>(l)));
        if (!name.empty() && name[0] != '\0') symbols_[name] = static_cast<std::uint16_t>(l);
      }
    }
    auto it = symbols_.find(symbol);
    if (it == symbols_.end()) return std::nullopt;
    return it->second;
  }

  // (bid price, bid size, ask price, ask size); prices in 1/10000 dollars, None if empty.
  py::tuple bbo(const std::string& symbol) {
    const me::OrderBook* book = book_for(symbol);
    const me::Level* bid = book ? book->bids().best() : nullptr;
    const me::Level* ask = book ? book->asks().best() : nullptr;
    auto price = [](const me::Level* l) { return l ? py::object(py::int_(l->price)) : py::none(); };
    auto size = [](const me::Level* l) { return l ? py::object(py::int_(l->total)) : py::none(); };
    return py::make_tuple(price(bid), size(bid), price(ask), size(ask));
  }

  // {"bids": [(price, total, orders), ...], "asks": [...]}, best first.
  py::dict depth(const std::string& symbol, std::size_t levels) {
    py::dict out;
    const me::OrderBook* book = book_for(symbol);
    for (auto side : {me::Side::buy, me::Side::sell}) {
      py::list rows;
      if (book) {
        std::vector<me::LevelView> view(std::min(levels, book->side(side).size()));
        view.resize(book->side(side).depth(view));
        for (const auto& v : view) rows.append(py::make_tuple(v.price, v.total, v.count));
      }
      out[side == me::Side::buy ? "bids" : "asks"] = rows;
    }
    return out;
  }

  py::dict stats() const {
    const auto& s = builder_.stats();
    py::dict out;
    out["messages"] = messages_;
    out["adds"] = s.adds;
    out["executes"] = s.executes;
    out["cancels"] = s.cancels;
    out["deletes"] = s.deletes;
    out["replaces"] = s.replaces;
    out["trades"] = s.trades;
    out["unknown_refs"] = s.unknown_refs;
    out["live_orders"] = s.live_orders;
    return out;
  }
  std::uint64_t now() const noexcept { return now_; }
  bool finished() const noexcept { return offset_ >= file_->bytes().size(); }

 private:
  const me::OrderBook* book_for(const std::string& symbol) {
    const auto l = locate(symbol);
    return l ? builder_.book(*l) : nullptr;
  }

  std::unique_ptr<me::platform::MappedFile> file_;
  me::BookBuilder builder_{1 << 20};
  std::size_t offset_ = 0;
  std::uint64_t messages_ = 0;
  std::uint64_t now_ = 0;
  std::unordered_map<std::string, std::uint16_t> symbols_;
};

me::OrderType order_type(const std::string& name) {
  if (name == "limit") return me::OrderType::limit;
  if (name == "market") return me::OrderType::market;
  if (name == "ioc") return me::OrderType::ioc;
  throw py::value_error("order type must be 'limit', 'market' or 'ioc'");
}

me::Side side_of(const std::string& name) {
  if (name == "buy") return me::Side::buy;
  if (name == "sell") return me::Side::sell;
  throw py::value_error("side must be 'buy' or 'sell'");
}

const char* event_name(me::EventType type) {
  switch (type) {
    case me::EventType::ack:
      return "ack";
    case me::EventType::trade:
      return "trade";
    case me::EventType::cancelled:
      return "cancelled";
    case me::EventType::rejected:
      return "rejected";
  }
  return "unknown";
}

const char* reason_name(me::RejectReason reason) {
  switch (reason) {
    case me::RejectReason::none:
      return "none";
    case me::RejectReason::invalid_quantity:
      return "invalid_quantity";
    case me::RejectReason::invalid_price:
      return "invalid_price";
    case me::RejectReason::duplicate_id:
      return "duplicate_id";
    case me::RejectReason::unknown_order:
      return "unknown_order";
    case me::RejectReason::no_liquidity:
      return "no_liquidity";
  }
  return "unknown";
}

// Runs a list of requests through a fresh engine and returns every event as a dict.
// Requests are dicts: {"op": "new", "id", "side", "price", "qty", "type"},
// {"op": "cancel", "id"} or {"op": "modify", "id", "price", "qty"}.
py::list match(const py::list& requests) {
  me::MatchingEngine engine;
  py::list events;
  auto sink = [&](const me::ExecEvent& e) {
    py::dict d;
    d["type"] = event_name(e.type);
    d["id"] = e.id;
    if (e.type == me::EventType::trade) d["maker_id"] = e.maker_id;
    if (e.type == me::EventType::rejected) {
      d["reason"] = reason_name(e.reason);
    } else {
      d["price"] = e.price;
      d["qty"] = e.qty;
    }
    events.append(d);
  };
  for (const auto& item : requests) {
    const auto r = item.cast<py::dict>();
    const auto op = r["op"].cast<std::string>();
    const auto id = r["id"].cast<std::uint64_t>();
    if (op == "new") {
      engine.submit(me::NewOrder{id, side_of(r["side"].cast<std::string>()),
                                 r.contains("price") ? r["price"].cast<std::uint32_t>() : 0U,
                                 r["qty"].cast<std::uint32_t>(),
                                 order_type(r.contains("type") ? r["type"].cast<std::string>()
                                                               : std::string("limit"))},
                    sink);
    } else if (op == "cancel") {
      engine.submit(me::CancelOrder{id}, sink);
    } else if (op == "modify") {
      engine.submit(
          me::ModifyOrder{id, r["price"].cast<std::uint32_t>(), r["qty"].cast<std::uint32_t>()},
          sink);
    } else {
      throw py::value_error("op must be 'new', 'cancel' or 'modify'");
    }
  }
  return events;
}

}  // namespace

PYBIND11_MODULE(me_py, m) {
  m.doc() = "ITCH book replay and price-time matching engine (prices in 1/10000 dollars)";
  py::class_<Replay>(m, "Replay")
      .def(py::init<const std::string&>(), py::arg("path"))
      .def("advance_to", &Replay::advance_to, py::arg("timestamp_ns"),
           "Apply messages up to and including timestamp_ns; returns messages applied")
      .def("run_to_end", &Replay::run_to_end)
      .def("bbo", &Replay::bbo, py::arg("symbol"),
           "(bid, bid_size, ask, ask_size) for a symbol; None for an empty side")
      .def("depth", &Replay::depth, py::arg("symbol"), py::arg("levels") = 5)
      .def("locate", &Replay::locate, py::arg("symbol"))
      .def("stats", &Replay::stats)
      .def_property_readonly("now", &Replay::now, "timestamp of the last applied message (ns)")
      .def_property_readonly("finished", &Replay::finished);
  m.def("match", &match, py::arg("requests"),
        "Run requests through a fresh matching engine; returns the event list");
}
