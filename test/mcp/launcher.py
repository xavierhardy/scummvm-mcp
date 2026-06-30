#!/usr/bin/env python3
"""Headless ScummVM launcher + game-data/save-slot resolution for the tests.

Renders a per-game ini, isolates saves, and spawns ScummVM with MCP enabled, plus
the ``GAME_PATHS`` map and the ``require_*`` skip guards. Imported via ``utils``
for back-compat.
"""

import os
import shutil
import subprocess
import tempfile


def _logs_dir() -> str:
    """Return the (created) directory ScummVM's log files are written to."""
    logs_dir = os.path.join(os.path.dirname(__file__), "logs")
    os.makedirs(logs_dir, exist_ok=True)
    return logs_dir


def _write_ini(game_id: str, game_path: str, port: int, scummvm_log: str) -> str:
    """Render the per-game ini template into a fresh temp dir, return its path."""
    ini_template = os.path.join(
        os.path.dirname(__file__), "ini_files", f"scummvm_{game_id}.ini"
    )
    with open(ini_template) as ini_file:
        content = ini_file.read() % {
            "game_path": game_path,
            "mcp_port": port,
            "logfile": scummvm_log,
        }
    tmpdir = tempfile.mkdtemp(prefix=f"scummvm_{game_id}_")
    ini_path = os.path.join(tmpdir, "scummvm.ini")
    with open(ini_path, "w") as f:
        f.write(content)
    return ini_path


def _resolve_save_path(game_id: str, ini_path: str, isolate_saves: bool) -> str:
    """Return the ``--savepath`` for this instance.

    When ``isolate_saves`` is true, copy the repo's ``save_slots/<game_id>`` into
    a private folder (next to the ini) so concurrent same-game instances never
    share (and clobber) save files; otherwise use the repo directory directly.
    """
    repo_save_path = os.path.join(os.path.dirname(__file__), f"save_slots/{game_id}")
    if not isolate_saves:
        return repo_save_path
    save_path = os.path.join(os.path.dirname(ini_path), "saves")
    if os.path.isdir(repo_save_path):
        shutil.copytree(repo_save_path, save_path)
    else:
        os.makedirs(save_path, exist_ok=True)
    return save_path


def _launch_args(
    game_id: str,
    scummvm_binary: str,
    ini_path: str,
    save_slot: int,
    save_path: str,
) -> list[str]:
    """Build the ScummVM command line for ``game_id``."""
    if game_id in ("atlantis",):
        # No save slot — these games start from scratch and handle their own intro.
        return [scummvm_binary, "-c", ini_path, game_id]
    return [
        scummvm_binary,
        "-c",
        ini_path,
        f"--save-slot={save_slot}",
        f"--savepath={save_path}",
        "--talkspeed=255",
        game_id,
    ]


def _open_log_handles(game_id: str, port: int, args: list[str], ini_path: str):
    """Write a header to the stdout log and return ``(stdout_fh, stderr_fh,
    log_file, stderr_file)`` append handles for the launched process."""
    logs_dir = _logs_dir()
    log_file = os.path.join(logs_dir, f"scummvm_{game_id}_{port}.log")
    stderr_file = os.path.join(logs_dir, f"scummvm_{game_id}_{port}.stderr")
    with open(log_file, "w") as logf:
        logf.write(f"Command: {' '.join(args)}\n")
        logf.write("Environment: SDL_AUDIODRIVER=dummy\n")
        logf.write(f"Config: {ini_path}\n")
        logf.write("=" * 80 + "\n\n")
    return open(log_file, "a"), open(stderr_file, "a"), log_file, stderr_file


