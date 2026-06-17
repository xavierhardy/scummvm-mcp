/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "common/config-manager.h"
#include "common/debug.h"
#include "common/system.h"
#include "common/hashmap.h"
#include "common/hash-str.h"
#include "common/util.h"
#include "common/ustr.h"

#include "scumm/actor.h"
#include "scumm/scumm_v0.h"
#include "scumm/detection.h"
#include "scumm/mcp.h"
#include "scumm/object.h"
#include "scumm/scumm.h"
#include "scumm/verbs.h"
#include "scumm/boxes.h"
#ifdef ENABLE_SCUMM_7_8
#include "scumm/scumm_v7.h"
#endif

namespace Scumm {

using Networking::mcpJsonString;
using Networking::mcpJsonInt;
using Networking::mcpJsonBool;
using Networking::mcpProp;
using Networking::mcpPropOneOf;
using Networking::mcpObjectSchema;
using Networking::mcpSanitizeString;
using Networking::mcpLowerTrimmed;
using Networking::mcpNormalizeSpaces;

namespace {

Common::String cleanGameText(const Common::String &text);

// V6+ standard action icon verb IDs.
// Sam & Max uses V6 image verbs, but verb IDs differ from other SCUMM variants,
// so keep both common V6 and Sam & Max candidate IDs.
static const int kV6ActionIds[]    = {4, 5, 6, 7, 11, 13, 14, 15};
static const int kNumV6ActionIds   = 8;

static bool isV6ActionVerb(int verbid) {
	for (int i = 0; i < kNumV6ActionIds; ++i)
		if (kV6ActionIds[i] == verbid) return true;
	return false;
}

static bool isSentenceLikeDialogLabel(const Common::String &label) {
	if (label.empty()) return false;
	int alphaCount = 0;
	int spaceCount = 0;
	for (uint i = 0; i < label.size(); ++i) {
		const char c = label[i];
		if (Common::isAlpha((byte)c)) ++alphaCount;
		if (c == ' ') ++spaceCount;
	}
	return alphaCount >= 6 && spaceCount >= 1;
}

// Canonical V6 verb label table (verbid → name/label for image-verb games).
// Sam & Max (V6) has no verb bar: a right-click cycles a context cursor through
// the available verbs, and a left-click runs the current verb. The current verb
// cursor is mirrored in script var 177; the "mouth" (talk) cursor is object 877.
// talk_to is therefore dispatched (see toolAct/pumpStream) by cycling the cursor
// to the mouth and then clicking the actor — resolveVerb maps it to a sentinel.
static const int kSnmCursorVerbVar = 177;
static const int kSnmMouthCursor   = 877;
// The "use/operate" context cursor (the hand reaching for a control). Reached by
// cycling the right-click verb, exactly like the mouth (877). Some objects expose
// no per-object verb-7 script for it — the action lives in the scene-click input
// script instead (e.g. boarding the street DeSoto, which plays the drive-away
// cutscene). For those, 'use' is dispatched by cycling to this cursor + clicking.
static const int kSnmUseCursor     = 878;
// The pickup-hand cursor; clicking an actor with it (e.g. Max) takes them
// in hand: the cursor then becomes the held character (889 for Max), which
// is NOT part of the right-click rotation — cycling away drops them again.
static const int kSnmPickupCursor  = 890;
static const int kSnmTalkSentinel  = -200;
// Sentinel for "click with whatever is currently held": matches any cursor
// outside the standard rotation (875,876,877,878,890), e.g. Max-in-hand 889.
static const int kSnmItemCursorSentinel = -201;
// Verify phase after a pickup click: wait a few frames, then check that the
// cursor turned into a held-item cursor; if not (the actor wandered out from
// under the click), aim and click again.
static const int kSnmVerifyHeldSentinel = -202;

static bool snmIsStandardCursor(int v) {
	return v == 875 || v == 876 || v == 877 || v == 878 || v == kSnmPickupCursor;
}

struct V6VerbEntry { int id; const char *name; const char *label; };
static const V6VerbEntry kV6CanonicalVerbs[] = {
	// Common V6 mapping used by several games
	{4,  "pick_up", "pick up"},
	{5,  "look_at", "look at"},
	{6,  "talk_to", "talk to"},
	{7,  "use",     "use"},
	{13, "walk_to", "walk to"},
	// Sam & Max mapping (icon verbs)
	{11, "use",     "use"},
	{14, "pick_up", "pick up"},
	{15, "look_at", "look at"},
	{0,  nullptr,   nullptr}
};

static const V6VerbEntry *findV6Verb(int verbid) {
	for (int i = 0; kV6CanonicalVerbs[i].name; ++i)
		if (kV6CanonicalVerbs[i].id == verbid) return &kV6CanonicalVerbs[i];
	return nullptr;
}

// Return the display name of an object/actor. Empty string if unnamed.
Common::String getObjName(const ScummMcpBridge *bridge, int obj) {
	if (!bridge) return "";
	const byte *name = bridge->callGetObjOrActorName(obj);
	if (!name || !*name) return "";
	return Common::String((const char *)name);
}

// Clean up game text: remove control characters, trim whitespace, remove trailing @, remove unicode chars
Common::String cleanGameText(const Common::String &text) {
	Common::String src(text);
	// V8 (Curse of Monkey Island) messages start with a "/<TAG>/" prefix
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
					// Orichalum beads:�� = 0xEF 0xBF 0xBD 0x04 0xEF 0xBF 0xBD
					if (c3 == 0x04) {
						out += src[i];
						out += src[++i];
						out += src[++i];
						out += src[++i];
						continue;
					}
				}
				// Replace with space rather than skipping: V6 games emit non-ASCII
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

// Conversation topic icons carry no text — Sam & Max draws them as picture
// objects in the verb strip and The Dig as blast objects in the dialog
// panel. Map the known icon object numbers to their on-screen meaning so
// MCP clients see "question"/"exclamation"/... instead of an opaque
// icon_<num>. Unknown icons keep the icon_<num> fallback.
static const char *dialogIconLabel(int gameId, int objNum) {
	struct IconEntry { int obj; const char *label; };
	// Sam & Max: ?, !, golden duck (tease), waving hand (goodbye), Max head.
	static const IconEntry kSamnmaxIcons[] = {
		{997,  "question"},
		{998,  "exclamation"},
		{1001, "tease"},
		{999,  "bye"},
		{1067, "max"},
		{0, nullptr}
	};
	// The Dig: ?, !, stop hand (goodbye).
	static const IconEntry kDigIcons[] = {
		{158, "question"},
		{159, "exclamation"},
		{160, "bye"},
		{0, nullptr}
	};
	const IconEntry *table = nullptr;
	if (gameId == GID_SAMNMAX)
		table = kSamnmaxIcons;
	else if (gameId == GID_DIG)
		table = kDigIcons;
	if (!table)
		return nullptr;
	for (int i = 0; table[i].label; ++i)
		if (table[i].obj == objNum)
			return table[i].label;
	return nullptr;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ScummMcpBridge::ScummMcpBridge(ScummEngine *vm)
	: _vm(vm),
	  _enabled(false),
	  _skipToolEnabled(false),
	  _debugToolsEnabled(false),
	  _server(nullptr),
	  _nextMessageSeq(1),
	  _frameCounter(0),
	  _streaming(false),
	  _sseStartFrame(0),
	  _sseDoneAtFrame(0),
	  _sseStuckAtFrame(0),
	  _sseLastEventFrame(0),
	  _sseEgoMoved(false),
	  _sseTargetObject(0),
	  _ssePreRoom(0),
	  _ssePrePosX(0),
	  _ssePrePosY(0),
	  _ssePendingSecondClick(false),
	  _sseClickMouseX(0),
	  _sseClickMouseY(0),
	  _ssePrevNoteValue(0),
	  _sseLastNoteFedFrame(0) {
	if (!_vm) return;

	_enabled = ConfMan.getBool("mcp");
	if (!_enabled) return;

	_skipToolEnabled = ConfMan.hasKey("mcp_skip_tool") && ConfMan.getBool("mcp_skip_tool");
	_debugToolsEnabled = ConfMan.hasKey("mcp_debug") && ConfMan.getBool("mcp_debug");

	int port = ConfMan.hasKey("mcp_port") ? ConfMan.getInt("mcp_port") : 23456;
	Common::String host = ConfMan.hasKey("mcp_host") ? ConfMan.get("mcp_host") : "127.0.0.1";
	_server = new Networking::McpServer(port, "scummvm", "1.0", host);
	if (!_server->isListening()) {
		delete _server;
		_server = nullptr;
		_enabled = false;
		return;
	}
	_server->setToolHandler(this);
	registerTools();
}

ScummMcpBridge::~ScummMcpBridge() {
	delete _server;
}

// ---------------------------------------------------------------------------
// Game-loop hook
// ---------------------------------------------------------------------------

void ScummMcpBridge::pump() {
	if (!_enabled || !_server) return;
	++_frameCounter;

	// V7 (The Dig, Full Throttle) — ScummEngine_v7::actorTalk does not call the
	// base ScummEngine::actorTalk, so the onActorLine hook in actor.cpp is not
	// triggered for these games. Poll _charsetBuffer + the talking-actor var
	// here and synthesize a message whenever a new line appears. Detection runs
	// only while a talker is active, so it stays cheap and won't fire during
	// idle frames.
	// V7 (The Dig, Full Throttle) — ScummEngine_v7::actorTalk does not call the
	// base ScummEngine::actorTalk, so the onActorLine hook in actor.cpp is not
	// triggered. _charsetBuffer holds the active line for the duration of the
	// message; it is prefixed by 0xFF-coded sound/voice metadata blocks (each 4
	// bytes long) emitted by the original interpreter for lip-sync. Skip those
	// to read the printable text, then push a message whenever it changes.
	// V7: snapshot the "normal" verb script on the first pump frame where it is
	// non-zero and we're not in a stream. This lets hasPendingQuestion() detect
	// when the game switches to a dialog input handler (e.g., script 38 → 69).
	// Only snapshot once the engine has handed input back to the player
	// (_userPut > 0). Full Throttle's intro/dumpster cutscene runs a different
	// verb script (25) than normal gameplay (302); snapshotting during the
	// cutscene would record 25 and make hasPendingQuestion() believe a dialog
	// is always pending once gameplay's 302 takes over. Gating on _userPut > 0
	// captures the true gameplay baseline. The Dig's first interactive frame
	// already carries its gameplay value, so this does not change its behavior.
	if (_vm && _vm->_game.version == 7 && _baseVerbScript == 0 && !_streaming &&
	    _vm->_userPut > 0 && _vm->VAR_VERB_SCRIPT != 0xFF) {
		int cur = (int)_vm->VAR(_vm->VAR_VERB_SCRIPT);
		if (cur != 0)
			_baseVerbScript = cur;
	}

	if (_vm && _vm->_game.version == 7 && _vm->_haveMsg) {
		const byte *p = _vm->_charsetBuffer;
		const byte *end = p + sizeof(_vm->_charsetBuffer);
		// Skip the leading metadata: Full Throttle (and The Dig) prefix each spoken
		// line with one or more 0xFF-coded 4-byte talkie/sound blocks
		// (0xFF <code> <id-lo> <id-hi>). Advance past every such block.
		while (p + 3 < end && *p == 0xFF)
			p += 4;
		if (p < end && *p != 0) {
			Common::String text((const char *)p);
			int actor = _vm->getTalkingActor();
			// Reject fragments that carry no printable ASCII letter or digit: between
			// real lines the buffer momentarily holds only embedded sound codes
			// (e.g. 0xFF 0x0A <id>), which would otherwise surface as garbage
			// "messages". Real dialog always contains alphanumeric text.
			bool hasAlnum = false;
			for (uint ti = 0; ti < text.size(); ++ti) {
				byte c = (byte)text[ti];
				if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
				    (c >= '0' && c <= '9')) { hasAlnum = true; break; }
			}
			if (hasAlnum && (text != _lastV7TalkText || actor != _lastV7TalkActor)) {
				_lastV7TalkText = text;
				_lastV7TalkActor = actor;
				onActorLine(actor == 0xFF ? -1 : actor, text);
			}
		}
	} else if (_vm && _vm->_game.version == 7) {
		_lastV7TalkText.clear();
		_lastV7TalkActor = 0;
	}

	if (_vm && _vm->_game.id == GID_SAMNMAX)
		onV7BlastTextSnapshot();

	// Release the simulated mouse button a couple frames after a debug
	// mouse_click so the engine sees a complete press/release cycle.
	if (_debugButtonReleaseFrame != 0 && _frameCounter >= _debugButtonReleaseFrame) {
		_vm->_leftBtnPressed  &= ~0x01; // clear msDown
		_vm->_rightBtnPressed &= ~0x01;
		_debugButtonReleaseFrame = 0;
	}

	// Drop cached dialog choices once V7 leaves dialog mode (verb script is
	// back to its baseline). Otherwise stale choices from the prior turn
	// would leak into state.
	if (_vm && _vm->_game.version == 7 && _vm->VAR_VERB_SCRIPT != 0xFF &&
	    _baseVerbScript != 0 &&
	    (int)_vm->VAR(_vm->VAR_VERB_SCRIPT) == _baseVerbScript &&
	    !_v7DialogChoices.empty()) {
		_v7DialogChoices.clear();
	}
	if (_vm && _vm->_game.id == GID_SAMNMAX && !hasPendingQuestion() && !_v7DialogChoices.empty())
		_v7DialogChoices.clear();

	_server->pump();
}

// ---------------------------------------------------------------------------
// Loom segment detection
// ---------------------------------------------------------------------------

// True when the engine is running the Loom mini-game. For full Loom this is
// always true; for Passport to Adventure (3 mini-games) we detect Loom by the
// empty text verb bar — Indy3 and MI1 populate the standard V3 verb slots
// (Open / Look at / Pick up / etc.), but Loom uses a single-cursor + distaff
// interface and leaves them empty.
// True when an Indy3 fist-fight is active. Indy3's fight HUD is driven by a
// stable set of script vars (322..327) that hold each fighter's punch-power
// gauge and health. Outside of a fight these vars are 0 (or unused for
// non-Indy3 games), so a non-zero opponent health on the right game is a
// reliable signal. We restrict to GID_INDY3 and GID_PASS (the Passport demo
// containing Indy3) to avoid colliding with var indices used by other games.
bool ScummMcpBridge::isInIndy3Fight() const {
	if (!_vm || !_vm->_scummVars) return false;
	if (_vm->_game.id != GID_INDY3 && _vm->_game.id != GID_PASS) return false;
	if (_vm->_numVariables <= 327) return false;
	// Both health values should be non-zero AND not stale (heuristic: max
	// health ~1000, current health is in 1..1000 range during a fight).
	int32 indyHealth = _vm->_scummVars[325];
	int32 oppHealth  = _vm->_scummVars[327];
	return indyHealth > 0 && oppHealth > 0 && indyHealth <= 2000 && oppHealth <= 2000;
}

bool ScummMcpBridge::isInLoomSection() const {
	if (!_vm) return false;
	if (_vm->_game.id == GID_LOOM) return true;
	if (_vm->_game.id != GID_PASS) return false;
	// Loom in Passport renders its distaff as the verb bar: every slot's label
	// is a single-character glyph (note icons), e.g. 'z', '{', '^'. Indy3 and
	// MI1 segments populate the bar with multi-character English verbs
	// ("Open", "Look at", etc.). Detect by examining slot label lengths.
	bool sawAnyVerb = false;
	bool sawWordLabel = false;
	for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (!vs.verbid) continue;
		if (vs.saveid != 0) continue;
		if (_vm->_game.version > 0 && vs.verbid == 1) continue; // OBIM slot
		if (vs.curmode != 0 && vs.curmode != 1) continue;
		const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
		if (!ptr) continue;
		byte textBuf[256] = {};
		_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
		const char *label = (const char *)textBuf;
		if (!label[0]) continue;
		sawAnyVerb = true;
		// Word-length English labels (>= 3 chars starting with a letter) are
		// the hallmark of Indy3 / MI1 verb bars.
		size_t len = strlen(label);
		if (len >= 3 && Common::isAlpha((byte)label[0]))
			sawWordLabel = true;
	}
	// Loom: many populated slots, none with word labels (or no slots at all).
	return sawAnyVerb && !sawWordLabel;
}

// ---------------------------------------------------------------------------
// Message capture from engine
// ---------------------------------------------------------------------------

void ScummMcpBridge::pushMessage(const char *type, int actorId, const Common::String &text) {
	if (!_enabled || text.empty()) return;
	MessageEntry m;
	m.seq = _nextMessageSeq++;
	m.frame = _frameCounter;
	m.room = _vm ? _vm->_currentRoom : 0;
	m.actorId = actorId;
	m.type = type;
	m.text = text;
	_messages.push_back(m);
	const uint kMaxMessages = 512;
	if (_messages.size() > kMaxMessages)
		_messages.remove_at(0);
}

void ScummMcpBridge::onActorLine(int actorId, const Common::String &text) {
	// V6 (Sam & Max) and V7 (The Dig / Full Throttle): spoken lines arrive
	// straight from _charsetBuffer (actor.cpp and the V7 pump() both feed this
	// hook), prefixed by one or more 0xFF-coded 4-byte talkie/sound blocks
	// (0xFF <code> <id-lo> <id-hi>). Strip them, then drop the fragments that
	// hold only embedded sound codes with no readable text — e.g. Sam & Max's
	// voice-only reaction cues, which would otherwise surface as garbage
	// "messages". Restricted to V6/V7 so older text engines pass through
	// untouched.
	if (_vm && _vm->_game.version >= 6) {
		const byte *p = (const byte *)text.c_str();
		const byte *end = p + text.size();
		while (p + 3 < end && *p == 0xFF)
			p += 4;
		Common::String line((const char *)p);
		// Require at least two ASCII letters: a real spoken line is a word, while
		// a leftover sound-code fragment (e.g. 0xFF 0x0A 'L') carries at most a
		// stray byte that happens to fall in the letter range.
		int letters = 0;
		for (uint i = 0; i < line.size(); ++i) {
			byte c = (byte)line[i];
			if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
				++letters;
		}
		bool hasAlnum = (letters >= 2);
		if (!hasAlnum)
			return;
		pushMessage("actor", actorId, line);
		return;
	}
	pushMessage("actor", actorId, text);
}
void ScummMcpBridge::onSystemLine(const Common::String &text) {
	pushMessage("system", -1, text);
}
void ScummMcpBridge::onDialogPrompt(const Common::String &text) {
	pushMessage("dialog", -1, text);
}

void ScummMcpBridge::collectSamnMaxDialogChoices(Common::Array<V7Choice> &out) {
	out.clear();
	if (!_vm || _vm->_game.id != GID_SAMNMAX || !_vm->_objs)
		return;

	// The conversation panel is a horizontal row of equal-width icon slots drawn
	// in the bottom verb strip. Each slot is a floating object (fl_object_index
	// != 0) about 40x24 px wide. Unused slots all share one blank "box" image;
	// real topic icons each carry a distinct image. Collect candidate slots,
	// keep the densest screen row, then drop the blank (duplicate-image) ones.
	struct Slot { int obj; int x; int cx; int cy; uint32 sum; };
	Common::Array<Slot> slots;
	const int stripTop = _vm->_screenHeight * 3 / 4; // bottom quarter (y>=150 @200px)
	for (int i = 1; i < _vm->_numLocalObjects; ++i) {
		const ObjectData &od = _vm->_objs[i];
		if (!od.obj_nr || od.fl_object_index == 0)
			continue;
		if (od.width < 24 || od.width > 56 || od.height < 12 || od.height > 40)
			continue;
		int oy = _vm->getObjY(od.obj_nr);
		if (oy < stripTop)
			continue;
		// Skip real inventory items that may share the strip.
		bool isInv = false;
		for (int k = 0; _vm->_inventory && k < _vm->_numInventory; ++k)
			if (_vm->_inventory[k] == od.obj_nr) { isInv = true; break; }
		if (isInv)
			continue;
		// The strip icons track the camera so they stay screen-fixed: getObjX
		// returns room coordinates (offset by the scroll). Convert to screen X so
		// the click target is correct even when the room has scrolled.
		int ox = _vm->getObjX(od.obj_nr) - _vm->_virtscr[kMainVirtScreen].xstart;
		uint32 sum = 0;
		const byte *im = _vm->getObjectImage(_vm->getOBIMFromObjectData(od), _vm->getState(od.obj_nr));
		if (im)
			for (int b = 0; b < 256; ++b)
				sum = sum * 31u + im[b];
		Slot s;
		s.obj = od.obj_nr;
		s.x   = ox;
		s.cx  = ox + od.width / 2;
		s.cy  = oy + od.height / 2;
		s.sum = sum;
		slots.push_back(s);
	}
	if (slots.size() < 2)
		return; // a lone strip flobject (e.g. the inventory tab) is not a dialog

	// Keep only the densest screen row (the topic icons all share one y).
	int bestY = slots[0].cy, bestCount = 0;
	for (uint i = 0; i < slots.size(); ++i) {
		int cnt = 0;
		for (uint j = 0; j < slots.size(); ++j)
			if (slots[j].cy == slots[i].cy) ++cnt;
		if (cnt > bestCount) { bestCount = cnt; bestY = slots[i].cy; }
	}
	Common::Array<Slot> row;
	for (uint i = 0; i < slots.size(); ++i)
		if (slots[i].cy == bestY) row.push_back(slots[i]);
	if (row.size() < 2)
		return;

	// Identify the blank-slot image: the most frequent checksum in the row,
	// but only when it actually repeats (>= 2 slots share it).
	uint32 blankSum = 0;
	int blankCount = 1;
	for (uint i = 0; i < row.size(); ++i) {
		int cnt = 0;
		for (uint j = 0; j < row.size(); ++j)
			if (row[j].sum == row[i].sum) ++cnt;
		if (cnt > blankCount) { blankCount = cnt; blankSum = row[i].sum; }
	}
	bool haveBlank = (blankCount >= 2);

	// Real topics: row slots that are not the blank box, ordered left-to-right by
	// the engine's reported X.
	for (uint i = 0; i + 1 < row.size(); ++i)
		for (uint j = 0; j + 1 < row.size() - i; ++j)
			if (row[j].x > row[j + 1].x) { Slot t = row[j]; row[j] = row[j + 1]; row[j + 1] = t; }
	Common::Array<Slot> topics;
	for (uint i = 0; i < row.size(); ++i) {
		if (haveBlank && row[i].sum == blankSum)
			continue;
		topics.push_back(row[i]);
	}
	if (topics.empty())
		return;

	// The game draws the active topic icons packed contiguously from the left of
	// the strip and fills the remaining slots with blank boxes. A topic added mid
	// conversation (e.g. the office "fifth" icon, object 1067) keeps its dormant
	// home X — parked behind the rightmost blank box — even though it is actually
	// drawn in the next free left slot. Trusting getObjX there would aim a click
	// at the blank on top of it and miss. So derive each topic's real screen
	// position from its left-to-right rank against the strip's slot pitch instead
	// of the (possibly stale) reported X. The leftmost topic and the slot width
	// are stable, so already-correct icons keep their coordinates.
	const int originX    = topics[0].x;            // leftmost topic's screen X
	const int halfWidth  = topics[0].cx - topics[0].x;
	const int slotWidth  = halfWidth > 0 ? halfWidth * 2 : 40;
	for (uint i = 0; i < topics.size(); ++i) {
		V7Choice c;
		c.objNumber = topics[i].obj;
		const char *iconLabel = dialogIconLabel(GID_SAMNMAX, topics[i].obj);
		c.text = iconLabel ? Common::String(iconLabel)
		                   : Common::String::format("icon_%d", topics[i].obj);
		c.x = originX + (int)i * slotWidth + halfWidth;
		c.y = topics[i].cy;
		out.push_back(c);
	}
}

void ScummMcpBridge::onV7BlastTextSnapshot() {
	if (!_vm || (_vm->_game.version != 7 && _vm->_game.id != GID_SAMNMAX)) return;
	if (_vm->_game.id == GID_SAMNMAX) {
		// Sam & Max conversations are not blast objects (unlike The Dig). The
		// topic icons are drawn as a row of equal-width floating objects in the
		// bottom verb strip; the choices are refreshed here every frame and
		// consumed by hasPendingQuestion()/toolState()/toolAnswer().
		collectSamnMaxDialogChoices(_v7DialogChoices);
		return;
	}
	ScummEngine_v7 *v7 = (ScummEngine_v7 *)_vm;
	// Capture each frame regardless of script state. Dialog-mode gating is
	// applied in toolState/toolAnswer; cleanup happens in pump() once the
	// verb script returns to its baseline. The blast text queue holds new
	// items for one frame only (the engine drains it after drawing), so we
	// have to grab them whenever they appear, including the brief moments
	// between the talk script finishing and the dialog script taking over.

	Common::Array<V7Choice> fresh;
	for (int i = 0; i < v7->_blastTextQueuePos &&
	     i < (int)ARRAYSIZE(v7->_blastTextQueue); ++i) {
		const ScummEngine_v7::BlastText &bt = v7->_blastTextQueue[i];
		// Dialog choices in The Dig / Full Throttle are drawn in the bottom
		// status area (y >= ~160). Actor speech (y around 8) is excluded.
		if (bt.ypos < 160) continue;
		Common::String text = cleanGameText(safeUtf8(Common::String((const char *)bt.text)));
		text.trim();
		if (text.empty()) continue;
		V7Choice c;
		c.text = text;
		c.x = bt.xpos;
		c.y = bt.ypos;
		fresh.push_back(c);
	}
	// The Dig draws conversation choices as picture-icon blast OBJECTS in the
	// bottom dialog panel rather than blast text. Capture them while a dialog
	// input script is active. Each choice icon is a blast object nested inside
	// a wider "dialog background" blast object; the nested ones are the
	// selectable choices. The blast rect is in room coordinates (its top was
	// offset by _screenTop at enqueue time), so the on-screen click target is
	// the rect centre with _screenTop subtracted back out; x carries no scroll.
	if (fresh.empty() && _vm->_game.id == GID_DIG) {
		bool dialogPending = (_baseVerbScript != 0 && _vm->VAR_VERB_SCRIPT != 0xFF &&
		                      (int)_vm->VAR(_vm->VAR_VERB_SCRIPT) != _baseVerbScript);
		if (dialogPending) {
			ScummEngine_v6 *v6 = (ScummEngine_v6 *)_vm;
			int n = v6->_blastObjectQueuePos;
			if (n > (int)ARRAYSIZE(v6->_blastObjectQueue))
				n = ARRAYSIZE(v6->_blastObjectQueue);
			for (int i = 0; i < n; ++i) {
				const ScummEngine_v6::BlastObject &eo = v6->_blastObjectQueue[i];
				// A choice icon sits inside a strictly larger blast object (the
				// dialog background). Standalone panels contain nothing and are
				// skipped.
				bool nested = false;
				for (int j = 0; j < n; ++j) {
					if (j == i) continue;
					const Common::Rect &o = v6->_blastObjectQueue[j].rect;
					if (o.left <= eo.rect.left && o.right >= eo.rect.right &&
					    o.top <= eo.rect.top && o.bottom >= eo.rect.bottom &&
					    (o.width() > eo.rect.width() || o.height() > eo.rect.height())) {
						nested = true;
						break;
					}
				}
				if (!nested)
					continue;
				V7Choice c;
				c.objNumber = eo.number;
				Common::String nm = safeUtf8(getObjName(this, eo.number));
				const char *iconLabel = nm.empty() ? dialogIconLabel(GID_DIG, eo.number) : nullptr;
				if (iconLabel)
					c.text = iconLabel;
				else
					c.text = nm.empty() ? Common::String::format("icon_%d", eo.number)
					                    : normalizeActionName(nm);
				// Store the icon centre in ROOM coordinates. The dialog input
				// script hit-tests VAR_VIRT_MOUSE (room space) against the icon
				// positions, so the click dispatch needs room — not screen —
				// coordinates. The blast rect is already room space (its top was
				// offset by _screenTop at enqueue time).
				c.x = (eo.rect.left + eo.rect.right) / 2;
				c.y = (eo.rect.top + eo.rect.bottom) / 2;
				fresh.push_back(c);
			}
		}
	}
	// Only overwrite when the new snapshot is non-empty: between
	// re-renders the script briefly empties the queue, and we'd lose the
	// choices if we cleared on every frame.
	if (!fresh.empty())
		_v7DialogChoices = fresh;
}

const byte *ScummMcpBridge::callGetObjOrActorName(int obj) const {
	return _vm ? _vm->getObjOrActorName(obj) : nullptr;
}

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

void ScummMcpBridge::registerTools() {
	// --- state ---
	{
		Common::JSONObject inputProps;
		Common::JSONObject outputProps;
		Common::JSONObject roomProps;
		roomProps.setVal("id", mcpProp("integer", "Current room ID"));
		roomProps.setVal("name", mcpProp("string", "Human-readable room name (optional)"));
		Common::JSONObject roomSchema;
		roomSchema.setVal("type", mcpJsonString("object"));
		roomSchema.setVal("properties", new Common::JSONValue(roomProps));
		outputProps.setVal("room", new Common::JSONValue(roomSchema));

		Common::JSONObject positionProps;
		positionProps.setVal("x", mcpProp("integer", "X coordinate"));
		positionProps.setVal("y", mcpProp("integer", "Y coordinate"));
		Common::JSONObject positionSchema;
		positionSchema.setVal("type", mcpJsonString("object"));
		positionSchema.setVal("properties", new Common::JSONValue(positionProps));
		outputProps.setVal("position", new Common::JSONValue(positionSchema));

		auto makeStringArray = []() -> Common::JSONValue * {
			Common::JSONObject arr;
			arr.setVal("type",  mcpJsonString("array"));
			arr.setVal("items", mcpProp("string"));
			return new Common::JSONValue(arr);
		};
		outputProps.setVal("verbs",     makeStringArray());
		outputProps.setVal("inventory", makeStringArray());

		if (_vm->_game.id == GID_MANIAC) {
			outputProps.setVal("controlling",          mcpProp("string", "Name of the currently controlled kid"));
			outputProps.setVal("available_characters", makeStringArray());
		}

		Common::JSONObject objectItemProps;
		objectItemProps.setVal("id",              mcpProp("integer", "Object ID"));
		objectItemProps.setVal("name",            mcpProp("string",  "Object name"));
		objectItemProps.setVal("state",           mcpProp("integer", "Object state"));
		objectItemProps.setVal("x",               mcpProp("integer", "X coordinate"));
		objectItemProps.setVal("y",               mcpProp("integer", "Y coordinate"));
		objectItemProps.setVal("pathway",         mcpProp("boolean", "Is pathway/exit"));
		objectItemProps.setVal("compatible_verbs",mcpProp("array",   "Verbs that have script handlers for this object"));
		Common::JSONObject objectItem;
		objectItem.setVal("type",       mcpJsonString("object"));
		objectItem.setVal("properties", new Common::JSONValue(objectItemProps));
		Common::JSONObject objectsArray;
		objectsArray.setVal("type",  mcpJsonString("array"));
		objectsArray.setVal("items", new Common::JSONValue(objectItem));
		outputProps.setVal("objects", new Common::JSONValue(objectsArray));

		// actors[] removed — NPCs now appear in objects[] with compatible_verbs

		Common::JSONObject msgItemProps;
		msgItemProps.setVal("text",  mcpProp("string", "Message text"));
		msgItemProps.setVal("actor", mcpProp("string", "Actor name (optional)"));
		Common::JSONObject msgItem;
		msgItem.setVal("type",       mcpJsonString("object"));
		msgItem.setVal("properties", new Common::JSONValue(msgItemProps));
		Common::JSONObject messagesArray;
		messagesArray.setVal("type",  mcpJsonString("array"));
		messagesArray.setVal("items", new Common::JSONValue(msgItem));
		outputProps.setVal("messages", new Common::JSONValue(messagesArray));

		Common::JSONObject choiceItemProps;
		choiceItemProps.setVal("id",    mcpProp("integer", "1-based choice index"));
		choiceItemProps.setVal("label", mcpProp("string",  "Choice text"));
		Common::JSONObject choiceItem;
		choiceItem.setVal("type",       mcpJsonString("object"));
		choiceItem.setVal("properties", new Common::JSONValue(choiceItemProps));
		Common::JSONObject choicesArray;
		choicesArray.setVal("type",  mcpJsonString("array"));
		choicesArray.setVal("items", new Common::JSONValue(choiceItem));
		Common::JSONObject questionProps;
		questionProps.setVal("choices", new Common::JSONValue(choicesArray));
		Common::JSONObject questionSchema;
		questionSchema.setVal("type",       mcpJsonString("object"));
		questionSchema.setVal("properties", new Common::JSONValue(questionProps));
		outputProps.setVal("question", new Common::JSONValue(questionSchema));

		Networking::McpServer::ToolSpec spec;
		spec.name = "state";
		spec.description =
		    "Returns the current game state: room, position, inventory, scene objects "
		    "(including NPCs with their compatible_verbs — always includes talk_to), "
		    "active verbs, latest messages (cleared after reading), "
		    "and pending dialog question if any. The player character is never listed. "
		    "Use act(verb='talk_to', target1=<npc_name>) to speak to an NPC.";
		spec.inputSchema  = mcpObjectSchema(inputProps);
		spec.outputSchema = mcpObjectSchema(outputProps);
		spec.streaming    = false;
		_server->registerTool(spec);
	}

	// Shared output schema factory for streaming tools.
	auto makeChangesSchema = []() -> Common::JSONValue * {
		Common::JSONObject props;
		props.setVal("inventory_added", mcpProp("array",   "Names of items added to inventory"));
		props.setVal("room_changed",    mcpProp("integer", "New room number (only present if room changed)"));
		Common::JSONObject posProps;
		posProps.setVal("x", mcpProp("integer", "X coordinate"));
		posProps.setVal("y", mcpProp("integer", "Y coordinate"));
		Common::JSONObject posSchema;
		posSchema.setVal("type",       mcpJsonString("object"));
		posSchema.setVal("properties", new Common::JSONValue(posProps));
		props.setVal("position",        new Common::JSONValue(posSchema));
		props.setVal("objects_changed", mcpProp("array",  "Objects whose state changed: [{name, old_state, new_state}]"));
		props.setVal("messages",        mcpProp("array",  "Dialog/narration lines spoken during the action: [{text, actor?}]"));
		props.setVal("question",        mcpProp("object", "Pending dialog question if action ended with one: {choices:[{id,label}]}"));
		return mcpObjectSchema(props);
	};

	// --- act ---
	{
		Common::JSONObject props;
		props.setVal("verb",    mcpProp("string", "Verb name (e.g. 'open', 'use'). Required."));
		props.setVal("target1", mcpPropOneOf("string", "integer",
		    "Primary target: name or numeric id of an object/inventory item "
		    "currently present in state (objects[] or inventory[]). "
		    "NPCs appear in objects[] and can be targeted by name. "
		    "For 'use X on Y', this is X."));
		props.setVal("target2", mcpPropOneOf("string", "integer",
		    "Secondary target for 'use X on Y' (Y): name or numeric id, "
		    "must currently exist in state."));
		const char *req[] = {"verb"};
		Networking::McpServer::ToolSpec spec;
		spec.name = "act";
		spec.description =
		    "Perform a verb action on one or two named targets. Blocks until the "
		    "action/cutscene sequence completes, streaming dialog and events via SSE, "
		    "then returns state changes. For walking to specific coordinates, use 'walk'. "
		    "IMPORTANT: Actions are sequential - only one can be in progress at a time. "
		    "Wait for the previous act/answer/walk call to complete before sending the next one. "
		    "Fails if a question is pending (use 'answer' first) or another action is running.";
		spec.inputSchema  = mcpObjectSchema(props, req, 1);
		spec.outputSchema = makeChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}

	// --- answer ---
	{
		Common::JSONObject props;
		props.setVal("id", mcpProp("integer", "1-indexed dialog choice (1 = first option shown in state.question.choices)."));
		const char *req[] = {"id"};
		Networking::McpServer::ToolSpec spec;
		spec.name = "answer";
		spec.description =
		    "Select a dialog choice by 1-based index. Blocks until the conversation "
		    "sequence completes, streaming events via SSE, then returns state changes. "
		    "IMPORTANT: Actions are sequential - only one can be in progress at a time. "
		    "Wait for the previous act/answer/walk call to complete before sending the next one. "
		    "Fails if no question is currently pending or another action is running.";
		spec.inputSchema  = mcpObjectSchema(props, req, 1);
		spec.outputSchema = makeChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}

	// --- walk ---
	{
		Common::JSONObject props;
		props.setVal("x", mcpProp("integer", "Target X pixel coordinate (auto-clamped to room bounds)"));
		props.setVal("y", mcpProp("integer", "Target Y pixel coordinate (auto-clamped to room bounds)"));
		const char *req[] = {"x", "y"};
		Networking::McpServer::ToolSpec spec;
		spec.name = "walk";
		spec.description =
		    "Walk ego to explicit (x, y) pixel coordinates in the current room. "
		    "Out-of-bounds values are automatically clamped to the room bounds. "
		    "Use 'act' with verb='walk_to' and target1=<name> to walk to a named object. "
		    "Blocks until the walk completes and returns state changes.";
		spec.inputSchema  = mcpObjectSchema(props, req, 2);
		spec.outputSchema = makeChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}

	// --- skip ---
	if (_skipToolEnabled) {
		Networking::McpServer::ToolSpec spec;
		spec.name = "skip";
		spec.description =
		    "Skip/cancel current action or cutscene by simulating an Escape key press. "
		    "Useful for skipping long intros or animations. Returns state changes.";
		spec.inputSchema  = nullptr;  // No input required
		spec.outputSchema = makeChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}

	// --- play_note (Loom only) ---
	{
		Common::JSONObject props;
		props.setVal("note", mcpProp("string",
		    "Single note to play on the Loom distaff. One of: c d e f g a b C "
		    "(C is the high octave)."));
		props.setVal("notes", mcpProp("array",
		    "Optional sequence of note strings to play in order, e.g. ['e','c','e','d']."));
		Networking::McpServer::ToolSpec spec;
		spec.name = "play_note";
		spec.description =
		    "Play Loom distaff notes. Accepts either {note:'c'} for one note, or "
		    "{notes:['e','c','e','d']} to play a full sequence in one call. "
		    "Only valid in the Loom segment of Passport to Adventure (and full Loom).";
		spec.inputSchema  = mcpObjectSchema(props);
		spec.outputSchema = makeChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}

	// --- shoot_cannon (CMI cannon minigame only) ---
	if (_vm->_game.id == GID_CMI) {
		Common::JSONObject props;
		props.setVal("x", mcpProp("integer",
		    "Screen X coordinate to aim the cannon at (0–639)."));
		props.setVal("y", mcpProp("integer",
		    "Screen Y coordinate to aim the cannon at (0–479)."));
		const char *req[] = {"x", "y"};
		Networking::McpServer::ToolSpec spec;
		spec.name = "shoot_cannon";
		spec.description =
		    "Aim the cannon at screen position (x, y) and fire a cannonball. "
		    "Only available in the Curse of Monkey Island cannon minigame. "
		    "Moves the mouse cursor to the target coordinates and simulates a "
		    "left click to fire. Blocks until the shot resolves — cannonball "
		    "flight, explosion, and any resulting speech (e.g. Guybrush "
		    "apologising if the fort is hit) — then returns state changes.";
		spec.inputSchema  = mcpObjectSchema(props, req, 2);
		spec.outputSchema = makeChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}

	// --- switch_character (Maniac Mansion only) ---
	// V0 (C64/Apple II) maps F1-F3 to switchActor(slot)/VAR(97+slot); the
	// V1/V2 ports use the in-game "New Kid" verb but share the same ego/kid
	// vars, so the tool drives the switch directly for them.
	if (_vm->_game.id == GID_MANIAC) {
		Common::JSONObject props;
		props.setVal("name", mcpProp("string",
		    "Name of the kid to control, as listed in state.available_characters (e.g. 'dave')."));
		const char *req[] = {"name"};
		Networking::McpServer::ToolSpec spec;
		spec.name = "switch_character";
		spec.description =
		    "Switch the player-controlled kid (the F1-F3 keys in Maniac Mansion). "
		    "state lists the available names in 'available_characters' and the "
		    "current one in 'controlling'. Only allowed during normal gameplay (not "
		    "in a cutscene and not while kid switching is disabled). Blocks until "
		    "the switch settles, then returns state changes — room_changed/position "
		    "reflect the newly controlled kid.";
		spec.inputSchema  = mcpObjectSchema(props, req, 1);
		spec.outputSchema = makeChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}

	// --- dial (Maniac Mansion phone keypad) ---
	if (_vm->_game.id == GID_MANIAC) {
		Common::JSONObject props;
		props.setVal("number", mcpProp("string",
		    "The number to dial, as a string of keypad keys: digits 0-9 plus "
		    "'*' and '#' (e.g. '1234')."));
		const char *req[] = {"number"};
		Networking::McpServer::ToolSpec spec;
		spec.name = "dial";
		spec.description =
		    "Dial a number on the phone dial pad in Maniac Mansion. Only valid "
		    "while the dial pad is on screen (use the phone first via "
		    "act(verb='use', target1='phone')). Presses the keypad buttons one "
		    "at a time, blocks until the sequence (and any resulting call) "
		    "settles, then returns state changes.";
		spec.inputSchema  = mcpObjectSchema(props, req, 1);
		spec.outputSchema = makeChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}

	// --- debug tools (gated by mcp_debug ini option) ---
	if (_debugToolsEnabled) {
		// debug — return raw engine state for diagnostics
		{
			Networking::McpServer::ToolSpec spec;
			spec.name = "debug";
			spec.description =
			    "Return raw engine state for diagnostics: room id, ego position, "
			    "_userPut, _mouse, _virtualMouse, _leftBtnPressed, _rightBtnPressed, "
			    "_mouseAndKeyboardStat, _keyPressed, _currentRoom, plus a slice of "
			    "SCUMM script vars (0..127 by default; pass 'from' and 'to' to widen). "
			    "Engine-version-agnostic.";
			Common::JSONObject props;
			props.setVal("from", mcpProp("integer",
			    "First SCUMM var index to return (default 0)."));
			props.setVal("to", mcpProp("integer",
			    "Last SCUMM var index to return (inclusive, default 127)."));
			spec.inputSchema  = mcpObjectSchema(props);
			spec.outputSchema = nullptr;
			spec.streaming    = false;
			_server->registerTool(spec);
		}
		// keystroke — inject a key event
		{
			Networking::McpServer::ToolSpec spec;
			spec.name = "keystroke";
			spec.description =
			    "Inject a keyboard keystroke into the engine. Sets _keyPressed so "
			    "the next engine frame processes it. Useful for skipping cutscenes "
			    "(Escape), opening menus, or sending game-specific shortcuts.";
			Common::JSONObject props;
			props.setVal("key", mcpProp("string",
			    "Key to press: a single ASCII character ('a', 'C', '1'), or a name "
			    "('Escape', 'Return', 'Space', 'Tab', 'Backspace', 'F1'..'F12', "
			    "'Up', 'Down', 'Left', 'Right')."));
			props.setVal("ctrl",  mcpProp("boolean", "Hold Ctrl modifier (default false)."));
			props.setVal("shift", mcpProp("boolean", "Hold Shift modifier (default false)."));
			props.setVal("alt",   mcpProp("boolean", "Hold Alt modifier (default false)."));
			const char *req[] = {"key"};
			spec.inputSchema  = mcpObjectSchema(props, req, 1);
			spec.outputSchema = nullptr;
			spec.streaming    = false;
			_server->registerTool(spec);
		}
		// mouse_move — set the virtual mouse position
		{
			Networking::McpServer::ToolSpec spec;
			spec.name = "mouse_move";
			spec.description =
			    "Move the virtual mouse cursor to (x, y) in room/screen coordinates. "
			    "Updates _mouse, _virtualMouse, and VAR_VIRT_MOUSE_X/Y so the engine "
			    "and scripts read the new position. Does not click.";
			Common::JSONObject props;
			props.setVal("x", mcpProp("integer", "X coordinate."));
			props.setVal("y", mcpProp("integer", "Y coordinate."));
			const char *req[] = {"x", "y"};
			spec.inputSchema  = mcpObjectSchema(props, req, 2);
			spec.outputSchema = nullptr;
			spec.streaming    = false;
			_server->registerTool(spec);
		}
		// mouse_click — simulate a mouse click at (x, y)
		{
			Networking::McpServer::ToolSpec spec;
			spec.name = "mouse_click";
			spec.description =
			    "Simulate a mouse click at (x, y). The engine processes the click "
			    "the same way as a real player click (walks ego, runs verb script, "
			    "etc.). Set 'double' for a double click. Button defaults to left.";
			Common::JSONObject props;
			props.setVal("x", mcpProp("integer", "X coordinate."));
			props.setVal("y", mcpProp("integer", "Y coordinate."));
			props.setVal("button", mcpProp("string",
			    "Mouse button: 'left' (default), 'right', or 'middle'."));
			props.setVal("double", mcpProp("boolean",
			    "True for a double click (two clicks within ~250ms). Default false."));
			const char *req[] = {"x", "y"};
			spec.inputSchema  = mcpObjectSchema(props, req, 2);
			spec.outputSchema = nullptr;
			spec.streaming    = false;
			_server->registerTool(spec);
		}
	}
}

// ---------------------------------------------------------------------------
// Tool dispatch
// ---------------------------------------------------------------------------

Common::JSONValue *ScummMcpBridge::callTool(const Common::String &name,
                                             const Common::JSONValue &args,
                                             Common::String &errorOut) {
	if (!_vm) {
		errorOut = "No game loaded";
		return nullptr;
	}
	if (name == "state")
		return toolState(args, errorOut);
	if (name == "act") {
		if (!toolAct(args, errorOut)) return nullptr;
		return nullptr; // streaming started
	}
	if (name == "answer") {
		if (!toolAnswer(args, errorOut)) return nullptr;
		return nullptr;
	}
	if (name == "walk") {
		if (!toolWalk(args, errorOut)) return nullptr;
		return nullptr;
	}
	if (name == "skip") {
		if (!toolSkip(args, errorOut)) return nullptr;
		return nullptr;
	}
	if (name == "play_note") {
		if (!toolPlayNote(args, errorOut)) return nullptr;
		return nullptr;
	}
	if (name == "shoot_cannon") {
		if (!toolShootCannon(args, errorOut)) return nullptr;
		return nullptr;
	}
	if (name == "switch_character") {
		if (!toolSwitchCharacter(args, errorOut)) return nullptr;
		return nullptr;
	}
	if (name == "dial") {
		if (!toolDial(args, errorOut)) return nullptr;
		return nullptr;
	}
	if (name == "debug")        return toolDebug(args, errorOut);
	if (name == "keystroke")    {
		if (!toolKeystroke(args, errorOut)) return nullptr;
		return new Common::JSONValue(Common::JSONObject());
	}
	if (name == "mouse_move")   {
		if (!toolMouseMove(args, errorOut)) return nullptr;
		return new Common::JSONValue(Common::JSONObject());
	}
	if (name == "mouse_click")  {
		if (!toolMouseClick(args, errorOut)) return nullptr;
		return new Common::JSONValue(Common::JSONObject());
	}
	errorOut = "Unknown tool: " + name;
	return nullptr;
}

// ---------------------------------------------------------------------------
// Tool: state
// ---------------------------------------------------------------------------

Common::JSONValue *ScummMcpBridge::toolState(const Common::JSONValue &, Common::String &) {
	Common::JSONObject out;

	Common::JSONObject roomObj;
	roomObj.setVal("id", mcpJsonInt(_vm->_currentRoom));
	if (_vm->_objs && _vm->_numLocalObjects > 0 && _vm->_objs[0].obj_nr) {
		Common::String rn = getObjName(this, _vm->_objs[0].obj_nr);
		if (!rn.empty()) {
			rn = normalizeActionName(rn);
			bool hasCtrl = false;
			for (uint ci = 0; ci < rn.size(); ++ci)
				if ((unsigned char)rn[ci] < 0x20) { hasCtrl = true; break; }
			if (!hasCtrl)
				roomObj.setVal("name", mcpJsonString(safeUtf8(rn)));
		}
	}
	out.setVal("room", new Common::JSONValue(roomObj));

	Actor *ego = getEgoActor();
	if (ego) {
		Common::JSONObject pos;
		pos.setVal("x", mcpJsonInt(ego->getRealPos().x));
		pos.setVal("y", mcpJsonInt(ego->getRealPos().y));
		out.setVal("position", new Common::JSONValue(pos));
	}

	// Maniac Mansion: expose the switchable kids and the current one so
	// clients can drive the switch_character tool by name.
	if (_vm->_game.id == GID_MANIAC) {
		Common::Array<ManiacKid> kids;
		collectManiacKids(kids);
		if (!kids.empty()) {
			int egoNum = (_vm->VAR_EGO != 0xFF) ? (int)_vm->VAR(_vm->VAR_EGO) : -1;
			Common::JSONArray charArr;
			for (uint i = 0; i < kids.size(); ++i) {
				charArr.push_back(mcpJsonString(kids[i].name));
				if (kids[i].actorId == egoNum)
					out.setVal("controlling", mcpJsonString(kids[i].name));
			}
			out.setVal("available_characters", new Common::JSONValue(charArr));
		}
	}

	// Check for pending dialog question before building the verb bar.
	// When a question is pending, the verb bar is replaced by dialog choices
	// (in V4/MI1, the same verb slots are reused with new text; in V5/Indy4,
	// new slots are added). Either way, we emit an empty verbs list and put
	// the choices into 'question' instead.
	bool questionPending = hasPendingQuestion();

	struct VerbInfo { int verbId; Common::String name; Common::String label; };
	Common::Array<VerbInfo> activeVerbs;
	Common::JSONArray verbsArr;
	if (!questionPending) {
		for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
			const VerbSlot &vs = _vm->_verbs[slot];
			if (!vs.verbid || vs.saveid != 0 || (_vm->_game.version > 0 && vs.verbid == 1)) continue;
			if (vs.curmode == 0 && (vs.key < '1' || vs.key > '9')) continue;
			if (vs.curmode != 0 && vs.curmode != 1) continue;
			const byte *ptr2 = _vm->getResourceAddress(rtVerb, slot);
			if (!ptr2) continue;
			byte textBuf2[256];
			_vm->convertMessageToString(ptr2, textBuf2, sizeof(textBuf2));
			if (!textBuf2[0]) continue;
			Common::String label = mcpLowerTrimmed((const char *)textBuf2);
			if (label.empty()) continue;
			if (label == "obim") continue;
			bool labelHasCtrl = false;
			for (uint ci = 0; ci < label.size(); ++ci)
				if ((unsigned char)label[ci] < 0x20) { labelHasCtrl = true; break; }
			if (labelHasCtrl) continue;
			Common::String safe2 = safeUtf8(normalizeActionName(label));
			Common::String safeLabel = safeUtf8(label);
			verbsArr.push_back(mcpJsonString(safeLabel));
			VerbInfo vi;
			vi.verbId = vs.verbid;
			vi.name   = safe2;
			vi.label  = safeLabel;
			activeVerbs.push_back(vi);
		}
	}

