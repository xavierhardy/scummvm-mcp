#!/usr/bin/env python3
"""MCP integration test utilities.

Back-compat facade: the implementations now live in focused modules —
:mod:`mcp_client` (client + connection), :mod:`launcher` (ScummVM launch +
game-data/save resolution), and :mod:`state_helpers` (state/verb/dialog
inspection + readiness guards). This module re-exports them so existing
``from utils import ...`` imports keep working; new code may import the modules
directly.
"""

from launcher import (
    GAME_PATHS,
    has_captured_save,
    launch_scummvm,
    require_game_path,
    require_save_slot,
    save_slot_path,
)
from mcp_client import (
    MCP_CONNECT_TIMEOUT_SECS,
    MCP_HOST,
    MCP_PORT,
    MCP_PORT_BASE,
    MCP_TIMEOUT_SECS,
    MCP_TOOLS,
    McpClient,
    get_mcp_port,
    wait_for_mcp,
)
from state_helpers import (
    VerbActor,
    _find_object,
    _state_or_skip,
    _wait_until,
    bind_verb,
    choice_labels,
    find_choice_id,
    find_choice_id_containing,
    find_id,
    find_object_by_name,
    find_object_with_verb,
    get_state_with_retry,
    joined_message_text,
    make_verbs,
    message_texts,
    object_by_id,
    object_names,
    pathways,
    require_interactive,
    skip_intros,
    skip_unless,
    wait_for_interactive,
    wait_until_or_skip,
)

__all__ = [
    "GAME_PATHS",
    "MCP_CONNECT_TIMEOUT_SECS",
    "MCP_HOST",
    "MCP_PORT",
    "MCP_PORT_BASE",
    "MCP_TIMEOUT_SECS",
    "MCP_TOOLS",
    "McpClient",
    "VerbActor",
    "_find_object",
    "_state_or_skip",
    "_wait_until",
    "bind_verb",
    "choice_labels",
    "find_choice_id",
    "find_choice_id_containing",
    "find_id",
    "find_object_by_name",
    "find_object_with_verb",
    "get_mcp_port",
    "has_captured_save",
    "get_state_with_retry",
    "joined_message_text",
    "launch_scummvm",
    "make_verbs",
    "message_texts",
    "object_by_id",
    "object_names",
    "pathways",
    "require_game_path",
    "require_interactive",
    "require_save_slot",
    "save_slot_path",
    "skip_intros",
    "skip_unless",
    "wait_for_interactive",
    "wait_for_mcp",
    "wait_until_or_skip",
]
