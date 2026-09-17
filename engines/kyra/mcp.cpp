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

#include "kyra/mcp.h"
#include "kyra/mcp_names.h"

#include "kyra/detection.h"
#include "kyra/kyra_v1.h"
#include "kyra/engine/kyra_lok.h"
#include "kyra/engine/kyra_hof.h"
#include "kyra/engine/kyra_mr.h"
#include "kyra/graphics/screen.h"
#include "kyra/gui/gui.h"

#include "common/endian.h"
#include "common/events.h"
#include "common/system.h"
#include "common/util.h"

namespace Kyra {

using Networking::mcpJsonString;
using Networking::mcpJsonInt;
using Networking::mcpJsonBool;
using Networking::mcpProp;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

KyraMcpBridge *KyraMcpBridge::create(KyraEngine_v1 *vm) {
	if (vm == nullptr)
		return nullptr;
	const int game = vm->game();
	if (game != GI_KYRA1 && game != GI_KYRA2 && game != GI_KYRA3)
		return nullptr;
	KyraMcpBridge *bridge = new KyraMcpBridge(vm);
	bridge->init();
	return bridge;
}

KyraMcpBridge::KyraMcpBridge(KyraEngine_v1 *vm) :
	MCP::McpBridge(vm, "scummvm", "1.0"),
	_vm(vm),
	_inStallPump(false),
	_lastFrameMs(0),
	_lastLoopFrame(0),
	_passesSinceInput(0),
	_skipStream(false),
	_lastSkipFrame(0),
	_pendingClick(false),
	_pendingX(0), _pendingY(0),
	_nameRoom(-1),
	_nameRoomFrame(0),
	_nameClicked(false),
	_ssePreRoom(-1),
	_ssePreX(0), _ssePreY(0),
	_ssePreHand(-1),
	_sseTrackRoom(-1), _sseTrackX(0), _sseTrackY(0), _sseTrackHand(-1),
	_sseTrackSteps(0) {
}

KyraMcpBridge::~KyraMcpBridge() {
}

bool KyraMcpBridge::isFirstGame() const {
	return _vm != nullptr && _vm->game() == GI_KYRA1;
}

bool KyraMcpBridge::isThirdGame() const {
	return _vm != nullptr && _vm->game() == GI_KYRA3;
}

bool KyraMcpBridge::engineReady() const {
	if (_vm == nullptr)
		return false;
	// The bridge is built in the engine's constructor so the port binds before
	// anything can block on startup, which means a call can arrive long before
	// there is a game behind it.
	if (isFirstGame()) {
		const KyraEngine_LoK *lok = static_cast<const KyraEngine_LoK *>(_vm);
		return lok->_currentCharacter != nullptr && lok->_roomTable != nullptr &&
		       lok->_buttonData != nullptr;
	}
	if (isThirdGame()) {
		const KyraEngine_MR *mr = static_cast<const KyraEngine_MR *>(_vm);
		return mr->_sceneList != nullptr && mr->_mainButtonData != nullptr;
	}
	const KyraEngine_HoF *hof = static_cast<const KyraEngine_HoF *>(_vm);
	return hof->_sceneList != nullptr && hof->_inventoryButtons != nullptr;
}

// ---------------------------------------------------------------------------
// Pumping
// ---------------------------------------------------------------------------

void KyraMcpBridge::pump() {
	if (!isEnabled())
		return;
	_lastLoopFrame = _frameCounter + 1;
	_passesSinceInput++;
	recoverAbandonedStream();
	MCP::McpBridge::pump();
}

void KyraMcpBridge::pumpFromStall() {
	if (!isEnabled() || _inStallPump)
		return;
	_inStallPump = true;
	recoverAbandonedStream();
	const uint32 now = g_system != nullptr ? g_system->getMillis() : 0;
	if (now - _lastFrameMs >= kFrameMs) {
		_lastFrameMs = now;
		MCP::McpBridge::pump();
	} else {
		pumpTransportOnly();
	}
	_inStallPump = false;
}

void KyraMcpBridge::recoverAbandonedStream() {
	// A client that went away mid-action (its own timeout, usually) took the
	// lines streamed to it with it. They go back on the queue state() reads,
	// so nothing said during a long scene is lost to a caller that comes back
	// and asks. Checked before the base pump, which forgets the stream.
	if (!isStreaming() || _server == nullptr || _server->isStreaming())
		return;
	for (int i = (int)_sseMessages.size() - 1; i >= 0; i--)
		_messages.insert_at(0, _sseMessages[i]);
	_sseMessages.clear();
}

void KyraMcpBridge::pumpGame() {
	// The action's clicks are only ever sent between passes of the game's
	// own loop. From inside a blocking wait the game is still busy with the
	// last click, and a click queued now would land in the middle of it.
	if (_inStallPump)
		return;
	runSteps();
}

void KyraMcpBridge::pumpStreamGame() {
	// skip: press the key again for as long as a scene keeps playing, since
	// each press ends one line or one shot rather than the whole scene.
	if (!_skipStream || playerHasControl())
		return;
	if (_frameCounter - _lastSkipFrame >= kSkipRepeatFrames) {
		_lastSkipFrame = _frameCounter;
		injectKey(Common::KeyState(Common::KEYCODE_ESCAPE, 27));
		_sseLastEventFrame = _frameCounter;
	}
}

// ---------------------------------------------------------------------------
// Reading the game
// ---------------------------------------------------------------------------

int KyraMcpBridge::roomNumber() const {
	if (!engineReady())
		return -1;
	if (isFirstGame())
		return static_cast<const KyraEngine_LoK *>(_vm)->_currentCharacter->sceneId;
	// The character's scene rather than _currentScene: it is set first when a
	// scene changes, and _currentScene holds a placeholder (0x7FFF) until the
	// first scene of a session is entered.
	return static_cast<const KyraEngine_v2 *>(_vm)->_mainCharacter.sceneId;
}

void KyraMcpBridge::heroPosition(int &x, int &y) const {
	x = y = 0;
	if (!engineReady())
		return;
	if (isFirstGame()) {
		const KyraEngine_LoK *lok = static_cast<const KyraEngine_LoK *>(_vm);
		x = lok->_currentCharacter->x1;
		y = lok->_currentCharacter->y1;
		return;
	}
	const KyraEngine_v2 *v2 = static_cast<const KyraEngine_v2 *>(_vm);
	x = v2->_mainCharacter.x1;
	y = v2->_mainCharacter.y1;
}

int KyraMcpBridge::handItem() const {
	if (!engineReady())
		return -1;
	int item;
	if (isFirstGame())
		item = static_cast<const KyraEngine_LoK *>(_vm)->_itemInHand;
	else
		item = static_cast<const KyraEngine_v2 *>(_vm)->_itemInHand;
	return kyraIsEmptyItem(item) ? -1 : item;
}

bool KyraMcpBridge::playerHasControl() const {
	if (!engineReady())
		return false;
	// Three things, all of which a player can see. The last input poll was the
	// main loop's rather than a cutscene's (see _mcpInMainLoop); the game loop
	// itself has come round lately, rather than the game sitting inside a walk
	// or a speech of its own; and the pointer is showing, which all three games
	// hide while a scene plays.
	if (!_vm->_mcpInMainLoop)
		return false;
	if (_frameCounter - _lastLoopFrame > kLoopGoneFrames)
		return false;
	return const_cast<KyraEngine_v1 *>(_vm)->screen()->isMouseVisible();
}

Common::String KyraMcpBridge::itemLabel(int itemId) const {
	if (!engineReady() || kyraIsEmptyItem(itemId))
		return Common::String();
	if (isFirstGame()) {
		const KyraEngine_LoK *lok = static_cast<const KyraEngine_LoK *>(_vm);
		if (lok->_itemList == nullptr)
			return Common::String();
		const int index = const_cast<KyraEngine_LoK *>(lok)->getItemListIndex((Item)itemId);
		if (index < 0 || index >= lok->_itemList_Size)
			return Common::String();
		const char *name = lok->_itemList[index];
		return name != nullptr ? Common::String(name) : Common::String();
	}
	if (isThirdGame()) {
		// The item-name file is a table of (id, offset) pairs. The engine's
		// own lookup walks it until it finds the id, and never stops if the id
		// is not there, so it is walked here with a bound instead.
		const KyraEngine_MR *mr = static_cast<const KyraEngine_MR *>(_vm);
		const uint8 *file = mr->_itemFile;
		if (file == nullptr)
			return Common::String();
		const uint16 entries = READ_LE_UINT16(file);
		for (uint i = 0; i < entries; i++) {
			if (READ_LE_UINT16(file + 2 + i * 2) != itemId)
				continue;
			const uint16 offset = READ_LE_UINT16(file + 2 + entries * 2 + i * 2);
			return Common::String((const char *)file + offset);
		}
		return Common::String();
	}
	// Kyrandia 2 prints an item's name from its string file, at the item's
	// number plus 54 - updateCommandLineEx(item + 54, ...). The file starts
	// with its offset table, so the first offset says how many entries it has.
	const KyraEngine_HoF *hof = static_cast<const KyraEngine_HoF *>(_vm);
	if (hof->_cCodeBuffer == nullptr)
		return Common::String();
	const uint entries = READ_LE_UINT16(hof->_cCodeBuffer) / 2;
	if ((uint)(itemId + 54) >= entries)
		return Common::String();
	return const_cast<KyraEngine_HoF *>(hof)->getTableString(itemId + 54, hof->_cCodeBuffer, true);
}

Common::String KyraMcpBridge::itemName(int itemId) const {
	Common::String name = kyraItemName(itemLabel(itemId));
	if (name.empty())
		name = Common::String::format("item_%d", itemId);
	return name;
}

static void kyraNameTargets(Common::Array<Common::String> &seen, Common::String &name) {
	uint occurrence = 0;
	for (uint j = 0; j < seen.size(); j++) {
		if (seen[j] == name)
			occurrence++;
	}
	seen.push_back(name);
	name = kyraDisambiguate(name, occurrence);
}

void KyraMcpBridge::collectTargets(Common::Array<Target> &out) const {
	if (!engineReady())
		return;
	const int room = roomNumber();
	if (room < 0)
		return;

	Common::Array<Common::String> seen;
	auto addItem = [&](int item, int x, int y) {
		Target target;
		target.label = itemLabel(item);
		target.name = kyraItemName(target.label);
		if (target.name.empty())
			target.name = Common::String::format("item_%d", item);
		kyraNameTargets(seen, target.name);
		target.kind = kTargetItem;
		target.itemId = item;
		target.x = x;
		// An item's coordinate is where it stands on the floor; the click
		// that picks it up lands on the item itself, just above.
		target.y = MAX(y - 4, 0);
		target.held = false;
		target.slot = -1;
		out.push_back(target);
	};

	// The two generations keep the room's items in different places: the
	// first game in a table belonging to the room, the later two in one flat
	// list where each entry says which scene it is lying in.
	if (isFirstGame()) {
		const KyraEngine_LoK *lok = static_cast<const KyraEngine_LoK *>(_vm);
		if (room >= lok->_roomTableSize)
			return;
		const Room &here = lok->_roomTable[room];
		for (int i = 0; i < 12; i++) {
			if (!kyraIsEmptyItem(here.itemsTable[i]))
				addItem(here.itemsTable[i], here.itemsXPos[i], here.itemsYPos[i]);
		}
		// The other characters of the story, where they stand in this room.
		// The data has nothing to call them by but their slot.
		if (lok->_characterList != nullptr) {
			for (int i = 1; i < 5; i++) {
				const Character &who = lok->_characterList[i];
				if (who.sceneId != room || who.x1 <= 0 || who.y1 <= 0)
					continue;
				Target target;
				target.name = Common::String::format("character_%d", i);
				kyraNameTargets(seen, target.name);
				target.kind = kTargetCharacter;
				target.itemId = -1;
				target.x = CLIP<int>(who.x1, 16, 304);
				target.y = MAX(who.y1 - 20, 12);
				target.held = false;
				target.slot = -1;
				out.push_back(target);
			}
		}
	} else {
		const KyraEngine_v2 *v2 = static_cast<const KyraEngine_v2 *>(_vm);
		if (v2->_itemList != nullptr) {
			for (int i = 0; i < v2->_itemListSize; i++) {
				const KyraEngine_v2::ItemDefinition &item = v2->_itemList[i];
				if (item.sceneId == room && !kyraIsEmptyItem((int16)item.id))
					addItem(item.id, item.x, item.y);
			}
		}
	}

	// The four ways out. They have no names in either generation - a scene
	// simply says which scene lies north of it - so the names are the bridge's.
	int exits[4] = { -1, -1, -1, -1 };
	int exitX[4] = { 0, 0, 0, 0 };
	int exitY[4] = { 0, 0, 0, 0 };
	if (isFirstGame()) {
		const KyraEngine_LoK *lok = static_cast<const KyraEngine_LoK *>(_vm);
		const Room &here = lok->_roomTable[room];
		exits[0] = here.northExit; exits[1] = here.eastExit;
		exits[2] = here.southExit; exits[3] = here.westExit;
		exitX[0] = lok->_sceneExits.northXPos; exitY[0] = lok->_sceneExits.northYPos;
		exitX[1] = lok->_sceneExits.eastXPos;  exitY[1] = lok->_sceneExits.eastYPos;
		exitX[2] = lok->_sceneExits.southXPos; exitY[2] = lok->_sceneExits.southYPos;
		exitX[3] = lok->_sceneExits.westXPos;  exitY[3] = lok->_sceneExits.westYPos;
	} else {
		const KyraEngine_v2 *v2 = static_cast<const KyraEngine_v2 *>(_vm);
		exits[0] = v2->_sceneExit1; exits[1] = v2->_sceneExit2;
		exits[2] = v2->_sceneExit3; exits[3] = v2->_sceneExit4;
		exitX[0] = v2->_sceneEnterX1; exitY[0] = v2->_sceneEnterY1;
		exitX[1] = v2->_sceneEnterX2; exitY[1] = v2->_sceneEnterY2;
		exitX[2] = v2->_sceneEnterX3; exitY[2] = v2->_sceneEnterY3;
		exitX[3] = v2->_sceneEnterX4; exitY[3] = v2->_sceneEnterY4;
	}
	// Where to click to leave, which is not where the scene's own coordinate
	// points: that is where the character *stands* at this edge, and clicking
	// it is an ordinary walk. All three games decide a click is an exit by
	// which band of the picture it lands in - KyraEngine_LoK::processInput,
	// and the cursor KyraEngine_HoF::updateMouse / KyraEngine_MR::updateMouse
	// set on hover - so the click lands in the band, with the scene's
	// coordinate choosing where along the edge.
	//
	// LoK: x < 12 west, x >= 308 east, y >= 136 south, y < 12 north.
	// HoF: x <= 6, x >= 312, y >= 135, y <= 6.
	// MR:  x <= 8, x >= 311, y >= 171, y <= 8.
	const int westX  = isFirstGame() ? 10 : (isThirdGame() ? 4 : 4);
	const int eastX  = isFirstGame() ? 310 : (isThirdGame() ? 314 : 314);
	const int northY = isFirstGame() ? 10 : 4;
	const int southY = isFirstGame() ? 140 : (isThirdGame() ? 176 : 140);
	const int lowX   = isFirstGame() ? 12 : 9;
	const int highX  = isFirstGame() ? 307 : 310;
	const int lowY   = isFirstGame() ? 12 : 9;
	const int highY  = isFirstGame() ? 135 : (isThirdGame() ? 170 : 134);
	for (int i = 0; i < 4; i++) {
		// 0xFFFF is how both generations spell "there is nothing that way".
		if (exits[i] < 0 || exits[i] == 0xFFFF)
			continue;
		Target target;
		target.name = kyraExitName(i);
		target.label = Common::String::format("to room %d", exits[i]);
		target.kind = kTargetExit;
		target.itemId = -1;
		target.held = false;
		target.slot = -1;
		switch (i) {
		case 0:
			target.x = CLIP(exitX[i], lowX, highX);
			target.y = northY;
			break;
		case 1:
			target.x = eastX;
			target.y = CLIP(exitY[i], lowY, highY);
			break;
		case 2:
			target.x = CLIP(exitX[i], lowX, highX);
			target.y = southY;
			break;
		default:
			target.x = westX;
			target.y = CLIP(exitY[i], lowY, highY);
			break;
		}
		out.push_back(target);
	}

	// The later games also mark doorways inside the picture, each leading the
	// way one of the four exits does.
	if (!isFirstGame()) {
		const KyraEngine_v2 *v2 = static_cast<const KyraEngine_v2 *>(_vm);
		for (int i = 0; i < v2->_specialExitCount && i < 5; i++) {
			// Where it goes is often left to the room's script, in which case
			// the matching compass exit says nothing.
			const int dir = v2->_specialExitTable[20 + i] / 2;
			Target target;
			target.name = "doorway";
			kyraNameTargets(seen, target.name);
			if (dir >= 0 && dir <= 3 && exits[dir] >= 0 && exits[dir] != 0xFFFF)
				target.label = Common::String::format("to room %d", exits[dir]);
			target.kind = kTargetExit;
			target.itemId = -1;
			target.held = false;
			target.slot = -1;
			target.x = (v2->_specialExitTable[0 + i] + v2->_specialExitTable[10 + i]) / 2;
			target.y = (v2->_specialExitTable[5 + i] + v2->_specialExitTable[15 + i]) / 2;
			out.push_back(target);
		}
	}
}

void KyraMcpBridge::collectInventory(Common::Array<Target> &out) const {
	if (!engineReady())
		return;
	Common::Array<Common::String> seen;
	auto add = [&](int item, int slot) {
		Target entry;
		entry.label = itemLabel(item);
		entry.name = kyraItemName(entry.label);
		if (entry.name.empty())
			entry.name = Common::String::format("item_%d", item);
		kyraNameTargets(seen, entry.name);
		entry.kind = kTargetCarried;
		entry.itemId = item;
		entry.x = entry.y = 0;
		entry.held = slot < 0;
		entry.slot = slot;
		out.push_back(entry);
	};
	for (int i = 0; i < slotCount(); i++) {
		const int item = slotItem(i);
		if (!kyraIsEmptyItem(item))
			add(item, i);
	}
	// The hand is carried too: an item is only ever in a box or in the hand.
	if (handItem() >= 0)
		add(handItem(), -1);
}

bool KyraMcpBridge::resolveTarget(const Common::String &name, Target &out) const {
	const Common::String wanted = MCP::McpBridge::normalizeActionName(name);
	Common::Array<Target> targets;
	collectTargets(targets);
	for (uint i = 0; i < targets.size(); i++) {
		if (MCP::McpBridge::normalizeActionName(targets[i].name) == wanted) {
			out = targets[i];
			return true;
		}
	}
	Common::Array<Target> carried;
	collectInventory(carried);
	for (uint i = 0; i < carried.size(); i++) {
		if (MCP::McpBridge::normalizeActionName(carried[i].name) == wanted) {
			out = carried[i];
			return true;
		}
	}
	return false;
}

Common::String KyraMcpBridge::namesHere() const {
	Common::String here;
	Common::Array<Target> targets;
	collectTargets(targets);
	collectInventory(targets);
	for (uint i = 0; i < targets.size(); i++) {
		if (!here.empty())
			here += ", ";
		here += targets[i].name;
	}
	return here.empty() ? Common::String("nothing") : here;
}

// ---------------------------------------------------------------------------
// The inventory boxes
// ---------------------------------------------------------------------------

int KyraMcpBridge::boxCount() const {
	return 10;
}

int KyraMcpBridge::slotCount() const {
	// Kyrandia 2 keeps twenty and shows ten, turning them five at a time with
	// the scroll beside the boxes.
	return _vm != nullptr && _vm->game() == GI_KYRA2 ? 20 : 10;
}

int KyraMcpBridge::slotItem(int slot) const {
	if (!engineReady() || slot < 0 || slot >= slotCount())
		return -1;
	if (isFirstGame())
		return static_cast<const KyraEngine_LoK *>(_vm)->_currentCharacter->inventoryItems[slot];
	const int item = static_cast<const KyraEngine_v2 *>(_vm)->_mainCharacter.inventory[slot];
	return kyraIsEmptyItem(item) ? -1 : item;
}

const Button *KyraMcpBridge::boxButton(int slot) const {
	if (!engineReady() || slot < 0 || slot >= boxCount())
		return nullptr;
	// Each game numbers its box buttons from a different base, and each
	// button's callback reads the slot back as index - base.
	if (isFirstGame()) {
		const KyraEngine_LoK *lok = static_cast<const KyraEngine_LoK *>(_vm);
		for (int i = 0; i < 15; i++) {
			if (lok->_buttonData[i].index == slot + 2)
				return &lok->_buttonData[i];
		}
		return nullptr;
	}
	if (isThirdGame())
		return &static_cast<const KyraEngine_MR *>(_vm)->_mainButtonData[4 + slot];
	return &static_cast<const KyraEngine_HoF *>(_vm)->_inventoryButtons[5 + slot];
}

bool KyraMcpBridge::boxCenter(int slot, int &x, int &y) const {
	const Button *button = boxButton(slot);
	if (button == nullptr || button->width == 0)
		return false;
	x = button->x + button->width / 2;
	y = button->y + button->height / 2;
	return true;
}

int KyraMcpBridge::freeSlots() const {
	int free = 0;
	for (int i = 0; i < slotCount(); i++) {
		if (slotItem(i) < 0)
			free++;
	}
	return free;
}

int KyraMcpBridge::findItemSlot(int itemId) const {
	for (int i = 0; i < slotCount(); i++) {
		if (slotItem(i) == itemId)
			return i;
	}
	return -1;
}

bool KyraMcpBridge::inventoryShown() const {
	if (!isThirdGame())
		return true;
	return static_cast<const KyraEngine_MR *>(_vm)->_inventoryState;
}

// ---------------------------------------------------------------------------
// The step machine
// ---------------------------------------------------------------------------

int KyraMcpBridge::slotFor(int itemId, int slot) const {
	if (slot >= 0 && slotItem(slot) == itemId)
		return slot;
	return findItemSlot(itemId);
}

void KyraMcpBridge::queueStep(StepKind kind, int itemId, int slot, int x, int y) {
	Step step;
	step.kind = kind;
	step.itemId = itemId;
	step.slot = slot;
	step.x = x;
	step.y = y;
	step.attempts = 0;
	_steps.push_back(step);
}

void KyraMcpBridge::pointAt(int x, int y) {
	injectMouseMove(x, y);
	_passesSinceInput = 0;
	_nameClicked = true;
}

void KyraMcpBridge::pointAndClick(int x, int y) {
	// A real mouse hovers before it clicks, and the later games decide what a
	// click means from the cursor the hover set.
	pointAt(x, y);
	_pendingClick = true;
	_pendingX = x;
	_pendingY = y;
}

void KyraMcpBridge::abandonSteps(const Common::String &why) {
	debug(1, "mcp: kyra action stopped: %s", why.c_str());
	_stepError = why;
	_steps.clear();
	_pendingClick = false;
}

bool KyraMcpBridge::clickBoxOrReveal(int slot, uint &attempts) {
	// Kyrandia 3 draws its boxes only while the pointer rests at the bottom of
	// the screen, and ignores a click on a box it has not drawn.
	if (!inventoryShown()) {
		if (++attempts > kMaxStepAttempts) {
			abandonSteps("the inventory would not open");
			return false;
		}
		pointAt(160, 196);
		return false;
	}
	// Kyrandia 2 shows ten of its twenty slots; the scroll turns them.
	if (slot >= boxCount()) {
		if (++attempts > kMaxStepAttempts * 2) {
			abandonSteps("the inventory would not turn to that item");
			return false;
		}
		const Button &scroll = static_cast<const KyraEngine_HoF *>(_vm)->_inventoryButtons[4];
		pointAndClick(scroll.x + scroll.width / 2, scroll.y + scroll.height / 2);
		return false;
	}
	int x = 0, y = 0;
	if (!boxCenter(slot, x, y)) {
		abandonSteps("that inventory box cannot be clicked");
		return false;
	}
	if (++attempts > kMaxStepAttempts) {
		abandonSteps("the inventory box did not take the click");
		return false;
	}
	pointAndClick(x, y);
	return true;
}

void KyraMcpBridge::runSteps() {
	if (_pendingClick) {
		if (_passesSinceInput < kPointPasses)
			return;
		_pendingClick = false;
		injectMouseClick(_pendingX, _pendingY, "left", false);
		_passesSinceInput = 0;
		return;
	}
	if (_steps.empty() || _passesSinceInput < kAfterClickPasses || !playerHasControl())
		return;

	Step &step = _steps[0];
	switch (step.kind) {
	case kStepStowHand: {
		if (handItem() < 0) {
			_steps.remove_at(0);
			return;
		}
		// A free box on screen if there is one; otherwise, in Kyrandia 2, a
		// free slot among the ten that are turned away.
		int slot = -1;
		for (int i = 0; i < slotCount() && slot < 0; i++) {
			if (slotItem(i) < 0)
				slot = i;
		}
		if (slot < 0) {
			abandonSteps("every inventory box is full");
			return;
		}
		clickBoxOrReveal(slot, step.attempts);
		return;
	}

	case kStepHoldItem: {
		if (handItem() == step.itemId) {
			_steps.remove_at(0);
			return;
		}
		if (handItem() >= 0) {
			// Something else is in the hand: it goes away first.
			Step stow;
			stow.kind = kStepStowHand;
			stow.itemId = -1;
			stow.slot = -1;
			stow.x = stow.y = 0;
			stow.attempts = 0;
			_steps.insert_at(0, stow);
			return;
		}
		const int slot = slotFor(step.itemId, step.slot);
		if (slot < 0) {
			abandonSteps("that item is no longer carried");
			return;
		}
		clickBoxOrReveal(slot, step.attempts);
		return;
	}

	case kStepClickBox: {
		const int slot = slotFor(step.itemId, step.slot);
		if (slot < 0) {
			abandonSteps("that item is no longer carried");
			return;
		}
		if (clickBoxOrReveal(slot, step.attempts))
			_steps.remove_at(0);
		return;
	}

	case kStepClickAt:
		// Kyrandia 3's boxes cover the bottom of the picture while they are
		// shown, and a click there goes to them; moving the pointer up puts
		// them away first.
		if (!inventoryShown() || !isThirdGame()) {
			pointAndClick(step.x, step.y);
			_steps.remove_at(0);
			return;
		}
		if (++step.attempts > kMaxStepAttempts) {
			abandonSteps("the inventory would not close");
			return;
		}
		pointAt(step.x, MIN(step.y, 100));
		return;

	default:
		_steps.remove_at(0);
		return;
	}
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void KyraMcpBridge::injectKey(const Common::KeyState &ks) {
	Common::Event down;
	down.type = Common::EVENT_KEYDOWN;
	down.kbd = ks;
	g_system->getEventManager()->pushEvent(down);
	Common::Event up;
	up.type = Common::EVENT_KEYUP;
	up.kbd = ks;
	g_system->getEventManager()->pushEvent(up);
}

void KyraMcpBridge::injectMouseMove(int x, int y) {
	Common::Event move;
	move.type = Common::EVENT_MOUSEMOVE;
	move.mouse = Common::Point(x, y);
	g_system->getEventManager()->pushEvent(move);
	g_system->warpMouse(x, y);
}

void KyraMcpBridge::injectMouseClick(int x, int y, const Common::String &button, bool) {
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

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

int KyraMcpBridge::currentRoomForMessages() const {
	return roomNumber();
}

Common::String KyraMcpBridge::messageActorName(int actorId) const {
	if (!engineReady() || actorId < 0)
		return Common::String();
	if (actorId == 0)
		return isFirstGame() ? "brandon" : (isThirdGame() ? "malcolm" : "zanthia");
	if (isFirstGame())
		return Common::String::format("character_%d", actorId);
	// The later games name whoever else speaks by the animation file their
	// talking head is drawn from ("MARKO", "BONKBEND").
	const char *file = nullptr;
	if (isThirdGame())
		file = static_cast<const KyraEngine_MR *>(_vm)->_talkObjectList != nullptr ?
		       static_cast<const KyraEngine_MR *>(_vm)->_talkObjectList[actorId].filename : nullptr;
	else
		file = static_cast<const KyraEngine_HoF *>(_vm)->_talkObjectList != nullptr ?
		       static_cast<const KyraEngine_HoF *>(_vm)->_talkObjectList[actorId].filename : nullptr;
	const Common::String name = file != nullptr ? kyraTalkerName(file) : Common::String();
	return name.empty() ? Common::String::format("character_%d", actorId) : name;
}

void KyraMcpBridge::onGameText(const Common::String &text) {
	if (!isEnabled() || text.empty())
		return;
	// The first line printed in a room nobody has clicked in yet is its name.
	const int room = roomNumber();
	const int rooms = isFirstGame() ? static_cast<const KyraEngine_LoK *>(_vm)->_roomTableSize :
	                  static_cast<const KyraEngine_v2 *>(_vm)->_sceneListSize;
	if (room >= 0 && room < rooms) {
		if (room != _nameRoom) {
			_nameRoom = room;
			_nameRoomFrame = _frameCounter;
			_nameClicked = false;
		}
		if (!_nameClicked && _frameCounter - _nameRoomFrame <= kRoomNameFrames &&
		    !_roomNames.contains(room)) {
			const Common::String name = MCP::mcpCleanGameText(text);
			if (!name.empty())
				_roomNames[room] = name;
		}
	} else if (_frameCounter < kRoomNameFrames * 4) {
		// Printed while the game was still setting its first room up: kept
		// for that room, once there is one to give it to.
		_startupName = MCP::mcpCleanGameText(text);
	}
	onSystemLine(text);
}

void KyraMcpBridge::onGameSpeech(int speaker, const Common::String &text) {
	if (!isEnabled() || text.empty())
		return;
	onActorLine(speaker, text);
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

static const char *kyraKindName(int kind) {
	switch (kind) {
	case 0:  return "item";
	case 1:  return "character";
	case 2:  return "exit";
	default: return "carried";
	}
}

Common::JSONValue *KyraMcpBridge::toolState(const Common::JSONValue &, Common::String &errorOut) {
	if (!engineReady()) {
		errorOut = "state: the game is still starting up";
		return nullptr;
	}
	// Track arrivals here too, so a room entered while nobody was streaming
	// still learns its name.
	if (roomNumber() != _nameRoom) {
		_nameRoom = roomNumber();
		_nameRoomFrame = _frameCounter;
		_nameClicked = false;
	}
	if (!_startupName.empty()) {
		if (!_roomNames.contains(_nameRoom))
			_roomNames[_nameRoom] = _startupName;
		_startupName.clear();
	}

	Common::JSONObject out;

	const int roomId = roomNumber();
	Common::JSONObject room;
	room.setVal("id", mcpJsonInt(roomId));
	if (_roomNames.contains(roomId))
		room.setVal("name", mcpJsonString(_roomNames[roomId]));
	out.setVal("room", new Common::JSONValue(room));

	int x = 0, y = 0;
	heroPosition(x, y);
	Common::JSONObject position;
	position.setVal("x", mcpJsonInt(x));
	position.setVal("y", mcpJsonInt(y));
	out.setVal("position", new Common::JSONValue(position));

	out.setVal("can_act", mcpJsonBool(playerHasControl() && _steps.empty() && !_pendingClick));
	out.setVal("action_in_progress", mcpJsonBool(!_steps.empty() || _pendingClick));

	Common::JSONArray verbs;
	verbs.push_back(mcpJsonString("use"));
	verbs.push_back(mcpJsonString("pick_up"));
	out.setVal("verbs", new Common::JSONValue(verbs));

	Common::Array<Target> targets;
	collectTargets(targets);
	Common::JSONArray objects;
	for (uint i = 0; i < targets.size(); i++) {
		Common::JSONObject entry;
		entry.setVal("name", mcpJsonString(targets[i].name));
		entry.setVal("kind", mcpJsonString(kyraKindName(targets[i].kind)));
		entry.setVal("x", mcpJsonInt(targets[i].x));
		entry.setVal("y", mcpJsonInt(targets[i].y));
		if (targets[i].kind == kTargetExit)
			entry.setVal("pathway", mcpJsonBool(true));
		if (!targets[i].label.empty())
			entry.setVal("label", mcpJsonString(targets[i].label));
		objects.push_back(new Common::JSONValue(entry));
	}
	out.setVal("objects", new Common::JSONValue(objects));

	Common::Array<Target> carried;
	collectInventory(carried);
	Common::JSONArray inventory;
	for (uint i = 0; i < carried.size(); i++) {
		Common::JSONObject entry;
		entry.setVal("name", mcpJsonString(carried[i].name));
		entry.setVal("id", mcpJsonInt(carried[i].itemId));
		if (!carried[i].label.empty())
			entry.setVal("label", mcpJsonString(carried[i].label));
		if (carried[i].held) {
			entry.setVal("held", mcpJsonBool(true));
			out.setVal("held_item", mcpJsonString(carried[i].name));
		}
		inventory.push_back(new Common::JSONValue(entry));
	}
	out.setVal("inventory", new Common::JSONValue(inventory));
	out.setVal("free_boxes", mcpJsonInt(freeSlots()));

	Common::JSONArray messages;
	for (uint i = 0; i < _messages.size(); i++) {
		const Common::String text = MCP::mcpCleanGameText(safeUtf8(_messages[i].text));
		if (text.empty())
			continue;
		Common::JSONObject m;
		m.setVal("text", mcpJsonString(text));
		m.setVal("type", mcpJsonString(_messages[i].type));
		const Common::String actor = messageActorName(_messages[i].actorId);
		if (!actor.empty())
			m.setVal("actor", mcpJsonString(actor));
		messages.push_back(new Common::JSONValue(m));
	}
	_messages.clear();
	out.setVal("messages", new Common::JSONValue(messages));

	return new Common::JSONValue(out);
}

bool KyraMcpBridge::toolAct(const Common::JSONValue &args, Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "act: another action is already in progress";
		return false;
	}
	if (!engineReady()) {
		errorOut = "act: the game is still starting up";
		return false;
	}
	if (!args.isObject()) {
		errorOut = "act: arguments must be an object";
		return false;
	}
	const Common::JSONObject &a = args.asObject();

	Common::String verb = "use";
	if (a.contains("verb") && a["verb"]->isString())
		verb = MCP::McpBridge::normalizeActionName(a["verb"]->asString());
	// "take" and "get" arrive here already folded into "pick_up".
	auto argName = [&](const char *key) -> Common::String {
		if (!a.contains(key))
			return Common::String();
		if (a[key]->isString())
			return a[key]->asString();
		return Common::String();
	};
	const Common::String name1 = argName("target1");
	const Common::String name2 = argName("target2");
	const bool hasPoint = a.contains("x") && a.contains("y") &&
	                      a["x"]->isIntegerNumber() && a["y"]->isIntegerNumber();

	if (name1.empty() && !hasPoint) {
		errorOut = "act: 'target1' is required (or 'x' and 'y' for a spot in the room)";
		return false;
	}

	// Resolve before asking whether the game is listening: a name this room
	// does not have is wrong whatever the game happens to be doing.
	Target first, second;
	bool haveFirst = false, haveSecond = false;
	if (!name1.empty()) {
		if (!resolveTarget(name1, first)) {
			errorOut = Common::String::format("act: nothing here is called '%s'. Here: %s",
			                                  name1.c_str(), namesHere().c_str());
			return false;
		}
		haveFirst = true;
	}
	if (!name2.empty()) {
		if (!resolveTarget(name2, second)) {
			errorOut = Common::String::format("act: nothing here is called '%s'. Here: %s",
			                                  name2.c_str(), namesHere().c_str());
			return false;
		}
		haveSecond = true;
	}

	// The verb is checked after the names, so a wrong name is reported as
	// that whichever verb it came with.
	if (verb != "use" && verb != "pick_up") {
		errorOut = Common::String::format(
			"act: '%s' is not a verb here. This game has a single button: 'use' "
			"clicks a thing, which looks at it, speaks to it, opens it or goes "
			"through it - whatever it is for - and 'pick_up' picks an item up into "
			"an inventory box. Use a carried item on something with 'use' and "
			"'target2'.", verb.c_str());
		return false;
	}

	// What is used, and what it is used on. "use A on B" names the carried
	// item first; the other order is accepted too, as long as one of the two
	// is carried.
	bool withItem = false;
	int itemId = -1;
	int itemSlot = -1;
	Target onto;
	bool ontoPoint = false;
	int pointX = 0, pointY = 0;
	if (hasPoint) {
		pointX = CLIP<int>((int)a["x"]->asIntegerNumber(), 0, 319);
		pointY = CLIP<int>((int)a["y"]->asIntegerNumber(), 0, 199);
	}

	if (verb == "pick_up") {
		if (haveSecond || !haveFirst) {
			errorOut = "act: 'pick_up' names one item lying in the room as 'target1'";
			return false;
		}
		if (first.kind == kTargetCarried) {
			errorOut = Common::String::format("act: '%s' is already carried", first.name.c_str());
			return false;
		}
		if (first.kind != kTargetItem) {
			errorOut = Common::String::format(
				"act: '%s' is not an item that can be taken; use verb='use' to click it",
				first.name.c_str());
			return false;
		}
		// The item needs a box, and so does whatever is in the hand now.
		const int needed = handItem() >= 0 ? 2 : 1;
		if (freeSlots() < needed) {
			errorOut = "act: every inventory box is full; use something up first";
			return false;
		}
	} else {
		if (haveFirst && first.kind == kTargetCarried) {
			withItem = true;
			itemId = first.itemId;
			itemSlot = first.slot;
			if (haveSecond) {
				onto = second;
			} else if (hasPoint) {
				ontoPoint = true;
			} else {
				errorOut = Common::String::format(
					"act: say what to use '%s' on, as 'target2' (a thing in the room "
					"or another carried item)", first.name.c_str());
				return false;
			}
		} else if (haveSecond && second.kind == kTargetCarried) {
			withItem = true;
			itemId = second.itemId;
			itemSlot = second.slot;
			if (haveFirst)
				onto = first;
			else
				ontoPoint = true;
		} else if (haveSecond) {
			errorOut = "act: 'target2' must be an item you are carrying";
			return false;
		} else if (haveFirst) {
			onto = first;
		} else {
			ontoPoint = true;
		}
		if (withItem && !ontoPoint && onto.kind == kTargetCarried && onto.slot == itemSlot) {
			errorOut = "act: an item cannot be used on itself";
			return false;
		}
		if (!withItem && handItem() >= 0 && freeSlots() < 1) {
			errorOut = "act: the hand is full and every inventory box is too";
			return false;
		}
	}

	// Not refused while the game is busy with something of its own - an idle
	// animation holds the game loop for seconds at a time. The first click
	// simply waits for the game to come back, the way a player's would, and
	// the stream carries whatever is said meanwhile.
	if (!_steps.empty() || _pendingClick) {
		errorOut = "act: the previous action is still being carried out; call state "
		           "to follow it";
		return false;
	}

	_steps.clear();
	_stepError.clear();
	_skipStream = false;

	if (verb == "pick_up") {
		queueStep(kStepStowHand);
		queueStep(kStepClickAt, -1, -1, first.x, first.y);
		queueStep(kStepStowHand);
	} else if (!withItem) {
		queueStep(kStepStowHand);
		if (ontoPoint)
			queueStep(kStepClickAt, -1, -1, pointX, pointY);
		else
			queueStep(kStepClickAt, -1, -1, onto.x, onto.y);
		// Clicking an item lying in the room picks it up. Where it goes after
		// that is a box, which is where a carried item lives.
		queueStep(kStepStowHand);
	} else {
		queueStep(kStepHoldItem, itemId, itemSlot);
		if (!ontoPoint && onto.kind == kTargetCarried)
			queueStep(kStepClickBox, onto.itemId, onto.slot);
		else if (ontoPoint)
			queueStep(kStepClickAt, -1, -1, pointX, pointY);
		else
			queueStep(kStepClickAt, -1, -1, onto.x, onto.y);
		queueStep(kStepStowHand);
	}
	beginStream();
	return true;
}

bool KyraMcpBridge::toolAnswer(const Common::JSONValue &, Common::String &errorOut) {
	errorOut = "answer: this game never puts a numbered choice to the player";
	return false;
}

bool KyraMcpBridge::toolWalk(const Common::JSONValue &args, Common::String &errorOut) {
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
	if (!_steps.empty() || _pendingClick) {
		errorOut = "walk: the previous action is still being carried out; call state "
		           "to follow it";
		return false;
	}
	// Walking is a click on the floor with an empty hand: with an item in it
	// the same click would put the item down there.
	_steps.clear();
	_stepError.clear();
	_skipStream = false;
	queueStep(kStepStowHand);
	queueStep(kStepClickAt, -1, -1,
	          CLIP<int>((int)args.asObject()["x"]->asIntegerNumber(), 0, 319),
	          CLIP<int>((int)args.asObject()["y"]->asIntegerNumber(), 0, 199));
	beginStream();
	return true;
}

bool KyraMcpBridge::toolSkip(const Common::JSONValue &, Common::String &errorOut) {
	if (!_skipToolEnabled) {
		errorOut = "skip: tool is disabled (set mcp_skip_tool=true)";
		return false;
	}
	if (isStreaming()) {
		errorOut = "skip: another action is already in progress";
		return false;
	}
	_skipStream = true;
	_lastSkipFrame = 0;
	beginStream();
	return true;
}

Common::JSONValue *KyraMcpBridge::toolDebug(const Common::JSONValue &, Common::String &) {
	Common::JSONObject out;
	out.setVal("ready", mcpJsonBool(engineReady()));
	if (!engineReady())
		return new Common::JSONValue(out);
	out.setVal("game", mcpJsonInt(_vm->game()));
	out.setVal("room", mcpJsonInt(roomNumber()));
	out.setVal("can_act", mcpJsonBool(playerHasControl()));
	out.setVal("in_main_loop", mcpJsonBool(_vm->_mcpInMainLoop));
	out.setVal("frames_since_loop", mcpJsonInt(_frameCounter - _lastLoopFrame));
	out.setVal("item_in_hand", mcpJsonInt(handItem()));
	out.setVal("steps", mcpJsonInt(_steps.size()));
	out.setVal("inventory_shown", mcpJsonBool(inventoryShown()));
	Common::JSONArray slots;
	for (int i = 0; i < slotCount(); i++)
		slots.push_back(mcpJsonInt(slotItem(i)));
	out.setVal("slots", new Common::JSONValue(slots));
	if (!isFirstGame()) {
		const KyraEngine_v2 *v2 = static_cast<const KyraEngine_v2 *>(_vm);
		Common::JSONArray exits;
		exits.push_back(mcpJsonInt(v2->_sceneExit1));
		exits.push_back(mcpJsonInt(v2->_sceneExit2));
		exits.push_back(mcpJsonInt(v2->_sceneExit3));
		exits.push_back(mcpJsonInt(v2->_sceneExit4));
		out.setVal("scene_exits", new Common::JSONValue(exits));
		out.setVal("special_exits", mcpJsonInt(v2->_specialExitCount));
	}
	return new Common::JSONValue(out);
}

// ---------------------------------------------------------------------------
// What an agent reads about the tools
// ---------------------------------------------------------------------------

Common::String KyraMcpBridge::stateToolDescription() const {
	Common::String desc =
	    "The room as it is now: its id (and its name, once the game has "
	    "printed it), where the hero stands, everything in it that can be "
	    "clicked - items lying there, people, and the ways out (marked "
	    "'pathway') - what is carried in the inventory boxes (and in the "
	    "hand, marked 'held'), how many boxes are free, and every line said "
	    "or printed since the last read (reading them clears them). While "
	    "can_act is false the game is busy with something of its own: an "
	    "action sent then waits for it, and state keeps collecting what is "
	    "said, including the lines of an action whose call gave up waiting.";
	if (_skipToolEnabled)
		desc += " skip cuts a scene short.";
	desc += " The names here are the names act() takes.";
	return desc;
}

Common::String KyraMcpBridge::actToolDescription() const {
	return "Act on something state() named. The game has a single button. "
	       "'use' clicks the target, which does whatever it is for: looks at "
	       "it, speaks to somebody, goes through a way out. 'pick_up' puts an "
	       "item lying in the room into a free inventory box. To "
	       "use a carried item on something, give the item as 'target1' and "
	       "the thing - in the room, or another carried item - as 'target2'. "
	       "'x' and 'y' click a spot of the room that has no name. " +
	       streamingToolNote();
}

Common::String KyraMcpBridge::walkToolDescription() const {
	return "Walk the hero to a point by clicking the floor there, which is "
	       "what a player does. To leave the room, act() on one of the ways "
	       "out state() lists instead. " + streamingToolNote();
}

Common::String KyraMcpBridge::skipToolDescription() const {
	return "Cut short a scene that is playing itself out - a conversation, a "
	       "cutscene - by pressing the skip key line after line until the "
	       "player has control again. " + streamingToolNote();
}

Common::String KyraMcpBridge::debugToolDescription() const {
	return "Raw engine state: which of the three games this is, the room "
	       "number, whether the player has control, what is in the hand and "
	       "in each inventory slot.";
}

// ---------------------------------------------------------------------------
// Schemas
// ---------------------------------------------------------------------------

static Common::JSONValue *kyraArraySchema(const Common::JSONObject &props) {
	Common::JSONObject item;
	item.setVal("type", mcpJsonString("object"));
	item.setVal("properties", new Common::JSONValue(props));
	Common::JSONObject array;
	array.setVal("type", mcpJsonString("array"));
	array.setVal("items", new Common::JSONValue(item));
	return new Common::JSONValue(array);
}

void KyraMcpBridge::augmentStateSchema(Common::JSONObject &outputProps) {
	outputProps.setVal("can_act", mcpProp("boolean",
	    "Whether the game is taking input right now. An action sent while it "
	    "is false waits for the game before its first click."));
	outputProps.setVal("action_in_progress", mcpProp("boolean",
	    "True while an earlier action is still being carried out."));
	outputProps.setVal("verbs", mcpProp("array", "The verbs act() takes."));
	outputProps.setVal("held_item", mcpProp("string",
	    "The item in the hand, if one is: it is put back in a box before the "
	    "next action."));
	outputProps.setVal("free_boxes", mcpProp("integer",
	    "How many inventory boxes are empty."));

	Common::JSONObject obj;
	obj.setVal("name", mcpProp("string", "Name to use in act()."));
	obj.setVal("kind", mcpProp("string", "'item', 'character' or 'exit'."));
	obj.setVal("label", mcpProp("string", "What the game itself calls it, where it does."));
	obj.setVal("x", mcpProp("integer", "Where it is, in game coordinates."));
	obj.setVal("y", mcpProp("integer", "Where it is, in game coordinates."));
	obj.setVal("pathway", mcpProp("boolean", "Present when it leads out of the room."));
	outputProps.setVal("objects", kyraArraySchema(obj));

	Common::JSONObject item;
	item.setVal("name", mcpProp("string", "Name to use in act()."));
	item.setVal("id", mcpProp("integer", "The game's number for the item."));
	item.setVal("label", mcpProp("string", "What the game itself calls it."));
	item.setVal("held", mcpProp("boolean", "Present when it is in the hand rather than a box."));
	outputProps.setVal("inventory", kyraArraySchema(item));
}

void KyraMcpBridge::augmentChangesSchema(Common::JSONObject &props) {
	props.setVal("room_name", mcpProp("string",
	    "Name of the new room, when it changed and the game has printed it."));
	props.setVal("can_act", mcpProp("boolean",
	    "Whether the game is taking input now the action is over."));
	props.setVal("held_item", mcpProp("string",
	    "The item left in the hand, when one is (every box was full)."));
	props.setVal("error", mcpProp("string",
	    "Why the action stopped short of its last click, when it did."));
	Common::JSONObject changed;
	changed.setVal("name", mcpProp("string", "The thing that appeared or went."));
	changed.setVal("new_state", mcpProp("string", "'present' or 'gone'."));
	props.setVal("objects_changed", kyraArraySchema(changed));
}

void KyraMcpBridge::augmentActSchema(Common::JSONObject &props) {
	props.setVal("verb", mcpProp("string",
	    "'use' (click the target, or use target1 on target2) or 'pick_up' "
	    "(put an item lying in the room into an inventory box)."));
	props.setVal("x", mcpProp("integer",
	    "X of a spot in the room to click, instead of a named target (or as "
	    "what a carried target1 is used on)."));
	props.setVal("y", mcpProp("integer", "Y of that spot."));
}

Common::JSONValue *KyraMcpBridge::buildDebugSchema() const {
	Common::JSONObject props;
	return Networking::mcpObjectSchema(props);
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

void KyraMcpBridge::snapshotPreAction() {
	_ssePreRoom = roomNumber();
	heroPosition(_ssePreX, _ssePreY);
	_ssePreHand = handItem();
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
	_sseTrackX = _ssePreX;
	_sseTrackY = _ssePreY;
	_sseTrackHand = _ssePreHand;
	_sseTrackSteps = _steps.size();
}

// Names in `after` that are not in `before`, counting duplicates.
static void kyraNamesAdded(const Common::Array<Common::String> &before,
                           const Common::Array<Common::String> &after,
                           Common::JSONArray &added) {
	Common::Array<Common::String> pool = before;
	for (uint i = 0; i < after.size(); i++) {
		bool found = false;
		for (uint j = 0; j < pool.size(); j++) {
			if (pool[j] == after[i]) {
				pool.remove_at(j);
				found = true;
				break;
			}
		}
		if (!found)
			added.push_back(mcpJsonString(after[i]));
	}
}

Common::JSONObject KyraMcpBridge::buildStateChanges() const {
	Common::JSONObject out;

	const int room = roomNumber();
	if (room != _ssePreRoom) {
		out.setVal("room_changed", mcpJsonInt(room));
		if (_roomNames.contains(room))
			out.setVal("room_name", mcpJsonString(_roomNames[room]));
	}

	int x = 0, y = 0;
	heroPosition(x, y);
	Common::JSONObject pos;
	pos.setVal("x", mcpJsonInt(x));
	pos.setVal("y", mcpJsonInt(y));
	out.setVal("position", new Common::JSONValue(pos));

	Common::Array<Target> carried;
	collectInventory(carried);
	Common::Array<Common::String> held;
	for (uint i = 0; i < carried.size(); i++)
		held.push_back(carried[i].name);
	Common::JSONArray added, removed;
	kyraNamesAdded(_ssePreInventory, held, added);
	kyraNamesAdded(held, _ssePreInventory, removed);
	out.setVal("inventory_added", new Common::JSONValue(added));
	out.setVal("inventory_removed", new Common::JSONValue(removed));
	if (handItem() >= 0)
		out.setVal("held_item", mcpJsonString(itemName(handItem())));

	// Only within the room the action started in: a new room's contents are
	// all new, and state() is the place to read them.
	if (room == _ssePreRoom) {
		Common::Array<Target> targets;
		collectTargets(targets);
		Common::Array<Common::String> now;
		for (uint i = 0; i < targets.size(); i++)
			now.push_back(targets[i].name);
		Common::JSONArray appeared, gone;
		kyraNamesAdded(_ssePreTargets, now, appeared);
		kyraNamesAdded(now, _ssePreTargets, gone);
		Common::JSONArray changed;
		for (uint i = 0; i < appeared.size(); i++) {
			Common::JSONObject c;
			c.setVal("name", mcpJsonString(appeared[i]->asString()));
			c.setVal("new_state", mcpJsonString("present"));
			changed.push_back(new Common::JSONValue(c));
		}
		for (uint i = 0; i < gone.size(); i++) {
			Common::JSONObject c;
			c.setVal("name", mcpJsonString(gone[i]->asString()));
			c.setVal("new_state", mcpJsonString("gone"));
			changed.push_back(new Common::JSONValue(c));
		}
		for (uint i = 0; i < appeared.size(); i++)
			delete appeared[i];
		for (uint i = 0; i < gone.size(); i++)
			delete gone[i];
		if (!changed.empty())
			out.setVal("objects_changed", new Common::JSONValue(changed));
	}

	Common::JSONArray messages;
	for (uint i = 0; i < _sseMessages.size(); i++) {
		const Common::String text = MCP::mcpCleanGameText(safeUtf8(_sseMessages[i].text));
		if (text.empty())
			continue;
		Common::JSONObject m;
		m.setVal("text", mcpJsonString(text));
		m.setVal("type", mcpJsonString(_sseMessages[i].type));
		const Common::String actor = messageActorName(_sseMessages[i].actorId);
		if (!actor.empty())
			m.setVal("actor", mcpJsonString(actor));
		messages.push_back(new Common::JSONValue(m));
	}
	out.setVal("messages", new Common::JSONValue(messages));

	if (!_stepError.empty())
		out.setVal("error", mcpJsonString(_stepError));
	out.setVal("can_act", mcpJsonBool(playerHasControl()));
	return out;
}

bool KyraMcpBridge::isActionDone() const {
	if (!_steps.empty() || _pendingClick)
		return false;
	if (_skipStream)
		return playerHasControl();
	return _passesSinceInput >= kAfterClickPasses && playerHasControl();
}

bool KyraMcpBridge::hasPendingQuestion() const {
	return false;
}

void KyraMcpBridge::pumpStreamTrack() {
	const int room = roomNumber();
	int x = 0, y = 0;
	heroPosition(x, y);
	const int hand = handItem();
	if (room != _sseTrackRoom || x != _sseTrackX || y != _sseTrackY ||
	    hand != _sseTrackHand || _steps.size() != _sseTrackSteps || _pendingClick) {
		_sseTrackRoom = room;
		_sseTrackX = x;
		_sseTrackY = y;
		_sseTrackHand = hand;
		_sseTrackSteps = _steps.size();
		_sseLastEventFrame = _frameCounter;
	}
}

} // End of namespace Kyra
