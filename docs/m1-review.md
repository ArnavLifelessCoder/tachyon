# M1 review packet

Implementation revision: `0f9b6db71e188c37b155d9877fa5a1d05b063a1f`.
Branch: `m1`. Approval and the merge to `main` are pending.

## Built

- Empty C++20 `me` interface library and a GoogleTest smoke test.
- Ninja presets for debug, release, ASan, and Linux TSan.
- Windows/MSVC and Linux/GCC and Clang debug/ASan CI, plus lint.
- Synthetic ITCH encoder for all ten required message types, raw framing,
  unsigned-field validation, and integer prices.
- A streaming, size-checked day downloader with SHA-256 output and tests using
  mocked responses. No full-day download was performed.
- A raw/gzip fixture cutter with truncation detection, overwrite protection,
  and a strict size cap.
- Reproducible synthetic fixture: 15 messages, 484 bytes, covering two symbols
  and all required message types. Its manifest is in
  [fixture.json](../results/m1/fixture.json).
- Local test helper and build/data documentation.

## Acceptance

| Criterion | Evidence |
| --- | --- |
| CMake presets, empty library, passing test | `CMakeLists.txt`, `CMakePresets.json`, local test logs |
| CI green on all applicable jobs | All seven active jobs pass in [run 39413393178](https://github.com/ArnavLifelessCoder/tachyon/actions/runs/39413393178); snapshot in `results/m1/ci.json` |
| Download and fixture scripts, synthetic writer | Unit and command-line tests in both local runs |
| Small committed synthetic fixture | Deterministic regeneration and framing/count tests |
| Local debug and ASan green | One C++ test and 72 Python tests pass in each run |
| Specs excluded, full data excluded | `.gitignore`; no spec or full-day file is tracked |
| TSan | Not applicable until M4; Ubuntu job is present but disabled |

## Full local test output

- [Debug, complete verbose CTest output](../results/m1/debug-tests.txt)
- [MSVC ASan, complete verbose CTest output](../results/m1/asan-tests.txt)
- [Lint and formatting](../results/m1/lint.txt)
- [Local environment](../results/m1/environment.json)

These are test observations, not benchmark or profiling results. ASan instruments
the C++ executable; pytest runs Python tooling separately within the same CTest
invocation. Linux CI additionally uses UBSan.

The saved CI snapshot identifies the implementation revision. A subsequent
evidence-only commit adds this packet and the logs; its CI is checked separately
before presenting the branch for approval.

## Verification issues resolved

The initial CI lint check caught include ordering in the C++ smoke test. Local
pytest also exposed a Windows environment-variable length limit when a large
byte string became a parameter name; explicit short IDs fixed test reporting.
The oversized-payload test remains in the suite.

One local ASan process stalled after printing its passing test result. Its parent
test runner was terminated before rebuilding. Subsequent discovery runs hit
GoogleTest's default five-second discovery timeout, including in debug mode.
Discovery now allows 60 seconds, and individual C++ tests and the Python suite
have explicit timeouts. The complete reruns linked above exited successfully.
The cause of the initial exit stall was not established; no sanitizer finding
was reported.

## Files changed

All files were added for M1. The two repository-policy files are in the initial
local `main` root commit; the remaining implementation commits are on `m1`.

| Area | Files |
| --- | --- |
| Repository policy | `.gitignore`, `.gitattributes` |
| C++ build | `CMakeLists.txt`, `CMakePresets.json`, `.clang-format`, `.clang-tidy`, `include/me/me.hpp` |
| CI | `.github/workflows/ci.yml` |
| Python setup | `pyproject.toml`, `requirements-dev.txt` |
| Tools | `python/itch_writer.py`, `scripts/download_itch.py`, `scripts/make_fixture.py`, `scripts/test.ps1` |
| Tests | `tests/smoke_test.cpp`, `tests/python/test_skeleton.py`, `tests/python/test_itch_writer.py`, `tests/python/test_make_fixture.py`, `tests/python/test_download_itch.py`, `tests/python/test_fixture.py` |
| Fixture | `tests/fixtures/synthetic.json`, `tests/fixtures/synthetic.itch` |
| Reserved layout | `src/platform/.gitkeep`, `bench/.gitkeep` |
| Documentation | `README.md`, `docs/build.md`, `docs/data.md`, `docs/m1-review.md` |
| Evidence | `results/m1/debug-tests.txt`, `results/m1/asan-tests.txt`, `results/m1/lint.txt`, `results/m1/environment.json`, `results/m1/fixture.json`, `results/m1/ci.json` |

## Boundaries and approval

The downloader requires a filesystem with hard-link support to publish a completed
file without overwriting an existing destination. The synthetic writer checks
wire representation, not order lifecycle validity. Full-day replay, parser work,
native C++ platform adapters, and performance measurements have not started.

After approval, merge `m1` into `main` with a merge commit, push `main`, and confirm
its CI is green before closing M1. M2 has not started.
