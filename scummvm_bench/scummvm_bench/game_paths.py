"""Per-machine game-data folders, read from the non-committed local config.

Game data is not in the repository and sits somewhere different on every
machine, so no game-data path may live in tracked code — not in ``bench.toml``,
not in a Python default. The single source is ``game_paths.local.toml`` at the
repository root (``game_paths.local.toml.example`` documents the format), or the
per-game environment variable. A game configured by neither has no path, and the
bench tests skip it rather than fail.

``test/mcp/launcher.py`` reads the same file with its own copy of this logic:
the runtime package deliberately does not import from the test tree.
"""

import os
import tomllib
from pathlib import Path

# game_paths -> scummvm_bench -> scummvm_bench -> repo root
REPO_ROOT = Path(__file__).resolve().parents[2]
LOCAL_PATHS_FILE = REPO_ROOT / "game_paths.local.toml"

# game id -> environment variable that overrides its folder.
GAME_PATH_ENV = {
    "monkey-ega-demo": "MONKEY_DEMO_PATH",
    "monkey-ega-demo-de": "MONKEY_DEMO_DE_PATH",
    "maniac-c64": "MANIAC_C64_PATH",
    "zak": "ZAK_PATH",
    "pass": "PASS_DEMO_PATH",
    "monkey2": "MONKEY2_PATH",
    "atlantis": "ATLANTIS_DEMO_PATH",
    "samnmax": "SAMNMAX_DEMO_PATH",
    "tentacle": "TENTACLE_PATH",
    "ft-demo": "FT_DEMO_PATH",
    "dig-demo": "DIG_DEMO_PATH",
    "comi-demo": "COMI_DEMO_PATH",
    "sword1-demo": "SWORD1_DEMO_PATH",
    "sky": "SKY_PATH",
    "queen": "QUEEN_PATH",
    "woodruff": "WOODRUFF_PATH",
}


def local_paths_file() -> Path:
    """The local config actually in use (``MCP_GAME_PATHS_FILE`` overrides it)."""
    override = os.environ.get("MCP_GAME_PATHS_FILE")
    return Path(override) if override else LOCAL_PATHS_FILE


def load_game_paths() -> dict[str, str]:
    """Return the configured ``game id -> data folder`` map.

    Entries come from the local config's ``[games]`` table, each overridable by
    its environment variable. Games with no folder configured are absent from
    the map; ``~`` is expanded.
    """
    path = local_paths_file()
    paths: dict[str, str] = {}
    if path.is_file():
        raw = tomllib.loads(path.read_text())
        games = raw.get("games", {})
        if not isinstance(games, dict):
            raise ValueError(f"{path}: [games] must be a table of game id -> folder")
        paths = {
            game_id: os.path.expanduser(str(folder))
            for game_id, folder in games.items()
        }
    for game_id, env_var in GAME_PATH_ENV.items():
        folder = os.environ.get(env_var)
        if folder:
            paths[game_id] = os.path.expanduser(folder)
    return paths


def game_path(game_id: str) -> str:
    """The configured data folder for *game_id*, or "" when there is none."""
    return load_game_paths().get(game_id, "")


def missing_path_reason(game_id: str) -> str:
    """A skip message explaining how to configure *game_id*'s data folder."""
    env_var = GAME_PATH_ENV.get(game_id, f"<{game_id} path env var>")
    return (
        f"{game_id} has no data folder configured — add it under [games] in "
        f"{local_paths_file()} (see game_paths.local.toml.example) or set {env_var}"
    )