	// V6+ games use image verbs (kImageVerbType) with no text labels. Build the
	// verb list from the canonical verbid -> name table for any image-type slots
	// not already captured by the text path above.
	if (_vm->_game.version >= 6 && !questionPending) {
		for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
			const VerbSlot &vs = _vm->_verbs[slot];
			if (!vs.verbid || vs.saveid != 0) continue;
			if (_vm->_game.version > 0 && vs.verbid == 1) continue;
			if (vs.curmode == 0) continue;
			if (vs.type != kImageVerbType) continue;
			const V6VerbEntry *entry = findV6Verb(vs.verbid);
			if (!entry) continue;
			bool alreadyAdded = false;
			for (uint k = 0; k < activeVerbs.size(); ++k)
				if (activeVerbs[k].verbId == vs.verbid) { alreadyAdded = true; break; }
			if (alreadyAdded) continue;
			verbsArr.push_back(mcpJsonString(entry->label));
			VerbInfo vi2;
			vi2.verbId = vs.verbid;
			vi2.name   = entry->name;
			vi2.label  = entry->label;
			activeVerbs.push_back(vi2);
		}
	}

	// The Dig and Full Throttle (both V7) use single-cursor / pie-menu interfaces
	// with no persistent verb bar. Expose 'interact' (universal context action)
	// and 'use_item' (inventory item on room object) — both map to verb ID 7 internally.
	if (_vm->_game.id == GID_DIG && !questionPending && activeVerbs.empty()) {
		struct FallbackVerb { int id; const char *name; const char *label; };
		static const FallbackVerb kV7Fallback[] = {
			{7, "interact", "interact"},
			{7, "use_item", "use item"},
			{0, nullptr,    nullptr}
		};
		for (int i = 0; kV7Fallback[i].name; ++i) {
			verbsArr.push_back(mcpJsonString(kV7Fallback[i].label));
			VerbInfo vi;
			vi.verbId = kV7Fallback[i].id;
			vi.name   = kV7Fallback[i].name;
			vi.label  = kV7Fallback[i].label;
			activeVerbs.push_back(vi);
		}
	}

	// Full Throttle uses a verb-coin: holding over a hotspot pops up three icons
	// around Ben's head — fist (grab/punch/use), kick (boot), mouth (talk/look) —
	// plus a generic single-click action. Expose the three coin verbs, a generic
	// 'interact', and 'use item' (inventory item on a target). Each maps to a real
	// per-object verb script (fist=9, mouth=8, kick=5); 'interact' picks the
	// object's best available action at dispatch time.
	if (_vm->_game.id == GID_FT && !questionPending && activeVerbs.empty()) {
		struct FallbackVerb { int id; const char *name; const char *label; };
		static const FallbackVerb kFtFallback[] = {
			{9,  "fist",     "fist"},
			{5,  "kick",     "kick"},
			{8,  "mouth",    "mouth"},
			{-3, "walk_to",  "walk to"},
			{-1, "interact", "interact"},
			{-1, "use_item", "use item"},
			{0,  nullptr,    nullptr}
		};
		for (int i = 0; kFtFallback[i].name; ++i) {
			verbsArr.push_back(mcpJsonString(kFtFallback[i].label));
			VerbInfo vi;
			vi.verbId = kFtFallback[i].id;
			vi.name   = kFtFallback[i].name;
			vi.label  = kFtFallback[i].label;
			activeVerbs.push_back(vi);
		}
	}

	// Sam & Max (V6) does not populate the classic text verb slots; expose a
	// stable MCP verb set even when _verbs[] is empty.
	// Loom (full game) and the Loom mini-game inside Passport to Adventure use
	// a single-cursor model + distaff instead of the V3 text verb bar. Discard
	// any V3 verbs that the text-slot extraction may have populated and expose
	// only 'interact' (left-click) and 'use_item' (inventory-on-object). Note
	// input goes through the separate 'play_note' tool. The Indy3 and MI1
	// segments of Passport to Adventure keep the standard V3 verb bar.
	if (isInLoomSection() && !questionPending) {
		verbsArr.clear();
		activeVerbs.clear();
		struct FallbackVerb { int id; const char *name; const char *label; };
		static const FallbackVerb kLoomFallback[] = {
			{11, "interact", "interact"},
			{11, "use_item", "use item"},
			{0,  nullptr,    nullptr}
		};
		for (int i = 0; kLoomFallback[i].name; ++i) {
			verbsArr.push_back(mcpJsonString(kLoomFallback[i].label));
			VerbInfo vi;
			vi.verbId = kLoomFallback[i].id;
			vi.name   = kLoomFallback[i].name;
			vi.label  = kLoomFallback[i].label;
			activeVerbs.push_back(vi);
		}
	}

	if (_vm->_game.id == GID_SAMNMAX && !questionPending && activeVerbs.empty()) {
		struct FallbackVerb { int id; const char *name; const char *label; };
		static const FallbackVerb kSamnMaxFallback[] = {
			{13, "walk_to", "walk to"},
			{15, "look_at", "look at"},
			{11, "use", "use"},
			{6,  "talk_to", "talk to"},
			{14, "pick_up", "pick up"},
			{0, nullptr, nullptr}
		};
		for (int i = 0; kSamnMaxFallback[i].name; ++i) {
			verbsArr.push_back(mcpJsonString(kSamnMaxFallback[i].label));
			VerbInfo vi;
			vi.verbId = kSamnMaxFallback[i].id;
			vi.name = kSamnMaxFallback[i].name;
			vi.label = kSamnMaxFallback[i].label;
			activeVerbs.push_back(vi);
		}
	}

	// Curse of Monkey Island (V8) uses a single-cursor model similar to The Dig
	// and Full Throttle, with no persistent verb bar. Expose the 5 core verbs.
	// Always clear whatever the text-slot scan may have picked up (e.g. lingering
	// dialog-choice slots after a conversation ends) and replace with the fixed set.
	if (_vm->_game.id == GID_CMI && !questionPending) {
		verbsArr.clear();
		activeVerbs.clear();
		struct FallbackVerb { int id; const char *name; const char *label; };
		static const FallbackVerb kCMIFallback[] = {
			{13, "walk_to", "walk to"},
			{6,  "talk_to", "talk to"},
			{14, "pick_up", "pick up"},
			{5,  "look_at", "look at"},
			{7,  "use", "use"},
			{0, nullptr, nullptr}
		};
		for (int i = 0; kCMIFallback[i].name; ++i) {
			verbsArr.push_back(mcpJsonString(kCMIFallback[i].label));
			VerbInfo vi;
			vi.verbId = kCMIFallback[i].id;
			vi.name = kCMIFallback[i].name;
			vi.label = kCMIFallback[i].label;
			activeVerbs.push_back(vi);
		}
	}

	out.setVal("verbs", new Common::JSONValue(verbsArr));

	Common::Array<NamedEntity> entities;
	buildEntityMap(entities);

	Common::JSONArray inventory, objects;
	for (uint i = 0; i < entities.size(); ++i) {
		const NamedEntity &ne = entities[i];
		// displayName is already UTF-8 (buildEntityMap normalized it).
		Common::String safe = ne.displayName;
		switch (ne.kind) {
		case NamedEntity::kInventory: {
			Common::String cleanItem = cleanGameText(safe);
			if (!cleanItem.empty()) {
				inventory.push_back(mcpJsonString(cleanItem));
			}
			break;
		}
		case NamedEntity::kObject: {
			// Skip objects that are out of bounds for the object space
			if (_vm->_numGlobalObjects > 0 && ne.numId >= _vm->_numGlobalObjects) break;

			// Find the actual verb bar labels and check if verbs exist
			Common::String lookAtLabel, walkToLabel;
			bool lookAtExists = false, walkToExists = false;
			for (uint k = 0; k < activeVerbs.size(); ++k) {
				if (activeVerbs[k].name == "look_at") { lookAtLabel = activeVerbs[k].label; lookAtExists = true; }
				if (activeVerbs[k].name == "walk_to") { walkToLabel = activeVerbs[k].label; walkToExists = true; }
			}

			Common::JSONArray compatVerbs;
			bool hasLookAt = false, hasWalkTo = false;
			int handlerCount = 0;
			bool walkToHasHandler = false;
			// The Dig and Full Throttle (V7) use click-callbacks / pie-menu rather than
			// per-verb SCUMM entrypoints, so getVerbEntrypoint returns 0 for all objects.
			// Treat every selectable object as supporting all exposed verbs.
			// Loom (and the Loom segment of Passport to Adventure) similarly uses a
			// single-cursor model where 'interact' applies to every selectable object.
			// Curse of Monkey Island (V8) also uses a single-cursor model.
			if (_vm->_game.id == GID_FT) {
				// Full Throttle objects carry real per-verb entrypoints. List only
				// the coin verbs (fist=9/mouth=8/kick=5) the object actually scripts,
				// then always offer the generic 'interact' and 'use item'.
				for (uint k = 0; k < activeVerbs.size(); ++k) {
					const VerbInfo &v = activeVerbs[k];
					// Negative ids are generic sentinels (interact / use item /
					// walk to) that apply to every object; coin verbs (>0) are
					// listed only when the object actually scripts them.
					bool include = (v.verbId < 0) ||
					               (_vm->getVerbEntrypoint(ne.numId, v.verbId) != 0);
					if (include) {
						compatVerbs.push_back(mcpJsonString(v.label));
						if (v.verbId > 0) handlerCount++;
					}
				}
			} else if (_vm->_game.id == GID_DIG || _vm->_game.id == GID_CMI || isInLoomSection()) {
				for (uint k = 0; k < activeVerbs.size(); ++k) {
					compatVerbs.push_back(mcpJsonString(activeVerbs[k].label));
					handlerCount++;
				}
			} else {
				Common::Array<int> countedVerbIds;
				for (uint k = 0; k < activeVerbs.size(); ++k) {
					if (_vm->getVerbEntrypoint(ne.numId, activeVerbs[k].verbId) != 0) {
						compatVerbs.push_back(mcpJsonString(activeVerbs[k].label));
						// Count each unique verbId only once for pathway detection.
						bool counted = false;
						for (uint j = 0; j < countedVerbIds.size(); ++j)
							if (countedVerbIds[j] == activeVerbs[k].verbId) { counted = true; break; }
						if (!counted) {
							countedVerbIds.push_back(activeVerbs[k].verbId);
							handlerCount++;
						}
						if (activeVerbs[k].name == "look_at") hasLookAt = true;
						if (activeVerbs[k].name == "walk_to") { hasWalkTo = true; walkToHasHandler = true; }
					}
				}
				if (!hasLookAt && lookAtExists) compatVerbs.push_back(mcpJsonString(lookAtLabel));
				if (!hasWalkTo && walkToExists) compatVerbs.push_back(mcpJsonString(walkToLabel));
			}

			// CMI (V8): pathway objects have no action verb handlers (ep 6/7/8 all 0).
			// These are exit hotspots where the game dispatches verb 1 (internal walk/default).
			bool isCMIPathway = (_vm->_game.id == GID_CMI) &&
			    _vm->getVerbEntrypoint(ne.numId, 6) == 0 &&
			    _vm->getVerbEntrypoint(ne.numId, 7) == 0 &&
			    _vm->getVerbEntrypoint(ne.numId, 8) == 0;
			// Full Throttle (V7): exit hotspots (e.g. the alley scene exits) have no
			// verb-coin handlers — fist (9), mouth (8) and kick (5) all 0. Mirror CMI:
			// these are pure walk-through pathways the player clicks to change rooms.
			// Objects that DO script a coin verb (a kickable door, a usable lever) are
			// not flagged, even when they double as an exit, since they advertise real
			// actions.
			bool isFTPathway = (_vm->_game.id == GID_FT) &&
			    _vm->getVerbEntrypoint(ne.numId, 9) == 0 &&
			    _vm->getVerbEntrypoint(ne.numId, 8) == 0 &&
			    _vm->getVerbEntrypoint(ne.numId, 5) == 0;
			bool isPathway = isCMIPathway || isFTPathway ||
			    ((_vm->_game.id != GID_DIG && _vm->_game.id != GID_FT && _vm->_game.id != GID_CMI && !isInLoomSection()) && walkToHasHandler && (handlerCount == 1));

			Common::JSONObject obj;
			obj.setVal("id",               mcpJsonInt(ne.numId));
			obj.setVal("name",             mcpJsonString(safe));
			obj.setVal("state",            mcpJsonInt(_vm->getState(ne.numId)));
			obj.setVal("x",                mcpJsonInt(_vm->getObjX(ne.numId)));
			obj.setVal("y",                mcpJsonInt(_vm->getObjY(ne.numId)));
			obj.setVal("pathway",          mcpJsonBool(isPathway));
			obj.setVal("compatible_verbs", new Common::JSONValue(compatVerbs));
			objects.push_back(new Common::JSONValue(obj));
			break;
		}
		case NamedEntity::kActor: {
			int actorObjId = _vm->actorToObj(ne.numId);
			// Skip actors whose converted object ID is out of bounds
			if (_vm->_numGlobalObjects > 0 && actorObjId >= _vm->_numGlobalObjects) break;

			// Find the actual verb bar label for talk_to and check if it exists
			Common::String talkToLabel;
			bool talkToExists = false;
			for (uint k = 0; k < activeVerbs.size(); ++k) {
				if (activeVerbs[k].name == "talk_to") { talkToLabel = activeVerbs[k].label; talkToExists = true; }
			}

			Common::JSONArray compatVerbs;
			bool hasTalkTo = false;
			// For GID_DIG and GID_FT, all selectable actors support 'interact' (click-callback / pie-menu model).
			// Same reasoning applies to Loom's single-cursor model and CMI's single-cursor model.
			if (_vm->_game.id == GID_DIG || _vm->_game.id == GID_FT || _vm->_game.id == GID_CMI || isInLoomSection()) {
				for (uint k = 0; k < activeVerbs.size(); ++k)
					compatVerbs.push_back(mcpJsonString(activeVerbs[k].label));
			} else {
				for (uint k = 0; k < activeVerbs.size(); ++k) {
					if (_vm->getVerbEntrypoint(actorObjId, activeVerbs[k].verbId) != 0) {
						compatVerbs.push_back(mcpJsonString(activeVerbs[k].label));
						if (activeVerbs[k].name == "talk_to") hasTalkTo = true;
					}
				}
				if (!hasTalkTo && talkToExists) compatVerbs.push_back(mcpJsonString(talkToLabel));
			}
			Common::JSONObject actorObj;
			actorObj.setVal("id",               mcpJsonInt(actorObjId));
			actorObj.setVal("name",             mcpJsonString(safe));
			actorObj.setVal("state",            mcpJsonInt(_vm->getState(actorObjId)));
			actorObj.setVal("x",                mcpJsonInt(_vm->getObjX(actorObjId)));
			actorObj.setVal("y",                mcpJsonInt(_vm->getObjY(actorObjId)));
			actorObj.setVal("pathway",          mcpJsonBool(false));
			actorObj.setVal("compatible_verbs", new Common::JSONValue(compatVerbs));
			objects.push_back(new Common::JSONValue(actorObj));
			break;
		}
		}
	}
	out.setVal("inventory", new Common::JSONValue(inventory));
	out.setVal("objects",   new Common::JSONValue(objects));

	// Indy3 fist-fight HUD — surface each fighter's health and punch-power
	// gauge so the MCP client can mirror what the in-game HUD shows
	// ("Indiana Jones' Health" / "Punch power" / "Boxing Coach's Health" /
	// "Punch power"). Driven by Indy3's script vars 322..327.
	if (isInIndy3Fight()) {
		Common::JSONObject fight;

		Common::JSONObject indy;
		indy.setVal("health",      mcpJsonInt((int)_vm->_scummVars[325]));
		indy.setVal("punch_power", mcpJsonInt((int)_vm->_scummVars[322]));
		fight.setVal("indy", new Common::JSONValue(indy));

		Common::JSONObject opponent;
		opponent.setVal("health",      mcpJsonInt((int)_vm->_scummVars[327]));
		opponent.setVal("punch_power", mcpJsonInt((int)_vm->_scummVars[323]));
		fight.setVal("opponent", new Common::JSONValue(opponent));

		out.setVal("fight", new Common::JSONValue(fight));
	}

	Common::JSONArray msgsArr;
	for (uint i = 0; i < _messages.size(); ++i) {
		const MessageEntry &m = _messages[i];
		Common::String cleanText = cleanGameText(safeUtf8(m.text));
		if (cleanText.empty()) continue;
		Common::JSONObject entry;
		if (m.actorId >= 0) {
			int objId = _vm->actorToObj(m.actorId);
			// Only include actor name if the object ID is within bounds
			if (_vm->_numGlobalObjects <= 0 || objId < _vm->_numGlobalObjects) {
				Common::String actorName = getObjName(this, objId);
				if (!actorName.empty()) {
					Common::String safe = safeUtf8(mcpLowerTrimmed(actorName));
					entry.setVal("actor", mcpJsonString(safe));
				}
			}
		}
		entry.setVal("text", mcpJsonString(cleanText));
		msgsArr.push_back(new Common::JSONValue(entry));
	}
	out.setVal("messages", new Common::JSONValue(msgsArr));
	_messages.clear();

	if (questionPending) {
		int choiceCount = 0;
		Common::JSONArray choiceList;
		// V7 (Dig/FT): dialog choices are not in verb slots. Expose placeholder
		// numbered choices so the client can call answer(id) with a digit key.
		// The actual choice labels are shown on screen and not accessible from here.
		bool v7DialogPending = (_vm->_game.version == 7 && _baseVerbScript != 0 &&
		                        _vm->VAR_VERB_SCRIPT != 0xFF &&
		                        (int)_vm->VAR(_vm->VAR_VERB_SCRIPT) != _baseVerbScript);
		if (_vm->_game.id == GID_SAMNMAX && !_v7DialogChoices.empty()) {
			Common::Array<V7Choice> sorted = _v7DialogChoices;
			for (uint i = 0; i + 1 < sorted.size(); ++i) {
				for (uint j = 0; j + 1 < sorted.size() - i; ++j) {
					bool swap = (sorted[j].y > sorted[j + 1].y) ||
					            (sorted[j].y == sorted[j + 1].y && sorted[j].x > sorted[j + 1].x);
					if (swap) { V7Choice tmp = sorted[j]; sorted[j] = sorted[j + 1]; sorted[j + 1] = tmp; }
				}
			}
			for (uint i = 0; i < sorted.size(); ++i) {
				Common::JSONObject choice;
				choice.setVal("id",    mcpJsonInt((int)i + 1));
				choice.setVal("label", mcpJsonString(sorted[i].text));
				choiceList.push_back(new Common::JSONValue(choice));
				++choiceCount;
			}
		} else if (v7DialogPending) {
			// V7 (Dig/FT) dialog choices are drawn directly via the blast-text
			// queue (no verb slots are populated). The bridge snapshots the
			// queue in onV7BlastTextSnapshot(); sort the captured lines by Y so
			// choice IDs match top-to-bottom screen order. For The Dig demo the
			// choices are icon blast-OBJECTS instead, so _v7DialogChoices may be
			// empty — in that case expose 9 numbered placeholders so callers can
			// at least dismiss the dialog.
			Common::Array<V7Choice> sorted = _v7DialogChoices;
			for (uint i = 0; i + 1 < sorted.size(); ++i) {
				for (uint j = 0; j + 1 < sorted.size() - i; ++j) {
					bool swap = (sorted[j].y > sorted[j + 1].y) ||
					            (sorted[j].y == sorted[j + 1].y && sorted[j].x > sorted[j + 1].x);
					if (swap) {
						V7Choice tmp = sorted[j];
						sorted[j] = sorted[j + 1];
						sorted[j + 1] = tmp;
					}
				}
			}
			if (sorted.empty()) {
				const int kV7MaxChoices = 9;
				for (int i = 1; i <= kV7MaxChoices; ++i) {
					Common::JSONObject choice;
					choice.setVal("id",    mcpJsonInt(i));
					choice.setVal("label", mcpJsonString(Common::String::format("Choice %d", i)));
					choiceList.push_back(new Common::JSONValue(choice));
					choiceCount = i;
				}
			} else {
				for (uint i = 0; i < sorted.size(); ++i) {
					Common::JSONObject choice;
					choice.setVal("id",    mcpJsonInt((int)i + 1));
					choice.setVal("label", mcpJsonString(sorted[i].text));
					choiceList.push_back(new Common::JSONValue(choice));
					++choiceCount;
				}
			}
		} else if (_vm->_game.version >= 6) {
			// V6+/V8 dialog choices are usually non-action verb slots. In COMI (V8)
			// they are full sentences and may not always follow curmode conventions.
			for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
				const VerbSlot &vs = _vm->_verbs[slot];
				if (!vs.verbid || vs.saveid != 0) continue;
				if (_vm->_game.version > 0 && vs.verbid == 1) continue;
				if (isV6ActionVerb(vs.verbid)) continue;
				Common::String label;
				if (const byte *ptr = _vm->getResourceAddress(rtVerb, slot)) {
					byte textBuf[256] = {};
					_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
					label = cleanGameText(safeUtf8(Common::String((const char *)textBuf)));
				}
				bool allowAsChoice = true;
				if (_vm->_game.version >= 7) {
					if (_vm->_game.version == 8) {
						allowAsChoice = (vs.curmode == 1);
					} else {
						allowAsChoice = (vs.curmode == 1) || isSentenceLikeDialogLabel(label) || (vs.key >= '1' && vs.key <= '9');
					}
				} else {
					if (vs.curmode == 0 && (vs.key < '1' || vs.key > '9')) allowAsChoice = false;
					if (vs.curmode != 0 && vs.curmode != 1) allowAsChoice = false;
				}
				if (!allowAsChoice) continue;
				if (label.empty())
					label = Common::String::format("Topic %d", choiceCount + 1);
				Common::JSONObject choice;
				choice.setVal("id",    mcpJsonInt(++choiceCount));
				choice.setVal("label", mcpJsonString(label));
				choiceList.push_back(new Common::JSONValue(choice));
			}
		} else {
			for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
				const VerbSlot &vs = _vm->_verbs[slot];
				if (!vs.verbid || vs.saveid != 0 || (_vm->_game.version > 0 && vs.verbid == 1)) continue;
				if (vs.curmode == 0 && (vs.key < '1' || vs.key > '9')) continue;
				if (vs.curmode != 0 && vs.curmode != 1) continue;
				const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
				if (!ptr) continue;
				byte textBuf[256];
				_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
				if (!textBuf[0]) continue;
				Common::String cleanLabel = cleanGameText(safeUtf8(Common::String((const char *)textBuf)));
				if (cleanLabel.empty()) continue;
				Common::JSONObject choice;
				choice.setVal("id",    mcpJsonInt(++choiceCount));
				choice.setVal("label", mcpJsonString(cleanLabel));
				choiceList.push_back(new Common::JSONValue(choice));
			}
		}
		if (choiceCount > 0) {
			Common::JSONObject question;
			question.setVal("choices", new Common::JSONValue(choiceList));
			out.setVal("question", new Common::JSONValue(question));
		}
	}

	return new Common::JSONValue(out);
}

