#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#include "parser_cli.hpp"

int main(int argc, char** argv) {
  const char* value = std::getenv("ITCH_PARSE_TRACE");
  const bool trace = value != nullptr && std::strcmp(value, "1") == 0;
  if (trace) {
    std::fputs("itch_parse: main entered\n", stderr);
    std::fflush(stderr);
  }
  int status = 1;
  {
    std::vector<std::filesystem::path> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    status = parser_cli(args);
  }
  if (trace) {
    std::fprintf(stderr, "itch_parse: main returning status=%d\n", status);
    std::fflush(stderr);
  }
  return status;
}
