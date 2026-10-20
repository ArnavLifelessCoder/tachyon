from pathlib import Path

import pytest


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
