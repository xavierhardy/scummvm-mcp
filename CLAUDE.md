# CLAUDE.md

Guidance for working in this repository. The work here is focused on the
**ScummVM MCP server** and the **MCP benchmark**; the rest of ScummVM is
upstream and mostly off-limits (see "Ground rules").

## What this repository is

This is a fork of [ScummVM](https://www.scummvm.org/) (a C++ engine that runs
classic point-and-click adventure games) that adds an **MCP (Model Context
Protocol) server** to the SCUMM engine, so an LLM/agent can *play* SCUMM games
through structured tool calls instead of pixels. Everything new lives in a few
places; treat the remainder of the tree as upstream ScummVM.

## What the MCP server does

When a SCUMM game runs with `mcp=true`, the engine hosts an MCP server (HTTP +
SSE on `127.0.0.1:23456` by default). It exposes the game as tools:

- **`state`** — current room, selectable objects/actors (by name), inventory,
  available verbs, pending dialog question, ego position. The semantic snapshot
  an agent reads each turn.
- **`act`** (verb + up to two targets), **`answer`** (dialog choice id),
  **`walk`** (x/y), **`skip`** — the streaming gameplay tools. They open an SSE
  channel, drive the engine, emit notifications (dialog lines, Loom notes) as
  the action plays out, then return a structured result (room/inventory/message
  changes).
- Game-specific helpers: `play_note`, `shoot_cannon`, `ride_bike`,
  `switch_character`, `dial`.
- **Debug tools** (only when `mcp_debug=true`): `debug` (read engine vars),
  `save_state`, `set_talk_speed`, `keystroke`, `mouse_move`, `mouse_click`,
  `screenshot`.

Verb/target names are normalized (aliases, case, the `@` name padding SCUMM
uses, and the V8 `/room.id/name` prefix) so an agent can echo back what `state`
showed it. The wire protocol is documented in `docs/protocols/mcp.json`.

## Architecture — MCP server (C++)

| Path | Role |
|------|------|
| `backends/networking/mcp/mcp_server.{h,cpp}` | Engine-agnostic MCP transport: TCP listener, HTTP framing, JSON-RPC 2.0 dispatch, SSE streaming, tool registry (`IToolHandler`), and pure string/JSON helpers (`mcpNormalizeSpaces`, `mcpSanitizeString`, `mcpJson*`). |
| `engines/scumm/mcp.{h,cpp}` | `ScummMcpBridge` — the SCUMM-specific `IToolHandler`. Builds the state snapshot, implements every tool, runs the per-frame streaming state machine, and contains the game/engine-version-specific logic. The large file. |
| `engines/scumm/mcp_actionname.cpp` | `normalizeActionName` + `mcpStripNamePadding`, deliberately kept free of `ScummEngine` so unit tests can link them without the whole engine. |

Enable / configure via the game's `scummvm.ini` `[gameid]` section:
`mcp=true`, `mcp_port=N`, `mcp_host=...`, `mcp_skip_tool=true`,
`mcp_debug=true`. Engine versions differ a lot (V0 Maniac C64 → V8 Curse of
Monkey Island); much of `mcp.cpp` branches on the engine version and the
specific game.

### MCP server tests

- **C++ unit tests** — `test/engines/scumm/mcp.h` (CxxTest). Covers the
  engine-independent helpers only (no running engine). Wired into `make test`
  via `test/module.mk` when `ENABLE_SCUMM=STATIC_PLUGIN`.
- **Python integration tests** — `test/mcp/`. Launch a real headless ScummVM
  per game and drive the MCP server over HTTP, asserting on game state. ~10
  games (V0–V8), plus pure-Python unit tests in `test/mcp/test_unit.py`. See
  `test/mcp/README.md` for the full game/fixture map.

## Architecture — MCP benchmark (`scummvm_bench/`)

A self-contained MCP **proxy** that benchmarks different LLMs/agent harnesses at
*playing* SCUMM games. It sits between an agent harness and the real ScummVM MCP
server, forwards every tool call, records calls/failures, and scores progress
against per-game latching **goals**. See `scummvm_bench/README.md`.

| Path | Role |
|------|------|
| `scummvm_bench/proxy.py` | FastMCP proxy between harness and the ScummVM MCP server. |
| `scummvm_bench/mcp_client.py` | A trimmed, vendored copy of the `test/mcp` client + launcher, so the package has **no dependency on the test tree** (keep it that way). |
| `scummvm_bench/goals/` | Per-game goal sets (predicates that latch on a tool call or its returned state); registered in `goals/__init__.py`'s `GOAL_SETS`. |
| `scummvm_bench/harness/`, `orchestrator.py`, `recorder.py`, `report.py` | Run matrix (model → harness → game → save), recording, TSV/table output. |

### Benchmark tests

`scummvm_bench/tests/` — every game with a captured walkthrough
(`tests/walkthroughs.py`) has a **mock** full-run (`test_games_mock.py`, scripted
backend) and a **real** full-run (`test_games_real.py`, live ScummVM), both
required to reach 100% of the game's goals.

## Python tooling (both `test/mcp` and `scummvm_bench`)

Use **`uv`** (install/run), **`ruff`** (lint + format, replaces black/isort), and
**`ty`** (`astral/ty`, type-check, replaces mypy). Keep tooling uniform across the
two projects. Typing rules: rely on `Any` as little as possible; never use
`Optional`/`Union` (`X | None`, `X | Y`); prefer `list`/`dict`/`tuple` over the
`typing` aliases. Preserve ruff's default formatting.

