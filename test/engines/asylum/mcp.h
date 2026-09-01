#include <cxxtest/TestSuite.h>

#include "asylum/mcp_names.h"

// The Sanitarium naming helpers (mcp_names.cpp) are deliberately free of any
// engine dependency, so they link into the cxxtest runner without the engine
// runtime.
//
// Sanitarium carries a name for every object in its own data - not a label the
// player is shown, but the name its authors typed in their editor. These fold
// a builder's identifier into an agent's, and throw out the great many that
// are filler rather than names.

class AsylumMcpNamesTestSuite : public CxxTest::TestSuite {
public:
	// --- Object names -------------------------------------------------------
	void test_the_data_name_becomes_an_identifier() {
		TS_ASSERT_EQUALS(Asylum::asylumObjectName("DOOR TO HALLWAY"), "door_to_hallway");
		TS_ASSERT_EQUALS(Asylum::asylumObjectName("Chair_01"), "chair_01");
		TS_ASSERT_EQUALS(Asylum::asylumObjectName("desk"), "desk");
	}

	void test_every_kind_of_separator_is_made_uniform() {
		TS_ASSERT_EQUALS(Asylum::asylumObjectName("wall.lamp"), "wall_lamp");
		TS_ASSERT_EQUALS(Asylum::asylumObjectName("wall-lamp"), "wall_lamp");
		TS_ASSERT_EQUALS(Asylum::asylumObjectName("wall  lamp"), "wall_lamp");
		TS_ASSERT_EQUALS(Asylum::asylumObjectName("wall__lamp"), "wall_lamp");
	}

	void test_a_name_with_nothing_in_it_folds_to_nothing() {
		TS_ASSERT_EQUALS(Asylum::asylumObjectName(""), "");
		TS_ASSERT_EQUALS(Asylum::asylumObjectName(" _.- "), "");
	}

	// --- Disambiguation -----------------------------------------------------
	void test_the_first_of_a_name_keeps_it_and_the_rest_are_numbered() {
		TS_ASSERT_EQUALS(Asylum::asylumDisambiguate("chair", 0), "chair");
		TS_ASSERT_EQUALS(Asylum::asylumDisambiguate("chair", 1), "chair_2");
	}

	// --- Filler -------------------------------------------------------------
	// The game ships hundreds of objects the scripts push around that were
	// never given a name. Offering those to an agent is offering it noise.
	void test_the_editors_filler_is_not_a_name() {
		TS_ASSERT(Asylum::asylumIsPlaceholderName(""));
		TS_ASSERT(Asylum::asylumIsPlaceholderName("0"));
		TS_ASSERT(Asylum::asylumIsPlaceholderName("123"));
		TS_ASSERT(Asylum::asylumIsPlaceholderName("xxx"));
		TS_ASSERT(Asylum::asylumIsPlaceholderName("XXX"));
		TS_ASSERT(Asylum::asylumIsPlaceholderName("none"));
		TS_ASSERT(Asylum::asylumIsPlaceholderName("unused"));
	}

	// A name with a number in it is still a name.
	void test_a_real_name_survives_even_with_a_number_in_it() {
		TS_ASSERT(!Asylum::asylumIsPlaceholderName("Chair_01"));
		TS_ASSERT(!Asylum::asylumIsPlaceholderName("DOOR TO HALLWAY"));
		TS_ASSERT(!Asylum::asylumIsPlaceholderName("box2"));
	}
};
