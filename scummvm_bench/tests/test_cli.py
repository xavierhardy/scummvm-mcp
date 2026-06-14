"""CLI argument parsing and matrix building (no real launches)."""

import pytest

from scummvm_bench.cli import (
    build_parser,
    parse_game,
    parse_model,
    resolve_games,
    selection_from_args,
)
from scummvm_bench.config import BenchConfig
from scummvm_bench.models import GameSpec
from scummvm_bench.orchestrator import build_matrix


def _config() -> BenchConfig:
    return BenchConfig(
        games={
            "monkey-ega-demo": GameSpec(
                "monkey-ega-demo", "/games/monkey", save_slot=1
            ),
        }
    )


def test_parse_game() -> None:
    assert parse_game("monkey-ega-demo:2") == ("monkey-ega-demo", 2)
    assert parse_game("monkey-ega-demo") == ("monkey-ega-demo", None)


def test_resolve_games_fills_slot_from_config() -> None:
    games = resolve_games(["monkey-ega-demo"], _config())
    assert games == [("monkey-ega-demo", 1)]
    games = resolve_games(["monkey-ega-demo:5"], _config())
    assert games == [("monkey-ega-demo", 5)]


def test_parse_model_combined_token() -> None:
    assert parse_model("openai/gpt-4o") == ("openai", "gpt-4o")
    # model id may itself contain slashes; split on the first one only
    assert parse_model("local/google/gemma-3-4b") == ("local", "google/gemma-3-4b")


def test_parse_model_requires_slash() -> None:
    with pytest.raises(ValueError):
        parse_model("gpt-4o")


def test_selection_pi_requires_models() -> None:
    args = build_parser().parse_args(["--harness", "pi", "--game", "monkey-ega-demo"])
    with pytest.raises(ValueError):
        selection_from_args(args, _config())


def test_selection_rejects_model_without_provider() -> None:
    args = build_parser().parse_args(["--model", "gpt-4o", "--game", "monkey-ega-demo"])
    with pytest.raises(ValueError):
        selection_from_args(args, _config())


def test_selection_defaults_to_none_harness() -> None:
    args = build_parser().parse_args(["--game", "monkey-ega-demo"])
    selection = selection_from_args(args, _config())
    assert selection.harnesses == ["none"]
    assert selection.models == []


def test_build_matrix_nesting_model_outermost() -> None:
    args = build_parser().parse_args(
        [
            "--harness",
            "pi",
            "--model",
            "p1/m1",
            "--model",
            "p2/m2",
            "--game",
            "monkey-ega-demo:1",
            "--game",
            "monkey-ega-demo:2",
            "--max-calls",
            "40",
        ]
    )
    selection = selection_from_args(args, _config())
    specs = build_matrix(selection)
    assert len(specs) == 4  # 2 models x 1 harness x 2 games
    # provider/model round-trip into the spec
    assert (specs[0].provider, specs[0].model) == ("p1", "m1")
    # model is the outermost loop
    assert (specs[0].model, specs[1].model) == ("m1", "m1")
    assert specs[2].model == "m2"
    assert all(s.max_calls == 40 for s in specs)


def test_build_matrix_multiple_harnesses_and_games() -> None:
    args = build_parser().parse_args(
        [
            "--harness",
            "pi",
            "--harness",
            "none",
            "--model",
            "openai/gpt-4o",
            "--game",
            "monkey-ega-demo:1",
        ]
    )
    selection = selection_from_args(args, _config())
    specs = build_matrix(selection)
    # 1 model x 2 harnesses x 1 game
    assert len(specs) == 2
    assert {s.harness for s in specs} == {"pi", "none"}


def test_game_argument_required() -> None:
    with pytest.raises(SystemExit):
        build_parser().parse_args(["--harness", "none"])
