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

#include "toon/mcp.h"
#include "toon/mcp_names.h"

#include "toon/anim.h"
#include "toon/audio.h"
#include "toon/character.h"
#include "toon/conversation.h"
#include "toon/drew.h"
#include "toon/flux.h"
#include "toon/hotspot.h"
#include "toon/movie.h"
#include "toon/path.h"
#include "toon/picture.h"
#include "toon/state.h"
#include "toon/toon.h"

#include "common/debug.h"
#include "common/events.h"
#include "common/system.h"

namespace Toon {

using Networking::mcpJsonString;
using Networking::mcpJsonInt;
using Networking::mcpJsonBool;
using Networking::mcpProp;
using Networking::mcpObjectSchema;

// The whole vocabulary: which button the player would have pressed, plus
// "just go over there", which is a click on the ground beside the thing
// rather than on the thing itself.
struct VerbName {
	const char *name;
	bool rightButton;
	bool walkOnly;
};
static const VerbName kVerbs[] = {
	{ "use",     false, false },
	{ "look_at", true,  false },
	{ "walk_to", false, true  }
};

// Aliases an agent is likely to reach for, folded onto the three real ones.
static bool verbFromName(const Common::String &name, const VerbName *&out) {
	Common::String n = name;
	if (n == "interact" || n == "pick_up" || n == "talk_to" || n == "open" ||
	    n == "close" || n == "push" || n == "pull" || n == "give" || n == "take")
		n = "use";
	else if (n == "examine" || n == "read" || n == "look")
		n = "look_at";
	else if (n == "go_to" || n == "move_to" || n == "approach")
		n = "walk_to";
	for (uint i = 0; i < ARRAYSIZE(kVerbs); i++) {
		if (n == kVerbs[i].name) {
			out = &kVerbs[i];
			return true;
		}
	}
	return false;
}

// True for a non-empty run of decimal digits, i.e. a target given as an id.
static bool allDigits(const Common::String &s) {
	if (s.empty())
		return false;
	for (uint i = 0; i < s.size(); i++)
		if (s[i] < '0' || s[i] > '9')
			return false;
	return true;
}

// Two things can carry the same label; suffix the later ones so every name
// resolves to exactly one thing.
static void disambiguate(Common::Array<Common::String> &names) {
	Common::Array<Common::String> base(names);
	for (uint i = 0; i < names.size(); i++) {
		uint seen = 0;
		for (uint j = 0; j < i; j++)
			if (base[j] == base[i])
				seen++;
		names[i] = toonDisambiguate(base[i], seen);
	}
}

// { "type": "array", "items": { "type": "object", "properties": props } }
static Common::JSONValue *objectArraySchema(Common::JSONObject &props) {
	Common::JSONObject item;
	item.setVal("type",       mcpJsonString("object"));
	item.setVal("properties", new Common::JSONValue(props));
	Common::JSONObject arr;
	arr.setVal("type",  mcpJsonString("array"));
	arr.setVal("items", new Common::JSONValue(item));
	return new Common::JSONValue(arr);
}

// The hand holds nothing / whatever it already holds.
static const int32 kHandEmpty = -1;
static const int32 kHandLeave = -2;

// Where a conversation's icon row is drawn, in the game's own numbers
// (ToonEngine::haveAConversation).
static const int kChoiceX = 75, kChoiceStride = 60, kChoiceY = 360;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ToonMcpBridge::ToonMcpBridge(ToonEngine *vm)
	: MCP::McpBridge(vm),
	  _vm(vm),
	  _pendingPhase(kPhaseNone),
	  _pendingX(0),
	  _pendingY(0),
	  _pendingRight(false),
	  _pendingHeld(kHandLeave),
	  _pendingScrollTo(0),
	  _pendingFrame(0),
	  _scrollPinned(false),
	  _escapePressed(false),
	  _scrollWasLocked(false),
	  _skipStream(false),
	  _ssePreScene(0),
	  _ssePreHeld(-1),
	  _sseActionStarted(false),
	  _sseTrackScene(0),
	  _sseTrackPosX(0),
	  _sseTrackPosY(0),
	  _sseTrackControl(false) {
}

ToonMcpBridge::~ToonMcpBridge() {
}

ToonMcpBridge *ToonMcpBridge::create(ToonEngine *vm) {
	ToonMcpBridge *bridge = new ToonMcpBridge(vm);
	bridge->init();
	return bridge;
}

// ---------------------------------------------------------------------------
// Engine state helpers
// ---------------------------------------------------------------------------

bool ToonMcpBridge::engineReady() const {
	return _vm->state() != nullptr && _vm->getHotspots() != nullptr &&
	       _vm->getDrew() != nullptr && _vm->getPicture() != nullptr &&
	       _vm->state()->_currentScene > 0;
}

bool ToonMcpBridge::inConversation() const {
	return engineReady() && _vm->state()->_inConversation &&
	       _vm->state()->_showConversationIcons;
}

bool ToonMcpBridge::leadMoving() const {
	return engineReady() && (_vm->getDrew()->getFlag() & 1) != 0;
}

bool ToonMcpBridge::canAct() const {
	if (!engineReady())
		return false;
	if (_vm->getMoviePlayer() && _vm->getMoviePlayer()->isPlaying())
		return false;
	State *gs = _vm->state();
	// The engine hides the pointer for as long as it is busy with something of
	// its own - a line being spoken, a script running - and drops every click
	// that arrives meanwhile.
	if (gs->_mouseHidden || gs->_inMenu || gs->_inInventory)
		return false;
	if (gs->_inConversation)
		return false;
	return !leadMoving();
}

void ToonMcpBridge::leadPos(int &x, int &y) const {
	x = y = 0;
	if (!engineReady())
		return;
	x = _vm->getDrew()->getX();
	y = _vm->getDrew()->getY();
}

int ToonMcpBridge::sceneWidth() const {
	if (!engineReady() || !_vm->getPicture())
		return TOON_SCREEN_WIDTH;
	return MAX<int>(TOON_SCREEN_WIDTH, _vm->getPicture()->getWidth());
}

int ToonMcpBridge::sceneId() const {
	return engineReady() ? _vm->state()->_currentScene : 0;
}

Common::String ToonMcpBridge::sceneName() const {
	if (!engineReady())
		return Common::String();
	return toonSceneName(_vm->state()->_locations[_vm->state()->_currentScene]._name);
}

// What the leftmost column on screen is, in scene coordinates. A close-up or a
// cutaway is drawn on the second screen of the back buffer, which is why its
// things live a whole back buffer to the right of the room's.
static int viewOrigin(ToonEngine *vm) {
	int origin = vm->state()->_currentScrollValue;
	if (vm->state()->_inCutaway)
		origin += TOON_BACKBUFFER_WIDTH;
	return origin;
}

int ToonMcpBridge::wantedScroll(int x) const {
	State *gs = _vm->state();
	// Only the room itself scrolls; a close-up or a cutaway is one screen wide
	// and the engine pins the view for it.
	if (gs->_inCutaway || gs->_inCloseUp)
		return gs->_currentScrollValue;

	const int maxScroll = MAX(0, sceneWidth() - TOON_SCREEN_WIDTH);
	if (maxScroll == 0)
		return 0;

	const int screenX = x - gs->_currentScrollValue;
	// Leave room around the point: a detection box is bigger than the spot
	// picked in it, and the pointer has to land inside it.
	const int margin = 40;
	if (screenX >= margin && screenX <= TOON_SCREEN_WIDTH - margin)
		return gs->_currentScrollValue;
	return CLIP<int>(x - TOON_SCREEN_WIDTH / 2, 0, maxScroll);
}

// ---------------------------------------------------------------------------
// Targets
// ---------------------------------------------------------------------------

Common::String ToonMcpBridge::hotspotLabel(int index) const {
	HotspotData *hot = _vm->getHotspots()->get(index);
	if (!hot)
		return Common::String();

	int32 item = hot->getData(14);
	if (hot->getType() == 3) {
		// A way out: what it is called is where it leads.
		if (item < 0 || item >= 256)
			return Common::String();
		const char *where = _vm->getLocationString(item, _vm->state()->_locations[item]._visited);
		return where ? toonLabelToName(where) : Common::String();
	}
	if (item <= 0 || item >= 2000)
		return Common::String();
	const char *line = _vm->mcpText(item);
	return line ? toonLabelToName(line) : Common::String();
}

bool ToonMcpBridge::hotspotUsable(int index) const {
	HotspotData *hot = _vm->getHotspots()->get(index);
	if (!hot)
		return false;
	// Mode -1 is not a thing of its own: it is extra clickable area that
	// stands for another box.
	if (hot->getMode() == -1)
		return false;
	// The scripts switch a box off by taking its priority below every other,
	// which is how the game itself stops finding it.
	if (hot->getPriority() < 0)
		return false;
	if (hot->getX2() < hot->getX1() || hot->getY2() < hot->getY1())
		return false;
	// Only what is on the screen the player is looking at.
	const bool cutawayBox = hot->getX1() >= TOON_BACKBUFFER_WIDTH;
	return cutawayBox == _vm->state()->_inCutaway;
}

// Probe one box for a point that a click would resolve to `index`: its middle
// first, then a coarse grid over it.
static bool probeBox(Hotspots *hotspots, HotspotData *box, int index, int &x, int &y) {
	const int left = box->getX1(), right = box->getX2();
	const int top = box->getY1(), bottom = box->getY2();
	if (right < left || bottom < top)
		return false;

	const int midX = (left + right) / 2, midY = (top + bottom) / 2;
	if (hotspots->find(midX, midY) == index) {
		x = midX;
		y = midY;
		return true;
	}
	for (int gy = 1; gy < 4; gy++) {
		for (int gx = 1; gx < 4; gx++) {
			const int px = left + ((right - left) * gx) / 4;
			const int py = top + ((bottom - top) * gy) / 4;
			if (hotspots->find(px, py) == index) {
				x = px;
				y = py;
				return true;
			}
		}
	}
	return false;
}

bool ToonMcpBridge::hotspotClickPoint(int index, int &x, int &y) const {
	Hotspots *hotspots = _vm->getHotspots();
	HotspotData *hot = hotspots->get(index);
	if (!hot)
		return false;

	// Boxes overlap, and the game hands the click to the one with the highest
	// priority. Look for a spot that really resolves to this one, so that
	// acting on it means what pointing at it means.
	if (probeBox(hotspots, hot, index, x, y))
		return true;

	// Failing that, over the boxes that are extra clickable area for it.
	for (int i = 0; i < hotspots->getCount(); i++) {
		HotspotData *box = hotspots->get(i);
		if (!box || box->getMode() != -1 || box->getRef() != index)
			continue;
		if (probeBox(hotspots, box, index, x, y))
			return true;
	}
	return false;
}

void ToonMcpBridge::collectTargets(Common::Array<Target> &out) const {
	out.clear();
	if (!engineReady())
		return;

	State *gs = _vm->state();
	Hotspots *hotspots = _vm->getHotspots();
	for (int i = 0; i < hotspots->getCount(); i++) {
		if (!hotspotUsable(i))
			continue;
		Target t;
		if (!hotspotClickPoint(i, t.x, t.y))
			continue;   // buried under something else; the player cannot reach it either
		HotspotData *hot = hotspots->get(i);
		t.id = i;
		t.isExit = (hot->getType() == 3);
		t.isCharacter = false;
		t.x1 = hot->getX1();
		t.y1 = hot->getY1();
		t.x2 = hot->getX2();
		t.y2 = hot->getY2();
		t.name = hotspotLabel(i);
		if (t.name.empty())
			t.name = toonFallbackName(t.isExit ? "exit" : "object", i);
		out.push_back(t);
	}

	// The sidekick, who is a target in his own right rather than a box.
	if (_vm->getFlux() && _vm->getFlux()->getVisible() && !gs->_inCutaway) {
		int16 x1 = 0, y1 = 0, x2 = 0, y2 = 0;
		if (_vm->getFlux()->getAnimationInstance()) {
			_vm->getFlux()->getAnimationInstance()->getRect(&x1, &y1, &x2, &y2);
			Target t;
			t.id = kThingFlux;
			t.isExit = false;
			t.isCharacter = true;
			t.x1 = x1; t.y1 = y1; t.x2 = x2; t.y2 = y2;
			t.x = (x1 + x2) / 2;
			t.y = (y1 + y2) / 2;
			t.name = toonLabelToName(_vm->_specialInfoLine ? _vm->_specialInfoLine[2] : "");
			if (t.name.empty())
				t.name = toonFallbackName("character", 1);
			out.push_back(t);
		}
	}

	// The bag itself is deliberately not a target: what is in it is reported
	// as the carried items, and clicking it would only open a screen that
	// takes the game away from everything the tools drive.

	Common::Array<Common::String> names;
	for (uint i = 0; i < out.size(); i++)
		names.push_back(out[i].name);
	disambiguate(names);
	for (uint i = 0; i < out.size(); i++)
		out[i].name = names[i];
}

bool ToonMcpBridge::resolveTarget(const Common::String &name, Target &out,
                                  Common::String &errorOut) const {
	Common::String wanted = toonLabelToName(name);
	const int numeric = allDigits(wanted) ? atoi(wanted.c_str()) : -1;

	Common::Array<Target> targets;
	collectTargets(targets);
	for (uint i = 0; i < targets.size(); i++) {
		if (targets[i].name == wanted || (numeric >= 0 && targets[i].id == numeric)) {
			out = targets[i];
			return true;
		}
	}

	Common::String available;
	for (uint i = 0; i < targets.size() && i < 16; i++) {
		if (!available.empty())
			available += ", ";
		available += targets[i].name;
	}
	errorOut = "unknown target '" + name + "'; here: " + available;
	return false;
}

// ---------------------------------------------------------------------------
// Inventory
// ---------------------------------------------------------------------------

void ToonMcpBridge::collectInventory(Common::Array<int> &ids) const {
	ids.clear();
	if (!engineReady())
		return;
	State *gs = _vm->state();
	for (int32 i = 0; i < gs->_numInventoryItems && i < 35; i++)
		if (gs->_inventory[i] > 0)
			ids.push_back(gs->_inventory[i]);
	if (gs->_mouseState > 0) {
		bool known = false;
		for (uint i = 0; i < ids.size(); i++)
			known |= (ids[i] == gs->_mouseState);
		if (!known)
			ids.push_back(gs->_mouseState);
	}
}

// What the game itself says about an item, before two of them are told apart.
// The line the player character speaks when asked about the item is the only
// place the game ever names it, so that is where the name comes from; the
// label of whatever it came out of is the fallback, and a numbered name the
// last resort.
Common::String ToonMcpBridge::rawItemName(int id) const {
	const char *desc = engineReady() ? _vm->mcpText(1000 + id) : nullptr;
	if (desc) {
		Common::String name = toonItemName(MCP::mcpCleanGameText(desc));
		if (!name.empty())
			return name;
	}
	for (uint i = 0; i < _namedItemIds.size(); i++)
		if (_namedItemIds[i] == id && !_namedItemNames[i].empty())
			return _namedItemNames[i];
	return toonFallbackName("item", id);
}

Common::String ToonMcpBridge::itemName(int id) const {
	const Common::String base = rawItemName(id);

	// Two items the game describes the same way are told apart by a suffix,
	// in the order they sit in the bag.
	Common::Array<int> ids;
	collectInventory(ids);
	uint occurrence = 0;
	for (uint i = 0; i < ids.size(); i++) {
		if (ids[i] == id)
			break;
		if (rawItemName(ids[i]) == base)
			occurrence++;
	}
	return toonDisambiguate(base, occurrence);
}

void ToonMcpBridge::nameItem(int32 item, const Common::String &name) {
	if (item <= 0 || name.empty())
		return;
	for (uint i = 0; i < _namedItemIds.size(); i++)
		if (_namedItemIds[i] == item)
			return;   // the first thing it came out of is the one that names it
	_namedItemIds.push_back(item);
	_namedItemNames.push_back(name);
}

void ToonMcpBridge::pumpTransport() {
	// The bag screen reads the mouse itself, so a click is all it takes: one
	// on a spot no item occupies is how a player leaves it.
	if (isEnabled() && engineReady() && _vm->state()->_inInventory) {
		// That screen sees a press only once the button has been up for a
		// pass, so the press and the release go one pass apart.
		const int x = TOON_SCREEN_WIDTH / 2, y = 16;
		moveCursorTo(x, y);
		Common::Event ev;
		ev.mouse = Common::Point(x, y);
		ev.type = _escapePressed ? Common::EVENT_LBUTTONUP : Common::EVENT_LBUTTONDOWN;
		g_system->getEventManager()->pushEvent(ev);
		_escapePressed = !_escapePressed;
	} else {
		_escapePressed = false;
	}
	pumpTransportOnly();
}

void ToonMcpBridge::onItemInHand(int32 item) {
	if (!isEnabled())
		return;
	nameItem(item, _lastClickedName);
}

bool ToonMcpBridge::resolveItem(const Common::String &name, int &id) const {
	Common::String wanted = toonLabelToName(name);
	const int numeric = allDigits(wanted) ? atoi(wanted.c_str()) : -1;

	Common::Array<int> ids;
	collectInventory(ids);
	for (uint i = 0; i < ids.size(); i++) {
		if (itemName(ids[i]) == wanted || (numeric > 0 && ids[i] == numeric)) {
			id = ids[i];
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// Conversations
// ---------------------------------------------------------------------------

void ToonMcpBridge::collectChoices(Common::Array<int> &slots) const {
	slots.clear();
	if (!inConversation())
		return;
	State *gs = _vm->state();
	const int32 conv = gs->_currentConversationId;
	if (conv < 0 || conv >= 60)
		return;
	for (int i = 0; i < 10; i++)
		if (gs->_conversationState[conv].state[i]._data2 == 1)
			slots.push_back(i);
}

Common::String ToonMcpBridge::choiceLabel(int slot) const {
	State *gs = _vm->state();
	const int32 conv = gs->_currentConversationId;
	if (conv < 0 || conv >= 60 || slot < 0 || slot >= 10)
		return Common::String();

	const void *script = gs->_conversationState[conv].state[slot]._data4;
	if (!script)
		return Common::String();

	// The script for an option is a run of (command, argument) pairs, and a
	// command below 100 is "say this line". The first one is what picking the
	// option opens with, which is the closest thing it has to a name. Read it;
	// running it is what answer() is for.
	const char *p = (const char *)script + 2;
	const int16 command = READ_LE_INT16(p);
	if (command < 0 || command >= 100)
		return Common::String();
	const char *line = _vm->mcpText(READ_LE_INT16(p + 2));
	if (!line)
		return Common::String();
	return toonTopicName(MCP::mcpCleanGameText(safeUtf8(line)));
}

void ToonMcpBridge::buildQuestion(Common::JSONObject &question) const {
	Common::Array<int> slots;
	collectChoices(slots);

	Common::Array<Common::String> labels;
	for (uint i = 0; i < slots.size(); i++) {
		Common::String label = choiceLabel(slots[i]);
		if (label.empty())
			label = toonFallbackName("topic", slots[i]);
		labels.push_back(label);
	}
	disambiguate(labels);

	Common::JSONArray out;
	for (uint i = 0; i < slots.size(); i++) {
		Common::JSONObject c;
		c.setVal("id",    mcpJsonInt((int)i + 1));
		c.setVal("label", mcpJsonString(labels[i]));
		// Slot 1 is the one the game always keeps for taking leave.
		if (slots[i] == 1)
			c.setVal("ends_conversation", mcpJsonBool(true));
		out.push_back(new Common::JSONValue(c));
	}
	question.setVal("choices", new Common::JSONValue(out));
}

// ---------------------------------------------------------------------------
// Clicking
// ---------------------------------------------------------------------------

void ToonMcpBridge::queueClick(int x, int y, bool rightButton, int32 wantHeld) {
	_pendingX = x;
	_pendingY = y;
	_pendingRight = rightButton;
	_pendingHeld = wantHeld;
	_pendingScrollTo = wantedScroll(x);
	_pendingPhase = kPhaseHand;
	_pendingFrame = _frameCounter + kScrollFrames;
}

void ToonMcpBridge::applyHand() {
	if (_pendingHeld == kHandLeave)
		return;

	State *gs = _vm->state();
	if (gs->_mouseState == _pendingHeld)
		return;

	// Put back whatever is in hand first - the same thing a right click does.
	if (gs->_mouseState >= 0)
		_vm->addItemToInventory(gs->_mouseState);

	if (_pendingHeld >= 0) {
		// And take the wanted one out, the way the bag screen takes it: some
		// items turn into something else on the way out, and the game decides
		// which.
		for (int32 i = 0; i < gs->_numInventoryItems && i < 35; i++) {
			if (gs->_inventory[i] != _pendingHeld)
				continue;
			const int32 modItem = _vm->getSpecialInventoryItem(_pendingHeld);
			if (modItem == -1) {
				gs->_mouseState = _pendingHeld;
				gs->_inventory[i] = 0;
			} else if (modItem) {
				gs->_mouseState = modItem;
			}
			break;
		}
	}

	if (gs->_mouseState >= 0)
		_vm->setCursor(gs->_mouseState, true, -18, -14);
	else
		_vm->setCursor(0);
}

bool ToonMcpBridge::pumpStreamGameEarly() {
	if (_pendingPhase == kPhaseNone)
		return false;

	// Getting ready counts as activity, so the stream's deadline is not spent
	// waiting for the view to travel.
	_sseLastEventFrame = _frameCounter;

	State *gs = _vm->state();
	switch (_pendingPhase) {
	case kPhaseHand:
		applyHand();
		_pendingPhase = kPhaseScroll;
		return false;

	case kPhaseScroll: {
		if (gs->_currentScrollValue != _pendingScrollTo && _frameCounter < _pendingFrame) {
			// Pin the view and walk it there: the engine only ever moves it to
			// follow the character, and it would pull it straight back.
			pinView();
			const int step = 16;
			if (gs->_currentScrollValue < _pendingScrollTo)
				gs->_currentScrollValue = MIN<int>(_pendingScrollTo, gs->_currentScrollValue + step);
			else
				gs->_currentScrollValue = MAX<int>(_pendingScrollTo, gs->_currentScrollValue - step);
			return false;
		}
		_pendingPhase = kPhasePress;
		return false;
	}

	case kPhasePress: {
		// Hold the view still over the click itself, so what is under the
		// pointer when the engine reads it is what was aimed at.
		pinView();
		const int screenX = CLIP<int>(_pendingX - viewOrigin(_vm), 0, TOON_SCREEN_WIDTH - 1);
		const int screenY = CLIP<int>(_pendingY, 0, TOON_SCREEN_HEIGHT - 1);
		moveCursorTo(screenX, screenY);
		Common::Event down;
		down.type = _pendingRight ? Common::EVENT_RBUTTONDOWN : Common::EVENT_LBUTTONDOWN;
		down.mouse = Common::Point(screenX, screenY);
		g_system->getEventManager()->pushEvent(down);
		_pendingPhase = kPhaseRelease;
		return false;
	}

	case kPhaseRelease: {
		Common::Event up;
		up.type = _pendingRight ? Common::EVENT_RBUTTONUP : Common::EVENT_LBUTTONUP;
		up.mouse = Common::Point(_vm->getMouseX(), _vm->getMouseY());
		g_system->getEventManager()->pushEvent(up);
		_pendingPhase = kPhaseNone;
		return false;
	}

	default:
		break;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Tool: state
// ---------------------------------------------------------------------------

Common::String ToonMcpBridge::stateToolDescription() const {
	return "Returns the current game state: the scene (id + name), where the "
	       "player character stands, everything in the scene that can be "
	       "pointed at (by name), the items carried, the three verbs, the "
	       "lines said since the last read (cleared after reading) and the "
	       "pending conversation question if any. Every objects[] entry "
	       "carries a 'kind' ('character' or 'scenery'), a "
	       "position, and 'pathway' when it leads out of the scene. Objects "
	       "and items can be targeted by 'name' or by 'id' in act(). An item "
	       "that shows as 'held' is the one in hand. While can_act is false, "
	       "act and walk are refused; a question, when one is pending, is "
	       "still answered.";
}

Common::String ToonMcpBridge::actToolDescription() const {
	return "Do something to a named target: 'use' is the action the target "
	       "itself decides on (opening, taking, talking, whatever it is), "
	       "'look_at' is a remark about it, and 'walk_to' only goes there "
	       "without touching it. Give 'target2' as a carried item to do it "
	       "with that item in hand - that is how one thing is used on "
	       "another; without it the hand is emptied first. " +
	       streamingToolNote();
}

Common::String ToonMcpBridge::answerToolDescription() const {
	return "Choose one of the options the current conversation offers, by the "
	       "'id' state.question.choices gave it. Each option is labelled with "
	       "what the player character will open with, and the one marked "
	       "'ends_conversation' takes leave. " + streamingToolNote();
}

Common::String ToonMcpBridge::walkToolDescription() const {
	return "Walk the player character to a point, given in the coordinates "
	       "state reports positions in (clamped to the scene). The character "
	       "goes to the nearest reachable spot, so a point on no walkable "
	       "ground leaves them close by rather than exactly there. " +
	       streamingToolNote();
}

Common::String ToonMcpBridge::skipToolDescription() const {
	return "Cut short whatever is playing itself out: a movie, or the line "
	       "being spoken. Use it when nothing is accepting input and the "
	       "scene is not moving on by itself. " + streamingToolNote();
}

void ToonMcpBridge::augmentStateSchema(Common::JSONObject &outputProps) {
	outputProps.setVal("can_act", mcpProp("boolean",
	    "False while the game is not accepting a new action (a scene playing "
	    "itself out, a movie, a conversation waiting for an answer). act and "
	    "walk are refused then; answering a pending question is not."));

	Common::JSONObject obj;
	obj.setVal("id",      mcpProp("integer", "Identifier to use in act()."));
	obj.setVal("name",    mcpProp("string",  "Name to use in act()."));
	obj.setVal("kind",    mcpProp("string",  "'character' or 'scenery'."));
	obj.setVal("x",       mcpProp("integer", "Where it is, in scene coordinates."));
	obj.setVal("y",       mcpProp("integer", "Where it is, in scene coordinates."));
	obj.setVal("pathway", mcpProp("boolean", "Present when it leads out of the scene."));
	outputProps.setVal("objects", objectArraySchema(obj));

	Common::JSONObject item;
	item.setVal("id",   mcpProp("integer", "Identifier to use in act()."));
	item.setVal("name", mcpProp("string",  "Name to use in act()."));
	item.setVal("held", mcpProp("boolean", "Present when this is the item in hand."));
	outputProps.setVal("inventory", objectArraySchema(item));

	outputProps.setVal("held_item", mcpProp("string",
	    "The item currently in hand, if any. An action with no 'target2' puts "
	    "it away first."));
	outputProps.setVal("verbs", mcpProp("array", "The verbs act() takes."));
}

void ToonMcpBridge::augmentChangesSchema(Common::JSONObject &props) {
	props.setVal("held_item", mcpProp("string",
	    "The item in hand after the action, empty when the hand was emptied."));
	Common::JSONObject changed;
	changed.setVal("name",      mcpProp("string", "The thing that appeared or went."));
	changed.setVal("new_state", mcpProp("string", "'present' or 'gone'."));
	props.setVal("objects_changed", objectArraySchema(changed));
	props.setVal("room_name", mcpProp("string",
	    "Name of the new scene (only present if the scene changed)."));
}

Common::JSONValue *ToonMcpBridge::toolState(const Common::JSONValue &, Common::String &) {
	Common::JSONObject out;

	Common::JSONObject room;
	room.setVal("id", mcpJsonInt(sceneId()));
	Common::String name = sceneName();
	if (!name.empty())
		room.setVal("name", mcpJsonString(name));
	out.setVal("room", new Common::JSONValue(room));

	int px = 0, py = 0;
	leadPos(px, py);
	Common::JSONObject pos;
	pos.setVal("x", mcpJsonInt(px));
	pos.setVal("y", mcpJsonInt(py));
	out.setVal("position", new Common::JSONValue(pos));

	out.setVal("can_act", mcpJsonBool(canAct()));

	Common::JSONArray objects;
	Common::Array<Target> targets;
	collectTargets(targets);
	for (uint i = 0; i < targets.size(); i++) {
		Common::JSONObject o;
		o.setVal("id",   mcpJsonInt(targets[i].id));
		o.setVal("name", mcpJsonString(targets[i].name));
		o.setVal("kind", mcpJsonString(targets[i].isCharacter ? "character"
		                               : "scenery"));
		o.setVal("x",    mcpJsonInt(targets[i].x));
		o.setVal("y",    mcpJsonInt(targets[i].y));
		if (targets[i].isExit)
			o.setVal("pathway", mcpJsonBool(true));
		objects.push_back(new Common::JSONValue(o));
	}
	out.setVal("objects", new Common::JSONValue(objects));

	Common::JSONArray inventory;
	Common::Array<int> ids;
	collectInventory(ids);
	const int held = engineReady() ? _vm->state()->_mouseState : -1;
	for (uint i = 0; i < ids.size(); i++) {
		Common::JSONObject item;
		item.setVal("id",   mcpJsonInt(ids[i]));
		item.setVal("name", mcpJsonString(itemName(ids[i])));
		if (ids[i] == held)
			item.setVal("held", mcpJsonBool(true));
		inventory.push_back(new Common::JSONValue(item));
	}
	out.setVal("inventory", new Common::JSONValue(inventory));
	if (held > 0)
		out.setVal("held_item", mcpJsonString(itemName(held)));

	Common::JSONArray verbs;
	for (uint i = 0; i < ARRAYSIZE(kVerbs); i++)
		verbs.push_back(mcpJsonString(kVerbs[i].name));
	out.setVal("verbs", new Common::JSONValue(verbs));

	// The lines said since the last read, cleared after reading: without this
	// an agent could never see what was said while can_act was false.
	Common::JSONArray messages;
	for (uint i = 0; i < _messages.size(); i++) {
		Common::String text = MCP::mcpCleanGameText(safeUtf8(_messages[i].text));
		if (text.empty())
			continue;
		Common::JSONObject m;
		m.setVal("text", mcpJsonString(text));
		Common::String actor = messageActorName(_messages[i].actorId);
		if (!actor.empty())
			m.setVal("actor", mcpJsonString(actor));
		m.setVal("type", mcpJsonString(_messages[i].type));
		messages.push_back(new Common::JSONValue(m));
	}
	_messages.clear();
	out.setVal("messages", new Common::JSONValue(messages));

	if (hasPendingQuestion()) {
		Common::JSONObject question;
		buildQuestion(question);
		out.setVal("question", new Common::JSONValue(question));
	}

	return new Common::JSONValue(out);
}

// ---------------------------------------------------------------------------
// Tool: act
// ---------------------------------------------------------------------------

bool ToonMcpBridge::toolAct(const Common::JSONValue &args, Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "act: another action is already in progress";
		return false;
	}
	if (hasPendingQuestion()) {
		errorOut = "act: a conversation question is pending - use 'answer' first";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("verb") ||
	    !args.asObject()["verb"]->isString()) {
		errorOut = "act: 'verb' string is required";
		return false;
	}
	const Common::JSONObject &a = args.asObject();

	const VerbName *verb = nullptr;
	Common::String verbName = normalizeActionName(a["verb"]->asString());
	if (!verbFromName(verbName, verb)) {
		errorOut = "act: unknown verb '" + verbName + "'; use use, look_at or walk_to";
		return false;
	}

	auto targetName = [&](const char *key) -> Common::String {
		if (!a.contains(key))
			return Common::String();
		if (a[key]->isString())
			return a[key]->asString();
		if (a[key]->isIntegerNumber())
			return Common::String::format("%d", (int)a[key]->asIntegerNumber());
		return Common::String();
	};
	Common::String name1 = targetName("target1");
	Common::String name2 = targetName("target2");

	if (name1.empty()) {
		errorOut = "act: 'target1' is required";
		return false;
	}

	// Resolve before asking whether the game is listening: what an agent sent
	// is either a thing that exists or it is not, and saying which is wrong is
	// more use than saying "not now" to a call that was malformed anyway.
	//
	// "use X on Y": X may be the carried item and Y the thing in the scene, or
	// the other way round. Whichever of the two is an item goes in hand.
	Target scene;
	int item = 0;
	Common::String sceneName1 = name1;
	if (!name2.empty()) {
		if (resolveItem(name1, item))
			sceneName1 = name2;
		else if (!resolveItem(name2, item)) {
			errorOut = "act: 'target2' must be an item you are carrying";
			return false;
		}
	}
	if (!resolveTarget(sceneName1, scene, errorOut)) {
		errorOut = "act: " + errorOut;
		return false;
	}
	if (item > 0 && verb->rightButton) {
		errorOut = "act: 'look_at' is about the target alone; drop 'target2'";
		return false;
	}

	if (!canAct()) {
		errorOut = "act: the game is not accepting input right now";
		return false;
	}

	// Remember what was aimed at: if the click hands over an object, the thing
	// it came from is the only place its name is written down.
	_lastClickedName = scene.name;
	_skipStream = false;

	if (verb->walkOnly) {
		// Go there without touching it: a click on the ground beside it, which
		// walks and stops, rather than a click on the thing, which would act
		// on it.
		int wx = 0, wy = 0;
		if (!groundBeside(scene, wx, wy)) {
			errorOut = "act: there is no clear ground beside '" + scene.name +
			           "' to stop at; use verb='use' to go and act on it, or "
			           "walk() to a point of your own";
			return false;
		}
		queueClick(wx, wy, false, kHandLeave);
		beginStream();
		return true;
	}

	queueClick(scene.x, scene.y, verb->rightButton, item > 0 ? item : kHandEmpty);
	beginStream();
	return true;
}

// ---------------------------------------------------------------------------
// Walking
// ---------------------------------------------------------------------------

Common::String ToonMcpBridge::coveringName(int x, int y) const {
	// The characters are looked at first: the game tests them before its
	// boxes, so one standing over a box is what a click there would reach.
	Common::Array<Target> targets;
	collectTargets(targets);
	for (uint i = 0; i < targets.size(); i++) {
		const Target &t = targets[i];
		if (t.isCharacter && x >= t.x1 && x <= t.x2 && y >= t.y1 && y <= t.y2)
			return t.name;
	}
	const int32 covered = _vm->getHotspots()->find(x, y);
	if (covered == -1)
		return Common::String();
	for (uint i = 0; i < targets.size(); i++)
		if (targets[i].id == covered)
			return targets[i].name;
	// A box the snapshot does not publish (switched off, or buried under
	// another): name it the way an unnamed one would be named anyway.
	Common::String label = hotspotLabel(covered);
	return label.empty() ? toonFallbackName("object", covered) : label;
}

bool ToonMcpBridge::groundIsClear(int x, int y) const {
	if (x < 0 || y < 0 || x >= sceneWidth() || y >= TOON_SCREEN_HEIGHT)
		return false;
	if (!coveringName(x, y).empty())
		return false;
	int16 wx = 0, wy = 0;
	return _vm->getPathFinding()->findClosestWalkingPoint(x, y, &wx, &wy) != 0;
}

bool ToonMcpBridge::groundBeside(const Target &target, int &x, int &y) const {
	// The spot the thing itself names, when it has one: that is where the game
	// puts the character to act on it, so it is the right place to stop.
	HotspotData *hot = (target.id >= 0) ? _vm->getHotspots()->get(target.id) : nullptr;
	if (hot && hot->getData(5) && groundIsClear(hot->getData(5), hot->getData(6))) {
		x = hot->getData(5);
		y = hot->getData(6);
		return true;
	}

	// Otherwise work outwards from its foot: below it first, which is where
	// the ground in front of something is, then to either side of it.
	static const int kSteps[] = { 4, 12, 24, 40, 60, 90 };
	const int midX = (target.x1 + target.x2) / 2;
	for (uint i = 0; i < ARRAYSIZE(kSteps); i++) {
		const int d = kSteps[i];
		if (groundIsClear(midX, target.y2 + d)) {
			x = midX;
			y = target.y2 + d;
			return true;
		}
		if (groundIsClear(target.x1 - d, target.y2)) {
			x = target.x1 - d;
			y = target.y2;
			return true;
		}
		if (groundIsClear(target.x2 + d, target.y2)) {
			x = target.x2 + d;
			y = target.y2;
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// Tool: walk
// ---------------------------------------------------------------------------

bool ToonMcpBridge::toolWalk(const Common::JSONValue &args, Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "walk: another action is already in progress";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("x") || !args.asObject().contains("y") ||
	    !args.asObject()["x"]->isIntegerNumber() || !args.asObject()["y"]->isIntegerNumber()) {
		errorOut = "walk: integer 'x' and 'y' are required";
		return false;
	}
	if (!canAct()) {
		errorOut = "walk: the game is not accepting input right now";
		return false;
	}
	if (_vm->state()->_inCutaway || _vm->state()->_inCloseUp) {
		errorOut = "walk: this is a scene played out close up, with nowhere to "
		           "walk to; act on what it shows, or skip past it";
		return false;
	}
	int x = CLIP<int>((int)args.asObject()["x"]->asIntegerNumber(), 0, sceneWidth() - 1);
	int y = CLIP<int>((int)args.asObject()["y"]->asIntegerNumber(), 0, TOON_SCREEN_HEIGHT - 1);

	// A click only walks where it lands on nothing: anywhere else the game
	// gives it to the thing that is there and acts on it instead.
	const Common::String covered = coveringName(x, y);
	if (!covered.empty()) {
		const Common::String &what = covered;
		errorOut = Common::String::format(
		    "walk: (%d, %d) is covered by '%s', so going there means acting on "
		    "it - use act(verb='walk_to', target1='%s') to stop beside it, or "
		    "aim at open ground", x, y, what.c_str(), what.c_str());
		return false;
	}

	int16 wx = 0, wy = 0;
	if (!_vm->getPathFinding()->findClosestWalkingPoint(x, y, &wx, &wy)) {
		errorOut = Common::String::format(
		    "walk: (%d, %d) has no reachable ground anywhere near it", x, y);
		return false;
	}

	_lastClickedName.clear();
	_skipStream = false;
	queueClick(x, y, false, kHandLeave);
	beginStream();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: answer
// ---------------------------------------------------------------------------

bool ToonMcpBridge::toolAnswer(const Common::JSONValue &args, Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "answer: another action is already in progress";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("id") ||
	    !args.asObject()["id"]->isIntegerNumber()) {
		errorOut = "answer: integer 'id' is required";
		return false;
	}
	if (!hasPendingQuestion()) {
		errorOut = "answer: no conversation question is pending";
		return false;
	}

	Common::Array<int> slots;
	collectChoices(slots);
	const int id = (int)args.asObject()["id"]->asIntegerNumber();
	if (id < 1 || id > (int)slots.size()) {
		errorOut = Common::String::format("answer: 'id' must be between 1 and %d",
		                                  (int)slots.size());
		return false;
	}

	// The conversation waits for a press over one of the icons it drew, so
	// pick the option the way the player does: point at its icon and press.
	_lastClickedName.clear();
	_skipStream = false;
	_pendingX = kChoiceX + (id - 1) * kChoiceStride + viewOrigin(_vm);
	_pendingY = kChoiceY;
	_pendingRight = false;
	_pendingHeld = kHandLeave;
	_pendingScrollTo = _vm->state()->_currentScrollValue;
	_pendingPhase = kPhasePress;
	_pendingFrame = _frameCounter + kScrollFrames;
	beginStream();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: skip
// ---------------------------------------------------------------------------

bool ToonMcpBridge::toolSkip(const Common::JSONValue &, Common::String &errorOut) {
	if (!_skipToolEnabled) {
		errorOut = "skip: tool is disabled (set mcp_skip_tool=true)";
		return false;
	}
	if (!_vm->state()) {
		errorOut = "skip: the game is still starting up";
		return false;
	}

	// The two things a player can cut short, raised the way their key bindings
	// raise them: a movie stops on the first, a spoken line on the second.
	Common::Event skip;
	skip.type = Common::EVENT_CUSTOM_ENGINE_ACTION_START;
	skip.customType = kActionEscape;
	g_system->getEventManager()->pushEvent(skip);
	skip.customType = kActionStopCurrentVoice;
	g_system->getEventManager()->pushEvent(skip);

	if (!isStreaming()) {
		_skipStream = true;
		beginStream();
	}
	return true;
}

// ---------------------------------------------------------------------------
// Tool dispatch / gating
// ---------------------------------------------------------------------------

Common::JSONValue *ToonMcpBridge::callTool(const Common::String &name,
                                           const Common::JSONValue &args,
                                           Common::String &errorOut) {
	// The bridge is constructed before run() loads the first scene, so a call
	// arriving that early has nothing to read. state is the exception: it
	// answers whatever the game is doing - with can_act false and nothing in
	// it - because it is what an agent reads to find out that much.
	if (name != "state" && !engineReady()) {
		errorOut = name + ": the game is still starting up";
		return nullptr;
	}
	return MCP::McpBridge::callTool(name, args, errorOut);
}

// ---------------------------------------------------------------------------
// Input injection (debug tools)
// ---------------------------------------------------------------------------

void ToonMcpBridge::injectKey(const Common::KeyState &ks) {
	Common::Event event;
	event.type = Common::EVENT_KEYDOWN;
	event.kbd = ks;
	g_system->getEventManager()->pushEvent(event);
	event.type = Common::EVENT_KEYUP;
	g_system->getEventManager()->pushEvent(event);
}

void ToonMcpBridge::injectMouseMove(int x, int y) {
	// Everything the tools speak is in scene coordinates; the cursor lives on
	// the screen, which is a window onto the scene.
	moveCursorTo(x - viewOrigin(_vm), y);
}

// Put the pointer at a screen position, and make sure the game sees it move:
// warping alone leaves nothing in the queue for a backend that draws nowhere.
void ToonMcpBridge::moveCursorTo(int screenX, int screenY) {
	g_system->warpMouse(screenX, screenY);
	Common::Event event;
	event.type = Common::EVENT_MOUSEMOVE;
	event.mouse = Common::Point(screenX, screenY);
	g_system->getEventManager()->pushEvent(event);
}

void ToonMcpBridge::injectMouseClick(int x, int y, const Common::String &button, bool isDouble) {
	injectMouseMove(x, y);
	const int screenX = x - viewOrigin(_vm), screenY = y;
	const bool right = (button == "right");
	Common::Event down, up;
	down.type = right ? Common::EVENT_RBUTTONDOWN : Common::EVENT_LBUTTONDOWN;
	up.type   = right ? Common::EVENT_RBUTTONUP   : Common::EVENT_LBUTTONUP;
	down.mouse = up.mouse = Common::Point(screenX, screenY);
	const int clicks = isDouble ? 2 : 1;
	for (int i = 0; i < clicks; i++) {
		g_system->getEventManager()->pushEvent(down);
		g_system->getEventManager()->pushEvent(up);
	}
}

// ---------------------------------------------------------------------------
// Text capture
// ---------------------------------------------------------------------------

int ToonMcpBridge::currentRoomForMessages() const {
	return sceneId();
}

Common::String ToonMcpBridge::messageActorName(int actorId) const {
	if (actorId < 0 || (uint)actorId >= _messageActors.size())
		return Common::String();
	const int character = _messageActors[actorId];
	// The two the game names for the info line; everyone else by number.
	if ((character == 0 || character == 1) && _vm->_specialInfoLine) {
		Common::String named = toonLabelToName(_vm->_specialInfoLine[character == 0 ? 3 : 2]);
		if (!named.empty())
			return named;
	}
	return toonFallbackName("character", character);
}

void ToonMcpBridge::onSpeech(int32 characterId, const char *text) {
	if (!isEnabled() || !text || !*text)
		return;
	int slot = -1;
	for (uint i = 0; i < _messageActors.size(); i++)
		if (_messageActors[i] == (int)characterId)
			slot = (int)i;
	if (slot < 0) {
		_messageActors.push_back((int)characterId);
		slot = (int)_messageActors.size() - 1;
	}
	onActorLine(slot, Common::String(text));
}

// ---------------------------------------------------------------------------
// Per-frame
// ---------------------------------------------------------------------------

void ToonMcpBridge::pumpGame() {
	// The view is pinned only for as long as a click is being aimed. Put the
	// engine's own setting back as soon as that is over, however the action
	// ended, so a stream that never got to press does not leave the camera
	// stuck.
	if (_scrollPinned && _pendingPhase == kPhaseNone && engineReady()) {
		_vm->state()->_currentScrollLock = _scrollWasLocked;
		_scrollPinned = false;
	}
}

// Hold the view still while a click is aimed, remembering what the game had
// asked for so it can be put back.
void ToonMcpBridge::pinView() {
	if (!_scrollPinned) {
		_scrollWasLocked = _vm->state()->_currentScrollLock;
		_scrollPinned = true;
	}
	_vm->state()->_currentScrollLock = true;
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

void ToonMcpBridge::snapshotPreAction() {
	_sseActionStarted = false;
	_sseTrackScene = sceneId();
	_sseTrackControl = canAct();
	leadPos(_sseTrackPosX, _sseTrackPosY);
	_ssePreScene = sceneId();
	_ssePreRoom = _ssePreScene;
	collectInventory(_ssePreInventory);
	_ssePreInventoryNames.clear();
	for (uint i = 0; i < _ssePreInventory.size(); i++)
		_ssePreInventoryNames.push_back(itemName(_ssePreInventory[i]));
	_ssePreHeld = engineReady() ? _vm->state()->_mouseState : -1;
	Common::Array<Target> targets;
	collectTargets(targets);
	_ssePreTargets.clear();
	for (uint i = 0; i < targets.size(); i++)
		_ssePreTargets.push_back(targets[i].name);
	leadPos(_ssePrePosX, _ssePrePosY);
}

void ToonMcpBridge::pumpStreamTrack() {
	// Only a *change* counts as progress. A condition that merely stays true -
	// the pointer hidden for the length of a scene - would otherwise keep
	// pushing the deadline back for as long as the scene lasts.
	bool moved = false;
	int px = 0, py = 0;
	leadPos(px, py);
	if (px != _sseTrackPosX || py != _sseTrackPosY) {
		_sseTrackPosX = px;
		_sseTrackPosY = py;
		moved = true;
	}
	const int scene = sceneId();
	if (scene != _sseTrackScene) {
		_sseTrackScene = scene;
		moved = true;
	}
	const bool control = canAct();
	if (control != _sseTrackControl) {
		_sseTrackControl = control;
		moved = true;
	}
	if (inConversation())
		moved = true;

	if (moved) {
		_sseActionStarted = true;
		_sseLastEventFrame = _frameCounter;
	}
}

bool ToonMcpBridge::streamRoomChanged() const {
	// Never close on the scene change alone. Arriving somewhere is the start
	// of what the game does about it - the scene walks the player in, and
	// often has something to say - and a stream that stopped at the change
	// would hand back control while all of that was still running, so the
	// next action would be thrown away. The ordinary settle decides instead.
	return false;
}

bool ToonMcpBridge::hasPendingQuestion() const {
	return inConversation();
}

bool ToonMcpBridge::isActionDone() const {
	// The click has not even been made yet.
	if (_pendingPhase != kPhaseNone)
		return false;
	if (!engineReady())
		return false;
	// A skip is one press: it reports what that press did rather than waiting
	// for whatever it cut short to unwind.
	if (_skipStream)
		return _frameCounter - _sseStartFrame >= kSkipFrames;
	// A conversation asking for an answer is a settled state: the stream
	// closes and reports the question.
	if (inConversation())
		return true;
	if (!canAct())
		return false;
	// Give the action a short window to show its first effect before
	// concluding it was a no-op.
	if (!_sseActionStarted && _frameCounter - _sseStartFrame < kNoOpFrames)
		return false;
	return true;
}

Common::JSONObject ToonMcpBridge::buildStateChanges() const {
	Common::JSONObject changes;

	Common::Array<int> ids;
	collectInventory(ids);
	auto contains = [](const Common::Array<int> &arr, int v) -> bool {
		for (uint i = 0; i < arr.size(); i++)
			if (arr[i] == v)
				return true;
		return false;
	};

	Common::JSONArray added;
	for (uint i = 0; i < ids.size(); i++)
		if (!contains(_ssePreInventory, ids[i]))
			added.push_back(mcpJsonString(itemName(ids[i])));
	if (!added.empty())
		changes.setVal("inventory_added", new Common::JSONValue(added));

	Common::JSONArray removed;
	for (uint i = 0; i < _ssePreInventory.size(); i++)
		if (!contains(ids, _ssePreInventory[i]))
			removed.push_back(mcpJsonString(_ssePreInventoryNames[i]));
	if (!removed.empty())
		changes.setVal("inventory_removed", new Common::JSONValue(removed));

	const int held = _vm->state()->_mouseState;
	if (held != _ssePreHeld)
		changes.setVal("held_item", mcpJsonString(held > 0 ? itemName(held) : Common::String()));

	const int scene = sceneId();
	if (scene != _ssePreScene) {
		changes.setVal("room_changed", mcpJsonInt(scene));
		Common::String name = sceneName();
		if (!name.empty())
			changes.setVal("room_name", mcpJsonString(name));
	}

	int px = 0, py = 0;
	leadPos(px, py);
	if (px != _ssePrePosX || py != _ssePrePosY) {
		Common::JSONObject pos;
		pos.setVal("x", mcpJsonInt(px));
		pos.setVal("y", mcpJsonInt(py));
		changes.setVal("position", new Common::JSONValue(pos));
	}

	// Things that appeared or vanished within the same scene: a door that
	// opened, a character who walked off.
	if (scene == _ssePreScene) {
		Common::Array<Target> targets;
		collectTargets(targets);
		auto hasName = [](const Common::Array<Common::String> &arr,
		                  const Common::String &n) -> bool {
			for (uint i = 0; i < arr.size(); i++)
				if (arr[i] == n)
					return true;
			return false;
		};
		Common::Array<Common::String> now;
		for (uint i = 0; i < targets.size(); i++)
			now.push_back(targets[i].name);

		Common::JSONArray changed;
		for (uint i = 0; i < now.size(); i++) {
			if (hasName(_ssePreTargets, now[i]))
				continue;
			Common::JSONObject c;
			c.setVal("name",      mcpJsonString(now[i]));
			c.setVal("new_state", mcpJsonString("present"));
			changed.push_back(new Common::JSONValue(c));
		}
		for (uint i = 0; i < _ssePreTargets.size(); i++) {
			if (hasName(now, _ssePreTargets[i]))
				continue;
			Common::JSONObject c;
			c.setVal("name",      mcpJsonString(_ssePreTargets[i]));
			c.setVal("new_state", mcpJsonString("gone"));
			changed.push_back(new Common::JSONValue(c));
		}
		if (!changed.empty())
			changes.setVal("objects_changed", new Common::JSONValue(changed));
	}

	if (!_sseMessages.empty()) {
		Common::JSONArray messages;
		for (uint i = 0; i < _sseMessages.size(); i++) {
			Common::String text = MCP::mcpCleanGameText(safeUtf8(_sseMessages[i].text));
			if (text.empty())
				continue;
			Common::JSONObject m;
			m.setVal("text", mcpJsonString(text));
			Common::String actor = messageActorName(_sseMessages[i].actorId);
			if (!actor.empty())
				m.setVal("actor", mcpJsonString(actor));
			m.setVal("type", mcpJsonString(_sseMessages[i].type));
			messages.push_back(new Common::JSONValue(m));
		}
		if (!messages.empty())
			changes.setVal("messages", new Common::JSONValue(messages));
	}

	if (hasPendingQuestion()) {
		Common::JSONObject question;
		buildQuestion(question);
		changes.setVal("question", new Common::JSONValue(question));
	}

	return changes;
}

// ---------------------------------------------------------------------------
// Tool: debug
// ---------------------------------------------------------------------------

Common::String ToonMcpBridge::debugToolDescription() const {
	return "Return raw engine state for diagnostics. Sections are selected by "
	       "flag: 'system' (scene, its size and the part of it on screen, "
	       "pointer, what the game currently thinks is being pointed at), "
	       "'objects' (every pointable thing with its raw record), 'items' "
	       "(the carried objects). Defaults to 'system'.";
}

Common::JSONValue *ToonMcpBridge::buildDebugSchema() const {
	Common::JSONObject props;
	props.setVal("system",  mcpProp("boolean", "Include the engine state summary (default true)."));
	props.setVal("objects", mcpProp("boolean", "Include the pointable things in the scene."));
	props.setVal("items",   mcpProp("boolean", "Include the carried objects."));
	return mcpObjectSchema(props);
}

Common::JSONValue *ToonMcpBridge::toolDebug(const Common::JSONValue &args, Common::String &) {
	const bool hasArgs = args.isObject();
	auto flag = [&](const char *key, bool dflt) -> bool {
		if (!hasArgs || !args.asObject().contains(key) || !args.asObject()[key]->isBool())
			return dflt;
		return args.asObject()[key]->asBool();
	};

	State *gs = _vm->state();
	Common::JSONObject out;

	if (flag("system", true)) {
		Common::JSONObject sys;
		sys.setVal("scene",         mcpJsonInt(sceneId()));
		sys.setVal("scene_name",    mcpJsonString(sceneName()));
		sys.setVal("scene_file",    mcpJsonString(gs->_locations[gs->_currentScene]._name));
		sys.setVal("scene_width",   mcpJsonInt(sceneWidth()));
		sys.setVal("view_x",        mcpJsonInt(gs->_currentScrollValue));
		sys.setVal("view_locked",   mcpJsonBool(gs->_currentScrollLock));
		sys.setVal("can_act",       mcpJsonBool(canAct()));
		sys.setVal("mouse_hidden",  mcpJsonBool(gs->_mouseHidden));
		sys.setVal("in_cutaway",    mcpJsonBool(gs->_inCutaway));
		sys.setVal("in_close_up",   mcpJsonBool(gs->_inCloseUp));
		sys.setVal("in_inventory",  mcpJsonBool(gs->_inInventory));
		sys.setVal("in_conversation", mcpJsonBool(gs->_inConversation));
		sys.setVal("conversation_id", mcpJsonInt(gs->_currentConversationId));
		sys.setVal("movie",         mcpJsonBool(_vm->getMoviePlayer() && _vm->getMoviePlayer()->isPlaying()));
		sys.setVal("lead_moving",   mcpJsonBool(leadMoving()));
		sys.setVal("held_item",     mcpJsonInt(gs->_mouseState));
		sys.setVal("pointing_at",   mcpJsonInt(_vm->mcpCurrentHotspotItem()));
		// In scene coordinates, the space everything else here is given in.
		sys.setVal("cursor_x",      mcpJsonInt(_vm->getMouseX() + viewOrigin(_vm)));
		sys.setVal("cursor_y",      mcpJsonInt(_vm->getMouseY()));
		int px = 0, py = 0;
		leadPos(px, py);
		sys.setVal("lead_x",        mcpJsonInt(px));
		sys.setVal("lead_y",        mcpJsonInt(py));
		sys.setVal("chapter",       mcpJsonInt(gs->_currentChapter));
		sys.setVal("frame_counter", mcpJsonInt((int)_frameCounter));
		out.setVal("system", new Common::JSONValue(sys));
	}

	if (flag("objects", false)) {
		Common::JSONArray arr;
		Common::Array<Target> targets;
		collectTargets(targets);
		for (uint i = 0; i < targets.size(); i++) {
			Common::JSONObject o;
			o.setVal("name",  mcpJsonString(targets[i].name));
			o.setVal("id",    mcpJsonInt(targets[i].id));
			o.setVal("exit",  mcpJsonBool(targets[i].isExit));
			o.setVal("x",     mcpJsonInt(targets[i].x));
			o.setVal("y",     mcpJsonInt(targets[i].y));
			o.setVal("x1",    mcpJsonInt(targets[i].x1));
			o.setVal("y1",    mcpJsonInt(targets[i].y1));
			o.setVal("x2",    mcpJsonInt(targets[i].x2));
			o.setVal("y2",    mcpJsonInt(targets[i].y2));
			if (targets[i].id >= 0) {
				HotspotData *hot = _vm->getHotspots()->get(targets[i].id);
				if (hot) {
					o.setVal("priority", mcpJsonInt(hot->getPriority()));
					o.setVal("type",     mcpJsonInt(hot->getType()));
					o.setVal("mode",     mcpJsonInt(hot->getMode()));
					o.setVal("text_id",  mcpJsonInt(hot->getData(14)));
				}
			}
			arr.push_back(new Common::JSONValue(o));
		}
		out.setVal("objects", new Common::JSONValue(arr));
	}

	if (flag("items", false)) {
		Common::JSONArray arr;
		Common::Array<int> ids;
		collectInventory(ids);
		for (uint i = 0; i < ids.size(); i++) {
			Common::JSONObject o;
			o.setVal("id",   mcpJsonInt(ids[i]));
			o.setVal("name", mcpJsonString(itemName(ids[i])));
			o.setVal("held", mcpJsonBool(ids[i] == gs->_mouseState));
			const char *desc = _vm->mcpText(1000 + ids[i]);
			o.setVal("description", mcpJsonString(desc ? MCP::mcpCleanGameText(desc) : Common::String()));
			arr.push_back(new Common::JSONValue(o));
		}
		out.setVal("items", new Common::JSONValue(arr));
	}

	return new Common::JSONValue(out);
}

} // End of namespace Toon
