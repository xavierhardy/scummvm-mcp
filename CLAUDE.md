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
  `choose_kids`, `switch_character`, `dial` (the phone keypad, in both Maniac
  Mansion and Zak McKracken).
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
| `engines/mcp_bridge.{h,cpp}` | `MCP::McpBridge` — the engine-agnostic *bridge* base (one level up from transport). Owns `mcp*` config reading + server lifecycle, the captured-message queue, the frame counter, `callTool()` dispatch, `registerTools()` + `buildChangesSchema()`, the generic debug-tool plumbing, and the per-frame streaming state machine (timing budgets are virtuals so each engine can scale them). Shared by SCUMM, Broken Sword, Beneath a Steel Sky, Flight of the Amazon Queen and Woodruff (gob). |
| `engines/mcp_bridge_text.cpp` | `normalizeActionName` + the verb-alias table, `mcpStripNamePadding`, `mcpCleanGameText`, `mcpJsonKeyToKeyState` — deliberately free of `Engine` so both engines' unit tests link them without a running engine. |
| `engines/scumm/mcp.{h,cpp}` (+ `mcp_subclasses.h`, `mcp_v*/mcp_classic.cpp`) | `ScummMcpBridge : MCP::McpBridge` — the SCUMM adapter. Builds the state snapshot, implements every SCUMM tool, and holds the game/engine-version-specific logic. The large file. |
| `engines/scumm/mcp_actionname.cpp` | A shim: `Scumm::mcpStripNamePadding` forwards to the shared `MCP::` version (kept for the external symbol the SCUMM unit test forward-declares). |
| `engines/sword1/mcp.{h,cpp}` | `Sword1McpBridge : MCP::McpBridge` — the Broken Sword 1 adapter (one-click game; world coordinates; click injection replays `Mouse::engine()`). Written to extend to Broken Sword 2 unchanged. |
| `engines/sword1/mcp_names.{h,cpp}` | Broken Sword naming tables (pockets, symbolic compact ids, screens) + the `object_<section>_<index>` fallback resolver. Engine-free, so its unit test links without the engine. |
| `engines/sky/mcp.{h,cpp}` | `SkyMcpBridge : MCP::McpBridge` — the Beneath a Steel Sky adapter (two-button game; game/compact coordinates). Every action replays real input: warp the virtual cursor + press a button, and `Mouse::mouseEngine()` does the rest; inventory acts drive the game's own top icon bar through a per-frame click machine. Names come from the game data (cursorText / compact names). |
| `engines/sky/mcp_names.{h,cpp}` | BASS naming tables (talkable-character ids, screen names). Engine-free, so its unit test links without the engine. |
| `engines/queen/mcp.{h,cpp}` | `QueenMcpBridge : MCP::McpBridge` — the Flight of the Amazon Queen adapter (verb-panel game). `act` builds the finished command in the panel's own encoding and hands it to `Command::mcpExecute()`; dialogues answer through the digit-shortcut path `Talk::selectSentence()` already polls. |
| `engines/gob/mcp.{h,cpp}` | `GobMcpBridge : MCP::McpBridge` — the gob-engine adapter, covering two very different game shapes. **Woodruff**: script-driven one-click game; no verb bar, no object model — just rectangular `Hotspots`. Every action replays real cursor input through `Util::processInput`; a single left click is walk-to-then-default-action. Object/inventory names are harvested live from the status-bar text the game draws (`Draw_v2` DRAW_PRINTTEXT) by parking the cursor on each hotspot (name sweep) and by opening the game's own inventory overlay after each action. **Gobliiins** (`usesCharacterTeam()`, gated on `kGameTypeGob1`): no hover text and one screen-wide click zone, so the snapshot comes from the engine's own tables instead (`Goblin::_goblins[3]`, `_objects[]`, `_itemIndInPocket`); a click means whatever the *cursor* means, and the right button cycles that through the game's three settings (`VAR(111)`: 0 walk there, 3 use the character's ability, 4 take/put down — `Goblin::doMove()` reads it as the action for the click), so `act`/`walk` first right-click until the cursor matches the verb (`kStepCursorMode`) and then left-click the named object or the plain x/y; `switch_character` calls `Goblin::switchGoblin()`. `gameBusy()` reads the goblin's own `_pathExistence`/`_goesAtTarget`/`_readyToAct` so a stream closes only once the character has stopped. The name sweep and the inventory overlay machine are off there. Tools are registered from `onGameIdentified()` (called by `GobEngine::initGame`), not from the constructor: the bridge is built before the game is known, and the tool table depends on it. |
| `engines/gob/mcp_names.{h,cpp}` | gob naming helpers (hover-label → identifier, exit detection, TOT file → room name/id). Engine-free, so its unit test links without the engine. |

Enable / configure via the game's `scummvm.ini` `[gameid]` section:
`mcp=true`, `mcp_port=N`, `mcp_host=...`, `mcp_skip_tool=true`,
`mcp_debug=true` — read once in `MCP::McpBridge`'s constructor, so they work
identically for every engine. SCUMM engine versions differ a lot (V0 Maniac C64
→ V8 Curse of Monkey Island); much of `scumm/mcp.cpp` branches on the engine
version and the specific game. Per-engine tool semantics are documented under
`engineNotes` in `docs/protocols/mcp.json`.

