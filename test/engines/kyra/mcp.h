#include <cxxtest/TestSuite.h>

#include "kyra/mcp_names.h"

// The Kyrandia naming helpers (mcp_names.cpp) are deliberately free of any
// engine dependency, so they link into the cxxtest runner without the engine
// runtime.
//
// Kyrandia labels nothing on screen and holds no script names. The only words
// it owns for the things in it are its item-name table - the strings it writes
// into its own sentence line - so these fold that prose into identifiers, and
// give the four compass exits the names the engine never gave them.

class KyraMcpNamesTestSuite : public CxxTest::TestSuite {
public:
	// --- Item names ---------------------------------------------------------
	void test_the_printed_name_becomes_an_identifier() {
		TS_ASSERT_EQUALS(Kyra::kyraItemName("a golden ring"), "golden_ring");
		TS_ASSERT_EQUALS(Kyra::kyraItemName("the Kyragem"), "kyragem");
		TS_ASSERT_EQUALS(Kyra::kyraItemName("Garnet"), "garnet");
	}

	// Only a leading article: the "of" in the middle belongs to the name.
	void test_an_article_in_the_middle_stays() {
		TS_ASSERT_EQUALS(Kyra::kyraItemName("a piece of the moon"), "piece_of_the_moon");
	}

	void test_punctuation_is_dropped_and_separators_do_not_double_up() {
		TS_ASSERT_EQUALS(Kyra::kyraItemName("Zanthia's potion"), "zanthias_potion");
		TS_ASSERT_EQUALS(Kyra::kyraItemName("fire-berry"), "fire_berry");
		TS_ASSERT_EQUALS(Kyra::kyraItemName("blue  stone"), "blue_stone");
	}

	void test_a_name_with_nothing_in_it_folds_to_nothing() {
		TS_ASSERT_EQUALS(Kyra::kyraItemName(""), "");
		TS_ASSERT_EQUALS(Kyra::kyraItemName("  "), "");
	}

	// --- Disambiguation -----------------------------------------------------
	void test_the_first_of_a_name_keeps_it_and_the_rest_are_numbered() {
		TS_ASSERT_EQUALS(Kyra::kyraDisambiguate("ring", 0), "ring");
		TS_ASSERT_EQUALS(Kyra::kyraDisambiguate("ring", 1), "ring_2");
	}

	void test_nothing_is_never_numbered() {
		TS_ASSERT_EQUALS(Kyra::kyraDisambiguate("", 2), "");
	}

	// --- Exits --------------------------------------------------------------
	// A scene says which scene lies north of it and where to click; it never
	// says what that way out is called, so the bridge names it.
	void test_the_four_ways_out_are_named_for_their_directions() {
		TS_ASSERT_EQUALS(Kyra::kyraExitName(0), "exit_north");
		TS_ASSERT_EQUALS(Kyra::kyraExitName(1), "exit_east");
		TS_ASSERT_EQUALS(Kyra::kyraExitName(2), "exit_south");
		TS_ASSERT_EQUALS(Kyra::kyraExitName(3), "exit_west");
	}

	void test_there_is_no_fifth_direction() {
		TS_ASSERT_EQUALS(Kyra::kyraExitName(4), "");
		TS_ASSERT_EQUALS(Kyra::kyraExitName(-1), "");
	}

	// --- Empty slots --------------------------------------------------------
	// Both generations keep fixed-length item tables. The first spells an
	// empty slot -1 and the later two spell it 0xFFFF; either way there is
	// nothing there to offer an agent.
	void test_both_spellings_of_an_empty_slot_are_recognised() {
		TS_ASSERT(Kyra::kyraIsEmptyItem(-1));
		TS_ASSERT(Kyra::kyraIsEmptyItem(0xFFFF));
	}

	void test_a_real_item_id_is_not_empty() {
		TS_ASSERT(!Kyra::kyraIsEmptyItem(0));
		TS_ASSERT(!Kyra::kyraIsEmptyItem(43));
	}
};
