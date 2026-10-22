import json
import os
import subprocess
from pathlib import Path

import pytest


def pytest_configure(config):
    config.me_failures = []


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    report = (yield).get_result()
    if report.failed:
        error = call.excinfo.value if call.excinfo else None
        record = {"kind": "failure", "message": str(report.longrepr)}
        if isinstance(error, subprocess.TimeoutExpired):
            record = getattr(error, "me_timeout", record).copy()
        record.update(nodeid=item.nodeid, phase=report.when)
        item.config.me_failures.append(record)


def pytest_sessionfinish(session, exitstatus):
    destination = os.environ.get("ME_PYTEST_REPORT")
    if destination:
        Path(destination).write_text(
            json.dumps({"exitstatus": int(exitstatus), "failures": session.config.me_failures})
            + "\n",
            encoding="utf-8",
        )


def pytest_addoption(parser):
    parser.addoption("--parser", help="path to the built itch_parse executable")
    parser.addoption("--seed", type=int, default=20261020, help="differential-test base seed")


@pytest.fixture(scope="session")
def parser_executable(request):
    supplied = request.config.getoption("--parser")
    if supplied is None:
        root = Path(__file__).resolve().parents[2]
        supplied = root / "build" / "debug" / "itch_parse"
        if not supplied.exists():
            supplied = supplied.with_suffix(".exe")
    executable = Path(supplied).resolve()
    assert executable.is_file(), "Build itch_parse first or supply --parser"
    return executable
