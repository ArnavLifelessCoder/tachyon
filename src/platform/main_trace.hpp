#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdlib>
#include <cstring>
#endif

#include <cstdio>
#include <utility>

namespace me::platform {
inline bool main_trace_enabled() {
#ifdef _WIN32
  wchar_t value[2]{};
  return (GetEnvironmentVariableW(L"ITCH_PARSE_TRACE", value, 2) == 1 && value[0] == L'1') ||
         (GetEnvironmentVariableW(L"ME_TRACE", value, 2) == 1 && value[0] == L'1');
#else
  const auto enabled = [](const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
  };
  return enabled("ITCH_PARSE_TRACE") || enabled("ME_TRACE");
#endif
}

// Keep all executable work inside body so its locals are destroyed before the return marker.
template <class Body>
int traced_main(const char* program, Body&& body) {
  const bool trace = main_trace_enabled();
  if (trace) {
    std::fprintf(stderr, "%s: main entered\n", program);
    std::fflush(stderr);
  }
  const int status = std::forward<Body>(body)();
  if (trace) {
    std::fprintf(stderr, "%s: main returning status=%d\n", program, status);
    std::fflush(stderr);
  }
  return status;
}
}  // namespace me::platform