**Adding a new engine**: subclass `MCP::McpBridge`, implement the pure-virtual
tools + streaming hooks, create it from the engine (bind the server *before* any
blocking GUI init — see how `SwordEngine` builds the bridge first thing in its
constructor and creates its debugger only after `initGraphics()`), pump it once
per game cycle, and pump-transport-only from any place the main loop stalls
(fades, cutscenes, modal panels).

### MCP server tests

- **C++ unit tests** — `test/engines/scumm/mcp.h`,
  `test/engines/sword1/mcp.h`, `test/engines/sky/mcp.h` and
  `test/engines/gob/mcp.h` (CxxTest). Cover
  the engine-independent helpers only (no running engine). Wired into
  `make test` via `test/module.mk` under the matching
  `ENABLE_<ENGINE>=STATIC_PLUGIN`; the shared `mcp_server.o` +
  `mcp_bridge_text.o` are linked once for whichever is enabled.
- **Python integration tests** — `test/mcp/`. Launch a real headless ScummVM
  per game and drive the MCP server over HTTP, asserting on game state. ~13
  SCUMM games (V0–V8) — mostly demos, plus four full games covered by
  compatibility smoke tests only (Zak McKracken `test_zak.py`, Monkey Island 2
  `test_monkey2.py`, Day of the Tentacle `test_tentacle.py`, and Maniac Mansion
  `test_maniac_full.py`, which starts fresh at the title screen) — plus
  Broken Sword 1 (`test_sword1.py`, engine `sword1`),
  Beneath a Steel Sky (`test_sky.py`, engine `sky`), Flight of the Amazon
  Queen (`test_queen.py`, engine `queen`) and Woodruff
  (`test_woodruff.py`, engine `gob` — no save support, so it starts fresh and
  skips the intro like the atlantis/ft demos) and Gobliiins
  (`test_gob1.py`, engine `gob`, same fresh-start pattern),
  and pure-Python unit tests in `test/mcp/test_unit.py`. See
  `test/mcp/README.md` for the full game/fixture map. (`launcher._SAVE_NAME_FMT`
  maps a game to its engine's save-file naming — sword1 uses `sword1.NNN`,
  sky uses `SKY-VM.NNN`.)

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
**`ty`** (`astral/ty`, type-check, replaces mypy). Typing rules: rely on `Any` as
little as possible; never use `Optional`/`Union` (`X | None`, `X | Y`); prefer
`list`/`dict`/`tuple` over the `typing` aliases. Preserve ruff's default formatting.

**Both test trees are ONE Python project** rooted at `scummvm_bench/pyproject.toml`
(one venv, one config). Its `[tool.pytest] testpaths` include `../test/mcp`, so a
single `uv run pytest` (optionally `-n N`) from `scummvm_bench/` runs both trees in
one xdist pool. Select a subset with `-o testpaths=...`:

```bash
cd scummvm_bench
export UV_CONFIG_FILE="$PWD/uv.toml" && uv sync
uv run --no-sync pytest -o testpaths=tests          # bench only
uv run --no-sync pytest -o testpaths=../test/mcp     # MCP server tests only
uv run --no-sync pytest --run-real -n auto           # both trees, one pool
```

**`ruff`/`ty` scope:** they only ever apply to this one project. Run them from
`scummvm_bench/` (`uv run ruff format . && uv run ruff check . && uv run ty check`).
Because `ruff`/`ty` discover config by walking *up* from each file and `test/mcp`
is a *sibling*, lint/type-check that tree with the explicit config:
`uv run ruff check --config "$PWD/pyproject.toml" ../test/mcp` and
`uv run ty check --project "$PWD" ../test/mcp`. Never run `ruff`/`ty` from the repo
root or point them at the surrounding ScummVM source (not ours to lint).

## Build & run tests

```bash
# Build the engine (produces ./scummvm). First time:
./configure --disable-all-engines --enable-engine=scumm --enable-engine=scumm-7-8 \
    --enable-engine=sword1 --enable-engine=sky --enable-engine=queen --enable-engine=gob
make

# C++ unit tests (CxxTest). Needs a `python` on PATH for cxxtestgen — if only
# python3 exists, create a venv to provide one:  uv venv .venv && PATH=.venv/bin:$PATH make test
make test

# Python — both suites share one project in scummvm_bench (testpaths cover
# ../test/mcp). Game tests skip when data is absent.
cd scummvm_bench && export UV_CONFIG_FILE="$PWD/uv.toml" && uv sync
uv run --no-sync pytest                              # both trees (no real ScummVM bench runs)
uv run --no-sync pytest --run-real -n auto           # both trees, one xdist pool, full runs
uv run --no-sync pytest -o testpaths=../test/mcp     # MCP server tests only
uv run --no-sync pytest -o testpaths=tests           # bench only
# lint/type-check (test/mcp needs the explicit config — sibling dir):
uv run --no-sync ruff format . && uv run --no-sync ruff check . && uv run --no-sync ty check
uv run --no-sync ruff check --config "$PWD/pyproject.toml" ../test/mcp
uv run --no-sync ty check --project "$PWD" ../test/mcp
```

