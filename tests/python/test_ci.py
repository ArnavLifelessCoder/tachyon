"""Guard against artifact collisions between compiler matrix jobs."""

import json
import re
import subprocess
from pathlib import Path


def test_test_matrix_artifacts_have_unique_names():
    workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/ci.yml").read_text(
        encoding="utf-8"
    )
    entries = re.findall(r"- \{ os: ([\w-]+), compiler: ([\w+]+), preset: (\w+) \}", workflow)
    template = re.search(r"name: (tests-.+)", workflow).group(1)
    names = []
    assert len(entries) == 6
    for os, compiler, preset in entries:
        name = template
        for key, value in {"os": os, "compiler": compiler, "preset": preset}.items():
            name = name.replace("${{ matrix." + key + " }}", value)
        assert "${{" not in name
        names.append(name)
    assert len(names) == len(set(names)), f"Artifact names collide: {names}"


def test_native_suites_use_one_launch_each(parser_executable):
    build = parser_executable.parent
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    ctest = re.search(r"^CMAKE_CTEST_COMMAND:INTERNAL=(.+)$", cache, re.MULTILINE).group(1)
    result = subprocess.run(
        [ctest, "--test-dir", str(build), "--show-only=json-v1"],
        capture_output=True,
        text=True,
        timeout=20,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    tests = json.loads(result.stdout)["tests"]
    allocation = next(test for test in tests if test["name"] == "parser_allocations")
    properties = {prop["name"]: prop["value"] for prop in allocation["properties"]}
    assert properties.get("TIMEOUT") == 60, "Allocation probe needs a bounded CTest deadline"
    for executable in ("me_tests", "parser_tests", "mapping_tests"):
        commands = [
            test["command"] for test in tests if Path(test["command"][0]).stem == executable
        ]
        assert len(commands) == 1, f"{executable}: {len(commands)} process launches"
        assert len(commands[0]) == 1, f"{executable}: expected the entire suite, without a filter"
