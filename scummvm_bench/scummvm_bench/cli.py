"""Command-line interface: parse args, build the matrix, run, and report."""

import argparse

from .config import BenchConfig, load_config
from .orchestrator import MatrixSelection, Orchestrator, build_matrix
from .pool import WORK_TYPES, PoolConfig
from .report import render_table, write_tsv

DEFAULT_OUT = "scummvm_bench_results.tsv"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="scummvm-bench",
        description="Benchmark LLMs/harnesses against the ScummVM MCP server.",
    )
    parser.add_argument(
        "--harness",
        action="append",
        choices=["pi", "none"],
        help="Harness to drive the run (repeatable; default: none).",
    )
    parser.add_argument(
        "--model",
        action="append",
        metavar="PROVIDER/MODEL",
        help="Provider and model as one token, e.g. openai/gpt-4o (repeatable).",
    )
    parser.add_argument(
        "--game",
        action="append",
        required=True,
        metavar="GAME[:SLOT]",
        help="ScummVM target id and optional save slot (repeatable).",
    )
    parser.add_argument("--save-folder", help="Override the save-state folder.")
    parser.add_argument(
        "--time-limit", type=float, help="Wall-clock budget in seconds (optional)."
    )
    parser.add_argument(
        "--max-calls", type=int, help="MCP tool-call budget (optional)."
    )
    parser.add_argument(
        "--workers", type=int, default=1, help="1 = sequential; >1 = parallel."
    )
    parser.add_argument(
        "--work-type", choices=WORK_TYPES, default="thread", help="Parallelism backend."
    )
    parser.add_argument(
        "--local",
        action="store_true",
        help="Manage the model via LM Studio (download/load/unload/delete).",
    )
    parser.add_argument("--config", help="Path to a bench.toml config file.")
    parser.add_argument("--out", default=DEFAULT_OUT, help="TSV output path.")
    return parser


def parse_game(spec: str) -> tuple[str, int | None]:
    """Parse ``GAME[:SLOT]`` into ``(game_id, save_slot)``."""
    if ":" in spec:
        game_id, _, slot = spec.rpartition(":")
        return game_id, int(slot)
    return spec, None


def parse_model(token: str) -> tuple[str, str]:
    """Parse a ``provider/model`` token into ``(provider, model)``.

    Splits on the first ``/`` so model ids may themselves contain slashes
    (e.g. ``local/google/gemma-3-4b`` -> ``("local", "google/gemma-3-4b")``).
    """
    provider, sep, model = token.partition("/")
    if not sep or not provider or not model:
        raise ValueError(f"--model must be 'provider/model', got {token!r}")
    return provider, model


def resolve_games(
    game_args: list[str], config: BenchConfig
) -> list[tuple[str, int | None]]:
    """Resolve game args, filling missing save slots from the config defaults."""
    games: list[tuple[str, int | None]] = []
    for raw in game_args:
        game_id, slot = parse_game(raw)
        if slot is None:
            cfg = config.games.get(game_id)
            if cfg is not None:
                slot = cfg.save_slot
        games.append((game_id, slot))
    return games


def selection_from_args(
    args: argparse.Namespace, config: BenchConfig
) -> MatrixSelection:
    """Build a :class:`MatrixSelection` from parsed args + config (pure)."""
    model_pairs: list[tuple[str | None, str | None]] = [
        parse_model(token) for token in (args.model or [])
    ]

    harnesses = args.harness or ["none"]
    if "pi" in harnesses and not model_pairs:
        raise ValueError(
            "the 'pi' harness requires at least one --model provider/model"
        )

    return MatrixSelection(
        harnesses=harnesses,
        models=model_pairs,
        games=resolve_games(args.game, config),
        max_calls=args.max_calls,
        time_limit_s=args.time_limit,
        local=args.local,
    )


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    config = load_config(args.config)

    try:
        selection = selection_from_args(args, config)
    except ValueError as exc:
        parser.error(str(exc))

    specs = build_matrix(selection)
    orchestrator = Orchestrator(
        session_config=config.session_config(save_folder=args.save_folder),
        pool_config=PoolConfig(workers=args.workers, work_type=args.work_type),
        local=args.local,
        local_models=config.local_model_map(),
        lms_bin=config.lms_bin,
        models_json=config.models_json,
    )
    results = orchestrator.run(specs)

    write_tsv(results, args.out)
    print(render_table(results))
    print(f"\nWrote {len(results)} result(s) to {args.out}")
    return 1 if any(r.error for r in results) else 0


if __name__ == "__main__":
    raise SystemExit(main())
