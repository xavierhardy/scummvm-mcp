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

#ifndef ASYLUM_MCP_NAMES_H
#define ASYLUM_MCP_NAMES_H

#include "common/str.h"

namespace Asylum {

// Naming helpers for the Sanitarium MCP bridge. Deliberately free of any
// engine dependency so the unit test links them without a running engine.
//
// Sanitarium is unusual among these games in that its data carries a name for
// every object already - not a label the player is shown, but the name the
// game's own authors gave it in the editor ("DOOR TO HALLWAY", "Chair_01").
// So these fold a builder's identifier into an agent's, the way the SCI
// helpers do with script names, rather than folding painted prose into one.

// Identifier for an object, from the name its data carries. Those names are
// upper-case with spaces, or CamelCase, or run together with underscores, so
// everything is lower-cased and the separators are made uniform:
// "DOOR TO HALLWAY" -> "door_to_hallway", "Chair_01" -> "chair_01". Returns an
// empty string when nothing survives.
Common::String asylumObjectName(const Common::String &dataName);

// Given a name and how many things before it already claimed that same name,
// the name to publish: the first keeps it, later ones are suffixed
// ("chair", "chair_2").
Common::String asylumDisambiguate(const Common::String &name, uint occurrence);

// True when an object's name is the editor's filler rather than a name. The
// game ships a great many objects called things like "0" or "xxx" or nothing
// at all - scenery the scripts move about - and offering those to an agent as
// things to try is offering it noise.
bool asylumIsPlaceholderName(const Common::String &dataName);

} // End of namespace Asylum

#endif
