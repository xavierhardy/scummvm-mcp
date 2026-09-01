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


#ifndef MOHAWK_MCP_NAMES_H
#define MOHAWK_MCP_NAMES_H

#include "common/str.h"

namespace Mohawk {

// Naming helpers for the Mohawk MCP bridge. Deliberately free of any engine
// dependency so the unit test links them without a running engine.
//
// Where in Time is Carmen Sandiego? names what the player can point at in the
// one place a player ever sees it: the line the game writes when the cursor
// rests on something. Each hotspot carries the number of that line, and the
// case holds the lines, so a name here is the game's own rollover text folded
// into an identifier.

// Identifier for something in a scene, from the line the game shows for it.
// Lower-cased, runs of anything that is not a letter or digit folded to one
// underscore ("Ancient Egyptian Vase" -> "ancient_egyptian_vase"). A trailing
// full stop or ellipsis - these lines are written as prose - is dropped.
// Empty in, empty out.
Common::String mohawkLabelToName(const Common::String &label);

// Name for something the game leaves unlabelled, so it can still be targeted.
// `kind` is the sort of thing it is ("hotspot", "character", "item").
Common::String mohawkFallbackName(const char *kind, int id);

// Given a name and how many things before it already claimed that same name,
// the name to publish: the first keeps it, later ones are suffixed
// ("vase", "vase_2", "vase_3").
Common::String mohawkDisambiguate(const Common::String &name, uint occurrence);

} // End of namespace Mohawk

#endif
