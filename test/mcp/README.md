# MCP Integration Tests

Integration tests for the ScummVM MCP server, covering three classic adventure games:

- **Monkey Island 1 EGA Demo** (`test_monkey.py`) — 12 tests
- **Maniac Mansion C64 Demo** (`test_maniac_c64.py`) — 10 tests
- **Indiana Jones: Fate of Atlantis Demo** (`test_atlantis.py`) — 7 tests

## Requirements

- ScummVM built with MCP support (`./scummvm` in the repo root)
- Game data files in the expected locations (see Game Paths below)
- Python 3.7+ with `pytest` and `httpx`

## Game Paths

Tests look for game data in these locations (or environment variables):

| Game | Default Path | Environment Variable |
|------|----------|------|
| Monkey Island 1 EGA Demo | `/home/pi/games/MonkeyDemo` | `MONKEY_DEMO_PATH` |
| Maniac Mansion C64 Demo | `/home/pi/games/ManiacC64` | `MANIAC_C64_PATH` |
| Indiana Jones Fate of Atlantis Demo | `/home/pi/games/Indy4Demo` | `ATLANTIS_DEMO_PATH` |

Tests automatically **skip** if the game files are not found (using `pytest.skip()`).

## Installation

Create a Python venv and install dependencies:

```bash
python3 -m venv venv
. venv/bin/activate
pip install pytest pytest-xdist httpx
```

Or use the existing ScummVM TUI venv:

```bash
/home/pi/scummvm-tui-venv/bin/python -m pip install pytest httpx
```

## Running Tests

### All tests
```bash
cd /home/pi/monkey/scummvm/test/mcp
python -m pytest
```

### Specific test file
```bash
python -m pytest test_atlantis.py -v
```

### Specific parallelize by file
```bash
python -m pytest -n3 --dist=loadfile -v
```

### With custom game paths
```bash
ATLANTIS_DEMO_PATH=/path/to/indy4 python -m pytest test_atlantis.py -v
```

### Run with detailed output
```bash
python -m pytest -vv -s
```

## Test Structure

Each test file contains:

1. **Initial state test** — Verifies room, objects, and game state exist
2. **Action sequence tests** — Step through a scripted walkthrough
3. **Assertion helpers** — Utility functions to check state changes

### Key Assertions

- **`assert_inventory_contains(result, item)`** — Verify item pickup
- **`assert_messages_produced(result)`** — Verify dialog/messages appear
- **Movement checks** — `result.get("room_changed")` or `result.get("position")`

### Example Test

```python
def test_monkey_pickup_bowl(monkey_client: McpClient) -> None:
    """Pick up bowl o' mints."""
    result = monkey_client.act("pick_up", "bowl o' mints")
    assert_inventory_contains(result, "bowl o' mints")
```

## Utilities

### `utils.py`

**Classes:**
- `McpClient` — Synchronous HTTP MCP client with SSE streaming support
  - `state()` → Get current game state (sync)
  - `act(verb, target1, target2)` → Execute verb (streaming)
  - `answer(choice_id)` → Select dialog choice (streaming)
  - `initialize()` → Establish MCP session
  - `close()` → Cleanly disconnect

**Functions:**
- `launch_scummvm(game_id, game_path, port, scummvm_binary)` → Start ScummVM headlessly
- `wait_for_mcp(host, port, timeout)` → Poll until MCP server is ready
- `require_game_path(game_id)` → Skip test if game files missing
- `GAME_PATHS` — Dict of game data locations

### `conftest.py`

Pytest fixtures (session-scoped, one per game):
- `monkey_client` — Monkey Island fixture (port 23456)
- `maniac_client` — Maniac Mansion fixture (port 23457)
- `atlantis_client` — Atlantis fixture (port 23458)

Each fixture:
1. Checks game files exist (skips if missing)
2. Launches ScummVM with MCP on a dedicated port
3. Waits for MCP server to respond
4. Yields the initialized `McpClient`
5. Cleans up on teardown

## Debugging

### View MCP debug output
```bash
./scummvm --debugflags=mcp --debuglevel=11 monkey
```

### Manual MCP testing
```python
from utils import McpClient, launch_scummvm, wait_for_mcp

proc = launch_scummvm("atlantis", "/home/pi/games/Indy4Demo")
client = wait_for_mcp("127.0.0.1", 23458)
print(client.state())
result = client.act("walk_to", "path away from dock")
print(result)
```

## Notes

- Tests use `SDL_VIDEODRIVER=dummy` and `SDL_AUDIODRIVER=dummy` for headless operation
- Each game runs on a separate port to allow parallel test execution
- Tests are designed to work with the game demos; some sequences may not match the full game exactly
- Assertions are conservative: they check for state changes, not exact game progression
