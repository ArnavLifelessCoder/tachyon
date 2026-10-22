# Build and test

Requirements: CMake 3.25+ (FetchContent SYSTEM for pybind11), Ninja, Python 3.11+, and either Visual Studio 2022 C++
Build Tools on Windows or GCC 12+/Clang 15+ on Linux. Initial CMake configuration
needs network access to fetch GoogleTest. Google Benchmark is fetched only when
`ME_BUILD_BENCHMARKS=ON`; there are no benchmark targets yet.

## Windows

Run from the repository root in PowerShell. Keep temporary files and package
caches inside the project:

```powershell
New-Item -ItemType Directory -Force .tmp,.cache | Out-Null
$env:TEMP = Join-Path $PWD '.tmp'
$env:TMP = $env:TEMP
$env:UV_CACHE_DIR = Join-Path $PWD '.cache/uv'
uv venv .venv
uv pip install --python .venv/Scripts/python.exe -r requirements-dev.txt
./scripts/test.ps1 -Preset debug
./scripts/test.ps1 -Preset asan
```

The helper loads the installed VS 2022 x64 developer environment if needed,
configures CMake with the local Python environment, builds with Ninja, and runs
CTest verbosely. Complete test output is saved under ignored `.local/review/`;
review evidence belongs in the pull request description, not in `results/`.
CTest launches each complete GoogleTest executable once, without discovery or
per-case child processes. The parser suite has a 60-second CTest timeout; the
other native suites retain their 20-second timeout.

To run one case directly after building (replace the executable path as needed
on Linux):

```powershell
./build/debug/parser_tests.exe --gtest_filter=Parser.MutatedCorpusExactAllocationAndUnalignedInput --seed=1234
./build/asan/parser_tests.exe --gtest_filter=Parser.MutatedCorpusExactAllocationAndUnalignedInput --seed=1234
```

Use `--gtest_list_tests` on the executable to list available case names.
`ctest -R parser_tests` selects the complete suite, not one GoogleTest case.
The local test helper enables `ITCH_PARSE_TRACE=1` and `ME_TRACE=1` for entry/return
diagnostics and restores the previous setting afterward. For a direct parser
invocation, set that environment variable explicitly when tracing is needed.

Local ASan runs enable `ASAN_OPTIONS=verbosity=1` and write per-process diagnostic
logs under `.local/review/` and `build/asan/.local/review/`, with timestamped
prefixes. Diagnostic settings are applied only to child processes, preserving
the calling environment.

### Sanitizer gate and Windows exit stalls (M3 onward)

Linux CI ASan + UBSan on **both GCC and Clang** is the sanitizer gate and must
be green on every milestone branch. Windows tests remain required under the
following owner-approved policy:

- An `itch_parse` timeout qualifies as the known MSVC toolchain exit stall only
  when both trace markers printed in order and `main returning status=N` matches
  the status expected by the test. Expected status is supplied by the caller,
  never inferred from the marker.
- A GoogleTest executable timeout qualifies only when its final
  `[  PASSED  ] N tests.` summary printed before the hang.
- Other native executables use the same entry/return convention with their
  executable name; the expected status is the CTest success status, zero.
- Any missing marker, wrong status, failing assertion, sanitizer report, or
  unclassified failure blocks. A second stall of the same suite in the retry
  also blocks. Debug failures and Linux failures do not receive this exception.
- Log a qualifying stall in the PR with the suite, elapsed time, marker lines,
  and ASan log path. Rerun that complete CTest suite **once**. A successful retry
  does not erase the first attempt or make it an uninterrupted clean run.

`scripts/test.ps1` calls `scripts/run_test_policy.py` to apply this policy.
It reads CTest JUnit output (requires CMake 3.21+) and structured pytest failure
records, then classifies each suite as `passed`, `known_stall`, or `failure`.
Only qualifying suites are selected for one retry; an unrelated failure blocks
all retries. Pytest is one CTest suite, so any qualifying child timeout causes
one full pytest rerun. No child invocation retries itself.

Each run saves immutable attempt logs, CTest XML, pytest evidence, `report.json`,
and a PR-ready `review.md` under `.local/review/<preset>-<run>/`. ASan logs have
attempt-specific prefixes under that directory and its counterpart beneath the
build directory. Both resolved log paths and search patterns are recorded;
when the runtime emits diagnostics only on stderr, the attempt log retains them.
`<preset>-policy-latest.json` points to the latest report. `Policy result: PASS`
means every suite passed initially or on its single authorized retry.

New M3+ C++ executables run by CTest must wrap their entry point using
`me::platform::traced_main` from `src/platform/main_trace.hpp`, with the executable
name as the first argument. Put all work and owned resources inside its callable
body, return the intended exit status, and allow exceptions to remain failures.
The helper emits flushed entry/return markers when either `ITCH_PARSE_TRACE=1`
or `ME_TRACE=1` is set. The return marker follows body-local cleanup. GoogleTest
executables still qualify through their final passed summary. Do not bypass
normal shutdown, suppress sanitizer reports, or raise a deadline to hide a stall.

Alternatively, use an x64 Native Tools Command Prompt and run CMake directly,
passing `-DPython3_EXECUTABLE=<absolute path to .venv/Scripts/python.exe>`.

## Linux

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install --no-cache-dir -r requirements-dev.txt
cmake --preset debug -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build --preset debug
ctest --preset debug --verbose
cmake --preset asan -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build --preset asan
ctest --preset asan --verbose
```

Set `CXX=clang++` before the first configure to use Clang. Use a fresh build
directory when changing compiler.

## Presets and checks

| Preset | Windows | Linux |
| --- | --- | --- |
| `debug` | `/Od /Zi`, runtime checks | `-g`, unoptimized |
| `release` | `/O2 /DNDEBUG` | `-O3 -march=native -DNDEBUG` |
| `asan` | `/O1 /Zi /fsanitize=address`, no incremental link or `/RTC1` | ASan + UBSan, `-O1 -g`, frame pointers |
| `tsan` | Explicitly unsupported | TSan, `-O1 -g`, frame pointers |

Release is reserved for benchmarks. Project C++ targets use `/W4 /WX` on MSVC
and `-Wall -Wextra -Wpedantic -Werror` elsewhere. `me` contains the header-only
span parser. `me_platform` supplies native file mappings; `itch_parse` is the
diagnostic executable used by the differential integration tests.

CTest runs both GoogleTest and pytest. Python tooling is pinned in
`requirements-dev.txt`, including clang-format 19.1.5 for both local use and CI.
Activate `.venv` or use its executable paths before running these checks:

```sh
ruff check .
ruff format --check .
git ls-files '*.cpp' '*.hpp' | xargs clang-format --dry-run --Werror
```

The last command uses a POSIX shell. In PowerShell, pass the paths returned by
`git ls-files '*.cpp' '*.hpp'` as arguments to `clang-format`.
CTest supplies pytest with the executable from its own build preset through
`--parser`; direct pytest runs can supply that option explicitly.

CI tests debug and ASan on Windows/MSVC and Linux/GCC and Clang, and checks
formatting and Python lint. The Ubuntu TSan job is declared but disabled until
M4 introduces concurrency. Windows ASan does not supply UBSan or TSan.

Milestones use branches `m1`, `m2`, and so on. Branch CI must pass before review.
After approval, merge with `--no-ff`, push `main`, and verify main's CI.
