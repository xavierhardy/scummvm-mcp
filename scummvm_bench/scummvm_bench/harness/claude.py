"""The Claude Code (`claude`) harness runner.

Mirrors :class:`PiHarness`: points ``claude`` at the session's FastMCP proxy via
a project-local MCP config, launches it non-interactively, and streams its JSONL
events to ``claude.jsonl``.

The agent is locked to the MCP tools: ``--tools`` lists only the
``mcp__scummvm__*`` tools, so no built-in tool (Bash/Read/Write/WebFetch/…)
exists for the session, and ``--strict-mcp-config`` ignores the user's global MCP
servers. Permissions are bypassed (``--dangerously-skip-permissions``) so the
agent never stalls on a prompt in ``--print`` mode — safe because the tool list
already excludes everything dangerous and the run is wrapped in a
``sandbox-exec`` jail (on macOS) with a scrubbed environment.
"""

import json
import os

from .base import RunContext, launch_agent
from .prompts import build_prompt
from .sandbox import build_agent_env, wrap_command

# The proxy's tool surface (see ``proxy.py:_build_app``). Claude namespaces MCP
# tools as ``mcp__<server>__<tool>``.
_PROXY_TOOLS = (
    "state",
    "act",
    "answer",
    "walk",
    "skip",
    "play_note",
    "switch_character",
    "dial",
    "shoot_cannon",
    "keystroke",
)


class ClaudeCodeHarness:
    """Launches the `claude` coding agent against the bench proxy."""

    def __init__(
        self,
        claude_bin: str = "claude",
        server_name: str = "scummvm",
    ) -> None:
        self.claude_bin = claude_bin
        self.server_name = server_name

    def run(self, ctx: RunContext) -> str | None:
        if not ctx.spec.model:
            return "claude harness requires --model provider/model"

        self._write_mcp_json(ctx.bench_port, ctx.agent_dir)
        config_dir = ctx.claude_config_dir or os.path.join(ctx.agent_dir, "claude-home")
        os.makedirs(config_dir, exist_ok=True)
        prompt = build_prompt(ctx.spec)
        tools = ",".join(f"mcp__{self.server_name}__{name}" for name in _PROXY_TOOLS)
        args = [
            self.claude_bin,
            "--print",
            "--output-format",
            "stream-json",
            "--verbose",
            "--model",
            ctx.spec.model,
            "--mcp-config",
            os.path.join(ctx.agent_dir, "claude.mcp.json"),
            "--strict-mcp-config",
            "--tools",
            tools,
            "--dangerously-skip-permissions",
            prompt,
        ]
        args = wrap_command(
            args,
            backend_dir=ctx.backend_dir,
            scummvm_port=ctx.scummvm_port,
            deny_paths=(ctx.game_path, ctx.save_folder),
        )
        env = build_agent_env({"CLAUDE_CONFIG_DIR": config_dir, "IS_SANDBOX": "1"})
        jsonl_path = os.path.join(ctx.agent_dir, "claude.jsonl")
        try:
            return launch_agent(ctx, args, jsonl_path, env, label="claude")
        except FileNotFoundError:
            return f"claude binary not found: {self.claude_bin!r}"
        except Exception as exc:  # noqa: BLE001
            return f"{type(exc).__name__}: {exc}"

    def _write_mcp_json(self, bench_port: int, agent_dir: str) -> None:
        config = {
            "mcpServers": {
                self.server_name: {
                    "type": "http",
                    "url": f"http://127.0.0.1:{bench_port}/mcp",
                }
            }
        }
        path = os.path.join(agent_dir, "claude.mcp.json")
        with open(path, "w") as handle:
            json.dump(config, handle, indent=2)
