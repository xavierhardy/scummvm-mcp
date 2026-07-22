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

#include "sky/mcp_names.h"

#include "common/util.h"

namespace Sky {

// The people Foster can talk to, by compact id. Same set the engine's touch UI
// (Mouse::getInteractIcon) tags with its talk icon.
static const uint32 kCharacterIds[] = {
	1,     // joey
	16,    // lamb
	136,   // power room man
	137,   // anita
	4122,  // hobbins
	8205,  // rad suit man
	8211,  // sam
	8301,  // norville
	8309,  // guard on walkway
	8324,  // guard
	8544,  // clipboard man
	12289, // mr cool
	12407, // dr burke
	12430, // anchor man
	12442, // insurance man
	12546, // galagher
	16441, // piermont
	16516, // henri
	16599, // gameboy kid
	16600, // gardener
	16601, // guard
	16701, // barman
	16731, // man
	16737, // man
	16772, // babs
	20911, // ken
	21014  // father
};

bool skyIsCharacter(uint32 compactId) {
	for (uint i = 0; i < ARRAYSIZE(kCharacterIds); i++)
		if (kCharacterIds[i] == compactId)
			return true;
	return false;
}

// Screen names for the areas the game visits. Only the screens with an obvious
// authored identity are named; everything else reports its number only.
struct ScreenName {
	uint32 screen;
	const char *name;
};

static const ScreenName kScreenNames[] = {
	{ 0, "plant_walkway" },  // the game's opening screen, top of the plant
	{ 1, "plant_overhang" }, // the ledge behind the forced door
	{ 2, "lift_room" }       // transporter robot, lift, and Joey's shell
};

const char *skyScreenName(uint32 screen) {
	for (uint i = 0; i < ARRAYSIZE(kScreenNames); i++)
		if (kScreenNames[i].screen == screen)
			return kScreenNames[i].name;
	return nullptr;
}

} // End of namespace Sky
