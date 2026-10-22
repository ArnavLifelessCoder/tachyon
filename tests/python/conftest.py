import importlib
import json
import os
import subprocess
import sys
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
    parser.addoption("--module-dir", help="directory containing the built me_py module")


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


@pytest.fixture(scope="session")
def me_py(request):
    """The pybind11 module, or a skip when this build has none (sanitizer presets)."""
    supplied = request.config.getoption("--module-dir")
    if supplied == "":
        pytest.skip("me_py is not built in this configuration (sanitizer build)")
    if supplied is None:
        default = Path(__file__).resolve().parents[2] / "build" / "debug"
        if not any(default.glob("me_py*")):
            pytest.skip("me_py is not built in this configuration; pass --module-dir")
        supplied = default
    sys.path.insert(0, str(Path(supplied).resolve()))
    return importlib.import_module("me_py")
