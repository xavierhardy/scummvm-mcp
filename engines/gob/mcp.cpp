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

#include "gob/mcp.h"

#include "gob/mcp_names.h"

#include "gob/draw.h"
#include "gob/game.h"
#include "gob/global.h"
#include "gob/gob.h"
#include "gob/inter.h"
#include "gob/resources.h"
#include "gob/script.h"
#include "gob/util.h"
#include "gob/variables.h"
#include "gob/video.h"
#include "gob/videoplayer.h"

#include "common/debug.h"
#include "common/events.h"
#include "common/system.h"

namespace Gob {

using Networking::mcpJsonString;
using Networking::mcpJsonInt;
using Networking::mcpJsonBool;
using Networking::mcpProp;
using Networking::mcpObjectSchema;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

GobMcpBridge::GobMcpBridge(GobEngine *vm)
	: MCP::McpBridge(vm),
	  _vm(vm),
	  _inPump(false),
	  _lastFrameMs(0),
	  _lastInputPollFrame(0),
	  _lastAnyPollFrame(0),
	  _lastPushedFrame(0),
	  _nextDrawSeq(1),
	  _sweepIndex(-1),
	  _sweepMoveFrame(0),
	  _sweepMoveSeq(0),
	  _sweepEndFrame(0),
	  _sweepReturnX(0),
	  _sweepReturnY(0),
	  _lastStepFrame(0),
	  _lastDrawnTextFrame(0),
	  _inventoryKnown(false),
	  _inventoryDirty(true),
	  _invState(kInvIdle),
	  _invStateFrame(0),
	  _invMoveSeq(0),
	  _invSlotIndex(0),
	  _sseActionStarted(false),
	  _sseSkipFast(false) {
}

GobMcpBridge::~GobMcpBridge() {
}

GobMcpBridge *GobMcpBridge::create(GobEngine *vm) {
	GobMcpBridge *bridge = new GobMcpBridge(vm);
	bridge->init();
	return bridge;
}

// ---------------------------------------------------------------------------
// Engine hooks
// ---------------------------------------------------------------------------

bool GobMcpBridge::engineReady() const {
	return _vm->_game && _vm->_game->_hotspots && _vm->_util && _vm->_draw &&
	       _vm->_global && _vm->_inter && _vm->_inter->_variables &&
	       _vm->_video && _vm->_vidPlayer;
}

void GobMcpBridge::pumpFromInput() {
	if (!isEnabled() || _inPump)
		return;
	_inPump = true;
	uint32 now = g_system->getMillis();
	if (now - _lastFrameMs >= kFrameMs) {
		_lastFrameMs = now;
		pump();
	} else {
		pumpTransportOnly();
	}
	_inPump = false;
}

void GobMcpBridge::onInputPoll(uint8 handleMouse) {
	if (!isEnabled())
		return;
	_lastAnyPollFrame = _frameCounter;
	if (handleMouse)
		_lastInputPollFrame = _frameCounter;
}

void GobMcpBridge::onTextDrawn(const char *text, int16 x, int16 y, int16 surface) {
	if (!isEnabled() || !text || !text[0])
		return;
	(void)surface;
	_lastDrawnTextFrame = _frameCounter;
	DrawnText dt;
	// The game's own code page, decoded once here so names, labels and dialogue
	// all reach the client as UTF-8.
	dt.text = mcpGobTextToUtf8(text);
	dt.x = x;
	dt.y = y;
	dt.frame = _frameCounter;
	dt.seq = _nextDrawSeq++;
	_drawnTexts.push_back(dt);
	const uint kMaxDrawnTexts = 256;
	if (_drawnTexts.size() > kMaxDrawnTexts)
		_drawnTexts.remove_at(0);
}

// The hover label of the hotspot currently under the virtual cursor, if any
// is known. Used to keep the status-bar name a real player sees while
// pointing from surfacing as a "message".
Common::String GobMcpBridge::hoveredLabel() const {
	if (!engineReady())
		return Common::String();
	int mx = _vm->_global->_inter_mouseX;
	int my = _vm->_global->_inter_mouseY;
	Common::Array<Hotspots::McpDesc> spots;
	collectHotspots(spots);
	for (uint i = 0; i < spots.size(); i++) {
		const Hotspots::McpDesc &d = spots[i];
		if (!isPointableHotspot(d))
			continue;
		if (mx < d.left || mx > d.right || my < d.top || my > d.bottom)
			continue;
		Common::String label;
		if (cachedName(d, label) && !label.empty())
			return label;
	}
	return Common::String();
}

// Coalesce the segments drawn on earlier frames into lines (one line per text
// row, segments joined left to right). Rows drawn while the hover sweep is
// parked on a hotspot become that hotspot's name; everything else is queued as
// a message. Hover names redraw every frame; the consecutive-duplicate check
// drops the repeats.
void GobMcpBridge::pumpGame() {
	// Flush finished rows of drawn text.
	while (!_drawnTexts.empty() && _drawnTexts[0].frame < _frameCounter) {
		uint32 frame = _drawnTexts[0].frame;
		uint32 seq = _drawnTexts[0].seq;
		int16 rowY = _drawnTexts[0].y;
		Common::String line;
		uint i = 0;
		while (i < _drawnTexts.size() && _drawnTexts[i].frame == frame &&
		       ABS(_drawnTexts[i].y - rowY) <= 4) {
			if (!line.empty())
				line += " ";
			line += _drawnTexts[i].text;
			i++;
		}
		while (i-- > 0)
			_drawnTexts.remove_at(0);
		line = MCP::mcpCleanGameText(safeUtf8(line));
		if (line.empty())
			continue;
		if (_sweepIndex >= 0 || _frameCounter - _sweepEndFrame < 6) {
			// Attribute the row to the hovered hotspot when it was drawn
			// after the sweep cursor moved there; every other row seen around
			// a sweep is a stale redraw and is dropped rather than surfaced
			// as a message.
			if (_sweepIndex >= 0 && seq >= _sweepMoveSeq)
				_sweepCaptured = line;
			continue;
		}
		if (_invState != kInvIdle) {
			// The overlay draws the name of the current item in its status
			// area (on open, and again whenever the cursor lands on a slot).
			// Keep the latest; the refresh attributes it to the slot it is
			// dwelling on as it steps through them.
			_invCapturedLabel = line;
			continue;
		}
		if (line == _lastPushedText)
			continue;
		// The status-bar name of the hotspot the cursor is on is UI, not
		// dialogue.
		if (line == MCP::mcpCleanGameText(safeUtf8(hoveredLabel())))
			continue;
		// The "Use <item> on ..." command bar the game paints while a use
		// action drags the item around is UI, not dialogue.
		if (!_useCmdLabel.empty()) {
			Common::String low = line;
			low.toLowercase();
			Common::String lbl = _useCmdLabel;
			lbl.toLowercase();
			if (low == lbl || low.hasPrefix("use " + lbl))
				continue;
		}
		// Suppress the game's whole-subtitle redraws: a line seen again within
		// a short window is the same block being re-drawn, not a new line.
		const uint32 kRedrawWindow = 100; // frames (~4s at 40ms/frame)
		bool recentDup = false;
		for (uint r = 0; r < _recentLines.size(); r++) {
			if (_recentLines[r].text == line &&
			    _frameCounter - _recentLines[r].frame < kRedrawWindow) {
				recentDup = true;
				break;
			}
		}
		if (recentDup)
			continue;
		_recentLines.push_back({line, _frameCounter});
		if (_recentLines.size() > 24)
			_recentLines.remove_at(0);
		_lastPushedText = line;
		_lastPushedFrame = _frameCounter;
		pushMessage("text", -1, line);
	}

	pumpSteps();
	pumpInventoryRefresh();
	pumpNameSweep();
}

// Play out the synthetic-input queue, one step per frame.
void GobMcpBridge::pumpSteps() {
	if (_steps.empty())
		return;
	Step s = _steps[0];
	switch (s.kind) {
	case kStepHover: {
		// Put the cursor on the target and let the game notice it before the
		// button goes down. See the kStepHover comment in the header: pressing
		// in the same breath as the move leaves the scripts acting on whatever
		// the cursor was previously over.
		if (_frameCounter < s.notBeforeFrame)
			return;
		Step &st = _steps[0];
		if (!st.hoverSent) {
			pushMouseMove(st.x, st.y);
			st.hoverSent  = true;
			st.hoverFrame = _frameCounter;
			return; // never press on the same frame as the move
		}
		// Wait for the engine to register the hover, but never longer than
		// kHoverMaxFrames: a click on bare floor enters no hotspot at all.
		if (_frameCounter - st.hoverFrame < kHoverMaxFrames &&
		    !hoverRegistered(st.x, st.y))
			return;
		break;
	}
	case kStepPress:
		if (_frameCounter < s.notBeforeFrame)
			return;
		pushButton(true, s.right, s.x, s.y);
		break;
	case kStepRelease:
		// Hold the button for a few frames even when the press's exact frame
		// was not known at queue time.
		if (_frameCounter < s.notBeforeFrame || _frameCounter - _lastStepFrame < 3)
			return;
		pushButton(false, s.right, s.x, s.y);
		break;
	case kStepClickItem: {
		// Click the inventory slot with this id, resolved against the live
		// overlay hotspots (their position depends on where it was opened).
		Common::Array<Hotspots::McpDesc> spots;
		collectHotspots(spots);
		for (uint i = 0; i < spots.size(); i++) {
			if (!isPointableHotspot(spots[i]))
				continue;
			if ((spots[i].id & 0x0FFF) != s.slotId)
				continue;
			int cx = (spots[i].left + spots[i].right) / 2;
			int cy = (spots[i].top + spots[i].bottom) / 2;
			// Replace this step with a plain click at the slot.
			_steps.remove_at(0);
			Step hover;
			hover.kind = kStepHover;
			hover.x = cx;
			hover.y = cy;
			hover.right = false;
			hover.slotId = 0;
			hover.notBeforeFrame = _frameCounter;
			_steps.insert_at(0, hover);
			Step press = hover;
			press.kind = kStepPress;
			press.notBeforeFrame = _frameCounter;
			_steps.insert_at(1, press);
			Step release = press;
			release.kind = kStepRelease;
			release.notBeforeFrame = _frameCounter + 3;
			_steps.insert_at(2, release);
			return;
		}
		// Slot not on screen (yet); give the overlay a moment, then give up.
		if (_frameCounter >= s.notBeforeFrame) {
			debug(1, "mcp: inventory slot %u never appeared, dropping steps", s.slotId);
			_steps.clear();
		}
		return;
	}
	case kStepWaitOverlay:
		if (!inventoryOverlayOpen() && _frameCounter < s.notBeforeFrame)
			return;
		break;
	case kStepWaitWorld:
		if (inventoryOverlayOpen() && _frameCounter < s.notBeforeFrame)
			return;
		break;
	case kStepWaitReady:
		// Hold the click back until the script would honour it. The safety
		// timeout matters: on a screen where the animation flag never clears,
		// clicking late still beats never clicking at all.
		if (!readyForClick() && _frameCounter < s.notBeforeFrame)
			return;
		break;
	default:
		break;
	}
	debug(2, "mcp: step kind=%d (%d,%d) done at frame %u", (int)s.kind, s.x, s.y, _frameCounter);
	_steps.remove_at(0);
	_lastStepFrame = _frameCounter;
}

// ---------------------------------------------------------------------------
// Hover-sweep naming
// ---------------------------------------------------------------------------

// Woodruff (and the other Gob games) show an object's name while the cursor
// hovers it: the hotspot's enter/position scripts draw it into the status bar.
// While the game is idle the bridge replays exactly that — park the virtual
// cursor on each not-yet-named hotspot for a few frames and record what the
// game draws. Learned names are cached per TOT/hotspot.

bool GobMcpBridge::isPointableHotspot(const Hotspots::McpDesc &d) const {
	if (d.window != 0)
		return false;
	if (d.left > d.right || d.top > d.bottom)
		return false;
	// Parked hotspots sit at x >= 1000, off the 640x480 screen.
	if (d.right >= _vm->_width || d.bottom >= _vm->_height)
		return false;
	// Only mouse zones; input fields and key-only entries are not pointable.
	if (d.type != (uint16)Hotspots::kTypeMove && d.type != (uint16)Hotspots::kTypeClick &&
	    d.type != (uint16)Hotspots::kTypeClickEnter)
		return false;
	// Screen-wide catch-all zones (the walk area) are served by `walk`.
	uint32 area = (uint32)(d.right - d.left + 1) * (uint32)(d.bottom - d.top + 1);
	if (area > (uint32)(_vm->_width * _vm->_height) * 6 / 10)
		return false;
	return true;
}

Common::String GobMcpBridge::nameKeyFor(const Hotspots::McpDesc &d) const {
	return Common::String::format("%s#%u", _vm->_game->_curTotFile.c_str(), d.id & 0x0FFF);
}

bool GobMcpBridge::cachedName(const Hotspots::McpDesc &d, Common::String &out) const {
	Common::HashMap<Common::String, Common::String>::const_iterator it =
	    _nameCache.find(nameKeyFor(d));
	if (it == _nameCache.end())
		return false;
	out = it->_value;
	return true;
}

void GobMcpBridge::cancelNameSweep() {
	if (_sweepIndex >= 0)
		_sweepEndFrame = _frameCounter;
	_sweepIndex = -1;
	_sweepSpots.clear();
	_sweepCaptured.clear();
}

void GobMcpBridge::pumpNameSweep() {
	// Never take the cursor away while the game is animating: the status bar it
	// paints then belongs to the action, not to whatever the sweep points at.
	if (!engineReady() || !readyForClick() || !_steps.empty())
		return;
	if (_invState != kInvIdle)
		return;
	// During a stream, only sweep once the action has settled into idle.
	if (isStreaming() && _sseDoneAtFrame == 0)
		return;
	// Keep out of windows where dialogue text is still appearing, so a spoken
	// line can never be mistaken for a hover name.
	if (_sweepIndex < 0 && _frameCounter - _lastDrawnTextFrame < 8)
		return;

	if (_sweepIndex < 0) {
		// Anything new to name?
		Common::Array<Hotspots::McpDesc> spots;
		collectHotspots(spots);
		_sweepSpots.clear();
		Common::String dummy;
		for (uint i = 0; i < spots.size(); i++) {
			if (isPointableHotspot(spots[i]) && !cachedName(spots[i], dummy))
				_sweepSpots.push_back(spots[i]);
		}
		if (_sweepSpots.empty())
			return;
		const uint kMaxCache = 512;
		if (_nameCache.size() > kMaxCache)
			_nameCache.clear();
		_sweepReturnX = _vm->_global->_inter_mouseX;
		// Never park the cursor back on the top edge: that is the hover zone
		// that opens the game's own menu bar.
		_sweepReturnY = MAX<int>(_vm->_global->_inter_mouseY, 16);
		_sweepIndex = 0;
		_sweepCaptured.clear();
		_sweepMoveFrame = _frameCounter;
		_sweepMoveSeq = _nextDrawSeq;
		pushMouseMove((_sweepSpots[0].left + _sweepSpots[0].right) / 2,
		              (_sweepSpots[0].top + _sweepSpots[0].bottom) / 2);
		debug(2, "mcp: name sweep started (%u hotspots)", _sweepSpots.size());
		return;
	}

	// Give the hover scripts a few frames to draw the name. A hotspot that has
	// painted nothing by then waits out a longer grace window first: what gets
	// cached below is final ("this one has no name"), so a label that merely
	// arrived late leaves a perfectly nameable hotspot stuck as hotspot_<id> for
	// the rest of the room. Hotspots that do paint promptly are unaffected.
	const uint32 kSweepDwellFrames = 4;
	const uint32 kSweepGraceFrames = 16;
	uint32 dwelledFrames = _frameCounter - _sweepMoveFrame;
	if (dwelledFrames < kSweepDwellFrames)
		return;
	if (_sweepCaptured.empty() && dwelledFrames < kSweepGraceFrames)
		return;

	_nameCache[nameKeyFor(_sweepSpots[_sweepIndex])] = _sweepCaptured;
	debug(2, "mcp: name sweep: hotspot %u -> '%s'",
	      _sweepSpots[_sweepIndex].id & 0x0FFF, _sweepCaptured.c_str());
	_sweepCaptured.clear();

	_sweepIndex++;
	if ((uint)_sweepIndex >= _sweepSpots.size()) {
		pushMouseMove(_sweepReturnX, _sweepReturnY);
		cancelNameSweep();
		return;
	}
	_sweepMoveFrame = _frameCounter;
	_sweepMoveSeq = _nextDrawSeq;
	pushMouseMove((_sweepSpots[_sweepIndex].left + _sweepSpots[_sweepIndex].right) / 2,
	              (_sweepSpots[_sweepIndex].top + _sweepSpots[_sweepIndex].bottom) / 2);
}

// ---------------------------------------------------------------------------
// Inventory tracking (Woodruff)
// ---------------------------------------------------------------------------

// A right click anywhere in the world opens the inventory overlay: the item
// icons pop up around the click and hovering one shows its name in the status
// bar, exactly like the world hotspots. The refresh machine replays that
// whenever the inventory may have changed: open, hover each slot, close.

bool GobMcpBridge::inventoryOverlayOpen() const {
	return engineReady() && _vm->_game->_curTotFile.equalsIgnoreCase("MENU.tot");
}

void GobMcpBridge::pumpInventoryRefresh() {
	if (!engineReady() || _vm->getGameType() != kGameTypeWoodruff)
		return;
	if (isStreaming())
		return;
	// A tool click that is still waiting for the game to be ready must not
	// deadlock the refresh it is waiting on: keep going in that case, and let
	// the refresh's own clicks jump ahead of it (see queueOverlayClick()).
	if (!_steps.empty() && _steps[0].kind != kStepWaitReady)
		return;

	switch (_invState) {
	case kInvIdle:
		if (!_inventoryDirty)
			return;
		// Opening the game's own menu on top of a running animation is the most
		// intrusive thing the bridge does; wait until the game is truly idle.
		if (!readyForClick() || _sweepIndex >= 0)
			return;
		if (inventoryOverlayOpen())
			return; // the player-visible menu is open; not ours to drive
		if (_frameCounter - _lastDrawnTextFrame < 8)
			return;
		// Remember the world the refresh is leaving, so `state` keeps reporting
		// it while the overlay is up.
		_worldTot = _vm->_game->_curTotFile;
		buildObjectList(_worldObjects);
		// Open the overlay in the middle of the screen.
		queueOverlayClick(320, 240, true);
		_invState = kInvOpening;
		_invStateFrame = _frameCounter;
		_invCapturedLabel.clear();
		debug(2, "mcp: inventory refresh started");
		return;

	case kInvOpening:
		if (!inventoryOverlayOpen()) {
			// Either it never opened, or an empty inventory opened and closed
			// on its own before any slot could be hovered. Either way, after a
			// grace period conclude the inventory is empty and give up.
			if (_frameCounter - _invStateFrame > 50) {
				_inventory.clear();
				_inventoryKnown = true;
				_inventoryDirty = false;
				_invState = kInvIdle;
				_invStateFrame = _frameCounter;
			}
			return;
		}
		// Enumerate the item slots (everything pointable in the overlay). The
		// menu script needs a few frames after opening to draw its item icons
		// and register their hotspots, so keep scanning until they appear
		// rather than mistaking the still-populating overlay for an empty one.
		_invSlots.clear();
		_invPending.clear();
		{
			Common::Array<Hotspots::McpDesc> spots;
			collectHotspots(spots);
			for (uint i = 0; i < spots.size(); i++) {
				if (!isPointableHotspot(spots[i]))
					continue;
				_invSlots.push_back(spots[i]);
				InventoryItem item;
				item.slotId = spots[i].id & 0x0FFF;
				_invPending.push_back(item);
			}
		}
		_invSlotIndex = 0;
		if (_invSlots.empty()) {
			// No slots yet: give the overlay time to populate before deciding
			// the inventory is truly empty.
			if (_frameCounter - _invStateFrame < 40)
				return;
			// Empty inventory: dismiss the banner with a left click.
			queueOverlayClick(320, 400, false);
			_invState = kInvClosing;
			_invStateFrame = _frameCounter;
			return;
		}
		_invMoveSeq = _nextDrawSeq;
		pushMouseMove((_invSlots[0].left + _invSlots[0].right) / 2,
		              (_invSlots[0].top + _invSlots[0].bottom) / 2);
		_invState = kInvHovering;
		_invStateFrame = _frameCounter;
		return;

	case kInvHovering:
		if (_frameCounter - _invStateFrame < 12)
			return;
		// Record the name the overlay drew while dwelling on this slot.
		if (_invSlotIndex < _invPending.size() && !_invCapturedLabel.empty()) {
			_invPending[_invSlotIndex].label = _invCapturedLabel;
			_invCapturedLabel.clear();
		}
		_invSlotIndex++;
		if (_invSlotIndex >= _invSlots.size()) {
			// Done; close with a right click (toggles the overlay away).
			queueOverlayClick(320, 400, true);
			_invState = kInvClosing;
			_invStateFrame = _frameCounter;
			return;
		}
		_invMoveSeq = _nextDrawSeq;
		pushMouseMove((_invSlots[_invSlotIndex].left + _invSlots[_invSlotIndex].right) / 2,
		              (_invSlots[_invSlotIndex].top + _invSlots[_invSlotIndex].bottom) / 2);
		_invStateFrame = _frameCounter;
		return;

	case kInvClosing:
		if (inventoryOverlayOpen()) {
			if (_frameCounter - _invStateFrame > 75) {
				// Stuck in the overlay; try a plain left click to dismiss.
				queueOverlayClick(320, 400, false);
				_invStateFrame = _frameCounter;
			}
			return;
		}
		// Back in the world: publish what was learned.
		_inventory.clear();
		for (uint i = 0; i < _invPending.size(); i++) {
			InventoryItem item = _invPending[i];
			item.name = mcpGobObjectName(item.label);
			if (item.name.empty())
				item.name = Common::String::format("item_%u", item.slotId);
			// De-duplicate names (two boots -> boot, boot_2).
			uint n = 1;
			for (uint j = 0; j < _inventory.size(); j++)
				if (_inventory[j].name == item.name ||
				    _inventory[j].name.hasPrefix(item.name + "_"))
					n++;
			if (n > 1)
				item.name = Common::String::format("%s_%u", item.name.c_str(), n);
			_inventory.push_back(item);
		}
		_inventoryKnown = true;
		_inventoryDirty = false;
		_invState = kInvIdle;
		debug(2, "mcp: inventory refresh done (%u items)", _inventory.size());
		return;

	default:
		return;
	}
}

// ---------------------------------------------------------------------------
// Input injection
// ---------------------------------------------------------------------------

// Game coordinates -> event/screen coordinates (the inverse of
// Util::getMouseState()).
void GobMcpBridge::pushMouseMove(int gameX, int gameY) {
	int x = gameX - _vm->_video->_scrollOffsetX + _vm->_video->_screenDeltaX;
	int y = gameY - _vm->_video->_scrollOffsetY + _vm->_video->_screenDeltaY;
	x = CLIP<int>(x, 0, _vm->_width - 1);
	y = CLIP<int>(y, 0, _vm->_height - 1);
	Common::Event event;
	event.type = Common::EVENT_MOUSEMOVE;
	event.mouse = Common::Point(x, y);
	g_system->getEventManager()->pushEvent(event);
}

void GobMcpBridge::pushButton(bool down, bool right, int gameX, int gameY) {
	int x = CLIP<int>(gameX - _vm->_video->_scrollOffsetX + _vm->_video->_screenDeltaX,
	                  0, _vm->_width - 1);
	int y = CLIP<int>(gameY - _vm->_video->_scrollOffsetY + _vm->_video->_screenDeltaY,
	                  0, _vm->_height - 1);
	Common::Event event;
	if (right)
		event.type = down ? Common::EVENT_RBUTTONDOWN : Common::EVENT_RBUTTONUP;
	else
		event.type = down ? Common::EVENT_LBUTTONDOWN : Common::EVENT_LBUTTONUP;
	event.mouse = Common::Point(x, y);
	g_system->getEventManager()->pushEvent(event);
}

// Queue a click on behalf of a tool: hold it back until the game would honour
// it (see readyForClick()), then play it out. Everything an agent asks for goes
// through here; the bridge's own housekeeping clicks use queueClick() directly,
// since they run only when the game is already idle.
void GobMcpBridge::queueClickWhenReady(int gameX, int gameY, bool right) {
	cancelNameSweep();
	Step ready;
	ready.kind = kStepWaitReady;
	ready.x = ready.y = 0;
	ready.right = false;
	ready.slotId = 0;
	ready.notBeforeFrame = _frameCounter + 150; // ~6s safety timeout
	_steps.push_back(ready);
	queueClick(gameX, gameY, right);
}


// A click belonging to the inventory-overlay machine. It runs ahead of a tool
// click that is still waiting for the game to be ready — that click is waiting
// for this very overlay to close.
void GobMcpBridge::queueOverlayClick(int gameX, int gameY, bool right) {
	if (_steps.empty() || _steps[0].kind != kStepWaitReady) {
		queueClick(gameX, gameY, right);
		return;
	}
	Step hover;
	hover.kind = kStepHover;
	hover.x = gameX;
	hover.y = gameY;
	hover.right = right;
	hover.slotId = 0;
	hover.notBeforeFrame = _frameCounter;
	_steps.insert_at(0, hover);
	Step press = hover;
	press.kind = kStepPress;
	press.notBeforeFrame = _frameCounter;
	_steps.insert_at(1, press);
	Step release = press;
	release.kind = kStepRelease;
	release.notBeforeFrame = _frameCounter + 3;
	_steps.insert_at(2, release);
}

// Has the engine taken notice of the cursor sitting at these coordinates?
// True once the hotspot it reports as hovered is the one covering the point
// (or once there is nothing there to hover).
bool GobMcpBridge::hoverRegistered(int gameX, int gameY) const {
	if (!engineReady())
		return true;

	Common::Array<Hotspots::McpDesc> spots;
	collectHotspots(spots);

	uint16 wanted = 0;
	for (uint i = 0; i < spots.size(); i++) {
		const Hotspots::McpDesc &d = spots[i];
		if (gameX < d.left || gameX > d.right || gameY < d.top || gameY > d.bottom)
			continue;
		wanted = d.id;
		break;
	}
	if (wanted == 0)
		return true; // nothing to enter here — don't hold the click back

	return _vm->_game->_hotspots->mcpCurrentId() == wanted;
}

// Queue a complete click: hover the target so the game's enter() handler runs,
// press, then release a few frames later so the scripts' polling loops see the
// button held down.
void GobMcpBridge::queueClick(int gameX, int gameY, bool right) {
	// A real action owns the cursor from here; drop any hover sweep.
	cancelNameSweep();

	Step hover;
	hover.kind = kStepHover;
	hover.x = gameX;
	hover.y = gameY;
	hover.right = right;
	hover.slotId = 0;
	hover.notBeforeFrame = _frameCounter;
	_steps.push_back(hover);

	Step press;
	press.kind = kStepPress;
	press.x = gameX;
	press.y = gameY;
	press.right = right;
	press.slotId = 0;
	press.notBeforeFrame = _frameCounter;
	_steps.push_back(press);

	Step release = press;
	release.kind = kStepRelease;
	release.notBeforeFrame = _frameCounter + 3;
	_steps.push_back(release);
}

void GobMcpBridge::injectKey(const Common::KeyState &ks) {
	Common::Event event;
	event.type = Common::EVENT_KEYDOWN;
	event.kbd = ks;
	g_system->getEventManager()->pushEvent(event);
	event.type = Common::EVENT_KEYUP;
	g_system->getEventManager()->pushEvent(event);
}

void GobMcpBridge::injectMouseMove(int x, int y) {
	pushMouseMove(x, y);
}

void GobMcpBridge::injectMouseClick(int x, int y, const Common::String &button, bool isDouble) {
	(void)isDouble;
	queueClickWhenReady(x, y, button == "right");
}

// ---------------------------------------------------------------------------
// State helpers
// ---------------------------------------------------------------------------

bool GobMcpBridge::waitingForInput() const {
	return _frameCounter - _lastInputPollFrame <= 3;
}

// Woodruff's character controller keeps two script globals worth reading. The
// script polls for input the whole time (a player may redirect a walk mid-way),
// so neither of them can be replaced by "is the game reading the mouse".
//
//   VAR(kWoodruffActionTarget) — the id of the hotspot the last click resolved
//     to (a floor click included; the walk area is a hotspot too). Non-zero
//     from the click until the walk and its action have played out: this is
//     "still carrying out my click".
//   VAR(kWoodruffActionKind)   — set while the character is playing a scripted
//     animation, and for a few seconds after an action while it winds down.
//     Clicks that arrive in that window are swallowed by the script, so the
//     bridge must not inject one (or move the cursor) until it clears.
//
// VAR(932), their neighbour, is the per-frame animation phase — it toggles
// constantly and means nothing here.
enum {
	kWoodruffActionKind   = 933,
	kWoodruffActionTarget = 934
};

// Can the script variables be read at all? (They are sized by the game.)
bool GobMcpBridge::actionVarsReadable() const {
	return engineReady() && _vm->getGameType() == kGameTypeWoodruff &&
	       _vm->_inter->_variables->getSize() / 4 > kWoodruffActionTarget;
}

bool GobMcpBridge::gameBusy() const {
	return actionVarsReadable() && VAR(kWoodruffActionTarget) != 0;
}

bool GobMcpBridge::readyForClick() const {
	if (!waitingForInput() || gameBusy())
		return false;
	// The bridge's own inventory overlay owns the screen while it is up.
	if (_invState != kInvIdle)
		return false;
	return !actionVarsReadable() || VAR(kWoodruffActionKind) == 0;
}

void GobMcpBridge::collectHotspots(Common::Array<Hotspots::McpDesc> &out) const {
	out.clear();
	if (!engineReady())
		return;
	_vm->_game->_hotspots->mcpList(out);
}

void GobMcpBridge::buildObjectList(Common::Array<ObjectEntry> &out) const {
	out.clear();
	Common::Array<Hotspots::McpDesc> spots;
	collectHotspots(spots);
	for (uint i = 0; i < spots.size(); i++) {
		if (!isPointableHotspot(spots[i]))
			continue;
		// Scripts often stack several hotspots on the same rectangle (one per
		// interaction); a click can only ever reach the first, so only list it.
		bool duplicate = false;
		for (uint j = 0; j < out.size(); j++) {
			if (out[j].desc.left == spots[i].left && out[j].desc.top == spots[i].top &&
			    out[j].desc.right == spots[i].right && out[j].desc.bottom == spots[i].bottom) {
				duplicate = true;
				break;
			}
		}
		if (duplicate)
			continue;
		ObjectEntry entry;
		entry.desc = spots[i];
		cachedName(spots[i], entry.label);
		entry.name = mcpGobObjectName(entry.label);
		if (entry.name.empty())
			entry.name = mcpGobHotspotFallbackName(spots[i].id & 0x0FFF);
		out.push_back(entry);
	}

	// De-duplicate names so they resolve unambiguously (boot, boot_2, …).
	for (uint i = 0; i < out.size(); i++) {
		uint n = 1;
		for (uint j = 0; j < i; j++)
			if (out[j].name == out[i].name ||
			    out[j].name.hasPrefix(out[i].name + "_"))
				n++;
		if (n > 1)
			out[i].name = Common::String::format("%s_%u", out[i].name.c_str(), n);
	}
}

bool GobMcpBridge::resolveTarget(const Common::String &name, Hotspots::McpDesc &out,
                                 Common::String &errorOut) const {
	Common::String normalized = normalizeActionName(name);

	Common::Array<ObjectEntry> objects;
	buildObjectList(objects);

	for (uint i = 0; i < objects.size(); i++) {
		if (objects[i].name == normalized) {
			out = objects[i].desc;
			return true;
		}
	}

	// Accept "hotspot_<id>" or a bare numeric id for any pointable hotspot.
	int wantedId = mcpGobParseHotspotTarget(normalized);
	if (wantedId >= 0) {
		for (uint i = 0; i < objects.size(); i++) {
			if ((int)(objects[i].desc.id & 0x0FFF) == wantedId) {
				out = objects[i].desc;
				return true;
			}
		}
	}

	Common::String available;
	for (uint i = 0; i < objects.size() && i < 20; i++) {
		if (!available.empty())
			available += ", ";
		available += objects[i].name;
	}
	errorOut = "unknown target '" + name + "'; objects on this screen: " + available;
	return false;
}

int GobMcpBridge::currentRoomForMessages() const {
	return 0;
}

// ---------------------------------------------------------------------------
// Tool: state
// ---------------------------------------------------------------------------

Common::String GobMcpBridge::stateToolDescription() const {
	return "Returns the current game state: screen (id + TOT name), the screen's "
	       "named hotspots, the inventory, the two verbs, and the lines the game "
	       "has drawn since the last read (cleared after reading). This is a "
	       "one-click game: every objects[] entry — scenery, characters and exits "
	       "alike — is acted on with act(verb='interact', target1=<name>), which "
	       "walks Woodruff over and runs the hotspot's own script. Exits carry "
	       "pathway=true. Names are harvested from the status-bar text the game "
	       "shows a player, so they are only final once naming_pending is false. "
	       "Nothing is accepted while can_act is false. This game asks no dialog "
	       "questions: conversations play out as messages, so answer is never used.";
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

// Drop a property the shared (SCUMM-shaped) schema declares but this snapshot
// never fills, so what the schema promises is what an agent actually gets.
static void dropProp(Common::JSONObject &props, const char *key) {
	Common::JSONObject::iterator it = props.find(key);
	if (it == props.end())
		return;
	delete it->_value;
	props.erase(key);
}

void GobMcpBridge::augmentStateSchema(Common::JSONObject &outputProps) {
	outputProps.setVal("can_act", mcpProp("boolean",
	    "False while the game is not accepting input (video, scripted sequence). "
	    "act/walk are rejected until it turns true again."));
	outputProps.setVal("naming_pending", mcpProp("boolean",
	    "True while object names are still being resolved (the bridge hovers "
	    "each new hotspot to learn the name the game shows a player). Call "
	    "state again shortly for the final names."));

	// Woodruff's snapshot differs from the SCUMM one the shared schema
	// describes: restate the collections it fills differently, and drop what it
	// never fills. Everything an agent may echo back into act() has to be
	// declared, or a client validating the result against this schema rejects it.
	{
		Common::JSONObject props;
		props.setVal("id",      mcpProp("integer", "Hotspot id; usable as an act() target."));
		props.setVal("name",    mcpProp("string",  "Object name, as act() expects it."));
		props.setVal("label",   mcpProp("string",
		    "The raw status-bar text the game shows for this hotspot, in the "
		    "game's own language. Absent until the name sweep has read it."));
		props.setVal("x",       mcpProp("integer", "X coordinate of the hotspot centre."));
		props.setVal("y",       mcpProp("integer", "Y coordinate of the hotspot centre."));
		props.setVal("pathway", mcpProp("boolean",
		    "Present and true when the hotspot leads to another screen."));
		outputProps.setVal("objects", objectArraySchema(props));
	}
	{
		Common::JSONObject props;
		props.setVal("name",  mcpProp("string",  "Item name, as act(verb='use') expects it."));
		props.setVal("label", mcpProp("string",  "The raw name the inventory overlay draws for it."));
		props.setVal("id",    mcpProp("integer", "Inventory slot id."));
		outputProps.setVal("inventory", objectArraySchema(props));
	}
	{
		Common::JSONObject props;
		props.setVal("text", mcpProp("string", "Line of game text."));
		props.setVal("type", mcpProp("string", "Always 'text' — this engine does not attribute lines."));
		outputProps.setVal("messages", objectArraySchema(props));
	}
	// No ego position is exposed (Woodruff is listed among the objects), and the
	// game never raises a dialog question.
	dropProp(outputProps, "position");
	dropProp(outputProps, "question");
}

void GobMcpBridge::augmentChangesSchema(Common::JSONObject &props) {
	{
		Common::JSONObject msgProps;
		msgProps.setVal("text", mcpProp("string", "Line of game text drawn during the action."));
		msgProps.setVal("type", mcpProp("string", "Always 'text'."));
		props.setVal("messages", objectArraySchema(msgProps));
	}
	props.setVal("room_changed", mcpProp("integer",
	    "The new screen id, present only when the action moved to another screen."));
	// The bridge reports the screen change and the lines that were said; it has
	// no object states, no inventory diff and no dialog questions to report.
	dropProp(props, "inventory_added");
	dropProp(props, "inventory_removed");
	dropProp(props, "position");
	dropProp(props, "objects_changed");
	dropProp(props, "question");
}

Common::JSONValue *GobMcpBridge::toolState(const Common::JSONValue &, Common::String &) {
	Common::JSONObject out;

	// While the bridge has the game's inventory overlay open, the live screen is
	// MENU.tot and its hotspots are the item slots. That is bridge bookkeeping,
	// not a screen the agent moved to, so report the world it came from (and
	// can_act=false, since a click would land in the overlay).
	const bool overlayBusy = _invState != kInvIdle;

	Common::JSONObject roomObj;
	const Common::String &tot = overlayBusy ? _worldTot : _vm->_game->_curTotFile;
	roomObj.setVal("id", mcpJsonInt(mcpGobScreenId(tot)));
	roomObj.setVal("name", mcpJsonString(mcpGobScreenName(tot)));
	out.setVal("room", new Common::JSONValue(roomObj));

	out.setVal("can_act", mcpJsonBool(waitingForInput() && !overlayBusy));

	Common::JSONArray objects;
	Common::Array<ObjectEntry> entries;
	if (overlayBusy)
		entries = _worldObjects;
	else
		buildObjectList(entries);
	for (uint i = 0; i < entries.size(); i++) {
		const ObjectEntry &e = entries[i];
		Common::JSONObject o;
		o.setVal("id",   mcpJsonInt(e.desc.id & 0x0FFF));
		o.setVal("name", mcpJsonString(e.name));
		if (!e.label.empty())
			o.setVal("label", mcpJsonString(e.label));
		o.setVal("x",    mcpJsonInt((e.desc.left + e.desc.right) / 2));
		o.setVal("y",    mcpJsonInt((e.desc.top + e.desc.bottom) / 2));
		if (mcpGobIsExitLabel(e.label))
			o.setVal("pathway", mcpJsonBool(true));
		objects.push_back(new Common::JSONValue(o));
	}
	out.setVal("objects", new Common::JSONValue(objects));

	bool namingPending = _sweepIndex >= 0;
	if (!namingPending) {
		Common::String dummy;
		for (uint i = 0; i < entries.size(); i++) {
			if (!cachedName(entries[i].desc, dummy)) {
				namingPending = true;
				break;
			}
		}
	}
	if (_vm->getGameType() == kGameTypeWoodruff && _inventoryDirty)
		namingPending = true;
	out.setVal("naming_pending", mcpJsonBool(namingPending));

	Common::JSONArray inventory;
	for (uint i = 0; i < _inventory.size(); i++) {
		Common::JSONObject item;
		item.setVal("name", mcpJsonString(_inventory[i].name));
		if (!_inventory[i].label.empty())
			item.setVal("label", mcpJsonString(_inventory[i].label));
		item.setVal("id", mcpJsonInt(_inventory[i].slotId));
		inventory.push_back(new Common::JSONValue(item));
	}
	out.setVal("inventory", new Common::JSONValue(inventory));

	Common::JSONArray verbs;
	verbs.push_back(mcpJsonString("interact"));
	verbs.push_back(mcpJsonString("use"));
	out.setVal("verbs", new Common::JSONValue(verbs));

	// Latest captured messages, cleared after reading (SCUMM behaviour).
	Common::JSONArray messages;
	for (uint i = 0; i < _messages.size(); i++) {
		Common::JSONObject m;
		Common::String text = MCP::mcpCleanGameText(safeUtf8(_messages[i].text));
		if (text.empty())
			continue;
		m.setVal("text", mcpJsonString(text));
		m.setVal("type", mcpJsonString(_messages[i].type));
		messages.push_back(new Common::JSONValue(m));
	}
	_messages.clear();
	out.setVal("messages", new Common::JSONValue(messages));

	return new Common::JSONValue(out);
}

// ---------------------------------------------------------------------------
// Tools: act / walk / answer / skip
// ---------------------------------------------------------------------------

bool GobMcpBridge::toolAct(const Common::JSONValue &args, Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "act: another action is already in progress";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("verb") ||
	    !args.asObject()["verb"]->isString()) {
		errorOut = "act: 'verb' string is required";
		return false;
	}
	const Common::JSONObject &a = args.asObject();
	Common::String verb = normalizeActionName(a["verb"]->asString());

	// Woodruff is a one-click game: every verb (including examining — the
	// game has no separate look) is a left click on the target.
	if (verb == "look_at") {
		errorOut = "act: this game has no separate look verb — 'interact' walks "
		           "to the target and performs its default action";
		return false;
	}
	if (verb != "interact" && verb != "use" && verb != "walk_to" &&
	    verb != "open" && verb != "close" && verb != "pick_up" &&
	    verb != "talk_to" && verb != "push" && verb != "pull") {
		errorOut = "act: unknown verb '" + verb + "' (use 'interact')";
		return false;
	}
	bool right = false;

	Common::String name1;
	if (a.contains("target1")) {
		if (a["target1"]->isString())
			name1 = a["target1"]->asString();
		else if (a["target1"]->isIntegerNumber())
			name1 = Common::String::format("%d", (int)a["target1"]->asIntegerNumber());
	}
	if (name1.empty()) {
		errorOut = "act: 'target1' is required";
		return false;
	}
	if (!waitingForInput()) {
		errorOut = "act: game is not accepting input right now";
		return false;
	}

	Common::String name2;
	if (a.contains("target2")) {
		if (a["target2"]->isString())
			name2 = a["target2"]->asString();
		else if (a["target2"]->isIntegerNumber())
			name2 = Common::String::format("%d", (int)a["target2"]->asIntegerNumber());
	}

	// A fresh action: no use-command bar to filter unless the use path sets it.
	_useCmdLabel.clear();

	// "use <inventory item> on <target>": open the overlay, pick the item up
	// onto the cursor, then click the target with it. Only 'use' consults the
	// inventory, so a world object sharing an item's name stays reachable.
	int invIndex = -1;
	Common::String normalized1 = normalizeActionName(name1);
	if (verb == "use") {
		for (uint i = 0; i < _inventory.size(); i++)
			if (_inventory[i].name == normalized1)
				invIndex = (int)i;
	}
	if (invIndex >= 0) {
		if (name2.empty()) {
			errorOut = "act: '" + normalized1 + "' is an inventory item — give a "
			           "'target2' to use it on";
			return false;
		}
		Hotspots::McpDesc target;
		if (!resolveTarget(name2, target, errorOut)) {
			errorOut = "act: " + errorOut;
			return false;
		}
		int tx = (target.left + target.right) / 2;
		int ty = (target.top + target.bottom) / 2;
		uint16 slotId = _inventory[invIndex].slotId;
		// Suppress the game's "Use <item> on ..." command bar from messages.
		_useCmdLabel = _inventory[invIndex].label;

		queueClickWhenReady(320, 240, true); // open the overlay
		Step wait;
		wait.kind = kStepWaitOverlay;
		wait.x = wait.y = 0;
		wait.right = false;
		wait.slotId = 0;
		wait.notBeforeFrame = _frameCounter + 50;
		_steps.push_back(wait);
		Step item;
		item.kind = kStepClickItem;
		item.x = item.y = 0;
		item.right = false;
		item.slotId = slotId;
		item.notBeforeFrame = _frameCounter + 75;
		_steps.push_back(item);
		Step worldWait;
		worldWait.kind = kStepWaitWorld;
		worldWait.x = worldWait.y = 0;
		worldWait.right = false;
		worldWait.slotId = 0;
		worldWait.notBeforeFrame = _frameCounter + 125;
		_steps.push_back(worldWait);
		Step press;
		press.kind = kStepPress;
		press.x = tx;
		press.y = ty;
		press.right = false;
		press.slotId = 0;
		press.notBeforeFrame = 0;
		_steps.push_back(press);
		Step release = press;
		release.kind = kStepRelease;
		release.notBeforeFrame = 0;
		_steps.push_back(release);

		_inventoryDirty = true;
		beginStream();
		_sseSkipFast = false;
		return true;
	}

	if (verb == "use" && !name2.empty()) {
		errorOut = "act: 'use X on Y' needs X to be an inventory item; "
		           "inventory: " + inventoryNameList();
		return false;
	}

	Hotspots::McpDesc target;
	if (!resolveTarget(name1, target, errorOut)) {
		errorOut = "act: " + errorOut;
		return false;
	}

	// One click on the target is the whole action: the game walks Woodruff over
	// and runs the hotspot's script itself, for objects, characters and exits
	// alike. (Driving the walk separately and clicking again afterwards only
	// re-issues the action and, when the script has already started talking,
	// throws a stray click into the conversation.)
	{
		int cx = (target.left + target.right) / 2;
		int cy = (target.top + target.bottom) / 2;
		queueClickWhenReady(cx, cy, right);
	}
	_inventoryDirty = true;
	beginStream();
	_sseSkipFast = false;
	return true;
}

Common::String GobMcpBridge::inventoryNameList() const {
	Common::String names;
	for (uint i = 0; i < _inventory.size(); i++) {
		if (!names.empty())
			names += ", ";
		names += _inventory[i].name;
	}
	return names.empty() ? Common::String("(empty)") : names;
}

bool GobMcpBridge::toolWalk(const Common::JSONValue &args, Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "walk: another action is already in progress";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("x") || !args.asObject().contains("y") ||
	    !args.asObject()["x"]->isIntegerNumber() || !args.asObject()["y"]->isIntegerNumber()) {
		errorOut = "walk: integer 'x' and 'y' are required";
		return false;
	}
	if (!waitingForInput()) {
		errorOut = "walk: game is not accepting input right now";
		return false;
	}
	int x = (int)args.asObject()["x"]->asIntegerNumber();
	int y = (int)args.asObject()["y"]->asIntegerNumber();