// ---------------------------------------------------------------------------
// Tool: act
// ---------------------------------------------------------------------------

bool ScummMcpBridge::toolAct(const Common::JSONValue &args, Common::String &errorOut) {
	if (_streaming) {
		errorOut = "act: another action is already in progress";
		return false;
	}
	if (_vm->_userPut <= 0) {
		errorOut = "act: game is not accepting input right now";
		return false;
	}
	if (hasPendingQuestion()) {
		errorOut = "act: a dialog question is pending — use 'answer' first";
		return false;
	}
	if (!args.isObject()) {
		errorOut = "act: arguments must be an object";
		return false;
	}

	const Common::JSONObject &a = args.asObject();
	if (!a.contains("verb") || !a["verb"]->isString()) {
		errorOut = "act: missing 'verb'";
		return false;
	}
	Common::String verbStr = a["verb"]->asString();

	int verbId = -1;
	if (!resolveVerb(verbStr, verbId)) {
		errorOut = "act: unknown verb '" + verbStr + "'";
		return false;
	}

	auto resolveTarget = [&](const char *param, int &out) -> bool {
		if (!a.contains(param)) return true;
		const Common::JSONValue *v = a[param];
		if (v->isIntegerNumber()) {
			out = (int)v->asIntegerNumber();
			if (out < 0) {
				errorOut = Common::String::format("act: %s id %d is negative", param, out);
				return false;
			}
			if (_vm->_numGlobalObjects > 0 && out >= _vm->_numGlobalObjects) {
				errorOut = Common::String::format(
					"act: %s id %d out of bounds (0-%d)",
					param, out, _vm->_numGlobalObjects - 1);
				return false;
			}
			return true;
		}
		if (v->isString()) {
			NamedEntity ent;
			if (!resolveEntityByName(v->asString(), ent)) {
				const char *str = v->asString().c_str();
				char *endptr = nullptr;
				long val = strtol(str, &endptr, 10);
				if (endptr != str && *endptr == '\0') {
					out = (int)val;
					if (out < 0) {
						errorOut = Common::String::format("act: %s id %d is negative", param, out);
						return false;
					}
				} else {
					errorOut = Common::String("act: unknown ") + param + " '" + v->asString() + "'";
					return false;
				}
			} else {
				out = (ent.kind == NamedEntity::kActor) ? _vm->actorToObj(ent.numId) : ent.numId;
			}
			if (_vm->_numGlobalObjects > 0 && out >= _vm->_numGlobalObjects) {
				errorOut = Common::String::format(
					"act: %s id %d out of bounds (0-%d)",
					param, out, _vm->_numGlobalObjects - 1);
				return false;
			}
			return true;
		}
		errorOut = Common::String("act: ") + param + " must be a string name or integer id";
		return false;
	};

	int targetA = 0, targetB = 0;
	if (!resolveTarget("target1", targetA)) return false;
	if (!resolveTarget("target2", targetB)) return false;

	debug(1, "mcp: act verb='%s' verbId=%d targetA=%d targetB=%d",
	      verbStr.c_str(), verbId, targetA, targetB);

	// Full Throttle "interact" (generic single-action button): the verb-coin
	// sentinel (-1) with a single target resolves to whichever verb-coin action
	// the object actually scripts — preferring fist (9, use/grab), then mouth
	// (8, examine/talk), then kick (5). This mirrors the default action a single
	// click performs in-game and gives a useful response instead of the no-op a
	// raw scene click produces. "use item" (two targets) keeps the click path.
	if (_vm->_game.id == GID_FT && verbId == -1 && targetA != 0 && targetB == 0) {
		static const int kFtCoinVerbs[] = {9, 8, 5};
		for (int i = 0; i < 3; ++i) {
			if (_vm->getVerbEntrypoint(targetA, kFtCoinVerbs[i]) != 0) {
				verbId = kFtCoinVerbs[i];
				break;
			}
		}
		debug(1, "mcp: FT interact resolved to verbId=%d for target %d", verbId, targetA);
	}

	// For Indy4, actors are handled by the sentence script, not verb entrypoints.
	// Skip the entrypoint check for actors and proceed to doSentence.
	bool isIndy4Actor = _vm->_game.id == GID_INDY4 && targetA != 0 && _vm->objIsActor(targetA);
	bool isSamnMaxActor = _vm->_game.id == GID_SAMNMAX && targetA != 0 && _vm->objIsActor(targetA);

	// Indy4 single-target actor sentences (e.g. talk_to sophia) dispatched via
	// doSentence() do not reliably fire the talk action — the sentence script
	// walks ego a few pixels and stops without triggering dialog. Replicating
	// the user click flow (activate verb on the verb bar, then scene-click on
	// the actor) goes through the same input scripts the engine uses for real
	// clicks and reliably opens the dialog. Restrict to single-target sentences;
	// 2-target verbs like 'give X to Y' still use doSentence.
	bool isIndy4ActorClick = (isIndy4Actor || isSamnMaxActor) && targetB == 0;

	// CMI (V8): dispatch via doSentence() directly.
	bool isCMIClick = false;

	// Loom interact: skip the entrypoint check entirely (verbId == -1 sentinel
	// means we're dispatching via simulated scene click, not doSentence).
	bool isLoomClick = (verbId == -1);

	// V7 (The Dig / Full Throttle) single-cursor model: route every act() with
	// a target through a simulated scene click so the engine's verb script
	// picks the right action — talk for actors, look/pick-up for scenery,
	// walk-to + room transition for pathway clearings (which have ep_13 but no
	// ep_7, so doSentence(7, ...) would never trigger their walk_to handler).
	// The Dig: every targeted act routes through a simulated scene click.
	// Full Throttle: only the generic "interact"/"use_item" sentinel (verbId==-1)
	// uses the click path; explicit verb-coin actions (fist/kick/mouth) carry a
	// real verb id and are dispatched via doSentence below.
	bool isV7NaturalClick = (targetA != 0) &&
	                        (_vm->_game.id == GID_DIG ||
	                         (_vm->_game.id == GID_FT && verbId == -1));

	if (targetA != 0 && !isIndy4Actor && !isSamnMaxActor && !isLoomClick && !isCMIClick && !isV7NaturalClick) {
		int ep = _vm->getVerbEntrypoint(targetA, verbId);
		debug(1, "mcp: act entrypoint for obj %d verb %d = %d", targetA, verbId, ep);

		// For Indiana Jones: if this object has no handler, search for one that does
		if (ep == 0 && _vm->_game.id == GID_INDY4 && _vm->_objs && verbId > 0) {
			for (int i = 1; i < _vm->_numLocalObjects; ++i) {
				const ObjectData &od = _vm->_objs[i];
				if (!od.obj_nr) continue;
				if (_vm->_numGlobalObjects > 0 && od.obj_nr >= _vm->_numGlobalObjects) continue;
				int tryEp = _vm->getVerbEntrypoint(od.obj_nr, verbId);
				if (tryEp != 0) {
					debug(1, "mcp: no handler on %d, trying object %d instead", targetA, od.obj_nr);
					targetA = od.obj_nr;
					ep = tryEp;
					break;
				}
			}
		}
	}

	// In Maniac Mansion, executing verbs without entrypoints can cause out-of-bounds errors
	// when the default sentence handling code accesses object 386. Skip execution if no handler.
	if (_vm->_game.id == GID_MANIAC && targetA != 0) {
		int ep = _vm->getVerbEntrypoint(targetA, verbId);
		if (ep == 0) {
			debug(1, "mcp: skipping verb with no entrypoint on object %d", targetA);
			errorOut = "verb has no handler for this object";
			return false;
		}
		// In V0, certain transitive verbs require a second object (direct object).
		// Executing them without one causes a crash in the sentence handler.
		// For 'use' the preposition comes from the object's OBCD header (see
		// getVerbPrepId): objects like the phone take "use <obj>" with no
		// second object, so only reject when the object actually demands one.
		if (_vm->_game.version == 0 && targetB == 0 &&
		    (verbId == kVerbUse || verbId == kVerbGive || verbId == kVerbUnlock || verbId == kVerbFix)) {
			bool needsSecond = true;
			if (verbId == kVerbUse) {
				const byte *obcd = _vm->getOBCDFromObject(targetA, true);
				if (obcd && (*(obcd + 11) >> 5) == kVerbPrepNone)
					needsSecond = false;
			}
			if (needsSecond) {
				debug(1, "mcp: skipping verb %d on object %d (requires second object)", verbId, targetA);
				errorOut = "transitive verb requires second object";
				return false;
			}
		}
	}

	// For V0: ScummEngine_v0 asserts that the primary object (st.objectA) exists.
	// We must check if targetA is valid and present before starting the action.
	if (_vm->_game.version == 0) {
		if (targetA == 0 || _vm->whereIsObject(targetA) == WIO_NOT_FOUND) {
			errorOut = "target object does not exist or is not available";
			return false;
		}
	}

	snapshotPreAction();
	_streaming = true;
	_sseAnswerStream = false;
	_sseStartFrame = _frameCounter;
	_sseDoneAtFrame = 0;
	_sseStuckAtFrame = 0;
	_sseLastEventFrame = 0;
	_sseEgoMoved = false;
	_sseMessages.clear();
	_ssePendingSecondClick = false;
	_ssePendingNotes.clear();
	_sseButtonClearFrame = 0;
	_ssePendingV7Choice = 0;
	_ssePendingV7UseClick = false;
	_sseSnmTalkActor = 0;
	_sseSnmTalkClicks = 0;
	_sseSnmCursorTarget = 0;
	_sseSnmPendingUseTarget = 0;
	_sseSnmForcedCursor = 0;
	// Capture the current input script so we can detect when the game switches
	// to a dialog-mode script (V7: VAR_VERB_SCRIPT changes to a different value).
	_sseVerbScript = (_vm->VAR_VERB_SCRIPT != 0xFF) ? (int)_vm->VAR(_vm->VAR_VERB_SCRIPT) : 0;
	_sseInitialVerbScript = _sseVerbScript;
	_sseVerbScriptChanged = false;
	// For V0: track the primary target so isActionDone() can wait for its script to finish.
	// V0 scripts do not lock _userPut during execution, unlike V5, so script-slot polling
	// is the only reliable signal that the verb script has completed.
	// For The Dig (V7): some scenery objects are "walk there, then run a multi-frame
	// entry sequence" exits — e.g. clicking the wreck (obj 81) in room 18 walks Low
	// over and the object script then climbs the team inside and calls startScene
	// (room 19). Ego stops the instant it arrives, so the settle logic would
	// otherwise close the stream with an empty result before that entry script
	// fires the room change. Tracking the clicked object lets the settle logic wait
	// for its script to finish (capped) so a single act() reaches the new room.
	_sseTargetObject = (_vm->_game.version == 0 || _vm->_game.id == GID_DIG) ? targetA : 0;
	if (_vm->_game.id == GID_SAMNMAX && verbId == kSnmTalkSentinel &&
	    targetA != 0 && _vm->objIsActor(targetA)) {
		// Sam & Max talk_to: there is no talk verb to dispatch — instead drive
		// the in-game interface. pumpStream right-clicks to cycle the context
		// cursor to the "mouth" icon over the actor, then left-clicks to open
		// the conversation (whose topic icons are then exposed as choices).
		_sseSnmTalkActor = targetA;
		_sseSnmCursorTarget = kSnmMouthCursor;
		_sseSnmHovered = false;
		_sseSnmTalkClicks = 0;
		_sseSnmTalkNextFrame = _frameCounter + 1;
	} else if (_vm->_game.id == GID_SAMNMAX && verbId == 7 && targetA != 0 &&
	           targetB == 0 && !_vm->objIsActor(targetA) &&
	           _vm->getVerbEntrypoint(targetA, 7) == 0) {
		// Sam & Max 'use' on an object that carries no verb-7 script (e.g. the
		// beat-up DeSoto on the street). Boarding the car / its drive-away cutscene
		// is fired by the scene-click input script when clicked with the
		// "use/operate" context cursor — doSentence(7, ...) finds no entrypoint and
		// does nothing. Drive the interface like talk_to does: pumpStream cycles the
		// cursor to the use icon (878) over the object, then left-clicks it. Objects
		// that DO script verb 7 (doors, etc.) keep the doSentence path below.
		_sseSnmTalkActor = targetA;
		_sseSnmCursorTarget = kSnmUseCursor;
		_sseSnmHovered = false;
		_sseSnmTalkClicks = 0;
		_sseSnmTalkNextFrame = _frameCounter + 1;
	} else if (_vm->_game.id == GID_SAMNMAX && verbId == 5 && targetA != 0 &&
	           targetB == 0 && _vm->objIsActor(targetA)) {
		// Sam & Max 'pick_up' on an actor (e.g. Max himself): there is no
		// pickup entrypoint to dispatch — clicking the actor with the hand
		// cursor is the mechanic, and it leaves the actor held as the mouse
		// cursor (Max-in-hand, 889). The machinery tracks the actor's live
		// position, which matters because Max wanders around the scene.
		_sseSnmTalkActor = targetA;
		_sseSnmCursorTarget = kSnmPickupCursor;
		_sseSnmHovered = false;
		_sseSnmTalkClicks = 0;
		_sseSnmTalkNextFrame = _frameCounter + 1;
	} else if (_vm->_game.id == GID_SAMNMAX && verbId == 7 && targetA != 0 &&
	           targetB != 0 && snmIsMaxEntity(targetA)) {
		// Sam & Max 'use Max on Y' (e.g. give Max to the street kitten, which
		// yields the carnival tickets). This is a two-target "give" interaction,
		// and — like the other single-cursor games — a two-target sentence is
		// dispatched straight through doSentence rather than driven by the
		// cursor-cycling pickup machinery used for the single-target context
		// actions (talk/use/pick_up). Compare CMI ("use A on B" -> doSentence(5,
		// A, B)) and The Dig ("use item on target" -> doSentence(3, B, A)):
		// dispatching the sentence directly runs the right per-object script,
		// whereas simulating clicks does not reliably arm the held cursor.
		//
		// "max" may resolve to the Max actor (id 3) or to the inventory tool
		// (max_the_object); the give script keys on the inventory object, so
		// resolve to it. Giving Max produces doSentence(3 'give', max, target).
		int maxObj = (_vm->whereIsObject(targetA) == WIO_INVENTORY) ? targetA : 0;
		if (maxObj == 0) {
			for (int ii = 0; _vm->_inventory && ii < _vm->_numInventory; ++ii) {
				int obj = _vm->_inventory[ii];
				if (obj && normalizeActionName(safeUtf8(getObjName(this, obj))) == "max_the_object") {
					maxObj = obj;
					break;
				}
			}
		}
		if (maxObj == 0) {
			errorOut = "use: Max is not available as a usable item right now";
			_streaming = false;
			return false;
		}
		debug(1, "mcp: Sam & Max give Max (obj %d) to target %d via doSentence(3)", maxObj, targetB);
		_vm->doSentence(3 /* give */, maxObj, targetB);
	} else if (_vm->_game.id == GID_SAMNMAX && verbId == 7 && targetA != 0 &&
	           targetB != 0 && _vm->whereIsObject(targetA) == WIO_INVENTORY) {
		// Sam & Max 'use <held inventory item> on Y' (non-Max items): the held
		// item is its own cursor outside the standard rotation, so click the
		// target with it. doSentence cannot drive this — the interaction lives in
		// the scene-click input script, not in a verb entrypoint — and cycling can
		// never reach an item cursor (a right-click drops the held item), so the
		// item must already be in hand.
		int cur = (kSnmCursorVerbVar < _vm->_numVariables && _vm->_scummVars)
		          ? (int)_vm->_scummVars[kSnmCursorVerbVar] : -1;
		if (cur <= 0 || snmIsStandardCursor(cur)) {
			errorOut = "use: nothing is held — pick the item up first (e.g. act pick_up <item>)";
			_streaming = false;
			return false;
		}
		_sseSnmTalkActor = targetB;
		_sseSnmCursorTarget = kSnmItemCursorSentinel;
		_sseSnmHovered = false;
		_sseSnmTalkClicks = 0;
		_sseSnmTalkNextFrame = _frameCounter + 1;
	} else if (_vm->_game.id == GID_SAMNMAX && verbId == 7 && targetA != 0 &&
	           targetB != 0 && _vm->objIsActor(targetA)) {
		// Sam & Max 'use <actor> on Y' (non-Max actors): the actor must first be
		// picked up (click with the hand cursor), then the target clicked with the
		// held-actor cursor. Chain these two clicks inside one MCP action so
		// callers need not micromanage the transient cursor.
		_sseSnmTalkActor = targetA;
		_sseSnmCursorTarget = kSnmPickupCursor;
		_sseSnmPendingUseTarget = targetB;
		_sseSnmHovered = false;
		_sseSnmTalkClicks = 0;
		_sseSnmTalkNextFrame = _frameCounter + 4; // Delay to allow cursor forcing
	} else if (isIndy4ActorClick) {
		// Activate the verb on the verb bar (sets the cursor verb). Equivalent
		// to the user pressing the verb's keyboard shortcut.
		_vm->runInputScript(kVerbClickArea, verbId, 1);

		// Place the virtual mouse over the actor's screen position and trigger
		// a scene click. The engine's checkExecVerbs() will route this to the
		// scene-click input script, which builds and dispatches the sentence
		// the same way a real click does.
		int objX = _vm->getObjX(targetA);
		int objY = _vm->getObjY(targetA);
		VirtScreen *vs = &_vm->_virtscr[kMainVirtScreen];
		int mouseX = objX - vs->xstart;
		int mouseY = objY + vs->topline;
		if (mouseX < 0) mouseX = 0;
		if (mouseX > _vm->_screenWidth - 1) mouseX = _vm->_screenWidth - 1;
		if (mouseY < 0) mouseY = 0;
		if (mouseY > _vm->_screenHeight - 1) mouseY = _vm->_screenHeight - 1;

		_vm->_mouse.x        = mouseX;
		_vm->_mouse.y        = mouseY;
		_vm->_virtualMouse.x = objX;
		_vm->_virtualMouse.y = objY;
		if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = objX;
		if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = objY;
		if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = mouseX;
		if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = mouseY;

		_vm->_leftBtnPressed |= 0x03; // msClicked | msDown
	} else if (isCMIClick) {
		// CMI: place virtual mouse over target, select verb cursor, then dispatch
		// a scene click via the input scripts. This is the same path the engine
		// uses for real user clicks.
		int objX = _vm->getObjX(targetA);
		int objY = _vm->getObjY(targetA);
		VirtScreen *vs = &_vm->_virtscr[kMainVirtScreen];
		int mouseX = objX - vs->xstart;
		int mouseY = objY + vs->topline;
		if (mouseX < 0) mouseX = 0;
		if (mouseX > _vm->_screenWidth - 1) mouseX = _vm->_screenWidth - 1;
		if (mouseY < 0) mouseY = 0;
		if (mouseY > _vm->_screenHeight - 1) mouseY = _vm->_screenHeight - 1;

		_vm->_mouse.x        = mouseX;
		_vm->_mouse.y        = mouseY;
		_vm->_virtualMouse.x = objX;
		_vm->_virtualMouse.y = objY;
		if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = objX;
		if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = objY;
		if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = mouseX;
		if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = mouseY;

		// Select verb cursor (kVerbClickArea, verbid, mode=1=activate cursor)
		_vm->runInputScript(kVerbClickArea, verbId, 1);
		// Dispatch the scene click (left button). Mode 1 = left click.
		_vm->runInputScript(kSceneClickArea, 0, 1);
	} else if (isLoomClick || isV7NaturalClick) {
		// Convert object world coords to on-screen mouse coords (Passport Loom has
		// horizontally scrolling rooms, so screen X != world X).
		// For V3/V4 Loom rooms, getObjX/Y returns the object's walk-to coordinate
		// (od.walk_x/walk_y) which is where ego walks to — *not* the visible
		// bounding box. The engine's findObject() runs at the click coordinate,
		// so clicking on the walk-to point often misses the leaf/etc bbox. Use
		// the bbox center (od.x_pos + width/2, od.y_pos + height/2) for room
		// objects in V3/V4 so the click actually lands inside the object.
		int objX = _vm->getObjX(targetA);
		int objY = _vm->getObjY(targetA);
		// getObjX/Y returns the object's hotspot/walk-to reference point, which for
		// tall scenery (e.g. The Dig's wreck, obj 81) sits at the base — outside the
		// visible bounding box. The engine's findObject() runs at the click coordinate,
		// so clicking the hotspot misses the bbox and no sentence is set up (ego just
		// walks toward the point and stops). Use the bbox center so the click lands
		// inside the object. V3/V4 (Loom) needs this for leaves/eggs; The Dig (V7)
		// needs it for scenery exits like the wreck. bbox center is always inside the
		// bbox, so this is strictly safer for findObject than the hotspot.
		if ((_vm->_game.version <= 4 || _vm->_game.id == GID_DIG) &&
		    targetA != 0 && !_vm->objIsActor(targetA)) {
			int idx = _vm->getObjectIndex(targetA);
			if (idx >= 0 && _vm->_objs) {
				const ObjectData &od = _vm->_objs[idx];
				if (od.width > 0 && od.height > 0) {
					objX = od.x_pos + od.width / 2;
					objY = od.y_pos + od.height / 2;
				}
			}
		}
		VirtScreen *vs = &_vm->_virtscr[kMainVirtScreen];
		int mouseX = objX - vs->xstart;
		int mouseY = objY + vs->topline;
		if (mouseX < 0) mouseX = 0;
		if (mouseX > _vm->_screenWidth - 1) mouseX = _vm->_screenWidth - 1;
		if (mouseY < 0) mouseY = 0;
		if (mouseY > _vm->_screenHeight - 1) mouseY = _vm->_screenHeight - 1;

		_vm->_mouse.x        = mouseX;
		_vm->_mouse.y        = mouseY;
		_vm->_virtualMouse.x = objX;
		_vm->_virtualMouse.y = objY;
		if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = objX;
		if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = objY;
		if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = mouseX;
		if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = mouseY;

		// V7 (Dig/FT) — single-cursor pie-menu model. The engine's scene-click
		// input script picks the right action (talk_to for actors, look_at for
		// scenery, use for inventory-on-object) based on the active held item
		// and the object class. Replicate a real left-click on the object: the
		// mouse position has already been placed over the target above. For
		// "use item" with a second target we first arm the inventory item as
		// the held cursor by simulating a click on the inventory icon, then
		// click on the room object.
		if (_vm->_game.id == GID_DIG || _vm->_game.id == GID_FT || _vm->_game.id == GID_CMI) {
			// V7 (Dig/FT) and V8 (CMI) single-cursor pie-menu / click model.
			// The engine's scene-click script decides the action based on the held
			// inventory cursor and the object class: no held cursor → talk/interact
			// for actors; look_at cursor held → look action; inventory item held →
			// use-on action. For "interact" (targetB==0) we let the game script
			// decide with whatever cursor is currently held — typically none, which
			// gives the natural talk action when clicking an actor. For "use item"
			// (targetB!=0) we first arm the inventory item as the held cursor.
			if (targetB != 0 && _vm->_game.id == GID_DIG) {
				// V7 The Dig "use item on target": the engine's sentence script
				// dispatches verb 3 with the *target* as object A and the item
				// as object B (the trowel has the verb-3 entrypoint that
				// branches on B's class — actors get "I don't think she'd want
				// that", scenery gets "I can't use these things together", and
				// scripted objects get their own handler). Simulating the click
				// pipeline does not arm the held cursor in this game, so the
				// click on an actor falls back to plain talk_to ("Robbins...");
				// dispatching the sentence directly drives the correct script.
				_vm->doSentence(3, targetB, targetA);
				_server->startStreaming();
				return true;
			}
			if (targetB != 0 && _vm->_game.id == GID_FT) {
				// V7: arm the inventory cursor, then defer the scene click by a
				// frame. The engine schedules the "held cursor" update via a
				// follow-up script (started from kInventoryClickArea) and that
				// state must be in place before the scene-click handler reads
				// it, otherwise the click on an actor is dispatched as plain
				// talk_to instead of use-item.
				_vm->runInputScript(kInventoryClickArea, targetA, 1);
				// Recompute mouse position over targetB so pumpStream's deferred
				// click resolves to it instead of the inventory item.
				int objX2 = _vm->getObjX(targetB);
				int objY2 = _vm->getObjY(targetB);
				VirtScreen *vs2 = &_vm->_virtscr[kMainVirtScreen];
				int mx = objX2 - vs2->xstart;
				int my = objY2 + vs2->topline;
				if (mx < 0) mx = 0;
				if (mx > _vm->_screenWidth - 1) mx = _vm->_screenWidth - 1;
				if (my < 0) my = 0;
				if (my > _vm->_screenHeight - 1) my = _vm->_screenHeight - 1;
				_ssePendingV7UseClick = true;
				_ssePendingV7UseMouseX = mx;
				_ssePendingV7UseMouseY = my;
				_ssePendingV7UseObjX   = objX2;
				_ssePendingV7UseObjY   = objY2;
			} else {
				if (targetB != 0) {
					// CMI / V8 path retained from the original code.
					_vm->runInputScript(kInventoryClickArea, targetA, 0);
					int objX2 = _vm->getObjX(targetB);
					int objY2 = _vm->getObjY(targetB);
					VirtScreen *vs2 = &_vm->_virtscr[kMainVirtScreen];
					int mx = objX2 - vs2->xstart;
					int my = objY2 + vs2->topline;
					if (mx < 0) mx = 0;
					if (mx > _vm->_screenWidth - 1) mx = _vm->_screenWidth - 1;
					if (my < 0) my = 0;
					if (my > _vm->_screenHeight - 1) my = _vm->_screenHeight - 1;
					_vm->_mouse.x        = mx;
					_vm->_mouse.y        = my;
					_vm->_virtualMouse.x = objX2;
					_vm->_virtualMouse.y = objY2;
					if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = objX2;
					if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = objY2;
					if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = mx;
					if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = my;
				}
				_vm->_leftBtnPressed |= 0x03; // msClicked | msDown
				_sseButtonClearFrame = _frameCounter + 2;
			}
		} else {
			// Loom (V3/V4) — keep the original _leftBtnPressed pipeline so that
			// the engine processes the click on the next frame, preserving the
			// double-click cadence required for the egg listen/replay puzzle.
			_vm->_leftBtnPressed |= 0x03; // msClicked | msDown

			// In Loom's single-cursor model, a single click on a room object
			// just walks ego there (and the engine reports the object's name);
			// the actual interaction (egg singing, leaf falling, etc.) needs a
			// second click after ego has arrived. Queue a second click for any
			// non-actor room object so MCP "interact" matches a player's
			// double-click on the scene.
			if (targetA != 0 && !_vm->objIsActor(targetA)) {
				_ssePendingSecondClick = true;
				_sseClickMouseX = mouseX;
				_sseClickMouseY = mouseY;
				_vm->_lastInputScriptTime = _vm->_system->getMillis();
			}
		}
	} else if (targetA == 0) {
		// Verb-only invocation (e.g. Indy3 'travel'): dispatch the verb-click
		// input script directly, the same way the engine handles a click on
		// the verb bar slot.
		_vm->runInputScript(kVerbClickArea, verbId, 1);
	} else if (_vm->_game.id == GID_CMI && verbId == 7 && targetA != 0 && targetB != 0) {
		// CMI "use A on B": the engine's sentence dispatcher uses verb id 5
		// (the same id the engine raises when the player clicks an armed
		// inventory item on a target). doSentence(7, ...) does nothing —
		// the combination table and scripted use-handlers are wired to verb 5.
		// This works uniformly for inv-on-inv (combine table → new item) and
		// inv-on-room (target's verb-5 entrypoint runs with VAR_USE_OBJECT set).
		const int kCmiUseVerb = 5;
		_vm->doSentence(kCmiUseVerb, targetA, targetB);
	} else if (_vm->_game.id == GID_CMI && verbId == 13) {
		// CMI walk_to: verb 13 has no entrypoint in the game, so doSentence(13,...)
		// produces a "No." response. For objects with action handlers, startWalkActor
		// to the stand position is correct. For exit/pathway objects (no action handlers),
		// the game internally uses verb=1 (the walk/click verb) via doSentence — this
		// goes through the sentence script which handles room transitions. Mirror that here.
		bool hasActionHandler = (targetA != 0) &&
		    (_vm->getVerbEntrypoint(targetA, 6) != 0 ||  // look_at
		     _vm->getVerbEntrypoint(targetA, 7) != 0 ||  // pick_up / use
		     _vm->getVerbEntrypoint(targetA, 8) != 0);   // talk_to
		if (!hasActionHandler && targetA != 0) {
			// Exit/pathway: CMI exit hotspots are activated by the game's scene-click
			// handler, which detects objects by bounding box. Simulate a left click at
			// the object's bbox center — the scene script then walks ego there and
			// triggers the room transition, exactly as a real player click would.
			int idx = _vm->getObjectIndex(targetA);
			if (idx >= 0) {
				const ObjectData &od = _vm->_objs[idx];
				int clickX = od.x_pos + od.width  / 2;
				int clickY = od.y_pos + od.height / 2;
				_vm->_mouse.x        = clickX;
				_vm->_mouse.y        = clickY;
				_vm->_virtualMouse.x = clickX;
				_vm->_virtualMouse.y = clickY;
				if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = clickX;
				if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = clickY;
				if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = clickX;
				if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = clickY;
				_vm->_lastInputScriptTime = _vm->_system->getMillis();
				_vm->_leftBtnPressed |= 0x03; // msClicked | msDown
			}
		} else {
			Actor *ego = getEgoActor();
			if (ego) {
				int objX = _vm->getObjX(targetA);
				int objY = _vm->getObjY(targetA);
				ego->startWalkActor(objX, objY, -1);
			}
		}
	} else if (_vm->_game.id == GID_FT && verbId == -3 && targetA != 0) {
		// Full Throttle walk_to: walking is a scene-click in-game, and exit
		// hotspots (scene pathways, and doors once opened — e.g. the room 6 door
		// after it has been kicked open) only transition when the scene-click
		// input script runs, not via startWalkActor. Simulate a left click at the
		// target's location so the verb script walks Ben there and fires any
		// exit/room-transition handler — the same proven path toolWalk() uses for
		// the dumpster climb-out.
		int clickX = _vm->getObjX(targetA);
		int clickY = _vm->getObjY(targetA);
		_vm->_mouse.x        = clickX;
		_vm->_mouse.y        = clickY;
		_vm->_virtualMouse.x = clickX;
		_vm->_virtualMouse.y = clickY;
		if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = clickX;
		if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = clickY;
		if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = clickX;
		if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = clickY;
		_vm->_leftBtnPressed |= 0x03;       // msClicked | msDown
		_debugButtonReleaseFrame = _frameCounter + 2;
	} else {
		_vm->doSentence(verbId, targetA, targetB);
	}
	_server->startStreaming();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: answer
// ---------------------------------------------------------------------------

bool ScummMcpBridge::toolAnswer(const Common::JSONValue &args, Common::String &errorOut) {
	if (_streaming) {
		errorOut = "answer: another action is already in progress";
		return false;
	}
	if (!hasPendingQuestion()) {
		errorOut = "answer: no dialog question is currently pending";
		return false;
	}
	if (!args.isObject()) {
		errorOut = "answer: arguments must be an object";
		return false;
	}
	const Common::JSONObject &a = args.asObject();
	if (!a.contains("id") || !a["id"]->isIntegerNumber()) {
		errorOut = "answer: missing 'id'";
		return false;
	}
	int choiceIdx = (int)a["id"]->asIntegerNumber();
	if (choiceIdx < 1) {
		errorOut = "answer: id must be >= 1";
		return false;
	}

	if (_vm->_game.id == GID_SAMNMAX && !_v7DialogChoices.empty()) {
		Common::Array<V7Choice> sorted = _v7DialogChoices;
		for (uint i = 0; i + 1 < sorted.size(); ++i) {
			for (uint j = 0; j + 1 < sorted.size() - i; ++j) {
				bool swap = (sorted[j].y > sorted[j + 1].y) ||
				            (sorted[j].y == sorted[j + 1].y && sorted[j].x > sorted[j + 1].x);
				if (swap) { V7Choice tmp = sorted[j]; sorted[j] = sorted[j + 1]; sorted[j + 1] = tmp; }
			}
		}
		if (choiceIdx > (int)sorted.size()) {
			errorOut = Common::String::format("answer: choice %d not available (only %u shown)",
			                                  choiceIdx, sorted.size());
			return false;
		}

		snapshotPreAction();
		_streaming = true;
		_sseAnswerStream = true;
		_sseStartFrame = _frameCounter;
		_sseDoneAtFrame = 0;
		_sseStuckAtFrame = 0;
		_sseLastEventFrame = 0;
		_sseEgoMoved = false;
		_sseMessages.clear();
		_ssePendingSecondClick = false;
		_ssePendingNotes.clear();
		_sseTargetObject = 0;
		_sseButtonClearFrame = _frameCounter + 2;
		const V7Choice &ch = sorted[(uint)choiceIdx - 1];
		_vm->_mouse.x = ch.x;
		_vm->_mouse.y = ch.y;
		_vm->_virtualMouse.x = ch.x;
		_vm->_virtualMouse.y = ch.y;
		if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = ch.x;
		if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = ch.y;
		if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = ch.x;
		if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = ch.y;
		_vm->_leftBtnPressed |= 0x03;
		_v7DialogChoices.clear();
		_server->startStreaming();
		return true;
	}

	// V7 (Dig/FT): dialog choices are not in verb slots. The dialog input script
	// (e.g., script 69 in The Dig) reads VAR_MOUSE_Y to pick the choice. The
	// real click target is the Y coordinate of the dialog line as snapshotted
	// by onV7BlastTextSnapshot().
	const bool isV7Dialog = (_vm->_game.version == 7 && _baseVerbScript != 0 &&
	                         _vm->VAR_VERB_SCRIPT != 0xFF &&
	                         (int)_vm->VAR(_vm->VAR_VERB_SCRIPT) != _baseVerbScript);
	if (isV7Dialog) {
		// If we have captured blast-text choices (Full Throttle path), reject
		// out-of-range.
		if (!_v7DialogChoices.empty() && choiceIdx > (int)_v7DialogChoices.size()) {
			errorOut = Common::String::format("answer: choice %d not available (only %u shown)",
			                                  choiceIdx, _v7DialogChoices.size());
			return false;
		}
		if (choiceIdx > 9) {
			errorOut = Common::String::format("answer: V7 dialog supports up to 9 choices, got %d", choiceIdx);
			return false;
		}

		snapshotPreAction();
		_streaming = true;
		_sseStartFrame = _frameCounter;
		_sseDoneAtFrame = 0;
		_sseStuckAtFrame = 0;
		_sseLastEventFrame = 0;
		_sseEgoMoved = false;
		_sseMessages.clear();
		_ssePendingSecondClick = false;
		_ssePendingNotes.clear();
		_sseTargetObject = 0;
		_sseButtonClearFrame = 0;
		// Reset verb-script tracking so the new stream starts fresh.
		_sseVerbScript = (_vm->VAR_VERB_SCRIPT != 0xFF) ? (int)_vm->VAR(_vm->VAR_VERB_SCRIPT) : 0;
		_sseInitialVerbScript = _sseVerbScript;
		_sseVerbScriptChanged = false;
		// Store the choice digit to be fed when the game is ready (userPut > 0).
		// We cannot use _keyPressed here because processInput() consumes it
		// immediately (even if _userPut <= 0 prevents checkExecVerbs from acting).
		_ssePendingV7Choice = choiceIdx;
		_server->startStreaming();
		return true;
	}

	int current = 0;
	int chosenSlot = -1;
	if (_vm->_game.version >= 6) {
		for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
			const VerbSlot &vs = _vm->_verbs[slot];
			if (!vs.verbid || vs.saveid != 0) continue;
			if (_vm->_game.version > 0 && vs.verbid == 1) continue;
			if (isV6ActionVerb(vs.verbid)) continue;
			Common::String label;
			if (const byte *ptr = _vm->getResourceAddress(rtVerb, slot)) {
				byte textBuf[256] = {};
				_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
				label = cleanGameText(safeUtf8(Common::String((const char *)textBuf)));
			}
			bool allowAsChoice = true;
			if (_vm->_game.version >= 7) {
				// V8 (CMI): used dialog choices stay in the slot list with curmode=0;
				// only currently-active choices have curmode=1. Don't fall back to the
				// numeric-key heuristic, which would let used choices through.
				if (_vm->_game.version == 8) {
					allowAsChoice = (vs.curmode == 1);
				} else {
					allowAsChoice = (vs.curmode == 1) || isSentenceLikeDialogLabel(label) || (vs.key >= '1' && vs.key <= '9');
				}
			} else {
				if (vs.curmode == 0 && (vs.key < '1' || vs.key > '9')) allowAsChoice = false;
				if (vs.curmode != 0 && vs.curmode != 1) allowAsChoice = false;
			}
			if (!allowAsChoice) continue;
			++current;
			if (current == choiceIdx) { chosenSlot = slot; break; }
		}
	} else {
		for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
			const VerbSlot &vs = _vm->_verbs[slot];
			if (!vs.verbid || vs.saveid != 0 || (_vm->_game.version > 0 && vs.verbid == 1)) continue;
			// Only accept curmode=0 if slot has numeric key (dialog); otherwise require curmode=1
			if (vs.curmode == 0 && (vs.key < '1' || vs.key > '9')) continue;
			if (vs.curmode != 0 && vs.curmode != 1) continue;
			const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
			if (!ptr) continue;
			byte textBuf[256];
			_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
			if (!textBuf[0]) continue;
			++current;
			if (current == choiceIdx) { chosenSlot = slot; break; }
		}
	}
	if (chosenSlot < 0) {
		errorOut = Common::String::format("answer: choice %d not found (only %d available)", choiceIdx, current);
		return false;
	}

	const VerbSlot &vs = _vm->_verbs[chosenSlot];
	snapshotPreAction();
	_streaming = true;
	_sseAnswerStream = true;
	_sseStartFrame = _frameCounter;
	_sseDoneAtFrame = 0;
	_sseStuckAtFrame = 0;
	_sseLastEventFrame = 0;
	_sseEgoMoved = false;
	_sseMessages.clear();
	_ssePendingSecondClick = false;
	_ssePendingNotes.clear();
	_sseTargetObject = 0;  // dialog answer has no target object
	_sseButtonClearFrame = 0;
	_sseVerbScript = (_vm->VAR_VERB_SCRIPT != 0xFF) ? (int)_vm->VAR(_vm->VAR_VERB_SCRIPT) : 0;
	_sseInitialVerbScript = _sseVerbScript;
	_sseVerbScriptChanged = false;
	// CMI (V8) dialog choices are rendered as text lines on the screen. To
	// dispatch a choice we replicate a real click on the choice line: place the
	// mouse inside the verb slot's rect and run the verb-click input script
	// (mode 1 = activate / select).
	if (_vm->_game.id == GID_CMI) {
		const Common::Rect &rc = vs.curRect;
		int mouseX = (rc.left + rc.right) / 2;
		int mouseY = (rc.top + rc.bottom) / 2;
		if (mouseX < 0) mouseX = 0;
		if (mouseX > _vm->_screenWidth - 1) mouseX = _vm->_screenWidth - 1;
		if (mouseY < 0) mouseY = 0;
		if (mouseY > _vm->_screenHeight - 1) mouseY = _vm->_screenHeight - 1;
		_vm->_mouse.x = mouseX;
		_vm->_mouse.y = mouseY;
		_vm->_virtualMouse.x = mouseX;
		_vm->_virtualMouse.y = mouseY;
		if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = mouseX;
		if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = mouseY;
		if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = mouseX;
		if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = mouseY;
		_vm->runInputScript(kVerbClickArea, vs.verbid, 1);
	} else {
		_vm->runInputScript(kVerbClickArea, vs.verbid, 1);
	}
	_server->startStreaming();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: walk
