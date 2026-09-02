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

#ifndef ENGINES_MCP_BRIDGE_H
#define ENGINES_MCP_BRIDGE_H

#include "backends/networking/mcp/mcp_server.h"

#include "common/array.h"
#include "common/formats/json.h"
#include "common/keyboard.h"
#include "common/str.h"

class Engine;

namespace MCP {

// Engine-agnostic half of an MCP integration.
//
// Networking::McpServer is pure transport (sockets, HTTP framing, JSON-RPC,
// SSE). This class sits one level up: it owns the server, reads the `mcp*`
// configuration keys, keeps the captured-message queue, registers the common
// tool set, and runs the per-frame streaming state machine that turns a
// long-running game action into an SSE call.
//
// Everything that needs to know what a room, an object or a verb *is* stays in
// the per-engine subclass, reached through the hooks below. Six subclasses
// exist today: Scumm::ScummMcpBridge, Sword1::Sword1McpBridge,
// Sky::SkyMcpBridge, Queen::QueenMcpBridge, Gob::GobMcpBridge and
// Tinsel::TinselMcpBridge.
//
// Construction is two-phase: construct the leaf, then call init(). Tool
// registration dispatches through virtual hooks, so it cannot run from a base
// constructor.
class McpBridge : public Networking::McpServer::IToolHandler {
public:
	explicit McpBridge(Engine *engine, const Common::String &serverName = "scummvm",
	                   const Common::String &serverVersion = "1.0");
	~McpBridge() override;

	// Finish construction: register tools with the server. Must run after the
	// object is fully constructed. Called by each engine's create() factory.
	void init();

	// True when `mcp=true` was set and the server bound its port.
	bool isEnabled() const { return _enabled; }

	// Call once per game-loop frame. Advances the frame counter (the unit every
	// streaming budget below is expressed in) and services the server.
	virtual void pump();

	// Service the server and an in-flight stream *without* advancing the frame
	// counter. For engines whose main loop stalls (cutscene players, modal
	// panels, palette fades) and that still want the server to answer there.
	// Frame-anchored budgets therefore keep their game-cycle meaning across such
	// a stall; wallClockTimeoutMs() is the escape hatch that still fires.
	void pumpTransportOnly();

	// Text capture from the engine.
	virtual void onActorLine(int actorId, const Common::String &text);
	virtual void onSystemLine(const Common::String &text);
	virtual void onDialogPrompt(const Common::String &text);

	// Fold a verb/target name into its canonical form: whitespace and name
	// padding stripped, lower-cased, spaces and dashes turned into underscores,
	// then a table of common aliases applied ("examine" -> "look_at", …).
	static Common::String normalizeActionName(const Common::String &action);

	// IToolHandler
	Common::JSONValue *callTool(const Common::String &name,
	                            const Common::JSONValue &args,
	                            Common::String &errorOut) override;
	void pumpStream() override;

protected:
	// A line of game text captured from the engine, queued until the next
	// streaming pump can emit it as an MCP notification.
	struct MessageEntry {
		uint64 seq;
		uint32 frame;
		int room;
		int actorId;
		Common::String type;
		Common::String text;
	};

	// --- Tool implementations (per engine) ---------------------------------
	// Sync tools return a JSON value (ownership passes to the server) or
	// nullptr with errorOut set. Streaming tools validate, call
	// beginStream()/_server->startStreaming() and return true; on a validation
	// failure they set errorOut and return false without starting a stream.

	virtual Common::JSONValue *toolState(const Common::JSONValue &args,
	                                     Common::String &errorOut) = 0;
	virtual bool toolAct(const Common::JSONValue &args, Common::String &errorOut) = 0;
	virtual bool toolAnswer(const Common::JSONValue &args, Common::String &errorOut) = 0;
	virtual bool toolWalk(const Common::JSONValue &args, Common::String &errorOut) = 0;
	virtual bool toolSkip(const Common::JSONValue &args, Common::String &errorOut) = 0;
	virtual Common::JSONValue *toolDebug(const Common::JSONValue &args,
	                                     Common::String &errorOut) = 0;

	// --- Tool registration hooks -------------------------------------------

