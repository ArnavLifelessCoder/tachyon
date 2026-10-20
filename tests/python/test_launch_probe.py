import subprocess

from scripts import probe_parser_launches


def test_probe_stops_at_timeout_and_records_missing_return(monkeypatch, tmp_path):
    calls = []

    def run(source, executable):
        calls.append(source)
        if len(calls) == 2:
            raise subprocess.TimeoutExpired(
                [str(executable)], 20, stderr=b"itch_parse: main entered\n"
            )
        return subprocess.CompletedProcess(
            [], 0, "", "itch_parse: main entered\nitch_parse: main returning status=0\n"
        )

    monkeypatch.setattr(probe_parser_launches, "run_parser", run)
    records = probe_parser_launches.probe(tmp_path / "input", tmp_path / "parser", 100)
    assert len(records) == len(calls) == 2
    assert records[0]["entered_main"] and records[0]["returning_main"]
    assert records[1]["status"] == "timeout"
    assert records[1]["entered_main"] and not records[1]["returning_main"]


def test_probe_records_pre_main_timeout(monkeypatch, tmp_path):
    def run(source, executable):
        raise subprocess.TimeoutExpired([str(executable)], 20, stderr=b"")

    monkeypatch.setattr(probe_parser_launches, "run_parser", run)
    records = probe_parser_launches.probe(tmp_path / "input", tmp_path / "parser", 100)
    assert len(records) == 1
    assert not records[0]["entered_main"] and not records[0]["returning_main"]
