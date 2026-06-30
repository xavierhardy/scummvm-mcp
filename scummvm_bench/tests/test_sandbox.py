"""Environment scrub + sandbox-exec wrapping for the coding-agent harnesses."""

import scummvm_bench.harness.sandbox as sandbox_mod
from scummvm_bench.harness.sandbox import build_agent_env, wrap_command


def test_build_agent_env_keeps_allowlist_and_api_keys(monkeypatch) -> None:
    monkeypatch.setenv("PATH", "/usr/bin")
    monkeypatch.setenv("HOME", "/home/pi")
    monkeypatch.setenv("ANTHROPIC_API_KEY", "secret")
    monkeypatch.setenv("LC_ALL", "C")
    monkeypatch.setenv("MONKEY_DEMO_PATH", "/games/monkey")
    monkeypatch.setenv("SOME_OTHER_VAR", "nope")

    env = build_agent_env()

    assert env["PATH"] == "/usr/bin"
    assert env["HOME"] == "/home/pi"
    assert env["ANTHROPIC_API_KEY"] == "secret"
    assert env["LC_ALL"] == "C"
    # Game-path and unrelated vars are dropped.
    assert "MONKEY_DEMO_PATH" not in env
    assert "SOME_OTHER_VAR" not in env


def test_build_agent_env_applies_extra(monkeypatch) -> None:
    monkeypatch.setenv("PATH", "/usr/bin")
    env = build_agent_env({"CLAUDE_CONFIG_DIR": "/run/cfg"})
    assert env["CLAUDE_CONFIG_DIR"] == "/run/cfg"


def test_wrap_command_noop_off_darwin(monkeypatch) -> None:
    monkeypatch.setattr(sandbox_mod.sys, "platform", "linux")
    cmd = ["pi", "--print", "hello"]
    assert wrap_command(cmd, backend_dir="/x", scummvm_port=1234) == cmd


def test_wrap_command_jails_on_darwin(monkeypatch, tmp_path) -> None:
    monkeypatch.setattr(sandbox_mod.sys, "platform", "darwin")
    backend_dir = tmp_path / "backend"
    backend_dir.mkdir()
    game = tmp_path / "games" / "monkey"
    game.mkdir(parents=True)

    cmd = ["claude", "--print", "go"]
    wrapped = wrap_command(
        cmd,
        backend_dir=str(backend_dir),
        scummvm_port=4321,
        deny_paths=(str(game), None),
    )

    assert wrapped[0] == "sandbox-exec"
    assert wrapped[1] == "-f"
    profile = (tmp_path / "backend").glob("sandbox_*.sb")
    text = next(profile).read_text()
    assert wrapped[3:] == cmd
    # Blocks the raw ScummVM port and the answer-key / game / backend paths.
    # SBPL only allows ``localhost`` as the host; it still matches 127.0.0.1.
    assert 'remote tcp "localhost:4321"' in text
    assert "goals" in text
    assert str(game.resolve()) in text
    assert str(backend_dir.resolve()) in text
