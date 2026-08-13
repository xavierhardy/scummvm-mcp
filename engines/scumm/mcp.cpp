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
#include "scumm/mcp_subclasses.h"
#include "scumm/object.h"
#include "scumm/scumm.h"
#include "scumm/verbs.h"
#include "scumm/boxes.h"
#ifdef ENABLE_SCUMM_7_8
#include "scumm/scumm_v7.h"
#include "scumm/insane/insane.h"
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
	return bridge->decodeObjectName(name);
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
	: MCP::McpBridge(vm),
	  _vm(vm),
	  _sseEgoMoved(false),
	  _sseAllowLongCutscene(false),
	  _sseTargetObject(0),
	  _ssePendingSecondClick(false),
	  _sseClickMouseX(0),
	  _sseClickMouseY(0),
	  _ssePrevNoteValue(0),
	  _sseLastNoteFedFrame(0) {
}

ScummMcpBridge *ScummMcpBridge::create(ScummEngine *vm) {
	ScummMcpBridge *bridge = nullptr;
	if (!vm) {
		bridge = new ScummMcpBridge(vm);
	} else {
		switch (vm->_game.id) {
		case GID_MANIAC:     bridge = new McpBridgeManiac(vm);      break;
		case GID_LOOM:       bridge = new McpBridgeLoom(vm);        break;
		case GID_INDY3:      bridge = new McpBridgeIndy3(vm);       break;
		case GID_INDY4:      bridge = new McpBridgeIndy4(vm);       break;
		case GID_MONKEY_EGA:
		case GID_MONKEY_VGA:
		case GID_MONKEY:     bridge = new McpBridgeMonkey(vm);      break;
		case GID_MONKEY2:    bridge = new McpBridgeMonkey2(vm);     break;
		case GID_PASS:       bridge = new McpBridgePassport(vm);    break;
		case GID_SAMNMAX:    bridge = new McpBridgeSamnMax(vm);     break;
		case GID_TENTACLE:   bridge = new McpBridgeTentacle(vm);    break;
		case GID_DIG:        bridge = new McpBridgeDig(vm);         break;
		case GID_FT:         bridge = new McpBridgeFullThrottle(vm); break;
		case GID_CMI:        bridge = new McpBridgeComi(vm);        break;
		default:
			// Unsupported game: fall back to the bridge for its SCUMM version.
			switch (vm->_game.version) {
			case 0:
			case 1:
			case 2:  bridge = new McpBridgeV0(vm);      break;
			case 6:  bridge = new McpBridgeV6(vm);      break;
			case 7:  bridge = new McpBridgeV7(vm);      break;
			case 8:  bridge = new McpBridgeV8(vm);      break;
			default: bridge = new McpBridgeClassic(vm); break; // V3–V5
			}
			break;
		}
	}
	bridge->init();
	return bridge;
}

// ---------------------------------------------------------------------------
// Protected accessors for ScummEngine's protected internals (see mcp.h). The
// base bridge is the sole friend of ScummEngine; these wrappers let subclasses
// reach the members they need without each being befriended.
// ---------------------------------------------------------------------------

int8 ScummMcpBridge::vmUserPut() const                     { return _vm->_userPut; }
Actor *ScummMcpBridge::vmActor(int i) const                { return _vm->_actors[i]; }
Actor *ScummMcpBridge::vmActorOrNull(int i) const          { return (_vm->_actors && i >= 0 && i < _vm->_numActors) ? _vm->_actors[i] : nullptr; }
int ScummMcpBridge::vmNumActors() const                    { return _vm->_numActors; }
int ScummMcpBridge::vmNumVariables() const                 { return _vm->_numVariables; }
int ScummMcpBridge::vmNumVerbs() const                     { return _vm->_numVerbs; }
int32 ScummMcpBridge::vmVar(int i) const                   { return _vm->_scummVars ? _vm->_scummVars[i] : 0; }
void ScummMcpBridge::vmConvertMessageToString(const byte *msg, byte *dst, int dstSize) const { _vm->convertMessageToString(msg, dst, dstSize); }
int ScummMcpBridge::vmGetOwner(int obj) const              { return _vm->getOwner(obj); }
int ScummMcpBridge::vmGetState(int obj) const              { return _vm->getState(obj); }
int ScummMcpBridge::vmGetObjX(int obj) const               { return _vm->getObjX(obj); }
int ScummMcpBridge::vmGetObjY(int obj) const               { return _vm->getObjY(obj); }
int ScummMcpBridge::vmGetObjectIndex(int obj) const        { return _vm->getObjectIndex(obj); }
int ScummMcpBridge::vmGetVerbEntrypoint(int obj, int entry) const { return _vm->getVerbEntrypoint(obj, entry); }
int ScummMcpBridge::vmActorToObj(int actor) const          { return _vm->actorToObj(actor); }
int ScummMcpBridge::vmNumLocalObjects() const              { return _vm->_numLocalObjects; }
uint16 *ScummMcpBridge::vmInventory() const                { return _vm->_inventory; }
int ScummMcpBridge::vmNumInventory() const                 { return _vm->_numInventory; }
int ScummMcpBridge::vmActiveScriptCount() const            { return _vm->activeScriptCount(); }
void ScummMcpBridge::vmDoSentence(int verb, int objA, int objB) const { _vm->doSentence(verb, objA, objB); }
void ScummMcpBridge::vmRunInputScript(int clickArea, int val, int mode) const { _vm->runInputScript(clickArea, val, mode); }
void ScummMcpBridge::vmResetSentence() const               { _vm->resetSentence(); }
void ScummMcpBridge::vmActorFollowCamera(int actor) const  { _vm->actorFollowCamera(actor); }
bool ScummMcpBridge::v0InNormalMode() const  { return static_cast<ScummEngine_v0 *>(_vm)->_currentMode == ScummEngine_v0::kModeNormal; }
bool ScummMcpBridge::v0InKeypadMode() const  { return static_cast<ScummEngine_v0 *>(_vm)->_currentMode == ScummEngine_v0::kModeKeypad; }
void ScummMcpBridge::v0SwitchActor(int slot) const { static_cast<ScummEngine_v0 *>(_vm)->switchActor(slot); }

Common::String ScummMcpBridge::objName(int obj) const {
	const byte *name = _vm->getObjOrActorName(obj);
	if (!name || !*name)
		return "";
	return decodeObjectName(name);
}

Common::String ScummMcpBridge::decodeObjectName(const byte *raw) const {
	return Common::String((const char *)raw);
}
Common::Point &ScummMcpBridge::vmMouse() const             { return _vm->_mouse; }
Common::Point &ScummMcpBridge::vmVirtualMouse() const      { return _vm->_virtualMouse; }
uint32 &ScummMcpBridge::vmLastInputScriptTime() const      { return _vm->_lastInputScriptTime; }
byte &ScummMcpBridge::vmLeftBtnPressed() const             { return _vm->_leftBtnPressed; }
byte &ScummMcpBridge::vmRightBtnPressed() const            { return _vm->_rightBtnPressed; }

