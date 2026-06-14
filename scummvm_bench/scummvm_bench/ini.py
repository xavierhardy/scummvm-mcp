"""Render per-session ``scummvm.ini`` files from vendored templates.

Templates use ``%(game_path)s`` / ``%(mcp_port)s`` / ``%(logfile)s`` substitution
(matching the existing ``test/mcp`` templates). ``mcp_port`` is the *ScummVM* MCP
port, never the bench proxy port.
"""

from pathlib import Path

_CONFIG_DIR = Path(__file__).resolve().parent.parent / "config"
_TEMPLATE_DIR = _CONFIG_DIR / "ini_templates"


def default_template_path(game_id: str) -> str:
    """Return the vendored ini template path for ``game_id``."""
    return str(_TEMPLATE_DIR / f"scummvm_{game_id}.ini")


def render_ini(
    template_path: str,
    game_path: str,
    scummvm_port: int,
    logfile: str,
    dest_path: str,
) -> str:
    """Render ``template_path`` into ``dest_path`` and return the destination."""
    content = Path(template_path).read_text() % {
        "game_path": game_path,
        "mcp_port": scummvm_port,
        "logfile": logfile,
    }
    Path(dest_path).write_text(content)
    return dest_path