// ---------------------------------------------------------------------------

bool ScummMcpBridge::toolWalk(const Common::JSONValue &args, Common::String &errorOut) {
	if (_streaming) {
		errorOut = "walk: another action is already in progress";
		return false;
	}
	if (_vm->_userPut <= 0) {
		errorOut = "walk: game is not accepting input right now";
		return false;
	}
	if (hasPendingQuestion()) {
		errorOut = "walk: a dialog question is pending — use 'answer' first";
		return false;
	}
	if (!args.isObject()) {
		errorOut = "walk: arguments must be an object";
		return false;
	}
	const Common::JSONObject &a = args.asObject();
	if (!a.contains("x") || !a["x"]->isIntegerNumber() ||
	    !a.contains("y") || !a["y"]->isIntegerNumber()) {
		errorOut = "walk: 'x' and 'y' integer coordinates are required";
		return false;
	}

	Actor *ego = getEgoActor();
	if (!ego) {
		errorOut = "walk: no ego actor available";
		return false;
	}

	int wx = (int)a["x"]->asIntegerNumber();
	int wy = (int)a["y"]->asIntegerNumber();
	int maxX = (_vm->_roomWidth  > 0 ? _vm->_roomWidth  : _vm->_screenWidth)  - 1;
	int maxY = (_vm->_roomHeight > 0 ? _vm->_roomHeight : _vm->_screenHeight) - 1;
	if (maxX < 0) maxX = 0;
	if (maxY < 0) maxY = 0;
	wx = CLIP<int>(wx, 0, maxX);
	wy = CLIP<int>(wy, 0, maxY);

	snapshotPreAction();
	_streaming = true;
	_sseStartFrame = _frameCounter;
	_sseDoneAtFrame = 0;
	_sseStuckAtFrame = 0;
	_sseLastEventFrame = 0;
	_sseEgoMoved = false;
	_sseMessages.clear();
	_ssePendingSecondClick = false;
	_ssePendingNotes.clear();
	_sseTargetObject = 0;  // walk has no target object
	_sseButtonClearFrame = 0;
	if (_vm->_game.id == GID_FT) {
		// Full Throttle's walk + exit hotspots (e.g. climbing out of the opening
		// dumpster in the start room) are driven by the scene-click input script,
		// not startWalkActor. Simulate a real left click at the target point so
		// the verb script walks Ben there and fires any exit/transition handler.
		// Mirror the debug mouse_click path exactly (raw coords + a button release
		// scheduled in pump()), which is known to trigger the dumpster climb-out.
		_vm->_mouse.x        = wx;
		_vm->_mouse.y        = wy;
		_vm->_virtualMouse.x = wx;
		_vm->_virtualMouse.y = wy;
		if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = wx;
		if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = wy;
		if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = wx;
		if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = wy;
		_vm->_leftBtnPressed |= 0x03;       // msClicked | msDown
		_debugButtonReleaseFrame = _frameCounter + 2;
	} else {
		ego->startWalkActor(wx, wy, -1);
	}
	_server->startStreaming();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: skip
// ---------------------------------------------------------------------------

bool ScummMcpBridge::toolSkip(const Common::JSONValue &args, Common::String &errorOut) {
	// Allow skip even if streaming to interrupt current action
	if (!_streaming) {
		snapshotPreAction();
		_streaming = true;
		_sseStartFrame = _frameCounter;
		_sseDoneAtFrame = 0;
		_sseStuckAtFrame = 0;
		_sseLastEventFrame = 0;
		_sseEgoMoved = false;
		_sseMessages.clear();
		_ssePendingSecondClick = false;
		_ssePendingNotes.clear();
		_sseTargetObject = 0;
		_server->startStreaming();
	}

	// Simulate Escape key press to skip/cancel
	_vm->_keyPressed = Common::KeyCode(27); // ESC key
	return true;
}

// ---------------------------------------------------------------------------
// Tool: play_note (Loom distaff)
// ---------------------------------------------------------------------------

bool ScummMcpBridge::toolPlayNote(const Common::JSONValue &args, Common::String &errorOut) {
	if (_streaming) {
		errorOut = "play_note: another action is already in progress";
		return false;
	}
	if (!isInLoomSection()) {
		errorOut = "play_note: only available in the Loom segment";
		return false;
	}
	if (_vm->_userPut <= 0) {
		errorOut = "play_note: game is not accepting input right now";
		return false;
	}
	if (hasPendingQuestion()) {
		errorOut = "play_note: a dialog question is pending — use 'answer' first";
		return false;
	}
	if (!args.isObject()) {
		errorOut = "play_note: arguments must be an object with 'note' or 'notes'";
		return false;
	}
	const Common::JSONObject &a = args.asObject();

	struct NoteEntry { const char *name; char key; };
	static const NoteEntry kNotes[] = {
		{"c",  'c'}, {"d",  'd'}, {"e",  'e'}, {"f",  'f'},
		{"g",  'g'}, {"a",  'a'}, {"b",  'b'}, {"C",  'C'},
		{nullptr, 0}
	};
	auto mapNote = [&](const Common::String &s) -> char {
		for (int i = 0; kNotes[i].name; ++i)
			if (s == kNotes[i].name) return kNotes[i].key;
		return 0;
	};

	Common::Array<Common::KeyCode> keys;
	if (a.contains("notes") && a["notes"]->isArray()) {
		const Common::JSONArray &arr = a["notes"]->asArray();
		for (uint i = 0; i < arr.size(); ++i) {
			if (!arr[i] || !arr[i]->isString()) {
				errorOut = "play_note: 'notes' must be an array of strings";
				return false;
			}
			Common::String noteStr = arr[i]->asString();
			noteStr.trim();
			char key = mapNote(noteStr);
			if (!key) {
				errorOut = "play_note: unknown note '" + noteStr + "'. Use one of: c d e f g a b C";
				return false;
			}
			keys.push_back((Common::KeyCode)(byte)key);
		}
		if (keys.empty()) {
			errorOut = "play_note: 'notes' must not be empty";
			return false;
		}
	} else if (a.contains("note") && a["note"]->isString()) {
		Common::String noteStr = a["note"]->asString();
		noteStr.trim();
		char key = mapNote(noteStr);
		if (!key) {
			errorOut = "play_note: unknown note '" + noteStr + "'. Use one of: c d e f g a b C";
			return false;
		}
		keys.push_back((Common::KeyCode)(byte)key);
	} else {
		errorOut = "play_note: provide 'note' (string) or 'notes' (array of strings)";
		return false;
	}

	snapshotPreAction();
	_streaming = true;
	_sseStartFrame = _frameCounter;
	_sseDoneAtFrame = 0;
	_sseStuckAtFrame = 0;
	_sseLastEventFrame = 0;
	_sseEgoMoved = false;
	_sseMessages.clear();
	_ssePendingSecondClick = false;
	_ssePendingNotes = keys;
	_sseTargetObject = 0;

	// pumpStream() will dispatch the queued notes via runInputScript on
	// subsequent frames; nothing else to do here.
	_server->startStreaming();
	return true;
}

// ---------------------------------------------------------------------------
// Debug tools: 'debug', 'keystroke', 'mouse_move', 'mouse_click'
// ---------------------------------------------------------------------------

// Map a JSON 'key' value to a Common::KeyState. Single ASCII chars map to
// their Common::KeyCode (which equals the ASCII byte for printable letters
// and digits). Named keys ('Escape', 'Return', 'F1', 'Up'...) map via a
// small table. Returns false on unknown name.
static bool jsonKeyToKeyState(const Common::String &name, bool ctrl, bool shift, bool alt,
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

Common::JSONValue *ScummMcpBridge::toolDebug(const Common::JSONValue &args, Common::String &errorOut) {
	(void)errorOut;

	int from = 0;
	int to   = 127;
	if (args.isObject()) {
		const Common::JSONObject &a = args.asObject();
		if (a.contains("from") && a["from"]->isIntegerNumber())
			from = (int)a["from"]->asIntegerNumber();
		if (a.contains("to")   && a["to"]->isIntegerNumber())
			to   = (int)a["to"]->asIntegerNumber();
	}
	if (from < 0) from = 0;
	if (to >= _vm->_numVariables) to = _vm->_numVariables - 1;
	if (to < from) to = from;

	// Debug-only: trigger ScummVM's own screenshot capture (saved to the
	// configured screenshotpath) so MCP callers can inspect the rendered frame.
	bool wantScreenshot = args.isObject() && args.asObject().contains("screenshot") &&
	                      args.asObject()["screenshot"]->isBool() &&
	                      args.asObject()["screenshot"]->asBool();
	if (wantScreenshot && g_system)
		g_system->saveScreenshot();

	Common::JSONObject out;
	out.setVal("screenshot_saved", mcpJsonBool(wantScreenshot));
	out.setVal("game_id",       mcpJsonInt((int)_vm->_game.id));
	out.setVal("game_version",  mcpJsonInt(_vm->_game.version));
	out.setVal("current_room",  mcpJsonInt(_vm->_currentRoom));
	if (_vm->_game.version == 0)
		out.setVal("current_mode", mcpJsonInt(static_cast<ScummEngine_v0 *>(_vm)->_currentMode));
	out.setVal("user_put",      mcpJsonInt(_vm->_userPut));
	out.setVal("num_variables", mcpJsonInt(_vm->_numVariables));
	out.setVal("num_global_objects", mcpJsonInt(_vm->_numGlobalObjects));
	out.setVal("frame_counter", mcpJsonInt((int)_frameCounter));
	out.setVal("streaming",     mcpJsonBool(_streaming));
	out.setVal("in_loom_section", mcpJsonBool(isInLoomSection()));

	Common::JSONObject mouse;
	mouse.setVal("x", mcpJsonInt(_vm->_mouse.x));
	mouse.setVal("y", mcpJsonInt(_vm->_mouse.y));
	out.setVal("mouse", new Common::JSONValue(mouse));

	Common::JSONObject vmouse;
	vmouse.setVal("x", mcpJsonInt(_vm->_virtualMouse.x));
	vmouse.setVal("y", mcpJsonInt(_vm->_virtualMouse.y));
	out.setVal("virtual_mouse", new Common::JSONValue(vmouse));

	// Camera scroll: screen_x = world_x - camera.xstart (the conversion the
	// scene-click pipeline uses). Exposed so MCP callers can map object world
	// coords to on-screen click positions in horizontally scrolling rooms.
	Common::JSONObject camera;
	camera.setVal("xstart",   mcpJsonInt(_vm->_virtscr[kMainVirtScreen].xstart));
	camera.setVal("topline",  mcpJsonInt(_vm->_virtscr[kMainVirtScreen].topline));
	camera.setVal("cam_x",    mcpJsonInt(_vm->camera._cur.x));
	camera.setVal("cam_y",    mcpJsonInt(_vm->camera._cur.y));
	out.setVal("camera", new Common::JSONValue(camera));

	out.setVal("left_btn_pressed",  mcpJsonInt(_vm->_leftBtnPressed));
	out.setVal("right_btn_pressed", mcpJsonInt(_vm->_rightBtnPressed));
	out.setVal("mouse_keyboard_stat", mcpJsonInt(_vm->_mouseAndKeyboardStat));

	Common::JSONObject keyPressed;
	keyPressed.setVal("keycode", mcpJsonInt((int)_vm->_keyPressed.keycode));
	keyPressed.setVal("ascii",   mcpJsonInt((int)_vm->_keyPressed.ascii));
	keyPressed.setVal("flags",   mcpJsonInt((int)_vm->_keyPressed.flags));
	out.setVal("key_pressed", new Common::JSONValue(keyPressed));

	// Ego actor info
	Actor *ego = getEgoActor();
	if (ego) {
		Common::JSONObject e;
		e.setVal("number", mcpJsonInt(ego->_number));
		e.setVal("x",      mcpJsonInt(ego->getRealPos().x));
		e.setVal("y",      mcpJsonInt(ego->getRealPos().y));
		e.setVal("room",   mcpJsonInt(ego->_room));
		e.setVal("moving", mcpJsonInt(ego->_moving));
		out.setVal("ego", new Common::JSONValue(e));
	}

	// Slice of script vars
	Common::JSONArray vars;
	if (_vm->_scummVars) {
		for (int i = from; i <= to; ++i) {
			Common::JSONObject v;
			v.setVal("i", mcpJsonInt(i));
			v.setVal("v", mcpJsonInt((int)_vm->_scummVars[i]));
			vars.push_back(new Common::JSONValue(v));
		}
	}
	out.setVal("vars", new Common::JSONValue(vars));

	// Verb slots for debugging dialog choices etc.
	Common::JSONArray verbs;
	for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (slot >= 50) break;
		Common::JSONObject v;
		v.setVal("slot",    mcpJsonInt(slot));
		v.setVal("imgindex", mcpJsonInt(vs.imgindex));
		v.setVal("verbid",  mcpJsonInt(vs.verbid));
		v.setVal("saveid",  mcpJsonInt(vs.saveid));
		v.setVal("curmode", mcpJsonInt(vs.curmode));
		v.setVal("color",   mcpJsonInt(vs.color));
		v.setVal("dimcolor",mcpJsonInt(vs.dimcolor));
		v.setVal("hicolor", mcpJsonInt(vs.hicolor));
		v.setVal("key",     mcpJsonInt(vs.key));
		v.setVal("type",    mcpJsonInt(vs.type));
		v.setVal("center",  mcpJsonBool(vs.center));
		Common::JSONObject rect;
		rect.setVal("left",   mcpJsonInt(vs.curRect.left));
		rect.setVal("top",    mcpJsonInt(vs.curRect.top));
		rect.setVal("right",  mcpJsonInt(vs.curRect.right));
		rect.setVal("bottom", mcpJsonInt(vs.curRect.bottom));
		v.setVal("rect", new Common::JSONValue(rect));
		const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
		if (ptr) {
			byte textBuf[256] = {};
			_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
			v.setVal("label", mcpJsonString(safeUtf8((const char *)textBuf)));
		}
		verbs.push_back(new Common::JSONValue(v));
	}
	out.setVal("verbs", new Common::JSONValue(verbs));

	// Scan ALL room objects (including untouchable/hidden ones), useful for finding exits.
	Common::JSONArray roomObjs;
	for (int i = 1; _vm->_objs && i < _vm->_numLocalObjects; ++i) {
		const ObjectData &od = _vm->_objs[i];
		if (!od.obj_nr) continue;
		Common::JSONObject ro;
		ro.setVal("idx",         mcpJsonInt(i));
		ro.setVal("id",          mcpJsonInt(od.obj_nr));
		ro.setVal("x",           mcpJsonInt(_vm->getObjX(od.obj_nr)));
		ro.setVal("y",           mcpJsonInt(_vm->getObjY(od.obj_nr)));
		ro.setVal("x_pos",       mcpJsonInt(od.x_pos));
		ro.setVal("y_pos",       mcpJsonInt(od.y_pos));
		ro.setVal("w",           mcpJsonInt(od.width));
		ro.setVal("h",           mcpJsonInt(od.height));
		ro.setVal("walk_x",      mcpJsonInt(od.walk_x));
		ro.setVal("walk_y",      mcpJsonInt(od.walk_y));
		ro.setVal("state",       mcpJsonInt(od.state));
		ro.setVal("parent",      mcpJsonInt(od.parent));
		ro.setVal("parentstate", mcpJsonInt(od.parentstate));
		ro.setVal("untouchable", mcpJsonBool(_vm->getClass(od.obj_nr, kObjectClassUntouchable)));
		Common::String name = getObjName(this, od.obj_nr);
		ro.setVal("name", mcpJsonString(name.empty() ? Common::String::format("obj-%d", od.obj_nr) : normalizeActionName(name)));
		// Check verb entrypoints for v6+ verbs
		int ep6 = _vm->getVerbEntrypoint(od.obj_nr, 6);
		int ep7 = _vm->getVerbEntrypoint(od.obj_nr, 7);
		int ep8 = _vm->getVerbEntrypoint(od.obj_nr, 8);
		int ep13 = _vm->getVerbEntrypoint(od.obj_nr, 13);
		ro.setVal("ep_6",  mcpJsonInt(ep6));
		ro.setVal("ep_7",  mcpJsonInt(ep7));
		ro.setVal("ep_8",  mcpJsonInt(ep8));
		ro.setVal("ep_13", mcpJsonInt(ep13));
		roomObjs.push_back(new Common::JSONValue(ro));
	}
	out.setVal("room_objects", new Common::JSONValue(roomObjs));

	// V6+ blast-object queue (icon dialog choices in Sam & Max draw here, as in
	// The Dig). Dump for any V6+ game so dialog-icon debugging works for S&M too.
	if (_vm->_game.version >= 6) {
		Common::JSONArray bos6;
		ScummEngine_v6 *v6dbg = (ScummEngine_v6 *)_vm;
		for (int i = 0; i < v6dbg->_blastObjectQueuePos &&
		     i < (int)ARRAYSIZE(v6dbg->_blastObjectQueue); ++i) {
			const ScummEngine_v6::BlastObject &eo = v6dbg->_blastObjectQueue[i];
			Common::JSONObject bo;
			bo.setVal("number", mcpJsonInt(eo.number));
			bo.setVal("name", mcpJsonString(getObjName(this, eo.number)));
			bo.setVal("image", mcpJsonInt(eo.image));
			bo.setVal("mode",  mcpJsonInt(eo.mode));
			bo.setVal("rect_left",  mcpJsonInt(eo.rect.left));
			bo.setVal("rect_top",   mcpJsonInt(eo.rect.top));
			bos6.push_back(new Common::JSONValue(bo));
		}
		out.setVal("blast_objects_v6", new Common::JSONValue(bos6));
	}

	// V7-specific introspection: dump the subtitle queue so callers can see
	// which dialog choice lines are currently rendered and at what coords.
	if (_vm->_game.version == 7) {
		ScummEngine_v7 *v7 = (ScummEngine_v7 *)_vm;
		Common::JSONArray subs;
		for (int i = 0; i < v7->_subtitleQueuePos && i < (int)ARRAYSIZE(v7->_subtitleQueue); ++i) {
			const ScummEngine_v7::SubtitleText &st = v7->_subtitleQueue[i];
			Common::JSONObject s;
			s.setVal("idx",     mcpJsonInt(i));
			s.setVal("x",       mcpJsonInt(st.xpos));
			s.setVal("y",       mcpJsonInt(st.ypos));
			s.setVal("color",   mcpJsonInt(st.color));
			s.setVal("charset", mcpJsonInt(st.charset));
			s.setVal("center",  mcpJsonBool(st.center));
			s.setVal("wrap",    mcpJsonBool(st.wrap));
			s.setVal("speech",  mcpJsonBool(st.actorSpeechMsg));
			s.setVal("text",    mcpJsonString(safeUtf8(cleanGameText(Common::String((const char *)st.text)))));
			subs.push_back(new Common::JSONValue(s));
		}
		out.setVal("subtitle_queue", new Common::JSONValue(subs));
		Common::JSONArray choices;
		for (uint i = 0; i < _v7DialogChoices.size(); ++i) {
			Common::JSONObject c;
			c.setVal("text", mcpJsonString(_v7DialogChoices[i].text));
			c.setVal("x",    mcpJsonInt(_v7DialogChoices[i].x));
			c.setVal("y",    mcpJsonInt(_v7DialogChoices[i].y));
			choices.push_back(new Common::JSONValue(c));
		}
		out.setVal("v7_dialog_choices", new Common::JSONValue(choices));
		// Dump the blast-object queue: in The Dig the dialog topic icons are
		// drawn here, with `number` mapped to the topic's room object.
		Common::JSONArray bos;
		ScummEngine_v6 *v6 = (ScummEngine_v6 *)_vm;
		for (int i = 0; i < v6->_blastObjectQueuePos &&
		     i < (int)ARRAYSIZE(v6->_blastObjectQueue); ++i) {
			const ScummEngine_v6::BlastObject &eo = v6->_blastObjectQueue[i];
			Common::JSONObject bo;
			bo.setVal("number", mcpJsonInt(eo.number));
			bo.setVal("left",   mcpJsonInt(eo.rect.left));
			bo.setVal("top",    mcpJsonInt(eo.rect.top));
			bo.setVal("right",  mcpJsonInt(eo.rect.right));
			bo.setVal("bottom", mcpJsonInt(eo.rect.bottom));
			Common::String nm = getObjName(this, eo.number);
			bo.setVal("name", mcpJsonString(nm));
			bo.setVal("image", mcpJsonInt(eo.image));
			bo.setVal("mode",  mcpJsonInt(eo.mode));
			bos.push_back(new Common::JSONValue(bo));
		}
		out.setVal("blast_objects", new Common::JSONValue(bos));
		out.setVal("screen_top",      mcpJsonInt(_vm->_screenTop));
		out.setVal("main_vs_h",       mcpJsonInt(_vm->_virtscr[kMainVirtScreen].h));
		out.setVal("main_vs_topline", mcpJsonInt(_vm->_virtscr[kMainVirtScreen].topline));
		out.setVal("verb_vs_h",       mcpJsonInt(_vm->_virtscr[kVerbVirtScreen].h));
		out.setVal("verb_vs_topline", mcpJsonInt(_vm->_virtscr[kVerbVirtScreen].topline));
		out.setVal("text_vs_h",       mcpJsonInt(_vm->_virtscr[kTextVirtScreen].h));
		out.setVal("text_vs_topline", mcpJsonInt(_vm->_virtscr[kTextVirtScreen].topline));
		out.setVal("screen_width",    mcpJsonInt(_vm->_screenWidth));
		out.setVal("screen_height",   mcpJsonInt(_vm->_screenHeight));
		out.setVal("var_mouse_x", mcpJsonInt(_vm->VAR_MOUSE_X != 0xFF ? (int)_vm->VAR(_vm->VAR_MOUSE_X) : -2));
		out.setVal("var_mouse_y", mcpJsonInt(_vm->VAR_MOUSE_Y != 0xFF ? (int)_vm->VAR(_vm->VAR_MOUSE_Y) : -2));
		out.setVal("var_virt_mouse_x", mcpJsonInt(_vm->VAR_VIRT_MOUSE_X != 0xFF ? (int)_vm->VAR(_vm->VAR_VIRT_MOUSE_X) : -2));
		out.setVal("var_virt_mouse_y", mcpJsonInt(_vm->VAR_VIRT_MOUSE_Y != 0xFF ? (int)_vm->VAR(_vm->VAR_VIRT_MOUSE_Y) : -2));
		out.setVal("base_verb_script", mcpJsonInt(_baseVerbScript));
		if (_vm->VAR_VERB_SCRIPT != 0xFF)
			out.setVal("verb_script", mcpJsonInt((int)_vm->VAR(_vm->VAR_VERB_SCRIPT)));
	}

	return new Common::JSONValue(out);
}

// ---------------------------------------------------------------------------
// Tool: shoot_cannon (CMI cannon minigame)
// ---------------------------------------------------------------------------

bool ScummMcpBridge::toolShootCannon(const Common::JSONValue &args, Common::String &errorOut) {
	if (_streaming) {
		errorOut = "shoot_cannon: another action is already in progress";
		return false;
	}
	if (_vm->_game.id != GID_CMI) {
		errorOut = "shoot_cannon: only available in Curse of Monkey Island";
		return false;
	}
	if (_vm->_userPut <= 0) {
		errorOut = "shoot_cannon: game is not accepting input right now";
		return false;
	}
	if (!args.isObject()) {
		errorOut = "shoot_cannon: arguments must be an object";
		return false;
	}
	const Common::JSONObject &a = args.asObject();
	if (!a.contains("x") || !a["x"]->isIntegerNumber() ||
	    !a.contains("y") || !a["y"]->isIntegerNumber()) {
		errorOut = "shoot_cannon: integer 'x' and 'y' coordinates are required";
		return false;
	}

	int x = (int)a["x"]->asIntegerNumber();
	int y = (int)a["y"]->asIntegerNumber();
	int maxX = (_vm->_screenWidth  > 0 ? _vm->_screenWidth  : 640) - 1;
	int maxY = (_vm->_screenHeight > 0 ? _vm->_screenHeight : 480) - 1;
	x = CLIP<int>(x, 0, maxX);
	y = CLIP<int>(y, 0, maxY);

	// Move the virtual mouse cursor to the target and fire.
	_vm->_mouse.x        = x;
	_vm->_mouse.y        = y;
	_vm->_virtualMouse.x = x;
	_vm->_virtualMouse.y = y;
	if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = x;
	if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = y;
	if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = x;
	if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = y;
	_vm->_lastInputScriptTime = _vm->_system->getMillis();
	_vm->_leftBtnPressed |= 0x03; // msClicked | msDown

	snapshotPreAction();
	_streaming = true;
	_sseAnswerStream = false;
	_sseStartFrame = _frameCounter;
	_sseDoneAtFrame = 0;
	_sseStuckAtFrame = 0;
	_sseLastEventFrame = 0;
	_sseEgoMoved = false;
	_sseMessages.clear();
	_ssePendingSecondClick = false;
	_ssePendingNotes.clear();
	_sseButtonClearFrame = _frameCounter + 2;
	_ssePendingV7Choice = 0;
	_ssePendingV7UseClick = false;
	_sseVerbScript = 0;
	_sseInitialVerbScript = 0;
	_sseVerbScriptChanged = false;
	_sseTargetObject = 0;
	_server->startStreaming();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: switch_character (Maniac Mansion)
// ---------------------------------------------------------------------------

void ScummMcpBridge::collectManiacKids(Common::Array<ManiacKid> &out) const {
	out.clear();
	if (!_vm || _vm->_game.id != GID_MANIAC) return;
	// V0's F1-F3 handler maps slot N to the actor stored in VAR(97+N) (see
	// ScummEngine_v0::switchActor); the V1/V2 ports keep the same kid vars.
	// Slots holding no valid actor are skipped, so on a variant where these
	// vars are unused the list simply comes out empty.
	for (int slot = 0; slot < 3; ++slot) {
		int actorId = (int)_vm->VAR(97 + slot);
		if (actorId <= 0 || !_vm->isValidActor(actorId)) continue;
		ManiacKid kid;
		kid.slot = slot;
		kid.actorId = actorId;
		Common::String name = getObjName(this, _vm->actorToObj(actorId));
		kid.name = name.empty() ? Common::String::format("actor-%d", actorId)
		                        : normalizeActionName(safeUtf8(name));
		out.push_back(kid);
	}
}

bool ScummMcpBridge::toolSwitchCharacter(const Common::JSONValue &args, Common::String &errorOut) {
	if (_streaming) {
		errorOut = "switch_character: another action is already in progress";
		return false;
	}
	if (_vm->_game.id != GID_MANIAC) {
		errorOut = "switch_character: only available in Maniac Mansion";
		return false;
	}
	if (_vm->_userPut <= 0) {
		errorOut = "switch_character: game is not accepting input right now";
		return false;
	}
	// V0: mirror switchActor()'s own gate so the client gets an error instead
	// of a silent no-op when switching is disallowed (cutscene, keypad, lab
	// door). V1/V2 have no equivalent mode byte; _userPut covers them above.
	if (_vm->_game.version == 0) {
		ScummEngine_v0 *v0 = static_cast<ScummEngine_v0 *>(_vm);
		if (v0->_currentMode != ScummEngine_v0::kModeNormal) {
			errorOut = "switch_character: switching is not allowed right now (cutscene or kid switching disabled)";
			return false;
		}
	}
	if (!args.isObject()) {
		errorOut = "switch_character: arguments must be an object with a 'name' field";
		return false;
	}
	const Common::JSONObject &a = args.asObject();
	if (!a.contains("name") || !a["name"]->isString()) {
		errorOut = "switch_character: 'name' (string) is required";
		return false;
	}

	Common::Array<ManiacKid> kids;
	collectManiacKids(kids);
	Common::String wanted = normalizeActionName(a["name"]->asString());
	const ManiacKid *match = nullptr;
	Common::String available;
	for (uint i = 0; i < kids.size(); ++i) {
		if (!available.empty()) available += ", ";
		available += kids[i].name;
		if (kids[i].name == wanted) match = &kids[i];
	}
	if (!match) {
		errorOut = "switch_character: unknown character '" + a["name"]->asString() +
		           "'. Available: " + (available.empty() ? "(none)" : available);
		return false;
	}

	snapshotPreAction();
	_streaming = true;
	_sseAnswerStream = false;
	_sseStartFrame = _frameCounter;
	_sseDoneAtFrame = 0;
	_sseStuckAtFrame = 0;
	_sseLastEventFrame = 0;
	_sseEgoMoved = false;
	_sseMessages.clear();
	_ssePendingSecondClick = false;
	_ssePendingNotes.clear();
	_sseTargetObject = 0;
	_sseButtonClearFrame = 0;
	if (_vm->_game.version == 0) {
		static_cast<ScummEngine_v0 *>(_vm)->switchActor(match->slot);
	} else {
		// V1/V2: no engine-side helper exists (the original ports switch via
		// the "New Kid" verb script), so replicate V0's switchActor() body.
		_vm->resetSentence();
		_vm->VAR(_vm->VAR_EGO) = match->actorId;
		_vm->actorFollowCamera(match->actorId);
	}
	_server->startStreaming();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: dial (Maniac Mansion phone keypad)
// ---------------------------------------------------------------------------

bool ScummMcpBridge::toolDial(const Common::JSONValue &args, Common::String &errorOut) {
	if (_streaming) {
		errorOut = "dial: another action is already in progress";
		return false;
	}
	if (_vm->_game.id != GID_MANIAC) {
		errorOut = "dial: only available in Maniac Mansion";
		return false;
	}
	if (_vm->_userPut <= 0) {
		errorOut = "dial: game is not accepting input right now";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("number") ||
	    !args.asObject()["number"]->isString()) {
		errorOut = "dial: 'number' (string of keypad keys, e.g. '1234') is required";
		return false;
	}
	Common::String number = args.asObject()["number"]->asString();
	number.trim();
	if (number.empty() || number.size() > 16) {
		errorOut = "dial: 'number' must contain 1-16 keypad keys";
		return false;
	}
	for (uint i = 0; i < number.size(); ++i) {
		char c = number[i];
		if (!(c >= '0' && c <= '9') && c != '*' && c != '#') {
			errorOut = Common::String::format("dial: invalid keypad key '%c' (use 0-9, * or #)", c);
			return false;
		}
	}
	// V0 tracks the dial pad (and other selection screens) via _currentMode.
	// V1/V2 have no mode byte; for them the button-map scan below is the gate.
	if (_vm->_game.version == 0 &&
	    static_cast<ScummEngine_v0 *>(_vm)->_currentMode != ScummEngine_v0::kModeKeypad) {
		errorOut = "dial: no dial pad on screen — use the phone first (act verb='use' target1='phone')";
		return false;
	}

	// Build the key -> button-object map for the current room.
	// Strategy 1: buttons named after their key ("1".."9", "0", "*", "#").
	// Strategy 2: the C64 demo's buttons are unnamed — but the pad is exactly
	// 12 equal-sized button objects in the standard 3x4 phone grid, so sort
	// them row-major and assign the layout by position. (Object 427 carries
	// the name "6" in the demo and lands on '6' this way, confirming the
	// mapping.)
	static const char kDialPadLayout[] = "123456789*0#";
	struct DialButton { int obj; int x; int y; };
	Common::Array<DialButton> grid;
	int gridObjForKey[12] = {};
	int nameObjForKey[12] = {};
	auto layoutIndex = [](char c) -> int {
		for (int k = 0; k < 12; ++k)
			if (kDialPadLayout[k] == c) return k;
		return -1;
	};
	{
		// Dominant button size among the room objects.
		int bestW = 0, bestH = 0, bestCount = 0;
		for (int i = 1; _vm->_objs && i < _vm->_numLocalObjects; ++i) {
			const ObjectData &od = _vm->_objs[i];
			if (!od.obj_nr || od.width <= 0 || od.height <= 0) continue;
			int cnt = 0;
			for (int j = 1; j < _vm->_numLocalObjects; ++j) {
				const ObjectData &o2 = _vm->_objs[j];
				if (o2.obj_nr && o2.width == od.width && o2.height == od.height) ++cnt;
			}
			if (cnt > bestCount) { bestCount = cnt; bestW = od.width; bestH = od.height; }
		}
		for (int i = 1; _vm->_objs && i < _vm->_numLocalObjects; ++i) {
			const ObjectData &od = _vm->_objs[i];
			if (!od.obj_nr) continue;
			// Named buttons map directly regardless of geometry.
			Common::String nm = getObjName(this, od.obj_nr);
			nm.trim();
			if (nm.size() == 1) {
				int k = layoutIndex(nm[0]);
				if (k >= 0 && !nameObjForKey[k]) nameObjForKey[k] = od.obj_nr;
			}
			if (od.width == bestW && od.height == bestH) {
				DialButton b;
				b.obj = od.obj_nr;
				b.x = od.x_pos;
				b.y = od.y_pos;
				grid.push_back(b);
			}
		}
		if (grid.size() == 12) {
			// Row-major sort (top-to-bottom, left-to-right).
			for (uint i = 0; i + 1 < grid.size(); ++i)
				for (uint j = 0; j + 1 < grid.size() - i; ++j)
					if (grid[j].y > grid[j + 1].y ||
					    (grid[j].y == grid[j + 1].y && grid[j].x > grid[j + 1].x)) {
						DialButton t = grid[j]; grid[j] = grid[j + 1]; grid[j + 1] = t;
					}
			for (int k = 0; k < 12; ++k)
				gridObjForKey[k] = grid[(uint)k].obj;
		}
	}

	Common::Array<int> presses;
	for (uint i = 0; i < number.size(); ++i) {
		int k = layoutIndex(number[i]);
		int obj = nameObjForKey[k] ? nameObjForKey[k] : gridObjForKey[k];
		if (!obj) {
			errorOut = Common::String::format(
			    "dial: could not locate the '%c' button — is the dial pad on screen?", number[i]);
			return false;
		}
		presses.push_back(obj);
	}

	// The keypad buttons respond to the push verb: V0's input handler forces
	// _activeVerb = kVerbPush while in keypad mode; for V1/V2 resolve the verb
	// from the verb bar like act() does.
	int pushVerb = kVerbPush;
	if (_vm->_game.version != 0 && !resolveVerb("push", pushVerb)) {
		errorOut = "dial: could not resolve the 'push' verb";
		return false;
	}

	snapshotPreAction();
	_streaming = true;
	_sseAnswerStream = false;
	_sseStartFrame = _frameCounter;
	_sseDoneAtFrame = 0;
	_sseStuckAtFrame = 0;
	_sseLastEventFrame = 0;
	_sseEgoMoved = false;
	_sseMessages.clear();
	_ssePendingSecondClick = false;
	_ssePendingNotes.clear();
	_sseTargetObject = 0;
	_sseButtonClearFrame = 0;
	// Queue after snapshotPreAction (which clears the dial queue); pumpStream
	// feeds one press per spacing window starting next frame.
	_ssePendingDialObjs = presses;
	_sseDialVerbId = pushVerb;
	_sseLastDialFedFrame = 0;
	_server->startStreaming();
	return true;
}

bool ScummMcpBridge::toolKeystroke(const Common::JSONValue &args, Common::String &errorOut) {
	if (!args.isObject()) {
		errorOut = "keystroke: arguments must be an object with a 'key' field";
		return false;
	}
	const Common::JSONObject &a = args.asObject();
	if (!a.contains("key") || !a["key"]->isString()) {
		errorOut = "keystroke: 'key' string is required";
		return false;
	}
	bool ctrl  = a.contains("ctrl")  && a["ctrl"]->isBool()  && a["ctrl"]->asBool();
	bool shift = a.contains("shift") && a["shift"]->isBool() && a["shift"]->asBool();
	bool alt   = a.contains("alt")   && a["alt"]->isBool()   && a["alt"]->asBool();

	Common::KeyState ks;
	if (!jsonKeyToKeyState(a["key"]->asString(), ctrl, shift, alt, ks)) {
		errorOut = "keystroke: unknown key '" + a["key"]->asString() + "'";
		return false;
	}
	_vm->_keyPressed = ks;
	return true;
}

bool ScummMcpBridge::toolMouseMove(const Common::JSONValue &args, Common::String &errorOut) {
	if (!args.isObject()) { errorOut = "mouse_move: arguments must be an object"; return false; }
	const Common::JSONObject &a = args.asObject();
	if (!a.contains("x") || !a["x"]->isIntegerNumber() ||
	    !a.contains("y") || !a["y"]->isIntegerNumber()) {
		errorOut = "mouse_move: integer 'x' and 'y' are required";
		return false;
	}
	int x = (int)a["x"]->asIntegerNumber();
	int y = (int)a["y"]->asIntegerNumber();
	_vm->_mouse.x        = x;
	_vm->_mouse.y        = y;
	_vm->_virtualMouse.x = x;
	_vm->_virtualMouse.y = y;
	if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = x;
	if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = y;
	if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = x;
	if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = y;
	return true;
}

bool ScummMcpBridge::toolMouseClick(const Common::JSONValue &args, Common::String &errorOut) {
	if (!args.isObject()) { errorOut = "mouse_click: arguments must be an object"; return false; }
	const Common::JSONObject &a = args.asObject();
	if (!a.contains("x") || !a["x"]->isIntegerNumber() ||
	    !a.contains("y") || !a["y"]->isIntegerNumber()) {
		errorOut = "mouse_click: integer 'x' and 'y' are required";
		return false;
	}
	int x = (int)a["x"]->asIntegerNumber();
	int y = (int)a["y"]->asIntegerNumber();
	Common::String button = "left";
	if (a.contains("button") && a["button"]->isString()) button = a["button"]->asString();
	bool isDouble = a.contains("double") && a["double"]->isBool() && a["double"]->asBool();

	// Position the mouse first.
	_vm->_mouse.x        = x;
	_vm->_mouse.y        = y;
	_vm->_virtualMouse.x = x;
	_vm->_virtualMouse.y = y;
	if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = x;
	if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = y;
	if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = x;
	if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = y;

	// msClicked = 2, msDown = 1 (both bits set on a real button press).
	const byte kClicked = 2;
	const byte kDown    = 1;
	const byte mask     = kClicked | kDown;

	if (button == "right") {
		_vm->_rightBtnPressed |= mask;
	} else if (button == "middle") {
		// SCUMM doesn't distinguish middle clicks from a Common::Event level for the
		// mouseAndKeyboardStat field; route to left to avoid a no-op.
		_vm->_leftBtnPressed |= mask;
	} else {
		_vm->_leftBtnPressed |= mask;
	}
	// Schedule a button-up so the engine sees a complete click cycle. Without
	// this V7 input scripts treat the held button as a drag.
	_debugButtonReleaseFrame = _frameCounter + 2;

	// For a double click, we tweak _lastInputScriptTime so the engine's 250-500ms
	// delta check inside runInputScript flags this click as the second of a pair.
	if (isDouble) {
		_vm->_lastInputScriptTime = _vm->_system->getMillis();  // mark "first click was just now"
	}
	return true;
}

// ---------------------------------------------------------------------------
// Streaming pump
// ---------------------------------------------------------------------------

void ScummMcpBridge::emitPendingMessages() {
	while (!_messages.empty()) {
		const MessageEntry &m = _messages[0];
		_sseMessages.push_back(m);
		_sseLastEventFrame = _frameCounter;
		Common::JSONObject params;
		if (m.actorId >= 0) {
			int objId = _vm->actorToObj(m.actorId);
			// Only include actor name if the object ID is within bounds
			if (_vm->_numGlobalObjects <= 0 || objId < _vm->_numGlobalObjects) {
				Common::String actorName = getObjName(this, objId);
				if (!actorName.empty()) {
					Common::String safe = safeUtf8(mcpLowerTrimmed(actorName));
					params.setVal("actor", mcpJsonString(safe));
				}
			}
		}
		Common::String cleanText = cleanGameText(safeUtf8(m.text));
		if (!cleanText.empty()) {
			params.setVal("text", mcpJsonString(cleanText));
			params.setVal("type", mcpJsonString(safeUtf8(m.type)));
			_server->emitNotification(params);
		}
		_messages.remove_at(0);
	}
}

void ScummMcpBridge::pumpStream() {
	if (!_streaming) return;

	emitPendingMessages();

	// On the first pump of a new stream, snapshot the Loom note variable so
	// the watcher below only emits transitions occurring during this action.
	if (_frameCounter == _sseStartFrame) {
		_ssePrevNoteValue = (_vm->_scummVars && _vm->_numVariables > 259)
		                    ? _vm->_scummVars[259] : 0;
		_sseLastNoteFedFrame = 0;
	}

	// Loom note watcher: var(259) is set by the engine each time a distaff
	// note is played — both when an object sings (e.g. the egg playing the
	// Opening draft) and when the player presses a note key. Detect 0 -> note
	// transitions and surface them as MCP notifications so the client can
	// learn the songs objects play.
	if (_vm->_scummVars && _vm->_numVariables > 259) {
		int32 cur = _vm->_scummVars[259];
		if (cur != _ssePrevNoteValue) {
			if (cur >= 1 && cur <= 8) {
				static const char *kNoteNames[] = {"c", "d", "e", "f", "g", "a", "b", "C"};
				const char *noteName = kNoteNames[cur - 1];
				MessageEntry m;
				m.seq = _nextMessageSeq++;
				m.frame = _frameCounter;
				m.room = _vm->_currentRoom;
				m.actorId = -1;
				m.type = "note";
				m.text = noteName;
				_sseMessages.push_back(m);
				_sseLastEventFrame = _frameCounter;

				Common::JSONObject params;
				params.setVal("type", mcpJsonString("note"));
				params.setVal("text", mcpJsonString(noteName));
				_server->emitNotification(params);
			}
			_ssePrevNoteValue = cur;
		}
	}

	// Feed deferred synthetic inputs (used by Loom): second click for egg and
	// note sequences for play_note(notes=[...]).
	if (_ssePendingSecondClick) {
		_vm->_mouse.x = _sseClickMouseX;
		_vm->_mouse.y = _sseClickMouseY;
		if (_vm->VAR_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_MOUSE_X) = _sseClickMouseX;
		if (_vm->VAR_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_MOUSE_Y) = _sseClickMouseY;
		_vm->_lastInputScriptTime = _vm->_system->getMillis();
		_vm->_leftBtnPressed |= 0x03; // msClicked | msDown
		_ssePendingSecondClick = false;
	}
	// Feed the next pending note by invoking the engine's verb script
	// directly (kKeyClickArea). Each runInputScript invocation runs Script 97
	// (Loom's input handler) which kills any prior instance — meaning two
	// notes in rapid succession would have the second overwrite the first.
	// Pace feeds fast enough that the script's draft-buffer timeout doesn't
	// fire mid-sequence but slow enough that each note's script completes.
	const uint32 kNoteSpacingFrames = 15;
	if (!_ssePendingNotes.empty()
	    && (_sseLastNoteFedFrame == 0
	        || _frameCounter - _sseLastNoteFedFrame >= kNoteSpacingFrames)) {
		Common::KeyCode kc = _ssePendingNotes[0];
		_ssePendingNotes.remove_at(0);
		_sseLastNoteFedFrame = _frameCounter;
		// kKeyClickArea handler reads the ASCII value for the key. For the
		// distaff note keys (lowercase letters c/d/e/f/g/a/b plus capital C),
		// the keycode value equals the ASCII byte.
		_vm->runInputScript(kKeyClickArea, (int)kc, 1);
	}

	// Maniac Mansion dial pad: press the queued keypad buttons one at a time.
	// Wait for the previous press's sentence to dispatch (the keypad scripts
	// run without walking) and leave a few frames between presses so each
	// button script finishes before the next begins.
	const uint32 kDialSpacingFrames = 12;
	if (!_ssePendingDialObjs.empty() && _vm->_sentenceNum == 0 &&
	    (_sseLastDialFedFrame == 0
	     || _frameCounter - _sseLastDialFedFrame >= kDialSpacingFrames)) {
		int obj = _ssePendingDialObjs[0];
		_ssePendingDialObjs.remove_at(0);
		_sseLastDialFedFrame = _frameCounter;
		_sseLastEventFrame = _frameCounter;
		_vm->doSentence(_sseDialVerbId, obj, 0);
	}

	// Track whether ego moved at any point during this stream.
	{
		Actor *ego = getEgoActor();
		if (ego && ego->_moving)
			_sseEgoMoved = true;
	}

	// Track inventory changes (additions or removals): in CMI long actions
	// (e.g. use gaff on debris) the verb script dispatches a chained animation
	// that updates the inventory many frames after the sentence script ends.
	// Treat any inventory-set transition vs the pre-action snapshot as an event
	// so the settling window keeps extending until the inventory is stable.
	{
		Common::Array<uint16> currentInv;
		int egoForInv = (_vm->VAR_EGO != 0xFF) ? _vm->VAR(_vm->VAR_EGO) : 0;
		for (int i = 0; _vm->_inventory && i < _vm->_numInventory; ++i) {
			uint16 obj = _vm->_inventory[i];
			if (obj && _vm->getOwner(obj) == egoForInv)
				currentInv.push_back(obj);
		}
		auto invSetMatches = [&](const Common::Array<uint16> &a,
		                         const Common::Array<uint16> &b) -> bool {
			if (a.size() != b.size()) return false;
			for (uint i = 0; i < a.size(); ++i) {
				bool found = false;
				for (uint j = 0; j < b.size(); ++j) if (a[i] == b[j]) { found = true; break; }
				if (!found) return false;
			}
			return true;
		};
		if (currentInv.size() != _sseLastInventoryHashCount ||
		    !invSetMatches(currentInv, _sseLastInventorySnapshot)) {
			_sseLastInventoryHashCount = currentInv.size();
			_sseLastInventorySnapshot = currentInv;
			_sseLastEventFrame = _frameCounter;
		}
	}

	// V8 (CMI): use-on-X actions chain follow-up scripts (animation cues +
	// deferred inventory updates) that start in fresh slots after the
	// sentence script ends. Whenever a NEW script appears (count rises since
	// the previous frame), or the count is currently above the pre-action
	// baseline (a chained script is still running), bump the event frame so
	// the settling window keeps extending while the chain is in progress.
	if (_vm->_game.version == 8) {
		int curCount = _vm->activeScriptCount();
		// Only bump on a new script appearing this frame (count went up
		// since the last frame). Stable counts (background music etc.) and
		// counts above the pre-action baseline are not enough — those would
		// keep the stream open indefinitely for plain walk_to actions.
		if (curCount > _sseLastActiveScriptCount)
			_sseLastEventFrame = _frameCounter;
		_sseLastActiveScriptCount = curCount;
	}

	// Early-close: if the room has already changed, there is nothing left to settle —
	// no dialogue will appear in the old room and accessing old-room state is unsafe.
	if ((int)_vm->_currentRoom != _ssePreRoom) {
		debug(1, "mcp: room changed to %d during stream, closing immediately", _vm->_currentRoom);
		Common::JSONObject changes = buildStateChanges();
		_streaming = false;
		_server->endStream(new Common::JSONValue(changes), true);
		return;
	}

	// Early-exit: stuck (no speech, user-put locked).
	// This includes both idle and animated states (e.g., cutscenes with ego moving).
	// Use a short timeout when no events have occurred yet (action had no visible
	// effect and completed quickly), and a longer one when we've seen activity.
	// V8 (CMI) exception: userPut locked with no talkDelay means the dialog script
	// is between lines deciding what to say next — not frozen. isActionDone() already
	// guards this via its own userPut check, so skip stuck-close entirely here.
	{
		bool stuck = _vm->_talkDelay == 0 && _vm->_userPut <= 0;
		if (_vm->_game.version == 8 && _vm->_userPut <= 0)
			stuck = false;
		if (stuck) {
			if (_sseStuckAtFrame == 0) _sseStuckAtFrame = _frameCounter;
			bool hadActivity = _sseLastEventFrame > 0 || _sseEgoMoved;
			uint32 stuckLimit = hadActivity ? 90 : 15;
			if (_frameCounter - _sseStuckAtFrame > stuckLimit) {
				debug(1, "mcp: action stuck for %d frames — closing stream", stuckLimit);
				Common::JSONObject changes = buildStateChanges();
				_streaming = false;
				_server->endStream(new Common::JSONValue(changes), true);
				return;
			}
		} else {
			_sseStuckAtFrame = 0;
		}
	}

	// Hard timeout: 600 frames (~20 s) since the last event (or stream start).
	// For V7 (Dig/FT) and V8 (CMI), anchor to _sseLastEventFrame so that each
	// new dialog line resets the deadline — long exchanges and room-transition
	// cutscenes (e.g. walking out of a scene while characters talk) don't time
	// out between lines. Those games can have cutscenes far longer than two
	// minutes, so the absolute 3600-frame (~120 s) ceiling only guards the
	// older games; for V7/V8 the per-event 600-frame deadline (which still
	// fires 20 s after dialogue genuinely stalls) is the sole safety net.
	{
		uint32 timeoutAnchor = (_vm->_game.version >= 7 && _sseLastEventFrame > 0)
		    ? _sseLastEventFrame : _sseStartFrame;
		bool absoluteTimeout = (_vm->_game.version < 7) && (_frameCounter - _sseStartFrame > 3600);
		if (absoluteTimeout || _frameCounter - timeoutAnchor > 600) {
			debug(1, "mcp: stream timeout (anchor=%u, start=%u, last=%u, now=%u)",
			      timeoutAnchor, _sseStartFrame, _sseLastEventFrame, _frameCounter);
			_streaming = false;
			_server->endStream(nullptr, false, "action timed out");
			return;
		}
	}

	// V7: if the game switched to a dialog input script (VAR_VERB_SCRIPT changed),
	// the action is still in progress — reset the settle window so we wait for the
	// dialog choices to appear rather than closing the stream prematurely.
	if (_vm->_game.version == 7 && _sseVerbScript != 0 &&
	    _vm->VAR_VERB_SCRIPT != 0xFF) {
		int curVerbScript = (int)_vm->VAR(_vm->VAR_VERB_SCRIPT);
		if (curVerbScript != _sseVerbScript) {
			debug(1, "mcp: VAR_VERB_SCRIPT changed %d->%d at frame %d, resetting settle",
			      _sseVerbScript, curVerbScript, _frameCounter);
			_sseVerbScript = curVerbScript;
			_sseVerbScriptChanged = true;
			_sseDoneAtFrame = 0;
		}
	}

	// Clear the simulated mouse-button msDown bit a couple frames after the click
	// so that the dialog input script (V7 script 69) does not see a held button.
	if (_sseButtonClearFrame != 0 && _frameCounter >= _sseButtonClearFrame) {
		_vm->_leftBtnPressed  &= ~0x01; // clear msDown
		_vm->_rightBtnPressed &= ~0x01; // clear msDown (Dig pickup deselect)
		_sseButtonClearFrame = 0;
	}

	// The Dig: picking up a scene object grabs it onto the mouse cursor, turning
	// every subsequent click into "use <item> on X". Each MCP action is
	// discrete, so once a pickup has added a new inventory item we deposit it by
	// simulating the player's right-click — the game's input script puts the
	// held item back into the inventory and restores the default cursor. Fire
	// once per stream, after the item appears and the game accepts input.
	if (_vm->_game.id == GID_DIG && !_sseDigDeselectDone && _vm->_userPut > 0 &&
	    _frameCounter - _sseStartFrame >= 3) {
		int egoForPick = (_vm->VAR_EGO != 0xFF) ? _vm->VAR(_vm->VAR_EGO) : 0;
		bool pickedUp = false;
		for (int i = 0; _vm->_inventory && i < _vm->_numInventory && !pickedUp; ++i) {
			uint16 obj = _vm->_inventory[i];
			if (!obj || _vm->getOwner(obj) != egoForPick) continue;
			bool wasHeldBefore = false;
			for (uint k = 0; k < _ssePreInventory.size(); ++k)
				if (_ssePreInventory[k] == obj) { wasHeldBefore = true; break; }
			if (!wasHeldBefore) pickedUp = true;
		}
		if (pickedUp) {
			debug(1, "mcp: Dig — depositing picked-up item via right-click at frame %d", _frameCounter);
			_vm->_rightBtnPressed |= 0x03; // msClicked | msDown
			_sseButtonClearFrame = _frameCounter + 2;
			_sseDigDeselectDone = true;
			_sseDoneAtFrame = 0; // re-settle so the deselect completes before closing
		}
	}

	// V7: fire the deferred use-item scene click once the engine has had a
	// frame to commit the held-cursor state queued by the inventory click.
	if (_ssePendingV7UseClick && _vm->_userPut > 0 &&
	    _frameCounter - _sseStartFrame >= 2) {
		_vm->_mouse.x        = _ssePendingV7UseMouseX;
		_vm->_mouse.y        = _ssePendingV7UseMouseY;
		_vm->_virtualMouse.x = _ssePendingV7UseObjX;
		_vm->_virtualMouse.y = _ssePendingV7UseObjY;
		if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = _ssePendingV7UseObjX;
		if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = _ssePendingV7UseObjY;
		if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = _ssePendingV7UseMouseX;
		if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = _ssePendingV7UseMouseY;
		_vm->_leftBtnPressed |= 0x03; // msClicked | msDown
		_sseButtonClearFrame = _frameCounter + 2;
		_ssePendingV7UseClick = false;
		_sseDoneAtFrame = 0;
	}

	// V7: feed a deferred dialog-choice click once the game is ready.
	// toolAnswer() stores the choice here. The two V7 games render choices
	// differently, so they are dispatched differently:
	//   * The Dig — horizontal picture icons captured as blast OBJECTS, stored
	//     in ROOM coordinates (objNumber != 0). The dialog input script hit-
	//     tests VAR_VIRT_MOUSE (room space), so we point the virtual mouse at
	//     the icon, the screen mouse at the matching on-screen spot, and press
	//     the left button — exactly like a player clicking the icon.
	//   * Full Throttle — text lines captured as blast TEXT, stored in SCREEN
	//     coordinates (objNumber == 0). Its script reads VAR_MOUSE, so we keep
	//     the original screen-mouse + scene-click dispatch untouched.
	if (_ssePendingV7Choice != 0 && _vm->_userPut > 0 &&
	    _frameCounter - _sseStartFrame >= 3) {
		bool haveChoice = false;
		V7Choice chosen;
		if (!_v7DialogChoices.empty()) {
			Common::Array<V7Choice> sorted = _v7DialogChoices;
			for (uint i = 0; i + 1 < sorted.size(); ++i) {
				for (uint j = 0; j + 1 < sorted.size() - i; ++j) {
					bool swap = (sorted[j].y > sorted[j + 1].y) ||
					            (sorted[j].y == sorted[j + 1].y && sorted[j].x > sorted[j + 1].x);
					if (swap) {
						V7Choice tmp = sorted[j];
						sorted[j] = sorted[j + 1];
						sorted[j + 1] = tmp;
					}
				}
			}
			int idx = CLIP<int>(_ssePendingV7Choice - 1, 0, (int)sorted.size() - 1);
			chosen = sorted[idx];
			haveChoice = true;
		}

		if (haveChoice && chosen.objNumber != 0) {
			// The Dig: room-space icon. Drive the virtual mouse + a real click.
			int roomX = chosen.x;
			int roomY = chosen.y;
			VirtScreen *vs = &_vm->_virtscr[kMainVirtScreen];
			int screenX = CLIP<int>(roomX - vs->xstart, 0, _vm->_screenWidth - 1);
			int screenY = CLIP<int>(roomY - _vm->_screenTop, 0, _vm->_screenHeight - 1);
			debug(1, "mcp: feeding Dig dialog choice %d as left click at room (%d,%d) screen (%d,%d) frame %d",
			      _ssePendingV7Choice, roomX, roomY, screenX, screenY, _frameCounter);
			_vm->_mouse.x = screenX;
			_vm->_mouse.y = screenY;
			_vm->_virtualMouse.x = roomX;
			_vm->_virtualMouse.y = roomY;
			if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = screenX;
			if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = screenY;
			if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = roomX;
			if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = roomY;
			_vm->_leftBtnPressed |= 0x03; // msClicked | msDown — a real left click
			_sseButtonClearFrame = _frameCounter + 2;
			_vm->runInputScript(kSceneClickArea, 0, 1);
		} else {
			// Full Throttle (screen-space text lines) and the no-capture
			// fallback: place the screen mouse on the choice and run the
			// scene-click input script — the original, proven dispatch.
			int screenX = haveChoice ? chosen.x : 160;
			int screenY = haveChoice ? chosen.y : (163 + (_ssePendingV7Choice - 1) * 4);
			debug(1, "mcp: feeding V7 dialog choice %d as scene click at (%d,%d) frame %d",
			      _ssePendingV7Choice, screenX, screenY, _frameCounter);
			_vm->_mouse.x = screenX;
			_vm->_mouse.y = screenY;
			if (_vm->VAR_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_MOUSE_X) = screenX;
			if (_vm->VAR_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_MOUSE_Y) = screenY;
			_vm->runInputScript(kSceneClickArea, 0, 1);
		}
		_ssePendingV7Choice = 0;
		_ssePendingV7UseClick = false;
		// Reset settle window so we capture messages produced by this choice.
		_sseDoneAtFrame = 0;
	}

	// Sam & Max context-cursor click: cycle the verb cursor to _sseSnmCursorTarget
	// over the target (right-clicks), then left-click. Drives talk_to (mouth, 877)
	// and 'use' on no-verb-7 objects like the DeSoto (use/operate, 878).
	if (_sseSnmTalkActor != 0 && _vm->_userPut > 0 && _frameCounter >= _sseSnmTalkNextFrame) {
		int objX = _vm->getObjX(_sseSnmTalkActor);
		int objY = _vm->getObjY(_sseSnmTalkActor);
		// Actors report their foot point; aim a little higher so the click
		// lands on the body. Clicking the exact foot pixel misses small
		// actors (the street kitten) and wandering ones (Max).
		if (_vm->objIsActor(_sseSnmTalkActor))
			objY -= 8;
		VirtScreen *vs = &_vm->_virtscr[kMainVirtScreen];
		int mouseX = CLIP<int>(objX - vs->xstart, 0, _vm->_screenWidth - 1);
		int mouseY = CLIP<int>(objY + vs->topline, 0, _vm->_screenHeight - 1);
		// Keep the (virtual) mouse over the target so the verb cycle applies to it
		// and the eventual click lands on it.
		_vm->_mouse.x = mouseX;
		_vm->_mouse.y = mouseY;
		_vm->_virtualMouse.x = objX;
		_vm->_virtualMouse.y = objY;
		if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = objX;
		if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = objY;
		if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = mouseX;
		if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = mouseY;

		if (!_sseSnmHovered) {
			// Give the engine a frame of hover with the mouse pinned over the
			// target before any click: clicking on the same frame the mouse
			// was warped misses, because the object-under-cursor state has
			// not been refreshed yet.
			_sseSnmHovered = true;
			_sseSnmTalkNextFrame = _frameCounter + 2;
		} else {
			int curVerb = (kSnmCursorVerbVar < _vm->_numVariables && _vm->_scummVars)
			              ? (int)_vm->_scummVars[kSnmCursorVerbVar] : -1;
			debug(1, "mcp: snm cursor check: var=%d, curVerb=%d", _vm->_scummVars[kSnmCursorVerbVar], curVerb);
			bool cursorMatched = (curVerb == _sseSnmCursorTarget);
			// Item-cursor sentinel: stop on any cursor outside the standard
			// rotation (that is the held character/item, e.g. Max-in-hand 889).
			if (_sseSnmCursorTarget == kSnmItemCursorSentinel)
				cursorMatched = (curVerb > 0 && !snmIsStandardCursor(curVerb));
			if (_sseSnmCursorTarget == kSnmVerifyHeldSentinel) {
				// Post-pickup verification: the hand click only takes the
				// actor when it actually landed on them; wandering actors
				// (Max) move out from under the cursor. Re-click until the
				// cursor turns into the held-item cursor, within reason.
				if (curVerb > 0 && !snmIsStandardCursor(curVerb)) {
					debug(1, "mcp: snm pickup verified, held cursor=%d", curVerb);
					_sseSnmTalkActor = 0;
					_sseSnmForcedCursor = 0;
					_sseDoneAtFrame = 0;
				} else if (_sseSnmPendingUseTarget != 0) {
					// Continue the same stream by clicking the second target with
					// the held cursor. Reset the hover delay because the mouse is
					// about to jump from the picked-up actor/item to the target.
					debug(1, "mcp: switching to pending use target %d", _sseSnmPendingUseTarget);
					_sseSnmTalkActor = _sseSnmPendingUseTarget;
					_sseSnmPendingUseTarget = 0;
					_sseSnmCursorTarget = kSnmItemCursorSentinel;
					_sseSnmHovered = false;
					_sseSnmTalkClicks = 0;
					_sseSnmTalkNextFrame = _frameCounter + 2;
				} else if (_sseSnmTalkClicks >= 6) {
					debug(1, "mcp: snm pickup gave up after %d clicks (cursor=%d)",
					      _sseSnmTalkClicks, curVerb);
					_sseSnmTalkActor = 0;
					_sseSnmPendingUseTarget = 0;
					_sseSnmForcedCursor = 0;
					_sseDoneAtFrame = 0;
				} else {
					debug(1, "mcp: snm pickup re-click %d on %d at (%d,%d) cursor=%d",
					      _sseSnmTalkClicks, _sseSnmTalkActor, objX, objY, curVerb);
					_vm->_leftBtnPressed |= 0x03; // msClicked | msDown
					_sseButtonClearFrame = _frameCounter + 2;
					_sseSnmTalkClicks++;
					_sseSnmTalkNextFrame = _frameCounter + 12;
				}
			} else if (cursorMatched && _sseSnmCursorTarget == kSnmPickupCursor) {
				// Hand cursor reached: click the actor, then verify the grab.
				debug(1, "mcp: snm pickup click on %d at (%d,%d)",
				      _sseSnmTalkActor, objX, objY);
				_vm->_leftBtnPressed |= 0x03; // msClicked | msDown
				_sseButtonClearFrame = _frameCounter + 2;
				_sseSnmCursorTarget = kSnmVerifyHeldSentinel;
				_sseSnmTalkClicks = 0;
				_sseSnmTalkNextFrame = _frameCounter + 12;
			} else if (cursorMatched || _sseSnmTalkClicks >= 8) {
				// If we have a pending use target, switch to it now
				if (_sseSnmPendingUseTarget != 0) {
					debug(1, "mcp: switching to pending use target %d", _sseSnmPendingUseTarget);
					_sseSnmTalkActor = _sseSnmPendingUseTarget;
					_sseSnmPendingUseTarget = 0;
					// Reset hover delay since we're switching targets
					_sseSnmHovered = false;
					_sseSnmTalkClicks = 0;
					_sseSnmTalkNextFrame = _frameCounter + 2;
					// Don't click yet, wait for next frame to position mouse correctly
					return;
				}
				
				// Target cursor selected (or give up cycling): left-click to act.
				// The game uses whatever is currently held (e.g. a picked-up actor)
				// on the target under the cursor.
				debug(1, "mcp: snm click target=%d cursor=%d matched=%d at (%d,%d)",
				      _sseSnmTalkActor, curVerb, cursorMatched, objX, objY);
				_vm->_mouse.x = mouseX;
				_vm->_mouse.y = mouseY;
				_vm->_virtualMouse.x = objX;
				_vm->_virtualMouse.y = objY;
				if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = objX;
				if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = objY;
				if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = mouseX;
				if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = mouseY;
				_vm->_leftBtnPressed |= 0x03; // msClicked | msDown
				_sseButtonClearFrame = _frameCounter + 2;
				_sseSnmTalkActor = 0;
				_sseDoneAtFrame = 0; // re-settle so the resulting action can play out
			} else {
				// Right-click to advance the verb cursor, then wait for the cursor
				// manager script to process it before checking again.
				_vm->_rightBtnPressed |= 0x03; // msClicked | msDown
				_sseButtonClearFrame = _frameCounter + 2;
				_sseSnmTalkClicks++;
				_sseSnmTalkNextFrame = _frameCounter + 3;
			}
		}
	}

	bool done = isActionDone();
	if (done) {
		if (_sseDoneAtFrame == 0) {
			_sseDoneAtFrame = _frameCounter;
			debug(1, "mcp: action looks done at frame %d, settling (egoMoved=%d, lastEvent=%d)",
			      _frameCounter, _sseEgoMoved, _sseLastEventFrame);
		}

		// If a new message arrived after we first thought we were done, the action
		// script was still running — reset the window to wait for it to finish.
		if (_sseLastEventFrame > _sseDoneAtFrame) {
			debug(1, "mcp: new event at frame %d after done at %d, extending window",
			      _sseLastEventFrame, _sseDoneAtFrame);
			_sseDoneAtFrame = _sseLastEventFrame;
		}

		// V7: hasPendingQuestion() returns true whenever VAR_VERB_SCRIPT differs
		// from the baseline, even during an answer stream that started in dialog
		// mode. Suppress it for V7 here — dialog detection uses v7DialogReady.
		// V8 (CMI) during an answer stream: the stream should NOT close on the
		// initial hasPendingQuestion (the response is still playing). Once enough
		// frames have passed for the chosen line to be spoken, close as soon as
		// either (a) a new question has appeared (signalling the next dialog turn
		// is ready) or (b) dialog has ended (no more pending question and at
		// least one message captured).
		bool questionReady;
		if (_vm->_game.version == 7) {
			questionReady = false;
		} else if (_vm->_game.version == 8 && _sseAnswerStream) {
			bool hpq = hasPendingQuestion();
			bool waitedEnough = (_frameCounter - _sseStartFrame >= 30);
			bool gotMessage = !_sseMessages.empty();
			// New dialog turn: a question is now pending after we've waited.
			if (hpq && waitedEnough)
				questionReady = true;
			// End of dialog: no question pending and we already got a message.
			else if (!hpq && gotMessage && waitedEnough)
				questionReady = true;
			else
				questionReady = false;
		} else {
			questionReady = hasPendingQuestion();
		}
		uint32 settleFrames = (_vm->_game.version == 0) ? 3 : 10;
		if (_vm->_game.version != 0 && _sseEgoMoved && !questionReady)
			settleFrames = 20;
		// Monkey Island 1: keep the inter-action gap minimal so clients can
		// chain timing-sensitive actions themselves — e.g. the red-herring
		// dock puzzle ("walk on plank" x3, then "pick up herring" before the
		// seagull lands again). Pacing between the bounces is the client's
		// job: each bounce runs a short plank object script, and dispatching
		// the next walk too early interrupts it, so a client should leave a
		// brief pause between plank steps and then grab the fish immediately.
		if (_vm->_game.id == GID_MONKEY_EGA || _vm->_game.id == GID_MONKEY_VGA ||
		    _vm->_game.id == GID_MONKEY)
			settleFrames = 5;
		// The Dig (V7): dialog choices may appear after a brief script delay
		// even when ego hasn't moved (hero was already near the actor).
		// Use a longer settle window so we don't close before the question appears.
		// Full Throttle (also V7) does not need this floor: it detects dialog via a
		// VAR_VERB_SCRIPT change (which resets the settle window) and v7DialogReady,
		// so the extra wait only adds dead time after every action. Keep FT snappy.
		// Scenery exits that run a multi-frame entry sequence (e.g. the wreck, obj
		// 81 -> room 19) are held open by digEnterScriptRunning below while their
		// object script runs, then closed by the room-changed early-close — so the
		// settle floor here stays modest and does not slow down ordinary actions.
		if (_vm->_game.id == GID_DIG && !questionReady)
			settleFrames = MAX(settleFrames, (uint32)45);
		// V8 (CMI): isActionDone() now waits for the sentence/verb script to finish,
		// which is what triggers dialog choices. A 25-frame window is enough for
		// hasPendingQuestion() to detect them — both for act() and answer() streams.
		if (_vm->_game.version == 8 && !questionReady)
			settleFrames = MAX(settleFrames, (uint32)25);

		// For V0 (Maniac Mansion): after ego reaches the target object, runObjectScript
		// is called for the verb. Wait until that object script finishes (isScriptInUse
		// with the target object's ID). Cap at 30 frames after sseDoneAtFrame.
		bool v0ScriptRunning = (_vm->_game.version == 0) &&
		                       (_sseTargetObject != 0) &&
		                       _vm->isScriptInUse(_sseTargetObject) &&
		                       (_frameCounter - _sseDoneAtFrame < 30);

		// The Dig: a clicked exit/scenery object can run a multi-frame entry
		// sequence (walk-in + climb-inside + startScene) that only begins once
		// ego has arrived and stopped — e.g. entering the wreck (obj 81 -> room
		// 19). Keep the stream open while that object script is still in use so
		// the room transition is captured, rather than closing with an empty
		// result the instant ego stops. Capped so an object whose script never
		// resolves (ego cannot reach it) still settles instead of hanging; a real
		// transition trips the room-changed early-close well before the cap. The
		// dialog-ready paths below are independent, so talk_to is unaffected.
		bool digEnterScriptRunning = (_vm->_game.id == GID_DIG) &&
		                             (_sseTargetObject != 0) &&
		                             _vm->isScriptInUse(_sseTargetObject) &&
		                             (_frameCounter - _sseDoneAtFrame < 300);

		// V7: if the verb script changed toward dialog mode (not away from it),
		// AND the ego has stopped moving (reached the target), the dialog choices
		// are now visible on screen — close the stream to expose them.
		// We only trigger this when we ENTERED dialog mode from the normal script
		// (not during an answer stream that started already in dialog mode).
		bool startedInDialogMode = (_sseInitialVerbScript != 0 &&
		                            _baseVerbScript != 0 &&
		                            _sseInitialVerbScript != _baseVerbScript);
		bool egoNowMoving = false;
		{
			Actor *egoA = getEgoActor();
			if (egoA) egoNowMoving = (egoA->_moving != 0);
		}
		bool v7DialogReady = _sseVerbScriptChanged && !startedInDialogMode &&
		                     (_vm->_game.version == 7) && !egoNowMoving;
		// In V7 dialog mode (verb script changed, not started in dialog), suppress
		// settle-based close while ego is still moving — wait for v7DialogReady.
		bool inV7DialogWait = _sseVerbScriptChanged && !startedInDialogMode &&
		                      (_vm->_game.version == 7) && egoNowMoving;
		bool settled = !v0ScriptRunning && !inV7DialogWait && !digEnterScriptRunning &&
		               (_frameCounter - _sseDoneAtFrame >= settleFrames);
		if (questionReady || v7DialogReady || settled) {
			if (v7DialogReady)
				debug(1, "mcp: V7 dialog mode detected at frame %d (verbScript=%d, egoStopped), closing",
				      _frameCounter, _sseVerbScript);

			debug(1, "mcp: closing stream at frame %d (question=%d, settled=%d, settleFrames=%d)",
				_frameCounter, questionReady, settled, settleFrames);
			Common::JSONObject changes = buildStateChanges();
			_streaming = false;
			_server->endStream(new Common::JSONValue(changes), true);
		}
	} else {
		_sseDoneAtFrame = 0;
	}
}

// ---------------------------------------------------------------------------
// Pre-action snapshot + state-change diff
// ---------------------------------------------------------------------------

void ScummMcpBridge::snapshotPreAction() {
	_sseDigDeselectDone = false;
	_ssePendingDialObjs.clear();
	_sseLastDialFedFrame = 0;
	_ssePreRoom = _vm->_currentRoom;
	_ssePreInventory.clear();
	_ssePreInventoryNames.clear();
	_sseLastInventorySnapshot.clear();
	{
		int ego = (_vm->VAR_EGO != 0xFF) ? _vm->VAR(_vm->VAR_EGO) : 0;
		for (int i = 0; _vm->_inventory && i < _vm->_numInventory; ++i) {
			uint16 obj = _vm->_inventory[i];
			if (obj && _vm->getOwner(obj) == ego) {
				_ssePreInventory.push_back(obj);
				_ssePreInventoryNames.push_back(getObjName(this, obj));
				_sseLastInventorySnapshot.push_back(obj);
			}
		}
	}
	_sseLastInventoryHashCount = _sseLastInventorySnapshot.size();
	_ssePreActiveScriptCount = _vm->activeScriptCount();
	_sseLastActiveScriptCount = _ssePreActiveScriptCount;
	Actor *ego = getEgoActor();
	if (ego) {
		_ssePrePosX = ego->getRealPos().x;
		_ssePrePosY = ego->getRealPos().y;
	} else {
		_ssePrePosX = _ssePrePosY = 0;
	}
	_ssePreObjectStates.clear();
	if (!_vm->_objs) return;
	for (int i = 1; i < _vm->_numLocalObjects; ++i) {
		const ObjectData &od = _vm->_objs[i];
		if (!od.obj_nr) continue;
		// Skip objects that are out of bounds for the object space
		if (_vm->_numGlobalObjects > 0 && od.obj_nr >= _vm->_numGlobalObjects) continue;
		ObjStateSnap snap;
		snap.objNr = od.obj_nr;
		snap.state = _vm->getState(od.obj_nr);
		debug(1, "mcp: preSnapshot obj=%d state=%d", snap.objNr, snap.state);
		_ssePreObjectStates.push_back(snap);
	}
}

Common::JSONObject ScummMcpBridge::buildStateChanges() const {
	Common::JSONObject changes;

	Common::JSONArray added;
	int ego = (_vm->VAR_EGO != 0xFF) ? _vm->VAR(_vm->VAR_EGO) : 0;
	for (int i = 0; _vm->_inventory && i < _vm->_numInventory; ++i) {
		uint16 obj = _vm->_inventory[i];
		if (!obj) continue;
		// Skip inventory items that are out of bounds for the object space
		if (_vm->_numGlobalObjects > 0 && obj >= _vm->_numGlobalObjects) continue;
		if (_vm->getOwner(obj) != ego) continue;
		bool wasPresent = false;
		for (uint j = 0; j < _ssePreInventory.size(); ++j)
			if (_ssePreInventory[j] == obj) { wasPresent = true; break; }
		if (!wasPresent) {
			Common::String name = getObjName(this, obj);
			if (name.empty()) name = Common::String::format("obj-%d", obj);
			Common::String cleanName = cleanGameText(safeUtf8(normalizeActionName(name)));
			if (!cleanName.empty()) {
				added.push_back(mcpJsonString(cleanName));
			}
		}
	}
	if (!added.empty())
		changes.setVal("inventory_added", new Common::JSONValue(added));

	Common::JSONArray removed;
	for (uint j = 0; j < _ssePreInventory.size(); ++j) {
		uint16 obj = _ssePreInventory[j];
		if (!obj) continue;
		if (_vm->_numGlobalObjects > 0 && obj >= _vm->_numGlobalObjects) continue;
		if (_vm->getOwner(obj) == ego) continue;
		Common::String name = getObjName(this, obj);
		if (name.empty() && j < _ssePreInventoryNames.size())
			name = _ssePreInventoryNames[j];
		if (name.empty()) name = Common::String::format("obj-%d", obj);
		Common::String cleanName = cleanGameText(safeUtf8(normalizeActionName(name)));
		if (!cleanName.empty()) {
			removed.push_back(mcpJsonString(cleanName));
		}
	}
	if (!removed.empty())
		changes.setVal("inventory_removed", new Common::JSONValue(removed));

	if ((int)_vm->_currentRoom != _ssePreRoom)
		changes.setVal("room_changed", mcpJsonInt(_vm->_currentRoom));

	Actor *ego2 = getEgoActor();
	if (ego2) {
		int cx = ego2->getRealPos().x;
		int cy = ego2->getRealPos().y;
		if (cx != _ssePrePosX || cy != _ssePrePosY) {
			Common::JSONObject pos;
			pos.setVal("x", mcpJsonInt(cx));
			pos.setVal("y", mcpJsonInt(cy));
			changes.setVal("position", new Common::JSONValue(pos));
		}
	}

	Common::JSONArray objChanges;
	for (int i = 1; _vm->_objs && i < _vm->_numLocalObjects; ++i) {
		const ObjectData &od = _vm->_objs[i];
		if (!od.obj_nr) continue;
		// Skip objects that are out of bounds for the object space
		if (_vm->_numGlobalObjects > 0 && od.obj_nr >= _vm->_numGlobalObjects) continue;
		int newState = _vm->getState(od.obj_nr);
		debug(1, "mcp: buildStateChanges obj=%d newState=%d", od.obj_nr, newState);
		int preState = newState;
		for (uint j = 0; j < _ssePreObjectStates.size(); ++j) {
			if (_ssePreObjectStates[j].objNr == od.obj_nr) {
				preState = _ssePreObjectStates[j].state;
				break;
			}
		}
		if (newState == preState) continue;
		Common::String name = getObjName(this, od.obj_nr);
		if (name.empty()) name = Common::String::format("obj-%d", od.obj_nr);
		Common::JSONObject entry;
		entry.setVal("name",      mcpJsonString(safeUtf8(mcpLowerTrimmed(name))));
		entry.setVal("old_state", mcpJsonInt(preState));
		entry.setVal("new_state", mcpJsonInt(newState));
		objChanges.push_back(new Common::JSONValue(entry));
	}
	if (!objChanges.empty())
		changes.setVal("objects_changed", new Common::JSONValue(objChanges));

	{
		Common::JSONArray msgs;
		for (uint i = 0; i < _sseMessages.size(); ++i) {
			const MessageEntry &me = _sseMessages[i];
			Common::String cleanText = cleanGameText(safeUtf8(me.text));
			if (cleanText.empty()) continue;
			Common::JSONObject m;
			m.setVal("text", mcpJsonString(cleanText));
			if (me.actorId > 0) {
				const byte *actorNamePtr = callGetObjOrActorName(me.actorId);
				if (actorNamePtr) {
					Common::String actorName = safeUtf8(mcpLowerTrimmed(Common::String((const char *)actorNamePtr)));
					if (!actorName.empty())
						m.setVal("actor", mcpJsonString(actorName));
				}
			}
			msgs.push_back(new Common::JSONValue(m));
		}
		if (!msgs.empty())
			changes.setVal("messages", new Common::JSONValue(msgs));
	}

	if (hasPendingQuestion()) {
		int choiceCount = 0;
		Common::JSONArray choiceList;
		if (_vm->_game.id == GID_SAMNMAX && !_v7DialogChoices.empty()) {
			Common::Array<V7Choice> sorted = _v7DialogChoices;
			for (uint i = 0; i + 1 < sorted.size(); ++i) {
				for (uint j = 0; j + 1 < sorted.size() - i; ++j) {
					bool swap = (sorted[j].y > sorted[j + 1].y) ||
					            (sorted[j].y == sorted[j + 1].y && sorted[j].x > sorted[j + 1].x);
					if (swap) { V7Choice tmp = sorted[j]; sorted[j] = sorted[j + 1]; sorted[j + 1] = tmp; }
				}
			}
			for (uint i = 0; i < sorted.size(); ++i) {
				Common::JSONObject choice;
				choice.setVal("id",    mcpJsonInt((int)i + 1));
				choice.setVal("label", mcpJsonString(sorted[i].text));
				choiceList.push_back(new Common::JSONValue(choice));
				++choiceCount;
			}
		} else if (_vm->_game.version >= 6) {
			// V6+/V8 dialog choices are represented as non-action verb slots.
			for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
				const VerbSlot &vs = _vm->_verbs[slot];
				if (!vs.verbid || vs.saveid != 0) continue;
				if (_vm->_game.version > 0 && vs.verbid == 1) continue;
				if (isV6ActionVerb(vs.verbid)) continue;
				Common::String label;
				if (const byte *ptr = _vm->getResourceAddress(rtVerb, slot)) {
					byte textBuf[256] = {};
					_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
					label = cleanGameText(safeUtf8(Common::String((const char *)textBuf)));
				}
				bool allowAsChoice = true;
				if (_vm->_game.version >= 7) {
					if (_vm->_game.version == 8) {
						allowAsChoice = (vs.curmode == 1);
					} else {
						allowAsChoice = (vs.curmode == 1) || isSentenceLikeDialogLabel(label) || (vs.key >= '1' && vs.key <= '9');
					}
				} else {
					if (vs.curmode == 0 && (vs.key < '1' || vs.key > '9')) allowAsChoice = false;
					if (vs.curmode != 0 && vs.curmode != 1) allowAsChoice = false;
				}
				if (!allowAsChoice) continue;
				if (label.empty())
					label = Common::String::format("Topic %d", choiceCount + 1);
				Common::JSONObject choice;
				choice.setVal("id",    mcpJsonInt(++choiceCount));
				choice.setVal("label", mcpJsonString(label));
				choiceList.push_back(new Common::JSONValue(choice));
			}
		} else {
			for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
				const VerbSlot &vs = _vm->_verbs[slot];
				if (!vs.verbid || vs.saveid != 0 || (_vm->_game.version > 0 && vs.verbid == 1)) continue;
				// Only accept curmode=0 if slot has numeric key (dialog); otherwise require curmode=1
				if (vs.curmode == 0 && (vs.key < '1' || vs.key > '9')) continue;
				if (vs.curmode != 0 && vs.curmode != 1) continue;
				const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
				if (!ptr) continue;
				byte textBuf[256];
				_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
				if (!textBuf[0]) continue;
				Common::String cleanLabel = cleanGameText(safeUtf8(Common::String((const char *)textBuf)));
				if (cleanLabel.empty()) continue;
				Common::JSONObject choice;
				choice.setVal("id",    mcpJsonInt(++choiceCount));
				choice.setVal("label", mcpJsonString(cleanLabel));
				choiceList.push_back(new Common::JSONValue(choice));
			}
		}
		if (choiceCount > 0) {
			Common::JSONObject question;
			question.setVal("choices", new Common::JSONValue(choiceList));
			changes.setVal("question", new Common::JSONValue(question));
		}
	}

	return changes;
}

// ---------------------------------------------------------------------------
// Game-state helpers
// ---------------------------------------------------------------------------

Actor *ScummMcpBridge::getEgoActor() const {
	if (!_vm || _vm->VAR_EGO == 0xFF) return nullptr;
	int egoNum = _vm->VAR(_vm->VAR_EGO);
	if (!_vm->isValidActor(egoNum)) return nullptr;
	return _vm->derefActor(egoNum, "getEgoActor");
}

bool ScummMcpBridge::isActionDone() const {
	if (_frameCounter - _sseStartFrame < 3) return false;
	if (_ssePendingSecondClick || !_ssePendingNotes.empty()) return false;
	if (!_ssePendingDialObjs.empty()) return false;
	if (_ssePendingV7Choice != 0) return false;
	// Still cycling the Sam & Max verb cursor toward the mouth / opening talk.
	if (_sseSnmTalkActor != 0) return false;
	Actor *ego = getEgoActor();
	// Ego movement check with timeout only for V0 (Maniac Mansion):
	// V0 doesn't lock _userPut, so we need a timeout to prevent indefinite waits.
	// V5+ games handle movement more predictably and don't need this timeout.
	if (_vm->_talkDelay > 0) return false;
	if (_vm->_userPut <= 0) return false;
	if (_vm->_game.version == 0) {
		// V0 (Maniac Mansion): actors use _moving==2 for "arrived", not 0. The
		// reliable completion signal is: sentence dispatched (_sentenceNum==0)
		// AND walk-then-turn-then-act cycle finished (isWalkToObjectDone()).
		// By the time isActionDone() first runs (>= 3 frames in), checkAndRunSentenceScript
		// has already set _walkToObjectState to non-zero, so the initial state
		// (_sentenceNum==0, _walkToObjectState==kWalkToObjectStateDone) is safe.
		ScummEngine_v0 *v0 = static_cast<ScummEngine_v0 *>(_vm);
		if (_vm->_sentenceNum > 0 || !v0->isWalkToObjectDone())
			return false;
	} else {
		if (ego && ego->_moving) return false;
	}
	// V8 (CMI): the sentence script orchestrates walk + verb execution. Wait
	// until the sentence queue is empty so the verb script has dispatched.
	if (_vm->_game.version == 8) {
		if (_vm->_sentenceNum > 0) return false;
		if (_vm->VAR_SENTENCE_SCRIPT != 0xFF) {
			int sentScript = (int)_vm->VAR(_vm->VAR_SENTENCE_SCRIPT);
			if (sentScript > 0 && _vm->isScriptRunning(sentScript)) return false;
		}
	}
	return true;
}

bool ScummMcpBridge::hasPendingQuestion() const {
	if (!_vm || _vm->_userPut <= 0) return false;

	// Loom's distaff renders as ~14 unkeyed text-verb slots with single-char
	// glyph labels. The MI1-style "unkeyed → dialog" heuristic below would
	// otherwise misidentify them as a pending question. The distaff is the
	// permanent verb bar, not a transient dialog, so suppress the check.
	if (isInLoomSection()) return false;

	// V7 (Dig/FT): dialog choices are NOT stored in verb slots. Instead, the
	// dialog input handler script (e.g., script 69 in The Dig) renders choices
	// directly to screen. Detect dialog mode via VAR_VERB_SCRIPT deviating from
	// the normal baseline value recorded at game start.
	if (_vm->_game.version == 7 && _baseVerbScript != 0 &&
	    _vm->VAR_VERB_SCRIPT != 0xFF) {
		int cur = (int)_vm->VAR(_vm->VAR_VERB_SCRIPT);
		if (cur != _baseVerbScript) {
			// Full Throttle reuses the verb-script slot for transient action
			// sequences too — selecting an icon on the verb coin (fist/kick/mouth)
			// briefly swaps VAR_VERB_SCRIPT to a coin/action handler that is NOT a
			// dialog. A real conversation always renders its choice lines to the
			// bottom status area, which onV7BlastTextSnapshot() captures. Require
			// those captured choices so a punch/kick is not mistaken for a dialog.
			// The Dig keeps the simpler heuristic (its action verbs don't touch
			// the verb script, so it never false-positives).
			if (_vm->_game.id == GID_FT)
				return !_v7DialogChoices.empty();
			return true;
		}
	}

	if (_vm->_game.id == GID_SAMNMAX && !_v7DialogChoices.empty())
		return true;

	// V6+ (Sam & Max and later): dialog uses icon verb slots. The game saves the
	// five standard action icon verbs (saveid != 0) and inserts new topic icon slots.
	// Dialog is pending when at least one standard action verb is saved AND at least
	// one non-standard active verb exists (the topic choices).
	// V7 (Dig/FT) uses no permanent verb bar (single-cursor model), so there are
	// no saved action verbs; instead dialog choices appear as sentence-like slots.
	if (_vm->_game.version >= 6) {
		bool hasActiveSavedAction = false;
		bool hasActiveDialog = false;
		int sentenceLikeChoices = 0;
		debug(1, "mcp: hasPendingQuestion numVerbs=%d", _vm->_numVerbs);
		for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
			const VerbSlot &vs = _vm->_verbs[slot];
			if (vs.verbid || vs.key || vs.curmode || vs.saveid)
				debug(1, "mcp: slot=%d verbid=%d saveid=%d curmode=%d key=%d center=%d",
				      slot, vs.verbid, vs.saveid, vs.curmode, vs.key, vs.center);
			if (!vs.verbid) continue;
			if (_vm->_game.version > 0 && vs.verbid == 1) continue;
			if (isV6ActionVerb(vs.verbid)) {
				if (vs.saveid != 0) hasActiveSavedAction = true;
				debug(1, "mcp: hasPendingQuestion slot=%d verbid=%d saveid=%d curmode=%d key=%d [ACTION]",
				      slot, vs.verbid, vs.saveid, vs.curmode, vs.key);
				continue;
			}
			debug(1, "mcp: hasPendingQuestion slot=%d verbid=%d saveid=%d curmode=%d key=%d [non-action]",
			      slot, vs.verbid, vs.saveid, vs.curmode, vs.key);
			if (vs.saveid != 0) continue;
			Common::String label;
			if (const byte *ptr = _vm->getResourceAddress(rtVerb, slot)) {
				byte textBuf[256] = {};
				_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
				label = cleanGameText(safeUtf8(Common::String((const char *)textBuf)));
			}
			debug(1, "mcp: hasPendingQuestion   label='%s'", label.c_str());
			bool allowAsChoice = true;
			if (_vm->_game.version >= 7) {
				if (_vm->_game.version == 8) {
					allowAsChoice = (vs.curmode == 1);
				} else {
					// V7: sentence-like labels or active mode or numbered keys
					allowAsChoice = (vs.curmode == 1) || isSentenceLikeDialogLabel(label) || (vs.key >= '1' && vs.key <= '9');
				}
			} else {
				if (vs.curmode == 0 && (vs.key < '1' || vs.key > '9')) allowAsChoice = false;
				if (vs.curmode != 0 && vs.curmode != 1) allowAsChoice = false;
			}
			if (!allowAsChoice) continue;
			hasActiveDialog = true;
			if (isSentenceLikeDialogLabel(label))
				++sentenceLikeChoices;
		}
		debug(1, "mcp: hasPendingQuestion savedAction=%d activeDialog=%d sentenceLike=%d",
		      hasActiveSavedAction, hasActiveDialog, sentenceLikeChoices);
		if (hasActiveSavedAction && hasActiveDialog)
			return true;
		// V7 (Dig/FT) and V8 (COMI) expose full-sentence dialog topics directly
		// without the SO_SAVE_VERBS choreography used by V6. Accept if at least
		// one sentence-like choice is visible and at least two choices exist overall.
		if ((_vm->_game.version >= 7) && sentenceLikeChoices >= 1 && hasActiveDialog)
			return true;
		// COMI/V8 fallback for dialogs where all choices are sentence-like.
		return sentenceLikeChoices >= 2;
	}

	bool hasKeyed = false, hasUnkeyed = false, hasNumericKeyed = false;
	for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (!vs.verbid || vs.saveid != 0) continue;
		// Skip OBIM verb slots (verbid=1) — they are graphical UI elements, not text
		// choices, but their key=0 would incorrectly set hasUnkeyed=true and prevent
		// Indy4-style numeric-key dialog detection. In V0, verbid=1 is Open, not OBIM.
		if (_vm->_game.version > 0 && vs.verbid == 1) continue;
		// Only accept curmode=0 if slot has numeric key (dialog); otherwise require curmode=1
		if (vs.curmode == 0 && (vs.key < '1' || vs.key > '9')) continue;
		if (vs.curmode != 0 && vs.curmode != 1) continue;
		const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
		if (!ptr) continue;
		byte textBuf[256];
		_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
		if (!textBuf[0]) continue;
		if (vs.key) {
			hasKeyed = true;
			// Indy4/FOA assigns numeric keys '1'-'9' to dialog choices
			if (vs.key >= '1' && vs.key <= '9') hasNumericKeyed = true;
		} else {
			hasUnkeyed = true;
		}
	}
	// MI1-style: dialog choices are unkeyed, normal verb bar is keyed (or absent).
	// Skip this for Maniac Mansion, which has unkeyed verbs in normal gameplay.
	if (hasUnkeyed && !hasKeyed && _vm->_game.id != GID_MANIAC) return true;
	// Indy4-style: dialog choices have numeric keys; normal verb bar is saved (saveid!=0)
	if (hasNumericKeyed && !hasUnkeyed) return true;
	return false;
}

