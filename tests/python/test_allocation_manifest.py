import json
import subprocess

import pytest

from python.itch_writer import encode_events
from tests.run_allocation_probe import run_probe


@pytest.mark.parametrize("count", [0, 1, 3])
def test_probe_uses_manifest_count(parser_executable, tmp_path, count):
    fixture = tmp_path / "small.itch"
    fixture.write_bytes(encode_events([{"type": "S", "event_code": "O"}] * count))
    manifest = tmp_path / "small.manifest.json"
    manifest.write_text(json.dumps({"fixture_file": fixture.name, "messages": count}))
    executable = parser_executable.with_name("parser_allocation_test" + parser_executable.suffix)
    result = run_probe(executable, manifest)
    assert result.returncode == 0, result.stdout + result.stderr
    assert f"callbacks={count * 10}" in result.stdout
    assert "allocations=0" in result.stdout
    manifest.write_text(json.dumps({"fixture_file": fixture.name, "messages": count + 1}))
    assert run_probe(executable, manifest).returncode == 1


def test_probe_never_accepts_a_sanitizer_report(monkeypatch, tmp_path):
    manifest = tmp_path / "manifest.json"
    manifest.write_text(json.dumps({"fixture_file": "fixture.itch", "messages": 0}))
    monkeypatch.setattr(
        subprocess,
        "run",
        lambda *args, **kwargs: subprocess.CompletedProcess(
            [], 1, "", "ERROR: AddressSanitizer: heap-buffer-overflow"
        ),
    )
    with pytest.raises(RuntimeError, match="AddressSanitizer"):
        run_probe(tmp_path / "probe", manifest)
