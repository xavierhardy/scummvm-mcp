#include <cxxtest/TestSuite.h>

#include "gob/mcp_names.h"

#include "engines/mcp_bridge.h"

// The Gob naming helpers (mcp_names.cpp) are deliberately free of any GobEngine
// dependency — like engines/mcp_bridge_text.cpp — so they link into the cxxtest
// runner without the engine runtime. Runtime object/room names come from the
// game's status-bar text; these helpers only turn that text into stable target
// identifiers and decode TOT file names into room ids.

class GobMcpNamesTestSuite : public CxxTest::TestSuite {
public:
	// --- Object identifiers -------------------------------------------------
	void test_object_name_basic() {
		// Lower-cased, spaces to underscores, leading article stripped.
		TS_ASSERT_EQUALS(Gob::mcpGobObjectName("A trash heap"), "trash_heap");
		TS_ASSERT_EQUALS(Gob::mcpGobObjectName("The weird contraption"),
		                 "weird_contraption");
		TS_ASSERT_EQUALS(Gob::mcpGobObjectName("An onlooker"), "onlooker");
		TS_ASSERT_EQUALS(Gob::mcpGobObjectName("WOODRUFF"), "woodruff");
	}

	void test_object_name_punctuation_dropped() {
		// Everything outside [a-z0-9_] is dropped and underscore runs collapse.
		TS_ASSERT_EQUALS(Gob::mcpGobObjectName("\"Bouzouks, go home!\""),
		                 "bouzouks_go_home");
		TS_ASSERT_EQUALS(Gob::mcpGobObjectName("Site of Azimuth's house"),
		                 "site_of_azimuths_house");
	}

	void test_object_name_article_only_kept() {
		// A bare article is not stripped away to nothing.
		TS_ASSERT_EQUALS(Gob::mcpGobObjectName("the"), "the");
		TS_ASSERT_EQUALS(Gob::mcpGobObjectName(""), "");
	}

	// --- Exit detection -----------------------------------------------------
	void test_exit_labels() {
		TS_ASSERT(Gob::mcpGobIsExitLabel("TO STAIRS STREET"));
		TS_ASSERT(Gob::mcpGobIsExitLabel("To the street of the sad Boozook"));
		TS_ASSERT(!Gob::mcpGobIsExitLabel("A trash heap"));
		TS_ASSERT(!Gob::mcpGobIsExitLabel("onlooker"));
	}

	// --- Fallback names & target parsing ------------------------------------
	void test_fallback_name() {
		TS_ASSERT_EQUALS(Gob::mcpGobHotspotFallbackName(11), "hotspot_11");
	}

	void test_parse_hotspot_target() {
		TS_ASSERT_EQUALS(Gob::mcpGobParseHotspotTarget("hotspot_11"), 11);
		TS_ASSERT_EQUALS(Gob::mcpGobParseHotspotTarget("42"), 42);
		TS_ASSERT_EQUALS(Gob::mcpGobParseHotspotTarget("onlooker"), -1);
		TS_ASSERT_EQUALS(Gob::mcpGobParseHotspotTarget("hotspot_"), -1);
		TS_ASSERT_EQUALS(Gob::mcpGobParseHotspotTarget(""), -1);
	}

	// --- Room/screen names & ids from TOT files -----------------------------
	void test_screen_name() {
		TS_ASSERT_EQUALS(Gob::mcpGobScreenName("EMAP2002.TOT"), "emap2002");
		TS_ASSERT_EQUALS(Gob::mcpGobScreenName("MENU.tot"), "menu");
	}

	void test_screen_id() {
		TS_ASSERT_EQUALS(Gob::mcpGobScreenId("EMAP2002.TOT"), 2002);
		TS_ASSERT_EQUALS(Gob::mcpGobScreenId("EMAP2003.TOT"), 2003);
		TS_ASSERT_EQUALS(Gob::mcpGobScreenId("MENU.tot"), 0);
	}

	// --- Name normalization shared with the bridge --------------------------
	void test_normalization_shared() {
		TS_ASSERT_EQUALS(MCP::McpBridge::normalizeActionName("A trash heap"),
		                 "a_trash_heap");
		TS_ASSERT_EQUALS(MCP::McpBridge::normalizeActionName(" Young Woman "),
		                 "young_woman");
	}
};
