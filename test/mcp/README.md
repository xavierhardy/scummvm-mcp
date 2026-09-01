# MCP Server Tests

Tests for the ScummVM MCP server, spanning SCUMM engine versions **V0**
(Maniac Mansion C64) through **V8** (Curse of Monkey Island). Two layers:

- **Python integration tests** (this folder) — launch a real headless ScummVM
  per game and drive the MCP server over HTTP, asserting on room/inventory/
  dialog/message state. ~122 tests across the games below, plus **41 pure unit
  tests** (`test_unit.py`) for the helper functions that need no game.
- **C++ unit tests** — `test/engines/scumm/mcp.h` (CxxTest) covers the
  engine-independent MCP string helpers (`normalizeActionName`,
  `mcpNormalizeSpaces`, `mcpSanitizeString`, …). Run with `make test` from the
  repo root.

## Games covered

| Game | Engine | `game_id` | Test files | Path env var |
|------|:------:|-----------|------------|--------------|
| Maniac Mansion (C64 demo) | V0 | `maniac-c64` | `test_maniac_c64.py`, `test_maniac_phone.py` | `MANIAC_C64_PATH` |
| Maniac Mansion (full game) | V1 | `maniac` | `test_maniac_full.py` | `MANIAC_PATH` |
| Zak McKracken and the Alien Mindbenders | V2 | `zak` | `test_zak.py` | `ZAK_PATH` |
| Indiana Jones 3 (Passport demo) | V3 | `pass` | `test_indy3.py`, `test_indy3_travel.py` | `PASS_DEMO_PATH` |
| Loom (Passport demo) | V3 | `pass` | `test_loom.py`, `test_loom_leaf.py` | `PASS_DEMO_PATH` |
| Monkey Island 1 (EGA demo) | V4 | `monkey-ega-demo` | `test_monkey.py` | `MONKEY_DEMO_PATH` |
| Monkey Island 1 (German EGA demo) | V4 | `monkey-ega-demo-de` | `test_monkey_de.py` | `MONKEY_DEMO_DE_PATH` |
| Monkey Island 2: LeChuck's Revenge | V5 | `monkey2` | `test_monkey2.py` | `MONKEY2_PATH` |
| Indiana Jones 4: Fate of Atlantis (demo) | V5 | `atlantis` | `test_atlantis.py` | `ATLANTIS_DEMO_PATH` |
| Sam & Max Hit the Road (demo) | V6 | `samnmax` | `test_samnmax.py`, `test_samnmax_carnival_tickets.py` | `SAMNMAX_DEMO_PATH` |
| Day of the Tentacle | V6 | `tentacle` | `test_tentacle.py` | `TENTACLE_PATH` |
| Full Throttle (demo) | V7 | `ft-demo` | `test_ft.py` | `FT_DEMO_PATH` |
| The Dig (demo) | V7 | `dig-demo` | `test_dig.py`, `test_dig_wreck.py` | `DIG_DEMO_PATH` |
| The Curse of Monkey Island (demo) | V8 | `comi-demo` | `test_comi.py`, `test_comi_cannon.py`, `test_comi_s3.py` | `COMI_DEMO_PATH` |
| Broken Sword 1: Shadow of the Templars (demo) | sword1 | `sword1-demo` | `test_sword1.py` | `SWORD1_DEMO_PATH` |
| Broken Sword 2: The Smoking Mirror (demo) | sword2 | `sword2-demo` | `test_sword2.py` | `SWORD2_DEMO_PATH` |
| Beneath a Steel Sky (CD) | sky | `sky` | `test_sky.py` | `SKY_PATH` |
| Flight of the Amazon Queen (talkie) | queen | `queen` | `test_queen.py` | `QUEEN_PATH` |
| Woodruff and the Schnibble | gob | `woodruff` | `test_woodruff.py` | `WOODRUFF_PATH` |
| Gobliiins (interactive demo) | gob | `gob1-demo` | `test_gob1.py` | `GOB1_DEMO_PATH` |
| Discworld (CD demo) | tinsel | `dw1-demo` | `test_dw1.py` | `DW1_DEMO_PATH` |
| Discworld II (demo) | tinsel | `dw2-demo` | `test_dw2.py` | `DW2_DEMO_PATH` |
| Toonstruck (demo) | toon | `toon-demo` | `test_toon.py` | `TOON_DEMO_PATH` |

