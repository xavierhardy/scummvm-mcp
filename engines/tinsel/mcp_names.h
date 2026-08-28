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

#ifndef TINSEL_MCP_NAMES_H
#define TINSEL_MCP_NAMES_H

#include "common/str.h"

namespace Tinsel {

// Naming helpers for the Tinsel MCP bridge. Deliberately free of any engine
// dependency so the unit test links them without a running engine.
//
// Every name an agent sees comes from the game's own data: the label the game
// paints next to the cursor when the player points at something, and the data
// file a scene is loaded from. These functions only fold those raw strings
// into stable identifiers, and supply a fallback for the things the game
// leaves unnamed.

// Identifier for a scene, from the name of the file its data lives in
// ("KITCHEN.GRA" -> "kitchen", "us/mortuary.scn" -> "mortuary"). Empty in,
// empty out.
Common::String tinselSceneName(const Common::String &sceneFile);

// Identifier for a thing on screen, from the label the game paints for it.
// Lower-cased, spaces and dashes folded to underscores, everything else
// dropped ("The Librarian" -> "the_librarian", "Rincewind's hat!" ->
// "rincewinds_hat"). Returns an empty string when nothing survives.
Common::String tinselLabelToName(const Common::String &label);

// Name for something the game never labels, so it can still be targeted.
// `kind` is the sort of thing it is ("object", "exit", "actor", "item").
Common::String tinselFallbackName(const char *kind, int id);

// Given a name and how many things before it already claimed that same name,
// the name to publish: the first keeps it, later ones are suffixed
// ("door", "door_2", "door_3").
Common::String tinselDisambiguate(const Common::String &name, uint occurrence);

} // End of namespace Tinsel

#endif
