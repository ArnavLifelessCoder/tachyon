import json
import os
import re
import subprocess
import sys
from pathlib import Path

from scripts.run_test_policy import execute_ctest, read_junit
from scripts.stall_policy import run_suites

ROOT = Path(__file__).resolve().parents[2]


def test_ctest_timeout_schema(tmp_path, parser_executable):
    # Use real CTest output to guard against guessing its JUnit timeout schema.
    (tmp_path / "CTestTestfile.cmake").write_text(
        f'add_test(stall "{Path(sys.executable).as_posix()}" "-c" '
        "\"import time; print('[  PASSED  ] 1 test.', flush=True); time.sleep(30)\")\n"
        "set_tests_properties(stall PROPERTIES TIMEOUT 5)\n",
        encoding="utf-8",
    )
    output = tmp_path / "tests.xml"
    cache = (parser_executable.parent / "CMakeCache.txt").read_text(encoding="utf-8")
    ctest = re.search(r"^CMAKE_CTEST_COMMAND:INTERNAL=(.+)$", cache, re.MULTILINE).group(1)
    result = subprocess.run(
        [ctest, "--test-dir", str(tmp_path), "--output-junit", str(output)],
        capture_output=True,
        timeout=20,
    )
    assert result.returncode == 8
    suites = read_junit(output)
    assert len(suites) == 1
    assert suites[0]["status"] == "timeout"
    assert "[  PASSED  ] 1 test." in suites[0]["output"]


def test_pytest_report_keeps_timeout_and_assertion_distinct(tmp_path):
    source = tmp_path / "test_outcomes.py"
    source.write_text(
        "import subprocess\n"
        "from scripts.stall_policy import process_timeout\n"
        "def test_timeout():\n"
        "    error = subprocess.TimeoutExpired(['itch_parse'], 20)\n"
        "    error.me_timeout = process_timeout('itch_parse', 2, "
        "'itch_parse: main entered\\nitch_parse: main returning status=2\\n', '', 20)\n"
        "    raise error\n"
        "def test_assertion():\n"
        "    assert 1 == 2\n",
        encoding="utf-8",
    )
    report = tmp_path / "outcomes.json"
    env = {**os.environ, "ME_PYTEST_REPORT": str(report)}
    result = subprocess.run(
        [
            sys.executable,
            "-m",
            "pytest",
            "-p",
            "tests.python.conftest",
            str(source),
            "-q",
            "-o",
            f"cache_dir={tmp_path / 'cache'}",
            "--basetemp",
            str(tmp_path / "temp"),
        ],
        cwd=ROOT,
        env=env,
        capture_output=True,
        timeout=20,
    )
    assert result.returncode == 1, result.stdout + result.stderr
    data = json.loads(report.read_text())
    assert data["exitstatus"] == 1
    assert [item["kind"] for item in data["failures"]] == ["timeout", "failure"]
    assert data["failures"][0]["expected_status"] == 2


def test_real_ctest_retries_only_completed_stall(tmp_path, parser_executable, monkeypatch):
    cache = (parser_executable.parent / "CMakeCache.txt").read_text(encoding="utf-8")
    ctest = Path(re.search(r"^CMAKE_CTEST_COMMAND:INTERNAL=(.+)$", cache, re.MULTILINE).group(1))
    monkeypatch.setenv("PATH", str(ctest.parent) + os.pathsep + os.environ["PATH"])
    build = tmp_path / "build/asan"
    build.mkdir(parents=True)
    (tmp_path / "CMakePresets.json").write_text(
        json.dumps(
            {
                "version": 2,
                "configurePresets": [
                    {"name": "asan", "generator": "Ninja", "binaryDir": "${sourceDir}/build/asan"}
                ],
                "testPresets": [{"name": "asan", "configurePreset": "asan"}],
            }
        )
    )
    child = tmp_path / "child.py"
    child.write_text(
        "from pathlib import Path\n"
        "import time\n"
        "print('[  PASSED  ] 1 test.', flush=True)\n"
        "sentinel = Path('first-attempt')\n"
        "if not sentinel.exists():\n"
        "    sentinel.touch()\n"
        "    time.sleep(30)\n"
    )
    (build / "CTestTestfile.cmake").write_text(
        f'add_test(stall "{Path(sys.executable).as_posix()}" "{child.as_posix()}")\n'
        "set_tests_properties(stall PROPERTIES TIMEOUT 5)\n"
        f'add_test(peer "{Path(sys.executable).as_posix()}" "-c" "print(\'ok\')")\n'
    )
    directory = tmp_path / ".local/review/test"
    directory.mkdir(parents=True)

    def execute(names, attempt):
        return execute_ctest(tmp_path, "asan", directory, names, attempt)

    report = run_suites(execute, directory / "report.json", allow_known=True)
    assert report["passed"], report
    assert len(report["attempts"]) == 2
    assert [suite["name"] for suite in report["attempts"][1]] == ["stall"]
    assert report["attempts"][0][0]["classification"] == "known_stall"
    assert (directory / "attempt-1.txt").exists()
    assert (directory / "attempt-2.txt").exists()
