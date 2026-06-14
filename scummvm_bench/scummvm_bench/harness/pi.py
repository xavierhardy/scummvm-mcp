"""The `pi` harness runner.

Points the `pi` agent at the session's FastMCP proxy by writing a project-local
``.mcp.json`` (read by the ``pi-mcp-adapter`` extension) into pi's working
directory, launches ``pi --print --mode json`` with the game prompt, and streams
its JSONL events live to ``pi.jsonl`` for later analysis.
"""

import json
import os
import subprocess
import threading
import time

from .base import RunContext
from .prompts import build_prompt

# Tools surfaced directly in the agent's tool list (vs. the adapter proxy tool).
DIRECT_TOOLS = ["state", "act", "walk", "answer", "skip"]
# Extra wall-clock granted on top of the time limit before force-killing pi.
KILL_GRACE_S = 30.0
DEFAULT_DEADLINE_S = 1800.0


class PiHarness:
    """Launches the `pi` coding agent against the bench proxy."""

    def __init__(
        self,
        pi_bin: str = "pi",
        extra_args: list[str] | None = None,
        server_name: str = "scummvm",
    ) -> None:
        self.pi_bin = pi_bin
        self.extra_args = extra_args or [
            "--no-skills",
            "--no-themes",
            "--no-context-files",
            "--no-session",
        ]
        self.server_name = server_name

    def run(self, ctx: RunContext) -> str | None:
        if not ctx.spec.provider or not ctx.spec.model:
            return "pi harness requires both --provider and --model"

        self._write_mcp_json(ctx.bench_port, ctx.session_dir)
        prompt = build_prompt(ctx.spec)
        args = [
            self.pi_bin,
            "--print",
            "--mode",
            "json",
            "--provider",
            ctx.spec.provider,
            "--model",
            ctx.spec.model,
            *self.extra_args,
            prompt,
        ]
        jsonl_path = os.path.join(ctx.session_dir, "pi.jsonl")
        try:
            return self._launch(ctx, args, jsonl_path)
        except FileNotFoundError:
            return f"pi binary not found: {self.pi_bin!r}"
        except Exception as exc:  # noqa: BLE001
            return f"{type(exc).__name__}: {exc}"

    def _write_mcp_json(self, bench_port: int, session_dir: str) -> None:
        config = {
            "mcpServers": {
                self.server_name: {
                    "url": f"http://127.0.0.1:{bench_port}/mcp",
                    "directTools": DIRECT_TOOLS,
                }
            }
        }
        path = os.path.join(session_dir, ".mcp.json")
        with open(path, "w") as handle:
            json.dump(config, handle, indent=2)

    def _launch(self, ctx: RunContext, args: list[str], jsonl_path: str) -> str | None:
        deadline = time.monotonic() + (ctx.spec.time_limit_s or DEFAULT_DEADLINE_S)
        deadline += KILL_GRACE_S
        with open(jsonl_path, "w") as transcript:
            proc = subprocess.Popen(
                args,
                cwd=ctx.session_dir,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
            )
            pump = threading.Thread(
                target=_pump_stream,
                args=(proc.stdout, transcript),
                daemon=True,
            )
            pump.start()

            killed = False
            while proc.poll() is None:
                if ctx.stop_event.wait(0.2):
                    proc.kill()
                    killed = True
                    break
                if time.monotonic() > deadline:
                    proc.kill()
                    killed = True
                    break
            try:
                proc.wait(timeout=5)
            except Exception:  # noqa: BLE001
                pass
            pump.join(timeout=2)

        if killed:
            return None
        if proc.returncode not in (0, None):
            return f"pi exited with code {proc.returncode}"
        return None


def _pump_stream(stream, transcript) -> None:
    if stream is None:
        return
    for line in stream:
        transcript.write(line)
        transcript.flush()
