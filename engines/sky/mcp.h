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

#ifndef SKY_MCP_H
#define SKY_MCP_H

#include "engines/mcp_bridge.h"

#include "common/array.h"
#include "common/formats/json.h"
#include "common/str.h"

namespace Sky {

class SkyEngine;
class Logic;
class Mouse;
class Text;
class SkyCompact;
class Control;
class Sound;
struct Compact;

// MCP bridge for Beneath a Steel Sky.
//
// BASS is a two-button game: a left click on a hotspot walks Foster there and
// runs its interaction script, a right click examines it, and there is no verb
// bar. The `act` tool therefore exposes a fixed verb set and maps everything
// but look_at onto the left-click path, exactly like the Broken Sword bridge.
//
// Every action is dispatched by replaying real input: the bridge warps the
// engine's virtual mouse (Mouse::mouseMoved) and presses a button
// (Mouse::buttonPressed); the next Mouse::mouseEngine() call then runs the
// hotspot's hover and click scripts exactly as it would for a player. Because
// the engine re-reads the virtual cursor every cycle this needs no duplicated
// dispatch logic, and it extends to the parts of the UI that are only
// reachable by pointing: the top icon bar (inventory) and the text chooser
// (dialog options) are both driven by queueing a short sequence of
// warp/click steps that pumpStreamGame() plays out one frame at a time.
//
// All coordinates on the wire are game coordinates — the space of
// Compact::xcood/ycood and the mouse hit boxes. The visible play area spans
// (TOP_LEFT_X, TOP_LEFT_Y) = (128, 136) to (448, 328).
class SkyMcpBridge : public MCP::McpBridge {
public:
	// Factory mirroring the other bridges' two-phase construction: build the
	// object, then init() it (tool registration dispatches through virtual
	// hooks, so it cannot run from a constructor).
	static SkyMcpBridge *create(SkyEngine *vm);

	explicit SkyMcpBridge(SkyEngine *vm);
	~SkyMcpBridge() override;

	// Wire up the subsystems once SkyEngine::init() has constructed them. The
	// bridge itself is created first thing in the SkyEngine constructor so the
	// server binds its port before initGraphics() can block; every tool call
	// arriving before attach() is rejected with "still starting up".
	void attach(Logic *logic, Mouse *mouse, Text *text, SkyCompact *skyCompact,
	            Control *control, Sound *sound);

	// Called from Logic::stdSpeak for every line said in-game, whether or not
	// subtitles are on. `compactId` is the speaking compact.
	void onSpeech(uint16 compactId, uint32 textNum);

	// Called from Logic::fnStartMenu with the script-variable index the
	// inventory list starts at, so the bridge can read the inventory without
	// the icon bar being open.
	void onStartMenu(uint32 firstVar);

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

	// Reject every mutating tool until attach() has run and while the game is
	// not accepting input, so a call arriving mid-cutscene cannot corrupt the
	// engine.
	Common::JSONValue *callTool(const Common::String &name,
	                            const Common::JSONValue &args,
	                            Common::String &errorOut) override;

	// --- Input injection ----------------------------------------------------
	void injectKey(const Common::KeyState &ks) override;
	void injectMouseMove(int x, int y) override;
	void injectMouseClick(int x, int y, const Common::String &button, bool isDouble) override;

	// --- Text ---------------------------------------------------------------
	Common::String messageActorName(int actorId) const override;
	int currentRoomForMessages() const override;

	// --- Streaming ----------------------------------------------------------
	void snapshotPreAction() override;
	Common::JSONObject buildStateChanges() const override;
	bool isActionDone() const override;
	bool hasPendingQuestion() const override;
	bool streamRoomChanged() const override;
	bool isStreamStuck() const override;
	void pumpStreamGame() override;
	void pumpStreamTrack() override;

