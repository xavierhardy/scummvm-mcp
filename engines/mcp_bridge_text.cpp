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

// MCP text handling: action-name normalization, game-text cleanup and key-name
// parsing. Kept in its own translation unit, free of any Engine dependency, so
// the per-engine unit tests can link these helpers without dragging an engine
// (and its GUI/base globals) into the cxxtest runner.

#include "common/keyboard.h"
#include "common/str.h"
#include "common/util.h"

#include "engines/mcp_bridge.h"

#include "backends/networking/mcp/mcp_server.h"

namespace MCP {

using Networking::mcpNormalizeSpaces;

// Some engines pad object names to a fixed width with trailing '@' bytes (the
// SCUMM charset renderer draws '@' as nothing), e.g. the Monkey Island EGA
// demo's "roter Hering@@@@@...". Strip the padding (and any spaces it uncovers)
// so MCP clients never see it.
Common::String mcpStripNamePadding(const Common::String &s) {
	Common::String out(s);
	while (!out.empty() &&
	       (out[out.size() - 1] == '@' || out[out.size() - 1] == ' '))
		out.deleteLastChar();
	return out;
}

// Lowercase a string covering both ASCII and the UTF-8 Latin-1 Supplement
// uppercase letters (U+00C0–U+00DE, e.g. the German Ö/Ä/Ü).
// Common::String::toLowercase() only folds ASCII A–Z, so a verb label whose
// first letter is an accented uppercase character — the German "Öffne" (open)
// verb — never matched the lowercase "öffne" sent by MCP clients. CP-850 (and
// other single-byte) input is left untouched: a lone high byte is neither ASCII
// nor a 0xC3 UTF-8 lead, so it falls through unchanged.
static Common::String mcpUtf8ToLower(const Common::String &s) {
	Common::String out;
	for (uint i = 0; i < s.size(); ++i) {
		unsigned char c = (unsigned char)s[i];
		if (c >= 'A' && c <= 'Z') {
			out += (char)(c + 0x20);
		} else if (c == 0xC3 && i + 1 < s.size()) {
			unsigned char d = (unsigned char)s[i + 1];
			// U+00C0–U+00DE -> +0x20 on the trailing byte, skipping U+00D7 (×).
			if (d >= 0x80 && d <= 0x9E && d != 0x97)
				d += 0x20;
			out += (char)c;
			out += (char)d;
			++i;
		} else {
			out += (char)c;
		}
	}
	return out;
}

Common::String McpBridge::normalizeActionName(const Common::String &action) {
	// Clients may echo back labels containing non-breaking or repeated spaces,
	// or the trailing '@' name padding from older server versions; fold both
	// before the space -> underscore replacement below so the result matches
	// names built from server-normalized text.
	Common::String s(mcpStripNamePadding(mcpNormalizeSpaces(action)));
	s.trim();
	// SCUMM V8 (Curse of Monkey Island) object names are formatted as
	// "/<room>.<id>/<name>" — strip the leading metadata so the MCP client sees
	// just "<name>". Apply only to strings starting with '/' to avoid affecting
	// verbs or other engines.
	if (!s.empty() && s[0] == '/') {
		const char *str = s.c_str();
		const char *secondSlash = strchr(str + 1, '/');
		if (secondSlash) {
			s = Common::String(secondSlash + 1);
		}
	}
	s = mcpUtf8ToLower(s);
	s.replace('-', '_');
	s.replace(' ', '_');
	if (s == "walk")    return "walk_to";
	if (s == "goto")    return "walk_to";
	if (s == "look")    return "look_at";
	if (s == "what_is") return "look_at";
	if (s == "examine") return "look_at";
	if (s == "pick")    return "pick_up";
	if (s == "pickup")  return "pick_up";
	if (s == "take")    return "pick_up";
	if (s == "get")     return "pick_up";
	if (s == "talk")     return "talk_to";
	// German (DE_DEU) verb-bar labels, so an agent can drive a German build with
	// the canonical English verb names, the per-object compatible_verbs fallbacks
	// (walk_to / look_at) fire, and the generic door-state detection can identify
	// openables regardless of the build's language.
	if (s == "öffne")    return "open";
	if (s == "schließe") return "close";
	if (s == "schliesse") return "close";
	if (s == "geh_zu")   return "walk_to";
	if (s == "schau_an") return "look_at";
	if (s == "nimm")     return "pick_up";
	if (s == "rede_mit") return "talk_to";
	if (s == "gib")      return "give";
	if (s == "benutze")  return "use";
	if (s == "drücke")   return "push";
	if (s == "ziehe")    return "pull";
	// The Dig: single-cursor verbs map to the generic 'use' action (verb ID 7).
	if (s == "interact") return "use";
	if (s == "use_item") return "use";
	return s;
}

Common::String mcpCleanGameText(const Common::String &text) {
	Common::String src(text);
	// SCUMM V8 (Curse of Monkey Island) messages start with a "/<TAG>/" prefix
	// (e.g. "/BPIR001/Hold yer tongue, captive!"). Strip it so the MCP client
	// sees only the human-readable text.
	if (!src.empty() && src[0] == '/') {
		const char *str = src.c_str();
		const char *secondSlash = strchr(str + 1, '/');
		if (secondSlash) {
			// Verify the tag-like portion is alphanumeric (typical V8 tag format)
			bool isV8Tag = true;
			for (const char *p = str + 1; p < secondSlash; ++p) {
				if (!Common::isAlnum((byte)*p)) { isV8Tag = false; break; }
			}
			if (isV8Tag && (secondSlash - str) > 2)
				src = Common::String(secondSlash + 1);
		}
	}
	Common::String out;
	for (size_t i = 0; i < src.size(); ++i) {
		unsigned char c = (unsigned char)src[i];
		// Replace control characters (except newline) and DEL with space
		if ((c < 0x20 && c != 0x0A) || c == 0x7F) {
			out += ' ';
			continue;
		}
		// Skip UTF-8 sequences for replacement character (� = 0xEF 0xBF 0xBD)
		if (c == 0xEF && i + 2 < src.size()) {
			unsigned char c1 = (unsigned char)src[i+1];
			unsigned char c2 = (unsigned char)src[i+2];
			if (c1 == 0xBF && c2 == 0xBD) {
				// Skip replacement character unless it's part of orichalum beads
				if (i + 4 < src.size()) {
					unsigned char c3 = (unsigned char)src[i+3];
					// Orichalum beads: �\x04� = 0xEF 0xBF 0xBD 0x04 0xEF 0xBF 0xBD
					if (c3 == 0x04) {
						out += src[i];
						out += src[++i];
						out += src[++i];
						out += src[++i];
						continue;
					}
				}
				// Replace with space rather than skipping: some games emit non-ASCII
				// charset bytes that mcpSanitizeString converts to U+FFFD; silently
				// dropping them would erase whole dialog lines.
				out += ' ';
				i += 2;
				continue;
			}
		}
		out += src[i];
	}
	// Trim leading/trailing whitespace (including newlines)
	size_t start = 0;
	while (start < out.size() && (unsigned char)out[start] <= 0x20) start++;
	size_t end = out.size();
	while (end > start && (unsigned char)out[end-1] <= 0x20) end--;
	out = out.substr(start, end - start);
	// Remove trailing @
	while (!out.empty() && out[out.size()-1] == '@') {
		out = out.substr(0, out.size()-1);
	}
	// The control-character substitutions above can leave runs of spaces.
	return mcpNormalizeSpaces(out);
}

// Map a JSON 'key' value to a Common::KeyState. Single ASCII chars map to
// their Common::KeyCode (which equals the ASCII byte for printable letters
// and digits). Named keys ('Escape', 'Return', 'F1', 'Up'...) map via a
// small table. Returns false on unknown name.
bool mcpJsonKeyToKeyState(const Common::String &name, bool ctrl, bool shift, bool alt,
                          Common::KeyState &out) {
	struct NamedKey { const char *name; Common::KeyCode kc; };
	static const NamedKey kNamed[] = {
		{"Escape",    Common::KEYCODE_ESCAPE},
		{"Return",    Common::KEYCODE_RETURN},
		{"Enter",     Common::KEYCODE_RETURN},
		{"Space",     Common::KEYCODE_SPACE},
		{"Tab",       Common::KEYCODE_TAB},
		{"Backspace", Common::KEYCODE_BACKSPACE},
		{"Delete",    Common::KEYCODE_DELETE},
		{"Up",        Common::KEYCODE_UP},
		{"Down",      Common::KEYCODE_DOWN},
		{"Left",      Common::KEYCODE_LEFT},
		{"Right",     Common::KEYCODE_RIGHT},
		{"F1",        Common::KEYCODE_F1},
		{"F2",        Common::KEYCODE_F2},
		{"F3",        Common::KEYCODE_F3},
		{"F4",        Common::KEYCODE_F4},
		{"F5",        Common::KEYCODE_F5},
		{"F6",        Common::KEYCODE_F6},
		{"F7",        Common::KEYCODE_F7},
		{"F8",        Common::KEYCODE_F8},
		{"F9",        Common::KEYCODE_F9},
		{"F10",       Common::KEYCODE_F10},
		{"F11",       Common::KEYCODE_F11},
		{"F12",       Common::KEYCODE_F12},
		{nullptr,     Common::KEYCODE_INVALID}
	};

	Common::KeyCode kc = Common::KEYCODE_INVALID;
	uint16 ascii = 0;

	if (name.size() == 1) {
		byte ch = (byte)name[0];
		ascii = ch;
		// Lower-case letters and digits map directly to their KEYCODE values.
		// Upper-case letters use the lowercase keycode + Shift modifier.
		if (ch >= 'A' && ch <= 'Z') {
			kc = (Common::KeyCode)(ch - 'A' + 'a');
			shift = true;
		} else {
			kc = (Common::KeyCode)ch;
		}
	} else {
		for (int i = 0; kNamed[i].name; ++i) {
			if (name.equalsIgnoreCase(kNamed[i].name)) { kc = kNamed[i].kc; break; }
		}
		if (kc == Common::KEYCODE_INVALID) return false;
		// Set ASCII for keys that have a printable equivalent
		if (kc == Common::KEYCODE_RETURN)    ascii = 13;
		else if (kc == Common::KEYCODE_TAB)  ascii = 9;
		else if (kc == Common::KEYCODE_SPACE) ascii = ' ';
		else if (kc == Common::KEYCODE_ESCAPE) ascii = 27;
		else if (kc == Common::KEYCODE_BACKSPACE) ascii = 8;
	}

	byte flags = 0;
	if (ctrl)  flags |= Common::KBD_CTRL;
	if (shift) flags |= Common::KBD_SHIFT;
	if (alt)   flags |= Common::KBD_ALT;

	out = Common::KeyState(kc, ascii, flags);
	return true;
}

} // End of namespace MCP