	// Add engine-specific fields to the `state` tool's output schema.
	virtual void augmentStateSchema(Common::JSONObject &outputProps) { (void)outputProps; }
	// Add engine-specific fields to the shared streaming-result schema.
	virtual void augmentChangesSchema(Common::JSONObject &props) { (void)props; }
	// Add engine-specific arguments to the `act` tool's input schema (a game
	// whose targets are points on screen rather than named things needs
	// coordinates there).
	virtual void augmentActSchema(Common::JSONObject &props) { (void)props; }
	// Register tools specific to this game (shoot_cannon, play_note, …).
	virtual void registerGameTools() {}
	// Register debug tools beyond the shared set. Only called when mcp_debug is on.
	virtual void registerDebugTools() {}
	// Handle a tool call callTool() did not recognise. Set handled when consumed.
	virtual Common::JSONValue *dispatchGameTool(const Common::String &name,
	                                            const Common::JSONValue &args,
	                                            Common::String &errorOut, bool &handled) {
		(void)name; (void)args; (void)errorOut; handled = false; return nullptr;
	}
	// --- Tool descriptions --------------------------------------------------
	// What an agent reads about each tool. The defaults describe what every
	// game here has in common; a game whose tool behaves differently — or whose
	// snapshot carries different fields — overrides the one that differs, so
	// the text always matches what the tool actually does. Descriptions never
	// name a game or an engine, and never describe how the bridge is built:
	// what an agent needs is what it can ask for and what comes back.
	virtual Common::String stateToolDescription() const;
	virtual Common::String actToolDescription() const;
	virtual Common::String answerToolDescription() const;
	virtual Common::String walkToolDescription() const;
	virtual Common::String skipToolDescription() const;
	// Description shown for the `debug` tool (engines expose different state).
	virtual Common::String debugToolDescription() const;
	// Input schema for the `debug` tool. Ownership passes to the server.
	virtual Common::JSONValue *buildDebugSchema() const;
	// Output schema for the `debug` tool. The default leaves the sections open
	// (they are diagnostics, and differ per game); ownership passes to the
	// server.
	virtual Common::JSONValue *buildDebugOutputSchema() const;

	// Does this game ever put a question to the player? When it does not, the
	// `answer` tool is not registered and nothing refers to it.
	virtual bool usesDialogQuestions() const { return true; }

	// Does this game ever ask for something to be *typed*? Most of these games
	// are pointed at and never take a word from the keyboard, so the tool is
	// off by default and nothing refers to it. Turn it on for a game that
	// stops and waits for a line - a copy-protection screen asking for a
	// number, a parser waiting for a sentence, a name being entered.
	virtual bool usesTypedInput() const { return false; }
	// What an agent reads about `type_text`.
	virtual Common::String typeTextToolDescription() const;

	// The sentence every streaming tool ends with. Kept in one place so all of
	// them say the same thing, and so a tool that is not registered is never
	// named.
	Common::String streamingToolNote() const;

	// --- Streaming hooks ----------------------------------------------------

	// Capture the state an action will be diffed against. Called by beginStream().
	virtual void snapshotPreAction() = 0;
	// Build the streaming result: what changed since snapshotPreAction().
	virtual Common::JSONObject buildStateChanges() const = 0;
	// True once the engine looks idle again (no walk, no speech, input restored).
	// Latches the settle window; it does not close the stream by itself.
	virtual bool isActionDone() const = 0;
	// True when the game is waiting on a dialog choice.
	virtual bool hasPendingQuestion() const = 0;
	// True when the current room/screen differs from the pre-action snapshot;
	// closes the stream immediately (nothing left to settle in the old room).
	virtual bool streamRoomChanged() const = 0;
	// True when the engine is frozen with no visible progress (input locked and
	// nothing being said). Held for stuckFrames() frames before closing.
	virtual bool isStreamStuck() const { return false; }
	// True once the stream has shown any sign of life. Selects between the two
	// stuckFrames() budgets.
	virtual bool streamHadActivity() const { return _sseLastEventFrame > 0; }

	// Ordered per-frame streaming steps, run by pumpStream() between the generic
	// stages. Each defaults to doing nothing; see pumpStream() for the exact
	// order and for what belongs in which slot.
	virtual bool pumpStreamGameEarly() { return false; }  // true = end this frame now
	virtual void pumpStreamGame() {}                      // before the close/timeout checks
	virtual void pumpStreamTrack() {}                     // progress tracking (bumps the event frame)
	virtual void pumpStreamMid() {}                       // after the timeout checks
	virtual void pumpStreamGameLate() {}                  // after pumpStreamMid, before pre-settle
	virtual void pumpStreamPreSettle() {}                 // last step before the settle decision

