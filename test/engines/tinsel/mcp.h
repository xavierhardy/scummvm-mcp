#include <cxxtest/TestSuite.h>

#include "tinsel/mcp_names.h"

#include "engines/mcp_bridge.h"

// The Tinsel naming helpers (mcp_names.cpp) are deliberately free of any
// TinselEngine dependency — like engines/mcp_bridge_text.cpp — so they link
// into the cxxtest runner without the engine runtime. Every name an agent sees
// comes from the game's own data: the label painted next to the cursor, and
// the file a scene's data lives in. These functions only fold those raw
// strings into stable identifiers.

class TinselMcpNamesTestSuite : public CxxTest::TestSuite {
public:
	// --- Scene names --------------------------------------------------------
	void test_scene_name_from_data_file() {
		TS_ASSERT_EQUALS(Tinsel::tinselSceneName("KITCHEN.GRA"), "kitchen");
		TS_ASSERT_EQUALS(Tinsel::tinselSceneName("mortuary.scn"), "mortuary");
		TS_ASSERT_EQUALS(Tinsel::tinselSceneName("PERFECT1.GRA"), "perfect1");
	}

	void test_scene_name_drops_any_folder() {
		TS_ASSERT_EQUALS(Tinsel::tinselSceneName("us/MRSCAKES.SCN"), "mrscakes");
		TS_ASSERT_EQUALS(Tinsel::tinselSceneName("data\\DW.GRA"), "dw");
	}

	void test_scene_name_of_nothing_is_nothing() {
		TS_ASSERT_EQUALS(Tinsel::tinselSceneName(""), "");
		TS_ASSERT_EQUALS(Tinsel::tinselSceneName(".GRA"), "");
	}

	// --- Labels -------------------------------------------------------------
	void test_label_becomes_an_identifier() {
		TS_ASSERT_EQUALS(Tinsel::tinselLabelToName("The Librarian"), "the_librarian");
		TS_ASSERT_EQUALS(Tinsel::tinselLabelToName("BED"), "bed");
		TS_ASSERT_EQUALS(Tinsel::tinselLabelToName("broom handle"), "broom_handle");
	}

	void test_label_punctuation_is_dropped() {
		TS_ASSERT_EQUALS(Tinsel::tinselLabelToName("Rincewind's hat!"), "rincewinds_hat");
		TS_ASSERT_EQUALS(Tinsel::tinselLabelToName("  spaced  out  "), "spaced_out");
		TS_ASSERT_EQUALS(Tinsel::tinselLabelToName("half-open door"), "half_open_door");
	}

	void test_label_with_nothing_in_it() {
		TS_ASSERT_EQUALS(Tinsel::tinselLabelToName(""), "");
		TS_ASSERT_EQUALS(Tinsel::tinselLabelToName("!?..."), "");
	}

	// --- Fallbacks and disambiguation --------------------------------------
	void test_unlabelled_things_still_have_a_name() {
		TS_ASSERT_EQUALS(Tinsel::tinselFallbackName("exit", 4), "exit_4");
		TS_ASSERT_EQUALS(Tinsel::tinselFallbackName("item", 284), "item_284");
		TS_ASSERT_EQUALS(Tinsel::tinselFallbackName(nullptr, 1), "thing_1");
	}

	void test_the_first_of_a_name_keeps_it() {
		TS_ASSERT_EQUALS(Tinsel::tinselDisambiguate("door", 0), "door");
		TS_ASSERT_EQUALS(Tinsel::tinselDisambiguate("door", 1), "door_2");
		TS_ASSERT_EQUALS(Tinsel::tinselDisambiguate("door", 2), "door_3");
	}

	// --- Round trip through the shared normalizer ---------------------------
	void test_a_published_name_survives_being_echoed_back() {
		// What resolveTarget() does to whatever an agent sends back.
		Common::String published = Tinsel::tinselLabelToName("The Librarian");
		TS_ASSERT_EQUALS(Tinsel::tinselLabelToName(published), published);
		TS_ASSERT_EQUALS(Tinsel::tinselLabelToName("the librarian"), published);
		TS_ASSERT_EQUALS(MCP::McpBridge::normalizeActionName("Look At"), "look_at");
	}
};
