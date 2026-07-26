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

	// --- Localised text -----------------------------------------------------
	// The game stores its text in the DOS OEM code page (CP850) even in its
	// Windows release, so a localised release draws accented letters as single
	// high bytes. Left alone they are not valid UTF-8 and are lost on the wire.
	void test_text_to_utf8() {
		// Pure ASCII is already UTF-8 and must come back untouched.
		TS_ASSERT_EQUALS(Gob::mcpGobTextToUtf8("A trash heap"), "A trash heap");
		// 0x82 = e-acute, 0x85 = a-grave in CP850.
		TS_ASSERT_EQUALS(Gob::mcpGobTextToUtf8("Un recueil de d\x82tritus"),
		                 "Un recueil de d\xC3\xA9tritus");
		TS_ASSERT_EQUALS(Gob::mcpGobTextToUtf8("Etes-vous \x85 jour"),
		                 "Etes-vous \xC3\xA0 jour");
	}

	void test_fold_accents() {
		TS_ASSERT_EQUALS(Gob::mcpGobFoldAccents("d\xC3\xA9tritus"), "detritus");
		TS_ASSERT_EQUALS(Gob::mcpGobFoldAccents("\xC3\xA9trange"), "etrange");
		TS_ASSERT_EQUALS(Gob::mcpGobFoldAccents("gar\xC3\xA7on"), "garcon");
		TS_ASSERT_EQUALS(Gob::mcpGobFoldAccents("plain"), "plain");
	}

	// Accented identifiers stay typeable, and the French articles are stripped
	// like the English ones. The labels arrive already decoded (the bridge
	// converts once, in onTextDrawn), so these are the UTF-8 forms — decoding
	// again here would turn valid UTF-8 into mojibake.
	void test_object_name_localised() {
		TS_ASSERT_EQUALS(Gob::mcpGobObjectName("Un recueil de d\xC3\xA9tritus"),
		                 "recueil_de_detritus");
		TS_ASSERT_EQUALS(Gob::mcpGobObjectName("Un \xC3\xA9trange dispositif"),
		                 "etrange_dispositif");
		TS_ASSERT_EQUALS(Gob::mcpGobObjectName("Une botte en cuir"), "botte_en_cuir");
		TS_ASSERT_EQUALS(Gob::mcpGobObjectName("Un badaud"), "badaud");
	}

	// --- Exit detection -----------------------------------------------------
	void test_exit_labels() {
		TS_ASSERT(Gob::mcpGobIsExitLabel("TO STAIRS STREET"));
		TS_ASSERT(Gob::mcpGobIsExitLabel("To the street of the sad Boozook"));
		TS_ASSERT(!Gob::mcpGobIsExitLabel("A trash heap"));
		TS_ASSERT(!Gob::mcpGobIsExitLabel("onlooker"));
	}

	// The exits name where they lead in the game's own language.
	void test_exit_labels_localised() {
		TS_ASSERT(Gob::mcpGobIsExitLabel("VERS LA RUE DE L'ESCALIER"));
		TS_ASSERT(Gob::mcpGobIsExitLabel("VERS LA RUE DU BOUZOUK TRISTE"));
		TS_ASSERT(!Gob::mcpGobIsExitLabel("Un recueil de d\xC3\xA9tritus"));
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
