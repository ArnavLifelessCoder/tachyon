"""Classify captured test outcomes; never infer a timeout from marker text alone."""

import json
import re
from pathlib import Path

SANITIZER = re.compile(
    r"(?:ERROR|SUMMARY|WARNING): (?:AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer)"
    r"|AddressSanitizer:\s*(?:DEADLYSIGNAL|CHECK failed)|runtime error:",
    re.IGNORECASE,
)
ASSERTION = re.compile(
    r"\[\s*FAILED\s*\]|(?:^|\n).*?\bFailure(?:\r?\n|$)|AssertionError|assertion.*failed",
    re.IGNORECASE,
)


def process_timeout(program, expected_status, stderr, stdout, seconds):
    """Expected status comes from the caller, never from the return marker."""
    output = stdout + "\n" + stderr
    return {
        "kind": "timeout",
        "program": program,
        "expected_status": expected_status,
        "seconds": seconds,
        "stderr": stderr,
        "stdout": stdout,
        "markers": [line for line in output.splitlines() if re.fullmatch(r"[\w-]+: main .+", line)],
    }


def completed_main(record):
    program = record.get("program")
    expected = record.get("expected_status")
    if not isinstance(program, str) or type(expected) is not int:
        return False
    lines = record.get("stderr", "").splitlines()
    entry = f"{program}: main entered"
    returning = f"{program}: main returning status={expected}"
    markers = [line for line in lines if line.startswith(f"{program}: main ")]
    return markers == [entry, returning]


def classify(suite, *, allow_known):
    output = suite.get("output", "")
    if suite["name"] == "python_tests":
        # Parametrized case IDs contain test data, including synthetic diagnostic text.
        # Failure records below still decide whether an assertion or timeout occurred.
        output = "\n".join(
            line
            for line in output.splitlines()
            if not re.fullmatch(r"\S+::.* PASSED(?:\s+\[\s*\d+%\])?\s*", line)
        )
    if suite.get("asan_reports") or SANITIZER.search(output) or ASSERTION.search(output):
        return "failure"
    if suite["status"] == "passed":
        return "passed"
    if not allow_known:
        return "failure"
    if suite["name"] == "python_tests":
        report = suite.get("pytest")
        if not report or report.get("exitstatus") != 1 or not report.get("failures"):
            return "failure"
        for failure in report["failures"]:
            text = failure.get("stdout", "") + "\n" + failure.get("stderr", "")
            if (
                failure.get("kind") != "timeout"
                or not completed_main(failure)
                or SANITIZER.search(text)
                or ASSERTION.search(text)
            ):
                return "failure"
        # A whole pytest timeout is not a completed session with child exit stalls.
        return "known_stall" if suite["status"] == "failed" else "failure"
    if suite["status"] != "timeout":
        return "failure"
    if re.search(r"^\[\s*PASSED\s*\]\s+[1-9]\d* tests?\.\s*$", output, re.MULTILINE):
        return "known_stall"
    # Non-GoogleTest CTest executables must declare their expected status.
    if completed_main(suite):
        return "known_stall"
    return "failure"


def run_suites(execute, report_path, *, allow_known):
    """Run all suites, then at most one retry of each qualifying failed suite."""
    report = {"passed": False, "allow_known_stall": allow_known, "attempts": []}
    names = None
    for attempt in (1, 2):
        suites = execute(names, attempt)
        for suite in suites:
            suite["classification"] = classify(suite, allow_known=allow_known)
            if attempt == 2 and suite["classification"] == "known_stall":
                suite["classification"] = "failure"
                suite["reason"] = "second consecutive stall; retry exhausted"
        report["attempts"].append(suites)
        outcomes = [suite["classification"] for suite in suites]
        report["passed"] = bool(suites) and all(value == "passed" for value in outcomes)
        Path(report_path).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        if report["passed"] or "failure" in outcomes or attempt == 2 or not suites:
            return report
        names = [suite["name"] for suite in suites if suite["classification"] == "known_stall"]
    return report
