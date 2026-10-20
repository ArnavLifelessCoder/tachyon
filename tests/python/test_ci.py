"""Guard against artifact collisions between compiler matrix jobs."""

import re
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
