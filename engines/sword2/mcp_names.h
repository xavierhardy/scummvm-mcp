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

#ifndef SWORD2_MCP_NAMES_H
#define SWORD2_MCP_NAMES_H

#include "common/str.h"

namespace Sword2 {

// Naming helpers for the MCP bridge. Deliberately free of any Sword2Engine
// dependency so the unit test links them without the engine runtime — the same
// convention as engines/mcp_bridge_text.cpp.
//
// Unlike the first game, this one names things itself: every mouse-detection
// box may carry a text line the game shows next to the cursor when the object
// labels option is on. The bridge reads that line whatever the option says, so
// these helpers only have to fold it into an identifier and classify what the
// cursor shape says about the thing.

// What the cursor turning into `pointerRes` says the thing is: "floor" (walk
// there), "exit" (leaves the screen), "person" (can be talked to), "item"
// (can be picked up), "scroll" (the screen edge) or "object". Never null.
const char *sword2PointerKind(int32 pointerRes);

// True for the cursors that mean "this leaves the screen".
bool sword2PointerIsExit(int32 pointerRes);

// True for the plain floor cursor, i.e. the thing a walk is aimed at.
bool sword2PointerIsFloor(int32 pointerRes);

// Fold a raw string — an on-screen label, or a resource's authored name — into
// an identifier: lower case, spaces and dashes as underscores, everything else
// dropped. Empty in (or nothing left), empty out.
Common::String sword2CleanName(const Common::String &raw);

// Name for a thing the game leaves unlabelled, so it can still be targeted.
Common::String sword2FallbackName(int32 id);

// Given a name and how many things before it already claimed that same name,
// the name to publish: the first keeps it, later ones are suffixed
// ("door", "door_2", "door_3").
Common::String sword2Disambiguate(const Common::String &name, uint occurrence);

} // End of namespace Sword2

#endif
