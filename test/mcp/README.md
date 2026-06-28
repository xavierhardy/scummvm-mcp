# MCP Server Tests

Tests for the ScummVM MCP server, spanning SCUMM engine versions **V0**
(Maniac Mansion C64) through **V8** (Curse of Monkey Island). Two layers:

- **Python integration tests** (this folder) — launch a real headless ScummVM
  per game and drive the MCP server over HTTP, asserting on room/inventory/
  dialog/message state. ~104 tests across the games below, plus **41 pure unit
  tests** (`test_unit.py`) for the helper functions that need no game.
- **C++ unit tests** — `test/engines/scumm/mcp.h` (CxxTest) covers the
  engine-independent MCP string helpers (`normalizeActionName`,
  `mcpNormalizeSpaces`, `mcpSanitizeString`, …). Run with `make test` from the
  repo root.

## Games covered

| Game | Engine | `game_id` | Test files | Path env var |
|------|:------:|-----------|------------|--------------|
| Maniac Mansion (C64 demo) | V0 | `maniac-c64` | `test_maniac_c64.py`, `test_maniac_phone.py` | `MANIAC_C64_PATH` |
| Indiana Jones 3 (Passport demo) | V3 | `pass` | `test_indy3.py`, `test_indy3_travel.py` | `PASS_DEMO_PATH` |
| Loom (Passport demo) | V3 | `pass` | `test_loom.py`, `test_loom_leaf.py` | `PASS_DEMO_PATH` |
| Monkey Island 1 (EGA demo) | V4 | `monkey-ega-demo` | `test_monkey.py` | `MONKEY_DEMO_PATH` |
| Monkey Island 1 (German EGA demo) | V4 | `monkey-ega-demo-de` | `test_monkey_de.py` | `MONKEY_DEMO_DE_PATH` |
| Indiana Jones 4: Fate of Atlantis (demo) | V5 | `atlantis` | `test_atlantis.py` | `ATLANTIS_DEMO_PATH` |
| Sam & Max Hit the Road (demo) | V6 | `samnmax` | `test_samnmax.py`, `test_samnmax_carnival_tickets.py` | `SAMNMAX_DEMO_PATH` |
| Full Throttle (demo) | V7 | `ft-demo` | `test_ft.py` | `FT_DEMO_PATH` |
| The Dig (demo) | V7 | `dig-demo` | `test_dig.py`, `test_dig_wreck.py` | `DIG_DEMO_PATH` |
| The Curse of Monkey Island (demo) | V8 | `comi-demo` | `test_comi.py`, `test_comi_cannon.py`, `test_comi_s3.py` | `COMI_DEMO_PATH` |

Each test **skips** (not fails) when its game data is missing. Point the env var
at the data folder if it is not at the built-in default.

## Requirements

- ScummVM built with MCP support (`./scummvm` in the repo root — run `make`)
- Python 3.11+ and [`uv`](https://github.com/astral-sh/uv) (manages `pytest`/`httpx`)
- Game data only for the games you want to exercise (others skip)

## Install / dev

```bash
cd test/mcp
uv sync
uv run ruff format . && uv run ruff check . && uv run ty check
```

`uv sync` creates `.venv/` and installs `pytest`, `pytest-xdist` and `httpx`.
Lint/format is `ruff`; type-checking is `ty` (both via `uv run`).

## Running

```bash
uv run pytest                       # everything (game tests skip if data absent)
uv run pytest test_unit.py          # the 41 pure unit tests (no game needed)
uv run pytest test_comi.py -v       # one game
uv run pytest -n auto               # parallel (see fixture model below)
MANIAC_C64_PATH=/path/to/data uv run pytest test_maniac_c64.py
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
via the `xdist_group` mark.

## Code map

- `utils.py` — `McpClient` (sync HTTP MCP client with SSE streaming:
  `state`, `act`, `answer`, `walk`, `skip`, plus debug tools), `launch_scummvm`,
  `wait_for_mcp`, `GAME_PATHS`, and pure state/dialog helpers (`object_by_id`,
  `find_choice_id`, `choice_labels`, `make_verbs`, …).
- `assertions.py` — `assert_inventory_contains`, `assert_room`,
  `assert_message_contains`, `assert_no_talkie_garbage`, …
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
# launch ScummVM on a per-game save for you:
uv run python mcp_cli.py --launch pass --port 23462          # launch + REPL
uv run python mcp_cli.py --port 23462 state                  # one-shot
uv run python mcp_cli.py --port 23462 act pick_up "bowl o' mints"
```

## Notes

- Tests run headless (`SDL_VIDEODRIVER=dummy`, `SDL_AUDIODRIVER=dummy`).
- Assertions are conservative — they check for state *changes*, not exact game
  progression, since demo scripts can differ from the full games.
