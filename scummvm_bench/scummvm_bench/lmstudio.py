"""LM Studio model lifecycle for the ``--local`` flow.

As a context manager: on enter it downloads (``lms get``), loads (``lms load``)
the model and registers it as a provider in pi's ``models.json``; on exit it
unloads (``lms unload``), best-effort deletes the model, and restores
``models.json`` to its exact original bytes. Every external call is wrapped so a
failure never poisons benchmark results.
"""

import json
import os
import subprocess
from pathlib import Path

from .models import LocalModelSpec


def default_models_json() -> str:
    """Return pi's ``models.json`` path (honouring ``PI_CODING_AGENT_DIR``)."""
    agent_dir = os.environ.get("PI_CODING_AGENT_DIR")
    base = Path(agent_dir) if agent_dir else Path.home() / ".pi" / "agent"
    return str(base / "models.json")


class LmStudioManager:
    """Download/load/register and later unload/delete an LM Studio model."""

    def __init__(
        self,
        model: LocalModelSpec,
        lms_bin: str = "lms",
        models_json: str | None = None,
    ) -> None:
        self.model = model
        self.lms_bin = lms_bin
        self.models_json = models_json or default_models_json()
        self.identifier = model.model or model.key
        self._models_json_backup: str | None = None

    def __enter__(self) -> "LmStudioManager":
        self.get()
        self.load()
        self.register_pi_model()
        return self

    def __exit__(self, *exc: object) -> None:
        self.unload()
        self.delete()
        self.restore_models_json()

    # -- LM Studio CLI -----------------------------------------------------

    def get(self) -> None:
        self._run([self.lms_bin, "get", self.model.key, "-y"])

    def load(self) -> None:
        args = [self.lms_bin, "load", self.model.key, "--identifier", self.identifier]
        if self.model.context_length is not None:
            args += ["--context-length", str(self.model.context_length)]
        if self.model.gpu is not None:
            args += ["--gpu", self.model.gpu]
        if self.model.ttl is not None:
            args += ["--ttl", str(self.model.ttl)]
        args += self.model.extra_load_args
        self._run(args)

    def unload(self) -> None:
        self._run([self.lms_bin, "unload", self.identifier])

    def delete(self) -> None:
        """Best-effort deletion. ``lms`` has no delete; try the SDK if present."""
        try:
            import lmstudio  # ty: ignore[unresolved-import]

            remover = getattr(lmstudio, "delete_model", None)
            if callable(remover):
                remover(self.model.key)
        except Exception:  # noqa: BLE001 - deletion is optional cleanup
            pass

    # -- pi models.json ----------------------------------------------------

    def register_pi_model(self) -> None:
        """Add the local provider to pi's ``models.json`` (backing up the file)."""
        path = Path(self.models_json)
        if path.exists():
            self._models_json_backup = path.read_text()
            try:
                data = json.loads(self._models_json_backup)
            except json.JSONDecodeError:
                data = {}
        else:
            self._models_json_backup = None
            data = {}

        if not isinstance(data, dict):
            data = {}
        providers = data.setdefault("providers", {})
        if isinstance(providers, dict):
            providers[self.model.provider] = {
                "baseUrl": self.model.base_url,
                "apiKey": self.model.api_key,
            }
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data, indent=2))

    def restore_models_json(self) -> None:
        path = Path(self.models_json)
        if self._models_json_backup is not None:
            path.write_text(self._models_json_backup)
        elif path.exists():
            # We created it; remove our temporary entry by deleting the file only
            # if it solely contains our provider.
            try:
                data = json.loads(path.read_text())
                providers = data.get("providers", {}) if isinstance(data, dict) else {}
                if set(providers) <= {self.model.provider}:
                    path.unlink()
            except Exception:  # noqa: BLE001
                pass

    # -- helpers -----------------------------------------------------------

    @staticmethod
    def _run(args: list[str]) -> subprocess.CompletedProcess[str]:
        return subprocess.run(  # noqa: S603
            args,
            check=False,
            capture_output=True,
            text=True,
        )
