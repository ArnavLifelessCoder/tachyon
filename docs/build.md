# Build and test

Requirements: CMake 3.20+, Ninja, Python 3.11+, and either Visual Studio 2022 C++
Build Tools on Windows or GCC 12+/Clang 15+ on Linux. Initial CMake configuration
needs network access to fetch GoogleTest. Google Benchmark is fetched only when
`ME_BUILD_BENCHMARKS=ON`; M1 has no benchmark targets.

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
CTest verbosely. Complete test output is saved under `results/m1/`.

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
and `-Wall -Wextra -Wpedantic -Werror` elsewhere. `me` is an empty interface
library in M1; its first smoke test checks C++20 compilation.

CTest runs both GoogleTest and pytest. Python tooling is pinned in
`requirements-dev.txt`. Before committing, run:

```sh
ruff check .
ruff format --check .
clang-format --dry-run --Werror include/me/me.hpp tests/smoke_test.cpp
```

CI tests debug and ASan on Windows/MSVC and Linux/GCC and Clang, and checks
formatting and Python lint. The Ubuntu TSan job is declared but disabled until
M4 introduces concurrency. Windows ASan does not supply UBSan or TSan.

Milestones use branches `m1`, `m2`, and so on. Branch CI must pass before review.
After approval, merge with `--no-ff`, push `main`, and verify main's CI.
