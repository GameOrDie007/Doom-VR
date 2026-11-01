/*
** doomvr_mywads.h
**
** My WADs: the player's own WADs and pk3s, in <install>\My WADs\ - each one
** on the game it was made for (doomvr_switchgame.cpp says how that is chosen).
** Switch Game lists them; Build Your Own Game offers them as pieces.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#pragma once

#include <string>
#include <vector>

struct DoomVRMyWad
{
	std::string label;		// its row: the file's name, made unique
	std::string path;		// the file
	std::string launcher;	// the game it plays on
	std::string kind;		// "doom1" (ExMy maps) or "doom2" (MAPxx, or no maps)
	bool maps = false;		// it has maps of its own
	// What it brings of its own, as setup measures a megawad (setup.ps1's
	// ContentTags): ownmonsters, somemonsters, ownactors, owntextures,
	// ownmusic, ownsounds, ownweapons - and gameplay, for a file without maps
	// that changes how things behave (a gameplay mod).
	std::vector<std::string> tags;
};

std::vector<DoomVRMyWad> DoomVR_MyWads();
