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

#include "agos/mcp.h"
#include "agos/mcp_names.h"

#include "agos/agos.h"
#include "agos/intern.h"

#include "common/events.h"
#include "common/localization.h"
#include "common/system.h"

namespace AGOS {

using Networking::mcpJsonString;
using Networking::mcpJsonInt;
using Networking::mcpJsonBool;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

AgosMcpBridge *AgosMcpBridge::create(AGOSEngine *vm) {
	AgosMcpBridge *bridge = new AgosMcpBridge(vm);
	bridge->init();
	return bridge;
}

AgosMcpBridge::AgosMcpBridge(AGOSEngine *vm) :
	MCP::McpBridge(vm, "scummvm", "1.0"),
	_vm(vm),
	_inPump(false),
	_skipStream(false),
	_lastFrameMs(0),
	_scrollTries(0),
	_ssePreRoom(-1),
	_sseTrackRoom(-1),
	_sseTrackSteps(0) {
}

AgosMcpBridge::~AgosMcpBridge() {
}

bool AgosMcpBridge::engineReady() const {
	// The bridge is built in the engine's constructor so the port binds before
	// anything can block on startup; the item table is the last of the pieces
	// it reads to be built.
	return _vm != nullptr && _vm->_itemArrayPtr != nullptr;
}

// ---------------------------------------------------------------------------
// Pumping
// ---------------------------------------------------------------------------

void AgosMcpBridge::pumpFromDelay() {
	if (!isEnabled() || _inPump)
		return;
	_inPump = true;
	// delay() is called from the main loop, from waitForInput() and from every
	// blocking wait in between, at wildly different rates. A frame is defined
	// by the wall clock instead, so the streaming budgets mean the same thing
	// however hot the calling loop happens to be.
	const uint32 now = g_system != nullptr ? g_system->getMillis() : 0;
	if (now - _lastFrameMs >= kFrameMs) {
		_lastFrameMs = now;
		MCP::McpBridge::pump();
	} else {
		pumpTransportOnly();
	}
	_inPump = false;
}

void AgosMcpBridge::pumpGame() {
	// One queued step per frame, so a click is never sent before the hover
	// that should precede it has been taken.
	if (_steps.empty())
		return;
	if (_frameCounter < _steps[0].notBeforeFrame)
		return;
	if (!_steps[0].target.empty() && _steps[0].kind != kStepSettle && !resolveStepTarget(_steps[0]))
		return;
	Step &step = _steps[0];
	switch (step.kind) {
	case kStepHover:
		injectMouseMove(step.x, step.y);
		break;
	case kStepClick:
		injectMouseClick(step.x, step.y, "left", false);
		break;
	case kStepSettle:
		break;
	}
	_steps.remove_at(0);
	if (!_steps.empty())
		_steps[0].notBeforeFrame = _frameCounter + kStepFrames;
}

void AgosMcpBridge::queueStep(StepKind kind, int x, int y, uint32 delayFrames) {
	Step step;
	step.kind = kind;
	step.x = x;
	step.y = y;
	step.notBeforeFrame = _frameCounter + delayFrames;
	_steps.push_back(step);
}

void AgosMcpBridge::queueClick(int x, int y, uint32 delayFrames) {
	queueStep(kStepHover, x, y, delayFrames);
	queueStep(kStepClick, x, y, kStepFrames);
}

void AgosMcpBridge::queueTargetClick(const Common::String &target, uint32 delayFrames) {
	queueStep(kStepHover, 0, 0, delayFrames);
	_steps.back().target = target;
	queueStep(kStepClick, 0, 0, kStepFrames);
	_steps.back().target = target;
}

bool AgosMcpBridge::resolveStepTarget(Step &step) {
	Target target;
	Common::String error;
	if (!resolveTarget(step.target, target, error)) {
		// Gone while the action was being played out - taken, used up, or the
		// room changed under it. Nothing sensible is left to click.
		onSystemLine(error);
		_steps.clear();
		_scrollTries = 0;
		return false;
	}
	if (target.onScreen) {
		step.x = target.x;
		step.y = target.y;
		_scrollTries = 0;
		return true;
	}
	// Carried, but scrolled off the inventory strip. A player pages the strip
	// with its arrows until the item shows; so does this, down first and then
	// back up, and gives up once both ways have been tried.
	const HitArea *arrow = nullptr;
	if (_scrollTries < 8)
		arrow = inventoryArrow(true);
	if (arrow == nullptr && _scrollTries < 16)
		arrow = inventoryArrow(false);
	if (arrow == nullptr) {
		onSystemLine(Common::String::format(
			"%s is carried but could not be brought onto the inventory strip",
			target.name.c_str()));
		_steps.clear();
		_scrollTries = 0;
		return false;
	}
	_scrollTries++;
	const int ax = arrow->x + arrow->width / 2;
	const int ay = arrow->y + arrow->height / 2;
	Step hover;
	hover.kind = kStepHover;
	hover.x = ax;
	hover.y = ay;
	hover.notBeforeFrame = _frameCounter;
	Step click = hover;
	click.kind = kStepClick;
	_steps.insert_at(0, click);
	_steps.insert_at(0, hover);
	return false;
}

// ---------------------------------------------------------------------------
// Reading the game
// ---------------------------------------------------------------------------

int AgosMcpBridge::roomNumber() const {
	if (!engineReady())
		return -1;
	// The room is the item the player is inside: this engine keeps its world
	// as a tree of items, and moving between rooms is reparenting the player.
	// `_currentRoom` looks like the answer and is not - it is Waxworks' own
	// bookkeeping (AGOSEngine::loadRoomItems), and stays 0 for the whole of
	// Simon the Sorcerer - so it is only the fallback, for the games that do
	// keep it.
	Item *player = _vm->me();
	if (player != nullptr && player->parent != 0)
		return (int)player->parent;
	return (int)_vm->_currentRoom;
}

bool AgosMcpBridge::playerHasControl() const {
	// AGOS implements no canSaveGameStateCurrently() of its own, so the answer
	// every other bridge here leans on is not available. What it does have is
	// waitForInput(), which is where its whole loop sits between actions -
	// being inside that is exactly what "the game is waiting for the player"
	// means, and it is false for the whole of a cutscene.
	if (!engineReady())
		return false;
	if (!isSimon1())
		return _vm->_mcpWaitingForInput;
	// Simon the Sorcerer also takes a click while "Use X with" waits for its
	// thing, and takes none while the pointer is hidden - a scene playing out
	// inside waitForInput() - or while the postcard's panel is up.
	if (_vm->_mouseHideCount != 0 || filePanelOpen())
		return false;
	return _vm->_mcpWaitingForInput || secondTargetPending();
}

bool AgosMcpBridge::isSimon1() const {
	return _vm != nullptr && _vm->getGameType() == GType_SIMON1;
}

bool AgosMcpBridge::usesDialogQuestions() const {
	return isSimon1();
}

const HitArea *AgosMcpBridge::liveBox(uint16 id) const {
	if (!engineReady())
		return nullptr;
	for (uint i = 0; i < ARRAYSIZE(_vm->_hitAreas); i++) {
		const HitArea &area = _vm->_hitAreas[i];
		if (area.id != id || !(area.flags & kBFBoxInUse) || (area.flags & kBFBoxDead))
			continue;
		if (area.width == 0 || area.height == 0)
			continue;
		return &area;
	}
	return nullptr;
}

const HitArea *AgosMcpBridge::inventoryArrow(bool down) const {
	return liveBox(down ? 0x7FFC : 0x7FFB);
}

bool AgosMcpBridge::verbBarShowing() const {
	// The bar is boxes 101 to 112; "Walk to" is always among them.
	return liveBox(101) != nullptr;
}

bool AgosMcpBridge::filePanelOpen() const {
	// The postcard is the game's own menu: a card of Load, Save, Quit and
	// Continue (boxes 201 to 204), Load and Save open the file panel
	// (AGOSEngine_Simon1::userGame, whose way out is box 205), and Quit asks
	// "ARE YOU SURE ? Y/N". None of them is the game, and a click meant for
	// the room lands on one of their words instead.
	return isSimon1() && engineReady() &&
	       (liveBox(204) != nullptr || liveBox(205) != nullptr || _vm->_mcpAskingYesNo);
}

bool AgosMcpBridge::secondTargetPending() const {
	return isSimon1() && engineReady() && _vm->_mcpWaitingForClick &&
	       _vm->_mouseHideCount == 0 && verbBarShowing() && !filePanelOpen() && !choicePending();
}

bool AgosMcpBridge::choicePending() const {
	if (!isSimon1() || !engineReady() || !_vm->_mcpWaitingForClick || verbBarShowing())
		return false;
	Common::Array<Choice> choices;
	collectChoices(choices);
	return !choices.empty();
}

bool AgosMcpBridge::isInterfaceBox(const HitArea &area) const {
	// The verb bar, the inventory strip and its arrows, the save/load panel
	// and the [ OK ] of a message window: all things a player clicks, none of
	// them things in the room.
	const uint16 id = area.id;
	return (id >= 101 && id <= 112) || (id >= 200 && id <= 213) ||
	       id == 0x7FFB || id == 0x7FFC || id == 0x7FFD || id == 0x7FFF;
}

void AgosMcpBridge::collectChoices(Common::Array<Choice> &out) const {
	if (!isSimon1() || !engineReady())
		return;
	// A choice is a box across the text window with an item behind it and no
	// name of its own; what it says is whatever was written on its line.
	for (uint i = 0; i < ARRAYSIZE(_vm->_hitAreas); i++) {
		const HitArea &area = _vm->_hitAreas[i];
		if (!(area.flags & kBFBoxInUse) || (area.flags & kBFBoxDead))
			continue;
		// One line of text tall: the window's own backdrop is a box with an
		// item behind it too, and spans every line at once.
		if (area.itemPtr == nullptr || area.width < 100 || area.height == 0 || area.height > 12)
			continue;
		if (isInterfaceBox(area) || !itemLabel(area.itemPtr).empty())
			continue;
		Common::String text;
		for (uint j = 0; j < _windowLines.size(); j++) {
			if (_windowLines[j].y >= (int)area.y && _windowLines[j].y < (int)(area.y + area.height))
				text = _windowLines[j].text;
		}
		// The game numbers its lines itself ("1Yes please."); the id an
		// answer takes is the position, so the digit goes.
		uint start = 0;
		while (start < text.size() && (Common::isDigit(text[start]) || text[start] == ' ' || text[start] == '.'))
			start++;
		text = Common::String(text.c_str() + start);
		text.trim();
		if (text.empty())
			continue;
		Choice choice;
		choice.x = area.x + area.width / 2;
		choice.y = area.y + area.height / 2;
		choice.text = text;
		uint at = 0;
		while (at < out.size() && out[at].y < choice.y)
			at++;
		out.insert_at(at, choice);
	}
}

Common::String AgosMcpBridge::itemLabel(const Item *item) const {
	if (!engineReady() || item == nullptr)
		return Common::String();
	// The same lookup displayName() makes when the pointer rests on something:
	// the item's object child carries a string id, and that string is the name
	// the player reads along the bottom of the screen.
	SubObject *object = (SubObject *)_vm->findChildOfType(const_cast<Item *>(item), kObjectType);
	if (object == nullptr)
		return Common::String();
	const byte *text = _vm->getStringPtrByID(object->objectName);
	return text != nullptr ? Common::String((const char *)text) : Common::String();
}

void AgosMcpBridge::collectTargets(Common::Array<Target> &out) const {
	if (!engineReady())
		return;
	Common::Array<Common::String> seen;
	for (uint i = 0; i < ARRAYSIZE(_vm->_hitAreas); i++) {
		const HitArea &area = _vm->_hitAreas[i];
		// A dead box is one the scripts have switched off; a box with no item
		// behind it is a piece of interface (the verb bar, the scroll arrows)
		// rather than a thing in the room.
		if (area.flags & kBFBoxDead)
			continue;
		if (area.width == 0 || area.height == 0)
			continue;
		Common::String label;
		if (isSimon1()) {
			if (!(area.flags & kBFBoxInUse) || isInterfaceBox(area))
				continue;
			// Scenery - the door, the fireplace - is a text box: no item behind
			// it, and its name is a short string the flags point at, which is
			// what displayName() prints for it.
			if (area.flags & kBFTextBox) {
				const uint index = area.flags / 256;
				if (index < _vm->_numTextBoxes) {
					const byte *text = _vm->getStringPtrByID(_vm->_shortText[index]);
					if (text != nullptr)
						label = Common::String((const char *)text);
				}
			} else if (area.itemPtr != nullptr) {
				label = itemLabel(area.itemPtr);
			}
		} else {
			if (area.itemPtr == nullptr)
				continue;
			label = itemLabel(area.itemPtr);
		}
		Common::String name = agosObjectName(label);
		if (name.empty())
			continue;
		uint occurrence = 0;
		for (uint j = 0; j < seen.size(); j++) {
			if (seen[j] == name)
				occurrence++;
		}
		seen.push_back(name);

		Target target;
		target.name = agosDisambiguate(name, occurrence);
		target.label = label;
		target.x = area.x + area.width / 2;
		target.y = area.y + area.height / 2;
		target.hitAreaId = area.id;
		target.carried = false;
		target.onScreen = true;
		out.push_back(target);
	}
}

void AgosMcpBridge::collectInventory(Common::Array<Target> &out) const {
	if (!engineReady())
		return;
	Item *player = _vm->me();
	if (player == nullptr)
		return;
	Common::Array<Common::String> seen;
	// The engine keeps what is carried as the player item's children, threaded
	// through `next`.
	for (Item *held = _vm->derefItem(player->child); held != nullptr;
	     held = _vm->derefItem(held->next)) {
		const Common::String label = itemLabel(held);
		Common::String name = agosObjectName(label);
		if (name.empty())
			continue;
		uint occurrence = 0;
		for (uint j = 0; j < seen.size(); j++) {
			if (seen[j] == name)
				occurrence++;
		}
		seen.push_back(name);

		Target entry;
		entry.name = agosDisambiguate(name, occurrence);
		entry.label = label;
		entry.x = entry.y = 0;
		entry.hitAreaId = 0;
		entry.carried = true;
		entry.onScreen = false;
		// Simon the Sorcerer draws what is carried as icons along the strip,
		// each a box of its own; one that is scrolled away has none.
		for (uint i = 0; isSimon1() && i < ARRAYSIZE(_vm->_hitAreas); i++) {
			const HitArea &area = _vm->_hitAreas[i];
			if (area.id != 0x7FFD || area.itemPtr != held || (area.flags & kBFBoxDead) ||
			    !(area.flags & kBFBoxInUse))
				continue;
			entry.x = area.x + area.width / 2;
			entry.y = area.y + area.height / 2;
			entry.hitAreaId = area.id;
			entry.onScreen = true;
			break;
		}
		out.push_back(entry);
	}
}

bool AgosMcpBridge::resolveTarget(const Common::String &name, Target &out,
                                  Common::String &errorOut) const {
	Common::Array<Target> targets;
	collectTargets(targets);
	collectInventory(targets);
	const Common::String wanted = MCP::McpBridge::normalizeActionName(name);
	for (uint i = 0; i < targets.size(); i++) {
		if (MCP::McpBridge::normalizeActionName(targets[i].name) == wanted) {
			out = targets[i];
			return true;
		}
	}
	Common::String here;
	for (uint i = 0; i < targets.size(); i++) {
		if (!here.empty())
			here += ", ";
		here += targets[i].name;
	}
	if (here.empty())
		here = "nothing";
	errorOut = Common::String::format("nothing here is called '%s'. In this room: %s",
	                                  name.c_str(), here.c_str());
	return false;
}

bool AgosMcpBridge::verbButtonPosition(int index, int &x, int &y) const {
	if (!engineReady() || index < 0)
		return false;
	// Simon the Sorcerer's bar is boxes 101 to 112 in the order the verbs are
	// written, and that id is how the engine itself recognises a click on it
	// (input.cpp). Their `verb` is script data rather than a place on the bar,
	// so matching on it alone found no button on the Amiga release, and every
	// act() was refused as "the verb bar is not showing" while can_act was true.
	if (_vm->getGameType() == GType_SIMON1) {
		for (uint i = 0; i < ARRAYSIZE(_vm->_hitAreas); i++) {
			const HitArea &area = _vm->_hitAreas[i];
			if (area.id != (uint16)(101 + index))
				continue;
			if ((area.flags & kBFBoxDead) || area.width == 0 || area.height == 0)
				continue;
			x = area.x + area.width / 2;
			y = area.y + area.height / 2;
			return true;
		}
	}
	// The bar is hit areas like everything else, and each carries the verb it
	// stands for. Finding the button by what it does - rather than by where it
	// is - means a game that lays its bar out differently still works.
	for (uint i = 0; i < ARRAYSIZE(_vm->_hitAreas); i++) {
		const HitArea &area = _vm->_hitAreas[i];
		if (area.flags & kBFBoxDead)
			continue;
		if (area.width == 0 || area.height == 0)
			continue;
		// The engine numbers its verbs from 1 in the order the bar is written.
		if ((area.verb & 0x3FFF) != (uint16)(index + 1))
			continue;
		x = area.x + area.width / 2;
		y = area.y + area.height / 2;
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void AgosMcpBridge::injectKey(const Common::KeyState &ks) {
	Common::Event down;
	down.type = Common::EVENT_KEYDOWN;
	down.kbd = ks;
	g_system->getEventManager()->pushEvent(down);
	Common::Event up;
	up.type = Common::EVENT_KEYUP;
	up.kbd = ks;
	g_system->getEventManager()->pushEvent(up);
}

void AgosMcpBridge::injectMouseMove(int x, int y) {
	Common::Event move;
	move.type = Common::EVENT_MOUSEMOVE;
	move.mouse = Common::Point(x, y);
	g_system->getEventManager()->pushEvent(move);
	g_system->warpMouse(x, y);
}

void AgosMcpBridge::injectMouseClick(int x, int y, const Common::String &button, bool) {
	const bool right = (button == "right");
	Common::Event down;
	down.type = right ? Common::EVENT_RBUTTONDOWN : Common::EVENT_LBUTTONDOWN;
	down.mouse = Common::Point(x, y);
	g_system->getEventManager()->pushEvent(down);
	Common::Event up;
	up.type = right ? Common::EVENT_RBUTTONUP : Common::EVENT_LBUTTONUP;
	up.mouse = Common::Point(x, y);
	g_system->getEventManager()->pushEvent(up);
}

int AgosMcpBridge::currentRoomForMessages() const {
	return roomNumber();
}

void AgosMcpBridge::onGameText(const Common::String &text) {
	if (!isEnabled() || text.empty())
		return;
	// In Simon the Sorcerer this is only ever the name under the pointer,
	// which state() already carries as a label; as a message it drowned out
	// what was actually said, one "Magnet" per hover.
	if (isSimon1())
		return;
	onSystemLine(text);
}

void AgosMcpBridge::onSpeech(const Common::String &text) {
	if (!isEnabled() || !isSimon1() || text.empty())
		return;
	onDialogPrompt(text);
}

void AgosMcpBridge::onWindowChar(WindowBlock *window, byte c) {
	if (!isEnabled() || !isSimon1() || window == nullptr || c < 32)
		return;
	const int y = window->y + window->textRow * 8;
	for (uint i = 0; i < _windowLines.size(); i++) {
		WindowLine &line = _windowLines[i];
		if (line.window != window || line.y != y)
			continue;
		// A line written from its first column again is a new line.
		if (window->textLength == 0)
			line.text.clear();
		line.text += (char)c;
		return;
	}
	WindowLine line;
	line.window = window;
	line.y = y;
	line.text += (char)c;
	if (_windowLines.size() >= 32)
		_windowLines.remove_at(0);
	_windowLines.push_back(line);
}

void AgosMcpBridge::onWindowClear(WindowBlock *window) {
	for (uint i = _windowLines.size(); i > 0; i--) {
		if (_windowLines[i - 1].window == window)
			_windowLines.remove_at(i - 1);
	}
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

Common::JSONValue *AgosMcpBridge::toolState(const Common::JSONValue &, Common::String &errorOut) {
	if (!engineReady()) {
		errorOut = "state: the game is still starting up";
		return nullptr;
	}
	Common::JSONObject out;

	Common::JSONObject room;
	room.setVal("id", mcpJsonInt(roomNumber()));
	out.setVal("room", new Common::JSONValue(room));

	out.setVal("can_act", mcpJsonBool(playerHasControl()));

	// All twelve, always. The bar is the same in every game this engine runs,
	// and an agent that can read it never has to guess which words are taken.
	Common::JSONArray verbs;
	for (int i = 0; i < agosVerbCount(); i++)
		verbs.push_back(mcpJsonString(agosVerbName(i)));
	out.setVal("verbs", new Common::JSONValue(verbs));

	Common::Array<Target> targets;
	collectTargets(targets);
	Common::JSONArray objects;
	for (uint i = 0; i < targets.size(); i++) {
		Common::JSONObject entry;
		entry.setVal("name", mcpJsonString(targets[i].name));
		entry.setVal("label", mcpJsonString(targets[i].label));
		entry.setVal("x", mcpJsonInt(targets[i].x));
		entry.setVal("y", mcpJsonInt(targets[i].y));
		objects.push_back(new Common::JSONValue(entry));
	}
	out.setVal("objects", new Common::JSONValue(objects));

	Common::Array<Target> carried;
	collectInventory(carried);
	Common::JSONArray inventory;
	for (uint i = 0; i < carried.size(); i++) {
		Common::JSONObject entry;
		entry.setVal("name", mcpJsonString(carried[i].name));
		entry.setVal("label", mcpJsonString(carried[i].label));
		inventory.push_back(new Common::JSONValue(entry));
	}
	out.setVal("inventory", new Common::JSONValue(inventory));

	Common::JSONArray messages;
	for (uint i = 0; i < _messages.size(); i++) {
		const Common::String text = MCP::mcpCleanGameText(_messages[i].text);
		if (text.empty())
			continue;
		Common::JSONObject m;
		m.setVal("text", mcpJsonString(text));
		m.setVal("type", mcpJsonString(_messages[i].type));
		messages.push_back(new Common::JSONValue(m));
	}
	_messages.clear();
	out.setVal("messages", new Common::JSONValue(messages));

	addChangesExtras(out);
	return new Common::JSONValue(out);
}

void AgosMcpBridge::addChangesExtras(Common::JSONObject &out) const {
	if (!isSimon1())
		return;
	Common::Array<Choice> choices;
	if (choicePending())
		collectChoices(choices);
	if (!choices.empty()) {
		Common::JSONArray list;
		for (uint i = 0; i < choices.size(); i++) {
			Common::JSONObject choice;
			choice.setVal("id", mcpJsonInt((int)i + 1));
			choice.setVal("label", mcpJsonString(choices[i].text));
			list.push_back(new Common::JSONValue(choice));
		}
		Common::JSONObject question;
		question.setVal("choices", new Common::JSONValue(list));
		out.setVal("question", new Common::JSONValue(question));
	}
	out.setVal("menu_open", mcpJsonBool(filePanelOpen()));
	out.setVal("awaiting_second_target", mcpJsonBool(secondTargetPending()));
}

bool AgosMcpBridge::toolAct(const Common::JSONValue &args, Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "act: another action is already in progress";
		return false;
	}
	if (!engineReady()) {
		errorOut = "act: the game is still starting up";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("target1") ||
	    !args.asObject()["target1"]->isString()) {
		errorOut = "act: a string 'target1' is required";
		return false;
	}

	if (isSimon1()) {
		if (filePanelOpen()) {
			errorOut = _skipToolEnabled
			    ? "act: a menu is open (Load, Save, Quit, Continue) - call skip to close it and go back to the game"
			    : "act: a menu is open (Load, Save, Quit, Continue)";
			return false;
		}
		if (choicePending()) {
			errorOut = "act: a question is waiting - answer it with answer(id), see state.question";
			return false;
		}
	}

	Common::String verb = "look_at";
	if (args.asObject().contains("verb") && args.asObject()["verb"]->isString())
		verb = MCP::McpBridge::normalizeActionName(args.asObject()["verb"]->asString());

	Common::String secondName;
	if (args.asObject().contains("target2") && args.asObject()["target2"]->isString())
		secondName = args.asObject()["target2"]->asString();

	if (secondTargetPending()) {
		// "Use X with" is on the sentence line and the game wants the thing.
		// Whatever verb came with it, the click it is waiting for is on a thing,
		// and target2 is the second of a pair given in full.
		const Common::String name = secondName.empty() ? args.asObject()["target1"]->asString() : secondName;
		Target thing;
		if (!resolveTarget(name, thing, errorOut)) {
			errorOut = Common::String("act: ") + errorOut;
			return false;
		}
		_steps.clear();
		queueTargetClick(name, 0);
		queueStep(kStepSettle, 0, 0, kStepFrames);
		_skipStream = false;
		beginStream();
		return true;
	}

	const int index = agosVerbIndex(verb);
	if (index < 0) {
		Common::String list;
		for (int i = 0; i < agosVerbCount(); i++) {
			if (!list.empty())
				list += ", ";
			list += agosVerbName(i);
		}
		errorOut = Common::String::format("act: '%s' is not a verb here. The bar has: %s.",
		                                  verb.c_str(), list.c_str());
		return false;
	}

	Target target;
	if (!resolveTarget(args.asObject()["target1"]->asString(), target, errorOut)) {
		errorOut = Common::String("act: ") + errorOut;
		return false;
	}
	Target second;
	if (isSimon1() && !secondName.empty() && !resolveTarget(secondName, second, errorOut)) {
		errorOut = Common::String("act: ") + errorOut;
		return false;
	}

	// Asked *after* the name has been checked, deliberately. A name this room
	// does not have is wrong whatever the game happens to be doing, and saying
	// so is the only way a caller learns it; answering "not accepting input"
	// instead sends it away to wait for a moment that would not have helped.
	if (!playerHasControl()) {
		errorOut = "act: the game is not accepting input right now";
		return false;
	}

	int verbX = 0, verbY = 0;
	if (!verbButtonPosition(index, verbX, verbY)) {
		errorOut = Common::String::format(
			"act: the verb bar is not showing, so '%s' cannot be chosen. "
			"This happens during a cutscene and while a panel is open.",
			verb.c_str());
		return false;
	}

	// Exactly what a player does: click the word, then click the thing. Each
	// click is preceded by a hover, because the scripts act on the hover -
	// clicking without one resolves against wherever the pointer was before.
	_steps.clear();
	queueStep(kStepHover, verbX, verbY, 0);
	queueStep(kStepClick, verbX, verbY, kStepFrames);
	if (isSimon1()) {
		// Aimed by name, so a carried item is found on the strip - scrolled
		// to if need be - when its turn comes. "Use" and "Give" then wait for
		// a second thing ("Use magnet with"), and a target2 is that click;
		// without one the action ends with the game still asking for it.
		queueTargetClick(target.name, kStepFrames);
		if (!secondName.empty())
			queueTargetClick(second.name, kStepFrames * 2);
	} else {
		queueStep(kStepHover, target.x, target.y, kStepFrames);
		queueStep(kStepClick, target.x, target.y, kStepFrames);
	}
	queueStep(kStepSettle, 0, 0, kStepFrames);

	_skipStream = false;
	beginStream();
	return true;
}

bool AgosMcpBridge::toolAnswer(const Common::JSONValue &args, Common::String &errorOut) {
	if (!isSimon1()) {
		errorOut = "answer: this game never puts a numbered choice to the player";
		return false;
	}
	if (isStreaming()) {
		errorOut = "answer: another action is already in progress";
		return false;
	}
	Common::Array<Choice> choices;
	if (choicePending())
		collectChoices(choices);
	if (choices.empty()) {
		errorOut = "answer: no question is waiting";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("id") || !args.asObject()["id"]->isIntegerNumber()) {
		errorOut = "answer: an integer 'id' is required";
		return false;
	}
	const int id = (int)args.asObject()["id"]->asIntegerNumber();
	if (id < 1 || id > (int)choices.size()) {
		errorOut = Common::String::format("answer: id must be between 1 and %u", choices.size());
		return false;
	}
	// A player answers by clicking the line.
	_steps.clear();
	queueClick(choices[id - 1].x, choices[id - 1].y, 0);
	queueStep(kStepSettle, 0, 0, kStepFrames);
	_skipStream = false;
	beginStream();
	return true;
}

bool AgosMcpBridge::toolWalk(const Common::JSONValue &args, Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "walk: another action is already in progress";
		return false;
	}
	if (!engineReady()) {
		errorOut = "walk: the game is still starting up";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("x") || !args.asObject().contains("y") ||
	    !args.asObject()["x"]->isIntegerNumber() || !args.asObject()["y"]->isIntegerNumber()) {
		errorOut = "walk: integer 'x' and 'y' are required";
		return false;
	}
	if (isSimon1() && filePanelOpen()) {
		errorOut = _skipToolEnabled
			    ? "walk: a menu is open (Load, Save, Quit, Continue) - call skip to close it and go back to the game"
			    : "walk: a menu is open (Load, Save, Quit, Continue)";
		return false;
	}
	if (isSimon1() && choicePending()) {
		errorOut = "walk: a question is waiting - answer it with answer(id), see state.question";
		return false;
	}
	if (!playerHasControl()) {
		errorOut = "walk: the game is not accepting input right now";
		return false;
	}

	const int x = (int)args.asObject()["x"]->asIntegerNumber();
	const int y = (int)args.asObject()["y"]->asIntegerNumber();

	// Walking is "Walk to" on the bar and then a click on the floor, which is
	// how a player walks here too.
	int verbX = 0, verbY = 0;
	_steps.clear();
	if (verbButtonPosition(agosVerbIndex("walk_to"), verbX, verbY)) {
		queueStep(kStepHover, verbX, verbY, 0);
		queueStep(kStepClick, verbX, verbY, kStepFrames);
	}
	queueStep(kStepHover, x, y, kStepFrames);
	queueStep(kStepClick, x, y, kStepFrames);
	queueStep(kStepSettle, 0, 0, kStepFrames);

	_skipStream = false;
	beginStream();
	return true;
}

bool AgosMcpBridge::toolSkip(const Common::JSONValue &, Common::String &errorOut) {
	if (!_skipToolEnabled) {
		errorOut = "skip: tool is disabled (set mcp_skip_tool=true)";
		return false;
	}
	// The postcard's menu takes no key: its way out is a word a player clicks -
	// Continue on the card, the exit on the file panel. Only the quit question
	// is a key, and the answer that goes back to the game is N.
	if (isSimon1() && !isStreaming() && filePanelOpen()) {
		if (_vm->_mcpAskingYesNo) {
			Common::KeyCode keyYes, keyNo;
			Common::getLanguageYesNo(_vm->_language, keyYes, keyNo);
			injectKey(Common::KeyState(keyNo));
			_skipStream = true;
			beginStream();
			return true;
		}
		const HitArea *exit = liveBox(205);
		if (exit == nullptr)
			exit = liveBox(204);
		if (exit != nullptr) {
			_steps.clear();
			queueClick(exit->x + exit->width / 2, exit->y + exit->height / 2, 0);
			queueStep(kStepSettle, 0, 0, kStepFrames);
			_skipStream = false;
			beginStream();
			return true;
		}
	}
	// Two different waits, and skipping means getting past either: a running
	// cutscene ends through the engine's own exit-cutscene flag, and a line
	// sitting there to be dismissed goes away on a keypress.
	if (_vm != nullptr)
		_vm->mcpExitCutscene();
	Common::KeyState escape(Common::KEYCODE_ESCAPE, 27);
	injectKey(escape);
	if (!isStreaming()) {
		_skipStream = true;
		beginStream();
	}
	return true;
}

Common::JSONValue *AgosMcpBridge::toolDebug(const Common::JSONValue &, Common::String &) {
	Common::JSONObject out;
	out.setVal("ready", mcpJsonBool(engineReady()));
	if (!engineReady())
		return new Common::JSONValue(out);
	out.setVal("room", mcpJsonInt(roomNumber()));
	out.setVal("can_act", mcpJsonBool(playerHasControl()));
	out.setVal("verb_hit_area", mcpJsonInt(_vm->_verbHitArea));
	out.setVal("queued_steps", mcpJsonInt((int)_steps.size()));
	Common::Array<Target> targets;
	collectTargets(targets);
	out.setVal("clickable", mcpJsonInt((int)targets.size()));
	out.setVal("mouse_hidden", mcpJsonInt(_vm->_mouseHideCount));
	out.setVal("walking", mcpJsonBool(_vm->getBitFlag(11)));
	// Every live hit area, so what state() leaves out can be seen for what it is.
	Common::JSONArray areas;
	for (uint i = 0; i < ARRAYSIZE(_vm->_hitAreas); i++) {
		const HitArea &area = _vm->_hitAreas[i];
		if (!(area.flags & kBFBoxInUse))
			continue;
		Common::JSONObject a;
		a.setVal("id", mcpJsonInt(area.id));
		a.setVal("x", mcpJsonInt(area.x));
		a.setVal("y", mcpJsonInt(area.y));
		a.setVal("w", mcpJsonInt(area.width));
		a.setVal("h", mcpJsonInt(area.height));
		a.setVal("flags", mcpJsonInt(area.flags));
		a.setVal("verb", mcpJsonInt(area.verb));
		a.setVal("label", mcpJsonString(itemLabel(area.itemPtr)));
		// Not itemPtrToID(): the verb bar's boxes point at dummy items outside
		// the table, and it answers those by stopping in the debugger.
		int itemId = -1;
		for (uint32 j = 0; area.itemPtr != nullptr && j < _vm->_itemArraySize; j++) {
			if (_vm->_itemArrayPtr[j] == area.itemPtr) {
				itemId = (int)j;
				break;
			}
		}
		a.setVal("item", mcpJsonInt(itemId));
		areas.push_back(new Common::JSONValue(a));
	}
	out.setVal("hit_areas", new Common::JSONValue(areas));
	return new Common::JSONValue(out);
}

// ---------------------------------------------------------------------------
// What an agent reads about the tools
// ---------------------------------------------------------------------------

Common::String AgosMcpBridge::stateToolDescription() const {
	return "The room as it is now: its number, everything in it that can be "
	       "clicked with the words the game itself shows for them, the twelve "
	       "verbs on the bar, what is being carried, and every line said since "
	       "the last call (reading them clears them). The names here are the "
	       "names act() takes.";
}

Common::String AgosMcpBridge::actToolDescription() const {
	return "Act on something state() named. This game has a bar of twelve "
	       "verbs and an action is one of them applied to one thing, which is "
	       "done the way a player does it: the word is clicked and then the "
	       "thing is. state() lists the twelve.";
}

Common::String AgosMcpBridge::walkToolDescription() const {
	return "Walk to a point by choosing 'Walk to' on the bar and clicking the "
	       "floor there, which is what a player does. To go through a door, "
	       "act() on it instead.";
}

Common::String AgosMcpBridge::debugToolDescription() const {
	return "Raw engine state: the room, whether the player has control, the "
	       "verb the engine currently has selected, how many things are "
	       "clickable and how much queued input is still to be played out.";
}

// ---------------------------------------------------------------------------
// Schemas
// ---------------------------------------------------------------------------

static Common::JSONValue *agosObjectArraySchema(bool withPosition) {
	Common::JSONObject props;
	props.setVal("name", Networking::mcpProp("string", "Name to use in act()."));
	props.setVal("label", Networking::mcpProp("string",
	    "The words the game itself writes for it when the pointer rests on it."));
	if (withPosition) {
		props.setVal("x", Networking::mcpProp("integer", "Where it is, in game coordinates."));
		props.setVal("y", Networking::mcpProp("integer", "Where it is, in game coordinates."));
	}
	Common::JSONObject item;
	item.setVal("type", Networking::mcpJsonString("object"));
	item.setVal("properties", new Common::JSONValue(props));
	Common::JSONObject array;
	array.setVal("type", Networking::mcpJsonString("array"));
	array.setVal("items", new Common::JSONValue(item));
	return new Common::JSONValue(array);
}

void AgosMcpBridge::augmentStateSchema(Common::JSONObject &outputProps) {
	outputProps.setVal("can_act", Networking::mcpProp("boolean",
	    "Whether the game is taking input right now."));
	outputProps.setVal("verbs", Networking::mcpProp("array",
	    "The twelve verbs on the bar, which are the verbs act() takes."));
	outputProps.setVal("objects", agosObjectArraySchema(true));
	outputProps.setVal("inventory", agosObjectArraySchema(false));
	if (isSimon1()) {
		outputProps.setVal("menu_open", Networking::mcpProp("boolean",
		    _skipToolEnabled ? "A menu (Load, Save, Quit, Continue) is over the game; skip closes it."
		                     : "A menu (Load, Save, Quit, Continue) is over the game."));
		outputProps.setVal("awaiting_second_target", Networking::mcpProp("boolean",
		    "\"Use X with\" or \"Give X to\" is waiting for its second thing; act() on it."));
	}
}

void AgosMcpBridge::augmentChangesSchema(Common::JSONObject &props) {
	// The base schema carries `room_changed` as a plain flag; this bridge
	// answers with the room itself, because an agent that has just left one
	// wants to know where it came out.
	Common::JSONObject room;
	room.setVal("type", Networking::mcpJsonString("object"));
	Common::JSONObject roomProps;
	roomProps.setVal("id", Networking::mcpProp("integer", "The room now."));
	roomProps.setVal("changed", Networking::mcpProp("boolean",
	    "Whether this action left the room it started in."));
	room.setVal("properties", new Common::JSONValue(roomProps));
	props.setVal("room", new Common::JSONValue(room));

	props.setVal("can_act", Networking::mcpProp("boolean",
	    "Whether the game is taking input now the action is over."));
	props.setVal("objects_appeared", Networking::mcpProp("array",
	    "Things in the room that were not there before."));
	props.setVal("objects_gone", Networking::mcpProp("array",
	    "Things that were in the room and are not now."));
	props.setVal("items_gained", Networking::mcpProp("array",
	    "Items picked up while the action ran."));
	props.setVal("items_lost", Networking::mcpProp("array",
	    "Items put down or given away while the action ran."));
	if (isSimon1()) {
		props.setVal("menu_open", Networking::mcpProp("boolean",
		    _skipToolEnabled ? "A menu (Load, Save, Quit, Continue) is over the game; skip closes it."
		                     : "A menu (Load, Save, Quit, Continue) is over the game."));
		props.setVal("awaiting_second_target", Networking::mcpProp("boolean",
		    "\"Use X with\" or \"Give X to\" is waiting for its second thing; act() on it."));
	}
}

void AgosMcpBridge::augmentActSchema(Common::JSONObject &props) {
	Common::String list;
	for (int i = 0; i < agosVerbCount(); i++) {
		if (!list.empty())
			list += ", ";
		list += agosVerbName(i);
	}
	const Common::String desc = "One of the twelve verbs on the bar: " + list + ".";
	props.setVal("verb", Networking::mcpProp("string", desc.c_str()));
}

Common::JSONValue *AgosMcpBridge::buildDebugSchema() const {
	Common::JSONObject props;
	return Networking::mcpObjectSchema(props);
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

void AgosMcpBridge::snapshotPreAction() {
	_ssePreRoom = roomNumber();
	_ssePreTargets.clear();
	Common::Array<Target> targets;
	collectTargets(targets);
	for (uint i = 0; i < targets.size(); i++)
		_ssePreTargets.push_back(targets[i].name);
	_ssePreInventory.clear();
	Common::Array<Target> carried;
	collectInventory(carried);
	for (uint i = 0; i < carried.size(); i++)
		_ssePreInventory.push_back(carried[i].name);
	_sseTrackRoom = _ssePreRoom;
	_sseTrackSteps = _steps.size();
	_sseTrackThings.clear();
	for (uint i = 0; isSimon1() && i < _ssePreTargets.size(); i++)
		_sseTrackThings += _ssePreTargets[i] + ",";
}

static void agosDiffNames(const Common::Array<Common::String> &before,
                          const Common::Array<Common::String> &after,
                          Common::JSONArray &appeared, Common::JSONArray &gone) {
	for (uint i = 0; i < after.size(); i++) {
		bool was = false;
		for (uint j = 0; j < before.size(); j++)
			was = was || before[j] == after[i];
		if (!was)
			appeared.push_back(Networking::mcpJsonString(after[i]));
	}
	for (uint j = 0; j < before.size(); j++) {
		bool still = false;
		for (uint i = 0; i < after.size(); i++)
			still = still || after[i] == before[j];
		if (!still)
			gone.push_back(Networking::mcpJsonString(before[j]));
	}
}

Common::JSONObject AgosMcpBridge::buildStateChanges() const {
	Common::JSONObject out;

	const int room = roomNumber();
	Common::JSONObject roomObj;
	roomObj.setVal("id", mcpJsonInt(room));
	roomObj.setVal("changed", mcpJsonBool(room != _ssePreRoom));
	out.setVal("room", new Common::JSONValue(roomObj));

	Common::Array<Target> targets;
	collectTargets(targets);
	Common::Array<Common::String> now;
	for (uint i = 0; i < targets.size(); i++)
		now.push_back(targets[i].name);
	Common::JSONArray appeared, gone;
	agosDiffNames(_ssePreTargets, now, appeared, gone);
	out.setVal("objects_appeared", new Common::JSONValue(appeared));
	out.setVal("objects_gone", new Common::JSONValue(gone));

	Common::Array<Target> carried;
	collectInventory(carried);
	Common::Array<Common::String> held;
	for (uint i = 0; i < carried.size(); i++)
		held.push_back(carried[i].name);
	Common::JSONArray gained, lost;
	agosDiffNames(_ssePreInventory, held, gained, lost);
	out.setVal("items_gained", new Common::JSONValue(gained));
	out.setVal("items_lost", new Common::JSONValue(lost));

	out.setVal("can_act", mcpJsonBool(playerHasControl()));
	addChangesExtras(out);
	return out;
}

bool AgosMcpBridge::isActionDone() const {
	if (_skipStream) {
		if ((_frameCounter - _sseStartFrame) >= 12)
			return true;
		return g_system != nullptr && (g_system->getMillis() - _sseStartMs) >= kSkipMs;
	}
	// Not until every queued click has been played out, and then not until the
	// game is taking input again.
	if (!_steps.empty())
		return false;
	if (!isSimon1())
		return playerHasControl();
	// Simon the Sorcerer stays inside waitForInput() while he walks over to
	// what was clicked, so "taking input" came back true the moment the click
	// landed and the result was read before the magnet had moved. Done is
	// when he has stopped and the pointer is back - or when the game has
	// stopped to ask for something: an answer, a second thing, the panel.
	if (choicePending() || filePanelOpen() || secondTargetPending())
		return true;
	return _vm->_mcpWaitingForInput && _vm->_mouseHideCount == 0 && !_vm->getBitFlag(11);
}

bool AgosMcpBridge::hasPendingQuestion() const {
	return choicePending();
}

bool AgosMcpBridge::streamRoomChanged() const {
	if (roomNumber() == _ssePreRoom)
		return false;
	// Simon the Sorcerer builds the new room's boxes a few at a time after the
	// player has been moved into it - and after it is taking input again - so
	// a result read at the move named nothing there and half of the old room
	// as gone. Reading hit areas mid-change is harmless in this engine, so the
	// room change is left to the ordinary settle, which pumpStreamTrack()
	// keeps extending while the list of things is still changing.
	if (isSimon1())
		return false;
	return true;
}

void AgosMcpBridge::pumpStreamTrack() {
	const int room = roomNumber();
	Common::String things;
	if (isSimon1()) {
		Common::Array<Target> targets;
		collectTargets(targets);
		for (uint i = 0; i < targets.size(); i++)
			things += targets[i].name + ",";
	}
	if (room != _sseTrackRoom || _steps.size() != _sseTrackSteps || things != _sseTrackThings) {
		_sseTrackRoom = room;
		_sseTrackSteps = _steps.size();
		_sseTrackThings = things;
		_sseLastEventFrame = _frameCounter;
	}
}

} // End of namespace AGOS