	// Per-frame engine step run from pump() before the server is serviced.
	virtual void pumpGame() {}

	// --- Streaming budgets --------------------------------------------------
	// All in frames, i.e. in pump() calls, so an engine running at 12 Hz wants
	// roughly a fifth of an engine running at 60 Hz. Defaults are SCUMM's.

	// Frames an action must run before isActionDone() is even consulted.
	virtual uint32 minStreamFrames() const { return 3; }
	// Frames isStreamStuck() must hold before the stream is closed.
	virtual uint32 stuckFrames(bool hadActivity) const { return hadActivity ? 90 : 15; }
	// Frames without progress before the stream is failed as timed out.
	virtual uint32 timeoutFrames() const { return 600; }
	// Absolute ceiling since the stream started; 0 disables it.
	virtual uint32 absoluteTimeoutFrames() const { return 0; }
	// Frames the engine must stay done before the stream closes.
	virtual uint32 settleFrames() const { return 10; }
	// Wall-clock ceiling in milliseconds; 0 disables it. Needed by engines whose
	// frame counter can stop advancing (see pumpTransportOnly()).
	virtual uint32 wallClockTimeoutMs() const { return 0; }

	// How long an action that is *done* may be held back by the frame-based
	// gates below when the frame counter has stopped. A frame is a game cycle,
	// and an engine that is not running cycles - one sitting in a tight input
	// loop, say - never advances it, so minStreamFrames() and settleFrames()
	// can never be satisfied and the only thing that ever fires is the
	// wall-clock *timeout*, which reports a failure for an action that in fact
	// finished. Real time is the only clock running there. Zero, the default,
	// leaves every engine's behaviour exactly as it was.
	virtual uint32 wallClockCloseMs() const { return 0; }
	// The frame the timeout is measured from. By default the stream start, so
	// background chatter cannot hold a stream open forever.
	virtual uint32 streamTimeoutAnchor() const { return _sseStartFrame; }
	// The moment wallClockTimeoutMs() is measured from, and the counterpart of
	// streamTimeoutAnchor(). By default the stream start; an engine whose frame
	// deadline follows the last sign of life can move this one there too
	// (_sseLastEventMs), so an action that is plainly still progressing is not
	// cut short on a slow machine while a silent one still is.
	virtual uint32 streamTimeoutAnchorMs() const { return _sseStartMs; }
	// How long speech may go on holding a stream open once the action itself has
	// finished (see _sseWorkDoneFrame). A room can talk to itself forever —
	// Zak's living-room TV prints a line every few seconds with the player in
	// full control — and without a bound every action there runs until it times
	// out. Lines that arrive after the allowance are not lost: they are captured
	// like any other and surface in the next state call. 0 disables the bound.
	virtual uint32 postActionSpeechFrames() const { return 0; }
	// Final say on closing a settled stream. The default closes on a pending
	// question or once settleFrames() have elapsed since the engine went idle.
	virtual bool shouldCloseStream() const;

	// True once the action has been finished for longer than the post-action
	// speech allowance. From then on speech neither counts as the action still
	// running nor extends the settle window, so a room that talks to itself
	// cannot hold every action open until it times out.
	bool postActionSpeechExpired() const {
		uint32 limit = postActionSpeechFrames();
		return limit != 0 && _sseWorkDoneFrame != 0 &&
		       (_frameCounter - _sseWorkDoneFrame) > limit;
	}

	// --- Input injection (per engine) ---------------------------------------
	// The debug tools own their schema and argument parsing here; the effect on
	// the engine belongs to the subclass.

	virtual void injectKey(const Common::KeyState &ks) = 0;
	virtual void injectMouseMove(int x, int y) = 0;
	virtual void injectMouseClick(int x, int y, const Common::String &button, bool isDouble) = 0;

	// --- Text handling hooks ------------------------------------------------