Game data is not in the repo. Tests look for it via per-game env vars and skip
when it is missing (see `test/mcp/README.md`).

**Every spawned instance must be headless on _both_ SDL drivers**
(`SDL_AUDIODRIVER=dummy` *and* `SDL_VIDEODRIVER=dummy`, set by
`test/mcp/launcher.py` and the bench's vendored copy in
`scummvm_bench/mcp_client.py`). This is not cosmetic — it is what makes the
suites survive `-n auto`, measured on one game with four instances up:

| drivers | result |
|---------|--------|
| audio=dummy video=dummy | 15 game frames/s, every instance starts |
| audio=dummy video=**real** | **1 frame/s** at 3% CPU — the compositor paces the engine |
| audio=**real** video=dummy | the 2nd instance and beyond **never start** (the audio device is exclusive, so they block before binding their MCP port) |

## Diagnosing a suite full of `httpx.ReadTimeout`

Client timeouts spread across unrelated games usually mean the *engine frame
rate* collapsed, not that the MCP server hung. Every streaming budget in the
bridge is counted in engine frames (`timeoutFrames()` 600, `settleFrames()` 10,
…), so an engine at 1 fps stretches them ~15x and each action outlives the
client's 60 s HTTP timeout (`MCP_TIMEOUT_SECS`). Before suspecting the server:

- Compare `debug.frame_counter` over a fixed wall-clock window, one instance
  alone vs. several up. A collapse there is the whole story.
- Check CPU: blocked (low CPU) points at a driver/device, spinning (pegged)
  at a real loop.
- A/B the server itself with `mcp=true` vs `mcp=false` on the same game and
  count frames (`o5_breakHere` lines in the `debuglevel=11` log). The bridge
  costs nothing measurable, and the transport polls non-blocking once per
  frame, so an equal count clears it.

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

  Run it through the consolidated env (it lives in `test/mcp/`, so pass the path):

  ```bash
  cd scummvm_bench && export UV_CONFIG_FILE="$PWD/uv.toml"
  uv run --no-sync python ../test/mcp/mcp_cli.py --launch pass --port 23462   # launch + REPL
  uv run --no-sync python ../test/mcp/mcp_cli.py --port 23462 state           # one-shot
  uv run --no-sync python ../test/mcp/mcp_cli.py --port 23462 mouse_click 140 134 --double
  uv run --no-sync python ../test/mcp/mcp_cli.py --port 23462 debug --vars 250-263
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
  `engines/mcp_bridge*`, the per-engine adapters `engines/{scumm,sword1,sky,queen}/mcp*`),
  `test/mcp/`, and `scummvm_bench/`. Avoid editing the rest of the engine; touch
  shared engine code only when there is no alternative, and keep it minimal.
- **Do not break retro-compatibility.** A change made for one game must not break
  other games, other SCUMM engine versions, or upstream behaviour. The bridge is
  shared by every SCUMM game from V0 to V8 — gate game- or version-specific
  behaviour behind the appropriate version/game checks rather than changing
  common paths. When in doubt, prefer an additive, narrowly-scoped branch.
- **The tool surface is per game, and says only what is true.** Which tools are
  registered, and what each one says, depend on the game and on the optional
  settings: a game that never asks a dialog question registers no `answer`, a
  game with a switchable team registers `switch_character`, and the `skip` /
  debug tools appear only with `mcp_skip_tool` / `mcp_debug`. Descriptions
  inherit from the base ones in `MCP::McpBridge` (`stateToolDescription()`,
  `actToolDescription()`, …) and are overridden only where a game actually
  differs. Three rules hold for every description, and `test/mcp/
  test_tools_contract.py` enforces them across games: **every tool declares an
  input *and* an output schema**; **no description names a tool that is not
  registered**; **no description names a game, an engine, or an implementation
  detail** (no "SCUMM", no "hotspot", no member names) — an agent is told what
  it can ask for and what comes back, nothing else.
- **Never put a machine-specific path in tracked code.** Game-data folders (and
  anything else that differs per checkout) go **only** in the non-committed
  `game_paths.local.toml` at the repository root — no defaults in Python, in
  `bench.toml`, or in any other committed file. `game_paths.local.toml.example`
  documents the format and is the file that *is* committed. Both trees read it:
  `test/mcp/launcher.py` (`GAME_PATHS`) and `scummvm_bench/scummvm_bench/
  game_paths.py`, each with its own copy of the loader. A per-game environment
  variable may override an entry. **A game with no data folder configured is
  skipped** — never failed — in both the MCP tests and the MCP bench.
- The two Python test trees share one project/venv (`scummvm_bench/pyproject.toml`,
  whose `testpaths` include `../test/mcp`). The vendored `scummvm_bench/scummvm_bench/
  mcp_client.py` must still stay a self-contained copy — keep the **runtime package**
  `scummvm_bench` free of imports from the `test/mcp` tree; only the shared *test
  tooling* (one pyproject/venv) spans both.

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