	_useCmdLabel.clear();
	queueClickWhenReady(x, y, false);
	_inventoryDirty = true;
	beginStream();
	_sseSkipFast = false;
	return true;
}

bool GobMcpBridge::toolAnswer(const Common::JSONValue &args, Common::String &errorOut) {
	(void)args;
	errorOut = "answer: no dialog question is pending";
	return false;
}

bool GobMcpBridge::toolSkip(const Common::JSONValue &, Common::String &errorOut) {
	if (!_skipToolEnabled) {
		errorOut = "skip: tool is disabled (set mcp_skip_tool=true)";
		return false;
	}
	// When the game is already waiting for input there is nothing to skip and
	// the Escape key would open the game's own menu instead.
	if (waitingForInput() && !gameBusy()) {
		errorOut = "skip: nothing to skip — the game is waiting for input";
		return false;
	}
	// Videos and scripted waits poll the key buffer and abort on their break
	// key, which is Escape everywhere in this engine.
	Common::KeyState esc(Common::KEYCODE_ESCAPE, 27);
	injectKey(esc);
	if (!isStreaming()) {
		beginStream();
		_sseSkipFast = true;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Tool dispatch / gating
// ---------------------------------------------------------------------------

Common::JSONValue *GobMcpBridge::callTool(const Common::String &name,
                                          const Common::JSONValue &args,
                                          Common::String &errorOut) {
	// The bridge is constructed before GobEngine::run() builds the subsystems,
	// so a call arriving that early has nothing to read.
	if (!engineReady()) {
		errorOut = name + ": the engine is still starting up";
		return nullptr;
	}
	return MCP::McpBridge::callTool(name, args, errorOut);
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

void GobMcpBridge::snapshotPreAction() {
	_ssePreTot = _vm->_game->_curTotFile;
	_ssePreRoom = mcpGobScreenId(_ssePreTot);
	_sseActionStarted = false;
}

void GobMcpBridge::pumpStreamTrack() {
	// Any sign of life bumps the event frame: a blocking video, a script that
	// stopped polling for input (i.e. is doing something), a commanded action
	// still playing out, or a screen change.
	if (!waitingForInput() || gameBusy() || _vm->_game->_curTotFile != _ssePreTot) {
		_sseActionStarted = true;
		_sseLastEventFrame = _frameCounter;
	}
}

bool GobMcpBridge::streamRoomChanged() const {
	// The menu/inventory overlay is its own TOT; passing through it (a `use`
	// sequence, the player-driven menu) is not a screen change.
	if (inventoryOverlayOpen() || _ssePreTot.equalsIgnoreCase("MENU.tot"))
		return false;
	if (!_steps.empty())
		return false;
	if (_vm->_game->_curTotFile != _ssePreTot)
		return waitingForInput() && !gameBusy();
	return false;
}

bool GobMcpBridge::hasPendingQuestion() const {
	return false;
}

bool GobMcpBridge::isActionDone() const {
	if (!_steps.empty())
		return false;
	// A skip only needs to break the current video: report done after a short
	// fixed window even when the next video is already running.
	if (_sseSkipFast && _frameCounter - _sseStartFrame >= 40)
		return true;
	if (!waitingForInput() || gameBusy())
		return false;
	// Give the script a short window to react to the final synthetic click
	// before concluding the action produced nothing. Measured from the last
	// injected input rather than the stream start, because a pre-action
	// approach walk (walk-to-object then click) already tripped
	// _sseActionStarted and moved _sseStartFrame well into the past — without
	// this a short reply drawn a few frames after the click (e.g. Woodruff's
	// "I can't read") would land after the stream had already closed.
	if (_frameCounter - _lastStepFrame < 20)
		return false;
	return true;
}

bool GobMcpBridge::shouldCloseStream() const {
	// Skips ignore the "new event extends the settle window" rule — the whole
	// point is to return while the (chained) videos are still going.
	if (_sseSkipFast)
		return _frameCounter - _sseStartFrame >= 40 ||
		       (waitingForInput() && _frameCounter - _sseDoneAtFrame >= settleFrames());
	return MCP::McpBridge::shouldCloseStream();
}

Common::JSONObject GobMcpBridge::buildStateChanges() const {
	Common::JSONObject changes;

	if (_vm->_game->_curTotFile != _ssePreTot)
		changes.setVal("room_changed", mcpJsonInt(mcpGobScreenId(_vm->_game->_curTotFile)));

	if (!_sseMessages.empty()) {
		Common::JSONArray messages;
		for (uint i = 0; i < _sseMessages.size(); i++) {
			Common::JSONObject m;
			Common::String text = MCP::mcpCleanGameText(safeUtf8(_sseMessages[i].text));
			if (text.empty())
				continue;
			m.setVal("text", mcpJsonString(text));
			m.setVal("type", mcpJsonString(_sseMessages[i].type));
			messages.push_back(new Common::JSONValue(m));
		}
		if (!messages.empty())
			changes.setVal("messages", new Common::JSONValue(messages));
	}

	return changes;
}

// ---------------------------------------------------------------------------
// Tool: debug
// ---------------------------------------------------------------------------

Common::String GobMcpBridge::debugToolDescription() const {
	return "Return raw engine state for diagnostics. Sections are selected by "
	       "flag: 'vars' (a slice of the script variables, with 'from'/'to'), "
	       "'hotspots' (full detail of the current hotspots) and 'system' (the "
	       "engine's own read-out). Defaults to 'system'.";
}

Common::JSONValue *GobMcpBridge::buildDebugSchema() const {
	Common::JSONObject props;
	props.setVal("vars",     mcpProp("boolean", "Include script variables."));
	props.setVal("from",     mcpProp("integer", "First variable index (default 0)."));
	props.setVal("to",       mcpProp("integer", "Last variable index, inclusive (default 63)."));
	props.setVal("hotspots", mcpProp("boolean", "Include full hotspot descriptions."));
	props.setVal("text_items", mcpProp("boolean",
	    "Include the printable strings of the current TOT's text items (with 'from'/'to')."));
	props.setVal("system",   mcpProp("boolean", "Include the engine state summary (default true)."));
	return mcpObjectSchema(props);
}

Common::JSONValue *GobMcpBridge::toolDebug(const Common::JSONValue &args, Common::String &) {
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
		sys.setVal("tot", mcpJsonString(_vm->_game->_curTotFile));
		sys.setVal("mouse_x", mcpJsonInt(_vm->_global->_inter_mouseX));
		sys.setVal("mouse_y", mcpJsonInt(_vm->_global->_inter_mouseY));
		sys.setVal("mouse_buttons", mcpJsonInt((int)_vm->_game->_mouseButtons));
		sys.setVal("waiting_for_input", mcpJsonBool(waitingForInput()));
		sys.setVal("frame", mcpJsonInt((int)_frameCounter));
		sys.setVal("last_input_poll_frame", mcpJsonInt((int)_lastInputPollFrame));
		sys.setVal("last_any_poll_frame", mcpJsonInt((int)_lastAnyPollFrame));
		sys.setVal("video_live", mcpJsonBool(_vm->_vidPlayer->isPlayingLive()));
		sys.setVal("pending_steps", mcpJsonInt((int)_steps.size()));
		sys.setVal("width", mcpJsonInt(_vm->_width));
		sys.setVal("height", mcpJsonInt(_vm->_height));
		sys.setVal("screen_delta_x", mcpJsonInt(_vm->_video->_screenDeltaX));
		sys.setVal("screen_delta_y", mcpJsonInt(_vm->_video->_screenDeltaY));
		sys.setVal("scroll_offset_x", mcpJsonInt(_vm->_video->_scrollOffsetX));
		sys.setVal("scroll_offset_y", mcpJsonInt(_vm->_video->_scrollOffsetY));
		out.setVal("system", new Common::JSONValue(sys));
	}

	if (flag("hotspots", false)) {
		Common::JSONArray arr;
		Common::Array<Hotspots::McpDesc> spots;
		collectHotspots(spots);
		for (uint i = 0; i < spots.size(); i++) {
			const Hotspots::McpDesc &d = spots[i];
			Common::JSONObject o;
			o.setVal("id",     mcpJsonInt(d.id & 0x0FFF));
			o.setVal("raw_id", mcpJsonInt(d.id));
			o.setVal("index",  mcpJsonInt(d.index));
			o.setVal("left",   mcpJsonInt(d.left));
			o.setVal("top",    mcpJsonInt(d.top));
			o.setVal("right",  mcpJsonInt(d.right));
			o.setVal("bottom", mcpJsonInt(d.bottom));
			o.setVal("flags",  mcpJsonInt(d.flags));
			o.setVal("key",    mcpJsonInt(d.key));
			o.setVal("type",   mcpJsonInt(d.type));
			o.setVal("cursor", mcpJsonInt(d.cursor));
			o.setVal("window", mcpJsonInt(d.window));
			o.setVal("has_enter", mcpJsonBool(d.hasEnter));
			o.setVal("has_leave", mcpJsonBool(d.hasLeave));
			o.setVal("has_pos",   mcpJsonBool(d.hasPos));
			arr.push_back(new Common::JSONValue(o));
		}
		out.setVal("hotspots", new Common::JSONValue(arr));
	}

	if (flag("text_items", false) && _vm->_game->_resources) {
		int from = MAX(0, number("from", 0));
		int to = number("to", 255);
		Common::JSONArray arr;
		for (int i = from; i <= to; i++) {
			TextItem *item = _vm->_game->_resources->getTextItem(i);
			if (!item)
				continue;
			// Extract the printable runs; enough to spot names and lines.
			const byte *data = item->getData();
			int size = item->getSize();
			Common::String text;
			Common::String run;
			for (int j = 0; j < size; j++) {
				byte c = data[j];
				if (c >= 32 && c < 127) {
					run += (char)c;
				} else {
					if (run.size() >= 2) {
						if (!text.empty())
							text += " | ";
						text += run;
					}
					run.clear();
				}
			}
			if (run.size() >= 2) {
				if (!text.empty())
					text += " | ";
				text += run;
			}
			delete item;
			if (text.empty())
				continue;
			Common::JSONObject o;
			o.setVal("id", mcpJsonInt(i));
			o.setVal("text", mcpJsonString(safeUtf8(text)));
			arr.push_back(new Common::JSONValue(o));
		}
		out.setVal("text_items", new Common::JSONValue(arr));
	}

	if (flag("vars", false)) {
		int from = MAX(0, number("from", 0));
		int to = number("to", 63);
		if (_vm->_inter->_variables)
			to = MIN<int>(to, (int)(_vm->_inter->_variables->getSize() / 4) - 1);
		Common::JSONArray vars;
		for (int i = from; i <= to; i++) {
			Common::JSONObject v;
			v.setVal("index", mcpJsonInt(i));
			v.setVal("value", mcpJsonInt((int32)_vm->_inter->_variables->readVar32(i)));
			vars.push_back(new Common::JSONValue(v));
		}
		out.setVal("vars", new Common::JSONValue(vars));
	}

	return new Common::JSONValue(out);
}

} // End of namespace Gob
