#!/usr/bin/env python3
"""
MCP integration test utilities: client, ScummVM launcher, shared helpers.
"""

import argparse
import json
from errno import EINVAL, EIO, EPERM
from utils import (
    wait_for_mcp,
    MCP_TOOLS,
    MCP_HOST,
    MCP_PORT,
    MCP_TIMEOUT_SECS,
    MCP_CONNECT_TIMEOUT_SECS,
)


MCP_TOOLS_STR = ", ".join(MCP_TOOLS)


def parse_arguments():
    arg_parser = argparse.ArgumentParser("ScummVM MCP Client")
    arg_parser.add_argument("--host", help="MCP host", default=MCP_HOST, type=str)
    arg_parser.add_argument("--port", help="MCP port", default=MCP_PORT, type=int)
    arg_parser.add_argument(
        "--timeout", help="MCP timeout", default=MCP_TIMEOUT_SECS, type=float
    )
    arg_parser.add_argument(
        "--connect-timeout",
        help="MCP connection timeout",
        default=MCP_CONNECT_TIMEOUT_SECS,
        type=float,
    )
    tool_subparser = arg_parser.add_subparsers(help="tools", dest="tool")
    tool_subparser.add_parser("state", help="Get current state")
    answer_subparser = tool_subparser.add_parser("answer", help="Answer dialog")
    answer_subparser.add_argument(
        "choice", help="answer to select (integer greater or equal to 1)", type=int
    )

    act_subparser = tool_subparser.add_parser("act", help="Act")
    act_subparser.add_argument("verb", help="Action verb", type=str)
    act_subparser.add_argument("target1", help="First target of action", type=str)
    act_subparser.add_argument(
        "target2",
        help="Second target of action (may not be required)",
        type=str,
        nargs="?",
        default="",
    )

    walk_subparser = tool_subparser.add_parser("walk", help="Walk")
    walk_subparser.add_argument("x", help="X position", type=int)
    walk_subparser.add_argument("y", help="Y position", type=int)
    return arg_parser.parse_args()


def convert_target(target):
    if not target:
        return None
    elif target.isdigit():
        return int(target)

    return target


if __name__ == "__main__":
    arguments = parse_arguments()
    tool = arguments.tool

    if not tool:
        print(f"tool must be one of {MCP_TOOLS_STR}")
        exit(EINVAL)

    try:
        mcp_client = wait_for_mcp(
            arguments.host,
            arguments.port,
            connect_timeout=arguments.connect_timeout,
            timeout=arguments.timeout,
        )
    except Exception as exc:
        print(exc)
        exit(EIO)

    try:
        if tool == "state":
            result = mcp_client.state()
        elif tool == "act":
            if arguments.target1 in ("to", "in", "off", "on", "is"):
                verb = f"{arguments.verb}_{arguments.target1}"
                target1 = arguments.target2
                target2 = ""
            else:
                verb = arguments.verb
                target1 = arguments.target1
                target2 = arguments.target2

            result = mcp_client.act(
                verb,
                convert_target(target1),
                convert_target(target2),
            )
        elif tool == "answer":
            if arguments.choice < 1:
                print(
                    f"answer tool needs an integer equal or greater than 1, got: {arguments.choice}"
                )
                exit(EINVAL)

            result = mcp_client.answer(arguments.choice)
        else:
            errors = []
            if arguments.x < 0:
                errors.append(
                    f"x to be an integer equal or greater than 0, got: {arguments.x}"
                )

            if arguments.y < 0:
                errors.append(
                    f"y to be an integer equal or greater than 0, got: {arguments.y}"
                )

            if errors:
                print(f"walk tool needs {' and '.join(errors)}")
                exit(EINVAL)

            result = mcp_client.walk(arguments.x, arguments.y)

        print(json.dumps(result, indent=2))
    except Exception as exc:
        print(exc)
        exit(EPERM)
