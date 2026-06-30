"""The `pi` harness runner.

Points the `pi` agent at the session's FastMCP proxy by writing a project-local
``.mcp.json`` (read by the ``pi-mcp-adapter`` extension) into pi's working
directory, launches ``pi --print --mode json`` with the game prompt, and streams
its JSONL events live to ``pi.jsonl`` for later analysis.
"""

import json
import os

from .base import RunContext, launch_agent
from .prompts import build_prompt
from .sandbox import build_agent_env, wrap_command

# Tools surfaced directly in the agent's tool list (vs. the adapter proxy tool).
DIRECT_TOOLS = ["state", "act", "walk", "answer", "skip"]


class PiHarness:
    """Launches the `pi` coding agent against the bench proxy.

    The agent is locked to the MCP tools: ``--no-builtin-tools`` drops pi's
    read/bash/edit/write/find/grep/ls while keeping the ``pi-mcp-adapter``
    extension that surfaces the proxy's tools. It is launched in a scrubbed
    environment and (on macOS) inside a ``sandbox-exec`` jail so it cannot read
    the answer key / game data or reach the raw ScummVM port.
    """

    def __init__(
        self,
        pi_bin: str = "pi",
        extra_args: list[str] | None = None,
        server_name: str = "scummvm",
    ) -> None:
        self.pi_bin = pi_bin
        self.extra_args = extra_args or [
            "--no-builtin-tools",
            "--no-skills",
            "--no-themes",
            "--no-context-files",
            "--no-session",
        ]
        self.server_name = server_name

    def run(self, ctx: RunContext) -> str | None:
        if not ctx.spec.provider or not ctx.spec.model:
            return "pi harness requires both --provider and --model"

        self._write_mcp_json(ctx.bench_port, ctx.agent_dir)
        config_dir = ctx.pi_config_dir or os.path.join(ctx.agent_dir, "pi-home")
        os.makedirs(config_dir, exist_ok=True)
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
        args = wrap_command(
            args,
            backend_dir=ctx.backend_dir,
            scummvm_port=ctx.scummvm_port,
            deny_paths=(ctx.game_path, ctx.save_folder),
        )
        env = build_agent_env({"PI_CODING_AGENT_DIR": config_dir})
        jsonl_path = os.path.join(ctx.agent_dir, "pi.jsonl")
        try:
            return launch_agent(ctx, args, jsonl_path, env, label="pi")
        except FileNotFoundError:
            return f"pi binary not found: {self.pi_bin!r}"
        except Exception as exc:  # noqa: BLE001
            return f"{type(exc).__name__}: {exc}"

    def _write_mcp_json(self, bench_port: int, agent_dir: str) -> None:
        config = {
            "mcpServers": {
                self.server_name: {
                    "url": f"http://127.0.0.1:{bench_port}/mcp",
                    "directTools": DIRECT_TOOLS,
                }
            }
        }
        path = os.path.join(agent_dir, ".mcp.json")
        with open(path, "w") as handle:
            json.dump(config, handle, indent=2)
