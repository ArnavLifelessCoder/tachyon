#include <filesystem>
#include <vector>

#include "parser_cli.hpp"
#include "platform/main_trace.hpp"

int main(int argc, char** argv) {
  return me::platform::traced_main("itch_parse", [&] {
    std::vector<std::filesystem::path> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return parser_cli(args);
  });
}