def launch_scummvm(
    game_id: str,
    game_path: str,
    port: int = 23456,
    scummvm_binary: str = "./scummvm",
    save_slot: int = 1,
    isolate_saves: bool = True,
) -> subprocess.Popen:
    """Launch ScummVM headlessly with MCP enabled for the given game.

    When ``isolate_saves`` is true (the default, used by the test fixtures), the
    game's ``save_slots/<game_id>`` directory is copied into a private temporary
    folder used as the ``--savepath``. This keeps parallel instances of the same
    game from racing on a shared save directory and stops autosaves from
    polluting the committed save files. Pass ``isolate_saves=False`` (e.g. from
    ``launch_manual.py``) when you want ``save_state`` to write back into the
    repository's ``save_slots/<game_id>`` directory."""
    scummvm_log = os.path.join(_logs_dir(), f"scummvm_{game_id}_{port}.scummvm.log")
    ini_path = _write_ini(game_id, game_path, port, scummvm_log)
    save_path = _resolve_save_path(game_id, ini_path, isolate_saves)
    args = _launch_args(game_id, scummvm_binary, ini_path, save_slot, save_path)

    # Launch with no video/audio
    env = os.environ.copy()
    env["SDL_AUDIODRIVER"] = "dummy"

    stdout_file, stderr_fh, log_file, stderr_file = _open_log_handles(
        game_id, port, args, ini_path
    )
    # Run from test/mcp regardless of the caller's CWD: ScummVM resolves some
    # runtime state relative to the working directory, and the suite historically
    # ran from here. (Tests may now be driven from the consolidated project in
    # ../scummvm_bench, so don't inherit that CWD.)
    proc = subprocess.Popen(
        args, env=env, stdout=stdout_file, stderr=stderr_fh,
        cwd=os.path.dirname(__file__),
    )

    # Keep the log file handles alive for the process lifetime so they are not
    # garbage-collected; the fixture teardown closes them (see conftest.py).
    # setattr (not attribute assignment) because Popen has no typed slot for it.
    setattr(proc, "_log_handles", (stdout_file, stderr_fh))  # noqa: B010

    print(f"[MCP] {game_id} stdout: {log_file}", flush=True)
    print(f"[MCP] {game_id} stderr: {stderr_file}", flush=True)

    return proc


GAME_PATHS = {
    "monkey-ega-demo": os.environ.get("MONKEY_DEMO_PATH", "/home/pi/games/MonkeyDemo"),
    "monkey-ega-demo-de": os.environ.get(
        "MONKEY_DEMO_DE_PATH",
        "/Users/xhardy/Personal/llm/scummvm/games/monkey1-dos-ega-demo-de",
    ),
    "maniac-c64": os.environ.get("MANIAC_C64_PATH", "/home/pi/games/ManiacC64"),
    "atlantis": os.environ.get("ATLANTIS_DEMO_PATH", "/home/pi/games/Indy4Demo"),
    "samnmax": os.environ.get(
        "SAMNMAX_DEMO_PATH",
        "/Users/xhardy/Personal/llm/scummvm/games/samnmax-dos-demo-en",
    ),
    "dig-demo": os.environ.get(
        "DIG_DEMO_PATH",
        "/Users/xhardy/Personal/llm/scummvm/games/Dig",
    ),
    "ft-demo": os.environ.get(
        "FT_DEMO_PATH",
        "/Users/xhardy/Personal/llm/scummvm/games/ft-dos-demo",
    ),
    "pass": os.environ.get(
        "PASS_DEMO_PATH",
        "/Users/xhardy/Personal/llm/scummvm/games/pass",
    ),
    "comi-demo": os.environ.get(
        "COMI_DEMO_PATH",
        "/Users/xhardy/Personal/llm/scummvm/games/COMIDEMO",
    ),
}


def require_game_path(game_id: str) -> None:
    """Skip test if game files are not found."""
    import pytest

    path = GAME_PATHS.get(game_id)
    if not path or not os.path.isdir(path):
        pytest.skip(f"Game files not found at {path}")


def save_slot_path(game_id: str, slot: int) -> str:
    """Path to the committed save file for *game_id*'s *slot* (``<game>.sNN``)."""
    return os.path.join(
        os.path.dirname(__file__), "save_slots", game_id, f"{game_id}.s{slot:02d}"
    )


def require_save_slot(game_id: str, slot: int) -> None:
    """Skip test if the checkpoint save file for this slot hasn't been captured.

    Checkpoint slots are generated by ``make_save_states.py`` on a machine that
    has the game data; until that capture is run, the slot file is absent and
    the dependent tests skip rather than fail.
    """
    import pytest

    path = save_slot_path(game_id, slot)
    if not os.path.isfile(path):
        pytest.skip(
            f"checkpoint save {os.path.basename(path)} not found — run "
            f"make_save_states.py to capture it"
        )
