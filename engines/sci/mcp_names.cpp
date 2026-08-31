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


#include "sci/mcp_names.h"

#include "common/str.h"

namespace Sci {

// The prefixes SCI authors put on bookkeeping objects. Matched on the whole
// name or on the start of it, case-insensitively, and only after the name has
// been checked against the shorter list of exact ones below — "sound" is a
// thing in a room often enough that only an exact "sound" is written off.
static const char *const kInternalPrefixes[] = {
	"aMover", "aPolyPath", "aAvoider", "aScript", "aChangeState", "aRegionScript",
	"cycler", "aCycler", "sound", "aSound", "theMusic", "aTimer", "aScaler",
	"aList", "aSet", "aCode", "aPlane", "aCast",
	// Text and border objects a game puts in the cast to draw with. They are
	// on screen, but they are the screen furniture rather than things in the
	// room, and an agent offered "dtext" as something to look at is being
	// offered a distraction.
	"dtext", "dText", "theText", "aText", "bord", "border", "aBorder", nullptr
};

// Names that are bookkeeping exactly, and something else when they are longer.
static const char *const kInternalExact[] = {
	"sound", "music", "timer", "cycler", "mover", "script", "code", "plane",
	"cast", "list", "set", "region", "polygon", "theGame", "game", nullptr
};

static bool equalsNoCase(const Common::String &a, const char *b) {
	return a.equalsIgnoreCase(b);
}

bool sciIsInternalName(const Common::String &scriptName) {
	if (scriptName.empty())
		return true;
	for (int i = 0; kInternalExact[i] != nullptr; i++) {
		if (equalsNoCase(scriptName, kInternalExact[i]))
			return true;
	}
	for (int i = 0; kInternalPrefixes[i] != nullptr; i++) {
		const Common::String prefix(kInternalPrefixes[i]);
		if (scriptName.size() >= prefix.size() &&
		    Common::String(scriptName.c_str(), prefix.size()).equalsIgnoreCase(prefix.c_str()))
			return true;
	}
	return false;
}

Common::String sciObjectName(const Common::String &scriptName) {
	// Split camel humps, keep digits with the word they trail, drop the rest.
	Common::String out;
	bool previousLower = false;
	for (uint i = 0; i < scriptName.size(); i++) {
		const char c = scriptName[i];
		if (c >= 'A' && c <= 'Z') {
			// A hump only starts a word when something lower-case came before
			// it, so an all-caps run ("GK1") stays one word.
			if (previousLower && !out.empty())
				out += '_';
			out += (char)(c - 'A' + 'a');
			previousLower = false;
			continue;
		}
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
			out += c;
			previousLower = (c >= 'a' && c <= 'z');
			continue;
		}
		// Everything else is a separator, and separators never double up.
		if (!out.empty() && out.lastChar() != '_')
			out += '_';
		previousLower = false;
	}
	while (!out.empty() && out.lastChar() == '_')
		out.deleteLastChar();
	// "theShovel" and "shovel" are the same thing said two ways.
	if (out.size() > 4 && out.hasPrefix("the_"))
		out = Common::String(out.c_str() + 4);
	return out;
}

Common::String sciFallbackName(const char *kind, int id) {
	return Common::String::format("%s_%d", kind != nullptr ? kind : "object", id);
}

Common::String sciDisambiguate(const Common::String &name, uint occurrence) {
	if (occurrence == 0)
		return name;
	return Common::String::format("%s_%u", name.c_str(), occurrence + 1);
}

Common::String sciRoomName(const Common::String &roomObjectName) {
	return sciObjectName(roomObjectName);
}

} // End of namespace Sci
