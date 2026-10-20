# Build and test

Requirements: CMake 3.20+, Ninja, Python 3.11+, and either Visual Studio 2022 C++
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
The local test helper enables `ITCH_PARSE_TRACE=1` for parser entry/return
diagnostics and restores the previous setting afterward. For a direct parser
invocation, set that environment variable explicitly when tracing is needed.

Local ASan runs enable `ASAN_OPTIONS=verbosity=1` and write per-process diagnostic
logs under `.local/review/` and `build/asan/.local/review/`, with timestamped
prefixes. Existing options are restored after the run. If a process-exit timeout
recurs, retain and inspect these logs before considering another run; do not
hide it with an increased timeout or a sanitizer suppression.

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
