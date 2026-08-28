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

#include "tinsel/mcp_names.h"

namespace Tinsel {

// Fold one raw character into the identifier alphabet. Returns 0 for a
// character that carries no meaning (punctuation, control codes).
static char foldChar(char c) {
	if (c >= 'A' && c <= 'Z')
		return c - 'A' + 'a';
	if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
		return c;
	if (c == ' ' || c == '-' || c == '_' || c == '\t')
		return '_';
	return 0;
}

// Lower-case, fold separators to '_', drop everything else, and collapse runs
// of '_' (including leading and trailing ones).
static Common::String foldToIdentifier(const Common::String &raw) {
	Common::String out;
	for (uint i = 0; i < raw.size(); i++) {
		char c = foldChar(raw[i]);
		if (c == 0)
			continue;
		if (c == '_' && (out.empty() || out[out.size() - 1] == '_'))
			continue;
		out += c;
	}
	while (!out.empty() && out[out.size() - 1] == '_')
		out.deleteLastChar();
	return out;
}

Common::String tinselSceneName(const Common::String &sceneFile) {
	// The handle table stores a plain file name; some builds prefix it with a
	// language folder. Take the base name, drop the extension.
	uint start = 0;
	for (uint i = 0; i < sceneFile.size(); i++)
		if (sceneFile[i] == '/' || sceneFile[i] == '\\')
			start = i + 1;

	Common::String base;
	for (uint i = start; i < sceneFile.size(); i++) {
		if (sceneFile[i] == '.')
			break;
		base += sceneFile[i];
	}
	return foldToIdentifier(base);
}

Common::String tinselLabelToName(const Common::String &label) {
	return foldToIdentifier(label);
}

Common::String tinselFallbackName(const char *kind, int id) {
	return Common::String::format("%s_%d", kind ? kind : "thing", id);
}

Common::String tinselDisambiguate(const Common::String &name, uint occurrence) {
	if (occurrence == 0)
		return name;
	return Common::String::format("%s_%u", name.c_str(), occurrence + 1);
}

} // End of namespace Tinsel
