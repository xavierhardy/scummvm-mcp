#include <cxxtest/TestSuite.h>

#include "sky/mcp_names.h"

#include "engines/mcp_bridge.h"

// The sky naming tables (mcp_names.cpp) are deliberately free of any
// SkyEngine dependency — like engines/mcp_bridge_text.cpp — so they link into
// the cxxtest runner without the engine runtime. Runtime names come from the
// game data (cursorText / authored compact names); these tables only tag the
// talkable characters and name a few screens.

class SkyMcpNamesTestSuite : public CxxTest::TestSuite {
public:
	// --- Character tagging -------------------------------------------------
	void test_known_characters() {
		TS_ASSERT(Sky::skyIsCharacter(1));     // joey
		TS_ASSERT(Sky::skyIsCharacter(16));    // lamb
		TS_ASSERT(Sky::skyIsCharacter(137));   // anita
		TS_ASSERT(Sky::skyIsCharacter(4122));  // hobbins
		TS_ASSERT(Sky::skyIsCharacter(21014)); // father at the end
	}

	void test_non_characters() {
		TS_ASSERT(!Sky::skyIsCharacter(0));
		TS_ASSERT(!Sky::skyIsCharacter(3));    // foster himself is not listed
		TS_ASSERT(!Sky::skyIsCharacter(90));   // screen 0 door
		TS_ASSERT(!Sky::skyIsCharacter(0xFFFFFFFF));
	}

	// --- Screen names -------------------------------------------------------
	void test_named_screens() {
		TS_ASSERT_EQUALS(Common::String(Sky::skyScreenName(0)), "plant_walkway");
		TS_ASSERT_EQUALS(Common::String(Sky::skyScreenName(1)), "plant_overhang");
	}

	void test_unnamed_screen_is_null() {
		TS_ASSERT(Sky::skyScreenName(9999) == nullptr);
	}

	// --- Name normalization shared with the bridge --------------------------
	void test_cursor_text_normalization() {
		// What compactDisplayName() does to a decoded cursorText string.
		TS_ASSERT_EQUALS(MCP::McpBridge::normalizeActionName("ROBOT SHELL"), "robot_shell");
		TS_ASSERT_EQUALS(MCP::McpBridge::normalizeActionName(" Metal Bar "), "metal_bar");
	}
};
