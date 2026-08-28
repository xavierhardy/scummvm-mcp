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

#include "common/debug.h"
#include "common/events.h"
#include "common/system.h"

#include "sword2/sword2.h"
#include "sword2/defs.h"
#include "sword2/header.h"
#include "sword2/logic.h"
#include "sword2/mcp.h"
#include "sword2/mcp_names.h"
#include "sword2/memory.h"
#include "sword2/mouse.h"
#include "sword2/resman.h"
#include "sword2/screen.h"
#include "sword2/sound.h"

namespace Sword2 {

using Networking::mcpJsonString;
using Networking::mcpJsonInt;
using Networking::mcpJsonBool;
using Networking::mcpProp;
using Networking::mcpObjectSchema;

// The verbs the bridge accepts, and the words state.verbs advertises. The game
// has no verb bar: a left click runs a thing's interaction script and a right
// click looks at it, so every verb but look_at maps onto the left-click path.
// They are still listed separately because an agent reasons in verbs — and
// because these are the same six the first game's bridge accepts, so one way of
// asking works across both.
static const char *const kVerbs[] = {
	"look_at", "interact", "use", "talk_to", "pick_up", "walk_to", nullptr
};

static bool isLookVerb(const Common::String &verb) {
	return verb == "look_at";
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

Sword2McpBridge::Sword2McpBridge(Sword2Engine *vm)
	: MCP::McpBridge(vm),
	  _vm(vm),
	  _inventoryFrame(0),
	  _skipStream(false),
	  _ssePreLocation(0),
	  _ssePreBackground(0),
	  _ssePreHeld(0),
	  _sseActionStarted(false),
	  _sseTrackPosX(0),
	  _sseTrackPosY(0),
	  _sseTrackLocation(0),
	  _sseTrackCanAct(false) {
}

Sword2McpBridge::~Sword2McpBridge() {
}

Sword2McpBridge *Sword2McpBridge::create(Sword2Engine *vm) {
	Sword2McpBridge *bridge = new Sword2McpBridge(vm);
	bridge->init();
	return bridge;
}

// ---------------------------------------------------------------------------
// Engine state helpers
// ---------------------------------------------------------------------------

bool Sword2McpBridge::engineReady() const {
	// _logic->_scriptVars is only there once the globals resource is open,
	// which is what starting a game does.
	return _vm->_logic && _vm->_mouse && _vm->_resman && _vm->_screen &&
	       _vm->_logic->_scriptVars != nullptr;
}

uint32 Sword2McpBridge::location() const {
	return _vm->_logic->readVar(LOCATION);
}

uint32 Sword2McpBridge::backgroundLayer() const {
	return _vm->_screen->getScreenInfo()->background_layer_id;
}

Common::String Sword2McpBridge::locationName() const {
	// LOCATION is a screen number the scripts keep for themselves; the thing
	// that actually carries a name is the background layer the screen is
	// drawn from.
	return resourceName(backgroundLayer());
}

bool Sword2McpBridge::screenChangeSettled() const {
	return (int)location() != _ssePreLocation && backgroundLayer() != _ssePreBackground;
}

bool Sword2McpBridge::speaking() const {
	return _vm->_logic->_speechTextBlocNo != 0 ||
	       (_vm->_sound && _vm->_sound->getSpeechStatus() == RDERR_SPEECHPLAYING);
}

bool Sword2McpBridge::canAct() const {
	if (!engineReady() || location() == 0)
		return false;
	// The scripts turn the mouse off for the whole of a cutaway, a fade or any
	// sequence the player is not meant to interrupt.
	if (!_vm->_logic->readVar(MOUSE_AVAILABLE))
		return false;
	// A conversation waiting for a subject is answered, not acted on.
	if (_vm->_mouse->mcpChoosing())
		return false;
	return !speaking();
}

void Sword2McpBridge::playerPos(int &x, int &y) const {
	x = (int)_vm->_logic->readVar(PLAYER_FEET_X);
	y = (int)_vm->_logic->readVar(PLAYER_FEET_Y);
}

Common::String Sword2McpBridge::resourceName(uint32 id) const {
	if (!id || !_vm->_resman->mcpIsAvailable(id))
		return Common::String();
	byte buf[NAME_LEN + 1];
	memset(buf, 0, sizeof(buf));
	_vm->_resman->fetchName(id, buf);
	buf[NAME_LEN] = 0;
	return sword2CleanName(Common::String((const char *)buf));
}

Common::String Sword2McpBridge::textLine(int32 textId) const {
	if (!textId)
		return Common::String();
	uint32 textRes = (uint32)textId / SIZE;
	uint32 localText = (uint32)textId & 0xffff;
	if (!_vm->_resman->mcpIsAvailable(textRes))
		return Common::String();
	byte *file = _vm->_resman->openResource(textRes);
	if (!file)
		return Common::String();
	byte *line = _vm->fetchTextLine(file, localText);
	// The first two bytes are the line reference number, not the text.
	Common::String out = line ? Common::String((const char *)line + 2) : Common::String();
	_vm->_resman->closeResource(textRes);
	return out;
}

Common::String Sword2McpBridge::nameForObject(uint32 id, int32 pointerText) const {
	// The label the game itself would paint next to the cursor. It is there in
	// the mouse box whether or not the object-labels option is on: the option
	// only decides whether a player gets to see it.
	Common::String label = sword2CleanName(textLine(pointerText));
	if (!label.empty())
		return label;
	// Nothing to show the player, but the resource still carries the name its
	// authors gave it, which is what the game's own debug output uses.
	Common::String authored = resourceName(id);
	if (!authored.empty())
		return authored;
	return sword2FallbackName((int32)id);
}

void Sword2McpBridge::collectHotspots(Common::Array<Hotspot> &out) const {
	out.clear();
	if (!engineReady())
		return;

	const uint32 count = _vm->_mouse->mcpHotspotCount();
	for (uint32 i = 0; i < count; i++) {
		const MouseUnit &unit = _vm->_mouse->mcpHotspot(i);
		if (!unit.id)
			continue;
		Hotspot h;
		h.id = (uint32)unit.id;
		h.kind = sword2PointerKind(unit.pointer);
		h.isExit = sword2PointerIsExit(unit.pointer);
		h.isFloor = sword2PointerIsFloor(unit.pointer);
		h.priority = (int)unit.priority;
		h.x1 = unit.rect.left;
		h.y1 = unit.rect.top;
		h.x2 = unit.rect.right - 1;
		h.y2 = unit.rect.bottom - 1;
		h.name = nameForObject(h.id, unit.pointer_text);
		out.push_back(h);
	}

	// Two things can carry the same label; suffix the later ones so every name
	// resolves to exactly one of them.
	Common::Array<Common::String> base;
	for (uint i = 0; i < out.size(); i++)
		base.push_back(out[i].name);
	for (uint i = 0; i < out.size(); i++) {
		uint seen = 0;
		for (uint j = 0; j < i; j++)
			if (base[j] == base[i])
				seen++;
		out[i].name = sword2Disambiguate(base[i], seen);
	}
}

void Sword2McpBridge::refreshInventory() {
	_inventory.clear();
	if (!engineReady())
		return;

	// The demo ships only some of the game's clusters, and the object the
	// inventory script lives in may not be one of them.
	if (!_vm->_resman->mcpIsAvailable(MENU_MASTER_OBJECT))
		return;

	int32 icons[TOTAL_engine_pockets];
	int32 luggage[TOTAL_engine_pockets];
	uint32 count = _vm->_mouse->mcpBuildInventory(icons, luggage, TOTAL_engine_pockets);

	Common::Array<Common::String> names;
	for (uint32 i = 0; i < count; i++) {
		Item item;
		item.icon = icons[i];
		item.luggage = luggage[i];
		item.name = resourceName((uint32)icons[i]);
		if (item.name.empty())
			item.name = sword2FallbackName(icons[i]);
		names.push_back(item.name);
		_inventory.push_back(item);
	}
	for (uint i = 0; i < _inventory.size(); i++) {
		uint seen = 0;
		for (uint j = 0; j < i; j++)
			if (names[j] == names[i])
				seen++;
		_inventory[i].name = sword2Disambiguate(names[i], seen);
	}
}

void Sword2McpBridge::pumpGame() {
	// Rebuilding the inventory runs one of the game's own scripts, so it only
	// ever happens here — once per game cycle, from the main loop, where the
	// logic session has just finished. Doing it from a tool call would re-enter
	// the interpreter from inside whatever loop the server was pumped from.
	if (!engineReady() || location() == 0)
		return;

	// Every cycle while an action is in flight, so what the action reports
	// having picked up is current; on a slow tick otherwise.
	const bool due = isStreaming() ||
	                 (_frameCounter - _inventoryFrame >= kInventoryRefreshFrames);
	if (due) {
		_inventoryFrame = _frameCounter;
		refreshInventory();
	}
}

// ---------------------------------------------------------------------------
// Text capture
// ---------------------------------------------------------------------------

void Sword2McpBridge::onSpeech(uint32 id, const char *text) {
	if (!isEnabled() || !text || !*text)
		return;
	int slot = -1;
	for (uint i = 0; i < _messageActors.size(); i++)
		if (_messageActors[i] == id) {
			slot = (int)i;
			break;
		}
	if (slot < 0) {
		_messageActors.push_back(id);
		slot = (int)_messageActors.size() - 1;
	}
	onActorLine(slot, Common::String(text));
}

Common::String Sword2McpBridge::messageActorName(int actorId) const {
	if (actorId < 0 || (uint)actorId >= _messageActors.size() || !engineReady())
		return Common::String();
	return resourceName(_messageActors[actorId]);
}

int Sword2McpBridge::currentRoomForMessages() const {
	return engineReady() ? (int)location() : 0;
}

// ---------------------------------------------------------------------------
// Target resolution and click injection
// ---------------------------------------------------------------------------

bool Sword2McpBridge::resolveTarget(const Common::String &name, Hotspot &hotspot,
                                    bool &isHotspot, Item &item,
                                    Common::String &errorOut) const {
	isHotspot = false;
	Common::String wanted = sword2CleanName(name);
	int32 numeric = allDigits(wanted) ? (int32)atoi(wanted.c_str()) : 0;

	// A carried object first: an item stays targetable wherever the player is.
	for (uint i = 0; i < _inventory.size(); i++) {
		if (_inventory[i].name == wanted ||
		    (numeric && _inventory[i].icon == numeric)) {
			item = _inventory[i];
			return true;
		}
	}

	Common::Array<Hotspot> hotspots;
	collectHotspots(hotspots);
	for (uint i = 0; i < hotspots.size(); i++) {
		if (hotspots[i].name == wanted ||
		    (numeric && hotspots[i].id == (uint32)numeric)) {
			hotspot = hotspots[i];
			isHotspot = true;
			return true;
		}
	}

	Common::String available;
	for (uint i = 0; i < hotspots.size() && i < 14; i++) {
		if (!available.empty())
			available += ", ";
		available += hotspots[i].name;
	}
	errorOut = "unknown target '" + name + "'; on this screen: " + available;
	return false;
}

void Sword2McpBridge::clickHotspot(const Hotspot &hotspot, int x, int y, bool rightButton) {
	// Warping the real cursor matters as much as the click: the next cycle
	// recomputes what is being pointed at from the physical cursor, and without
	// the warp it would immediately report the player as pointing at whatever
	// sits under the never-moved one.
	ScreenInfo *screenInfo = _vm->_screen->getScreenInfo();
	int sx = x - (int)screenInfo->scroll_offset_x;
	int sy = y - (int)screenInfo->scroll_offset_y;
	if (sx >= 0 && sy >= 0 && sx < _vm->_screen->getScreenWide() &&
	    sy < _vm->_screen->getScreenDeep())
		g_system->warpMouse(sx, sy);

	_vm->_mouse->mcpClick(hotspot.id, x, y, rightButton);
}

// ---------------------------------------------------------------------------
// Tool: state
// ---------------------------------------------------------------------------

Common::String Sword2McpBridge::stateToolDescription() const {
	return "Returns the current game state: the screen (id + name), where the "
	       "player character stands, everything on screen that can be pointed "
	       "at (with the label the game itself shows for it), the items being "
	       "carried, the verbs, the lines said since the last read (cleared "
	       "after reading) and the pending conversation question if any. Every "
	       "objects[] entry carries a 'kind' — 'person' (can be talked to), "
	       "'item' (can be picked up), 'exit' (leads off this screen), 'floor' "
	       "(somewhere to walk) or 'object' — plus its position and the box it "
	       "covers. Objects and items can be targeted by 'name' or by 'id' in "
	       "act(). An item that shows as 'held' is the one an action will be "
	       "carried out with. Nothing is accepted while can_act is false.";
}

Common::String Sword2McpBridge::actToolDescription() const {
	return "Do something to a named target. 'look_at' examines it; every other "
	       "verb ('interact', 'use', 'talk_to', 'pick_up', 'walk_to') runs the "
	       "target's own interaction, which decides what happens — the "
	       "distinction is for your own reasoning, not the game's. Give "
	       "'target2' as a carried item to do it with that item in hand, which "
	       "is how one thing is used on another. A target marked 'pathway' "
	       "takes two goes: the first walks the character over to it, the "
	       "second leaves — so if the scene is unchanged afterwards, ask again. "
	       + streamingToolNote();
}

Common::String Sword2McpBridge::walkToolDescription() const {
	return "Walk the player character to a point, given in the coordinates "
	       "state reports positions in. The point has to be on one of the "
	       "walkable areas state lists (kind 'floor'); the error says which "
	       "those are. " + streamingToolNote();
}

void Sword2McpBridge::augmentStateSchema(Common::JSONObject &outputProps) {
	outputProps.setVal("can_act", mcpProp("boolean",
	    "False while the game is not accepting a new action (a scene playing "
	    "itself out, a line being said, a conversation waiting for an answer). "
	    "act/walk are rejected until it turns true again."));
	outputProps.setVal("held_item", mcpProp("string",
	    "The item currently in hand, which an action is carried out with. "
	    "Absent when nothing is held."));

	{
		Common::JSONObject props;
		props.setVal("id",   mcpProp("integer", "Target id; usable as an act() target."));
		props.setVal("name", mcpProp("string",  "Name, as act() expects it."));
		props.setVal("kind", mcpProp("string",
		    "'person', 'item', 'exit', 'floor', 'scroll' or 'object'."));
		props.setVal("pathway", mcpProp("boolean",
		    "Present and true when going there leaves the screen. Acting on one "
		    "twice is what leaves: the first walks the character over to it."));
		Common::JSONObject posProps;
		posProps.setVal("x", mcpProp("integer", "X coordinate"));
		posProps.setVal("y", mcpProp("integer", "Y coordinate"));
		Common::JSONObject posSchema;
		posSchema.setVal("type",       mcpJsonString("object"));
		posSchema.setVal("properties", new Common::JSONValue(posProps));
		props.setVal("position", new Common::JSONValue(posSchema));
		Common::JSONObject box;
		box.setVal("type",  mcpJsonString("array"));
		box.setVal("items", mcpProp("integer"));
		box.setVal("description",
		    mcpJsonString("The area it covers, as [x1, y1, x2, y2]."));
		props.setVal("box", new Common::JSONValue(box));
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

void Sword2McpBridge::augmentChangesSchema(Common::JSONObject &props) {
	props.setVal("held_item", mcpProp("string",
	    "The item in hand after the action, when it changed. Empty string when "
	    "the hand was emptied."));
	props.setVal("room_name", mcpProp("string",
	    "Name of the new screen (only present if the screen changed)."));
}

// The conversation subjects on offer, as (1-based id, label) pairs.
static void buildQuestion(const Common::Array<Common::String> &labels,
                          Common::JSONObject &question) {
	Common::JSONArray choices;
	for (uint i = 0; i < labels.size(); i++) {
		Common::JSONObject c;
		c.setVal("id", mcpJsonInt((int)i + 1));
		c.setVal("label", mcpJsonString(labels[i]));
		choices.push_back(new Common::JSONValue(c));
	}
	question.setVal("choices", new Common::JSONValue(choices));
}

// The subjects the chooser is offering, named after their icon resources.
static void subjectLabels(Sword2Engine *vm, Common::Array<Common::String> &out,
                          const Sword2McpBridge *bridge);

Common::JSONValue *Sword2McpBridge::toolState(const Common::JSONValue &, Common::String &) {
	Common::JSONObject out;

	Common::JSONObject room;
	room.setVal("id", mcpJsonInt((int)location()));
	Common::String name = locationName();
	if (!name.empty())
		room.setVal("name", mcpJsonString(name));
	out.setVal("room", new Common::JSONValue(room));

	int px = 0, py = 0;
	playerPos(px, py);
	Common::JSONObject pos;
	pos.setVal("x", mcpJsonInt(px));
	pos.setVal("y", mcpJsonInt(py));
	out.setVal("position", new Common::JSONValue(pos));

	out.setVal("can_act", mcpJsonBool(canAct()));

	Common::JSONArray objects;
	Common::Array<Hotspot> hotspots;
	collectHotspots(hotspots);
	for (uint i = 0; i < hotspots.size(); i++) {
		const Hotspot &h = hotspots[i];
		Common::JSONObject o;
		o.setVal("id",   mcpJsonInt((int)h.id));
		o.setVal("name", mcpJsonString(h.name));
		o.setVal("kind", mcpJsonString(h.kind));
		if (h.isExit)
			o.setVal("pathway", mcpJsonBool(true));
		Common::JSONObject p;
		p.setVal("x", mcpJsonInt((h.x1 + h.x2) / 2));
		p.setVal("y", mcpJsonInt((h.y1 + h.y2) / 2));
		o.setVal("position", new Common::JSONValue(p));
		Common::JSONArray box;
		box.push_back(mcpJsonInt(h.x1));
		box.push_back(mcpJsonInt(h.y1));
		box.push_back(mcpJsonInt(h.x2));
		box.push_back(mcpJsonInt(h.y2));
		o.setVal("box", new Common::JSONValue(box));
		objects.push_back(new Common::JSONValue(o));
	}
	out.setVal("objects", new Common::JSONValue(objects));

	const int32 held = (int32)_vm->_logic->readVar(OBJECT_HELD);
	Common::JSONArray inv;
	for (uint i = 0; i < _inventory.size(); i++) {
		Common::JSONObject item;
		item.setVal("id",   mcpJsonInt(_inventory[i].icon));
		item.setVal("name", mcpJsonString(_inventory[i].name));
		if (_inventory[i].icon == held)
			item.setVal("held", mcpJsonBool(true));
		inv.push_back(new Common::JSONValue(item));
	}
	out.setVal("inventory", new Common::JSONValue(inv));
	if (held) {
		Common::String heldName = resourceName((uint32)held);
		out.setVal("held_item", mcpJsonString(heldName.empty()
		    ? sword2FallbackName(held) : heldName));
	}

	Common::JSONArray verbs;
	for (int i = 0; kVerbs[i]; i++)
		verbs.push_back(mcpJsonString(kVerbs[i]));
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
		Common::Array<Common::String> labels;
		subjectLabels(_vm, labels, this);
		Common::JSONObject question;
		buildQuestion(labels, question);
		out.setVal("question", new Common::JSONValue(question));
	}

	return new Common::JSONValue(out);
}

// ---------------------------------------------------------------------------
// Tool: act
// ---------------------------------------------------------------------------

bool Sword2McpBridge::toolAct(const Common::JSONValue &args, Common::String &errorOut) {
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
	Common::String verb = normalizeActionName(a["verb"]->asString());

	bool known = false;
	for (int i = 0; kVerbs[i]; i++)
		if (verb == kVerbs[i]) { known = true; break; }
	if (!known) {
		errorOut = "act: unknown verb '" + verb + "'";
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

	// Resolve before asking whether the game is listening: a target that does
	// not exist is wrong whatever the game is doing, and saying which is more
	// use than "not right now".
	Hotspot hotspot;
	Item item;
	bool isHotspot = false, item1IsItem = false;
	Item held;
	if (!resolveTarget(name1, hotspot, isHotspot, item, errorOut)) {
		errorOut = "act: " + errorOut;
		return false;
	}
	item1IsItem = !isHotspot;
	if (item1IsItem)
		held = item;

	if (!name2.empty()) {
		Hotspot hotspot2;
		Item item2;
		bool isHotspot2 = false;
		if (!resolveTarget(name2, hotspot2, isHotspot2, item2, errorOut)) {
			errorOut = "act: " + errorOut;
			return false;
		}
		// "use X on Y": whichever of the two is a carried item goes in hand,
		// and the other is what gets clicked.
		if (item1IsItem && !isHotspot2) {
			errorOut = "act: one of the two targets has to be something on screen";
			return false;
		}
		if (item1IsItem) {
			hotspot = hotspot2;
			isHotspot = true;
		} else if (!isHotspot2) {
			held = item2;
			item1IsItem = true;
		} else {
			errorOut = "act: 'target2' must be an item you are carrying";
			return false;
		}
	}

	if (!isHotspot) {
		errorOut = "act: '" + name1 + "' is an item you are carrying; say what to "
		           "use it on as 'target2'";
		return false;
	}
	if (!canAct()) {
		errorOut = "act: the game is not accepting input right now";
		return false;
	}

	// What was in hand before this action, so that arming an item for it is
	// not itself reported as a change.
	const int32 heldBefore = (int32)_vm->_logic->readVar(OBJECT_HELD);

	// Nothing may be left in hand by accident, or the game reads the click as
	// "use <held item> on X".
	if (item1IsItem)
		_vm->_mouse->mcpHoldObject(held.icon, held.luggage);
	else
		_vm->_logic->writeVar(OBJECT_HELD, 0);

	_skipStream = false;
	clickHotspot(hotspot, (hotspot.x1 + hotspot.x2) / 2, (hotspot.y1 + hotspot.y2) / 2,
	             isLookVerb(verb));
	beginStream();
	_ssePreHeld = heldBefore;
	return true;
}

// ---------------------------------------------------------------------------
// Tool: walk
// ---------------------------------------------------------------------------

bool Sword2McpBridge::toolWalk(const Common::JSONValue &args, Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "walk: another action is already in progress";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("x") || !args.asObject().contains("y") ||
	    !args.asObject()["x"]->isIntegerNumber() || !args.asObject()["y"]->isIntegerNumber()) {
		errorOut = "walk: integer 'x' and 'y' are required";
		return false;
	}
	int x = (int)args.asObject()["x"]->asIntegerNumber();
	int y = (int)args.asObject()["y"]->asIntegerNumber();

	// Find the walkable area under the point and click it: its script is what
	// walks the player there, and going through the click is what keeps walk
	// and act on one path.
	Common::Array<Hotspot> hotspots;
	collectHotspots(hotspots);
	Common::String areas;
	for (uint i = 0; i < hotspots.size(); i++) {
		if (!hotspots[i].isFloor)
			continue;
		if (!areas.empty())
			areas += ", ";
		areas += Common::String::format("[%d,%d,%d,%d]", hotspots[i].x1, hotspots[i].y1,
		                                hotspots[i].x2, hotspots[i].y2);
	}
	// Areas overlap, and the game resolves that by priority: the lowest number
	// wins, then list order. Pick the same one a click there would, or the
	// walk would go to a different area than the player's own click.
	const Hotspot *floor = nullptr;
	for (int priority = 0; priority < 10 && !floor; priority++) {
		for (uint i = 0; i < hotspots.size(); i++) {
			if (!hotspots[i].isFloor || hotspots[i].priority != priority)
				continue;
			if (x >= hotspots[i].x1 && x <= hotspots[i].x2 &&
			    y >= hotspots[i].y1 && y <= hotspots[i].y2) {
				floor = &hotspots[i];
				break;
			}
		}
	}
	if (!floor) {
		errorOut = Common::String::format(
		    "walk: (%d, %d) is not on a walkable area; the ones on this screen are: ",
		    x, y) + areas;
		return false;
	}
	if (!canAct()) {
		errorOut = "walk: the game is not accepting input right now";
		return false;
	}

	const int32 heldBefore = (int32)_vm->_logic->readVar(OBJECT_HELD);
	_skipStream = false;
	_vm->_logic->writeVar(OBJECT_HELD, 0);
	clickHotspot(*floor, x, y, false);
	beginStream();
	_ssePreHeld = heldBefore;
	return true;
}

// ---------------------------------------------------------------------------
// Tool: answer
// ---------------------------------------------------------------------------

static void subjectLabels(Sword2Engine *vm, Common::Array<Common::String> &out,
                          const Sword2McpBridge *bridge) {
	(void)bridge;
	out.clear();
	uint32 count = vm->_logic->readVar(IN_SUBJECT);
	Common::Array<Common::String> base;
	for (uint32 i = 0; i < count; i++) {
		uint32 res = vm->_mouse->mcpSubject(i);
		Common::String name;
		if (vm->_resman->mcpIsAvailable(res)) {
			byte buf[NAME_LEN + 1];
			memset(buf, 0, sizeof(buf));
			vm->_resman->fetchName(res, buf);
			buf[NAME_LEN] = 0;
			name = sword2CleanName(Common::String((const char *)buf));
		}
		if (name.empty())
			name = Common::String::format("topic_%u", res);
		base.push_back(name);
	}
	for (uint i = 0; i < base.size(); i++) {
		uint seen = 0;
		for (uint j = 0; j < i; j++)
			if (base[j] == base[i])
				seen++;
		out.push_back(sword2Disambiguate(base[i], seen));
	}
}

bool Sword2McpBridge::toolAnswer(const Common::JSONValue &args, Common::String &errorOut) {
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
	const uint32 count = _vm->_logic->readVar(IN_SUBJECT);
	int id = (int)args.asObject()["id"]->asIntegerNumber();
	if (id < 1 || (uint32)id > count) {
		errorOut = Common::String::format("answer: 'id' must be between 1 and %u", count);
		return false;
	}
	if (!_vm->_mouse->mcpChooseSubject((uint32)(id - 1))) {
		errorOut = "answer: the conversation chooser is not open";
		return false;
	}
	_skipStream = false;
	beginStream();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: skip
// ---------------------------------------------------------------------------

bool Sword2McpBridge::toolSkip(const Common::JSONValue &, Common::String &errorOut) {
	if (!_skipToolEnabled) {
		errorOut = "skip: tool is disabled (set mcp_skip_tool=true)";
		return false;
	}
	if (!engineReady()) {
		errorOut = "skip: the game is still starting up";
		return false;
	}
	// A click past a line, and the button the movie player exits on, are the
	// same gesture; push it and let whichever is running take it.
	Common::Event event;
	event.type = Common::EVENT_LBUTTONDOWN;
	g_system->getEventManager()->pushEvent(event);
	event.type = Common::EVENT_LBUTTONUP;
	g_system->getEventManager()->pushEvent(event);
	if (!isStreaming()) {
		_skipStream = true;
		beginStream();
	}
	return true;
}

// ---------------------------------------------------------------------------
// Tool dispatch / gating
// ---------------------------------------------------------------------------

Common::JSONValue *Sword2McpBridge::callTool(const Common::String &name,
                                             const Common::JSONValue &args,
                                             Common::String &errorOut) {
	// The bridge is constructed before run() builds the subsystems and starts
	// a game, so a call arriving that early has nothing to read.
	if (!engineReady()) {
		errorOut = name + ": the game is still starting up";
		return nullptr;
	}
	return MCP::McpBridge::callTool(name, args, errorOut);
}

// ---------------------------------------------------------------------------
// Input injection (debug tools)
// ---------------------------------------------------------------------------

void Sword2McpBridge::injectKey(const Common::KeyState &ks) {
	Common::Event event;
	event.type = Common::EVENT_KEYDOWN;
	event.kbd = ks;
	g_system->getEventManager()->pushEvent(event);
	event.type = Common::EVENT_KEYUP;
	g_system->getEventManager()->pushEvent(event);
}

void Sword2McpBridge::injectMouseMove(int x, int y) {
	// Debug tools speak screen coordinates, which is what the cursor uses.
	g_system->warpMouse(x, y);
	Common::Event event;
	event.type = Common::EVENT_MOUSEMOVE;
	event.mouse = Common::Point(x, y);
	g_system->getEventManager()->pushEvent(event);
}

void Sword2McpBridge::injectMouseClick(int x, int y, const Common::String &button, bool isDouble) {
	injectMouseMove(x, y);
	bool right = (button == "right");
	Common::Event down, up;
	down.type = right ? Common::EVENT_RBUTTONDOWN : Common::EVENT_LBUTTONDOWN;
	up.type   = right ? Common::EVENT_RBUTTONUP   : Common::EVENT_LBUTTONUP;
	down.mouse = up.mouse = Common::Point(x, y);
	int clicks = isDouble ? 2 : 1;
	for (int i = 0; i < clicks; i++) {
		g_system->getEventManager()->pushEvent(down);
		g_system->getEventManager()->pushEvent(up);
	}
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

void Sword2McpBridge::snapshotPreAction() {
	_sseActionStarted = false;
	_ssePreLocation = (int)location();
	_ssePreBackground = backgroundLayer();
	_ssePreRoom = _ssePreLocation;
	_ssePreHeld = (int32)_vm->_logic->readVar(OBJECT_HELD);

	_ssePreInventory.clear();
	_ssePreInventoryNames.clear();
	for (uint i = 0; i < _inventory.size(); i++) {
		_ssePreInventory.push_back(_inventory[i].icon);
		_ssePreInventoryNames.push_back(_inventory[i].name);
	}

	Common::Array<Hotspot> hotspots;
	collectHotspots(hotspots);
	_ssePreHotspots.clear();
	for (uint i = 0; i < hotspots.size(); i++)
		_ssePreHotspots.push_back(hotspots[i].name);

	playerPos(_ssePrePosX, _ssePrePosY);
	_sseTrackPosX = _ssePrePosX;
	_sseTrackPosY = _ssePrePosY;
	_sseTrackLocation = _ssePreLocation;
	_sseTrackCanAct = canAct();
}

void Sword2McpBridge::pumpStreamTrack() {
	// Only a *change* counts as progress. A condition that merely stays true —
	// the mouse being off for the length of a cutaway — would otherwise keep
	// pushing the deadline back for as long as the cutaway lasts.
	bool moved = false;

	int px = 0, py = 0;
	playerPos(px, py);
	if (px != _sseTrackPosX || py != _sseTrackPosY) {
		_sseTrackPosX = px;
		_sseTrackPosY = py;
		moved = true;
	}
	int loc = (int)location();
	if (loc != _sseTrackLocation) {
		_sseTrackLocation = loc;
		moved = true;
	}
	bool act = canAct();
	if (act != _sseTrackCanAct) {
		_sseTrackCanAct = act;
		moved = true;
	}
	if (_vm->_mouse->mcpChoosing())
		moved = true;

	if (moved) {
		_sseActionStarted = true;
		_sseLastEventFrame = _frameCounter;
	}
}

bool Sword2McpBridge::streamRoomChanged() const {
	return engineReady() && screenChangeSettled() && canAct();
}

bool Sword2McpBridge::hasPendingQuestion() const {
	return engineReady() && _vm->_mouse->mcpChoosing() &&
	       _vm->_logic->readVar(IN_SUBJECT) > 0;
}

bool Sword2McpBridge::isStreamStuck() const {
	// Input locked with nothing being said and the player standing still.
	if (!engineReady())
		return true;
	if (speaking() || _vm->_mouse->mcpChoosing())
		return false;
	return !_vm->_logic->readVar(MOUSE_AVAILABLE) && !_sseActionStarted;
}

bool Sword2McpBridge::isActionDone() const {
	if (!engineReady())
		return false;
	// A skip is one click: it reports what that click did rather than waiting
	// for control, which the game may not hand back for several scenes yet.
	if (_skipStream)
		return _frameCounter - _sseStartFrame >= kSkipFrames ||
		       (g_system && g_system->getMillis() - _sseStartMs >= kSkipMs);
	// A conversation asking for an answer is a settled state: the stream
	// closes and reports the question.
	if (hasPendingQuestion())
		return true;
	if (!canAct())
		return false;
	// A screen change is under way: the new screen has nothing to report yet.
	if ((int)location() != _ssePreLocation && !screenChangeSettled())
		return false;
	// The click is queued as an event the target's script picks up on the next
	// cycle; until it has, nothing has happened yet.
	if (_vm->_logic->countEvents())
		return false;
	// Give the action a short window to show its first effect before
	// concluding it was a no-op.
	if (!_sseActionStarted && _frameCounter - _sseStartFrame < kNoOpFrames)
		return false;
	return true;
}

Common::JSONObject Sword2McpBridge::buildStateChanges() const {
	Common::JSONObject changes;

	// The cache pumpGame() keeps is what this reads: rebuilding it here would
	// run a game script from inside whichever loop the server was pumped from.
	auto contains = [](const Common::Array<int32> &arr, int32 v) -> bool {
		for (uint i = 0; i < arr.size(); i++)
			if (arr[i] == v)
				return true;
		return false;
	};

	Common::JSONArray added;
	for (uint i = 0; i < _inventory.size(); i++)
		if (!contains(_ssePreInventory, _inventory[i].icon))
			added.push_back(mcpJsonString(_inventory[i].name));
	if (!added.empty())
		changes.setVal("inventory_added", new Common::JSONValue(added));

	Common::JSONArray removed;
	for (uint i = 0; i < _ssePreInventory.size(); i++) {
		bool still = false;
		for (uint j = 0; j < _inventory.size(); j++)
			still |= (_inventory[j].icon == _ssePreInventory[i]);
		if (!still)
			removed.push_back(mcpJsonString(_ssePreInventoryNames[i]));
	}
	if (!removed.empty())
		changes.setVal("inventory_removed", new Common::JSONValue(removed));

	const int32 held = (int32)_vm->_logic->readVar(OBJECT_HELD);
	if (held != _ssePreHeld) {
		Common::String heldName;
		if (held) {
			heldName = resourceName((uint32)held);
			if (heldName.empty())
				heldName = sword2FallbackName(held);
		}
		changes.setVal("held_item", mcpJsonString(heldName));
	}

	const int loc = (int)location();
	if (loc != _ssePreLocation) {
		changes.setVal("room_changed", mcpJsonInt(loc));
		Common::String name = locationName();
		if (!name.empty())
			changes.setVal("room_name", mcpJsonString(name));
	}

	int px = 0, py = 0;
	playerPos(px, py);
	if (px != _ssePrePosX || py != _ssePrePosY) {
		Common::JSONObject pos;
		pos.setVal("x", mcpJsonInt(px));
		pos.setVal("y", mcpJsonInt(py));
		changes.setVal("position", new Common::JSONValue(pos));
	}

	// Things that appeared or vanished on the same screen: a door that opened,
	// someone who walked off.
	if (loc == _ssePreLocation) {
		Common::Array<Hotspot> hotspots;
		collectHotspots(hotspots);
		Common::Array<Common::String> now;
		for (uint i = 0; i < hotspots.size(); i++)
			now.push_back(hotspots[i].name);
		auto hasName = [](const Common::Array<Common::String> &arr,
		                  const Common::String &n) -> bool {
			for (uint i = 0; i < arr.size(); i++)
				if (arr[i] == n)
					return true;
			return false;
		};
		Common::JSONArray objChanges;
		for (uint i = 0; i < now.size(); i++) {
			if (!hasName(_ssePreHotspots, now[i])) {
				Common::JSONObject c;
				c.setVal("name", mcpJsonString(now[i]));
				c.setVal("old_state", mcpJsonString("absent"));
				c.setVal("new_state", mcpJsonString("present"));
				objChanges.push_back(new Common::JSONValue(c));
			}
		}
		for (uint i = 0; i < _ssePreHotspots.size(); i++) {
			if (!hasName(now, _ssePreHotspots[i])) {
				Common::JSONObject c;
				c.setVal("name", mcpJsonString(_ssePreHotspots[i]));
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
		Common::Array<Common::String> labels;
		subjectLabels(_vm, labels, this);
		Common::JSONObject question;
		buildQuestion(labels, question);
		changes.setVal("question", new Common::JSONValue(question));
	}

	return changes;
}

// ---------------------------------------------------------------------------
// Tool: debug
// ---------------------------------------------------------------------------

Common::String Sword2McpBridge::debugToolDescription() const {
	return "Return raw engine state for diagnostics. Sections are selected by "
	       "flag: 'system' (screen, input availability, what the game currently "
	       "thinks is being pointed at), 'objects' (every pointable thing with "
	       "its box and cursor), 'items' (the carried objects) and 'vars' (a "
	       "slice of the game's own script variables, with 'from'/'to'). "
	       "Defaults to 'system'.";
}

Common::JSONValue *Sword2McpBridge::buildDebugSchema() const {
	Common::JSONObject props;
	props.setVal("system",  mcpProp("boolean", "Include the engine state summary (default true)."));
	props.setVal("objects", mcpProp("boolean", "Include the pointable things on screen."));
	props.setVal("items",   mcpProp("boolean", "Include the carried objects."));
	props.setVal("vars",    mcpProp("boolean", "Include script variables."));
	props.setVal("from",    mcpProp("integer", "First script variable index (default 0)."));
	props.setVal("to",      mcpProp("integer", "Last script variable index, inclusive (default 63)."));
	return mcpObjectSchema(props);
}

Common::JSONValue *Sword2McpBridge::toolDebug(const Common::JSONValue &args, Common::String &) {
	const bool hasArgs = args.isObject();
	auto flag = [&](const char *key, bool dflt) -> bool {
		if (!hasArgs || !args.asObject().contains(key) || !args.asObject()[key]->isBool())
			return dflt;
		return args.asObject()[key]->asBool();
	};
	auto number = [&](const char *key, int dflt) -> int {
		if (!hasArgs || !args.asObject().contains(key) || !args.asObject()[key]->isIntegerNumber())
			return dflt;
		return (int)args.asObject()[key]->asIntegerNumber();
	};

	Common::JSONObject out;

	if (flag("system", true)) {
		Common::JSONObject sys;
		sys.setVal("location",        mcpJsonInt((int)location()));
		sys.setVal("location_name",   mcpJsonString(locationName()));
		sys.setVal("background_layer",
		    mcpJsonInt((int)_vm->_screen->getScreenInfo()->background_layer_id));
		sys.setVal("can_act",         mcpJsonBool(canAct()));
		sys.setVal("mouse_available", mcpJsonBool(_vm->_logic->readVar(MOUSE_AVAILABLE) != 0));
		sys.setVal("speaking",        mcpJsonBool(speaking()));
		sys.setVal("choosing",        mcpJsonBool(_vm->_mouse->mcpChoosing()));
		sys.setVal("in_subject",      mcpJsonInt((int)_vm->_logic->readVar(IN_SUBJECT)));
		sys.setVal("object_held",     mcpJsonInt((int)_vm->_logic->readVar(OBJECT_HELD)));
		sys.setVal("clicked_id",      mcpJsonInt((int)_vm->_logic->readVar(CLICKED_ID)));
		sys.setVal("mouse_touching",  mcpJsonInt((int)_vm->_mouse->getMouseTouching()));
		sys.setVal("events_pending",  mcpJsonInt((int)_vm->_logic->countEvents()));
		sys.setVal("object_labels",   mcpJsonBool(_vm->_mouse->getObjectLabels()));
		int px = 0, py = 0;
		playerPos(px, py);
		sys.setVal("player_x",        mcpJsonInt(px));
		sys.setVal("player_y",        mcpJsonInt(py));
		ScreenInfo *screenInfo = _vm->_screen->getScreenInfo();
		sys.setVal("scroll_x",        mcpJsonInt((int)screenInfo->scroll_offset_x));
		sys.setVal("scroll_y",        mcpJsonInt((int)screenInfo->scroll_offset_y));
		sys.setVal("frame_counter",   mcpJsonInt((int)_frameCounter));
		out.setVal("system", new Common::JSONValue(sys));
	}

	if (flag("objects", false)) {
		Common::JSONArray arr;
		Common::Array<Hotspot> hotspots;
		collectHotspots(hotspots);
		const uint32 count = _vm->_mouse->mcpHotspotCount();
		for (uint i = 0; i < hotspots.size() && i < count; i++) {
			const MouseUnit &unit = _vm->_mouse->mcpHotspot(i);
			Common::JSONObject o;
			o.setVal("id",           mcpJsonInt((int)hotspots[i].id));
			o.setVal("name",         mcpJsonString(hotspots[i].name));
			o.setVal("kind",         mcpJsonString(hotspots[i].kind));
			o.setVal("pointer",      mcpJsonInt((int)unit.pointer));
			o.setVal("pointer_text", mcpJsonInt((int)unit.pointer_text));
			// The two candidate names, so it is clear which one the
			// snapshot ended up publishing.
			o.setVal("label",         mcpJsonString(textLine(unit.pointer_text)));
			o.setVal("resource_name", mcpJsonString(resourceName(hotspots[i].id)));
			o.setVal("priority",     mcpJsonInt((int)unit.priority));
			o.setVal("x1",           mcpJsonInt(hotspots[i].x1));
			o.setVal("y1",           mcpJsonInt(hotspots[i].y1));
			o.setVal("x2",           mcpJsonInt(hotspots[i].x2));
			o.setVal("y2",           mcpJsonInt(hotspots[i].y2));
			arr.push_back(new Common::JSONValue(o));
		}
		out.setVal("objects", new Common::JSONValue(arr));
	}

	if (flag("items", false)) {
		Common::JSONArray arr;
		for (uint i = 0; i < _inventory.size(); i++) {
			Common::JSONObject o;
			o.setVal("id",      mcpJsonInt(_inventory[i].icon));
			o.setVal("name",    mcpJsonString(_inventory[i].name));
			o.setVal("luggage", mcpJsonInt(_inventory[i].luggage));
			arr.push_back(new Common::JSONValue(o));
		}
		out.setVal("items", new Common::JSONValue(arr));
	}

	if (flag("vars", false)) {
		int from = MAX(0, number("from", 0));
		int to = MAX(from, number("to", 63));
		Common::JSONArray vars;
		for (int i = from; i <= to; i++) {
			Common::JSONObject v;
			v.setVal("index", mcpJsonInt(i));
			v.setVal("value", mcpJsonInt((int)_vm->_logic->readVar(i)));
			vars.push_back(new Common::JSONValue(v));
		}
		out.setVal("vars", new Common::JSONValue(vars));
	}

	return new Common::JSONValue(out);
}

} // End of namespace Sword2