**Scope `ruff` and `ty` strictly to `test/mcp/` and `scummvm_bench/`.** Each owns
its own `pyproject.toml`; always run the tools from *inside* one of those two
directories (e.g. `cd scummvm_bench && uv run ruff format . && uv run ruff check .
&& uv run ty check`). Never run `ruff`/`ty` from the repo root or point them at
the rest of the tree — the surrounding ScummVM source is not ours to lint or
type-check, and doing so floods the output with upstream findings.

## Build & run tests

```bash
# Build the engine (produces ./scummvm). First time:
./configure --disable-all-engines --enable-engine=scumm --enable-engine=scumm-7-8
make

# C++ unit tests (CxxTest). Needs a `python` on PATH for cxxtestgen — if only
# python3 exists, create a venv to provide one:  uv venv .venv && PATH=.venv/bin:$PATH make test
make test

# Python — MCP server integration + unit tests
cd test/mcp && uv sync
uv run pytest                 # game tests skip when data is absent
uv run pytest test_unit.py    # pure unit tests, no game needed
uv run ruff format . && uv run ruff check . && uv run ty check

# Python — MCP benchmark
cd scummvm_bench && uv sync
uv run pytest                 # unit + mock full-runs (no binary needed)
uv run pytest --run-real tests/test_games_real.py   # drives a real ScummVM
uv run ruff format . && uv run ruff check . && uv run ty check
```

Game data is not in the repo. Tests look for it via per-game env vars and skip
when it is missing (see `test/mcp/README.md`).

## Debugging the MCP server

- **Engine debug log:** run with `--debugflags=mcp --debuglevel=11` to trace
  MCP server activity. The integration-test inis also set `logfile=` +
  `debuglevel=11`.
- **Debug tools** (set `mcp_debug=true` in the game's ini): use `debug` to read
  engine globals (`VAR(N)`), `mouse_move`/`mouse_click` to drive the cursor
  directly (e.g. to reach UI a verb can't), `keystroke` for raw keys,
  `screenshot` to capture the framebuffer (PNG to the game's `screenshotpath`),
  and `set_talk_speed` to force text speed.
- **`test/mcp/mcp_cli.py`** — an interactive REPL / one-shot CLI to call any MCP
  tool against a running instance, and to auto-launch a game on a save:

  ```bash
  cd test/mcp
  uv run python mcp_cli.py --launch pass --port 23462          # launch + REPL
  uv run python mcp_cli.py --port 23462 state                  # one-shot
  uv run python mcp_cli.py --port 23462 mouse_click 140 134 --double
  uv run python mcp_cli.py --port 23462 debug --vars 250-263
  ```

## Decompiling a game (recovering exact mechanics)

When a tool needs the game's real logic (ballistics, aim models, room
transitions, state flags), decompile its SCUMM bytecode instead of probing
blindly:

1. Build `descumm` from the separate `scummvm-tools` repo (one-off).
2. Add `dump_scripts=true` to the game's `scummvm.ini` `[gameid]` section and
   load a save in the room of interest — the engine writes `./dumps/room-N-*.dmp`,
   `entry-N`, `exit-N`, `script-NN`. (Don't use `SDL_VIDEODRIVER=dummy` here; the
   game needs a real graphics mode to boot.)
3. `descumm -<ver> dumps/room-N.dmp` (e.g. `-8` for V8/COMI). Globals decode as
   `varNNN` → `VAR(NNN)`; arrays `arrayNNN[i]` → `readArray(NNN, 0, i)`.

## Ground rules

When working on the MCP server, its tests, or the MCP bench:

- **Stay in scope.** Limit changes to the MCP server (`backends/networking/mcp/`,
  `engines/scumm/mcp*.cpp/.h`), `test/mcp/`, and `scummvm_bench/`. Avoid editing
  the rest of the engine; touch shared engine code only when there is no
  alternative, and keep it minimal.
- **Do not break retro-compatibility.** A change made for one game must not break
  other games, other SCUMM engine versions, or upstream behaviour. The bridge is
  shared by every SCUMM game from V0 to V8 — gate game- or version-specific
  behaviour behind the appropriate version/game checks rather than changing
  common paths. When in doubt, prefer an additive, narrowly-scoped branch.
- Keep `scummvm_bench` free of any dependency on the `test/mcp` tree.

## Conventions & attribution

This repo follows the project's [`AI-GUIDELINES.md`](AI-GUIDELINES.md) — read it.
Key points for any change made here:

- **Code style.** C++ MUST match ScummVM's
  [formatting and naming conventions](https://wiki.scummvm.org/index.php/Code_Formatting_Conventions)
  and the surrounding file: tabs for indentation, `lowerCamelCase` methods,
  `_camelCase` members, K&R braces. New changes should read like upstream engine
  code. Python follows the `ruff`/`ty` rules above.
- **Licensing.** All code is GPLv3+; new C++ source files carry the standard
  ScummVM GPL header. Never paste in code that is not GPLv3+ compatible.
- **Test before submitting.** Run `make test` and the relevant pytest suites,
  and exercise your change. Only submit code you understand — no "vibe coding".
- **Commit messages.** Follow ScummVM's conventions and the prefix style already
  used here (`MCP:`, `MCP_BENCH:`, `SCUMM:` …). AI assistance MUST be disclosed
  with an `Assisted-by:` trailer — and **not** a `Co-Authored-By:` trailer. The
  human committer is the sole author; an AI agent must never be a (co-)author.

  ```
  Assisted-by: Claude:claude-opus-4-8
  ```
- **No AI for art.** Never generate media assets (logos, icons, sprites, audio).
