#!/usr/bin/env python3
"""
Interactive MCP CLI for debugging ScummVM games.

Connects to a running ScummVM instance with the MCP server enabled and offers
a small REPL/one-shot interface to call any of the registered MCP tools,
including the new debug tools (`debug`, `keystroke`, `mouse_move`, `mouse_click`).

Usage examples:

  # one-shot
  python mcp_cli.py --port 23462 state
  python mcp_cli.py --port 23462 debug --vars 250-263
  python mcp_cli.py --port 23462 mouse_click 140 134 --double
  python mcp_cli.py --port 23462 keystroke c
  python mcp_cli.py --port 23462 act interact egg

  # interactive REPL (blank command exits)
  python mcp_cli.py --port 23462

  # auto-launch ScummVM on a per-game save and connect
  python mcp_cli.py --launch pass --port 23462
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import sys
from typing import Any

from utils import (
    GAME_PATHS,
    MCP_HOST,
    MCP_CONNECT_TIMEOUT_SECS,
    MCP_TIMEOUT_SECS,
    McpClient,
    launch_scummvm,
    require_game_path,
    wait_for_mcp,
)


def _post_tool(client: McpClient, name: str, arguments: dict[str, Any]) -> Any:
    """Generic tools/call wrapper that returns the inner JSON result.

    Falls back to the streaming SSE path automatically when the server
    advertises streaming for the tool.
    """
    payload = {
        "jsonrpc": "2.0",
        "id": client._next_id(),
        "method": "tools/call",
        "params": {"name": name, "arguments": arguments},
    }
    headers = client._headers({"Accept": "application/json, text/event-stream"})
    with client._client.stream("POST", client._url, json=payload, headers=headers) as resp:
        ctype = resp.headers.get("content-type", "")
        if "text/event-stream" in ctype:
            return client._decode_stream_response(resp=resp, tool=name)
        body = b"".join(resp.iter_bytes())
        if resp.status_code >= 400:
            raise RuntimeError(f"{name} HTTP {resp.status_code}: {body[:200]!r}")
        data = json.loads(body)
        if "error" in data:
            raise RuntimeError(f"{name} error: {data['error']}")
        return client._extract_result(data)


def cmd_state(client: McpClient, _: argparse.Namespace) -> Any:
    return client.state()


def cmd_debug(client: McpClient, args: argparse.Namespace) -> Any:
    arguments: dict[str, Any] = {}
    if args.vars:
        if "-" in args.vars:
            a, b = args.vars.split("-", 1)
            arguments["from"] = int(a)
            arguments["to"] = int(b)
        else:
            arguments["from"] = int(args.vars)
            arguments["to"] = int(args.vars)
    return _post_tool(client, "debug", arguments)


def cmd_act(client: McpClient, args: argparse.Namespace) -> Any:
    return client.act(args.verb, args.target1, args.target2)


def cmd_walk(client: McpClient, args: argparse.Namespace) -> Any:
    return client.walk(args.x, args.y)


def cmd_skip(client: McpClient, _: argparse.Namespace) -> Any:
    return client.skip()


def cmd_note(client: McpClient, args: argparse.Namespace) -> Any:
    return client.play_note(args.note)


def cmd_answer(client: McpClient, args: argparse.Namespace) -> Any:
    return client.answer(args.id)


def cmd_keystroke(client: McpClient, args: argparse.Namespace) -> Any:
    arguments: dict[str, Any] = {"key": args.key}
    if args.ctrl:
        arguments["ctrl"] = True
    if args.shift:
        arguments["shift"] = True
    if args.alt:
        arguments["alt"] = True
    return _post_tool(client, "keystroke", arguments)


def cmd_mouse_move(client: McpClient, args: argparse.Namespace) -> Any:
    return _post_tool(client, "mouse_move", {"x": args.x, "y": args.y})


def cmd_mouse_click(client: McpClient, args: argparse.Namespace) -> Any:
    arguments: dict[str, Any] = {"x": args.x, "y": args.y}
    if args.button != "left":
        arguments["button"] = args.button
    if args.double:
        arguments["double"] = True
    return _post_tool(client, "mouse_click", arguments)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="ScummVM MCP CLI")
    p.add_argument("--host", default=MCP_HOST)
    p.add_argument("--port", type=int, default=23456)
    p.add_argument("--connect-timeout", type=float, default=MCP_CONNECT_TIMEOUT_SECS)
    p.add_argument("--timeout", type=float, default=MCP_TIMEOUT_SECS)
    p.add_argument("--launch", help="Launch a known game id (e.g. 'pass') before connecting.")

    sub = p.add_subparsers(dest="cmd")

    sub.add_parser("state").set_defaults(fn=cmd_state)

    pdebug = sub.add_parser("debug")
    pdebug.add_argument("--vars", help="Var index range, e.g. 250-263 or 50.")
    pdebug.set_defaults(fn=cmd_debug)

    pact = sub.add_parser("act")
    pact.add_argument("verb")
    pact.add_argument("target1", nargs="?", default=None)
    pact.add_argument("target2", nargs="?", default=None)
    pact.set_defaults(fn=cmd_act)

    pwalk = sub.add_parser("walk")
    pwalk.add_argument("x", type=int)
    pwalk.add_argument("y", type=int)
    pwalk.set_defaults(fn=cmd_walk)

    sub.add_parser("skip").set_defaults(fn=cmd_skip)

    pnote = sub.add_parser("note")
    pnote.add_argument("note")
    pnote.set_defaults(fn=cmd_note)

    panswer = sub.add_parser("answer")
    panswer.add_argument("id", type=int)
    panswer.set_defaults(fn=cmd_answer)

    pkey = sub.add_parser("keystroke")
    pkey.add_argument("key")
    pkey.add_argument("--ctrl", action="store_true")
    pkey.add_argument("--shift", action="store_true")
    pkey.add_argument("--alt", action="store_true")
    pkey.set_defaults(fn=cmd_keystroke)

    pmm = sub.add_parser("mouse_move")
    pmm.add_argument("x", type=int)
    pmm.add_argument("y", type=int)
    pmm.set_defaults(fn=cmd_mouse_move)

    pmc = sub.add_parser("mouse_click")
    pmc.add_argument("x", type=int)
    pmc.add_argument("y", type=int)
    pmc.add_argument("--button", choices=("left", "right", "middle"), default="left")
    pmc.add_argument("--double", action="store_true")
    pmc.set_defaults(fn=cmd_mouse_click)

    return p


def maybe_launch(args: argparse.Namespace):
    """If --launch is set, start ScummVM with mcp + mcp_debug; return Popen.

    Note: launch_scummvm reads test/mcp/ini_files/scummvm_<game>.ini, which
    must enable both `mcp=true` and `mcp_debug=true` for the debug tools to
    show up.
    """
    if not args.launch:
        return None
    require_game_path(args.launch)

    scummvm_binary = os.path.join(os.path.dirname(__file__), "..", "..", "scummvm")
    proc = launch_scummvm(
        args.launch,
        GAME_PATHS[args.launch],
        port=args.port,
        scummvm_binary=scummvm_binary,
    )
    return proc


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    proc = maybe_launch(args)
    try:
        client = wait_for_mcp(args.host, args.port,
                              connect_timeout=args.connect_timeout,
                              timeout=args.timeout)

        if args.cmd:
            result = args.fn(client, args)
            print(json.dumps(result, indent=2, default=str))
            return 0

        print(f"Connected to {args.host}:{args.port}. "
              "Commands: state | debug [--vars FROM-TO] | act <verb> [t1] [t2] | "
              "walk X Y | skip | note <c|d|e|...|C> | answer <id> | "
              "keystroke <key> [--ctrl --shift --alt] | "
              "mouse_move X Y | mouse_click X Y [--button left|right|middle] [--double] | "
              "q to quit.")
        while True:
            try:
                line = input("mcp> ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                break
            if not line or line in ("q", "quit", "exit"):
                break
            try:
                tokens = shlex.split(line)
            except ValueError as e:
                print(f"parse error: {e}")
                continue
            try:
                ns = parser.parse_args(tokens)
            except SystemExit:
                continue
            if not getattr(ns, "fn", None):
                print(f"unknown command: {line}")
                continue
            try:
                result = ns.fn(client, ns)
                print(json.dumps(result, indent=2, default=str))
            except Exception as e:
                print(f"error: {e}")
        return 0
    finally:
        if proc is not None:
            try:
                proc.kill()
                proc.wait(timeout=5)
            except Exception:
                pass
            for f in ("_stdout_file", "_stderr_file"):
                if hasattr(proc, f):
                    getattr(proc, f).close()


if __name__ == "__main__":
    sys.exit(main())
