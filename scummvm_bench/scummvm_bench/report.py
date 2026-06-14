"""Render benchmark results as a TSV artifact and a printable stdout table."""

import csv

from .models import RunResult

_BASE_COLUMNS = [
    "harness",
    "provider",
    "model",
    "game",
    "save_slot",
    "score_pct",
    "reached",
    "total",
    "calls",
    "failures",
    "elapsed_s",
    "stopped_by",
    "error",
]


def _goal_ids(results: list[RunResult]) -> list[str]:
    """Union of goal ids across results, preserving first-seen order."""
    ordered: list[str] = []
    seen: set[str] = set()
    for result in results:
        for goal in result.goals:
            if goal.goal_id not in seen:
                seen.add(goal.goal_id)
                ordered.append(goal.goal_id)
    return ordered


def _stopped_by(result: RunResult) -> str:
    if result.stopped_by_goal:
        return "goal"
    return result.stopped_by_limit or ""


def _base_row(result: RunResult) -> list[str]:
    spec = result.spec
    return [
        spec.harness,
        spec.provider or "",
        spec.model or "",
        spec.game_id,
        "" if spec.save_slot is None else str(spec.save_slot),
        f"{result.score_pct:.1f}",
        str(result.reached_count),
        str(result.total_goals),
        str(result.call_count),
        str(result.failure_count),
        f"{result.elapsed_s:.3f}",
        _stopped_by(result),
        result.error or "",
    ]


def write_tsv(results: list[RunResult], path: str) -> None:
    """Write ``results`` to ``path`` as TSV with one 0/1 column per goal."""
    goal_ids = _goal_ids(results)
    with open(path, "w", newline="") as handle:
        writer = csv.writer(handle, delimiter="\t")
        writer.writerow(_BASE_COLUMNS + goal_ids)
        for result in results:
            reached = {g.goal_id: g.reached for g in result.goals}
            goal_cells = [
                ("1" if reached[gid] else "0") if gid in reached else ""
                for gid in goal_ids
            ]
            writer.writerow(_base_row(result) + goal_cells)


def render_table(results: list[RunResult]) -> str:
    """Render a fixed-width summary table plus a compact per-goal grid."""
    headers = [
        "harness",
        "model",
        "game",
        "save",
        "score%",
        "goals",
        "calls",
        "fails",
        "elapsed",
        "stopped",
    ]
    rows: list[list[str]] = []
    for result in results:
        spec = result.spec
        rows.append(
            [
                spec.harness,
                spec.model or "-",
                spec.game_id,
                "-" if spec.save_slot is None else str(spec.save_slot),
                f"{result.score_pct:.1f}",
                f"{result.reached_count}/{result.total_goals}",
                str(result.call_count),
                str(result.failure_count),
                f"{result.elapsed_s:.2f}s",
                _stopped_by(result) or "-",
            ]
        )

    widths = [
        max(len(headers[i]), *(len(row[i]) for row in rows))
        if rows
        else len(headers[i])
        for i in range(len(headers))
    ]

    def fmt(cells: list[str]) -> str:
        return "  ".join(cell.ljust(widths[i]) for i, cell in enumerate(cells))

    lines = [fmt(headers), fmt(["-" * w for w in widths])]
    lines += [fmt(row) for row in rows]

    grid = _goal_grid(results)
    if grid:
        lines.append("")
        lines.extend(grid)
    return "\n".join(lines)


def _goal_grid(results: list[RunResult]) -> list[str]:
    """A compact ✓/· grid: one legend then one marked row per result."""
    goal_ids = _goal_ids(results)
    if not goal_ids:
        return []
    legend = ["Goals:"]
    legend += [f"  [{i:02d}] {gid}" for i, gid in enumerate(goal_ids, start=1)]
    label_width = max(len(r.spec.label) for r in results)
    grid: list[str] = []
    for result in results:
        reached = {g.goal_id: g.reached for g in result.goals}
        marks = "".join(
            ("✓" if reached.get(gid) else "·") if gid in reached else " "
            for gid in goal_ids
        )
        grid.append(f"{result.spec.label.ljust(label_width)}  {marks}")
    return legend + [""] + grid