// ---------------------------------------------------------------------------
// Name resolution
// ---------------------------------------------------------------------------

// SCUMM pads object names to a fixed width with trailing '@' bytes (the
// charset renderer draws '@' as nothing), e.g. the EGA demo's
// "roter Hering@@@@@...". Strip the padding (and any spaces it uncovers) so
// MCP clients never see it.
static Common::String mcpStripNamePadding(const Common::String &s) {
	Common::String out(s);
	while (!out.empty() &&
	       (out[out.size() - 1] == '@' || out[out.size() - 1] == ' '))
		out.deleteLastChar();
	return out;
}

Common::String ScummMcpBridge::safeUtf8(const Common::String &raw) const {
	if (raw.empty()) return raw;
	// Normalize whitespace after the code-page decode: several game code pages
	// (e.g. CP-850's 0xFF) decode filler bytes to U+00A0, which would otherwise
	// leak into emitted text and break label round-trips with MCP clients.
	// Trailing '@' name padding is stripped for the same reason.
	Common::String utf8;
	if (!_vm)
		utf8 = mcpSanitizeString(raw);
	else {
		Common::CodePage cp = _vm->getDialogCodePage();
		utf8 = (cp == Common::kUtf8) ? mcpSanitizeString(raw)
		                             : Common::U32String(raw, cp).encode(Common::kUtf8);
	}
	return mcpStripNamePadding(mcpNormalizeSpaces(utf8));
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
	// The Dig: single-cursor verbs map to the generic 'use' action (verb ID 7).
	if (s == "interact") return "use";
	if (s == "use_item") return "use";
	return s;
}

