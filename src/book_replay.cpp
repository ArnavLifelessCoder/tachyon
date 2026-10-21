// Replays a raw (decompressed) ITCH file into per-symbol books and prints the
// replay statistics as one JSON object. Used for the full-day M3 checks.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "me/book_builder.hpp"
#include "me/itch_parser.hpp"
#include "platform/main_trace.hpp"
#include "platform/mapped_file.hpp"

namespace {
std::string symbol(const std::array<char, 8>& raw) {
  std::string out(raw.data(), raw.size());
  out.erase(out.find_last_not_of(" \0", std::string::npos, 2) + 1);
  return out;
}

// One stretch of consecutive book changes during which a symbol stayed crossed
// or locked, with the symbol's ITCH trading state (from H messages) at its start.
struct Episode {
  std::uint16_t locate;
  std::uint64_t start_ns, end_ns, events;
  char trading_state;
};

// Forwards every message to the book builder and records crossed episodes plus
// trading-state changes, so each crossed book can be explained afterwards.
struct ReplayHandler {
  me::BookBuilder& builder;
  std::vector<char> trading_state = std::vector<char>(me::BookBuilder::max_locates, '?');
  std::vector<std::int64_t> open_episode =
      std::vector<std::int64_t>(me::BookBuilder::max_locates, -1);
  std::vector<Episode> episodes;

  template <class Message>
  void on(const Message& m) {
    builder.on(m);
    const std::uint16_t locate = m.header.stock_locate;
    auto& open = open_episode[locate];
    if (builder.crossed_now(locate)) {
      if (open < 0) {
        open = static_cast<std::int64_t>(episodes.size());
        episodes.push_back(
            {locate, m.header.timestamp, m.header.timestamp, 0, trading_state[locate]});
      }
      auto& episode = episodes[static_cast<std::size_t>(open)];
      episode.end_ns = m.header.timestamp;
      ++episode.events;
    } else {
      open = -1;
    }
  }
  void on_unknown(unsigned char kind, std::span<const std::byte> payload) {
    builder.on_unknown(kind, payload);
    // H: stock trading action. State byte at offset 19: H halted, P paused,
    // Q quotation only, T trading.
    if (kind == 'H' && payload.size() >= 20) {
      const auto locate = static_cast<std::uint16_t>((std::to_integer<unsigned>(payload[1]) << 8) |
                                                     std::to_integer<unsigned>(payload[2]));
      trading_state[locate] = static_cast<char>(payload[19]);
    }
  }
};

std::string clock(std::uint64_t ns) {
  const std::uint64_t s = ns / 1'000'000'000ULL;
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%02u:%02u:%02u", static_cast<unsigned>(s / 3600),
                static_cast<unsigned>(s / 60 % 60), static_cast<unsigned>(s % 60));
  return buffer;
}

int run(const std::filesystem::path& path) {
  const me::platform::MappedFile file(path);
  me::BookBuilder builder(1 << 22);
  ReplayHandler handler{builder};
  me::itch::Stats parse_stats;
  const auto start = std::chrono::steady_clock::now();
  me::itch::parse(file.bytes(), handler, &parse_stats);
  const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
  const auto& s = builder.stats();

  std::vector<std::uint16_t> crossed_locates;
  std::uint64_t books = 0, resting_levels = 0;
  for (std::size_t locate = 0; locate < me::BookBuilder::max_locates; ++locate) {
    const auto l = static_cast<std::uint16_t>(locate);
    if (builder.crossed_events_for(l)) crossed_locates.push_back(l);
    if (const auto* book = builder.book(l)) {
      ++books;
      resting_levels += book->bids().size() + book->asks().size();
    }
  }
  std::sort(crossed_locates.begin(), crossed_locates.end(), [&](auto a, auto b) {
    return builder.crossed_events_for(a) > builder.crossed_events_for(b);
  });

  auto& out = std::cout;
  out << "{\"bytes\":" << parse_stats.bytes << ",\"messages\":" << parse_stats.messages
      << ",\"terminated\":" << (parse_stats.terminated ? "true" : "false")
      << ",\"replay_seconds\":" << elapsed.count() << ",\"counts\":{";
  bool first = true;
  for (std::size_t kind = 0; kind < parse_stats.counts.size(); ++kind) {
    if (!parse_stats.counts[kind]) continue;
    out << (first ? "" : ",") << '"' << static_cast<char>(kind)
        << "\":" << parse_stats.counts[kind];
    first = false;
  }
  out << "},\"adds\":" << s.adds << ",\"executes\":" << s.executes << ",\"cancels\":" << s.cancels
      << ",\"deletes\":" << s.deletes << ",\"replaces\":" << s.replaces
      << ",\"trades\":" << s.trades << ",\"unknown_refs\":" << s.unknown_refs
      << ",\"duplicate_adds\":" << s.duplicate_adds
      << ",\"locate_mismatches\":" << s.locate_mismatches
      << ",\"over_executions\":" << s.over_executions << ",\"crossed_events\":" << s.crossed_events
      << ",\"locked_events\":" << s.locked_events << ",\"crossed_episodes\":" << s.crossed_episodes
      << ",\"crossed_or_locked_events_by_hour\":[";
  for (std::size_t hour = 0; hour < 24; ++hour) {
    out << (hour ? "," : "") << s.crossed_events_by_hour[hour];
  }
  out << "],\"symbols_with_crossed_or_locked_books\":" << crossed_locates.size()
      << ",\"top_crossed_symbols\":[";
  for (std::size_t i = 0; i < std::min<std::size_t>(20, crossed_locates.size()); ++i) {
    const auto l = crossed_locates[i];
    out << (i ? "," : "") << "{\"locate\":" << l << ",\"symbol\":\"" << symbol(builder.symbol(l))
        << "\",\"events\":" << builder.crossed_events_for(l) << '}';
  }
  out << "],\"crossed_episodes_detail\":[";
  for (std::size_t i = 0; i < handler.episodes.size(); ++i) {
    const auto& e = handler.episodes[i];
    out << (i ? "," : "") << "{\"symbol\":\"" << symbol(builder.symbol(e.locate))
        << "\",\"start\":\"" << clock(e.start_ns) << "\",\"end\":\"" << clock(e.end_ns)
        << "\",\"symbol_messages\":" << e.events << ",\"trading_state_at_start\":\""
        << e.trading_state << "\"}";
  }
  out << "],\"books\":" << books << ",\"resting_levels_at_end\":" << resting_levels
      << ",\"live_orders_at_end\":" << s.live_orders
      << ",\"peak_live_orders\":" << s.peak_live_orders << "}\n"
      << std::flush;
  return s.unknown_refs == 0 ? 0 : 3;
}
}  // namespace

int main(int argc, char** argv) {
  return me::platform::traced_main("book_replay", [&] {
    if (argc != 2) {
      std::cerr << "usage: book_replay RAW_ITCH_FILE\n";
      return 1;
    }
    try {
      return run(argv[1]);
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  });
}
