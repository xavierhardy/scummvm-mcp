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


#include "ags/mcp_names.h"

namespace AGS3 {

// Fold a raw name into an identifier: lower case, camel humps split, and one
// underscore wherever anything else was.
static Common::String fold(const Common::String &raw) {
	Common::String out;
	bool wordOpen = false;
	for (uint i = 0; i < raw.size(); i++) {
		const char c = raw[i];
		if (c >= 'A' && c <= 'Z') {
			// A capital starts a word when the run before it has ended, so
			// "FrontDoor" is two words but "TV" stays one.
			if (wordOpen && !out.empty())
				out += '_';
			out += (char)(c - 'A' + 'a');
			wordOpen = false;
			continue;
		}
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
			out += c;
			wordOpen = true;
			continue;
		}
		// An apostrophe joins rather than separates: "Zak's" is one word.
		if (c == '\'')
			continue;
		if (!out.empty() && out.lastChar() != '_')
			out += '_';
		wordOpen = false;
	}
	while (!out.empty() && out.lastChar() == '_')
		out.deleteLastChar();
	return out;
}

Common::String agsDisplayName(const Common::String &name) {
	return fold(name);
}

Common::String agsScriptName(const Common::String &scriptName) {
	if (scriptName.size() < 2)
		return fold(scriptName);
	const char first = scriptName[0];
	const char second = scriptName[1];
	// The AGS convention is one lower-case letter of type in front of a
	// capitalised name. Only strip it when that is really the shape: a script
	// name that is simply a lower-case word keeps all of itself.
	const bool prefixed = (first == 'o' || first == 'h' || first == 'c' ||
	                       first == 'i' || first == 'g') &&
	                      (second >= 'A' && second <= 'Z');
	return fold(prefixed ? Common::String(scriptName.c_str() + 1) : scriptName);
}

Common::String agsThingName(const Common::String &displayName,
                            const Common::String &scriptName) {
	// The display name first: it is what the game calls the thing when it
	// talks to a player, so it is what an agent reading the game's own words
	// will recognise.
	const Common::String shown = agsDisplayName(displayName);
	if (!shown.empty())
		return shown;
	return agsScriptName(scriptName);
}

bool agsIsPlaceholderName(const Common::String &name, int id) {
	if (name.empty())
		return true;
	// The editor's own defaults: "Hotspot 4", "Object 2", "Character 7", and
	// the folded forms of them.
	static const char *const kKinds[] = { "hotspot", "object", "character", nullptr };
	const Common::String folded = agsDisplayName(name);
	for (int i = 0; kKinds[i] != nullptr; i++) {
		if (folded == Common::String::format("%s_%d", kKinds[i], id))
			return true;
	}
	return false;
}

Common::String agsFallbackName(const char *kind, int id) {
	return Common::String::format("%s_%d", kind != nullptr ? kind : "thing", id);
}

Common::String agsDisambiguate(const Common::String &name, uint occurrence) {
	if (occurrence == 0)
		return name;
	return Common::String::format("%s_%u", name.c_str(), occurrence + 1);
}

} // End of namespace AGS3