// Map of verb canonical names for looking up by label
static const struct {
	const char *canonical;
	const char *label;
} kVerbMap[] = {
	{"talk_to", "Talk to"},
	{"talk_to", "talk_to"},
	{"look_at", "Look at"},
	{"look_at", "look_at"},
	{"look_at", "What is"},		// Maniac Mansion C64
	{"pick_up", "Pick up"},
	{"pick_up", "pick_up"},
	{"walk_to", "Walk to"},
	{"walk_to", "walk_to"},
	{"open", "Open"},
	{"open", "open"},
	{"close", "Close"},
	{"close", "close"},
	{"use", "Use"},
	{"use", "use"},
	{"unlock", "Unlock"},
	{"unlock", "unlock"},
	{"give", "Give"},
	{"give", "give"},
	{"push", "Push"},
	{"push", "push"},
	{"pull", "Pull"},
	{"pull", "pull"},
	{nullptr, nullptr}
};

// Check if a verb bar label matches the canonical action
static bool verbLabelMatches(const Common::String &rawLabel, const Common::String &canonicalAction) {
	for (int i = 0; kVerbMap[i].canonical; ++i) {
		if (rawLabel == kVerbMap[i].label && canonicalAction == kVerbMap[i].canonical)
			return true;
	}
	// Fallback: normalize the raw label and compare directly
	Common::String normLabel = ScummMcpBridge::normalizeActionName(mcpLowerTrimmed(rawLabel));
	return normLabel == canonicalAction;
}

