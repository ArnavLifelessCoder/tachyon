import json

import pytest

from scripts.stall_policy import classify, process_timeout, run_suites

ENTER = "itch_parse: main entered"
RETURN = "itch_parse: main returning status=0"


def timeout(stderr=f"{ENTER}\n{RETURN}\n", expected=0):
    return process_timeout("itch_parse", expected, stderr, "", 20)


def suite(failures=None, **kwargs):
    return {
        "name": "python_tests",
        "status": "failed",
        "seconds": 24,
        "output": "",
        "pytest": {"exitstatus": 1, "failures": failures or [timeout()]},
        **kwargs,
    }


def test_qualified_child_timeout():
    assert classify(suite(), allow_known=True) == "known_stall"
    assert classify(suite(), allow_known=False) == "failure"


@pytest.mark.parametrize(
    "stderr,expected",
    [
        (ENTER, 0),
        (RETURN, 0),
        (f"{ENTER}\n{RETURN}", 2),
        (f"{RETURN}\n{ENTER}", 0),
        (f"{ENTER}\n{RETURN}\nERROR: AddressSanitizer: heap-buffer-overflow", 0),
        (f"{ENTER}\n{RETURN}\nSUMMARY: AddressSanitizer: heap-buffer-overflow", 0),
        (f"{ENTER}\n{RETURN}\nruntime error: invalid load", 0),
        (f"{ENTER}\n{RETURN}\n[  FAILED  ] case", 0),
    ],
)
def test_unqualified_timeout_blocks(stderr, expected):
    assert classify(suite([timeout(stderr, expected)]), allow_known=True) == "failure"


def test_nonzero_expected_status_must_match():
    record = timeout(f"{ENTER}\nitch_parse: main returning status=2", 2)
    assert classify(suite([record]), allow_known=True) == "known_stall"


@pytest.mark.parametrize(
    "extra",
    [
        {"kind": "failure", "message": "assert counts == expected"},
        {"kind": "timeout", "program": "itch_parse"},
    ],
)
def test_mixed_failures_block_retry(extra):
    assert classify(suite([timeout(), extra]), allow_known=True) == "failure"


@pytest.mark.parametrize("report", [None, {}, {"exitstatus": 3, "failures": []}])
def test_missing_or_incomplete_pytest_report_blocks(report):
    assert classify(suite(pytest=report), allow_known=True) == "failure"


def test_google_test_requires_timeout_and_passed_summary():
    native = {
        "name": "parser_tests",
        "status": "timeout",
        "seconds": 60,
        "output": "[==========] 34 tests ran.\n[  PASSED  ] 34 tests.\n",
    }
    assert classify(native, allow_known=True) == "known_stall"
    assert classify({**native, "status": "failed"}, allow_known=True) == "failure"
    for bad in ("[  FAILED  ] case", "ERROR: AddressSanitizer: bad-free", "Failure\n"):
        assert classify({**native, "output": native["output"] + bad}, allow_known=True) == "failure"
    assert classify({**native, "output": ""}, allow_known=True) == "failure"


def test_report_in_external_asan_log_blocks_even_a_pass():
    passed = {
        "name": "parser_tests",
        "status": "passed",
        "seconds": 1,
        "output": "[  PASSED  ] 34 tests.",
        "asan_reports": ["asan.123: ERROR: AddressSanitizer: heap-use-after-free"],
    }
    assert classify(passed, allow_known=True) == "failure"


def test_other_native_executable_requires_matching_status():
    native = {
        "name": "book_probe",
        "status": "timeout",
        "output": "",
        **process_timeout(
            "book_probe", 0, "book_probe: main entered\nbook_probe: main returning status=0", "", 20
        ),
    }
    assert classify(native, allow_known=True) == "known_stall"
    native["expected_status"] = 1
    assert classify(native, allow_known=True) == "failure"


def test_retry_exactly_the_affected_suite_and_preserve_evidence(tmp_path):
    calls = []

    def execute(names, attempt):
        calls.append((names, attempt))
        if attempt == 1:
            return [
                {"name": "parser_tests", "status": "passed", "output": "", "seconds": 1},
                suite(),
            ]
        return [{"name": "python_tests", "status": "passed", "output": "", "seconds": 2}]

    report = run_suites(execute, tmp_path / "report.json", allow_known=True)
    assert report["passed"]
    assert calls == [(None, 1), (["python_tests"], 2)]
    saved = json.loads((tmp_path / "report.json").read_text())
    assert saved["attempts"][0][1]["classification"] == "known_stall"
    assert saved["attempts"][0][1]["pytest"]["failures"][0]["markers"] == [ENTER, RETURN]


@pytest.mark.parametrize("second", ["timeout", "failed"])
def test_second_stall_or_failure_blocks_without_third_attempt(tmp_path, second):
    calls = []

    def execute(names, attempt):
        calls.append(attempt)
        return [suite() if second == "timeout" or attempt == 1 else suite([{"kind": "failure"}])]

    report = run_suites(execute, tmp_path / "report.json", allow_known=True)
    assert not report["passed"]
    assert calls == [1, 2]


def test_assertion_anywhere_blocks_all_retries(tmp_path):
    calls = []

    def execute(names, attempt):
        calls.append(attempt)
        return [suite(), {"name": "parser_tests", "status": "failed", "output": "assertion"}]

    assert not run_suites(execute, tmp_path / "report.json", allow_known=True)["passed"]
    assert calls == [1]
