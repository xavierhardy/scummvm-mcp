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

#ifndef GOB_MCP_H
#define GOB_MCP_H

#include "engines/mcp_bridge.h"

#include "common/array.h"
#include "common/formats/json.h"
#include "common/hashmap.h"
#include "common/hash-str.h"
#include "common/str.h"

#include "gob/hotspots.h"

namespace Gob {

class GobEngine;

// MCP bridge for the Gob engine (currently exercised with The Bizarre
// Adventures of Woodruff and the Schnibble).
//
// Gob games are fully script-driven: every screen is a block of bytecode that
// declares rectangular hotspots (Hotspots) and then sits in a wait loop
// (Hotspots::check) polling the mouse and keyboard. There is no verb bar and no
// engine-side object model — everything an agent can do, a player does by
// pointing and clicking, and one click on a hotspot is the whole interaction:
// the game walks the character over and runs the hotspot's script itself. The
// bridge therefore replays real input: `act` resolves its target to a hotspot,
// moves the cursor onto it, waits for the engine to register the hover, and
// only then pushes a button press, held down for a few frames so the script's
// own polling sees a complete click (see kStepHover — the hover is not
// cosmetic, the scripts act on it). Because the engine re-reads the event
// queue from every one of its blocking loops (Util::processInput), this works
// identically in the game proper, in menus and during dialogue choices.
//
// Woodruff shows an object's name next to the cursor while hovering: the
// hotspot's "position" script draws it every frame. The bridge captures every
// string the engine draws (Draw_v2::spriteOperation DRAW_PRINTTEXT hook), which
// yields both those hover names and all dialogue/subtitle lines.
//
// The engine has no single main loop to pump from; instead the bridge is
// pumped from Util::processInput() — the one choke point every blocking loop
// goes through — and converts wall-clock time into frames itself (a "frame"
// is at most one per kFrameMs milliseconds, so the base class's frame budgets
// keep their meaning no matter how hot the calling loop is).
class GobMcpBridge : public MCP::McpBridge {
public:
	// Factory mirroring the other bridges' two-phase construction.
	static GobMcpBridge *create(GobEngine *vm);

	explicit GobMcpBridge(GobEngine *vm);
	~GobMcpBridge() override;

	// Called from Util::processInput(). Advances the frame counter at most
	// once per kFrameMs; other calls only service the transport.
	void pumpFromInput();

	// Called from Draw_v2::spriteOperation for every string drawn to a
	// surface. Captures dialogue lines, subtitles and hover names.
	void onTextDrawn(const char *text, int16 x, int16 y, int16 surface);

