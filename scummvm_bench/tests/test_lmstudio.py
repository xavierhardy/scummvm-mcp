"""LM Studio manager: models.json patch/restore and a mocked lifecycle."""

import json
import subprocess

import scummvm_bench.lmstudio as lmstudio_mod
from scummvm_bench.lmstudio import LmStudioManager, default_models_json
from scummvm_bench.models import LocalModelSpec

MODEL = LocalModelSpec(key="google/gemma", provider="local", model="gemma")


def test_default_models_json_honours_env(monkeypatch, tmp_path) -> None:
    monkeypatch.setenv("PI_CODING_AGENT_DIR", str(tmp_path))
    assert default_models_json() == str(tmp_path / "models.json")


def test_register_then_restore_existing_file(tmp_path) -> None:
    path = tmp_path / "models.json"
    original = json.dumps({"providers": {"openai": {"baseUrl": "x"}}}, indent=2)
    path.write_text(original)

    mgr = LmStudioManager(MODEL, models_json=str(path))
    mgr.register_pi_model()
    data = json.loads(path.read_text())
    assert data["providers"]["local"]["baseUrl"] == MODEL.base_url
    assert "openai" in data["providers"]  # untouched

    mgr.restore_models_json()
    assert path.read_text() == original


def test_register_then_restore_created_file(tmp_path) -> None:
    path = tmp_path / "models.json"
    mgr = LmStudioManager(MODEL, models_json=str(path))
    mgr.register_pi_model()
    assert path.exists()
    mgr.restore_models_json()
    # We created it and it only held our provider -> removed.
    assert not path.exists()


def test_delete_is_best_effort() -> None:
    # lmstudio SDK is not installed; delete must not raise.
    LmStudioManager(MODEL, models_json="/nonexistent").delete()


def test_full_lifecycle_with_mocked_lms(monkeypatch, tmp_path) -> None:
    calls: list[list[str]] = []

    def fake_run(args, **kwargs):
        calls.append(args)
        return subprocess.CompletedProcess(args, 0, "", "")

    monkeypatch.setattr(lmstudio_mod.subprocess, "run", fake_run)
    path = tmp_path / "models.json"

    with LmStudioManager(MODEL, models_json=str(path)) as mgr:
        assert mgr.identifier == "gemma"
        assert json.loads(path.read_text())["providers"]["local"]

    commands = [args[1] for args in calls]
    assert commands[0] == "get"
    assert "load" in commands
    assert "unload" in commands
    # created file holding only our provider is cleaned up on exit
    assert not path.exists()
