// Runs one raw ITCH file through the single-threaded reference and the threaded
// pipeline and prints both results as JSON. Exit status 0 means the final books
// and the published BBO sequences are identical.

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "pipeline.hpp"
#include "platform/main_trace.hpp"
#include "platform/mapped_file.hpp"

namespace {
void print(std::ostream& out, const me::pipeline::Result& r) {
  const auto& h = r.latency_ns;
  out << "{\"messages\":" << r.parse.messages << ",\"updates\":" << r.updates
      << ",\"bbo_changes\":" << r.bbo_changes << ",\"unknown_refs\":" << r.book.unknown_refs
      << ",\"live_orders_at_end\":" << r.book.live_orders << ",\"book_digest\":\"" << std::hex
      << r.book_digest << "\",\"bbo_digest\":\"" << r.bbo_digest << std::dec
      << "\",\"seconds\":" << r.seconds
      << ",\"messages_per_second\":" << static_cast<double>(r.parse.messages) / r.seconds
      << ",\"latency_ns\":{\"count\":" << h.count() << ",\"p50\":" << h.percentile(0.5)
      << ",\"p90\":" << h.percentile(0.9) << ",\"p99\":" << h.percentile(0.99)
      << ",\"p999\":" << h.percentile(0.999) << ",\"max\":" << h.max() << ",\"mean\":" << h.mean()
      << "},\"ticks_per_ns\":" << r.ticks_per_ns
      << ",\"invariant_tsc\":" << (r.invariant_tsc ? "true" : "false") << ",\"pinned\":["
      << r.pinned[0] << ',' << r.pinned[1] << ',' << r.pinned[2] << "],\"checkpoint_digests\":[";
  for (std::size_t i = 0; i < r.checkpoint_count; ++i) {
    out << (i ? "," : "") << '"' << std::hex << r.checkpoints[i] << std::dec << '"';
  }
  out << "]}";
}

bool same_checkpoints(const me::pipeline::Result& a, const me::pipeline::Result& b) {
  if (a.checkpoint_count != b.checkpoint_count) return false;
  for (std::size_t i = 0; i < a.checkpoint_count; ++i) {
    if (a.checkpoints[i] != b.checkpoints[i]) return false;
  }
  return true;
}

int run(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: pipeline_replay RAW_ITCH_FILE [PARSER_CPU BOOK_CPU PUBLISHER_CPU]\n";
    return 1;
  }
  me::pipeline::Config config;
  config.expected_orders = 1 << 22;
  config.checkpoint_every = 25'000'000;
  if (argc == 5) {
    config.parser_cpu = std::atoi(argv[2]);
    config.book_cpu = std::atoi(argv[3]);
    config.publisher_cpu = std::atoi(argv[4]);
  }
  const me::platform::MappedFile file{std::filesystem::path(argv[1])};
  const auto single = me::pipeline::run_single(file.bytes(), config);
  const auto threaded = me::pipeline::run_threaded(file.bytes(), config);
  // Final books plus every mid-day checkpoint: a full day ends with all books
  // empty, so the checkpoints carry most of the evidence.
  const bool books_equal =
      single.book_digest == threaded.book_digest && same_checkpoints(single, threaded);
  const bool bbo_equal =
      single.bbo_digest == threaded.bbo_digest && single.bbo_changes == threaded.bbo_changes;
  std::cout << "{\"books_identical\":" << (books_equal ? "true" : "false")
            << ",\"checkpoint_every_messages\":" << config.checkpoint_every
            << ",\"bbo_sequence_identical\":" << (bbo_equal ? "true" : "false") << ",\"cpus\":["
            << config.parser_cpu << ',' << config.book_cpu << ',' << config.publisher_cpu
            << "],\"single\":";
  print(std::cout, single);
  std::cout << ",\"threaded\":";
  print(std::cout, threaded);
  std::cout << "}\n" << std::flush;
  return books_equal && bbo_equal ? 0 : 4;
}
}  // namespace

int main(int argc, char** argv) {
  return me::platform::traced_main("pipeline_replay", [&] {
    try {
      return run(argc, argv);
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  });
}
