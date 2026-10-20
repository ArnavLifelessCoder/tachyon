"""Run CTest and retain classified evidence for the Windows sanitizer policy."""

import argparse
import json
import os
import re
import subprocess
import sys
import time
import uuid
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from scripts.stall_policy import SANITIZER, run_suites  # noqa: E402


def read_junit(path):
    suites = []
    for case in ET.parse(path).getroot().iter("testcase"):
        failure = case.find("failure")
        status = "passed"
        if failure is not None:
            status = "timeout" if failure.get("message") == "Timeout" else "failed"
        elif case.find("skipped") is not None or case.get("status") not in (None, "run"):
            status = "failed"
        suites.append(
            {
                "name": case.attrib["name"],
                "status": status,
                "seconds": float(case.get("time", 0)),
                "output": case.findtext("system-out", ""),
            }
        )
    return suites


def execute_ctest(root, preset, directory, names, attempt):
    prefix = directory / f"attempt-{attempt}"
    junit = prefix.with_suffix(".xml")
    pytest_report = prefix.with_suffix(".pytest.json")
    log = prefix.with_suffix(".txt")
    env = os.environ.copy()
    env["ME_PYTEST_REPORT"] = str(pytest_report)
    env["ITCH_PARSE_TRACE"] = "1"
    env["ME_TRACE"] = "1"
    asan_prefix = prefix.relative_to(root).as_posix() + "-asan"
    if preset == "asan":
        # Relative paths avoid a drive-letter colon in ASAN_OPTIONS.
        (root / "build" / preset / directory.relative_to(root)).mkdir(parents=True, exist_ok=True)
        options = env.get("ASAN_OPTIONS", "")
        env["ASAN_OPTIONS"] = (
            options + ":" if options else ""
        ) + f"verbosity=1:log_path={asan_prefix}"
    command = ["ctest", "--preset", preset, "--verbose", "--output-junit", str(junit)]
    if names is not None:
        command += ["-R", "^(" + "|".join(re.escape(name) for name in names) + ")$"]
    inventory = subprocess.run(
        ["ctest", "--preset", preset, "--show-only=json-v1"],
        cwd=root,
        capture_output=True,
        text=True,
        timeout=20,
        check=True,
    )
    registered = {test["name"]: test for test in json.loads(inventory.stdout)["tests"]}
    started = time.perf_counter()
    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run(command, cwd=root, env=env, stdout=stream, stderr=subprocess.STDOUT)
    elapsed = time.perf_counter() - started
    print(log.read_text(encoding="utf-8"), end="", flush=True)
    try:
        suites = read_junit(junit)
    except (OSError, ET.ParseError, ValueError, KeyError):
        suites = []
    if not suites:
        suites = [
            {"name": "ctest", "status": "failed", "output": "Missing or invalid CTest report"}
        ]
    # A CTest infrastructure error must not be mistaken for a successful run.
    if result.returncode not in (0, 8) or (
        result.returncode and all(suite["status"] == "passed" for suite in suites)
    ):
        suites.append({"name": "ctest", "status": "failed", "output": f"exit={result.returncode}"})
    if names is not None and sorted(s["name"] for s in suites) != sorted(names):
        suites.append(
            {"name": "ctest_selection", "status": "failed", "output": "Suite set changed"}
        )
    asan_logs = []
    asan_reports = []
    for base in (root, root / "build" / preset):
        for path in base.glob(asan_prefix + "*"):
            if path.is_file():
                asan_logs.append(path.relative_to(root).as_posix())
                text = path.read_text(encoding="utf-8", errors="replace")
                if SANITIZER.search(text):
                    asan_reports.append(path.relative_to(root).as_posix())
    for suite in suites:
        test = registered.get(suite["name"], {})
        properties = {prop["name"]: prop["value"] for prop in test.get("properties", [])}
        if test.get("command") and not properties.get("WILL_FAIL", False):
            suite.update(
                program=Path(test["command"][0]).stem,
                expected_status=0,
                stderr=suite["output"],
            )
        suite.update(
            invocation_seconds=elapsed,
            log=log.relative_to(root).as_posix(),
            asan_logs=asan_logs,
            asan_log_patterns=[asan_prefix + ".*", f"build/{preset}/{asan_prefix}.*"],
            asan_reports=asan_reports,
            markers=re.findall(r"^(?:[\w-]+: main .+|\[\s*PASSED\s*\].+)$", suite["output"], re.M),
        )
        if suite["name"] == "python_tests":
            try:
                suite["pytest"] = json.loads(pytest_report.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                suite["pytest"] = None
    return suites


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preset", choices=["debug", "asan", "release"], required=True)
    args = parser.parse_args()
    stamp = time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:8]
    directory = ROOT / ".local/review" / f"{args.preset}-{stamp}"
    directory.mkdir(parents=True)
    report_path = directory / "report.json"

    def execute(names, attempt):
        return execute_ctest(ROOT, args.preset, directory, names, attempt)

    report = run_suites(
        execute, report_path, allow_known=sys.platform == "win32" and args.preset == "asan"
    )
    lines = ["# Test policy evidence", ""]
    for attempt, suites in enumerate(report["attempts"], 1):
        for suite in suites:
            lines.append(
                f"- Attempt {attempt}, {suite['name']}: {suite['classification']}; "
                f"{suite.get('seconds', 0):.2f} s. Log: `{suite['log']}`."
            )
            for marker in suite["markers"]:
                lines.append(f"  - `{marker}`")
            for failure in (suite.get("pytest") or {}).get("failures", []):
                lines.append(f"  - {failure.get('nodeid')}: {failure.get('seconds', 0)} s")
                for marker in failure.get("markers", []):
                    lines.append(f"    - `{marker}`")
            for pattern in suite["asan_log_patterns"]:
                lines.append(f"  - ASan log path: `{pattern}`")
    lines += ["", f"Policy result: {'PASS' if report['passed'] else 'FAIL'}", ""]
    (directory / "review.md").write_text("\n".join(lines), encoding="utf-8")
    latest = ROOT / ".local/review" / f"{args.preset}-policy-latest.json"
    latest.write_text(json.dumps({"report": str(report_path), **report}, indent=2) + "\n")
    print(f"Policy evidence: {report_path.relative_to(ROOT)}")
    print(f"Policy result: {'PASS' if report['passed'] else 'FAIL'}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
