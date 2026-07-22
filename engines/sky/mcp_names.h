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

#ifndef SKY_MCP_NAMES_H
#define SKY_MCP_NAMES_H

#include "common/scummsys.h"

namespace Sky {

// Naming helpers for the Beneath a Steel Sky MCP bridge. Deliberately free of
// any engine dependency so the unit test can link them without a running
// engine. Runtime names come from the game's own data (cursorText strings and
// authored compact names); these tables only add what the data does not carry.

// True for compacts that are people Foster can talk to. Mirrors the NPC list
// the engine's touch UI uses for its "talk" icon.
bool skyIsCharacter(uint32 compactId);

// Human-readable name for a screen number, or nullptr when unnamed.
const char *skyScreenName(uint32 screen);

} // End of namespace Sky

#endif
