/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

// MCP action-name normalization now lives in engines/mcp_bridge_text.cpp, shared
// with the other engines that host an MCP server. ScummMcpBridge inherits
// MCP::McpBridge::normalizeActionName(), so every call site — including
// test/engines/scumm/mcp.h — keeps resolving unchanged.
//
// This translation unit survives only for Scumm::mcpStripNamePadding, which has
// external linkage (it is called from safeUtf8() in mcp.cpp and forward-declared
// by the unit tests). Like the shared TU, it stays free of any ScummEngine
// dependency so the cxxtest runner can link it without the whole engine.

#include "common/str.h"

#include "engines/mcp_bridge.h"

namespace Scumm {

// SCUMM pads object names to a fixed width with trailing '@' bytes (the charset
// renderer draws '@' as nothing), e.g. the EGA demo's "roter Hering@@@@@...".
Common::String mcpStripNamePadding(const Common::String &s) {
	return MCP::mcpStripNamePadding(s);
}

} // End of namespace Scumm
