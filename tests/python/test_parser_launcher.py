import subprocess

import pytest

from python import parse_file


def test_single_timeout_is_short_and_identifies_input(monkeypatch, tmp_path):
    source = tmp_path / "seed-123.itch"
    calls = []

    def hang(command, **kwargs):
        calls.append(command)
        assert kwargs["timeout"] == 20
        kwargs["stderr"].write(b"itch_parse: main entered\n")
        kwargs["stderr"].flush()
        raise subprocess.TimeoutExpired(command, kwargs["timeout"])

    monkeypatch.setattr(subprocess, "run", hang)
    with pytest.raises(subprocess.TimeoutExpired) as info:
        parse_file.run_parser(source, tmp_path / "parser")
    assert len(calls) == 1
    assert str(source) in "\n".join(info.value.__notes__)
    assert b"main entered" in info.value.stderr


def test_batch_timeout_identifies_active_seed_without_retry(monkeypatch, tmp_path):
    sources = [tmp_path / f"seed-{seed}.itch" for seed in range(200)]
    calls = []

    def hang(command, **kwargs):
        calls.append(command)
        assert kwargs["timeout"] == 20
        assert len(kwargs["input"].splitlines()) == 200
        kwargs["stderr"].write(b"itch_parse: main entered\nitch_parse: input index=17\n")
        kwargs["stderr"].flush()
        raise subprocess.TimeoutExpired(command, kwargs["timeout"])

    monkeypatch.setattr(subprocess, "run", hang)
    with pytest.raises(subprocess.TimeoutExpired) as info:
        parse_file.run_parser_batch(sources, tmp_path / "parser", events=True)
    assert len(calls) == 1
    assert "seed-17.itch" in "\n".join(info.value.__notes__)
    assert b"input index=17" in info.value.stderr


def test_sanitizer_report_blocks_even_if_child_returns_expected_status(monkeypatch, tmp_path):
    def report(command, **kwargs):
        kwargs["stderr"].write(b"ERROR: AddressSanitizer: heap-buffer-overflow\n")
        return subprocess.CompletedProcess(command, 1)

    monkeypatch.setattr(subprocess, "run", report)
    with pytest.raises(RuntimeError, match="AddressSanitizer"):
        parse_file.run_parser(tmp_path / "missing", tmp_path / "parser", expected_status=1)
