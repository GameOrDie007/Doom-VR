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
	std::string path;		// the file, or a mod's folder
	std::vector<std::string> more;		// folders that load after it: its -Extras
	std::vector<std::string> unpack;	// an archive with the mod one folder down: those folders
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
// What a row loads, in order. An archive with the mod one folder down is
// unpacked first, the first time it is played.
std::vector<std::string> DoomVR_MyWadFiles(const DoomVRMyWad& w);
