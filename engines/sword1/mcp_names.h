/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef SWORD1_MCP_NAMES_H
#define SWORD1_MCP_NAMES_H

#include "common/str.h"

namespace Sword1 {

// Naming tables for the MCP bridge.
//
// Unlike SCUMM, Broken Sword attaches no name string to a scene object: there
// are no hover labels and the compacts carry no text id for a description. The
// only names the game data provides are the inventory items (via the pocket
// number) and a handful of symbolic compact ids hardcoded in sworddefs.h.
//
// So the bridge ships its own tables here, and every compact that is not in one
// falls back to "object_<section>_<index>" — derived straight from the compact
// id, which is fixed game data and therefore stable across saves and runs. That
// fallback is a first-class target name: an agent (or a developer authoring the
// tables with the `debug` tool's compact dump) can drive the whole game by it
// before any name has been written down.
//
// This translation unit is deliberately free of SwordEngine, ObjectMan and Menu
// so the unit tests can link it without the engine runtime — the same
// convention as engines/mcp_bridge_text.cpp. An engine adding MCP support for a
// game whose objects have no in-game names should follow the same pattern.

// Inventory item name for a pocket number (1..TOTAL_pockets). Null if unknown.
const char *sword1PocketName(int pocketNo);

// Pocket number for an inventory item name, or 0 if unknown.
int sword1PocketNumber(const Common::String &name);

// Authored name for a compact id, or null when the id is not in the tables.
const char *sword1CompactName(uint32 id);

// Human-readable screen name, or null when unknown.
const char *sword1ScreenName(uint32 screen);

// Conversation topic name for a subject id (BASE_SUBJECT + n), or null.
const char *sword1SubjectName(uint32 subjectId);

// Full resolver: the authored name if there is one, else the stable
// "object_<section>_<index>" fallback.
Common::String sword1ObjectName(uint32 id);

// Reverse of sword1ObjectName. Accepts an authored name, the
// "object_<section>_<index>" fallback form, a decimal id, or an "0x"-prefixed
// hex id. The input is folded through MCP::McpBridge::normalizeActionName first,
// so case and spacing do not matter. Returns false when nothing matches.
bool sword1ResolveName(const Common::String &name, uint32 &idOut);

// Number of entries in the authored compact-name table, and accessors for them.
// Used by the unit tests to check that every authored name round-trips.
int sword1CompactNameCount();
void sword1CompactNameAt(int index, uint32 &idOut, const char *&nameOut);

} // End of namespace Sword1

#endif
