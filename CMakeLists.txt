cmake_minimum_required(VERSION 3.25)
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
find_package(Threads REQUIRED)
add_library(me_platform STATIC src/platform/mapped_file_${ME_PLATFORM}.cpp
  src/platform/affinity_${ME_PLATFORM}.cpp)
target_include_directories(me_platform PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(me_platform PUBLIC Threads::Threads)
# Linked into the me_py shared module as well as executables, so it must be
# position-independent on Linux.
set_target_properties(me_platform PROPERTIES POSITION_INDEPENDENT_CODE ON)
me_strict_warnings(me_platform)
add_library(me_pipeline STATIC src/pipeline.cpp)
target_link_libraries(me_pipeline PUBLIC me me_platform)
me_strict_warnings(me_pipeline)
add_executable(match_orders src/match_orders.cpp)
target_link_libraries(match_orders PRIVATE me me_platform)
me_strict_warnings(match_orders)
add_executable(pipeline_replay src/pipeline_replay.cpp)
target_link_libraries(pipeline_replay PRIVATE me_pipeline)
me_strict_warnings(pipeline_replay)
add_executable(itch_parse src/parser_cli.cpp src/platform/cli_${ME_PLATFORM}.cpp)
target_link_libraries(itch_parse PRIVATE me me_platform)
me_strict_warnings(itch_parse)
add_executable(book_replay src/book_replay.cpp)
target_link_libraries(book_replay PRIVATE me me_platform)
me_strict_warnings(book_replay)
# Python bindings (M7). Built only without sanitizers: an instrumented extension
# cannot be loaded into an uninstrumented Python interpreter.
option(ME_BUILD_PYTHON "Build the me_py pybind11 module" ON)
if(ME_BUILD_PYTHON AND ME_SANITIZER STREQUAL "none")
  if(Python3_EXECUTABLE AND NOT Python_EXECUTABLE)
    set(Python_EXECUTABLE "${Python3_EXECUTABLE}")
  endif()
  find_package(Python 3.11 REQUIRED COMPONENTS Interpreter Development.Module)
  set(PYBIND11_FINDPYTHON ON)  # use the Python found above, not pybind11's legacy search
  FetchContent_Declare(pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG v2.13.6
    GIT_SHALLOW TRUE
    SYSTEM)
  FetchContent_MakeAvailable(pybind11)
  pybind11_add_module(me_py src/bindings/me_py.cpp)
  target_link_libraries(me_py PRIVATE me me_platform)
  me_strict_warnings(me_py)
endif()

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
  # Run each complete suite once, without discovery or per-case child processes.
  add_test(NAME smoke_tests COMMAND me_tests)
  find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/parser_cases.inc"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/generate_parser_cases.py"
      "${CMAKE_CURRENT_BINARY_DIR}/parser_cases.inc"
    DEPENDS tests/generate_parser_cases.py python/itch_writer.py
    VERBATIM)
  add_executable(parser_tests tests/parser_test.cpp "${CMAKE_CURRENT_BINARY_DIR}/parser_cases.inc")
  target_include_directories(parser_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(parser_tests PRIVATE me me_platform GTest::gtest)
  target_compile_definitions(parser_tests PRIVATE
    ME_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures")
  me_strict_warnings(parser_tests)
  add_test(NAME parser_tests COMMAND parser_tests)
  add_executable(mapping_tests tests/mapped_file_test.cpp)
  target_link_libraries(mapping_tests PRIVATE me_platform GTest::gtest_main)
  me_strict_warnings(mapping_tests)
  add_test(NAME mapping_tests COMMAND mapping_tests)
  set_tests_properties(smoke_tests mapping_tests PROPERTIES TIMEOUT 20)
  set_tests_properties(parser_tests PROPERTIES TIMEOUT 60)
  # Allocation probes replace global operator new/delete. Clang's statically linked
  # TSan runtime defines them too, and TSan only runs the concurrency suites, so
  # the probes are left out of TSan builds.
  if(NOT ME_SANITIZER STREQUAL "thread")
    add_executable(parser_allocation_test tests/parser_allocation_test.cpp)
    target_link_libraries(parser_allocation_test PRIVATE me me_platform)
    me_strict_warnings(parser_allocation_test)
    add_test(NAME parser_allocations COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/run_allocation_probe.py" "$<TARGET_FILE:parser_allocation_test>"
      "${PROJECT_SOURCE_DIR}/tests/fixtures/01302019.AAPL-MSFT-SPY.manifest.json")
  endif()
  add_executable(book_tests tests/book_test.cpp)
  target_link_libraries(book_tests PRIVATE me me_platform GTest::gtest)
  target_include_directories(book_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")
  target_compile_definitions(book_tests PRIVATE
    ME_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures")
  me_strict_warnings(book_tests)
  add_test(NAME book_tests COMMAND book_tests)
  # 10,000 differential seeds; the slowest native suite (about 4 minutes under MSVC ASan).
  set_tests_properties(book_tests PROPERTIES TIMEOUT 900)
  if(NOT ME_SANITIZER STREQUAL "thread")
    add_executable(book_allocation_test tests/book_allocation_test.cpp)
    target_link_libraries(book_allocation_test PRIVATE me me_platform)
    me_strict_warnings(book_allocation_test)
    # The probe replaces global new/delete with malloc/free on purpose; GCC cannot see
    # that the pair matches once both are inlined.
    target_compile_options(book_allocation_test PRIVATE
      $<$<CXX_COMPILER_ID:GNU>:-Wno-mismatched-new-delete>)
    add_test(NAME book_allocations COMMAND book_allocation_test
      "${PROJECT_SOURCE_DIR}/tests/fixtures/01302019.AAPL-MSFT-SPY.itch")
    set_tests_properties(book_allocations PROPERTIES TIMEOUT 60)
  endif()
  add_executable(matching_tests tests/matching_test.cpp)
  target_link_libraries(matching_tests PRIVATE me GTest::gtest_main)
  me_strict_warnings(matching_tests)
  add_test(NAME matching_tests COMMAND matching_tests)
  set_tests_properties(matching_tests PROPERTIES TIMEOUT 60)
  add_executable(pipeline_tests tests/pipeline_test.cpp)
  target_link_libraries(pipeline_tests PRIVATE me_pipeline GTest::gtest)
  target_compile_definitions(pipeline_tests PRIVATE
    ME_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures")
  me_strict_warnings(pipeline_tests)
  add_test(NAME pipeline_tests COMMAND pipeline_tests)
  # The concurrency label selects what the Linux TSan job runs.
  set_tests_properties(pipeline_tests PROPERTIES TIMEOUT 600 LABELS concurrency)
  # An empty --module-dir tells the binding tests this configuration has no module.
  set(ME_PY_TEST_ARGS "--module-dir=")
  if(TARGET me_py)
    set(ME_PY_TEST_ARGS "--module-dir=$<TARGET_FILE_DIR:me_py>")
  endif()
  add_test(NAME python_tests COMMAND "${Python3_EXECUTABLE}" -m pytest
    -v -ra "${PROJECT_SOURCE_DIR}/tests/python"
    --parser "$<TARGET_FILE:itch_parse>" ${ME_PY_TEST_ARGS}
    --basetemp "${CMAKE_BINARY_DIR}/pytest-tmp"
    -o "cache_dir=${CMAKE_BINARY_DIR}/pytest-cache")
  if(NOT ME_SANITIZER STREQUAL "thread")
    set_tests_properties(parser_allocations PROPERTIES TIMEOUT 60)
  endif()
  set_tests_properties(python_tests PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" TIMEOUT 300)
endif()

# Whole-system benchmark driver; built in every configuration so CI compiles it,
# but only meaningful in the release preset.
add_executable(bench_replay bench/bench_replay.cpp)
target_link_libraries(bench_replay PRIVATE me_pipeline)
me_strict_warnings(bench_replay)

if(ME_BUILD_BENCHMARKS)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.2
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(googlebenchmark)
  add_executable(bench_micro bench/bench_micro.cpp)
  target_link_libraries(bench_micro PRIVATE me me_platform benchmark::benchmark)
  me_strict_warnings(bench_micro)
endif()
