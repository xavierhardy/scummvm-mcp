# MCP Server Implementation Summary

## Overview

This document summarises the MCP (Model Context Protocol) server built into the ScummVM SCUMM engine. The server exposes game state and controls to AI agents over a standard HTTP interface, enabling large language models to observe and interact with classic LucasArts adventure games.

## Architecture

The implementation is split into two modules:

### Engine-agnostic transport — `backends/networking/mcp/`

`mcp_server.cpp` / `mcp_server.h`

Handles everything protocol-related:
- Non-blocking TCP listener bound to `127.0.0.1:23456` (port configurable)
- HTTP/1.1 request parsing (POST, GET, DELETE, OPTIONS/CORS preflight)
- JSON-RPC 2.0 envelope parsing and dispatch
- SSE (Server-Sent Events) streaming for long-running tool calls
- Session management and connection lifecycle
- Built-in method handlers: `initialize`, `tools/list`, `ping`
- Tool registration via `registerTool(ToolSpec, IToolHandler*)`
- Queues tool calls while a stream is active; drains them after the stream ends

This module has no dependency on SCUMM or any other engine. Any future engine can use it.

### SCUMM bridge — `engines/scumm/`

`mcp.cpp` / `mcp.h` — `class ScummMcpBridge`

Contains all SCUMM-specific logic:
- Tool implementations: `state` (sync), `act` / `answer` / `walk` (streaming)
- Entity resolution: maps object/actor names and numeric IDs to engine handles
- Verb lookup and normalisation (`normalizeActionName`)
- Action-completion detection: stuck-frame detection (90 frames), 600-frame timeout, 15-frame settling window
- Dialog message capture via `pushMessage` / `onActorLine` / `onSystemLine`
- Pre/post action state snapshotting for building change summaries
- SSE notification forwarding during streaming

## Configuration

Enable in `scummvm.ini`:

```ini
[monkey1]
mcp=true
```

Custom port (default `23456`):

```ini
[monkey1]
mcp=true
mcp_port=23456
```

Enable MCP debug output:

```bash
./scummvm --debuglevel=1 --debugflags=mcp monkey1
```

## Tools

| Tool    | Type      | Input | Description |
|---------|-----------|-------|-------------|
| `state`  | sync      | none  | Returns room, position, verbs, inventory, objects, actors, messages, question |
| `act`    | streaming | `verb`, `target1?`, `target2?` | Execute a verb on an object or actor; streams dialog; returns state diff |
| `answer` | streaming | `id`  | Select a dialog choice (1-based); streams conversation; returns state diff |
| `walk`   | streaming | `x`, `y` | Walk to pixel coordinates (auto-clamped); returns state diff |

### `act` input schema

```json
{
  "verb":    "string (required) — e.g. look_at, pick_up, talk_to, use, walk_to",
  "target1": "string | integer (optional) — primary target name or object ID",
  "target2": "string | integer (optional) — secondary target for two-object verbs"
}
```

Verb aliases: `walk`→`walk_to`, `look`→`look_at`, `pick`/`pickup`→`pick_up`, `talk`→`talk_to`. All verbs are case-insensitive and whitespace/hyphen-normalised.

### `walk` input schema

```json
{
  "x": "integer (required) — target X pixel coordinate, clamped to [0, roomWidth-1]",
  "y": "integer (required) — target Y pixel coordinate, clamped to [0, roomHeight-1]"
}
```

### State-change result (act / answer / walk)

```json
{
  "messages":          [{ "text": "...", "actor": "..." }],
  "inventory_added":   ["item"],
  "inventory_removed": ["item"],
  "room_changed":      5,
  "position":          { "x": 100, "y": 130 },
  "question":          { "choices": [{ "id": 1, "label": "..." }] }
}
```

## MCP Protocol Compliance

- Protocol version: `2025-03-26`
- Transport: Streamable HTTP (JSON-RPC 2.0 over HTTP + SSE)
- Server identity: `{ "name": "scummvm", "version": "1.0" }`
- Capabilities: `{ "tools": { "listChanged": true } }`
- Response format: `{ "content": [{ "type": "text", "text": "..." }], "structuredContent": { ... } }`

## Files

### New / added in this branch
- `backends/networking/mcp/mcp_server.h`
- `backends/networking/mcp/mcp_server.cpp`
- `backends/networking/mcp/module.mk`
- `engines/scumm/mcp.h`
- `engines/scumm/mcp.cpp`
- `test/engines/scumm/mcp.h` — CxxTest unit tests for `normalizeActionName`

### Modified
- `backends/module.mk` — registers `networking/mcp/mcp_server.o`
- `engines/scumm/module.mk` — `mcp.o` (renamed from `monkey_mcp.o`)
- `engines/scumm/scumm.h` — member type and forward declaration
- `engines/scumm/scumm.cpp` — config key, include, instantiation
- `engines/scumm/string.cpp` — message forwarding hooks
- `engines/scumm/actor.cpp` — actor line capture hook

### Removed
- `engines/scumm/monkey_mcp.cpp` (replaced by the two-module split above)
- `engines/scumm/monkey_mcp.h`
- `test/engines/scumm/monkey_mcp.h` (replaced by `mcp.h`)
- `test/mcp_test.cpp` (orphan google-test file with broken references)

## Unit Tests

`test/engines/scumm/mcp.h` covers `ScummMcpBridge::normalizeActionName`:

- Alias mappings (`walk`→`walk_to`, `look`→`look_at`, `pick`/`pickup`→`pick_up`, `talk`→`talk_to`)
- Pass-through for canonical verb names (`open`, `use`, `push`, `pull`, `close`, `give`, `walk_to`, `pick_up`)
- Case folding and whitespace/hyphen trimming (`"  Walk To  "`→`"walk_to"`, `"LOOK-AT"`→`"look_at"`)
- Empty and whitespace-only input returns `""`
