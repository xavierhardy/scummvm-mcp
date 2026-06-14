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
uv run pytest            # unit tests + mock full-run for every game (no binary needed)

# Opt-in: the real end-to-end tests launch an actual ScummVM and drive each
# game's captured walkthrough to 100% of its goals (each opens a window). A game
# is skipped individually if its data/save is absent; point the per-game env var
# at the data folder if it isn't auto-found.
uv run pytest --run-real tests/test_games_real.py            # all games
MONKEY_DEMO_PATH=/path/to/monkey uv run pytest --run-real \
    tests/test_full_run_real_monkey.py                       # Monkey Island
```

Every registered game has both a **mock** full-run (`tests/test_games_mock.py`,
replaying scripted backend responses) and a **real** full-run
(`tests/test_games_real.py`, driving a live ScummVM); both replay the same
captured walkthrough from `tests/walkthroughs.py` and must reach 100% of the
game's goals. Monkey Island keeps its own dedicated pair
(`tests/mock_harness.py:MONKEY_CALLS` + `tests/test_full_run_real_monkey.py`).
Goals latch on a tool **call** (e.g. walking the plank, captured via `Goal.times`
for the three indistinguishable bounces; or Indy3's three-punch combo) or on what
a call **returned** (a new room, inventory item, or dialogue line).

## Run

```bash
# Real run: launch pi against the Monkey Island demo, slot 1, cap 80 MCP calls.
# Provider and model are one combined token: provider/model.
uv run python -m scummvm_bench \
    --harness pi --model openai/gpt-4o \
    --game monkey-ega-demo:1 --max-calls 80 \
    --workers 1 --work-type thread --out results.tsv

# Sweep several models, harnesses and games (the matrix loops model->harness->game->save).
uv run python -m scummvm_bench \
    --harness pi --harness none \
    --model openai/gpt-4o --model anthropic/claude-opus-4-8 \
    --game monkey-ega-demo:1 --workers 2 --work-type thread

# Attach mode: bring up the game + proxy and let an already-running agent drive.
uv run python -m scummvm_bench --harness none --game monkey-ega-demo:1

# Local model: download/load via LM Studio, register in pi, run, then unload+delete.
uv run python -m scummvm_bench --local --harness pi \
    --model local/gemma-3-4b --game monkey-ega-demo:1
```

The agent reaches the proxy through the
[`pi-mcp-adapter`](https://www.npmjs.com/package/pi-mcp-adapter) extension: the
bench writes a per-session `.mcp.json` (with a `url` server entry + `directTools`)
into pi's working directory.

## CLI

| Flag | Meaning |
|------|---------|
| `--harness {pi,none}` | Harness to drive the run (repeatable). `none` = attach after the fact. |
| `--model PROVIDER/MODEL` | Provider+model as one token, e.g. `openai/gpt-4o` (repeatable; split on the first `/`). Required for `pi`. |
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

## Games

Six demo goal sets ship in `scummvm_bench/goals/`, each with a mock + real
full-run that reaches 100%. The `pass` target (Passport to Adventure) carries two
of them, disambiguated by save slot.

| `--game` | Game | Segment | Goals |
|----------|------|---------|-------|
| `monkey-ega-demo:1` | The Secret of Monkey Island | Mêlée Island → troll / fortune teller | 29 |
| `maniac-c64:1` | Maniac Mansion | Get into the mansion | 6 |
| `comi-demo:1` | The Curse of Monkey Island | Cannon-room pirate dialog | 6 |
| `samnmax:1` | Sam & Max Hit the Road | Office → street → DeSoto | 9 |
| `dig-demo:1` | The Dig | Canyon → dias → wreck → bone field | 8 |
| `pass:2` | Loom | Tree clearing → village → dark clearing | 7 |
| `pass:3` | Indiana Jones and the Last Crusade | Boxing gym sparring match | 7 |
