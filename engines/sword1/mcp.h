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

#ifndef SWORD1_MCP_H
#define SWORD1_MCP_H

#include "engines/mcp_bridge.h"

#include "common/array.h"
#include "common/formats/json.h"
#include "common/str.h"

namespace Sword1 {

class SwordEngine;
struct Object;

// MCP bridge for Broken Sword: The Shadow of the Templars.
//
// Broken Sword is a one-click game: a left click on an object runs its
// interaction script, a right click looks at it, and there is no verb bar. So
// the `act` tool exposes a fixed verb set and maps everything but look_at onto
// the left-click path — the tool's description says so, and state.verbs
// advertises it.
//
// Every action is dispatched by replaying what Mouse::engine() does for a real
// click (mouse.cpp): position MOUSE_X/MOUSE_Y, run the target's o_mouse_on
// script, set MOUSE_BUTTON, then run its o_mouse_click script through
// Logic::runMouseScript(). Driving the game's own scripts rather than the
// underlying opcodes is what keeps the player's script tree, o_place and the
// get-to routing consistent.
//
// All coordinates on the wire are world (compact) coordinates — the space of
// o_xcoord/o_ycoord, o_mouse_x1..y2 and _scriptVars[MOUSE_X/MOUSE_Y]. It is the
// only space every source of truth already agrees on, and it is scroll
// invariant, so coordinates an agent remembers stay valid across a scroll.
class Sword1McpBridge : public MCP::McpBridge {
public:
	// Factory, mirroring the SCUMM bridge's two-phase construction: build the
	// object, then init() it (tool registration dispatches through virtual
	// hooks, so it cannot run from a constructor). Broken Sword needs only one
	// leaf today; a per-game split would happen here.
	static Sword1McpBridge *create(SwordEngine *vm);

	explicit Sword1McpBridge(SwordEngine *vm);
	~Sword1McpBridge() override;

	// Called from Logic::fnISpeak for every line the game says, whether or not
	// subtitles are enabled. `id` is the speaking compact.
	void onSpeech(int compactId, const Common::String &text, bool isVoiceOver);

protected:
	// --- Tools --------------------------------------------------------------
	Common::JSONValue *toolState(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolAct(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolAnswer(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolWalk(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolSkip(const Common::JSONValue &args, Common::String &errorOut) override;
	Common::JSONValue *toolDebug(const Common::JSONValue &args, Common::String &errorOut) override;

	Common::String debugToolDescription() const override;
	Common::JSONValue *buildDebugSchema() const override;
	void augmentStateSchema(Common::JSONObject &outputProps) override;

	// Reject every mutating tool while the game cannot accept input, so a call
	// arriving through one of the secondary pump sites (a fade, the control
	// panel, a cutscene) cannot corrupt state the engine is mid-way through.
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
	void pumpStreamTrack() override;

	// Broken Sword's main loop runs at ~12.5 Hz (DEFAULT_FRAME_TIME 80), about a
	// fifth of SCUMM's rate, so every frame budget is scaled down to match.
	uint32 minStreamFrames() const override { return 3; }
	uint32 stuckFrames(bool hadActivity) const override { return hadActivity ? 18 : 3; }
	uint32 timeoutFrames() const override { return 125; }        // ~10 s
	uint32 absoluteTimeoutFrames() const override { return 750; } // ~60 s
	uint32 settleFrames() const override { return 3; }
	// The frame counter stops advancing entirely while a cutscene or the control
	// panel owns the main loop (those only reach pumpTransportOnly()), so a
	// frame-anchored timeout can never fire there. This is the backstop.
	uint32 wallClockTimeoutMs() const override { return 180000; }

private:
	// Is the game accepting player input right now? Mirrors Mouse::engine()'s own
	// gate: bit 0 of MOUSE_STATUS is "human on", bit 1 is "locked".
	bool canAct() const;
	// True while the control panel (save/load/options) owns the screen.
	bool panelShown() const;

	// Enumerate every live compact on the current screen, exactly as
	// Logic::engine() does. `mouseableOnly` applies the STAT_MOUSE filter that
	// decides whether the game would let the player click the object at all.
	// Reading Mouse::_objList instead would be wrong: it is cleared at the end of
	// every Mouse::engine() and is only valid for one window per cycle, so it
	// would come back empty from the cutscene and control-panel pump sites.
	void collectScreenObjects(Common::Array<uint32> &ids, bool mouseableOnly) const;

	// Resolve a tool target to either a scene compact or an inventory item.
	// Exactly one of compactId / pocketNo is non-zero on success.
	bool resolveTarget(const Common::String &name, uint32 &compactId, int &pocketNo,
	                   Common::String &errorOut) const;

	// Replay a real click on a scene compact: position the cursor, run the
	// hover script, then the click script. `rightClick` selects look-at.
	bool clickCompact(uint32 id, bool rightClick, Common::String &errorOut);

	// World -> screen conversion, the inverse of mouse.cpp's screen -> world.
	// Only needed when warping the physical cursor.
	void worldToScreen(int worldX, int worldY, int &screenX, int &screenY) const;

	// Current inventory as pocket numbers.
	void collectInventory(Common::Array<int> &pockets) const;

	// The description the game itself attaches to an inventory item, via the
	// item's textDesc ordinal. This is the only human-readable text Broken Sword
	// attaches to anything, so it is worth surfacing next to the authored name.
	Common::String pocketDescription(int pocketNo) const;

	SwordEngine *_vm;

	// Pre-action snapshot.
	int _ssePreScreen;
	Common::Array<int> _ssePreInventory;
	// Names captured at snapshot time so a consumed item can still be reported.
	Common::Array<Common::String> _ssePreInventoryNames;
	// True once the current stream has shown a visible effect (George started
	// walking, a line was said, or the screen changed). Until then isActionDone()
	// is held false so a stream cannot settle-close in the frames before the
	// clicked script has had a chance to act.
	bool _sseActionStarted = false;
};

} // End of namespace Sword1

#endif
