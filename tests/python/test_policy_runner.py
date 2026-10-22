import json
import os
import re
import subprocess
import sys
from pathlib import Path

from scripts.run_test_policy import execute_ctest, read_junit
from scripts.stall_policy import run_suites

ROOT = Path(__file__).resolve().parents[2]


def test_real_ctest_smoke(tmp_path, parser_executable):
    # Keep one real CTest launch; no intentional sleep or timeout race.
    (tmp_path / "CTestTestfile.cmake").write_text(
        f'add_test(smoke "{Path(sys.executable).as_posix()}" "-c" '
        "\"print('[  PASSED  ] 1 test.', flush=True)\")\n"
        "set_tests_properties(smoke PROPERTIES TIMEOUT 30)\n",
        encoding="utf-8",
    )
    output = tmp_path / "tests.xml"
    cache = (parser_executable.parent / "CMakeCache.txt").read_text(encoding="utf-8")
    ctest = re.search(r"^CMAKE_CTEST_COMMAND:INTERNAL=(.+)$", cache, re.MULTILINE).group(1)
    result = subprocess.run(
        [ctest, "--test-dir", str(tmp_path), "--output-junit", str(output)],
        capture_output=True,
        timeout=60,
    )
    assert result.returncode == 0
    suites = read_junit(output)
    assert len(suites) == 1
    assert suites[0]["status"] == "passed"
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


def test_ctest_retries_only_completed_stall(tmp_path, monkeypatch):
    # Canned process outcomes exercise the runner without timing interpreter launches.
    launches = []
    inventory = {
        "tests": [{"name": name, "command": [name], "properties": []} for name in ("stall", "peer")]
    }

    def fake_run(command, **kwargs):
        if "--show-only=json-v1" in command:
            return subprocess.CompletedProcess(command, 0, json.dumps(inventory))
        launches.append(command)
        first = len(launches) == 1
        failure = '<failure message="Timeout"/>' if first else ""
        status = "fail" if first else "run"
        peer = '<testcase name="peer" status="run" time="12"/>' if first else ""
        xml = (
            f'<testsuite><testcase name="stall" status="{status}" time="30">'
            f"{failure}<system-out>[  PASSED  ] 1 test.\n</system-out></testcase>"
            f"{peer}</testsuite>"
        )
        output = Path(command[command.index("--output-junit") + 1])
        output.write_text(xml, encoding="utf-8")
        kwargs["stdout"].write("first attempt timed out\n" if first else "retry passed\n")
        return subprocess.CompletedProcess(command, 8 if first else 0)

    monkeypatch.setattr(subprocess, "run", fake_run)
    directory = tmp_path / ".local/review/test"
    directory.mkdir(parents=True)

    def execute(names, attempt):
        return execute_ctest(tmp_path, "asan", directory, names, attempt)

    report = run_suites(execute, directory / "report.json", allow_known=True)
    assert report["passed"], report
    assert len(report["attempts"]) == len(launches) == 2
    assert "-R" not in launches[0]
    assert launches[1][-2:] == ["-R", "^(stall)$"]
    assert [suite["name"] for suite in report["attempts"][1]] == ["stall"]
    assert report["attempts"][0][0]["classification"] == "known_stall"
    assert (directory / "attempt-1.txt").read_text() == "first attempt timed out\n"
    assert (directory / "attempt-2.txt").read_text() == "retry passed\n"
