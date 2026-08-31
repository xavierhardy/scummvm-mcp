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

#include "tinsel/mcp.h"
#include "tinsel/mcp_names.h"

#include "tinsel/actors.h"
#include "tinsel/background.h"
#include "tinsel/bmv.h"
#include "tinsel/cursor.h"
#include "tinsel/dialogs.h"
#include "tinsel/font.h"
#include "tinsel/handle.h"
#include "tinsel/movers.h"
#include "tinsel/pcode.h"
#include "tinsel/polygons.h"
#include "tinsel/scene.h"
#include "tinsel/scroll.h"
#include "tinsel/strres.h"
#include "tinsel/tinsel.h"

#include "common/debug.h"
#include "common/events.h"
#include "common/system.h"

namespace Tinsel {

// Defined in scene.cpp / pdisplay.cpp; both are private to their file but
// every caller reaches them the same way.
extern SCNHANDLE GetSceneHandle();
extern int GetTaggedActor();
extern HPOLYGON GetTaggedPoly();

using Networking::mcpJsonString;
using Networking::mcpJsonInt;
using Networking::mcpJsonBool;
using Networking::mcpProp;
using Networking::mcpObjectSchema;

// The whole vocabulary: which button the player would have pressed.
struct VerbName {
	const char *name;
	PLR_EVENT event;
};
static const VerbName kVerbs[] = {
	{ "walk_to", PLR_WALKTO },
	{ "look_at", PLR_LOOK },
	{ "use",     PLR_ACTION }
};

// Aliases an agent is likely to reach for, folded onto the three real ones.
static bool verbFromName(const Common::String &name, PLR_EVENT &out) {
	Common::String n = name;
	if (n == "interact" || n == "pick_up" || n == "talk_to" || n == "open" ||
	    n == "close" || n == "push" || n == "pull" || n == "give")
		n = "use";
	for (uint i = 0; i < ARRAYSIZE(kVerbs); i++) {
		if (n == kVerbs[i].name) {
			out = kVerbs[i].event;
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
		names[i] = tinselDisambiguate(base[i], seen);
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

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TinselMcpBridge::TinselMcpBridge(TinselEngine *vm)
	: MCP::McpBridge(vm),
	  _vm(vm),
	  _harvestFrame(0),
	  _harvestNext(0),
	  _skipStream(false),
	  _pendingEvent(false),
	  _pendingKind(PLR_NOEVENT),
	  _pendingFrame(0),
	  _pendingX(0),
	  _pendingY(0),
	  _pendingScroll(false),
	  _ssePreScene(0),
	  _ssePreHeld(0),
	  _sseActionStarted(false),
	  _sseTrackScene(0),
	  _sseTrackPosX(0),
	  _sseTrackPosY(0),
	  _sseTrackControl(false) {
}

TinselMcpBridge::~TinselMcpBridge() {
}

TinselMcpBridge *TinselMcpBridge::create(TinselEngine *vm) {
	TinselMcpBridge *bridge = new TinselMcpBridge(vm);
	bridge->init();
	return bridge;
}

// ---------------------------------------------------------------------------
// Engine state helpers
// ---------------------------------------------------------------------------

bool TinselMcpBridge::engineReady() const {
	return _vm->_dialogs && _vm->_actor && _vm->_handle && _vm->_cursor &&
	       _vm->_bg && _vm->_bmv && GetSceneHandle() != 0;
}

bool TinselMcpBridge::inConversation() const {
	return engineReady() && _vm->_dialogs->isConvWindow() && !_vm->_dialogs->convIsHidden();
}

bool TinselMcpBridge::leadMoving() const {
	MOVER *lead = GetMover(_vm->_actor->GetLeadId());
	return lead != nullptr && MoverMoving(lead);
}

bool TinselMcpBridge::canAct() const {
	if (!engineReady())
		return false;
	if (_vm->_bmv->MoviePlaying())
		return false;
	if (inConversation() || _vm->_dialogs->menuActive())
		return false;
	// The inventory or a menu being up means the player is elsewhere; the
	// bridge never opens either, so this only guards against the game doing it.
	if (_vm->_dialogs->inventoryActive() && _vm->_dialogs->whichInventoryOpen() != INV_CONV)
		return false;
	return ControlIsOn() && !leadMoving();
}

void TinselMcpBridge::leadPos(int &x, int &y) const {
	x = y = 0;
	MOVER *lead = GetMover(_vm->_actor->GetLeadId());
	if (lead && MoverIs(lead))
		GetMoverPosition(lead, &x, &y);
	else
		_vm->_actor->GetActorPos(_vm->_actor->GetLeadId(), &x, &y);
}

int TinselMcpBridge::sceneId() const {
	return (int)(GetSceneHandle() >> SCNHANDLE_SHIFT);
}

Common::String TinselMcpBridge::sceneName() const {
	return tinselSceneName(_vm->_handle->GetSceneName(sceneId()));
}

// The point to put the cursor on for an actor: the middle of its box, low
// enough to be inside every shape of tag area the engine uses.
static bool actorPointingSpot(TinselEngine *vm, int ano, int &x, int &y) {
	int left = vm->_actor->GetActorLeft(ano);
	int right = vm->_actor->GetActorRight(ano);
	int top = vm->_actor->GetActorTop(ano);
	int bottom = vm->_actor->GetActorBottom(ano);
	if (right <= left || bottom <= top)
		return false;
	x = (left + right) / 2;
	y = top + ((bottom - top) * 5) / 8;
	return true;
}

// The point to put the cursor on for a tag/exit polygon, in world
// coordinates. Only path polygons get a pseudo-center computed for them, so
// work one out from the corners and fall back to a scan of the enclosing box
// for the odd concave shape. The corners are in the polygon's own space —
// which in the second game is offset from the world — so everything here is
// done there and the offset is added at the end.
static bool polyPointingSpot(HPOLYGON hp, int &x, int &y) {
	int offX = 0, offY = 0;
	GetPolyOffset(hp, &offX, &offY);

	int left = PolyCornerX(hp, 0), right = left;
	int top = PolyCornerY(hp, 0), bottom = top;
	int sumX = 0, sumY = 0;
	for (int c = 0; c < 4; c++) {
		int cx = PolyCornerX(hp, c);
		int cy = PolyCornerY(hp, c);
		sumX += cx;
		sumY += cy;
		left = MIN(left, cx);
		right = MAX(right, cx);
		top = MIN(top, cy);
		bottom = MAX(bottom, cy);
	}
	x = sumX / 4 + offX;
	y = sumY / 4 + offY;
	if (IsInPolygon(x, y, hp))
		return true;

	for (int iy = 1; iy < 8; iy++) {
		for (int ix = 1; ix < 8; ix++) {
			int px = left + ((right - left) * ix) / 8 + offX;
			int py = top + ((bottom - top) * iy) / 8 + offY;
			if (IsInPolygon(px, py, hp)) {
				x = px;
				y = py;
				return true;
			}
		}
	}
	return right > left && bottom > top;
}

void TinselMcpBridge::collectTargets(Common::Array<Target> &out) const {
	out.clear();
	if (!engineReady())
		return;

	char buf[TBUFSZ];
	const int lead = _vm->_actor->GetLeadId();

	// Tagged actors — the people and the animated scenery the game names.
	if (TinselVersion >= 2) {
		for (int ano = 0; (ano = _vm->_actor->NextTaggedActor(ano)) != 0;) {
			if (ano == lead)
				continue;
			Target t;
			if (!actorPointingSpot(_vm, ano, t.x, t.y))
				continue;
			buf[0] = 0;
			SCNHANDLE hTag = _vm->_actor->GetActorTagHandle(ano);
			if (hTag)
				LoadStringRes(hTag, buf, TBUFSZ);
			t.name = tinselLabelToName(buf);
			if (t.name.empty())
				t.name = tinselFallbackName("actor", ano);
			t.id = ano;
			t.isActor = true;
			t.isExit = false;
			out.push_back(t);
		}
	} else {
		_vm->_actor->FirstTaggedActor();
		for (int ano; (ano = _vm->_actor->NextTaggedActor()) != 0;) {
			if (ano == lead)
				continue;
			Target t;
			if (!actorPointingSpot(_vm, ano, t.x, t.y))
				continue;
			buf[0] = 0;
			SCNHANDLE hTag = _vm->_actor->GetActorTag(ano);
			if (hTag)
				LoadStringRes(hTag, buf, TBUFSZ);
			t.name = tinselLabelToName(buf);
			if (t.name.empty())
				t.name = tinselFallbackName("actor", ano);
			t.id = ano;
			t.isActor = true;
			t.isExit = false;
			out.push_back(t);
		}
	}

	// Tag and exit polygons — the scenery. A disabled one has had its type
	// changed to EX_TAG/EX_EXIT, so filtering on the live types is enough.
	for (int i = 0; i < MAX_POLY; i++) {
		HPOLYGON hp = GetPolyHandle(i);
		if (hp == NOPOLY)
			continue;
		PTYPE type = PolyType(hp);
		if (type != TAG && type != EXIT)
			continue;

		int tagX = 0, tagY = 0;
		SCNHANDLE hTag = GetPolyTagHandle(hp);
		if (!hTag)
			GetTagTag(hp, &hTag, &tagX, &tagY);

		buf[0] = 0;
		if (hTag)
			LoadStringRes(hTag, buf, TBUFSZ);

		Target t;
		if (!polyPointingSpot(hp, t.x, t.y))
			continue;
		t.name = tinselLabelToName(buf);
		if (t.name.empty())
			t.name = tinselFallbackName(type == EXIT ? "exit" : "object", i);
		t.id = i;
		t.isActor = false;
		t.isExit = (type == EXIT);
		out.push_back(t);
	}

	Common::Array<Common::String> names;
	for (uint i = 0; i < out.size(); i++)
		names.push_back(out[i].name);
	disambiguate(names);
	for (uint i = 0; i < out.size(); i++)
		out[i].name = names[i];
}

void TinselMcpBridge::collectInventory(Common::Array<int> &ids,
                                       Common::Array<Common::String> &names) const {
	ids.clear();
	names.clear();
	if (!engineReady())
		return;

	Dialogs *dialogs = _vm->_dialogs;
	for (int inv = INV_1; inv <= INV_2; inv++) {
		int count = dialogs->mcpInventoryCount(inv);
		for (int i = 0; i < count; i++) {
			int id = dialogs->mcpInventoryItem(inv, i);
			if (id <= 0)
				continue;
			ids.push_back(id);
			names.push_back(itemName(id));
		}
	}

	// The held item is not listed as a content of the inventory it came from.
	int held = dialogs->whichItemHeld();
	if (held > 0) {
		bool known = false;
		for (uint i = 0; i < ids.size(); i++)
			known |= (ids[i] == held);
		if (!known) {
			ids.push_back(held);
			names.push_back(itemName(held));
		}
	}

	disambiguate(names);
}

void TinselMcpBridge::collectChoices(Common::Array<int> &ids) const {
	ids.clear();
	if (!inConversation())
		return;
	int count = _vm->_dialogs->mcpInventoryCount(INV_CONV);
	for (int i = 0; i < count; i++) {
		int id = _vm->_dialogs->mcpInventoryItem(INV_CONV, i);
		if (id > 0)
			ids.push_back(id);
	}
}

Common::String TinselMcpBridge::itemName(int id) const {
	for (uint i = 0; i < _namedItemIds.size(); i++)
		if (_namedItemIds[i] == id && !_namedItemNames[i].empty())
			return _namedItemNames[i];
	return tinselFallbackName("item", id);
}

// ---------------------------------------------------------------------------
// Name harvesting
// ---------------------------------------------------------------------------

bool TinselMcpBridge::takeObjectName(SCNHANDLE hText, int objectId) {
	for (uint i = 0; i < _harvestPending.size(); i++) {
		if (_harvestPending[i] != objectId)
			continue;

		char buf[TBUFSZ];
		buf[0] = 0;
		if (hText)
			LoadStringRes(hText, buf, TBUFSZ);

		_namedItemIds.push_back(objectId);
		_namedItemNames.push_back(tinselLabelToName(buf));
		_harvestPending.remove_at(i);
		debug(3, "mcp: item %d is '%s'", objectId, _namedItemNames.back().c_str());
		return true;
	}
	return false;
}

void TinselMcpBridge::pumpNameHarvest() {
	if (!engineReady() || _vm->_bmv->MoviePlaying())
		return;

	if (!_harvestPending.empty()) {
		// The scripts had their chance; whatever did not answer has no name to
		// give, so record that and stop asking.
		if (_frameCounter - _harvestFrame < kHarvestGraceFrames)
			return;
		for (uint i = 0; i < _harvestPending.size(); i++) {
			_namedItemIds.push_back(_harvestPending[i]);
			_namedItemNames.push_back(Common::String());
		}
		_harvestPending.clear();
		return;
	}

	// Only while the game is idle. A name script does nothing but answer, but
	// there is no reason to run one in the middle of a scripted sequence.
	if (!ControlIsOn() && !inConversation())
		return;

	// Sweep every object the game defines, not just the ones in hand: an
	// agent should read a name the moment an item appears, and the whole
	// sweep is over within seconds of the game going idle.
	const int count = _vm->_dialogs->mcpObjectCount();
	while (_harvestNext < count && _harvestPending.size() < kHarvestBatch) {
		int id = _vm->_dialogs->mcpObjectIdAt(_harvestNext++);
		if (id <= 0)
			continue;
		bool tried = false;
		for (uint j = 0; j < _namedItemIds.size(); j++)
			tried |= (_namedItemIds[j] == id);
		if (tried)
			continue;
		if (!_vm->_dialogs->mcpRunObjectNameScript(id)) {
			// Nothing to ask: record it as nameless and move on.
			_namedItemIds.push_back(id);
			_namedItemNames.push_back(Common::String());
			continue;
		}
		_harvestPending.push_back(id);
	}
	if (!_harvestPending.empty())
		_harvestFrame = _frameCounter;
}

void TinselMcpBridge::pumpGame() {
	pumpNameHarvest();
}

// ---------------------------------------------------------------------------
// Text capture
// ---------------------------------------------------------------------------

int TinselMcpBridge::actorSlot(int actor) {
	for (uint i = 0; i < _messageActors.size(); i++)
		if (_messageActors[i] == actor)
			return (int)i;
	_messageActors.push_back(actor);
	return (int)_messageActors.size() - 1;
}

void TinselMcpBridge::onSpeech(int actor, const char *text) {
	if (!isEnabled() || !text || !*text)
		return;
	onActorLine(actorSlot(actor), Common::String(text));
}

void TinselMcpBridge::onPrint(const char *text) {
	if (!isEnabled() || !text || !*text)
		return;
	onSystemLine(Common::String(text));
}

Common::String TinselMcpBridge::messageActorName(int actorId) const {
	if (actorId < 0 || (uint)actorId >= _messageActors.size())
		return Common::String();
	int actor = _messageActors[actorId];
	if (!engineReady() || actor <= 0 || actor > _vm->_actor->GetCount())
		return Common::String();

	char buf[TBUFSZ];
	buf[0] = 0;
	SCNHANDLE hTag = (TinselVersion >= 2 && _vm->_actor->IsTaggedActor(actor))
	                     ? _vm->_actor->GetActorTagHandle(actor)
	                     : _vm->_actor->GetActorTag(actor);
	if (hTag)
		LoadStringRes(hTag, buf, TBUFSZ);
	Common::String name = tinselLabelToName(buf);
	if (name.empty())
		name = tinselFallbackName("actor", actor);
	return name;
}

int TinselMcpBridge::currentRoomForMessages() const {
	return engineReady() ? sceneId() : 0;
}

// ---------------------------------------------------------------------------
// Target resolution
// ---------------------------------------------------------------------------

bool TinselMcpBridge::resolveTarget(const Common::String &name, Target &out,
                                    Common::String &errorOut) const {
	Common::String wanted = tinselLabelToName(name);
	int numeric = allDigits(wanted) ? atoi(wanted.c_str()) : -1;

	Common::Array<Target> targets;
	collectTargets(targets);
	// Names first, over the whole scene: a name is unambiguous by construction
	// (duplicates are suffixed), while an id is only unique among its own kind.
	for (uint i = 0; i < targets.size(); i++) {
		if (targets[i].name == wanted) {
			out = targets[i];
			return true;
		}
	}
	for (uint i = 0; numeric >= 0 && i < targets.size(); i++) {
		if (targets[i].id == numeric) {
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

bool TinselMcpBridge::resolveItem(const Common::String &name, int &id) const {
	Common::String wanted = tinselLabelToName(name);
	int numeric = allDigits(wanted) ? atoi(wanted.c_str()) : -1;

	Common::Array<int> ids;
	Common::Array<Common::String> names;
	collectInventory(ids, names);
	for (uint i = 0; i < ids.size(); i++) {
		if (names[i] == wanted || (numeric > 0 && ids[i] == numeric)) {
			id = ids[i];
			return true;
		}
	}
	return false;
}

bool TinselMcpBridge::requestScroll(int x, int y) const {
	const int screenW = SCREEN_WIDTH, screenH = SCREEN_HEIGHT;
	const int bgW = _vm->_bg->BgWidth(), bgH = _vm->_bg->BgHeight();

	int left = 0, top = 0;
	_vm->_bg->PlayfieldGetPos(FIELD_WORLD, &left, &top);

	// How far the view can travel at all. A scene no bigger than the screen
	// never scrolls — the scroll process kills itself in that case.
	const int maxLeft = MAX(0, bgW - screenW);
	const int maxTop = MAX(0, bgH - screenH);

	int wantLeft = left, wantTop = top;
	if (maxLeft > 0 && (x - left < kScrollMargin || x - left > screenW - kScrollMargin))
		wantLeft = CLIP<int>(x - screenW / 2, 0, maxLeft);
	if (maxTop > 0 && (y - top < kScrollMargin || y - top > screenH - kScrollMargin))
		wantTop = CLIP<int>(y - screenH / 2, 0, maxTop);

	if (wantLeft == left && wantTop == top)
		return false;

	_vm->_scroll->ScrollTo(wantLeft, wantTop, 0, 0);
	return true;
}

void TinselMcpBridge::pointAndQueue(int x, int y, PLR_EVENT event) {
	_pendingEvent = true;
	_pendingKind = event;
	_pendingX = x;
	_pendingY = y;

	// The cursor is a screen object, so a target beyond the edge of the
	// screen cannot be pointed at until the view has come to it. Scroll
	// first, then point once the view has arrived.
	_pendingScroll = requestScroll(x, y);
	if (_pendingScroll) {
		_pendingFrame = _frameCounter + kScrollFrames;
		return;
	}

	// Point first: the game resolves a click against whatever its tag process
	// last latched onto, and that only updates once the cursor has been where
	// it is for a cycle.
	_vm->_cursor->SetCursorXY(x, y);
	_pendingFrame = _frameCounter + kPointFrames;
}

// ---------------------------------------------------------------------------
// Tool: state
// ---------------------------------------------------------------------------

Common::String TinselMcpBridge::stateToolDescription() const {
	return "Returns the current game state: the scene (id + name), where the "
	       "player character stands, everything in the scene that can be "
	       "pointed at (people and scenery, by name), the items carried, the "
	       "three verbs, the lines said since the last read (cleared after "
	       "reading) and the pending conversation question if any. Every "
	       "objects[] entry carries a 'kind' ('character' or 'scenery'), a "
	       "position, and 'pathway' when it leads out of the scene. Objects and "
	       "items can be targeted by 'name' or by 'id' in act(). An item that "
	       "shows as 'held' is the one an action will be carried out with. "
	       "Nothing is accepted while can_act is false.";
}

Common::String TinselMcpBridge::actToolDescription() const {
	return "Do something to a named target: 'walk_to' goes there, 'look_at' "
	       "examines it, 'use' is the action the target itself decides on "
	       "(opening, taking, talking, whatever it is). Give 'target2' as a "
	       "carried item to do it with that item in hand — that is how one "
	       "thing is used on another. " + streamingToolNote();
}

Common::String TinselMcpBridge::answerToolDescription() const {
	return "Choose one of the options the current conversation offers, by the "
	       "'id' state.question.choices gave it. The last option always ends "
	       "the conversation. " + streamingToolNote();
}

Common::String TinselMcpBridge::walkToolDescription() const {
	return "Walk the player character to a point, given in the coordinates "
	       "state reports positions in (clamped to the scene). If something "
	       "pointable covers that point, going there means the same as "
	       "act(verb='walk_to') on it. " + streamingToolNote();
}

void TinselMcpBridge::augmentStateSchema(Common::JSONObject &outputProps) {
	outputProps.setVal("can_act", mcpProp("boolean",
	    "False while the game is not accepting a new action (a scene playing "
	    "itself out, a conversation waiting for an answer). act/walk are "
	    "rejected until it turns true again."));
	outputProps.setVal("held_item", mcpProp("string",
	    "The item currently in hand, which every action is carried out with. "
	    "Absent when nothing is held."));

	{
		Common::JSONObject props;
		props.setVal("id",   mcpProp("integer", "Target id; usable as an act() target."));
		props.setVal("name", mcpProp("string",  "Name, as act() expects it."));
		props.setVal("kind", mcpProp("string",  "'character' or 'scenery'."));
		props.setVal("x",    mcpProp("integer", "X coordinate."));
		props.setVal("y",    mcpProp("integer", "Y coordinate."));
		props.setVal("pathway", mcpProp("boolean",
		    "Present and true when going there leaves the scene."));
		outputProps.setVal("objects", objectArraySchema(props));
	}
	{
		Common::JSONObject props;
		props.setVal("id",   mcpProp("integer", "Item id; usable as an act() target."));
		props.setVal("name", mcpProp("string",  "Item name, as act() expects it."));
		props.setVal("held", mcpProp("boolean",
		    "Present and true for the item currently in hand."));
		outputProps.setVal("inventory", objectArraySchema(props));
	}
	{
		Common::JSONObject props;
		props.setVal("text",  mcpProp("string", "Line of game text."));
		props.setVal("actor", mcpProp("string", "Who said it (absent for narration)."));
		props.setVal("type",  mcpProp("string", "'actor', 'system' or 'dialog'."));
		outputProps.setVal("messages", objectArraySchema(props));
	}
}

void TinselMcpBridge::augmentChangesSchema(Common::JSONObject &props) {
	props.setVal("held_item", mcpProp("string",
	    "The item in hand after the action, when it changed. Empty string when "
	    "the hand was emptied."));
	props.setVal("room_name", mcpProp("string",
	    "Name of the new scene (only present if the scene changed)."));
}

// The conversation options as (1-based id, label) pairs, with the last one
// always being the way out of the conversation.
void TinselMcpBridge::buildQuestion(Common::JSONObject &question) const {
	Common::Array<int> choices;
	collectChoices(choices);
	Common::Array<Common::String> labels;
	for (uint i = 0; i < choices.size(); i++)
		labels.push_back(itemName(choices[i]));
	disambiguate(labels);

	Common::JSONArray out;
	for (uint i = 0; i < choices.size(); i++) {
		Common::JSONObject c;
		c.setVal("id", mcpJsonInt((int)i + 1));
		c.setVal("label", mcpJsonString(labels[i]));
		out.push_back(new Common::JSONValue(c));
	}
	Common::JSONObject end;
	end.setVal("id", mcpJsonInt((int)choices.size() + 1));
	end.setVal("label", mcpJsonString("end_conversation"));
	out.push_back(new Common::JSONValue(end));
	question.setVal("choices", new Common::JSONValue(out));
}

Common::JSONValue *TinselMcpBridge::toolState(const Common::JSONValue &, Common::String &) {
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
		o.setVal("kind", mcpJsonString(targets[i].isActor ? "character" : "scenery"));
		o.setVal("x",    mcpJsonInt(targets[i].x));
		o.setVal("y",    mcpJsonInt(targets[i].y));
		if (targets[i].isExit)
			o.setVal("pathway", mcpJsonBool(true));
		objects.push_back(new Common::JSONValue(o));
	}
	out.setVal("objects", new Common::JSONValue(objects));

	Common::JSONArray inventory;
	Common::Array<int> ids;
	Common::Array<Common::String> names;
	collectInventory(ids, names);
	const int held = _vm->_dialogs->whichItemHeld();
	for (uint i = 0; i < ids.size(); i++) {
		Common::JSONObject item;
		item.setVal("id",   mcpJsonInt(ids[i]));
		item.setVal("name", mcpJsonString(names[i]));
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

bool TinselMcpBridge::toolAct(const Common::JSONValue &args, Common::String &errorOut) {
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

	PLR_EVENT event = PLR_NOEVENT;
	Common::String verbName = normalizeActionName(a["verb"]->asString());
	if (!verbFromName(verbName, event)) {
		errorOut = "act: unknown verb '" + verbName + "'; use walk_to, look_at or use";
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

	if (!canAct()) {
		errorOut = "act: the game is not accepting input right now";
		return false;
	}

	if (item > 0)
		_vm->_dialogs->holdItem(item);

	_skipStream = false;
	pointAndQueue(scene.x, scene.y, event);
	beginStream();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: walk
// ---------------------------------------------------------------------------

bool TinselMcpBridge::toolWalk(const Common::JSONValue &args, Common::String &errorOut) {
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
	int x = (int)args.asObject()["x"]->asIntegerNumber();
	int y = (int)args.asObject()["y"]->asIntegerNumber();
	// The tool advertises out-of-bounds coordinates as clamped: the scene is
	// usually wider than the screen, and the cursor cannot leave it.
	x = CLIP<int>(x, 0, MAX(0, _vm->_bg->BgWidth() - 1));
	y = CLIP<int>(y, 0, MAX(0, _vm->_bg->BgHeight() - 1));

	_skipStream = false;
	pointAndQueue(x, y, PLR_WALKTO);
	beginStream();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: answer
// ---------------------------------------------------------------------------

bool TinselMcpBridge::toolAnswer(const Common::JSONValue &args, Common::String &errorOut) {
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

	Common::Array<int> choices;
	collectChoices(choices);
	int id = (int)args.asObject()["id"]->asIntegerNumber();
	if (id < 1 || id > (int)choices.size() + 1) {
		errorOut = Common::String::format("answer: 'id' must be between 1 and %d",
		                                  (int)choices.size() + 1);
		return false;
	}

	// The window's own indices are what convAction() takes; the extra last
	// option is the game's "close" icon, which ends the conversation.
	_skipStream = false;
	_vm->_dialogs->convAction(id == (int)choices.size() + 1 ? INV_CLOSEICON : id - 1);
	beginStream();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: skip
// ---------------------------------------------------------------------------

bool TinselMcpBridge::toolSkip(const Common::JSONValue &, Common::String &errorOut) {
	if (!_skipToolEnabled) {
		errorOut = "skip: tool is disabled (set mcp_skip_tool=true)";
		return false;
	}
	if (!engineReady()) {
		errorOut = "skip: the game is still starting up";
		return false;
	}
	// Escape is what cuts an escapable sequence short, and what a movie stops on.
	ProcessKeyEvent(PLR_ESCAPE);
	if (!isStreaming()) {
		_skipStream = true;
		beginStream();
	}
	return true;
}

// ---------------------------------------------------------------------------
// Tool dispatch / gating
// ---------------------------------------------------------------------------

Common::JSONValue *TinselMcpBridge::callTool(const Common::String &name,
                                             const Common::JSONValue &args,
                                             Common::String &errorOut) {
	// The bridge is constructed before TinselEngine::run() builds the
	// subsystems and loads the first scene, so a call arriving that early has
	// nothing to read.
	if (!engineReady()) {
		errorOut = name + ": the game is still starting up";
		return nullptr;
	}
	return MCP::McpBridge::callTool(name, args, errorOut);
}

// ---------------------------------------------------------------------------
// Input injection (debug tools)
// ---------------------------------------------------------------------------

void TinselMcpBridge::injectKey(const Common::KeyState &ks) {
	Common::Event event;
	event.type = Common::EVENT_KEYDOWN;
	event.kbd = ks;
	g_system->getEventManager()->pushEvent(event);
	event.type = Common::EVENT_KEYUP;
	g_system->getEventManager()->pushEvent(event);
}

void TinselMcpBridge::injectMouseMove(int x, int y) {
	if (engineReady())
		_vm->_cursor->SetCursorXY(x, y);
}

void TinselMcpBridge::injectMouseClick(int x, int y, const Common::String &button, bool isDouble) {
	injectMouseMove(x, y);
	// The engine reads its button queue from its own mouse process, so a click
	// is queued there rather than pushed through the backend.
	bool right = (button == "right");
	int clicks = isDouble ? 2 : 1;
	for (int i = 0; i < clicks; i++) {
		_vm->_mouseButtons.push_back(right ? Common::EVENT_RBUTTONDOWN : Common::EVENT_LBUTTONDOWN);
		_vm->_mouseButtons.push_back(right ? Common::EVENT_RBUTTONUP : Common::EVENT_LBUTTONUP);
	}
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

void TinselMcpBridge::snapshotPreAction() {
	_sseActionStarted = false;
	_sseTrackScene = sceneId();
	_sseTrackControl = ControlIsOn();
	leadPos(_sseTrackPosX, _sseTrackPosY);
	_ssePreScene = sceneId();
	_ssePreRoom = _ssePreScene;
	collectInventory(_ssePreInventory, _ssePreInventoryNames);
	_ssePreHeld = _vm->_dialogs->whichItemHeld();
	Common::Array<Target> targets;
	collectTargets(targets);
	_ssePreTargets.clear();
	for (uint i = 0; i < targets.size(); i++)
		_ssePreTargets.push_back(targets[i].name);
	leadPos(_ssePrePosX, _ssePrePosY);
}

bool TinselMcpBridge::pumpStreamGameEarly() {
	if (_pendingEvent && _pendingScroll) {
		// The view is on its way to the target. Count it as activity so the
		// stream's deadline is not spent travelling, and point as soon as it
		// has arrived (or given up, for a destination it cannot reach).
		if (!_vm->_scroll->IsScrolling() || _frameCounter >= _pendingFrame) {
			_pendingScroll = false;
			_vm->_cursor->SetCursorXY(_pendingX, _pendingY);
			_pendingFrame = _frameCounter + kPointFrames;
		}
		_sseLastEventFrame = _frameCounter;
		return false;
	}
	if (_pendingEvent && _frameCounter >= _pendingFrame) {
		_pendingEvent = false;
		// The same entry point the keyboard bindings use: it reads the cursor
		// we parked on the target and raises the player event there.
		ProcessKeyEvent(_pendingKind);
		_sseLastEventFrame = _frameCounter;
	}
	return false;
}

void TinselMcpBridge::pumpStreamTrack() {
	// Only a *change* counts as progress. A condition that merely stays true —
	// control off for the length of a cutscene — would otherwise keep pushing
	// the deadline back for as long as the cutscene lasts.
	bool moved = false;
	int px = 0, py = 0;
	leadPos(px, py);
	if (px != _sseTrackPosX || py != _sseTrackPosY) {
		_sseTrackPosX = px;
		_sseTrackPosY = py;
		moved = true;
	}
	int scene = sceneId();
	if (scene != _sseTrackScene) {
		_sseTrackScene = scene;
		moved = true;
	}
	bool control = ControlIsOn();
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

bool TinselMcpBridge::streamRoomChanged() const {
	return engineReady() && sceneId() != _ssePreScene && ControlIsOn() && !leadMoving();
}

bool TinselMcpBridge::hasPendingQuestion() const {
	return inConversation();
}

bool TinselMcpBridge::isActionDone() const {
	// The event has not even been raised yet.
	if (_pendingEvent)
		return false;
	if (!engineReady())
		return false;
	// A skip is one press of Escape: it reports what that press did rather
	// than waiting for control, which the game may not hand back for several
	// scenes yet.
	if (_skipStream)
		return _frameCounter - _sseStartFrame >= kSkipFrames;
	// A conversation asking for an answer is a settled state: the stream
	// closes and reports the question.
	if (inConversation())
		return true;
	if (_vm->_bmv->MoviePlaying())
		return false;
	if (leadMoving() || !ControlIsOn())
		return false;
	// Give the action a short window to show its first effect before
	// concluding it was a no-op.
	if (!_sseActionStarted && _frameCounter - _sseStartFrame < kNoOpFrames)
		return false;
	return true;
}

Common::JSONObject TinselMcpBridge::buildStateChanges() const {
	Common::JSONObject changes;

	Common::Array<int> ids;
	Common::Array<Common::String> names;
	collectInventory(ids, names);
	auto contains = [](const Common::Array<int> &arr, int v) -> bool {
		for (uint i = 0; i < arr.size(); i++)
			if (arr[i] == v)
				return true;
		return false;
	};

	Common::JSONArray added;
	for (uint i = 0; i < ids.size(); i++)
		if (!contains(_ssePreInventory, ids[i]))
			added.push_back(mcpJsonString(names[i]));
	if (!added.empty())
		changes.setVal("inventory_added", new Common::JSONValue(added));

	Common::JSONArray removed;
	for (uint i = 0; i < _ssePreInventory.size(); i++)
		if (!contains(ids, _ssePreInventory[i]))
			removed.push_back(mcpJsonString(_ssePreInventoryNames[i]));
	if (!removed.empty())
		changes.setVal("inventory_removed", new Common::JSONValue(removed));

	const int held = _vm->_dialogs->whichItemHeld();
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

		Common::JSONArray objChanges;
		for (uint i = 0; i < now.size(); i++) {
			if (!hasName(_ssePreTargets, now[i])) {
				Common::JSONObject c;
				c.setVal("name", mcpJsonString(now[i]));
				c.setVal("old_state", mcpJsonString("absent"));
				c.setVal("new_state", mcpJsonString("present"));
				objChanges.push_back(new Common::JSONValue(c));
			}
		}
		for (uint i = 0; i < _ssePreTargets.size(); i++) {
			if (!hasName(now, _ssePreTargets[i])) {
				Common::JSONObject c;
				c.setVal("name", mcpJsonString(_ssePreTargets[i]));
				c.setVal("old_state", mcpJsonString("present"));
				c.setVal("new_state", mcpJsonString("absent"));
				objChanges.push_back(new Common::JSONValue(c));
			}
		}
		if (!objChanges.empty())
			changes.setVal("objects_changed", new Common::JSONValue(objChanges));
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

Common::String TinselMcpBridge::debugToolDescription() const {
	return "Return raw engine state for diagnostics. Sections are selected by "
	       "flag: 'system' (scene, its size and the part of it on screen, "
	       "control, cursor, what the game currently thinks is being pointed "
	       "at), 'objects' (every pointable thing with "
	       "its raw record), 'items' (the inventories and their harvested "
	       "names). Defaults to 'system'.";
}

Common::JSONValue *TinselMcpBridge::buildDebugSchema() const {
	Common::JSONObject props;
	props.setVal("system",  mcpProp("boolean", "Include the engine state summary (default true)."));
	props.setVal("objects", mcpProp("boolean", "Include the pointable things in the scene."));
	props.setVal("items",   mcpProp("boolean", "Include the inventories."));
	return mcpObjectSchema(props);
}

Common::JSONValue *TinselMcpBridge::toolDebug(const Common::JSONValue &args, Common::String &) {
	const bool hasArgs = args.isObject();
	auto flag = [&](const char *key, bool dflt) -> bool {
		if (!hasArgs || !args.asObject().contains(key) || !args.asObject()[key]->isBool())
			return dflt;
		return args.asObject()[key]->asBool();
	};

	Common::JSONObject out;

	if (flag("system", true)) {
		Common::JSONObject sys;
		sys.setVal("scene",        mcpJsonInt(sceneId()));
		sys.setVal("scene_name",   mcpJsonString(sceneName()));
		sys.setVal("scene_file",   mcpJsonString(_vm->_handle->GetSceneName(sceneId())));
		sys.setVal("version",      mcpJsonInt((int)TinselVersion));
		sys.setVal("control",      mcpJsonBool(ControlIsOn()));
		sys.setVal("can_act",      mcpJsonBool(canAct()));
		sys.setVal("lead_moving",  mcpJsonBool(leadMoving()));
		sys.setVal("movie",        mcpJsonBool(_vm->_bmv->MoviePlaying()));
		sys.setVal("conversation", mcpJsonBool(inConversation()));
		sys.setVal("inventory_open", mcpJsonInt(_vm->_dialogs->whichInventoryOpen()));
		sys.setVal("held_item",    mcpJsonInt(_vm->_dialogs->whichItemHeld()));
		sys.setVal("tagged_actor", mcpJsonInt(GetTaggedActor()));
		sys.setVal("tagged_poly",  mcpJsonInt((int)GetTaggedPoly()));
		int cx = 0, cy = 0;
		_vm->_cursor->GetCursorXYNoWait(&cx, &cy, true);
		sys.setVal("cursor_x",     mcpJsonInt(cx));
		sys.setVal("cursor_y",     mcpJsonInt(cy));
		int px = 0, py = 0;
		leadPos(px, py);
		sys.setVal("lead_x",       mcpJsonInt(px));
		sys.setVal("lead_y",       mcpJsonInt(py));
		int viewX = 0, viewY = 0;
		_vm->_bg->PlayfieldGetPos(FIELD_WORLD, &viewX, &viewY);
		sys.setVal("view_x",       mcpJsonInt(viewX));
		sys.setVal("view_y",       mcpJsonInt(viewY));
		sys.setVal("scene_width",  mcpJsonInt(_vm->_bg->BgWidth()));
		sys.setVal("scene_height", mcpJsonInt(_vm->_bg->BgHeight()));
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
			o.setVal("actor", mcpJsonBool(targets[i].isActor));
			o.setVal("exit",  mcpJsonBool(targets[i].isExit));
			o.setVal("x",     mcpJsonInt(targets[i].x));
			o.setVal("y",     mcpJsonInt(targets[i].y));
			arr.push_back(new Common::JSONValue(o));
		}
		out.setVal("objects", new Common::JSONValue(arr));
	}

	if (flag("items", false)) {
		Common::JSONArray arr;
		Common::Array<int> ids;
		Common::Array<Common::String> names;
		collectInventory(ids, names);
		for (uint i = 0; i < ids.size(); i++) {
			Common::JSONObject o;
			o.setVal("id",   mcpJsonInt(ids[i]));
			o.setVal("name", mcpJsonString(names[i]));
			o.setVal("held", mcpJsonBool(ids[i] == _vm->_dialogs->whichItemHeld()));
			arr.push_back(new Common::JSONValue(o));
		}
		out.setVal("items", new Common::JSONValue(arr));
	}

	return new Common::JSONValue(out);
}

} // End of namespace Tinsel