	// Called from the Hotspots::check() wait loop: the script is polling for
	// player input (handleMouse != 0 means the mouse is live).
	void onInputPoll(uint8 handleMouse);

protected:
	// --- Tools --------------------------------------------------------------
	Common::JSONValue *toolState(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolAct(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolAnswer(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolWalk(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolSkip(const Common::JSONValue &args, Common::String &errorOut) override;
	Common::JSONValue *toolDebug(const Common::JSONValue &args, Common::String &errorOut) override;

	Common::String stateToolDescription() const override;
	Common::String debugToolDescription() const override;
	Common::JSONValue *buildDebugSchema() const override;
	void augmentStateSchema(Common::JSONObject &outputProps) override;
	void augmentChangesSchema(Common::JSONObject &props) override;

	// Reject every tool until GobEngine::run() has built the subsystems (the
	// bridge is created in the GobEngine constructor so the port binds first).
	Common::JSONValue *callTool(const Common::String &name,
	                            const Common::JSONValue &args,
	                            Common::String &errorOut) override;

	// --- Input injection ----------------------------------------------------
	void injectKey(const Common::KeyState &ks) override;
	void injectMouseMove(int x, int y) override;
	void injectMouseClick(int x, int y, const Common::String &button, bool isDouble) override;

	// --- Text ---------------------------------------------------------------
	int currentRoomForMessages() const override;

	// --- Streaming ----------------------------------------------------------
	void snapshotPreAction() override;
	Common::JSONObject buildStateChanges() const override;
	bool isActionDone() const override;
	bool hasPendingQuestion() const override;
	bool streamRoomChanged() const override;
	bool shouldCloseStream() const override;
	void pumpGame() override;
	void pumpStreamTrack() override;

	// The frame counter advances at most once per kFrameMs (~25 fps), so the
	// budgets sit between Queen's 10 Hz and SCUMM's 60 Hz numbers.
	uint32 minStreamFrames() const override { return 4; }
	uint32 stuckFrames(bool hadActivity) const override { return hadActivity ? 45 : 10; }
	uint32 timeoutFrames() const override { return 250; }          // ~10 s without an event
	uint32 absoluteTimeoutFrames() const override { return 2500; } // ~100 s
	// A little over half a second of quiet before a settled action is reported:
	// the game routinely goes from "walk finished" to "and now the character
	// speaks" with a couple of idle frames in between.
	uint32 settleFrames() const override { return 14; }
	uint32 wallClockTimeoutMs() const override { return 180000; }
	// Anchor the deadline to the last sign of life: videos and scripted
	// sequences run long while still progressing.
	uint32 streamTimeoutAnchor() const override {
		return _sseLastEventFrame > 0 ? _sseLastEventFrame : _sseStartFrame;
	}

private:
	// Wall-clock length of one bridge frame.
	static const uint32 kFrameMs = 40;

	// A queued synthetic-input step, played out one frame at a time so the
	// scripts' own polling sees a complete press/release cycle.
	// kStepHover exists because the scripts key off the *hover*, not the click:
	// entering a hotspot runs its enter() handler, which is what tells the game
	// what the player is pointing at (it is also what sets VAR(17)). Moving the
	// cursor and pressing in the same breath means the click is resolved before
	// any of that has run, and the action then plays out against the previously
	// hovered hotspot — the game performs the action without walking there,
	// drawing the character a second time at the target, or waits forever for a
	// walk it never set up. A real mouse always hovers before it clicks.
	enum StepKind {
		kStepHover,       // move to (x, y), wait for the game to register it
		kStepPress,       // button down
		kStepRelease,     // button up
		kStepClickItem,   // click the inventory slot `slotId` (resolved live)
		kStepWaitOverlay, // wait (up to notBeforeFrame) for the menu overlay
		kStepWaitWorld,   // wait (up to notBeforeFrame) for the overlay to close
		kStepWaitReady    // wait (up to notBeforeFrame) until a click would land
	};

	// Frames to wait for the hover to register before pressing anyway.
	static const uint32 kHoverMaxFrames = 15;
	struct Step {
		StepKind kind;
		int x, y;        // game coordinates
		bool right;      // right instead of left button
		uint16 slotId;   // kStepClickItem: the inventory slot hotspot id
		uint32 notBeforeFrame;
		// kStepHover: set once the move has been pushed. The move cannot be
		// tied to notBeforeFrame — a step sitting behind a wait does not run on
		// the frame it was queued on — so the hover tracks its own start.
		bool hoverSent = false;
		uint32 hoverFrame = 0;
	};

	// Is a script sitting in an input wait loop right now?
	bool waitingForInput() const;

	// Is the game still carrying out a commanded action (Woodruff keeps
	// polling for input while the character walks)? See gameBusy() in the cpp.
	bool gameBusy() const;

	// Would a click land right now, or would the script swallow it? False
	// while the character is mid-animation — including the few seconds an
	// action takes to wind down after its dialogue is over. Everything the
	// bridge injects (clicks, cursor moves) waits for this.
	bool readyForClick() const;

	// Are the character-controller script variables in range?
	bool actionVarsReadable() const;

	// Are the engine subsystems constructed yet?
	bool engineReady() const;

	// A hotspot the player could actually point at: a sane on-screen rectangle
	// that is not one of the script's parked (off-screen) or screen-wide zones.
	bool isPointableHotspot(const Hotspots::McpDesc &d) const;

	// Name-cache key for a hotspot on the current screen.
	Common::String nameKeyFor(const Hotspots::McpDesc &d) const;

	// The known hover label of the hotspot under the virtual cursor ("" when
	// none). Status-bar redraws of it are UI, not dialogue.
	Common::String hoveredLabel() const;

	// The cached display name for a hotspot ("" when swept but nameless,
	// unset when not yet swept).
	bool cachedName(const Hotspots::McpDesc &d, Common::String &out) const;

	// Advance the hover-sweep machine: while the game is idle, hover each
	// unnamed pointable hotspot for a few frames and record the name the
	// game's own scripts draw in the status bar. See pumpGame().
	void pumpNameSweep();
	void cancelNameSweep();

	// Push a full click (hover, press, release) at game coordinates.
	void queueClick(int gameX, int gameY, bool right);

	// Has the engine registered the cursor sitting at these coordinates, i.e.
	// run the enter() handler of the hotspot there? See kStepHover.
	bool hoverRegistered(int gameX, int gameY) const;

	// Same, but preceded by a wait until the game would honour the click. Used
	// by everything an agent asks for.
	void queueClickWhenReady(int gameX, int gameY, bool right);

	// A click driving the inventory overlay; runs ahead of a tool click that is
	// still waiting for that overlay to close.
	void queueOverlayClick(int gameX, int gameY, bool right);

	// Execute one queued synthetic-input step per frame.
	void pumpSteps();

	// Comma-separated inventory names for error messages.
	Common::String inventoryNameList() const;

	// Is the inventory overlay (MENU.tot with the item icons) on screen?
	bool inventoryOverlayOpen() const;

	// Advance the inventory-refresh machine: when idle and the inventory is
	// stale, open the overlay with a right click, hover each item slot to
	// learn its name, and close it again. See pumpGame().
	void pumpInventoryRefresh();

	// Convert game coordinates to event/screen coordinates and push a mouse
	// move event.
	void pushMouseMove(int gameX, int gameY);
	void pushButton(bool down, bool right, int gameX, int gameY);

	// Describe the current live hotspots (enabled, clickable).
	void collectHotspots(Common::Array<Hotspots::McpDesc> &out) const;

	// A pointable hotspot with its learned display name attached.
	struct ObjectEntry {
		Hotspots::McpDesc desc;
		Common::String label;  // raw hover label ("A trash heap"), may be empty
		Common::String name;   // unique target identifier ("trash_heap")
	};

	// The current screen's pointable hotspots with names resolved from the
	// hover-name cache, de-duplicated (boot, boot_2, …).
	void buildObjectList(Common::Array<ObjectEntry> &out) const;

	// Resolve an `act` target (a learned name, "hotspot_<id>", or a bare id)
	// to a hotspot description.
	bool resolveTarget(const Common::String &name, Hotspots::McpDesc &out,
	                   Common::String &errorOut) const;

	GobEngine *_vm;

	// Re-entrancy guard: processInput() runs inside tool handlers too.
	bool _inPump;
	uint32 _lastFrameMs;

	// Synthetic input queue (see queueClick()).
	Common::Array<Step> _steps;
	// Frame the previous step executed at (paces press/release pairs whose
	// exact frames are not known when queued).
	uint32 _lastStepFrame;


	// Frame of the most recent Hotspots::check() poll with a live mouse.
	uint32 _lastInputPollFrame;
	uint32 _lastAnyPollFrame;

	// Text lines drawn this frame, coalesced by row before being queued.
	struct DrawnText {
		Common::String text;
		int16 x, y;
		uint32 frame;
		uint32 seq;  // draw order, finer than the frame counter
	};
	uint32 _nextDrawSeq;
	Common::Array<DrawnText> _drawnTexts;
	// Last text pushed, to drop the re-draws hover names produce every frame.
	Common::String _lastPushedText;
	uint32 _lastPushedFrame;
	// Recently surfaced dialogue lines (text + frame), to suppress the game's
	// whole-subtitle redraws that would otherwise double every spoken line.
	struct RecentLine {
		Common::String text;
		uint32 frame;
	};
	Common::Array<RecentLine> _recentLines;
	// Item label of the current "use X on Y" action, so the command bar the
	// game paints while the cursor drags the item ("Use BUTTON on ...") is
	// recognised as UI and kept out of the dialogue messages.
	Common::String _useCmdLabel;

	// Hover-sweep state. _sweepIndex is the index into _sweepSpots of the
	// hotspot currently hovered, or -1 while idle.
	Common::Array<Hotspots::McpDesc> _sweepSpots;
	int _sweepIndex;
	uint32 _sweepMoveFrame;
	// Draw-sequence stamp taken when the sweep cursor moved: only rows drawn
	// after it belong to the hovered hotspot.
	uint32 _sweepMoveSeq;
	// Frame the last sweep ended at (rows drawn right after it are stale).
	uint32 _sweepEndFrame;
	int _sweepReturnX, _sweepReturnY;
	// Text row(s) drawn while hovering the current sweep target.
	Common::String _sweepCaptured;
	// Hover names learned so far, keyed by nameKeyFor(). "" = swept, nameless.
	Common::HashMap<Common::String, Common::String> _nameCache;
	// Frame of the most recent drawn-text row, to keep sweeps out of windows
	// where dialogue is still being written to the screen.
	uint32 _lastDrawnTextFrame;

	// Inventory tracking (Woodruff: the overlay a right click opens).
	struct InventoryItem {
		uint16 slotId;
		Common::String label;
		Common::String name;
	};
	enum InvState {
		kInvIdle,       // nothing to do (or _inventoryDirty and waiting to start)
		kInvOpening,    // right click sent, waiting for the overlay
		kInvHovering,   // parked on an item slot, capturing its name
		kInvClosing     // close click sent, waiting for the world to return
	};
	Common::Array<InventoryItem> _inventory;
	bool _inventoryKnown;
	bool _inventoryDirty;
	InvState _invState;
	uint32 _invStateFrame;
	uint32 _invMoveSeq;
	Common::Array<Hotspots::McpDesc> _invSlots;
	uint _invSlotIndex;
	Common::Array<InventoryItem> _invPending;
	// The world screen the refresh left behind, served by `state` while the
	// overlay is up (the live screen is MENU.tot then).
	Common::String _worldTot;
	Common::Array<ObjectEntry> _worldObjects;
	// Latest item name the overlay drew in its status area; attributed to the
	// slot the refresh is dwelling on when it moves off it.
	Common::String _invCapturedLabel;

	// Pre-action snapshot.
	Common::String _ssePreTot;
	// Latched once the engine visibly reacted to the queued click, so the
	// stream does not close in the gap between the click and the script
	// picking it up.
	bool _sseActionStarted;

	// True for `skip` streams: close after a short fixed window instead of
	// waiting for the (possibly chained) videos to end, so an intro made of
	// several videos can be skipped with a quick skip call per video.
	bool _sseSkipFast;
};

} // End of namespace Gob

#endif
