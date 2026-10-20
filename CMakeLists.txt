cmake_minimum_required(VERSION 3.20)
project(tachyon VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(ME_SANITIZER "none" CACHE STRING "Sanitizer: none, address, or thread")
set_property(CACHE ME_SANITIZER PROPERTY STRINGS none address thread)
option(ME_BUILD_BENCHMARKS "Fetch Google Benchmark for benchmark targets" OFF)

if(NOT ME_SANITIZER MATCHES "^(none|address|thread)$")
  message(FATAL_ERROR "Unknown ME_SANITIZER: ${ME_SANITIZER}")
endif()
if(MSVC)
  if(ME_SANITIZER STREQUAL "thread")
    message(FATAL_ERROR "ThreadSanitizer requires the Linux toolchain")
  elseif(ME_SANITIZER STREQUAL "address")
    # /RTC1 and incremental linking are incompatible with MSVC AddressSanitizer.
    set(CMAKE_CXX_FLAGS_DEBUG "/O1 /Zi")
    add_compile_options(/fsanitize=address)
    add_link_options(/INCREMENTAL:NO)
  endif()
else()
  if(ME_SANITIZER STREQUAL "address")
    add_compile_options(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  elseif(ME_SANITIZER STREQUAL "thread")
    add_compile_options(-O1 -g -fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
  endif()
  add_compile_options("$<$<CONFIG:Release>:-march=native>")
endif()

add_library(me INTERFACE)
target_include_directories(me INTERFACE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(me INTERFACE cxx_std_20)

function(me_strict_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /Zc:__cplusplus)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()

include(FetchContent)
include(CTest)
if(WIN32)
  set(ME_PLATFORM windows)
else()
  set(ME_PLATFORM posix)
endif()
add_library(me_platform STATIC src/platform/mapped_file_${ME_PLATFORM}.cpp)
target_include_directories(me_platform PUBLIC "${PROJECT_SOURCE_DIR}/src")
me_strict_warnings(me_platform)
add_executable(itch_parse src/parser_cli.cpp src/platform/cli_${ME_PLATFORM}.cpp)
target_link_libraries(itch_parse PRIVATE me me_platform)
me_strict_warnings(itch_parse)
if(BUILD_TESTING)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.16.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(googletest)
  add_executable(me_tests tests/smoke_test.cpp)
  target_link_libraries(me_tests PRIVATE me GTest::gtest_main)
  me_strict_warnings(me_tests)
  include(GoogleTest)
  gtest_discover_tests(me_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 60)
  find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/parser_cases.inc"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/generate_parser_cases.py"
      "${CMAKE_CURRENT_BINARY_DIR}/parser_cases.inc"
    DEPENDS tests/generate_parser_cases.py python/itch_writer.py
    VERBATIM)
  add_executable(parser_tests tests/parser_test.cpp "${CMAKE_CURRENT_BINARY_DIR}/parser_cases.inc")
  target_include_directories(parser_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(parser_tests PRIVATE me GTest::gtest)
  me_strict_warnings(parser_tests)
  gtest_discover_tests(parser_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 60)
  add_executable(mapping_tests tests/mapped_file_test.cpp)
  target_link_libraries(mapping_tests PRIVATE me_platform GTest::gtest_main)
  me_strict_warnings(mapping_tests)
  gtest_discover_tests(mapping_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 60)
  add_test(NAME python_tests COMMAND "${Python3_EXECUTABLE}" -m pytest
    -v -ra "${PROJECT_SOURCE_DIR}/tests/python"
    --parser "$<TARGET_FILE:itch_parse>"
    --basetemp "${CMAKE_BINARY_DIR}/pytest-tmp"
    -o "cache_dir=${CMAKE_BINARY_DIR}/pytest-cache")
  set_tests_properties(python_tests PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" TIMEOUT 300)
endif()

if(ME_BUILD_BENCHMARKS)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.2
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(googlebenchmark)
endif()