	// Display name for a speaking actor, used to tag notifications. Empty means
	// the notification carries no `actor` field.
	virtual Common::String messageActorName(int actorId) const { (void)actorId; return Common::String(); }
	// Room/screen id stamped on captured messages.
	virtual int currentRoomForMessages() const { return 0; }
	// True for engines whose spoken lines arrive with the original interpreter's
	// 0xFF-coded talkie/sound blocks still attached (SCUMM V6+).
	virtual bool stripTalkieMetadata() const { return false; }
	// Convert raw game text to UTF-8. The default sanitizes at byte level;
	// engines that know their code page should override.
	virtual Common::String safeUtf8(const Common::String &raw) const;

	// --- Streaming helpers for subclasses -----------------------------------

	// Arm a stream: snapshot, reset the per-stream counters, open the SSE
	// channel. Call after the action has been dispatched to the engine.
	void beginStream();
	// Per-stream bookkeeping that must happen before the action runs: call it
	// from beginStream() or, for bridges that open-code the stream setup, from
	// their snapshotPreAction().
	void noteStreamStart();
	// Close the active stream with the diff built by buildStateChanges().
	void closeStreamSuccess();
	// Close the active stream as a failure (JSON-RPC -32000).
	void closeStreamFailure(const Common::String &reason);
	// True while a streaming tool call is in flight.
	bool isStreaming() const { return _streaming; }

	// Queue a line of game text for emission on the active stream.
	void pushMessage(const char *type, int actorId, const Common::String &text);
	// Drain the queue into SSE notifications. Called at the top of pumpStream().
	void emitPendingMessages();

	// Shared output schema for the streaming tools (act/answer/walk/skip); also
	// used by subclasses registering their own streaming tools.
	Common::JSONValue *buildChangesSchema() const;

	Engine *_engine;
	bool _enabled;
	bool _skipToolEnabled;
	bool _debugToolsEnabled;
	Networking::McpServer *_server;

	Common::Array<MessageEntry> _messages;
	uint64 _nextMessageSeq;
	uint32 _frameCounter;

	// Streaming (act/answer/walk/skip) state.
	bool _streaming;
	uint32 _sseStartFrame;
	uint32 _sseStartMs;
	uint32 _sseDoneAtFrame;
	uint32 _sseStuckAtFrame;
	uint32 _sseLastEventFrame;  // frame of the most recent event seen during the stream
	uint32 _sseLastEventMs;     // when that frame was, in real time
	// First frame on which everything the action itself had to do was finished
	// (ego idle, sentence dispatched, input restored) — speech aside. Maintained
	// by the engine bridge's pumpStreamTrack(); 0 while the action is still
	// running. Anchors the post-action speech allowance.
	uint32 _sseWorkDoneFrame;
	Common::Array<MessageEntry> _sseMessages;
	int _ssePreRoom;
	int _ssePrePosX, _ssePrePosY;

	// Frame at which a simulated mouse button should be released, so the engine
	// sees a complete press/release cycle rather than a drag.
	uint32 _debugButtonReleaseFrame;

private:
	void registerTools();
	Common::JSONValue *toolScreenshot(const Common::JSONValue &args, Common::String &errorOut);
	bool toolKeystroke(const Common::JSONValue &args, Common::String &errorOut);
	Common::JSONValue *toolTypeText(const Common::JSONValue &args, Common::String &errorOut);
	bool toolMouseMove(const Common::JSONValue &args, Common::String &errorOut);
	bool toolMouseClick(const Common::JSONValue &args, Common::String &errorOut);

protected:
	// save_state defers to the engine's own save path. SCUMM overrides it to use
	// its deferred requestSave().
	virtual Common::JSONValue *toolSaveState(const Common::JSONValue &args,
	                                         Common::String &errorOut);
};

// Strip the trailing '@' padding some engines use to pad object names to a
// fixed width, plus any trailing spaces.
Common::String mcpStripNamePadding(const Common::String &s);

// Collapse a raw game string into something safe to put on the wire: control
// codes dropped, whitespace normalized.
Common::String mcpCleanGameText(const Common::String &text);

// Parse a key name ("Escape", "F1", "a") plus modifiers into a KeyState.
bool mcpJsonKeyToKeyState(const Common::String &name, bool ctrl, bool shift, bool alt,
                          Common::KeyState &out);

} // End of namespace MCP

#endif