ScummMcpBridge::~ScummMcpBridge() {
}

// ---------------------------------------------------------------------------
// Game-loop hook. Called by MCP::McpBridge::pump() after the frame counter has
// advanced and before the server is serviced.
// ---------------------------------------------------------------------------

void ScummMcpBridge::pumpGame() {
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
	// Full Throttle: the bar runs a different gameplay verb script than the
	// dumpster where the baseline was captured, so the verb_script == baseline
	// clear above never fires there. A leftover choice captured from a one-shot
	// cutscene line (the keys sequence's last bartender line renders in the
	// bottom status band) would then keep hasPendingQuestion() true forever and
	// trap the player in the bar. A live conversation re-enqueues its choices
	// every frame, so expire any capture that has not been refreshed recently.
	const uint32 kV7ChoiceStaleFrames = 30;
	if (_vm && _vm->_game.id == GID_FT && !_v7DialogChoices.empty() &&
	    _frameCounter - _v7DialogChoicesFrame > kV7ChoiceStaleFrames) {
		_v7DialogChoices.clear();
	}
	if (_vm && _vm->_game.id == GID_SAMNMAX && !hasPendingQuestion() && !_v7DialogChoices.empty())
		_v7DialogChoices.clear();
}

// ---------------------------------------------------------------------------
// Message capture from engine
// ---------------------------------------------------------------------------

int ScummMcpBridge::currentRoomForMessages() const {
	return _vm ? _vm->_currentRoom : 0;
}

// V6 (Sam & Max) and V7 (The Dig / Full Throttle): spoken lines arrive straight
// from _charsetBuffer (actor.cpp and the V7 pumpGame() both feed onActorLine),
// prefixed by the original interpreter's 0xFF-coded talkie/sound blocks. Older
// text engines pass through untouched.
bool ScummMcpBridge::stripTalkieMetadata() const {
	return _vm && _vm->_game.version >= 6;
}

Common::String ScummMcpBridge::messageActorName(int actorId) const {
	int objId = _vm->actorToObj(actorId);
	// Only include the actor name if the object ID is within bounds
	if (_vm->_numGlobalObjects > 0 && objId >= _vm->_numGlobalObjects)
		return Common::String();
	return getObjName(const_cast<ScummMcpBridge *>(this), objId);
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
	// choices if we cleared on every frame. Stamp the refresh frame so pump()
	// can expire a capture that stops being re-rendered (see the staleness
	// clear there).
	if (!fresh.empty()) {
		_v7DialogChoices = fresh;
		_v7DialogChoicesFrame = _frameCounter;
	}
}

const byte *ScummMcpBridge::callGetObjOrActorName(int obj) const {
	return _vm ? _vm->getObjOrActorName(obj) : nullptr;
}

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

void ScummMcpBridge::augmentChangesSchema(Common::JSONObject &props) {
	(void)props;
}

Common::String ScummMcpBridge::stateToolDescription() const {
	return "Returns the current game state: the room, where the player character "
	       "stands, what is in the room, what is carried, the verbs on the verb "
	       "bar, the lines said since the last read (cleared by reading them) and "
	       "any dialog question waiting to be answered. The player character "
	       "itself is not listed among the room's objects. Each object lists the "
	       "verbs it responds to in compatible_verbs — start there rather than "
	       "guessing — and one with a meaningful state also carries a readable "
	       "state_name ('opened'/'closed' for a door, say). Characters are listed "
	       "like objects and are spoken to with act(verb='talk_to', "
	       "target1=<name>). Exits carry pathway=true.";
}

Common::String ScummMcpBridge::debugToolDescription() const {
	return "Return the raw state the game itself keeps, for diagnostics: the "
	       "current room, where the player character is, what the mouse and "
	       "keyboard look like to the game, whether it is taking input, and a "
	       "slice of its script variables (0..127 by default; pass 'from' and "
	       "'to' to widen).";
}

Common::JSONValue *ScummMcpBridge::buildDebugSchema() const {
	Common::JSONObject props;
	props.setVal("from", mcpProp("integer",
	    "First SCUMM var index to return (default 0)."));
	props.setVal("to", mcpProp("integer",
	    "Last SCUMM var index to return (inclusive, default 127)."));
	return mcpObjectSchema(props);
}

void ScummMcpBridge::augmentStateSchema(Common::JSONObject &outputProps) {
	outputProps.setVal("can_act", mcpProp("boolean",
	    "False while the game is not taking input — an intro, a cutscene, a "
	    "scripted sequence. act() and walk() are rejected until it turns true. "
	    "Skip or wait; nothing else will work."));
}

void ScummMcpBridge::registerDebugTools() {
	// set_talk_speed — force the text/talk speed at runtime
	Networking::McpServer::ToolSpec spec;
	spec.name = "set_talk_speed";
	spec.description =
	    "Set how fast text is shown, the same way the game's own speech-speed "
	    "control does, on a 0..255 scale (0 = slowest, 255 = instant). Some "
	    "games overwrite the configured speed when they start, leaving text "
	    "crawling; this sets it for the rest of the session.";
	Common::JSONObject props;
	props.setVal("speed", mcpProp("integer",
	    "Talk speed on the 0..255 scale (0 = slowest, 255 = fastest)."));
	const char *req[] = {"speed"};
	spec.inputSchema  = mcpObjectSchema(props, req, 1);
	Common::JSONObject outProps;
	outProps.setVal("talkspeed",  mcpProp("integer", "The speed that was set (0..255)."));
	outProps.setVal("text_speed", mcpProp("integer", "The same speed on the game's own scale."));
	outProps.setVal("charinc",    mcpProp("integer",
	    "The per-character delay the game now uses, when it has one."));
	spec.outputSchema = mcpObjectSchema(outProps);
	spec.streaming    = false;
	_server->registerTool(spec);
}

// ---------------------------------------------------------------------------
// Tool dispatch
// ---------------------------------------------------------------------------

