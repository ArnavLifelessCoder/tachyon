#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <vector>

#include "parser_cli.hpp"

int wmain(int argc, wchar_t** argv) {
  wchar_t value[2]{};
  const bool trace =
      GetEnvironmentVariableW(L"ITCH_PARSE_TRACE", value, 2) == 1 && value[0] == L'1';
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
