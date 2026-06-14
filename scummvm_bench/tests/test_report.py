"""TSV writing and stdout table rendering."""

import csv

from scummvm_bench.goals.engine import Goal, GoalEvent, GoalSet, on_call
from scummvm_bench.models import RunSpec
from scummvm_bench.recorder import LimitConfig, Recorder
from scummvm_bench.report import render_table, write_tsv


def _result(reached_walk: bool):
    gs = GoalSet(
        "game",
        1,
        {
            "walked": Goal("walked", "Walked", on_call("walk"), kind="call"),
            "done": Goal("done", "Done", on_call("done"), stopping=True, kind="call"),
        },
    )
    rec = Recorder(gs, LimitConfig())
    if reached_walk:
        rec.record(GoalEvent("walk", {}, {}, {}, True), None)
    return rec.result(RunSpec("pi", "openai", "gpt-4o", "game", 1))


def test_write_tsv_header_and_rows(tmp_path) -> None:
    path = tmp_path / "out.tsv"
    write_tsv([_result(True)], str(path))
    with open(path) as handle:
        rows = list(csv.reader(handle, delimiter="\t"))
    header, data = rows[0], rows[1]
    assert header[:5] == ["harness", "provider", "model", "game", "save_slot"]
    assert "walked" in header and "done" in header
    record = dict(zip(header, data, strict=True))
    assert record["harness"] == "pi"
    assert record["model"] == "gpt-4o"
    assert record["walked"] == "1"
    assert record["done"] == "0"
    assert record["reached"] == "1"
    assert record["total"] == "2"
    assert record["score_pct"] == "50.0"


def test_render_table_contains_summary() -> None:
    text = render_table([_result(True)])
    assert "harness" in text
    assert "score%" in text
    assert "1/2" in text  # reached/total
    assert "Goals:" in text
    assert "walked" in text


def test_render_table_empty() -> None:
    # No results: still renders headers without crashing.
    text = render_table([])
    assert "harness" in text


def test_tsv_multiple_rows_union_goal_columns(tmp_path) -> None:
    path = tmp_path / "out.tsv"
    write_tsv([_result(True), _result(False)], str(path))
    with open(path) as handle:
        rows = list(csv.reader(handle, delimiter="\t"))
    assert len(rows) == 3  # header + 2