// ---------------------------------------------------------------------------
// Selectability helpers
// ---------------------------------------------------------------------------

// Mirrors findObject(): returns false for objects the player cannot click on.
bool ScummMcpBridge::isObjectSelectable(const ObjectData &od) const {
	// The untouchable class flag is the primary gate — it applies to all games.
	if (_vm->getClass(od.obj_nr, kObjectClassUntouchable))
		return false;

	// V0 foreground objects and all V1/V2 objects also honor the untouchable
	// state flag (mirrors the version branch in findObject(); V3+ has no
	// state-flag veto).
	if ((_vm->_game.version == 0 && OBJECT_V0_TYPE(od.obj_nr) == kObjectV0TypeFG) ||
	    (_vm->_game.version > 0 && _vm->_game.version <= 2)) {
		if (od.state & kObjectStateUntouchable)
			return false;
	}

	// Hidden-object gate: findObject() walks the parent chain and only lets a
	// click land while every ancestor's (state & mask) equals the child's
	// parentstate. E.g. in Indy3's Henry's house the old book is parented to
	// the chest with parentstate=open and the chest itself to the table cloth,
	// so neither is reachable until its parent reaches the revealing state.
	// `parent` is a local-object *index*, not an object number.
	if (_vm->_objs) {
		const int mask = (_vm->_game.version <= 2) ? kObjectStateIntrinsic : 0xF;
		int b = _vm->getObjectIndex(od.obj_nr);
		// Hop counter guards against a malformed cyclic parent chain.
		for (int hops = 0; b > 0 && hops < _vm->_numLocalObjects; ++hops) {
			byte a = _vm->_objs[b].parentstate;
			b = _vm->_objs[b].parent;
			if (b == 0)
				break;
			if (b >= _vm->_numLocalObjects || (_vm->_objs[b].state & mask) != a)
				return false;
		}
	}
	return true;
}