### The full games

Twenty-six retail games, each the whole version of something above or beside
it, and all covered by one module — `test_full_games.py`. It asks each of them
the same shallow questions through the same tools an agent has: does `state`
answer with a room, does the room name things, are the tools registered, does
a wrong target come back as a refusal that says what *is* here, does an action
reach the game. Anything deeper about one of them belongs in a module of its
own.

Their ids are the demo's id with `-full` after it, so `ft-full` is the whole of
Full Throttle and `ft-demo` is still the demo; `sq2vga` and `pq2-full` have no
demo here. Paths go in `game_paths.local.toml` under those same ids, and each
starts from its own slot 1 captured just past the opening — a game whose slot
has not been captured on this machine skips, like any other unconfigured game.

King's Quest IV is deliberately absent: its copy ships a copy-protection crack
ScummVM refuses to load, and without it the game asks a question out of its
printed manual. The bridge drives that screen correctly — `type_text` types at
the prompt and the game echoes and refuses each answer — but there is no room
to start a test from.

Game-data folders are per-machine and are **never** in tracked code: list them
under `[games]` in the non-committed `game_paths.local.toml` at the repository
root (see `game_paths.local.toml.example`), or set the env var above, which
overrides the file. Each test **skips** (not fails) when its game has no folder
configured or the folder is missing. Flight of the Amazon Queen additionally
needs `queen.tbl`, and Toonstruck needs `toon.dat`; the launcher serves both
automatically from the repository's `dists/engine-data` via `extrapath`.

Broken Sword 2 has no save to load: its test is one ordered sequence on a
single fresh instance, past the demo's opening. Its ini asks the engine for the
`object_labels` game option — the bridge reads a thing's label out of its mouse
box whatever the option says, so the option only decides whether a human player
sees the same label on screen.

Toonstruck's demo has no save to load either: it opens on a logo movie and
then plays, so its test is one ordered walkthrough on a single fresh instance,
skipped past the opening.

Discworld II needs the **Windows** demo (`dw2-win-demo-en`): the DOS demo is
flagged unsupported by ScummVM's Tinsel engine — its scripts use a library-call
numbering the interpreter has no table for — so it refuses to start and
`dw2-demo` must not be pointed at it.

Zak McKracken, Monkey Island 2 and Day of the Tentacle are **full games**, not
demos, so their coverage is deliberately shallow — compatibility smoke tests
(verb bar, objects, a few verb dispatches, dialog for MI2) run from a committed
slot-1 save captured right after each intro. MI2's floppy copy protection is
bypassed by that save: the potion-mixer screen only needs to be cleared once,
which is what capturing the slot did. Zak also carries a slot-2 save parked in
the living room with the TV playing — the scene whose endless background lines
used to hold every action's stream open until it timed out — and MI2 a slot-2
save in the swamp, one step from both of its click-only screens (the island map
and the coffin).

> **These tests share one Python project with `scummvm_bench`.** There is no
> longer a `test/mcp/pyproject.toml` or venv — the single project lives at
> `../scummvm_bench/pyproject.toml` (one venv, one config), and its
> `[tool.pytest] testpaths` include this `../test/mcp` tree. Run everything from
> `scummvm_bench/` and select this tree with `-o testpaths=../test/mcp`.

## Requirements