Common::JSONValue *ScummMcpBridge::callTool(const Common::String &name,
                                             const Common::JSONValue &args,
                                             Common::String &errorOut) {
	// set_talk_speed is the one SCUMM-only tool. Handling it here rather than in
	// dispatchGameTool() keeps it working for the game leaves, which override
	// dispatchGameTool() for their own tools.
	if (name == "set_talk_speed")
		return toolSetTalkSpeed(args, errorOut);
	return MCP::McpBridge::callTool(name, args, errorOut);
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
		pos.setVal("x", mcpJsonInt(toRoomPixelX(ego->getRealPos().x)));
		pos.setVal("y", mcpJsonInt(toRoomPixelY(ego->getRealPos().y)));
		out.setVal("position", new Common::JSONValue(pos));
	}

	// Game-specific top-level state fields (e.g. Maniac Mansion's switchable kids).
	augmentState(out);

	// Check for pending dialog question before building the verb bar.
	// When a question is pending, the verb bar is replaced by dialog choices
	// (in V4/MI1, the same verb slots are reused with new text; in V5/Indy4,
	// new slots are added). Either way, we emit an empty verbs list and put
	// the choices into 'question' instead.
	bool questionPending = hasPendingQuestion();

	Common::Array<VerbInfo> activeVerbs;
	Common::JSONArray verbsArr;
	if (!questionPending) {
		for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
			const VerbSlot &vs = _vm->_verbs[slot];
			// Verb id 1 is the engine's reserved default/sentence verb in most
			// SCUMM games and is hidden from the exposed verb bar. A few games use
			// id 1 for a real bar verb instead (Monkey Island's "Open"), so the
			// skip is gated behind a per-game hook.
			if (!vs.verbid || vs.saveid != 0 ||
			    (_vm->_game.version > 0 && vs.verbid == 1 && !includeBarVerbId1())) continue;
			// The sentence line is the game echoing the pending command back at
			// the player ("Walk to swamp"), not a verb to pick.
			if (isSentenceLineSlot(vs)) continue;
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
	if (!usesTextVerbBar() && !questionPending) {
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

	// Game-specific verb-list overrides (CMI's fixed verb set, V7 single-cursor
	// fallbacks, Sam & Max's icon verbs, …).
	applyGameVerbs(verbsArr, activeVerbs, questionPending);

	out.setVal("verbs", new Common::JSONValue(verbsArr));

	Common::Array<NamedEntity> entities;
	buildEntityMap(entities);

	// On a click-only screen (MI2's island maps, its swamp coffin) nothing
	// scripts a verb: the one action is a click, which applyGameVerbs published
	// under the game's own name. Offer it on every object, or state would show a
	// screenful of things with nothing that can be done to them.
	const bool clickOnly = usesClickOnlyScreens() && isClickOnlyScreen();

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
			// Skip objects out of bounds for the object-detail helpers below
			// (getObjX/getState/getVerbEntrypoint assert id <= 255). V0 encodes
			// the object type in the high byte, so its background scene objects
			// (clock 269, gargoyles 342/344, ...) exceed _numGlobalObjects (256)
			// and are intentionally not listed here. They remain targetable via
			// `act` by name or id (toolAct uses a wider V0 ceiling).
			if (_vm->_numGlobalObjects > 0 && ne.numId >= _vm->_numGlobalObjects) break;

			// Find the actual verb bar labels and check if verbs exist. look_at is
			// only offered as a fallback when *something* in the scene scripts it:
			// act() resolves a verb through the same entrypoint test, so a verb no
			// object responds to would be advertised and then refused. The
			// exception is the V0-V2 "What is", which act() answers from the
			// object's name (lookAtAnswersWithName) and so always works.
			Common::String lookAtLabel, walkToLabel;
			bool lookAtExists = false, walkToExists = false;
			for (uint k = 0; k < activeVerbs.size(); ++k) {
				if (activeVerbs[k].name == "look_at" &&
				    (lookAtAnswersWithName() || verbHasAnyEntrypoint(activeVerbs[k].verbId))) {
					lookAtLabel = activeVerbs[k].label; lookAtExists = true;
				}
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
			} else if (_vm->_game.id == GID_DIG || _vm->_game.id == GID_CMI ||
			           isInLoomSection() || clickOnly) {
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
			obj.setVal("x",                mcpJsonInt(toRoomPixelX(_vm->getObjX(ne.numId))));
			obj.setVal("y",                mcpJsonInt(toRoomPixelY(_vm->getObjY(ne.numId))));
			obj.setVal("pathway",          mcpJsonBool(isPathway));
			Common::String stateName = objectStateName(ne.numId, _vm->getState(ne.numId), isPathway);
			if (!stateName.empty())
				obj.setVal("state_name", mcpJsonString(stateName));
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
			if (_vm->_game.id == GID_DIG || _vm->_game.id == GID_FT || _vm->_game.id == GID_CMI ||
			    isInLoomSection() || clickOnly) {
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
			actorObj.setVal("x",                mcpJsonInt(toRoomPixelX(_vm->getObjX(actorObjId))));
			actorObj.setVal("y",                mcpJsonInt(toRoomPixelY(_vm->getObjY(actorObjId))));
			actorObj.setVal("pathway",          mcpJsonBool(false));
			actorObj.setVal("compatible_verbs", new Common::JSONValue(compatVerbs));
			objects.push_back(new Common::JSONValue(actorObj));
			break;
		}
		}
	}
	// Game-specific synthetic scene objects (e.g. CMI cannon boat_N targets).
	augmentStateObjects(objects);

	out.setVal("inventory", new Common::JSONValue(inventory));
	out.setVal("objects",   new Common::JSONValue(objects));

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

	// The same flag act() and walk() enforce. Without it in the snapshot, a
	// client cannot tell a cutscene from a room where nothing has a name yet,
	// and every other engine's bridge already says so.
	out.setVal("can_act", mcpJsonBool(_vm->_userPut > 0));

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
		} else if (!usesTextVerbBar()) {
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
				// Never offer the sentence line as something to answer with.
				if (isSentenceLineSlot(vs)) continue;
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

	// Game-specific whole-act interception before verb/target resolution (e.g.
	// Fate of Atlantis turning a "page_N" of the Lost Dialogue book).
	bool interceptHandled = false;
	bool interceptOk = interceptGameAct(a, errorOut, interceptHandled);
	if (interceptHandled)
		return interceptOk;

	int verbId = -1;
	if (!resolveVerb(verbStr, verbId)) {
		// Distinguish "no such verb" from "the verb is on the bar, but nothing
		// in this room scripts it" — an agent echoing back state.verbs deserves
		// to be told which of the two it hit.
		if (verbOnBar(normalizeActionName(verbStr)))
			errorOut = "act: verb '" + verbStr +
			           "' is on the verb bar but nothing in this room responds to it";
		else
			errorOut = "act: unknown verb '" + verbStr + "'";
		return false;
	}

	// V0 (Maniac Mansion C64/Apple II) encodes an object's type in the high byte
	// of its number (OBJECT_V0(id,type) = type<<8 | id), so legitimate object
	// numbers run up to (kObjectV0TypeActor<<8 | 0xFF) = 0x2FF, well past
	// _numGlobalObjects (256). Use a wider ceiling for V0 so name-resolved scene
	// objects like Maniac's staircase gargoyles (e.g. 342) are not rejected.
	const int objIdLimit = (_vm->_game.version == 0) ? 0x300 : _vm->_numGlobalObjects;

	auto resolveTarget = [&](const char *param, int &out) -> bool {
		if (!a.contains(param)) return true;
		const Common::JSONValue *v = a[param];
		if (v->isIntegerNumber()) {
			out = (int)v->asIntegerNumber();
			if (out < 0) {
				errorOut = Common::String::format("act: %s id %d is negative", param, out);
				return false;
			}
			if (objIdLimit > 0 && out >= objIdLimit) {
				errorOut = Common::String::format(
					"act: %s id %d out of bounds (0-%d)",
					param, out, objIdLimit - 1);
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
			if (objIdLimit > 0 && out >= objIdLimit) {
				errorOut = Common::String::format(
					"act: %s id %d out of bounds (0-%d)",
					param, out, objIdLimit - 1);
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

	// "What is" (V0-V2): the engine answers it in the sentence line and runs no
	// sentence at all, so return the name the game would print there. Running it
	// as a sentence instead walks ego across the room to be told "That doesn't
	// seem to work", which is not the game's answer to the question.
	if (targetA != 0 && targetB == 0 && lookAtAnswersWithName() &&
	    normalizeActionName(verbStr) == "look_at")
		return beginNameAnswerStream(verbId, targetA);

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
		// "Walk to" is the one verb the engine — not a script — implements: a
		// click on it just walks ego to the object's walk point, which is why
		// state advertises it for every object. Do exactly that when the object
		// has no walk-to script, instead of refusing the sentence.
		if (ep == 0 && targetB == 0 && normalizeActionName(verbStr) == "walk_to") {
			int ox = 0, oy = 0, odir = 0;
			_vm->getObjectXYPos(targetA, ox, oy, odir);
			debug(1, "mcp: act walk_to obj %d without handler -> walking to (%d,%d)",
			      targetA, ox, oy);
			if (beginWalkStream(ox, oy, odir))
				return true;
			errorOut = "act: no ego actor available";
			return false;
		}
		if (ep == 0) {
			debug(1, "mcp: skipping verb with no entrypoint on object %d", targetA);
			errorOut = "verb has no handler for this object";
			// "unlock key with front door" is the usual mix-up: target1 is what
			// the verb acts on, target2 the item it is used with. Say so when the
			// swapped order is the one the game scripts.
			if (targetB != 0 && _vm->getVerbEntrypoint(targetB, verbId) != 0)
				errorOut += " — target1 is what the verb acts on, target2 the item "
				            "used with it; try them the other way round";
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
	} else if (dispatchGameAct(verbId, targetA, targetB)) {
		// Handled by the game-specific leaf (e.g. CMI use-on / walk-to exits).
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
	if (!usesTextVerbBar()) {
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
			// Count only displayed choices (curmode==1) with a non-empty label, so
			// the 1-based index matches exactly what toolState lists in
			// state.question. Hidden placeholder slots (curmode==0 numeric keys with
			// blank labels, e.g. Fate of Atlantis's captain menu) must be skipped
			// here too, or answer(N) would select the choice before the intended one.
			if (vs.curmode != 1) continue;
			const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
			if (!ptr) continue;
			byte textBuf[256];
			_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
			if (!textBuf[0]) continue;
			Common::String cleanLabel = cleanGameText(safeUtf8(Common::String((const char *)textBuf)));
			if (cleanLabel.empty()) continue;
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
	// Game-specific dialog-choice dispatch (e.g. CMI clicks the on-screen choice
	// line); otherwise run the default verb-click input script.
	if (!dispatchGameAnswer(vs.curRect, vs.verbid))
		_vm->runInputScript(kVerbClickArea, vs.verbid, 1);
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
	// The tool takes room pixels; V0-V2 walk on the compressed actor grid.
	wx = fromRoomPixelX(wx);
	wy = fromRoomPixelY(wy);

	// Click-only screens (MI2's island maps and its swamp coffin) move ego from
	// the input script, not from startWalkActor: rowing the coffin to a spot is
	// a click on that spot. Replay the click instead.
	if (usesClickOnlyScreens() && isClickOnlyScreen())
		return beginSceneClick(wx, wy);

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

void ScummMcpBridge::beginBareStream() {
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
}

bool ScummMcpBridge::beginWalkStream(int gx, int gy, int dir) {
	Actor *ego = getEgoActor();
	if (!ego)
		return false;
	beginBareStream();
	ego->startWalkActor(gx, gy, dir);
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
// Debug tools: 'debug', 'keystroke', 'mouse_move', 'mouse_click'
// ---------------------------------------------------------------------------

Common::JSONValue *ScummMcpBridge::toolSaveState(const Common::JSONValue &args, Common::String &errorOut) {
	if (!args.isObject() || !args.asObject().contains("slot") ||
	    !args.asObject()["slot"]->isIntegerNumber()) {
		errorOut = "save_state: missing integer 'slot'";
		return nullptr;
	}
	int slot = (int)args.asObject()["slot"]->asIntegerNumber();
	if (slot < 0) {
		errorOut = "save_state: 'slot' must be >= 0";
		return nullptr;
	}
	Common::String desc = "mcp save";
	if (args.asObject().contains("description") && args.asObject()["description"]->isString())
		desc = args.asObject()["description"]->asString();

	Common::JSONObject out;
	out.setVal("slot", mcpJsonInt(slot));

	// Refuse when the engine itself would refuse (e.g. mid-cutscene, save menu
	// script running, or HE games without scripted save support).
	if (!_vm->canSaveGameStateCurrently(nullptr)) {
		out.setVal("saved", mcpJsonBool(false));
		out.setVal("reason", mcpJsonString("game cannot be saved in the current state"));
		return new Common::JSONValue(out);
	}

	// Defer to the engine's own save path. requestSave() sets _saveLoadFlag,
	// which this same scummLoop() iteration (pump() runs at its top) processes
	// later in the frame, writing <target>.s<NN> under the active save path —
	// exactly what the in-game save menu produces.
	_vm->requestSave(slot, desc);
	debug(1, "mcp: save_state requested slot %d ('%s')", slot, desc.c_str());
	out.setVal("saved", mcpJsonBool(true));
	out.setVal("description", mcpJsonString(desc));
	return new Common::JSONValue(out);
}

Common::JSONValue *ScummMcpBridge::toolSetTalkSpeed(const Common::JSONValue &args, Common::String &errorOut) {
	if (!args.isObject() || !args.asObject().contains("speed") ||
	    !args.asObject()["speed"]->isIntegerNumber()) {
		errorOut = "set_talk_speed: missing integer 'speed'";
		return nullptr;
	}
	int speed = (int)args.asObject()["speed"]->asIntegerNumber();
	if (speed < 0)   speed = 0;
	if (speed > 255) speed = 255;

	// Persist on the 0..255 ConfMan scale (identical to --talkspeed) so any
	// later syncSoundSettings() keeps using it, then push it into the live
	// engine the same way the in-game speech-speed control does. This is what
	// makes the value stick on titles whose boot script clobbers the configured
	// talkspeed outside room 0 (e.g. the Fate of Atlantis demo), where the
	// VAR_CHARINC user-override in setVar() is skipped because _currentRoom != 0.
	ConfMan.setInt("talkspeed", speed);
	int ts = _vm->getTalkSpeed();              // 0..9, higher == faster text
	_vm->_defaultTextSpeed = ts;
	if (_vm->VAR_CHARINC != 0xFF)
		_vm->VAR(_vm->VAR_CHARINC) = 9 - ts;

	// Latch the value so a later intro/cutscene script can't override it (the
	// FoA demo sets VAR_CHARINC outside room 0, where the writeVar() user
	// override would otherwise be skipped). See ScummEngine::writeVar.
	_vm->_mcpForceTalkSpeed = true;

	debug(1, "mcp: set_talk_speed talkspeed=%d (text_speed=%d, charinc=%d)",
	      speed, ts, 9 - ts);

	Common::JSONObject out;
	out.setVal("talkspeed", mcpJsonInt(speed));
	out.setVal("text_speed", mcpJsonInt(ts));
	if (_vm->VAR_CHARINC != 0xFF)
		out.setVal("charinc", mcpJsonInt((int)_vm->VAR(_vm->VAR_CHARINC)));
	return new Common::JSONValue(out);
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

	// Debug-only: warp the virtual mouse to (mouse_x, mouse_y) without clicking.
	// Lets a caller probe how the engine maps cursor motion to on-screen aim
	// (e.g. the CMI cannon's velocity-style crosshair) by moving and re-reading.
	if (args.isObject() && args.asObject().contains("mouse_x") &&
	    args.asObject()["mouse_x"]->isIntegerNumber() &&
	    args.asObject().contains("mouse_y") &&
	    args.asObject()["mouse_y"]->isIntegerNumber()) {
		int mx = (int)args.asObject()["mouse_x"]->asIntegerNumber();
		int my = (int)args.asObject()["mouse_y"]->asIntegerNumber();
		_vm->_mouse.x        = mx;
		_vm->_mouse.y        = my;
		_vm->_virtualMouse.x = mx;
		_vm->_virtualMouse.y = my;
		if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = mx;
		if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = my;
		if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = mx;
		if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = my;
	}

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

	// Talk/text speed, so callers can verify set_talk_speed took effect:
	// 'talkspeed' is the 0..255 ConfMan value, 'text_speed' the engine's 0..9
	// scale (9 == fastest), and 'charinc' the live per-char delay (0 == instant).
	out.setVal("talkspeed",  mcpJsonInt(ConfMan.getInt("talkspeed")));
	out.setVal("text_speed", mcpJsonInt(_vm->getTalkSpeed()));
	if (_vm->VAR_CHARINC != 0xFF)
		out.setVal("charinc", mcpJsonInt((int)_vm->VAR(_vm->VAR_CHARINC)));

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

	// Debug-only: dump every valid actor with its live position. Used to find
	// moving sprites that aren't background objects (e.g. the CMI cannon-minigame
	// skeleton boats). Read-only — touches no game state.
	Common::JSONArray actorsArr;
	for (int i = 1; _vm->_actors && i < _vm->_numActors; ++i) {
		Actor *a = _vm->_actors[i];
		if (!a) continue;
		Common::JSONObject ja;
		ja.setVal("number",  mcpJsonInt(a->_number));
		ja.setVal("x",       mcpJsonInt(a->getRealPos().x));
		ja.setVal("y",       mcpJsonInt(a->getRealPos().y));
		ja.setVal("top",     mcpJsonInt(a->_top));
		ja.setVal("bottom",  mcpJsonInt(a->_bottom));
		ja.setVal("width",   mcpJsonInt((int)a->_width));
		ja.setVal("room",    mcpJsonInt(a->_room));
		ja.setVal("costume", mcpJsonInt((int)a->_costume));
		ja.setVal("visible", mcpJsonBool(a->_visible));
		ja.setVal("moving",  mcpJsonInt(a->_moving));
		ja.setVal("in_room", mcpJsonBool(a->isInCurrentRoom()));
		Common::String an = getObjName(this, _vm->actorToObj(a->_number));
		ja.setVal("name", mcpJsonString(an.empty() ? Common::String::format("actor-%d", a->_number)
		                                           : normalizeActionName(safeUtf8(an))));
		actorsArr.push_back(new Common::JSONValue(ja));
	}
	out.setVal("actors", new Common::JSONValue(actorsArr));

	// Debug-only: dump the current room's walk-boxes (number, 4 corners, flags).
	// The CMI cannon minigame scores a hit by testing whether the cannonball
	// (an actor) lands inside a per-boat box, so the box geometry is what aiming
	// must target. Read-only.
	{
		Common::JSONArray boxesArr;
		int nb = _vm->getNumBoxes();
		for (int b = 0; b < nb; ++b) {
			BoxCoords bc = _vm->getBoxCoordinates(b);
			Common::JSONObject jb;
			jb.setVal("box",   mcpJsonInt(b));
			jb.setVal("ul_x",  mcpJsonInt(bc.ul.x)); jb.setVal("ul_y", mcpJsonInt(bc.ul.y));
			jb.setVal("ur_x",  mcpJsonInt(bc.ur.x)); jb.setVal("ur_y", mcpJsonInt(bc.ur.y));
			jb.setVal("lr_x",  mcpJsonInt(bc.lr.x)); jb.setVal("lr_y", mcpJsonInt(bc.lr.y));
			jb.setVal("ll_x",  mcpJsonInt(bc.ll.x)); jb.setVal("ll_y", mcpJsonInt(bc.ll.y));
			jb.setVal("flags", mcpJsonInt((int)_vm->getBoxFlags(b)));
			boxesArr.push_back(new Common::JSONValue(jb));
		}
		out.setVal("boxes", new Common::JSONValue(boxesArr));
	}

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

	// Game-specific diagnostics (e.g. CMI cannon-minigame aim state).
	augmentDebug(out);

	return new Common::JSONValue(out);
}


// The base class owns the keystroke / mouse_move / mouse_click schemas and
// argument parsing; these three overrides are the SCUMM-specific effect.

void ScummMcpBridge::injectKey(const Common::KeyState &ks) {
	_vm->_keyPressed = ks;
}

void ScummMcpBridge::injectMouseMove(int x, int y) {
	_vm->_mouse.x        = x;
	_vm->_mouse.y        = y;
	_vm->_virtualMouse.x = x;
	_vm->_virtualMouse.y = y;
	if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = x;
	if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = y;
	if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = x;
	if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = y;
}

void ScummMcpBridge::injectMouseClick(int x, int y, const Common::String &button, bool isDouble) {
	// Position the mouse first.
	injectMouseMove(x, y);

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
}

// ---------------------------------------------------------------------------
// Streaming pump
// ---------------------------------------------------------------------------

// SCUMM's slots in the shared pumpStream() skeleton. The generic stages
// (message drain, room-change close, stuck close, timeout, settle) live in
// MCP::McpBridge::pumpStream(); these overrides carry the SCUMM specifics.

void ScummMcpBridge::pumpStreamTrack() {
	// Anchor for the post-action speech allowance: the first frame on which the
	// action's own work was over, speech aside (see actionWorkDone).
	if (actionWorkDone()) {
		if (_sseWorkDoneFrame == 0)
			_sseWorkDoneFrame = _frameCounter;
	} else {
		_sseWorkDoneFrame = 0;
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
}

bool ScummMcpBridge::streamRoomChanged() const {
	return (int)_vm->_currentRoom != _ssePreRoom;
}

// Stuck: no speech, user-put locked. This includes both idle and animated
// states (e.g. cutscenes with ego moving). V8 (CMI) exception: userPut locked
// with no talkDelay means the dialog script is between lines deciding what to
// say next — not frozen. isActionDone() already guards this via its own userPut
// check, so skip stuck-close entirely there.
bool ScummMcpBridge::isStreamStuck() const {
	if (_vm->_game.version == 8 && _vm->_userPut <= 0)
		return false;
	return _vm->_talkDelay == 0 && _vm->_userPut <= 0;
}

bool ScummMcpBridge::streamHadActivity() const {
	return _sseLastEventFrame > 0 || _sseEgoMoved;
}

// Short budget when the action had no visible effect and completed quickly, a
// longer one once activity has been seen.
uint32 ScummMcpBridge::stuckFrames(bool hadActivity) const {
	return hadActivity ? 90 : 15;
}

// The 3600-frame (~120 s) absolute ceiling only guards the older games; where
// the per-event deadline applies (see streamTimeoutAnchor) it — still firing
// 20 s after dialogue genuinely stalls — is the sole safety net.
uint32 ScummMcpBridge::absoluteTimeoutFrames() const {
	return (_vm->_game.version < 7) ? 3600 : 0;
}

// For V7 (Dig/FT) and V8 (CMI) — and for Loom's play_note hatch cutscene
// (_sseAllowLongCutscene) — anchor to _sseLastEventFrame so that each new dialog
// line resets the deadline: long exchanges and room-transition cutscenes (e.g.
// walking out of a scene while characters talk, or the egg-hatch's
// Hetchel/cygnet dialogue) don't time out between lines. Other pre-V7 streams
// keep the start-anchored deadline, so a scene with ambient looping dialogue
// (the Indy3 student-mob office) still terminates promptly instead of being held
// open by background chatter.
uint32 ScummMcpBridge::streamTimeoutAnchor() const {
	bool perEventDeadline = (_vm->_game.version >= 7 || _sseAllowLongCutscene);
	return (perEventDeadline && _sseLastEventFrame > 0) ? _sseLastEventFrame : _sseStartFrame;
}

void ScummMcpBridge::pumpStreamMid() {
	// Clear the simulated mouse-button msDown bit a couple frames after the click
	// so that the dialog input script (V7 script 69) does not see a held button.
	if (_sseButtonClearFrame != 0 && _frameCounter >= _sseButtonClearFrame) {
		_vm->_leftBtnPressed  &= ~0x01; // clear msDown
		_vm->_rightBtnPressed &= ~0x01; // clear msDown (Dig pickup deselect)
		_sseButtonClearFrame = 0;
	}
}

// Runs after pumpStreamGameLate() (the Dig pickup-deselect and the V7 use-item /
// dialog-choice clicks), immediately before the settle decision.
void ScummMcpBridge::pumpStreamPreSettle() {
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
}

// The generic settle machinery (latching _sseDoneAtFrame, extending it on a new
// event) lives in MCP::McpBridge::pumpStream(); this override is the SCUMM
// decision about whether a settled stream may actually close.
bool ScummMcpBridge::shouldCloseStream() const {
	{
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
			return true;
		}
		return false;
	}
}

// ---------------------------------------------------------------------------
// Pre-action snapshot + state-change diff
// ---------------------------------------------------------------------------

void ScummMcpBridge::snapshotPreAction() {
	// The SCUMM tools open-code the stream setup rather than going through
	// beginStream(), so the shared per-stream bookkeeping hangs off here.
	noteStreamStart();
	_sseAllowLongCutscene = false;
	_sseDigDeselectDone = false;
	resetGameStream();
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
			Common::String cleanName =
			    changeEntityName(obj, cleanGameText(safeUtf8(normalizeActionName(name))));
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
		Common::String cleanName =
		    changeEntityName(obj, cleanGameText(safeUtf8(normalizeActionName(name))));
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
			pos.setVal("x", mcpJsonInt(toRoomPixelX(cx)));
			pos.setVal("y", mcpJsonInt(toRoomPixelY(cy)));
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
		entry.setVal("name",      mcpJsonString(changeEntityName(od.obj_nr,
		                                                         safeUtf8(mcpLowerTrimmed(name)))));
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
		} else if (!usesTextVerbBar()) {
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

	// Game-specific state-change fields (e.g. CMI cannon boats_remaining).
	augmentStateChanges(changes);

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

// Everything isActionDone() weighs *except* speech: the action's own work —
// walking, the sentence script, the game-specific click machines — is over and
// the engine has handed control back. Anchors the post-action speech allowance,
// so a room that talks to itself cannot make an action look unfinished forever.
bool ScummMcpBridge::actionWorkDone() const {
	if (_frameCounter - _sseStartFrame < 3) return false;
	if (_ssePendingSecondClick || !_ssePendingNotes.empty()) return false;
	if (gameStreamBusy()) return false;
	if (_ssePendingV7Choice != 0) return false;
	// Still cycling the Sam & Max verb cursor toward the mouth / opening talk.
	if (_sseSnmTalkActor != 0) return false;
	Actor *ego = getEgoActor();
	// Ego movement check with timeout only for V0 (Maniac Mansion):
	// V0 doesn't lock _userPut, so we need a timeout to prevent indefinite waits.
	// V5+ games handle movement more predictably and don't need this timeout.
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

bool ScummMcpBridge::isActionDone() const {
	if (!actionWorkDone())
		return false;
	// Speech means the action is still playing out — unless the room simply
	// talks to itself (Zak's living-room TV prints a line every few seconds
	// forever, with the player in full control). Once the work has been done
	// for longer than the speech allowance, stop waiting on speech: those lines
	// keep being captured and reach the client through the next state call.
	if (_vm->_talkDelay > 0 && !postActionSpeechExpired())
		return false;
	return true;
}

// Pre-V7 games are the ones that can leave a permanent talker running with the
// player in control. V7/V8 anchor their timeout per event instead (see
// streamTimeoutAnchor) and their conversations are the action, so they keep
// waiting on speech for as long as it takes.
uint32 ScummMcpBridge::postActionSpeechFrames() const {
	return (_vm->_game.version < 7) ? 90 : 0;
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
	if (!usesTextVerbBar()) {
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
	bool hasSentenceLine = false;
	for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (!vs.verbid || vs.saveid != 0) continue;
		// Skip OBIM verb slots (verbid=1) — they are graphical UI elements, not text
		// choices, but their key=0 would incorrectly set hasUnkeyed=true and prevent
		// Indy4-style numeric-key dialog detection. In V0, verbid=1 is Open, not OBIM.
		if (_vm->_game.version > 0 && vs.verbid == 1) continue;
		// A clickable dialog choice is always displayed (curmode==1). Hidden slots
		// (curmode==0) must not count — whether a choice not yet shown or, more
		// importantly, the numeric-keyed verb slots that linger after a finished
		// conversation. Fate of Atlantis leaves the captain's dialog verbs resident
		// (curmode=0, keys '1'-'9', old labels) while you read Plato's Dialogue, and
		// counting them used to make the book close-up report a phantom question.
		if (vs.curmode != 1) continue;
		// The sentence line is unkeyed like a dialog choice, but it means the
		// opposite: the game is echoing the pending command back during normal
		// play, and a conversation replaces it. MI2 leaves it up alone ("Row to")
		// while the verb bar is saved away in the swamp coffin, which used to read
		// as a dialog and lock act/walk out with "a dialog question is pending".
		if (isSentenceLineSlot(vs)) {
			const byte *sentence = _vm->getResourceAddress(rtVerb, slot);
			byte sentenceBuf[256] = {};
			if (sentence) _vm->convertMessageToString(sentence, sentenceBuf, sizeof(sentenceBuf));
			if (sentenceBuf[0]) hasSentenceLine = true;
			continue;
		}
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
	// Indy4-style: dialog choices have numeric keys; normal verb bar is saved
	// (saveid!=0). A visible sentence line rules it out: the game only draws that
	// while the player is picking a command, and the numeric slots of a finished
	// conversation stay resident with their old labels (Monkey Island 1).
	if (hasNumericKeyed && !hasUnkeyed && !hasSentenceLine) return true;
	return false;
}

// ---------------------------------------------------------------------------
// Name resolution
// ---------------------------------------------------------------------------

// Strips the trailing '@' name padding SCUMM adds to object names. Defined in
// mcp_actionname.cpp (kept engine-independent so the unit tests can link
// normalizeActionName without the whole engine); see there for details.
Common::String mcpStripNamePadding(const Common::String &s);

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

// normalizeActionName() and its mcpUtf8ToLower() helper live in
// mcp_actionname.cpp (engine-independent, so the unit tests can link it).

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

bool ScummMcpBridge::usesTextVerbBar() const {
	// V3-V5 (and the V0-V2 family) label their verb slots with text and swap the
	// bar out for text dialog choices; V6+ moved to image verbs and icon dialogs.
	// Leaves override where the version alone gets it wrong (Day of the Tentacle).
	return _vm && _vm->_game.version <= 5;
}

void ScummMcpBridge::findOpenCloseVerbIds(int &openVerb, int &closeVerb) const {
	openVerb = 0;
	closeVerb = 0;
	for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (!vs.verbid || vs.saveid != 0) continue;
		const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
		if (!ptr) continue;
		byte textBuf[256] = {};
		_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
		Common::String norm = normalizeActionName(safeUtf8(Common::String((const char *)textBuf)));
		if (!openVerb && norm == "open") openVerb = vs.verbid;
		else if (!closeVerb && norm == "close") closeVerb = vs.verbid;
	}
}

Common::String ScummMcpBridge::objectStateName(int numId, int rawState, bool isPathway) const {
	(void)isPathway;
	// Generic door/passage naming: an object that scripts both the open and the
	// close verb is an openable, so surface its opened/closed state. Doors start
	// closed (state 0); a non-zero image state is the opened image (verified on
	// Monkey Island's Scumm Bar doors, and consistent with "already opened" doors
	// such as Indy3's gym reading as opened). Games whose objects carry no scripted
	// open/close verbs (V6+ single-cursor titles) simply report no door states.
	int openVerb = 0, closeVerb = 0;
	findOpenCloseVerbIds(openVerb, closeVerb);
	if (openVerb && closeVerb &&
	    _vm->getVerbEntrypoint(numId, openVerb) != 0 &&
	    _vm->getVerbEntrypoint(numId, closeVerb) != 0)
		return (rawState != 0) ? "opened" : "closed";
	return Common::String();
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
		// A game leaf may surface an otherwise-untouchable object under a friendly
		// name (e.g. Monkey Island's kitchen plank) so the agent can target it.
		Common::String forcedName = syntheticObjectName(od.obj_nr);
		// Exclude objects the player cannot interact with (mirrors findObject()),
		// unless a leaf has force-named this object.
		if (forcedName.empty() && !isObjectSelectable(od)) continue;
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
		if (!forcedName.empty()) {
			e.baseName = normalizeActionName(forcedName);
		} else if (isLoomPassPathway) {
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
			if (!usesTextVerbBar()) {
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
		// Game-specific entity classification (e.g. CMI exit-hotspot pathways).
		classifyGameEntity(e.numId, e.isPathway);
		// An unnamed exit is all early SCUMM offers for room-to-room navigation
		// (Maniac Mansion's front yard leads to the porch through a nameless
		// hotspot), and "obj_258" tells an agent nothing about what it is. Name
		// those the way the Loom segment above already does, so the way out of a
		// room is recognisable in state.objects.
		if (name.empty() && forcedName.empty() && !isLoomPassPathway && e.isPathway)
			e.baseName = Common::String::format("pathway_%d", od.obj_nr);
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

	// Give a game the last word on names that came out the same for several
	// entities (Fate of Atlantis calls all seven of its cave mouths "cave").
	disambiguateEntityNames(entities);
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
	// Sam & Max: when the user asks for "max", prefer the *actor* (the sidekick
	// walking the scene) over the "max_the_object" inventory alias. talk_to,
	// look_at and pick_up all act on the actor, and toolAct's two-target "use Max
	// on Y" path detects the actor (id 3) and maps it to the inventory object for
	// the give sentence (see snmIsMaxEntity), so the actor is the right default.
	if (_vm->_game.id == GID_SAMNMAX && normalized == "max") {
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
		// No actor present (e.g. a scene where Max is only an inventory tool):
		// fall back to the inventory alias so "use max on Y" still resolves.
		for (uint i = 0; i < entities.size(); ++i) {
			if (entities[i].kind == NamedEntity::kInventory &&
			    entities[i].displayName == "max_the_object") {
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
	// Nothing matched exactly: give the game its own last chance (a name the
	// entity map had to number to keep apart is still asked for unnumbered).
	if (resolveGameEntityName(normalized, entities, out)) {
		debug(1, "mcp: resolveEntityByName '%s' resolved by the game to %d",
		      normalized.c_str(), out.numId);
		return true;
	}
	debug(1, "mcp: resolveEntityByName '%s' not found", name.c_str());
	return false;
}

// ---------------------------------------------------------------------------
// Coordinate space: engine grid <-> room pixels
// ---------------------------------------------------------------------------
// V0-V2 store actor and object coordinates on a compressed grid (x/8, y/2);
// V3 and later store room pixels. Everything the MCP surface publishes or
// accepts is in room pixels, so convert at the boundary.

int ScummMcpBridge::toRoomPixelX(int x) const {
	return (_vm->_game.version <= 2) ? x * V12_X_MULTIPLIER : x;
}

int ScummMcpBridge::toRoomPixelY(int y) const {
	return (_vm->_game.version <= 2) ? y * V12_Y_MULTIPLIER : y;
}

int ScummMcpBridge::fromRoomPixelX(int x) const {
	return (_vm->_game.version <= 2) ? x / V12_X_MULTIPLIER : x;
}

int ScummMcpBridge::fromRoomPixelY(int y) const {
	return (_vm->_game.version <= 2) ? y / V12_Y_MULTIPLIER : y;
}

// ---------------------------------------------------------------------------
// The sentence line and click-only screens
// ---------------------------------------------------------------------------

bool ScummMcpBridge::isSentenceLineSlot(const VerbSlot &vs) const {
	// Every verb on a V3-V5 bar carries a keyboard shortcut; the sentence line
	// the game writes the pending command into carries none and is centered.
	// (Dialog choices are unkeyed too, but left-aligned.)
	return usesTextVerbBar() && vs.type == kTextVerbType && vs.key == 0 && vs.center;
}

Common::String ScummMcpBridge::sentenceLineLabel() const {
	for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (!vs.verbid || vs.saveid != 0 || vs.curmode != 1) continue;
		if (!isSentenceLineSlot(vs)) continue;
		const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
		if (!ptr) continue;
		byte textBuf[256] = {};
		_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
		Common::String label = mcpLowerTrimmed((const char *)textBuf);
		if (!label.empty())
			return safeUtf8(label);
	}
	return Common::String();
}

bool ScummMcpBridge::isClickOnlyScreen() const {
	if (!usesTextVerbBar() || _vm->_userPut <= 0)
		return false;
	for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		// A usable bar verb: shown, not saved away, and not the sentence line.
		if (!vs.verbid || vs.saveid != 0 || vs.curmode != 1) continue;
		if (vs.type != kTextVerbType || !vs.key) continue;
		return false;
	}
	return true;
}

bool ScummMcpBridge::beginSceneClick(int roomX, int roomY) {
	VirtScreen *vs = &_vm->_virtscr[kMainVirtScreen];
	int mouseX = CLIP<int>(roomX - vs->xstart, 0, _vm->_screenWidth - 1);
	int mouseY = CLIP<int>(roomY + vs->topline, 0, _vm->_screenHeight - 1);

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

	_vm->_mouse.x        = mouseX;
	_vm->_mouse.y        = mouseY;
	_vm->_virtualMouse.x = roomX;
	_vm->_virtualMouse.y = roomY;
	if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = roomX;
	if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = roomY;
	if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = mouseX;
	if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = mouseY;
	_vm->_leftBtnPressed |= 0x03; // msClicked | msDown
	_debugButtonReleaseFrame = _frameCounter + 2;

	_server->startStreaming();
	return true;
}

bool ScummMcpBridge::verbOnBar(const Common::String &normalized) const {
	for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (!vs.verbid || vs.saveid != 0) continue;
		const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
		if (!ptr) continue;
		byte textBuf[256] = {};
		_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
		Common::String rawLabel = safeUtf8(Common::String((const char *)textBuf));
		if (rawLabel.empty()) continue;
		if (verbLabelMatches(rawLabel, normalized))
			return true;
	}
	return false;
}

Common::String ScummMcpBridge::verbBarLabel(int verbId) const {
	for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (vs.verbid != verbId || vs.saveid != 0) continue;
		const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
		if (!ptr) continue;
		byte textBuf[256] = {};
		_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
		Common::String label = safeUtf8(Common::String((const char *)textBuf));
		if (!label.empty())
			return label;
	}
	return "";
}

bool ScummMcpBridge::lookAtAnswersWithName() const {
	// Maniac Mansion (V0/V1/V2) and Zak (V1/V2). From V3 on ("Look at") the verb
	// runs a real script that describes the object.
	return _vm->_game.version <= 2;
}

bool ScummMcpBridge::beginNameAnswerStream(int verbId, int obj) {
	Common::String name = cleanGameText(safeUtf8(objName(obj)));
	if (name.empty()) {
		// Nameless scenery (an exit hotspot, say): answer with the name state
		// gave it, which is the name the caller asked about.
		Common::Array<NamedEntity> entities;
		buildEntityMap(entities);
		for (uint i = 0; i < entities.size(); ++i) {
			if (entities[i].kind == NamedEntity::kObject && entities[i].numId == obj) {
				name = entities[i].displayName;
				break;
			}
		}
	}
	if (name.empty())
		name = Common::String::format("object %d", obj);
	Common::String label = verbBarLabel(verbId);
	if (label.empty())
		label = "What is";
	beginBareStream();
	// The sentence line is what the game shows for this verb; say the same.
	pushMessage("system", -1, label + " " + name);
	_server->startStreaming();
	return true;
}

bool ScummMcpBridge::verbHasAnyEntrypoint(int verbId) const {
	for (int oi = 1; _vm->_objs && oi < _vm->_numLocalObjects; ++oi) {
		if (!_vm->_objs[oi].obj_nr) continue;
		if (_vm->getVerbEntrypoint(_vm->_objs[oi].obj_nr, verbId) != 0)
			return true;
	}
	for (int ii = 0; _vm->_inventory && ii < _vm->_numInventory; ++ii) {
		int obj = _vm->_inventory[ii];
		if (!obj) continue;
		if (_vm->getVerbEntrypoint(obj, verbId) != 0)
			return true;
	}
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
		// may not use the verb entrypoint system. Same for walk_to: walking is
		// what the engine does by default, so it needs no object script (in the
		// early SCUMM games no object scripts it at all). "What is" (V0-V2) is
		// answered by the engine itself, in the sentence line, so it too has no
		// object script to look for.
		if (normalized == "talk_to" || normalized == "walk_to" ||
		    (normalized == "look_at" && lookAtAnswersWithName())) {
			verbId = vs.verbid;
			debug(1, "mcp: resolveVerb found verbid=%d via label match (%s)",
			      verbId, normalized.c_str());
			return true;
		}
		// For other verbs, verify the verb has an actual entrypoint; skip if not
		// (the verb bar text might be reused or mislabeled).
		if (verbHasAnyEntrypoint(vs.verbid)) {
			verbId = vs.verbid;
			debug(1, "mcp: resolveVerb found verbid=%d via label match", verbId);
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

	// Game-specific verb mapping (e.g. CMI's V8 verb ids differ from V6 and must
	// be checked before the V6+ canonical lookup below).
	if (resolveGameVerb(normalized, verbId))
		return true;

	// V6+: standard action verbs are image-based. Resolve by verbid directly.
	if (!usesTextVerbBar()) {
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

	// Text-verb-bar games: if no text label matched for walk_to, take the first
	// active verb slot. Skipped for the image-verb games, where walk_to is always
	// verbid 13 and handled by kFallback below.
	if (normalized == "walk_to" && usesTextVerbBar()) {
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
