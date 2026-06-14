# scummvm_bench

A self-contained MCP **proxy** that sits between an AI agent harness and the
ScummVM MCP server, to benchmark different LLMs/harnesses at *playing* ScummVM
adventure games.

The proxy (built with [FastMCP](https://github.com/jlowin/fastmcp)) forwards
every tool call to a real (or mocked) ScummVM backend, records every call and
failure, and scores progress against a per-game/per-save set of latching
**goals**. It can launch the harness itself (`pi`) or attach to one already
running, run combinations sequentially or in parallel, optionally manage a local
LM Studio model, and emit a TSV + a printed table.

## Architecture

```
harness (pi / external) ──MCP──▶ FastMCP proxy (bench_port) ──▶ ScummVM MCP (scummvm_port)
                                       │ records calls + failures, evaluates goals
                                       ▼
                                  Recorder ──▶ RunResult ──▶ TSV + table
```

The nested run matrix is: **model → harness → game → save-state**.

## Install / dev

```bash
cd scummvm_bench
uv sync
uv run ruff format . && uv run ruff check . && uv run ty check
uv run pytest            # unit tests + mock full-run (no ScummVM binary needed)
```

## Run

```bash
# Real run: launch pi against the Monkey Island demo, slot 1, cap 80 MCP calls.
uv run python -m scummvm_bench \
    --harness pi --provider openai --model gpt-4o \
    --game monkey-ega-demo:1 --max-calls 80 \
    --workers 1 --work-type thread --out results.tsv

# Attach mode: bring up the game + proxy and let an already-running agent drive.
uv run python -m scummvm_bench --harness none --game monkey-ega-demo:1

# Local model: download/load via LM Studio, register in pi, run, then unload+delete.
uv run python -m scummvm_bench --local --harness pi \
    --provider local --model gemma-3-4b --game monkey-ega-demo:1
```

The agent reaches the proxy through the
[`pi-mcp-adapter`](https://www.npmjs.com/package/pi-mcp-adapter) extension: the
bench writes a per-session `.mcp.json` (with a `url` server entry + `directTools`)
into pi's working directory.

## CLI

| Flag | Meaning |
|------|---------|
| `--harness {pi,none}` | Harness to drive the run (repeatable). `none` = attach after the fact. |
| `--provider` / `--model` | Provider/model id (repeatable; paired). Required for `pi`. |
| `--game GAME[:SLOT]` | ScummVM target id + optional save slot (repeatable). |
| `--save-folder PATH` | Override the folder holding `<game>/<game>.sNN`. |
| `--time-limit SEC` | Optional wall-clock budget (timer starts on the first MCP call). |
| `--max-calls N` | Optional MCP tool-call budget. |
| `--workers N` | 1 = sequential (no pool); >1 = parallel. |
| `--work-type {async,thread,process}` | Parallelism backend. |
| `--local` | Manage the model through LM Studio (download/load/unload/delete). |
| `--config PATH` | TOML config (CLI overrides it). |
| `--out PATH` | TSV output (default `scummvm_bench_results.tsv`). |

## Adding a game

1. Add an `scummvm_<id>.ini` template under `config/ini_templates/`
   (substitution keys `%(game_path)s`, `%(mcp_port)s`, `%(logfile)s`).
2. Add a goal module under `scummvm_bench/goals/` and register it in
   `goals/__init__.py`'s `GOAL_SETS` keyed by `(game_id, save_slot)`.
3. Add the game (path + save slot) to `config/bench.toml` or pass `--game`.

Goals are a `dict[goal_id -> Goal]`; each goal has a human-readable label and a
predicate that fires either on a tool **call** (in a given state) or on what a
call **returned**. Goals latch (reached once = reached forever); exactly one is a
**stopping** goal that ends the run. Score = % of goals reached.