	// BASS runs its main loop at ~12.5 Hz (gameSpeed 80 ms), the same rate as
	// Broken Sword, so the budgets match that bridge.
	uint32 minStreamFrames() const override { return 3; }
	uint32 stuckFrames(bool hadActivity) const override { return hadActivity ? 18 : 4; }
	uint32 timeoutFrames() const override { return 125; }         // ~10 s
	uint32 absoluteTimeoutFrames() const override { return 750; } // ~60 s
	uint32 settleFrames() const override { return 3; }
	// The frame counter stops while the intro or the control panel owns the
	// main loop (those only reach pumpTransportOnly() via SkyEngine::delay()),
	// so a frame-anchored timeout can never fire there. This is the backstop.
	uint32 wallClockTimeoutMs() const override { return 180000; }
	// Anchor the deadline to the last sign of life: a scripted sequence (the
	// walkway guard, a long exchange) easily runs past 10 s while still
	// visibly progressing. The absolute ceiling above still bounds it.
	uint32 streamTimeoutAnchor() const override {
		return _sseLastEventFrame > 0 ? _sseLastEventFrame : _sseStartFrame;
	}

private:
	// A queued synthetic-input step, played out by pumpStreamGame() one frame
	// at a time. See toolAct() for the sequences that get queued.
	enum StepKind {
		kStepClickAt,    // warp to (x, y) game coords, press `button`
		kStepOpenMenu,   // hold the cursor on the top line until the target item's icon is clickable
		kStepClickItem,  // click the (now on-screen) icon of item compact `target`
		kStepParkMouse   // warp back into the play area so the icon bar closes
	};
	struct Step {
		StepKind kind;
		uint16 x, y;      // game coordinates (kStepClickAt)
		uint16 target;    // item compact id (kStepOpenMenu / kStepClickItem)
		uint8 button;     // 2 = left, 1 = right (Sky::Mouse::buttonPressed)
		uint32 notBeforeFrame;
	};

	// A named, clickable thing on the current screen.
	struct ScreenObject {
		uint16 id;
		Common::String name;
		bool isFloor;
		bool isCharacter;
		int x1, y1, x2, y2;
	};

	// Is the game accepting player input right now? Mirrors
	// Mouse::mouseEngine()'s own gate on MOUSE_STATUS / MOUSE_STOP.
	bool canAct() const;

	// Enumerate the current screen's mouse list, exactly as
	// Mouse::pointerEngine() walks it. Names come from each compact's
	// cursorText string when it has one, from the compact's authored name
	// otherwise, and are de-duplicated by appending _2, _3, …
	void collectScreenObjects(Common::Array<ScreenObject> &out) const;

	// Current inventory as item compact ids, read from the script variables
	// fnStartMenu copies into the icon bar.
	void collectInventory(Common::Array<uint16> &items) const;

	// The chooser texts currently on screen (dialog options), in display
	// order. Empty when no question is pending.
	void collectChoices(Common::Array<uint16> &textCompacts) const;

	// The de-duplicated display name for a compact (cursorText, then authored
	// compact name, then "object_<id>").
	Common::String compactDisplayName(uint16 id) const;

	// Decode a game string. Empty when the text number is 0.
	Common::String textString(uint32 textNum) const;

	// Center of a compact's mouse hit box, in game coordinates.
	void mouseBoxCenter(const Compact *cpt, int &x, int &y) const;

	// Warp the virtual mouse to (x, y) game coordinates.
	void warpTo(int gameX, int gameY);

	// Queue helpers for toolAct()/toolWalk()/toolAnswer().
	void queueClickAt(int gameX, int gameY, uint8 button, uint32 delayFrames = 0);
	void queueItemClick(uint16 itemCompact, uint8 button);

	// Resolve a tool target name to a screen object or an inventory item.
	// Exactly one of screenId / itemId is non-zero on success.
	bool resolveTarget(const Common::String &name, uint16 &screenId, uint16 &itemId,
	                   Common::String &errorOut) const;

	SkyEngine *_vm;
	Logic *_logic;
	Mouse *_mouse;
	Text *_text;
	SkyCompact *_skyCompact;
	Control *_control;
	Sound *_sound;

	// First script variable of the inventory list, latched from fnStartMenu.
	// The default is the base the game scripts pass for the main game menu.
	uint32 _invVarsBase;

	Common::Array<Step> _steps;
	// Frame the last step executed at; holds the machine to one step per game
	// cycle even though pumpStream() can run more often (see pumpStreamGame()).
	uint32 _lastStepFrame = 0;

	// Pre-action snapshot.
	int _ssePreScreen;
	Common::Array<uint16> _ssePreInventory;
	Common::Array<Common::String> _ssePreInventoryNames;
	Common::Array<Common::String> _ssePreObjectNames;
	bool _sseActionStarted;
};

} // End of namespace Sky

#endif
