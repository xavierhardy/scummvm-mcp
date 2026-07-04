/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

// MCP action-name normalization. Kept in its own translation unit, free of any
// ScummEngine dependencies, so the unit tests (test/engines/scumm/mcp.h) can
// link ScummMcpBridge::normalizeActionName without dragging the whole engine
// (and its GUI/base globals) into the cxxtest runner.

#include "common/str.h"
#include "common/util.h"

#include "scumm/mcp.h"

#include "backends/networking/mcp/mcp_server.h"

namespace Scumm {

using Networking::mcpNormalizeSpaces;

// SCUMM pads object names to a fixed width with trailing '@' bytes (the
// charset renderer draws '@' as nothing), e.g. the EGA demo's
// "roter Hering@@@@@...". Strip the padding (and any spaces it uncovers) so
// MCP clients never see it.
//
// External linkage: also called from safeUtf8() in mcp.cpp.
Common::String mcpStripNamePadding(const Common::String &s) {
	Common::String out(s);
	while (!out.empty() &&
	       (out[out.size() - 1] == '@' || out[out.size() - 1] == ' '))
		out.deleteLastChar();
	return out;
}

// Lowercase a string covering both ASCII and the UTF-8 Latin-1 Supplement
// uppercase letters (U+00C0–U+00DE, e.g. the German Ö/Ä/Ü). SCUMM's
// Common::String::toLowercase() only folds ASCII A–Z, so a verb label whose
// first letter is an accented uppercase character — the German "Öffne" (open)
// verb — never matched the lowercase "öffne" sent by MCP clients. CP-850 (and
// other single-byte) input is left untouched: a lone high byte is neither ASCII
// nor a 0xC3 UTF-8 lead, so it falls through unchanged, exactly as before.
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

Common::String ScummMcpBridge::normalizeActionName(const Common::String &action) {
	// Clients may echo back labels containing non-breaking or repeated spaces,
	// or the trailing '@' name padding from older server versions; fold both
	// before the space -> underscore replacement below so the result matches
	// names built from server-normalized text.
	Common::String s(mcpStripNamePadding(mcpNormalizeSpaces(action)));
	s.trim();
	// V8 (Curse of Monkey Island) object names are formatted as
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
	// the canonical English verb names and the per-object compatible_verbs
	// fallbacks (walk_to / look_at) fire regardless of the build's language.
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

} // End of namespace Scumm
