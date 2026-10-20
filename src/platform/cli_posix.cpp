#include <cstdio>
#include <filesystem>
#include <vector>

#include "parser_cli.hpp"

int main(int argc, char** argv) {
  std::fputs("itch_parse: main entered\n", stderr);
  std::fflush(stderr);
  int status = 1;
  {
    std::vector<std::filesystem::path> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    status = parser_cli(args);
  }
  std::fprintf(stderr, "itch_parse: main returning status=%d\n", status);
  std::fflush(stderr);
  return status;
}