// Mirrors getActorFromPos(): returns false for actors the player cannot target.
bool ScummMcpBridge::isActorSelectable(int actorId) const {
	// The untouchable class flag is the only selectability gate for actors across
	// all three supported games; getActorFromPos() applies the same test.
	switch (_vm->_game.id) {
	case GID_MANIAC:
	case GID_MONKEY_EGA:
	case GID_INDY4:
		return !_vm->getClass(actorId, kObjectClassUntouchable);
	default:
		return !_vm->getClass(actorId, kObjectClassUntouchable);
	}
}

void ScummMcpBridge::buildEntityMap(Common::Array<NamedEntity> &entities) const {
	entities.clear();

	struct RawEntry {
		NamedEntity::Kind kind;
		int numId;
		Common::String baseName;
		bool visible = false;
		bool isPathway = false;
	};
	Common::Array<RawEntry> raw;

	int ego = (_vm->VAR_EGO != 0xFF) ? _vm->VAR(_vm->VAR_EGO) : 0;
	for (int i = 0; _vm->_inventory && i < _vm->_numInventory; ++i) {
		int obj = _vm->_inventory[i];
		if (!obj || _vm->getOwner(obj) != ego) continue;
		Common::String name = getObjName(this, obj);
		if (name.empty()) continue;
		RawEntry e;
		e.kind = NamedEntity::kInventory;
		e.numId = obj;
		e.baseName = normalizeActionName(safeUtf8(name));
		raw.push_back(e);
	}

	for (int i = 1; _vm->_objs && i < _vm->_numLocalObjects; ++i) {
		const ObjectData &od = _vm->_objs[i];
		if (!od.obj_nr) continue;
		// Exclude objects the player cannot interact with (mirrors findObject()).
		if (!isObjectSelectable(od)) continue;
		Common::String name = getObjName(this, od.obj_nr);
		// In the Passport-to-Adventure Loom segment, scene pathways/exits have
		// no OBNA name (they're invisible hotspots authored without a label).
		// Surface them under a stable, action-friendly name so the MCP client
		// can target them by name instead of by hardcoded id. We restrict this
		// to PASS+Loom; other games keep their existing placeholder behavior.
		bool isLoomPassPathway = false;
		if (name.empty() && _vm->_game.id == GID_PASS && isInLoomSection() &&
		    (od.x_pos != 0 || od.y_pos != 0) && od.width > 0 && od.height > 0) {
			isLoomPassPathway = true;
		}
		RawEntry e;
		e.kind = NamedEntity::kObject;
		e.numId = od.obj_nr;
		if (isLoomPassPathway) {
			e.baseName = Common::String::format("pathway_%d", od.obj_nr);
		} else {
			e.baseName = name.empty() ? Common::String::format("obj_%d", od.obj_nr)
			                          : normalizeActionName(safeUtf8(name));
		}
		// Visibility mask: v0-v2 use only the intrinsic (on/off) bit; v3+ use the full
		// lower nibble which encodes pickupable, untouchable, locked, and intrinsic.
		const int mask = (_vm->_game.version <= 2)
		    ? kObjectStateIntrinsic
		    : (kObjectStatePickupable | kObjectStateUntouchable | kObjectStateLocked | kObjectStateIntrinsic);
		e.visible = (od.state & mask) != 0;
		if (e.visible && od.parent != 0 && od.parent < _vm->_numLocalObjects)
			e.visible = ((_vm->_objs[od.parent].state & mask) == od.parentstate);
		// Pathway objects are invisible exits with only a walk_to handler.
		// They are kept in the list so the agent can navigate, but flagged separately.
		if (!e.visible) {
			bool hasWalkTo = false, hasOther = false;
			if (_vm->_game.version >= 6) {
				// V6+ verbs are image-based: identify walk_to by verbid (13) directly.
				for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
					const VerbSlot &vs = _vm->_verbs[slot];
					if (!vs.verbid || vs.saveid != 0) continue;
					if (_vm->getVerbEntrypoint(e.numId, vs.verbid) == 0) continue;
					if (vs.verbid == 13) hasWalkTo = true;
					else hasOther = true;
				}
			} else {
				for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
					const VerbSlot &vs = _vm->_verbs[slot];
					if (!vs.verbid || vs.saveid != 0) continue;
					if (_vm->getVerbEntrypoint(e.numId, vs.verbid) == 0) continue;
					const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
					if (!ptr) continue;
					byte textBuf[256];
					_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
					Common::String label = normalizeActionName((const char *)textBuf);
					if (label == "walk_to") hasWalkTo = true;
					else hasOther = true;
				}
			}
			if (hasWalkTo && !hasOther) e.isPathway = true;
		}
		// CMI (V8): exit hotspots have no action handlers (ep 6/7/8 all 0) — mark as pathway.
		if (!e.isPathway && _vm->_game.id == GID_CMI &&
		    _vm->getVerbEntrypoint(e.numId, 6) == 0 &&
		    _vm->getVerbEntrypoint(e.numId, 7) == 0 &&
		    _vm->getVerbEntrypoint(e.numId, 8) == 0) {
			e.isPathway = true;
		}
		if (!name.empty()) {
			bool hasCtrl = false;
			for (uint ci = 0; ci < e.baseName.size(); ++ci)
				if ((unsigned char)e.baseName[ci] < 0x20) { hasCtrl = true; break; }
			if (hasCtrl) continue;
		}
		// Sam & Max: drop dormant placeholder objects that have an empty bounding
		// box (zero width or height) and are not navigable pathways. findObject()
		// requires width+x_pos > x (and likewise for height), so a 0-sized box has
		// no on-screen hit area — the player can never click it. The street scene's
		// 'carnival_tickets' (id 95) sits at 0,0 with w=h=0 until the script later
		// places it; without this guard it surfaces as a phantom, pickable target.
		// Scoped to Sam & Max to preserve other games' object lists verbatim.
		if (_vm->_game.id == GID_SAMNMAX && !e.isPathway &&
		    (od.width == 0 || od.height == 0))
			continue;
		raw.push_back(e);
	}

	int egoNum = (_vm->VAR_EGO != 0xFF) ? _vm->VAR(_vm->VAR_EGO) : -1;
	for (int i = 1; _vm->_actors && i < _vm->_numActors; ++i) {
		Actor *a = _vm->_actors[i];
		if (!a) continue;
		// Only include actors that are properly placed in the current room.
		// In V4+ (Monkey Island, Atlantis), actors are visual representations only;
		// room objects carry the verb scripts. Requiring _room == currentRoom prevents
		// including actors placed by scripts at off-screen positions (e.g. (0,0)).
		if (!a->isInCurrentRoom()) continue;
		// Ego is the player character; it is never presented as an interaction target.
		if (a->_number == egoNum) continue;
		// Exclude actors the player cannot click on (mirrors getActorFromPos()).
		if (!isActorSelectable(a->_number)) continue;
		int objId = _vm->actorToObj(a->_number);
		// Even if objId is out of bounds, include the actor so it can be targeted
		// (the verb handler will handle whether the verb is available)
		Common::String name;
		if (_vm->_numGlobalObjects <= 0 || objId < _vm->_numGlobalObjects) {
			name = getObjName(this, objId);
		}
		// Sam & Max: actor names may not be available via actorToObj(), but can be
		// resolved directly from actor id (e.g. Max).
		if (name.empty() && _vm->_game.id == GID_SAMNMAX) {
			name = getObjName(this, a->_number);
		}
		RawEntry e;
		e.kind = NamedEntity::kActor;
		e.numId = a->_number;
		e.visible = a->_visible;
		if (name.empty() && _vm->_game.id == GID_SAMNMAX && a->_number == 3)
			name = "Max";
		e.baseName = name.empty() ? normalizeActionName(Common::String::format("actor-%d", a->_number))
		                          : normalizeActionName(safeUtf8(name));
		if (!name.empty()) {
			bool hasCtrl = false;
			for (uint ci = 0; ci < e.baseName.size(); ++ci)
				if ((unsigned char)e.baseName[ci] < 0x20) { hasCtrl = true; break; }
			if (hasCtrl) continue;
		}
		raw.push_back(e);
	}

	for (uint i = 0; i < raw.size(); ++i) {
		NamedEntity ne;
		ne.kind        = raw[i].kind;
		ne.numId       = raw[i].numId;
		ne.displayName = raw[i].baseName;
		ne.visible     = raw[i].visible;
		ne.isPathway   = raw[i].isPathway;
		entities.push_back(ne);
	}
}

bool ScummMcpBridge::snmIsMaxEntity(int obj) const {
	if (_vm->_game.id != GID_SAMNMAX || obj == 0)
		return false;
	if (obj == 3) // Max's actor id
		return true;
	Common::String n = normalizeActionName(safeUtf8(getObjName(this, obj)));
	return n == "max" || n == "max_the_object";
}

bool ScummMcpBridge::resolveEntityByName(const Common::String &name, NamedEntity &out) const {
	Common::String normalized = normalizeActionName(name);
	Common::Array<NamedEntity> entities;
	buildEntityMap(entities);
	debug(1, "mcp: resolveEntityByName '%s' (normalized='%s'), %u entities in map",
	      name.c_str(), normalized.c_str(), (uint)entities.size());
	for (uint i = 0; i < entities.size(); ++i) {
		debug(1, "mcp:   entity[%u] kind=%d id=%d name='%s' visible=%d",
		      i, (int)entities[i].kind, entities[i].numId,
		      entities[i].displayName.c_str(), entities[i].visible);
	}
	// When an actor and a room object share a name:
	// V0 (Maniac Mansion): actors carry the verb scripts → prefer actor over object.
	// V4+ (Monkey Island, Atlantis): room objects carry verb scripts; actors are
	//   visual only → prefer room object over actor so verbs execute correctly.
	int actorMatch = -1;
	int objectMatch = -1;
	int firstMatch = -1;
	for (uint i = 0; i < entities.size(); ++i) {
		if (entities[i].displayName != normalized) continue;
		if (firstMatch < 0) firstMatch = (int)i;
		if (entities[i].kind == NamedEntity::kActor) {
			if (actorMatch < 0) actorMatch = (int)i;
		} else {
			if (objectMatch < 0) objectMatch = (int)i;
		}
	}
	bool v0Game = (_vm->_game.version == 0);
	// Sam & Max: when user asks for "max", prefer the inventory item "max_the_object"
	// if it exists, otherwise fall back to the actor. This allows "use max on Y" to work
	// correctly while still supporting other actions that target Max the actor.
	if (_vm->_game.id == GID_SAMNMAX && normalized == "max") {
		// Look for the inventory item "max_the_object" first
		for (uint i = 0; i < entities.size(); ++i) {
			if (entities[i].kind == NamedEntity::kInventory &&
			    entities[i].displayName == "max_the_object") {
				out = entities[i];
				return true;
			}
		}
		// If no inventory item, fall back to actor
		if (actorMatch >= 0) {
			out = entities[(uint)actorMatch];
			return true;
		}
		// Some Sam & Max scenes don't expose actor names. Fall back to Max actor id (3).
		for (uint i = 0; i < entities.size(); ++i) {
			if (entities[i].kind == NamedEntity::kActor && entities[i].numId == 3) {
				out = entities[i];
				return true;
			}
		}
	}
	int best = v0Game
	    ? ((actorMatch  >= 0) ? actorMatch  : (objectMatch >= 0) ? objectMatch : firstMatch)
	    : ((objectMatch >= 0) ? objectMatch : (actorMatch  >= 0) ? actorMatch  : firstMatch);
	if (best >= 0) {
		// For V4+: if the best match is an actor and there is an untouchable room object
		// with the same name, prefer the room object — it carries the verb entrypoints
		// while the actor is a visual-only representation.
		if (!v0Game && entities[best].kind == NamedEntity::kActor && _vm->_objs) {
			for (int i = 1; i < _vm->_numLocalObjects; ++i) {
				const ObjectData &od = _vm->_objs[i];
				if (!od.obj_nr) continue;
				if (_vm->_numGlobalObjects > 0 && od.obj_nr >= _vm->_numGlobalObjects) continue;
				Common::String objName = getObjName(this, od.obj_nr);
				if (objName.empty()) continue;
				if (normalizeActionName(safeUtf8(objName)) == normalized) {
					out.kind        = NamedEntity::kObject;
					out.numId       = od.obj_nr;
					out.displayName = normalized;
					out.visible     = false;
					debug(1, "mcp: resolveEntityByName '%s' redirected from actor to room obj %d",
					      normalized.c_str(), od.obj_nr);
					return true;
				}
			}
		}
		out = entities[best]; return true;
	}
	debug(1, "mcp: resolveEntityByName '%s' not found", name.c_str());
	return false;
}

bool ScummMcpBridge::resolveVerb(const Common::String &action, int &verbId) const {
	Common::String normalized = normalizeActionName(action);
	debug(1, "mcp: resolveVerb '%s' (normalized='%s')", action.c_str(), normalized.c_str());
	for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (!vs.verbid) continue;
		const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
		byte textBuf[256] = {};
		if (ptr) _vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
		Common::String rawLabel = safeUtf8(Common::String((const char *)textBuf));
		Common::String normLabel = normalizeActionName(rawLabel);
		debug(1, "mcp:   slot=%d verbid=%d saveid=%d curmode=%d key=%d label='%s'",
		      slot, vs.verbid, vs.saveid, vs.curmode, vs.key, rawLabel.c_str());
		if (vs.saveid != 0) continue;
		if (!ptr) continue;
		if (rawLabel.empty()) continue;
		// Match label against verb variants
		if (!verbLabelMatches(rawLabel, normalized)) continue;

		// For talk_to, accept the verb bar match even without entrypoints; dialog
		// may not use the verb entrypoint system.
		if (normalized == "talk_to") {
			verbId = vs.verbid;
			debug(1, "mcp: resolveVerb found verbid=%d via label match (talk_to)", verbId);
			return true;
		}
		// For other verbs, verify the verb has an actual entrypoint; skip if not
		// (the verb bar text might be reused or mislabeled).
		bool hasEntrypoint = false;
		for (int oi = 1; _vm->_objs && oi < _vm->_numLocalObjects; ++oi) {
			if (!_vm->_objs[oi].obj_nr) continue;
			if (_vm->getVerbEntrypoint(_vm->_objs[oi].obj_nr, vs.verbid) != 0) {
				hasEntrypoint = true;
				break;
			}
		}
		if (!hasEntrypoint) {
			for (int ii = 0; _vm->_inventory && ii < _vm->_numInventory; ++ii) {
				int obj = _vm->_inventory[ii];
				if (!obj) continue;
				if (_vm->getVerbEntrypoint(obj, vs.verbid) != 0) {
					hasEntrypoint = true;
					break;
				}
			}
		}
		if (hasEntrypoint) {
			verbId = vs.verbid;
			debug(1, "mcp: resolveVerb found verbid=%d via label match", verbId);
			return true;
		}
	}
	// The Dig and Full Throttle (V7) use a single-cursor click / pie-menu model.
	// Map interact / use_item to a click sentinel and let toolAct dispatch via a
	// simulated scene click — the doSentence path used for V6 does not reliably
	// trigger the per-object scripts for V7 games.
	if ((_vm->_game.id == GID_DIG || _vm->_game.id == GID_FT) && normalized == "use") {
		verbId = -1;
		debug(1, "mcp: resolveVerb V7 interact/use_item -> click dispatch sentinel");
		return true;
	}

	// Full Throttle verb-coin verbs: fist (grab/punch/use), kick (boot), mouth
	// (talk/look) map to the per-object verb scripts 9/5/8; the generic 'interact'
	// uses the click-dispatch sentinel (-1), resolved to the object's best
	// available coin verb in toolAct().
	if (_vm->_game.id == GID_FT) {
		if (normalized == "fist")     { verbId = 9;  return true; }
		if (normalized == "kick")     { verbId = 5;  return true; }
		if (normalized == "mouth")    { verbId = 8;  return true; }
		if (normalized == "walk_to")  { verbId = -3; return true; }
		if (normalized == "interact") { verbId = -1; return true; }
	}

	// Full Throttle verb-coin actions dispatched via doSentence (objects carry
	// real per-verb entrypoints, unlike The Dig). Debug helper: "v_N"/"verb_N"
	// dispatches an arbitrary verb id for empirical mapping.
	if (_vm->_game.id == GID_FT && _debugToolsEnabled &&
	    (normalized.hasPrefix("v_") || normalized.hasPrefix("verb_"))) {
		const char *p = normalized.c_str() + (normalized.hasPrefix("v_") ? 2 : 5);
		int id = atoi(p);
		if (id > 0) {
			verbId = id;
			return true;
		}
	}

	// Sam & Max: talk_to has no verb id — it is the "mouth" cursor reached by
	// cycling the right-click verb. Map it to a sentinel that toolAct dispatches
	// via the verb-cycle-then-click state machine in pumpStream.
	if (_vm->_game.id == GID_SAMNMAX && normalized == "talk_to") {
		verbId = kSnmTalkSentinel;
		return true;
	}

	// Sam & Max icon-verb ids differ from the common V6 layout used by Day of the
	// Tentacle / Monkey 2 (where verb 4 == pick_up and verb 5 == look_at). In Sam
	// & Max the eye (look at) is verb 4 and the hand (pick up) is verb 5 — the
	// reverse — verified empirically: doSentence(4, roach_farm) speaks the look
	// description ("It's Max's roach farm.") while doSentence(5, ...) gives the
	// pick-up refusal ("I can't pick that up."). Map these explicitly before the
	// generic V6 canonical lookup below, which would otherwise match {4:pick_up,
	// 5:look_at} first and swap the two actions. 'use' (verb 7, e.g. the office
	// door's ep7 exit handler) and 'walk_to' (verb 13) keep the common ids.
	if (_vm->_game.id == GID_SAMNMAX) {
		static const struct { const char *name; int id; } kSnmVerbs[] = {
			{"look_at", 4}, {"pick_up", 5}, {"use", 7}, {"walk_to", 13}, {nullptr, 0}
		};
		for (int i = 0; kSnmVerbs[i].name; ++i) {
			if (normalized == kSnmVerbs[i].name) {
				verbId = kSnmVerbs[i].id;
				debug(1, "mcp: resolveVerb S&M '%s' -> verbid=%d", normalized.c_str(), verbId);
				return true;
			}
		}
	}

	// Sam & Max debug helper: accept "v_N"/"verb_N" to dispatch arbitrary
	// verb IDs while mapping the icon interface.
	if (_vm->_game.id == GID_SAMNMAX && _debugToolsEnabled &&
	    (normalized.hasPrefix("v_") || normalized.hasPrefix("verb_"))) {
		const char *p = normalized.c_str() + (normalized.hasPrefix("v_") ? 2 : 5);
		int id = atoi(p);
		if (id > 0) {
			verbId = id;
			return true;
		}
	}

	// Curse of Monkey Island (V8) verb IDs differ from V6. Empirically determined:
	//   verb 6 -> look_at  (e.g. "Nice cannon balls.")
	//   verb 7 -> pick_up  (e.g. "They're too heavy to carry.")
	//   verb 8 -> talk_to  (opens dialog wheel)
	//   verb 13 -> walk_to (default cursor action)
	// Must be checked before the V6+ canonical lookup which would otherwise map
	// look_at to verb 5 (the V6 canonical id, which is wrong for V8).
	if (_vm->_game.id == GID_CMI) {
		// Debug helper (gated by mcp_debug): accept "v_N"/"verb_N" to dispatch
		// arbitrary verb IDs for testing.
		if (_debugToolsEnabled && (normalized.hasPrefix("v_") || normalized.hasPrefix("verb_"))) {
			const char *p = normalized.c_str() + (normalized.hasPrefix("v_") ? 2 : 5);
			int id = atoi(p);
			if (id > 0) {
				verbId = id;
				return true;
			}
		}
		static const struct { const char *name; int id; } kCMIVerbs[] = {
			{"walk_to", 13}, {"look_at", 6}, {"pick_up", 7},
			{"talk_to",  8}, {"use",     7}, {nullptr,   0}
		};
		for (int ci = 0; kCMIVerbs[ci].name; ++ci) {
			if (normalized == kCMIVerbs[ci].name) {
				verbId = kCMIVerbs[ci].id;
				debug(1, "mcp: resolveVerb CMI '%s' -> verbid=%d", normalized.c_str(), verbId);
				return true;
			}
		}
	}

	// V6+: standard action verbs are image-based. Resolve by verbid directly.
	if (_vm->_game.version >= 6) {
		const V6VerbEntry *entry = nullptr;
		for (int i = 0; kV6CanonicalVerbs[i].name; ++i) {
			if (normalized == kV6CanonicalVerbs[i].name) { entry = &kV6CanonicalVerbs[i]; break; }
		}
		if (entry) {
			// Verify the verb slot is currently active (action icon bar is visible).
			for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
				const VerbSlot &vs = _vm->_verbs[slot];
				if (vs.verbid == entry->id && vs.saveid == 0 && vs.curmode != 0) {
					verbId = entry->id;
					debug(1, "mcp: resolveVerb V6 direct verbid=%d for '%s'", verbId, normalized.c_str());
					return true;
				}
			}
			// Slot not active (e.g. dialog in progress), but verb is still a known V6 verb.
			// Accept it unconditionally so the caller can still dispatch the action.
			verbId = entry->id;
			debug(1, "mcp: resolveVerb V6 verbid=%d (slot not active) for '%s'", verbId, normalized.c_str());
			return true;
		}
	}

	// V5 and below: if no text label matched for walk_to, take the first active verb slot.
	// Skipped for V6+ where walk_to is always verbid 13 and handled by kFallback below.
	if (normalized == "walk_to" && _vm->_game.version < 6) {
		for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
			const VerbSlot &vs = _vm->_verbs[slot];
			if (vs.verbid && vs.saveid == 0 && vs.curmode == 1) {
				verbId = vs.verbid;
				return true;
			}
		}
	}

	// Indy3's verb bar carries global verbs ("Travel") that are not bound to any
	// per-object verb script — clicking them runs the verb-click input script
	// directly. Accept the label match without requiring an entrypoint. The
	// caller (toolAct) dispatches verb-only calls via runInputScript.
	if (_vm->_game.id == GID_INDY3 || _vm->_game.id == GID_PASS) {
		for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
			const VerbSlot &vs = _vm->_verbs[slot];
			if (!vs.verbid || vs.saveid != 0) continue;
			if (vs.curmode != 1) continue;
			const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
			if (!ptr) continue;
			byte textBuf[256] = {};
			_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
			Common::String label = mcpLowerTrimmed(safeUtf8((const char *)textBuf));
			if (label.empty()) continue;
			if (normalizeActionName(label) == normalized) {
				verbId = vs.verbid;
				debug(1, "mcp: resolveVerb Indy3 global verb '%s' -> verbid=%d",
				      normalized.c_str(), verbId);
				return true;
			}
		}
	}

	// Loom uses a single-cursor click model. To preserve the genuine click
	// pipeline (walk + arrival callback scripts such as listen/replay on egg,
	// leaf fall, etc.), map interact/use_item to a Loom-specific sentinel and
	// dispatch via simulated mouse click in toolAct() instead of doSentence().
	if (isInLoomSection() && normalized == "use") {
		verbId = -1;
		debug(1, "mcp: resolveVerb Loom interact -> click dispatch sentinel");
		return true;
	}

	// Fallback for v6/v7/v8 games that use right-click context menus: the verb
	// bar is ephemeral so _verbs has no text labels during normal gameplay.
	// Try a canonical name→ID table, accepting an ID only if at least one room
	// object or inventory item actually has a script handler for it.
	static const struct { const char *name; int id; } kFallback[] = {
		{"open",    1}, {"close",   2}, {"give",    3},
		{"pick_up", 4}, {"look_at", 5}, {"talk_to", 6},
		{"use",     7}, {"push",    8}, {"pull",    9},
		{"walk_to", 13}, {nullptr,  0}
	};
	for (int fi = 0; kFallback[fi].name; ++fi) {
		if (normalized != kFallback[fi].name) continue;
		int cid = kFallback[fi].id;
		for (int oi = 1; _vm->_objs && oi < _vm->_numLocalObjects; ++oi) {
			if (!_vm->_objs[oi].obj_nr) continue;
			if (_vm->getVerbEntrypoint(_vm->_objs[oi].obj_nr, cid) != 0) {
				verbId = cid;
				return true;
			}
		}
		int ego = (_vm->VAR_EGO != 0xFF) ? _vm->VAR(_vm->VAR_EGO) : 0;
		for (int ii = 0; _vm->_inventory && ii < _vm->_numInventory; ++ii) {
			int obj = _vm->_inventory[ii];
			if (!obj || _vm->getOwner(obj) != ego) continue;
			if (_vm->getVerbEntrypoint(obj, cid) != 0) {
				verbId = cid;
				return true;
			}
		}
	}

	return false;
}

} // End of namespace Scumm
