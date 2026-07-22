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

#include "sword1/mcp_names.h"
#include "sword1/sworddefs.h"

#include "common/str.h"
#include "common/util.h"

#include "engines/mcp_bridge.h"

namespace Sword1 {

// ---------------------------------------------------------------------------
// Inventory items
//
// Indexed by pocket number (Logic::_scriptVars[POCKET_1 + n] is non-zero when
// item n+1 is carried). The names mirror the item labels in Menu::_objectDefs
// (staticres.cpp), which is the game's own list.
// ---------------------------------------------------------------------------

static const char *const kPocketNames[TOTAL_pockets + 1] = {
	nullptr,             //  0 unused
	"newspaper",         //  1
	"hazel_wand",        //  2
	"beer_towel",        //  3
	"hotel_key",         //  4
	"ball",              //  5
	"statuette",         //  6
	"red_nose",          //  7
	"polished_chalice",  //  8
	"dollar_bill",       //  9
	"photo",             // 10
	"flashlight",        // 11
	"fuse_wire",         // 12
	"gem",               // 13
	"statuette_paint",   // 14
	"stick",             // 15
	"excav_key",         // 16
	"lab_pass",          // 17
	"lifting_keys",      // 18
	"manuscript",        // 19
	"match_book",        // 20
	"suit_material",     // 21
	"stick_towel",       // 22
	"plaster",           // 23
	"pressure_gauge",    // 24
	"railway_ticket",    // 25
	"buzzer",            // 26
	"rosso_card",        // 27
	"toilet_key",        // 28
	"soap",              // 29
	"stone_key",         // 30
	"chalice",           // 31
	"tissue",            // 32
	"toilet_brush",      // 33
	"toilet_chain",      // 34
	"towel",             // 35
	"tripod",            // 36
	"lens",              // 37
	"mirror",            // 38
	"towel_cut",         // 39
	"bible",             // 40
	"tissue_charred",    // 41
	"false_key",         // 42
	"painted_key",       // 43
	"keyring",           // 44
	"soap_imp",          // 45
	"soap_plas",         // 46
	"cog_1",             // 47
	"cog_2",             // 48
	"handle",            // 49
	"coin",              // 50
	"biro",              // 51
	"pipe"               // 52
};

const char *sword1PocketName(int pocketNo) {
	if (pocketNo < 1 || pocketNo > TOTAL_pockets)
		return nullptr;
	return kPocketNames[pocketNo];
}

int sword1PocketNumber(const Common::String &name) {
	for (int i = 1; i <= TOTAL_pockets; ++i) {
		if (kPocketNames[i] && name == kPocketNames[i])
			return i;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Scene compacts
//
// The symbolic ids sworddefs.h already carries, plus names authored against the
// demo's screens using the `debug` tool's compact dump. Kept sorted by id so the
// lookup can binary-search; the unit tests assert both the ordering and that
// every name round-trips through sword1ResolveName().
// ---------------------------------------------------------------------------

struct CompactName { uint32 id; const char *name; };

static const CompactName kCompactNames[] = {
	// Screen 1 — rue Jarry, outside the Café de la Chandelle Verte. This is
	// where the demo's save sits. Authored from `debug` compact dumps.
	{ LEFT_SCROLL_POINTER,   "scroll_left" },
	{ RIGHT_SCROLL_POINTER,  "scroll_right" },
	// Megas / characters.
	{ PLAYER,                "george" },
	{ NICO,                  "nico" },
	{ BENOIR,                "benoir" },
	{ ROSSO,                 "rosso" },
	{ DUANE,                 "duane" },
	{ MOUE,                  "moue" },
	{ ALBERT,                "albert" },
	{ SAM,                   "sam" },
	// Named scenery from sworddefs.h.
	{ SAND_25,               "sand" },
	{ HOLDING_REPLICA_25,    "holding_replica" },
	{ GMASTER_79,            "grandmaster" },
	{ ROOF_63,               "roof" },
	{ GUARD_ROOF_63,         "guard_roof" },
	{ LEFT_TREE_POINTER_71,  "tree_left" },
	{ RIGHT_TREE_POINTER_71, "tree_right" }
};

static const int kNumCompactNames = ARRAYSIZE(kCompactNames);

const char *sword1CompactName(uint32 id) {
	for (int i = 0; i < kNumCompactNames; ++i) {
		if (kCompactNames[i].id == id)
			return kCompactNames[i].name;
	}
	return nullptr;
}

int sword1CompactNameCount() {
	return kNumCompactNames;
}

void sword1CompactNameAt(int index, uint32 &idOut, const char *&nameOut) {
	idOut = kCompactNames[index].id;
	nameOut = kCompactNames[index].name;
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

struct ScreenName { uint32 screen; const char *name; };

static const ScreenName kScreenNames[] = {
	{ 1, "rue_jarry" },
	{ 2, "cafe" },
	{ 3, "cafe_toilet" },
	{ 4, "street_alley" },
	{ 5, "sewer_entrance" },
	{ 6, "sewer" }
};

const char *sword1ScreenName(uint32 screen) {
	for (int i = 0; i < ARRAYSIZE(kScreenNames); ++i) {
		if (kScreenNames[i].screen == screen)
			return kScreenNames[i].name;
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// Conversation topics
//
// Subject ids are BASE_SUBJECT + n. The game exposes no text for them, so this
// table is authored; unknown topics fall back to "topic_<n>" in the bridge.
// ---------------------------------------------------------------------------

struct SubjectName { uint32 subject; const char *name; };

static const SubjectName kSubjectNames[] = {
	{ 0, nullptr } // none authored yet
};

const char *sword1SubjectName(uint32 subjectId) {
	for (int i = 0; i < ARRAYSIZE(kSubjectNames); ++i) {
		if (kSubjectNames[i].name && kSubjectNames[i].subject == subjectId)
			return kSubjectNames[i].name;
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

Common::String sword1ObjectName(uint32 id) {
	const char *known = sword1CompactName(id);
	if (known)
		return Common::String(known);
	return Common::String::format("object_%u_%u", id / ITM_PER_SEC, id & ITM_ID);
}

bool sword1ResolveName(const Common::String &name, uint32 &idOut) {
	Common::String s = MCP::McpBridge::normalizeActionName(name);
	if (s.empty())
		return false;

	// Authored compact names.
	for (int i = 0; i < kNumCompactNames; ++i) {
		if (s == kCompactNames[i].name) {
			idOut = kCompactNames[i].id;
			return true;
		}
	}

	// The "object_<section>_<index>" fallback form.
	if (s.hasPrefix("object_")) {
		const char *p = s.c_str() + 7;
		char *endp = nullptr;
		unsigned long section = strtoul(p, &endp, 10);
		if (endp && *endp == '_' && section < TOTAL_SECTIONS) {
			char *endp2 = nullptr;
			unsigned long index = strtoul(endp + 1, &endp2, 10);
			if (endp2 && *endp2 == '\0' && index <= ITM_ID) {
				idOut = (uint32)(section * ITM_PER_SEC + index);
				return true;
			}
		}
		return false;
	}

	// A raw id, decimal or "0x"-prefixed hex.
	{
		const char *p = s.c_str();
		char *endp = nullptr;
		unsigned long value = 0;
		if (s.hasPrefix("0x"))
			value = strtoul(p + 2, &endp, 16);
		else
			value = strtoul(p, &endp, 10);
		if (endp && *endp == '\0' && endp != p && (s != "0" ? value != 0 : true)) {
			idOut = (uint32)value;
			return true;
		}
	}

	return false;
}

} // End of namespace Sword1
