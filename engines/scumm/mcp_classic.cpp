/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "common/util.h"

#include "scumm/mcp_subclasses.h"
#include "scumm/object.h"
#include "scumm/resource.h"
#include "scumm/scumm.h"
#include "scumm/verbs.h"

namespace Scumm {

using Networking::mcpJsonString;
using Networking::mcpJsonInt;
using Networking::mcpJsonBool;
using Networking::mcpProp;
using Networking::mcpObjectSchema;

// Game/version-specific MCP bridge logic for the classic V3–V5 SCUMM family
// (Loom, Indy3, Indy4/Atlantis, Monkey Island, Passport to Adventure). Migrated
// out of mcp.cpp via the ScummMcpBridge virtual hooks. The shared Loom-note and
// Indy3-fight helpers live on McpBridgeClassic so the Loom, Indy3 and (mixed)
// Passport leaves can all reuse them under single inheritance.

// ---------------------------------------------------------------------------
// McpBridgeClassic — shared helpers
// ---------------------------------------------------------------------------

bool McpBridgeClassic::indy3FightActive() const {
	// Indy3's fight HUD is driven by a stable set of script vars (322..327) that
	// hold each fighter's punch-power gauge and health. Outside a fight these
	// vars are 0, so a non-zero opponent health (within a sane range) is a
	// reliable signal that a fist-fight is in progress.
	if (vmNumVariables() <= 327)
		return false;
	int32 indyHealth = vmVar(325);
	int32 oppHealth  = vmVar(327);
	return indyHealth > 0 && oppHealth > 0 && indyHealth <= 2000 && oppHealth <= 2000;
}

bool McpBridgeClassic::loomSectionByVerbLabels() const {
	// Loom in Passport renders its distaff as the verb bar: every slot's label
	// is a single-character glyph (note icons), e.g. 'z', '{', '^'. Indy3 and
	// MI1 segments populate the bar with multi-character English verbs
	// ("Open", "Look at", etc.). Detect by examining slot label lengths.
	bool sawAnyVerb = false;
	bool sawWordLabel = false;
	for (int slot = 1; _vm->_verbs && slot < vmNumVerbs(); ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (!vs.verbid) continue;
		if (vs.saveid != 0) continue;
		if (_vm->_game.version > 0 && vs.verbid == 1) continue; // OBIM slot
		if (vs.curmode != 0 && vs.curmode != 1) continue;
		const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
		if (!ptr) continue;
		byte textBuf[256] = {};
		vmConvertMessageToString(ptr, textBuf, sizeof(textBuf));
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

void McpBridgeClassic::applyLoomVerbs(Common::JSONArray &verbsArr,
                                      Common::Array<VerbInfo> &activeVerbs, bool questionPending) {
	// Loom (full game) and the Loom mini-game inside Passport to Adventure use
	// a single-cursor model + distaff instead of the V3 text verb bar. Discard
	// any V3 verbs that the text-slot extraction may have populated and expose
	// only 'interact' (left-click) and 'use_item' (inventory-on-object). Note
	// input goes through the separate 'play_note' tool.
	if (!isInLoomSection() || questionPending)
		return;
	verbsArr.clear();
	activeVerbs.clear();
	static const struct { int id; const char *name; const char *label; } kLoomFallback[] = {
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

void McpBridgeClassic::addIndy3FightSchema(Common::JSONObject &outputProps) {
	// The fight HUD is only present while a fist fight is on, but the state
	// schema is closed (additionalProperties: false), so it has to be declared
	// by every game that can emit it.
	Common::JSONObject fighter;
	fighter.setVal("health",      mcpProp("integer", "Health gauge (0 = knocked out)"));
	fighter.setVal("punch_power", mcpProp("integer", "Punch-power gauge"));
	Common::JSONObject fighterSchema;
	fighterSchema.setVal("type",       mcpJsonString("object"));
	fighterSchema.setVal("properties", new Common::JSONValue(fighter));
	Common::JSONObject fightProps;
	fightProps.setVal("indy",     new Common::JSONValue(fighterSchema));
	Common::JSONObject opponentSchema;
	opponentSchema.setVal("type",       mcpJsonString("object"));
	opponentSchema.setVal("properties", new Common::JSONValue(fighter));
	fightProps.setVal("opponent", new Common::JSONValue(opponentSchema));
	Common::JSONObject fightSchema;
	fightSchema.setVal("type",       mcpJsonString("object"));
	fightSchema.setVal("properties", new Common::JSONValue(fightProps));
	outputProps.setVal("fight", new Common::JSONValue(fightSchema));
}

void McpBridgeClassic::addIndy3FightHud(Common::JSONObject &out) const {
	// Indy3 fist-fight HUD — surface each fighter's health and punch-power gauge
	// so the MCP client can mirror what the in-game HUD shows. Driven by Indy3's
	// script vars 322..327.
	if (!isInIndy3Fight())
		return;
	Common::JSONObject fight;
	Common::JSONObject indy;
	indy.setVal("health",      mcpJsonInt((int)vmVar(325)));
	indy.setVal("punch_power", mcpJsonInt((int)vmVar(322)));
	fight.setVal("indy", new Common::JSONValue(indy));
	Common::JSONObject opponent;
	opponent.setVal("health",      mcpJsonInt((int)vmVar(327)));
	opponent.setVal("punch_power", mcpJsonInt((int)vmVar(323)));
	fight.setVal("opponent", new Common::JSONValue(opponent));
	out.setVal("fight", new Common::JSONValue(fight));
}

void McpBridgeClassic::registerPlayNoteTool() {
	// --- play_note (Loom distaff) ---
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
	spec.outputSchema = buildChangesSchema();
	spec.streaming    = true;
	_server->registerTool(spec);
}

Common::JSONValue *McpBridgeClassic::dispatchPlayNote(const Common::String &name,
                                                      const Common::JSONValue &args,
                                                      Common::String &errorOut, bool &handled) {
	if (name == "play_note") {
		handled = true;
		toolPlayNote(args, errorOut);
		return nullptr; // streaming
	}
	return ScummMcpBridge::dispatchGameTool(name, args, errorOut, handled);
}

bool McpBridgeClassic::toolPlayNote(const Common::JSONValue &args, Common::String &errorOut) {
	if (_streaming) {
		errorOut = "play_note: another action is already in progress";
		return false;
	}
	if (!isInLoomSection()) {
		errorOut = "play_note: only available in the Loom segment";
		return false;
	}
	if (vmUserPut() <= 0) {
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
	// Replaying the egg's draft can hatch it into a multi-minute cutscene whose
	// dialogue streams past the start-anchored 600-frame deadline; let each line
	// reset it (bounded by the absolute 3600-frame ceiling). See the timeout in
	// pumpStream() and _sseAllowLongCutscene.
	_sseAllowLongCutscene = true;
	_sseMessages.clear();
	_ssePendingSecondClick = false;
	_ssePendingNotes = keys;
	_sseTargetObject = 0;

	// pumpStream() will dispatch the queued notes via runInputScript on
	// subsequent frames; nothing else to do here.
	_server->startStreaming();
	return true;
}

void McpBridgeClassic::pumpStreamGame() {
	// The note watcher and the deferred note/second-click feed below are driven
	// by Loom-only state: var(259) (the distaff note variable) and the
	// _ssePending* flags, which only Loom's act/play_note paths ever set. They
	// are inert for the other classic games (Indy3/Monkey/Indy4), so this runs
	// unconditionally — matching the engine-wide behaviour it was migrated from —
	// rather than gating on the loomSectionByVerbLabels() heuristic, which can be
	// momentarily false mid-action and drop a queued Loom double-click.

	// On the first pump of a new stream, snapshot the Loom note variable so the
	// watcher below only emits transitions occurring during this action.
	if (_frameCounter == _sseStartFrame) {
		_ssePrevNoteValue = (vmNumVariables() > 259) ? vmVar(259) : 0;
		_sseLastNoteFedFrame = 0;
	}

	// Loom note watcher: var(259) is set by the engine each time a distaff note
	// is played — both when an object sings (e.g. the egg playing the Opening
	// draft) and when the player presses a note key. Detect 0 -> note transitions
	// and surface them as MCP notifications so the client can learn the songs
	// objects play.
	if (vmNumVariables() > 259) {
		int32 cur = vmVar(259);
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
		vmMouse().x = _sseClickMouseX;
		vmMouse().y = _sseClickMouseY;
		if (_vm->VAR_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_MOUSE_X) = _sseClickMouseX;
		if (_vm->VAR_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_MOUSE_Y) = _sseClickMouseY;
		vmLastInputScriptTime() = _vm->_system->getMillis();
		vmLeftBtnPressed() |= 0x03; // msClicked | msDown
		_ssePendingSecondClick = false;
	}

	// Feed the next pending note by invoking the engine's verb script directly
	// (kKeyClickArea). Each runInputScript invocation runs Script 97 (Loom's
	// input handler) which kills any prior instance — meaning two notes in rapid
	// succession would have the second overwrite the first. Pace feeds fast
	// enough that the script's draft-buffer timeout doesn't fire mid-sequence but
	// slow enough that each note's script completes.
	const uint32 kNoteSpacingFrames = 15;
	if (!_ssePendingNotes.empty()
	    && (_sseLastNoteFedFrame == 0
	        || _frameCounter - _sseLastNoteFedFrame >= kNoteSpacingFrames)) {
		Common::KeyCode kc = _ssePendingNotes[0];
		_ssePendingNotes.remove_at(0);
		_sseLastNoteFedFrame = _frameCounter;
		// kKeyClickArea handler reads the ASCII value for the key. For the distaff
		// note keys (lowercase letters c/d/e/f/g/a/b plus capital C), the keycode
		// value equals the ASCII byte.
		vmRunInputScript(kKeyClickArea, (int)kc, 1);
	}
}

// ---------------------------------------------------------------------------
// McpBridgeIndy4 — Fate of Atlantis "Lost Dialogue" book
// ---------------------------------------------------------------------------

// The "Lost Dialogue" close-up (room 83) is driven entirely by clicks: its entry
// script points VAR_VERB_SCRIPT at local script 200, which looks up whatever
// object sits under the mouse and acts on it. Clicking either page half closes
// the book; clicking a page's tab (the paper clips down the edge) runs that
// page's script (200 + page), which redraws the spread and prints its text.
//
// Object ids are regular: the page halves are 1103/1104, and page N (1-based)
// owns three tab objects — 1103 + 2N and 1104 + 2N (its clip drawn on the top
// edge / on the fore edge) plus 1114 + N (its clip as the page being shown).
// Exactly one of the three is ever enabled, and 1114 + N is the enabled one
// precisely when the book stands open at page N.
static const int kBookPageLeft  = 1103;
static const int kBookPageRight = 1104;

bool McpBridgeIndy4::isInAtlantisBook() const {
	// The page halves are only loaded while the close-up is open, so their
	// presence pins it without hard-coding the room id.
	return vmGetObjectIndex(kBookPageLeft) != -1 && vmGetObjectIndex(kBookPageRight) != -1;
}

int McpBridgeIndy4::atlantisBookPageFromName(const Common::String &name) {
	// Accept "page_N" (1..kAtlantisBookPages); return 0 for anything else.
	Common::String n = name;
	n.toLowercase();
	if (!n.hasPrefix("page_")) return 0;
	Common::String num = n;
	num.erase(0, 5);
	if (num.empty()) return 0;
	int page = 0;
	for (uint i = 0; i < num.size(); ++i) {
		if (num[i] < '0' || num[i] > '9') return 0;
		page = page * 10 + (num[i] - '0');
	}
	return (page >= 1 && page <= kAtlantisBookPages) ? page : 0;
}

bool McpBridgeIndy4::isAtlantisBookName(const Common::String &name) {
	Common::String n = normalizeActionName(name);
	return n == "book" || n == "lost_dialogue_of_plato" || n == "lost_dialogue";
}

int McpBridgeIndy4::atlantisBookCurrentPage() const {
	for (int p = 1; p <= kAtlantisBookPages; ++p) {
		if (vmGetObjectIndex(1114 + p) != -1 && vmGetState(1114 + p) != 0)
			return p;
	}
	return 0;
}

int McpBridgeIndy4::atlantisBookTabObject(int page) const {
	// Exactly one of a page's three tab objects is enabled at a time; script 200
	// ignores a click on a disabled one, so pick the live one.
	const int tabs[3] = { 1103 + 2 * page, 1104 + 2 * page, 1114 + page };
	for (int i = 0; i < 3; ++i) {
		if (vmGetObjectIndex(tabs[i]) != -1 && vmGetState(tabs[i]) != 0)
			return tabs[i];
	}
	return 0;
}

void McpBridgeIndy4::augmentStateObjects(Common::JSONArray &objects) {
	// Surface each book page as a synthetic "page_N" object so an MCP client can
	// turn to it with `act look_at page_N`, and the book itself so it can be shut
	// again with `act close book`. The page's lines stream back as messages, which
	// is how the randomised Thera -> Atlantis heading on page 3 is read without
	// the mouse/screenshot debug tools.
	if (!isInAtlantisBook())
		return;
	const int current = atlantisBookCurrentPage();
	for (int p = 1; p <= kAtlantisBookPages; ++p) {
		Common::JSONObject page;
		page.setVal("id",               mcpJsonInt(0));
		page.setVal("name",             mcpJsonString(Common::String::format("page_%d", p)));
		page.setVal("state",            mcpJsonInt(p == current ? 1 : 0));
		page.setVal("x",                mcpJsonInt(0));
		page.setVal("y",                mcpJsonInt(0));
		page.setVal("pathway",          mcpJsonBool(false));
		if (current != 0)
			page.setVal("state_name", mcpJsonString(p == current ? "open" : "not open"));
		Common::JSONArray pv;
		pv.push_back(mcpJsonString("look at"));
		page.setVal("compatible_verbs", new Common::JSONValue(pv));
		objects.push_back(new Common::JSONValue(page));
	}

	Common::JSONObject book;
	book.setVal("id",               mcpJsonInt(0));
	book.setVal("name",             mcpJsonString("book"));
	book.setVal("state",            mcpJsonInt(1));
	book.setVal("x",                mcpJsonInt(0));
	book.setVal("y",                mcpJsonInt(0));
	book.setVal("pathway",          mcpJsonBool(false));
	book.setVal("state_name",       mcpJsonString("open"));
	Common::JSONArray bv;
	bv.push_back(mcpJsonString("close"));
	book.setVal("compatible_verbs", new Common::JSONValue(bv));
	objects.push_back(new Common::JSONValue(book));
}

bool McpBridgeIndy4::interceptGameAct(const Common::JSONObject &args, Common::String &errorOut,
                                      bool &handled) {
	// While the book close-up is open, "page_N" turns to that page and the book
	// itself closes. Both are synthetic targets rather than real verb scripts, so
	// handle them up front, before verb/target resolution (the close-up has no
	// verb bar at all, so nothing would resolve).
	handled = false;
	if (!isInAtlantisBook())
		return false;
	if (!args.contains("target1") || !args["target1"]->isString())
		return false;
	Common::String target = args["target1"]->asString();

	int page = atlantisBookPageFromName(target);
	if (page > 0) {
		// Any verb turns the page: in the close-up a click carries no verb.
		handled = true;
		return turnAtlantisBookPage(page, errorOut);
	}
	if (isAtlantisBookName(target)) {
		// Only "close" shuts the book; other verbs on it fall through so they
		// still report an unknown verb rather than silently leaving the close-up.
		Common::String verb;
		if (args.contains("verb") && args["verb"]->isString())
			verb = normalizeActionName(args["verb"]->asString());
		if (verb == "close" || verb == "put_away" || verb == "exit") {
			handled = true;
			return closeAtlantisBook(errorOut);
		}
	}
	return false;
}

bool McpBridgeIndy4::clickAtlantisBookObject(int obj, Common::String &errorOut) {
	int idx = vmGetObjectIndex(obj);
	if (idx == -1) {
		errorOut = "act: the book is not open";
		return false;
	}
	// Click the middle of the object exactly as a player would. Driving the page
	// scripts through the engine's own input path (rather than running them
	// directly) is what keeps the close-up rendering right: the click is picked up
	// by local script 200, the room's verb script, which redraws the spread before
	// its text is printed.
	const ObjectData &od = _vm->_objs[idx];
	int x = od.x_pos + od.width / 2;
	int y = od.y_pos + od.height / 2;

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
	_sseVerbScript = (_vm->VAR_VERB_SCRIPT != 0xFF) ? (int)_vm->VAR(_vm->VAR_VERB_SCRIPT) : 0;
	_sseInitialVerbScript = _sseVerbScript;
	_sseVerbScriptChanged = false;

	vmMouse().x        = x;
	vmMouse().y        = y;
	vmVirtualMouse().x = x;
	vmVirtualMouse().y = y;
	if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = x;
	if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = y;
	if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = x;
	if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = y;
	vmLeftBtnPressed() |= 0x03;         // msClicked | msDown
	_debugButtonReleaseFrame = _frameCounter + 2;

	_server->startStreaming();
	return true;
}

bool McpBridgeIndy4::turnAtlantisBookPage(int page, Common::String &errorOut) {
	if (page < 1 || page > kAtlantisBookPages) {
		errorOut = "act: invalid book page";
		return false;
	}
	if (page == atlantisBookCurrentPage()) {
		// The tab of the page on show sits under its own disabled variants, so the
		// game ignores a click on it — the book never re-prints the open page.
		errorOut = Common::String::format(
		    "act: page_%d is already open (turn to another page and back to re-read it)", page);
		return false;
	}
	int tab = atlantisBookTabObject(page);
	if (tab == 0) {
		errorOut = "act: that page cannot be reached";
		return false;
	}
	return clickAtlantisBookObject(tab, errorOut);
}

bool McpBridgeIndy4::closeAtlantisBook(Common::String &errorOut) {
	// Clicking either page half shuts the book and returns to the room it was
	// opened from.
	return clickAtlantisBookObject(kBookPageLeft, errorOut);
}

// ---------------------------------------------------------------------------
// McpBridgeMonkey — Monkey Island (V4)
// ---------------------------------------------------------------------------

bool McpBridgeMonkey::isGerman() const {
	return _vm->_language == Common::DE_DEU;
}

// The Scumm Bar kitchen (room 51) hosts the red-herring dock puzzle. Two scene
// hotspots there are authored as untouchable, unnamed objects: the gangplank
// the player bounces (obj 307) and the seagull, whose feeding/flight phase is
// tracked by three mutually-exclusive animation objects (301 is the resting
// "eating" frame the bird starts and returns to; 300/302 are its flight frames).
// Surface them under friendly names so an agent can target/read them.
static const int kMonkeyKitchenRoom = 51;
static const int kMonkeyPlankObj = 307;
static const int kMonkeyBirdEating = 301;
static const int kMonkeyBirdFlyAway = 300;
static const int kMonkeyBirdFlyHigher = 302;

Common::String McpBridgeMonkey::syntheticObjectName(int numId) const {
	if (_vm->_currentRoom != kMonkeyKitchenRoom)
		return Common::String();
	if (numId == kMonkeyPlankObj)
		return isGerman() ? "Planke" : "plank";
	// The seagull is drawn by whichever of its three phase objects is currently
	// active (state != 0). Surface only that one, as a single "bird".
	if ((numId == kMonkeyBirdEating || numId == kMonkeyBirdFlyAway ||
	     numId == kMonkeyBirdFlyHigher) && vmGetState(numId) != 0)
		return isGerman() ? "Vogel" : "bird";
	return Common::String();
}

Common::String McpBridgeMonkey::objectStateName(int numId, int rawState, bool isPathway) const {
	if (_vm->_currentRoom == kMonkeyKitchenRoom) {
		// Seagull feeding/flight phase (see kMonkeyBird* above).
		if (numId == kMonkeyBirdEating)
			return isGerman() ? "frisst" : "eating";
		if (numId == kMonkeyBirdFlyAway)
			return isGerman() ? "fliegt davon" : "flying away";
		if (numId == kMonkeyBirdFlyHigher)
			return isGerman() ? "fliegt höher davon" : "flying away higher";
	}
	// Fall back to the generic openable-door naming.
	return ScummMcpBridge::objectStateName(numId, rawState, isPathway);
}

// ---------------------------------------------------------------------------
// McpBridgeMonkey2 — the island maps and the swamp coffin (click-only screens)
// ---------------------------------------------------------------------------

Common::String McpBridgeMonkey2::clickVerbLabel() const {
	// In the coffin the sentence line reads "Row to", which is the game's own
	// name for a click; on the island map it reads nothing at all. The line also
	// picks up whatever the cursor happens to hover ("Row to swamp"), so cut it
	// back to the verb: the object part is the name of something in the room.
	Common::String label = sentenceLineLabel();
	if (!label.empty()) {
		Common::Array<NamedEntity> entities;
		buildEntityMap(entities);
		uint cut = label.size();
		for (uint i = 0; i < entities.size(); ++i) {
			Common::String name = Networking::mcpLowerTrimmed(entities[i].displayName);
			name.replace('_', ' ');
			if (name.empty())
				continue;
			const char *hit = strstr(label.c_str(), name.c_str());
			if (hit && (uint)(hit - label.c_str()) < cut)
				cut = (uint)(hit - label.c_str());
		}
		while (cut > 0 && label[cut - 1] == ' ')
			--cut;
		if (cut > 0)
			return Common::String(label.c_str(), cut);
	}
	return "walk to";
}

void McpBridgeMonkey2::applyGameVerbs(Common::JSONArray &verbsArr,
                                      Common::Array<VerbInfo> &activeVerbs, bool questionPending) {
	if (questionPending)
		return;

	if (!activeVerbs.empty()) {
		// "Walk to" has no button on MI2's bar (its slot stays hidden, since
		// walking is what a plain click does), but it is what every exit and every
		// step is: expose it, as DOTT's leaf does, so pathway objects advertise it
		// and act(verb='walk to') resolves. Read the id and the label off the
		// hidden slot itself, so a localised release exposes its own wording.
		for (uint i = 0; i < activeVerbs.size(); ++i)
			if (activeVerbs[i].name == "walk_to")
				return;
		for (int slot = 1; _vm->_verbs && slot < vmNumVerbs(); ++slot) {
			const VerbSlot &vs = _vm->_verbs[slot];
			if (!vs.verbid || vs.saveid != 0 || vs.type != kTextVerbType) continue;
			const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
			if (!ptr) continue;
			byte textBuf[256] = {};
			vmConvertMessageToString(ptr, textBuf, sizeof(textBuf));
			Common::String label = Networking::mcpLowerTrimmed((const char *)textBuf);
			if (label.empty() || normalizeActionName(label) != "walk_to") continue;
			verbsArr.push_back(mcpJsonString(safeUtf8(label)));
			VerbInfo walk;
			walk.verbId = vs.verbid;
			walk.name   = "walk_to";
			walk.label  = safeUtf8(label);
			activeVerbs.push_back(walk);
			break;
		}
		return;
	}

	// The bar is saved away on the island maps and in the swamp coffin, so the
	// generic pass found nothing. Publish the one thing that works — a click —
	// under the game's own name, so state does not read as "nothing to do here".
	if (!isClickOnlyScreen())
		return;
	Common::String label = clickVerbLabel();
	verbsArr.push_back(mcpJsonString(label));
	VerbInfo vi;
	vi.verbId = kClickOnlyVerb;
	vi.name   = normalizeActionName(label);
	vi.label  = label;
	activeVerbs.push_back(vi);
}

bool McpBridgeMonkey2::interceptGameAct(const Common::JSONObject &args, Common::String &errorOut,
                                        bool &handled) {
	if (!isClickOnlyScreen())
		return false;
	handled = true;
	if (!args.contains("target1")) {
		errorOut = "act: on this screen the only action is clicking somewhere — "
		           "pass the place as target1, or use walk(x, y)";
		return false;
	}
	// Any verb is accepted here: the screen has exactly one action, and refusing
	// an agent's "walk to" because the game happens to call it "row to" would be
	// pedantry. Resolve the target by name or id like act() does.
	NamedEntity entity;
	const Common::JSONValue *v = args["target1"];
	int obj = 0;
	if (v->isIntegerNumber()) {
		obj = (int)v->asIntegerNumber();
	} else if (v->isString() && resolveEntityByName(v->asString(), entity)) {
		obj = entity.numId;
	} else {
		errorOut = "act: unknown target1 '" + (v->isString() ? v->asString() : Common::String("?")) + "'";
		return false;
	}
	if (vmGetObjectIndex(obj) == -1) {
		errorOut = "act: that target is not on this screen";
		return false;
	}
	return beginSceneClick(vmGetObjX(obj), vmGetObjY(obj));
}

} // End of namespace Scumm
