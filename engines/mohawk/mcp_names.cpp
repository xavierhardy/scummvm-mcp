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


#include "mohawk/mcp_names.h"

namespace Mohawk {

Common::String mohawkLabelToName(const Common::String &label) {
	Common::String out;
	for (uint i = 0; i < label.size(); i++) {
		const char c = label[i];
		if (c >= 'A' && c <= 'Z') {
			out += (char)(c - 'A' + 'a');
			continue;
		}
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
			out += c;
			continue;
		}
		// An apostrophe joins rather than separates: "Carmen's" is one word.
		if (c == '\'')
			continue;
		if (!out.empty() && out.lastChar() != '_')
			out += '_';
	}
	while (!out.empty() && out.lastChar() == '_')
		out.deleteLastChar();
	return out;
}

Common::String mohawkFallbackName(const char *kind, int id) {
	return Common::String::format("%s_%d", kind != nullptr ? kind : "thing", id);
}

Common::String mohawkDisambiguate(const Common::String &name, uint occurrence) {
	if (occurrence == 0)
		return name;
	return Common::String::format("%s_%u", name.c_str(), occurrence + 1);
}

} // End of namespace Mohawk
