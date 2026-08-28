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

#include "sword2/mcp_names.h"

namespace Sword2 {

// The cursor resource ids the game registers on its mouse-detection boxes.
// Kept here rather than shared with mouse.cpp's private copy so this file
// stays free of the engine; they are fixed game data either way.
enum {
	kPointerNormal   = 17,   // floor
	kPointerCroshair = 18,
	kPointerMouth    = 787,  // someone to talk to
	kPointerExit0    = 788,  // .. exit7 = 795
	kPointerExit7    = 795,
	kPointerExitDown = 796,
	kPointerExitUp   = 797,
	kPointerScrollL  = 1440,
	kPointerScrollR  = 1441,
	kPointerPickup   = 3099,
	kPointerUse      = 3100
};

bool sword2PointerIsExit(int32 pointerRes) {
	return (pointerRes >= kPointerExit0 && pointerRes <= kPointerExit7) ||
	       pointerRes == kPointerExitDown || pointerRes == kPointerExitUp;
}

bool sword2PointerIsFloor(int32 pointerRes) {
	return pointerRes == kPointerNormal;
}

const char *sword2PointerKind(int32 pointerRes) {
	if (sword2PointerIsFloor(pointerRes))
		return "floor";
	if (sword2PointerIsExit(pointerRes))
		return "exit";
	if (pointerRes == kPointerMouth)
		return "person";
	if (pointerRes == kPointerPickup)
		return "item";
	if (pointerRes == kPointerScrollL || pointerRes == kPointerScrollR)
		return "scroll";
	return "object";
}

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

Common::String sword2CleanName(const Common::String &raw) {
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

Common::String sword2FallbackName(int32 id) {
	return Common::String::format("object_%d", id);
}

Common::String sword2Disambiguate(const Common::String &name, uint occurrence) {
	if (occurrence == 0)
		return name;
	return Common::String::format("%s_%u", name.c_str(), occurrence + 1);
}

} // End of namespace Sword2
