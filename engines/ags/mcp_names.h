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


#ifndef AGS_MCP_NAMES_H
#define AGS_MCP_NAMES_H

#include "common/str.h"

namespace AGS3 {

// Naming helpers for the AGS MCP bridge. Deliberately free of any engine
// dependency so the unit test links them without a running engine.
//
// AGS is the friendliest engine here to name things in: everything in a room
// carries two names from the moment the game was authored - a display name
// meant for a player ("Front Door", "Zak's bed") and a script name meant for
// the game's own code ("oFrontDoor", "hDoor", "cZak"). Neither is quite what
// an agent wants. The display name is prose, and the script name carries the
// author's type prefix. These fold both into one identifier and prefer the
// display name, because that is what the game calls the thing when it talks
// to a player.

// Identifier for something in a room, from the name the author gave it.
// Lower-cased, runs of anything that is not a letter or digit folded to one
// underscore, and camel humps split ("Front Door" -> "front_door",
// "Zak's bed" -> "zaks_bed"). Empty in, empty out.
Common::String agsDisplayName(const Common::String &name);

// Identifier from a script name, with the author's type prefix taken off.
// AGS convention puts one letter in front: o for a room object, h for a
// hotspot, c for a character, i for an inventory item ("oFrontDoor" ->
// "front_door", "cZak" -> "zak"). The prefix only comes off when what
// follows starts with a capital, so a script name that is simply a lower-case
// word ("door") keeps all of itself.
Common::String agsScriptName(const Common::String &scriptName);

// The identifier to publish for a thing, given both of its names: the display
// name when there is one, the script name when there is not. Empty when the
// author left it nameless, which is how a thing says it is not worth
// offering.
Common::String agsThingName(const Common::String &displayName,
                            const Common::String &scriptName);

// True when a name is one the AGS editor supplied rather than one the author
// typed. A hotspot nobody named is still called "Hotspot 4" in the data, and
// offering that to an agent is offering a number dressed as a name - the
// thing has no description, no interaction, and nothing to say for itself.
bool agsIsPlaceholderName(const Common::String &name, int id);

// Name for something with no name at all, so it can still be targeted.
// `kind` is the sort of thing it is ("object", "hotspot", "character").
Common::String agsFallbackName(const char *kind, int id);

// Given a name and how many things before it already claimed that same name,
// the name to publish: the first keeps it, later ones are suffixed
// ("door", "door_2", "door_3").
Common::String agsDisambiguate(const Common::String &name, uint occurrence);

} // End of namespace AGS3

#endif
