#include <filesystem>
#include <vector>

#include "parser_cli.hpp"

int wmain(int argc, wchar_t** argv) {
  std::vector<std::filesystem::path> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
  return parser_cli(args);
}
