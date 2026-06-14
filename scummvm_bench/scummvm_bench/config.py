"""Bench configuration: a dataclass plus a TOML loader.

CLI flags always override values from the config file. Unknown keys are ignored
so the file can carry extra documentation.
"""

import tomllib
from dataclasses import dataclass, field
from pathlib import Path

from .models import GameSpec, LocalModelSpec
from .session import SessionConfig

_PACKAGE_ROOT = Path(__file__).resolve().parent.parent


def default_config_path() -> str:
    """Path to the vendored default ``bench.toml``."""
    return str(_PACKAGE_ROOT / "config" / "bench.toml")


@dataclass
class BenchConfig:
    """Everything the bench can read from a config file."""

    scummvm_binary: str = "../scummvm"
    save_folder: str | None = None
    games: dict[str, GameSpec] = field(default_factory=dict)
    local_models: list[LocalModelSpec] = field(default_factory=list)
    lms_bin: str = "lms"
    models_json: str | None = None

    def session_config(self, save_folder: str | None = None) -> SessionConfig:
        return SessionConfig(
            scummvm_binary=self.scummvm_binary,
            games=self.games,
            save_folder=save_folder or self.save_folder,
        )

    def local_model_map(self) -> dict[tuple[str | None, str | None], LocalModelSpec]:
        return {(m.provider, m.model): m for m in self.local_models}


def _coerce_int(value: object) -> int | None:
    return int(value) if isinstance(value, (int, float)) else None


def _game_from_dict(entry: dict[str, object]) -> GameSpec:
    game_id = str(entry["id"])
    return GameSpec(
        game_id=game_id,
        game_path=str(entry.get("path", "")),
        save_slot=_coerce_int(entry.get("save_slot")),
        ini_template=(
            str(entry["ini_template"]) if entry.get("ini_template") else None
        ),
    )


def _model_from_dict(entry: dict[str, object]) -> LocalModelSpec:
    return LocalModelSpec(
        key=str(entry["key"]),
        provider=str(entry.get("provider", "local")),
        model=str(entry.get("model", "")),
        context_length=_coerce_int(entry.get("context_length")),
        gpu=str(entry["gpu"]) if entry.get("gpu") else None,
        ttl=_coerce_int(entry.get("ttl")),
        base_url=str(entry.get("base_url", "http://localhost:1234/v1")),
        api_key=str(entry.get("api_key", "lm-studio")),
    )


def load_config(path: str | None = None) -> BenchConfig:
    """Load a :class:`BenchConfig` from ``path`` (or the vendored default)."""
    resolved = path or default_config_path()
    raw = tomllib.loads(Path(resolved).read_text())

    games_list = raw.get("games", [])
    games = {
        spec.game_id: spec
        for spec in (_game_from_dict(g) for g in games_list if isinstance(g, dict))
    }
    models_list = raw.get("local_models", [])
    local_models = [_model_from_dict(m) for m in models_list if isinstance(m, dict)]

    return BenchConfig(
        scummvm_binary=str(raw.get("scummvm_binary", "../scummvm")),
        save_folder=str(raw["save_folder"]) if raw.get("save_folder") else None,
        games=games,
        local_models=local_models,
        lms_bin=str(raw.get("lms_bin", "lms")),
        models_json=str(raw["models_json"]) if raw.get("models_json") else None,
    )