- ScummVM built with MCP support (`./scummvm` in the repo root — run `make`)
- Python 3.11+ and [`uv`](https://github.com/astral-sh/uv)
- Game data only for the games you want to exercise (others skip)

## Install / dev

```bash
cd scummvm_bench                      # the consolidated Python project
export UV_CONFIG_FILE="$PWD/uv.toml"
uv sync
# lint/type-check this (sibling) tree with the explicit config:
uv run --no-sync ruff format ../test/mcp
uv run --no-sync ruff check --config "$PWD/pyproject.toml" ../test/mcp
uv run --no-sync ty check --project "$PWD" ../test/mcp
```

## Running

All from `scummvm_bench/` (with `UV_CONFIG_FILE` exported as above):

# Narrow to specific files/tests with `-k` (a positional filename would resolve
# against scummvm_bench/, the wrong dir — `-o testpaths` + `-k` keeps the config).
```bash
uv run --no-sync pytest -o testpaths=../test/mcp                 # all MCP server tests
uv run --no-sync pytest -o testpaths=../test/mcp -k test_unit    # the 41 pure unit tests
uv run --no-sync pytest -o testpaths=../test/mcp -k comi -v      # one game
uv run --no-sync pytest -o testpaths=../test/mcp -n auto         # parallel (see fixture model below)
uv run --no-sync pytest                                          # BOTH trees (test/mcp + bench)
MANIAC_C64_PATH=/path/to/data uv run --no-sync pytest -o testpaths=../test/mcp -k maniac
```

The suite runs under `--dist=loadgroup` by default. `SKIP_SLOW_TESTS=1` skips
tests marked `slow`.

## How it works

Most game fixtures are **function-scoped**: each test launches its own fresh
ScummVM from a committed save slot (`save_slots/<game>/`), drives it, and tears
it down — so tests are independent and parallel-safe. Ports are assigned per
`(xdist worker, fixture)` by `utils.get_mcp_port` to avoid collisions.

Full Throttle and Atlantis demos cannot save/load arbitrary states, so their
fixtures are **session-scoped** ordered walkthroughs pinned to a single worker
via the `xdist_group` mark. Woodruff, Gobliiins, the two Discworld demos and
the full Maniac Mansion run the same way, each starting fresh and skipping past
its intro (or, for Maniac, its kid selection).

## Code map

- `mcp_client.py` — `McpClient` (sync HTTP MCP client with SSE streaming:
  `state`, `act`, `answer`, `walk`, `skip`, plus debug tools), `wait_for_mcp`,
  and the per-worker `get_mcp_port` allocator.
- `launcher.py` — `launch_scummvm`, `GAME_PATHS`, and the `require_game_path` /
  `require_save_slot` / `save_slot_path` skip guards.
- `state_helpers.py` — pure state/dialog helpers (`object_by_id`,
  `find_choice_id`, `choice_labels`, `make_verbs`, …) and the
  interactive-readiness polls (`wait_for_interactive`, `require_interactive`, …).
- `utils.py` — back-compat facade re-exporting the three modules above, so
  `from utils import …` keeps working.
- `assertions.py` — `assert_inventory_contains`, `assert_room`,
  `assert_message_contains`, `assert_no_talkie_garbage`, …
- `sword1_helpers.py` / `sky_helpers.py` / `queen_helpers.py` — per-engine
  world-model helpers for the non-SCUMM games (verb sets, readiness polls,
  walkthrough segments).
- `conftest.py` — the per-game pytest fixtures described above.
- `test_unit.py` — unit tests for the `utils`/`assertions` helpers (no game).

### Example test

```python
def test_pickup_bowl(monkey_client: McpClient) -> None:
    result = monkey_client.act("pick_up", "bowl o' mints")
    assert_inventory_contains(result, "bowl o' mints")
```

## Debugging

```bash
# Verbose MCP server logging from the engine:
./scummvm --debugflags=mcp --debuglevel=11 monkey

# Drive the server by hand with mcp_cli.py (REPL or one-shot). It can also
# launch ScummVM on a per-game save for you. Run it through the consolidated env
# (from scummvm_bench/, with UV_CONFIG_FILE exported):
uv run --no-sync python ../test/mcp/mcp_cli.py --launch pass --port 23462   # launch + REPL
uv run --no-sync python ../test/mcp/mcp_cli.py --port 23462 state           # one-shot
uv run --no-sync python ../test/mcp/mcp_cli.py --port 23462 act pick_up "bowl o' mints"
```

## Notes

- Tests run headless (`SDL_VIDEODRIVER=dummy`, `SDL_AUDIODRIVER=dummy`).
- Assertions are conservative — they check for state *changes*, not exact game
  progression, since demo scripts can differ from the full games.
