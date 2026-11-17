/*
** doomvr_multiplayer.cpp - Doom VR's Multiplayer menu: host a game, join one,
** and the lobby, all inside the game.
**
** UZDoom starts a network game only from its command line (-host N, -join
** address), with the lobby in a desktop window before the game's own window
** exists. Doom VR hosts and joins from its menu instead: the game restarts in
** this process with the network arguments added - the window, and the headset,
** stay up - and the lobby is drawn as game frames (DoomVR_NetLobbyFrame) on
** the same screen as the menus. Backing out of the lobby, or a lobby that
** fails (a wrong address, a full game, different game files), comes back to
** the game alone with the reason.
**
** Written for Doom VR.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include "doomvr_headset.h"
#include "doomstat.h"
#include "d_player.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "printf.h"
#include "cmdlib.h"
#include "m_argv.h"
#include "menu.h"
#include "menustate.h"
#include "d_eventbase.h"
#include "keydef.h"
#include "g_mapinfo.h"
#include "p_setup.h"
#include "v_font.h"
#include "v_video.h"
#include "v_draw.h"
#include "i_time.h"
#include "i_system.h"
#include "gamestate.h"
#include "d_net.h"
#include "i_net.h"
#include "g_game.h"
#include "filesystem.h"
#include "fs_findfile.h"
#include "resourcefile.h"
#include "serializer.h"
#include "savegamemanager.h"
#include "version.h"
#include "texturemanager.h"
#include <algorithm>

// The Game Or Die server: pinned first on Join a Game and never removed, once
// it is live. Empty: not shown (the server is not running yet).
static const char* const kGameOrDieServer = "";
static const char* const kGameOrDieServerName = "Game Or Die server";

CVAR(Int, vr_net_mode, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)			// 0 co-op
CVAR(Int, vr_net_players, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(String, vr_net_map, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)		// the host's first map; empty: the game's first
CVAR(String, vr_net_address, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// typed on Join a Game
CVAR(String, vr_net_saved, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)		// addresses joined before, newest first, ';' between
CVAR(String, vr_net_load, "new", 0)	// Host a Game: the co-op save to carry on from; "new" (or empty): a new game

extern bool wantToRestart;
void DoomVR_ForgetGame();
void D_Display();
void I_SetGameLobby(bool (*frame)());
const char* I_GameLobbyError();
void I_GameLobbyStartNow();
bool I_GetGameLobbyView(FString& status, bool& host, int& current, int& limit, TArray<FString>& names, TArray<bool>& ready);
FString I_LocalAddresses();
bool D_CheckNetGame();

static const int kMaxSaved = 8;
static FString gLobbyNotice;	// why the last lobby ended, shown once the title is up

static std::vector<std::string> SavedAddresses()
{
	std::vector<std::string> out;
	std::string all = *vr_net_saved;
	size_t start = 0;
	while (start <= all.size())
	{
		const size_t end = all.find(';', start);
		std::string a = all.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if (!a.empty()) out.push_back(a);
		if (end == std::string::npos) break;
		start = end + 1;
	}
	return out;
}

static void SetSavedAddresses(const std::vector<std::string>& list)
{
	std::string all;
	for (auto& a : list) { if (!all.empty()) all += ';'; all += a; }
	vr_net_saved = all.c_str();
}

// The current game's arguments without any network ones (-host N, -join addr).
static TArray<FString> ArgsWithoutNet()
{
	TArray<FString> args;
	for (int i = 0; i < Args->NumArgs(); ++i)
	{
		const char* a = Args->GetArg(i);
		if (stricmp(a, "-host") == 0 || stricmp(a, "-join") == 0 || stricmp(a, "-loadgame") == 0)
		{
			if (i + 1 < Args->NumArgs() && Args->GetArg(i + 1)[0] != '-' && Args->GetArg(i + 1)[0] != '+') ++i;
			continue;
		}
		args.Push(a);
	}
	return args;
}

// Into the same game again, in this process, with these arguments.
static void RestartWith(TArray<FString>& args, const char* why)
{
	*Args = FArgs(int(args.Size()), args.Data());
	Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: multiplayer - %s; the game restarts in place\n", why);
	M_ClearMenus();
	DoomVR_ForgetGame();
	wantToRestart = true;
}

// The maps the host can start on: the game's own, as its map list names them.
static void BuildMapChoices()
{
	FOptionValues* vals = new FOptionValues;
	bool chosenThere = false;
	for (auto& info : wadlevelinfos)
	{
		if (info.MapName.IsEmpty() || !P_CheckMapData(info.MapName.GetChars())) continue;
		FOptionValues::Pair& p = vals->mValues[vals->mValues.Reserve(1)];
		p.Value = 0;
		p.TextValue = info.MapName;
		FString name = info.LookupLevelName();
		p.Text = name.IsEmpty() || name.CompareNoCase(info.MapName) == 0 ? info.MapName : FStringf("%s - %s", info.MapName.GetChars(), name.GetChars());
		if (info.MapName.CompareNoCase(*vr_net_map) == 0) chosenThere = true;
	}
	if (!chosenThere && vals->mValues.Size() > 0) vr_net_map = vals->mValues[0].TextValue.GetChars();
	FOptionValues** old = OptionValues.CheckKey("DoomVRNetMaps");
	if (old != nullptr && *old != nullptr) delete *old;
	OptionValues["DoomVRNetMaps"] = vals;
}

// Co-op saves live in their own folder (Saved Games\UZDoom\NetGame\<game>);
// everyone in the game saves the same file there when the host saves.
static FString NetSavesFolder()
{
	const bool was = netgame;
	netgame = true;
	FString folder = G_GetSavegamesFolder();
	netgame = was;
	return folder;
}

// Host a Game's "Carry on from": a new game, or a co-op save of this game
// whose files are all loaded, newest first.
static void BuildSaveChoices()
{
	struct Choice { FString file, text, when; };
	std::vector<Choice> found;
	FileSys::FileList list;
	if (FileSys::ScanDirectory(list, NetSavesFolder().GetChars(), "*." SAVEGAME_EXT, true))
	{
		for (auto& entry : list)
		{
			std::unique_ptr<FResourceFile> save(FResourceFile::OpenResourceFile(entry.FilePath.c_str(), true));
			if (save == nullptr) continue;
			const auto info = save->FindEntry("info.json");
			if (info < 0) continue;
			auto data = save->Read(info);
			FSerializer arc;
			if (!arc.OpenReader(data.string(), data.size())) continue;
			int version = 0;
			arc("Save Version", version);
			const FString engine = arc.GetString("Engine");
			const FString iwad = arc.GetString("Game WAD");
			if (engine.CompareNoCase(GAMESIG) != 0 || version > SAVEVER || version < MINSAVEVER) continue;
			if (iwad.CompareNoCase(fileSystem.GetResourceFileName(fileSystem.GetIwadNum())) != 0) continue;
			if (!G_CheckSaveGameWads(arc, false)) continue;
			const FString title = arc.GetString("Title");
			const FString when = arc.GetString("Creation Time");
			found.push_back({ FString(entry.FileName.c_str()), title.IsEmpty() ? FString(entry.FileName.c_str()) : title, when });
		}
	}
	std::sort(found.begin(), found.end(), [](const Choice& a, const Choice& b) { return a.when.Compare(b.when) > 0; });
	FOptionValues* vals = new FOptionValues;
	auto add = [&](const char* value, const char* text)
	{
		FOptionValues::Pair& p = vals->mValues[vals->mValues.Reserve(1)];
		p.Value = 0;
		p.TextValue = value;
		p.Text = text;
	};
	// Not an empty value: an option row whose first value is empty takes all
	// its values as numbers, every save was 0 like "A new game", and the row
	// never left "A new game".
	add("new", "A new game");
	bool chosenThere = strlen(*vr_net_load) == 0 || stricmp(*vr_net_load, "new") == 0;
	for (auto& c : found)
	{
		add(c.file.GetChars(), c.when.IsEmpty() ? c.text.GetChars() : FStringf("%s (%s)", c.text.GetChars(), c.when.GetChars()).GetChars());
		if (c.file.CompareNoCase(*vr_net_load) == 0) chosenThere = true;
	}
	if (!chosenThere || strlen(*vr_net_load) == 0) vr_net_load = "new";
	FOptionValues** old = OptionValues.CheckKey("DoomVRNetSaves");
	if (old != nullptr && *old != nullptr) delete *old;
	OptionValues["DoomVRNetSaves"] = vals;
}

// Multiplayer > Host a Game: the co-op saves read fresh each time.
CCMD(vr_net_hostmenu)
{
	BuildSaveChoices();
	M_SetMenu("DoomVRHost");
}

// Join a Game: the Game Or Die server first (once live), then the addresses
// joined before; then the menu's own rows (type an address, join it).
static unsigned gJoinRows = 0;	// rows put in at the top last time
static void BuildJoinMenu()
{
	DMenuDescriptor** menu = MenuDescriptors.CheckKey("DoomVRJoin");
	if (menu == nullptr || *menu == nullptr || !(*menu)->IsKindOf(RUNTIME_CLASS(DOptionMenuDescriptor))) return;
	auto desc = static_cast<DOptionMenuDescriptor*>(*menu);
	for (unsigned i = 0; i < gJoinRows && desc->mItems.Size() > 0; ++i) desc->mItems.Delete(0);
	TArray<DMenuItemBase*> rows;
	if (kGameOrDieServer[0] != 0)
	{
		rows.Push(CreateOptionMenuItemCommand(kGameOrDieServerName, FStringf("vr_net_join \"%s\"", kGameOrDieServer).GetChars(), true));
		rows.Push(CreateOptionMenuItemStaticText(" "));
	}
	const auto saved = SavedAddresses();
	if (!saved.empty())
	{
		rows.Push(CreateOptionMenuItemStaticText("Joined before", 1));
		for (auto& a : saved)
			rows.Push(CreateOptionMenuItemCommand(a.c_str(), FStringf("vr_net_join \"%s\"", a.c_str()).GetChars(), true));
		rows.Push(CreateOptionMenuItemSubmenu("Remove an address from this list", "DoomVRJoinForget", 0));
		rows.Push(CreateOptionMenuItemStaticText(" "));
	}
	for (unsigned i = 0; i < rows.Size(); ++i)
	{
		desc->mItems.Insert(i, rows[i]);
		GC::WriteBarrier(desc, rows[i]);
	}
	gJoinRows = rows.Size();

	DMenuDescriptor** forget = MenuDescriptors.CheckKey("DoomVRJoinForget");
	if (forget != nullptr && *forget != nullptr && (*forget)->IsKindOf(RUNTIME_CLASS(DOptionMenuDescriptor)))
	{
		auto fd = static_cast<DOptionMenuDescriptor*>(*forget);
		fd->mItems.Clear();
		fd->mItems.Push(CreateOptionMenuItemStaticText("Pick one to remove it", 1));
		for (auto& a : saved)
		{
			auto it = CreateOptionMenuItemCommand(a.c_str(), FStringf("vr_net_forget \"%s\"", a.c_str()).GetChars(), true);
			fd->mItems.Push(it);
			GC::WriteBarrier(fd, it);
		}
		fd->mSelectedItem = fd->mItems.Size() > 1 ? 1 : -1;
	}
	desc->mSelectedItem = -1;
	for (unsigned i = 0; i < desc->mItems.Size(); ++i)
		if (desc->mItems[i]->mAction != NAME_None) { desc->mSelectedItem = int(i); break; }
}

// From DoomVR_BuildSwitchGameMenu, once every menu is read.
void DoomVR_BuildMultiplayerMenus()
{
	BuildMapChoices();
	BuildJoinMenu();
}

CCMD(vr_net_host)
{
	if (netgame) { Printf("vr_net_host: already in a network game\n"); return; }
	const int players = std::clamp<int>(vr_net_players, 2, MAXPLAYERS);
	TArray<FString> args = ArgsWithoutNet();
	args.Push("-host");
	args.Push(FStringf("%d", players));
	// Carrying on from a co-op save: the engine sends its name to everyone
	// who joins, and each loads their own copy of it.
	FString load = *vr_net_load;
	if (load.CompareNoCase("new") == 0) load = "";
	if (load.IsNotEmpty() && !FileExists(NetSavesFolder() + load))
	{
		vr_net_load = "new";
		load = "";
	}
	if (load.IsNotEmpty())
	{
		args.Push("-loadgame");
		args.Push(load);
		RestartWith(args, FStringf("hosting co-op for %d players from the save %s", players, load.GetChars()).GetChars());
		return;
	}
	RestartWith(args, FStringf("hosting co-op for %d players from %s", players, *vr_net_map).GetChars());
}

// The address being joined; it goes on the list joined before only once the
// host has answered, so a typo never fills the list.
static FString gJoinPending;
static void RememberAddress(const FString& address)
{
	if (address.IsEmpty() || (kGameOrDieServer[0] != 0 && address.CompareNoCase(kGameOrDieServer) == 0)) return;
	auto saved = SavedAddresses();
	for (size_t i = 0; i < saved.size(); ++i)
		if (stricmp(saved[i].c_str(), address.GetChars()) == 0) { saved.erase(saved.begin() + i); break; }
	saved.insert(saved.begin(), address.GetChars());
	if (saved.size() > kMaxSaved) saved.resize(kMaxSaved);
	SetSavedAddresses(saved);
}

CCMD(vr_net_join)
{
	if (netgame) { Printf("vr_net_join: already in a network game\n"); return; }
	FString address = argv.argc() > 1 ? FString(argv[1]) : FString(*vr_net_address);
	address.StripLeftRight();
	if (address.IsEmpty()) { M_StartMessage("Type the host's address first.", 1); return; }
	gJoinPending = address;
	TArray<FString> args = ArgsWithoutNet();
	args.Push("-join");
	args.Push(address);
	RestartWith(args, FStringf("joining %s", address.GetChars()).GetChars());
}

// Back to playing alone, from a network game.
CCMD(vr_net_leave)
{
	if (!netgame) return;
	TArray<FString> args = ArgsWithoutNet();
	RestartWith(args, "leaving the network game");
}

CCMD(vr_net_forget)
{
	if (argv.argc() < 2) return;
	auto saved = SavedAddresses();
	for (size_t i = 0; i < saved.size(); ++i)
		if (stricmp(saved[i].c_str(), argv[1]) == 0) { saved.erase(saved.begin() + i); break; }
	SetSavedAddresses(saved);
	if (CurrentMenu != nullptr) CurrentMenu->Close();
	BuildJoinMenu();
}

// Why the last lobby ended, once the game is up again.
CCMD(vr_net_notice)
{
	if (gLobbyNotice.IsEmpty()) return;
	Printf("DoomVR: multiplayer - told the player: %s\n", gLobbyNotice.GetChars());
	M_StartMessage(gLobbyNotice.GetChars(), 1);
	gLobbyNotice = "";
}

/*
	The lobby. Each step of it is followed by one game frame: the controller's
	or keyboard's back button (B, Escape, Backspace) leaves it, and the host's
	A, Enter or trigger starts at once with the players already in.
*/
static bool gLobbyUp = false;
bool DoomVR_NetLobbyUp() { return gLobbyUp; }
static std::chrono::steady_clock::time_point gLobbySince;
static bool DoomVR_NetLobbyFrame()
{
	I_StartTic();
	bool back = false;
	while (eventtail != eventhead)
	{
		const event_t ev = events[eventtail];
		eventtail = (eventtail + 1) & (MAXEVENTS - 1);
		if (ev.type != EV_KeyDown) continue;
		switch (ev.data1)
		{
		// The headset's controllers send their own keys (KEY_VR_*, and the
		// Steam Frame's KEY_VRF_*), not a gamepad's: without these, B did
		// nothing in the headset and the lobby could not be left.
		// The menu button leaves too, as it would back out of any menu.
		case KEY_ESCAPE: case KEY_BACKSPACE: case KEY_PAD_B: case KEY_JOY2:
		case KEY_VR_B: case KEY_VR_MENU: case KEY_VRF_B: case KEY_VRF_MENU: case KEY_VRF_VIEW:
			back = true;
			break;
		case KEY_ENTER: case KEY_SPACE: case KEY_PAD_A: case KEY_JOY1: case KEY_MOUSE1:
		case KEY_VR_A: case KEY_VRF_A:
			I_GameLobbyStartNow();
			break;
		default:
			break;
		}
	}
	if (back) { Printf("NetLobby:: left by the player\n"); return false; }
	D_Display();
	if (!DoomVR_Active()) std::this_thread::sleep_for(std::chrono::milliseconds(10));	// the headset paces its own frames
	return true;
}

/*
	The lobby's backdrop: a slow starfield coming toward the player, for some
	depth, behind a dim, soft Game Or Die skull (graphics/vrlobby.png in
	uzdoom.pk3, blurred when it was made). Only the 2D drawer and a texture:
	nothing here may touch game objects (the lobby runs between games).
*/
static void DrawLobbyBackdrop(int w, int h)
{
	constexpr int kStars = 320;
	struct Star { float x, y, z; };
	static Star stars[kStars];
	static bool seeded = false;
	static uint32_t rng = 0x9E3779B9u;
	auto rnd = [] { rng = rng * 1664525u + 1013904223u; return float(rng >> 8) / float(1u << 24); };
	auto place = [&](Star& s, float z) { s.x = rnd() * 2.f - 1.f; s.y = rnd() * 2.f - 1.f; s.z = z; };
	static std::chrono::steady_clock::time_point last;
	const auto now = std::chrono::steady_clock::now();
	if (!seeded)
	{
		for (auto& s : stars) place(s, 0.05f + rnd() * 0.95f);
		seeded = true;
		last = now;
	}
	const float dt = std::min(0.1f, std::chrono::duration<float>(now - last).count());
	last = now;

	// In the headset the stars are real, all round the player, drawn into the
	// eyes (DoomVR_LobbyStars); this screen is only the skull and the words,
	// on a panel the stars show through.
	const bool headset = DoomVR_Active();
	if (!headset) Dim(twod, 0, 1.f, 0, 0, w, h);
	const float cx = w * 0.5f, cy = h * 0.5f, spread = std::max(w, h) * 0.5f;
	const int unit = std::max(1, h / 400);
	for (auto& s : stars)
	{
		if (headset) break;
		s.z -= dt * 0.06f;	// about 15 s from the far end to the eye: calm, not warp speed
		if (s.z <= 0.04f) place(s, 1.f);
		const float sx = cx + s.x / s.z * spread * 0.6f, sy = cy + s.y / s.z * spread * 0.6f;
		if (sx < 0 || sy < 0 || sx >= w || sy >= h) { place(s, 1.f); continue; }
		const float near = 1.f - s.z;	// 0 far .. 1 near
		const int size = unit * (near > 0.75f ? 3 : near > 0.4f ? 2 : 1);
		const float bright = 0.3f + 0.7f * near;
		const uint8_t c = uint8_t(235 * bright);
		Dim(twod, PalEntry(255, c, c, uint8_t(c + 20)), 1.f, int(sx), int(sy), size, size);
	}

	static FTextureID skull;
	static bool looked = false;
	if (!looked)
	{
		skull = TexMan.CheckForTexture("graphics/vrlobby.png", ETextureType::Any, FTextureManager::TEXMAN_TryAny | FTextureManager::TEXMAN_ForceLookup);
		looked = true;
	}
	if (skull.isValid())
	{
		FGameTexture* tex = TexMan.GetGameTexture(skull);
		const double th = h * 0.9, tw = th * tex->GetDisplayWidth() / tex->GetDisplayHeight();
		DrawTexture(twod, tex, (w - tw) / 2, (h - th) / 2, DTA_DestWidthF, tw, DTA_DestHeightF, th,
			DTA_Alpha, headset ? 0.3 : 0.24, DTA_TopOffset, 0, DTA_LeftOffset, 0, TAG_DONE);
	}
}

// Drawn by D_Display while the lobby is up (gamestate GS_FULLCONSOLE).
bool DoomVR_DrawNetLobby()
{
	FString status;
	bool host = false;
	int current = 0, limit = 0;
	TArray<FString> names;
	TArray<bool> ready;
	if (!gLobbyUp || !I_GetGameLobbyView(status, host, current, limit, names, ready)) return false;

	const int w = twod->GetWidth(), h = twod->GetHeight();
	DrawLobbyBackdrop(w, h);
	const int scale = std::max(1, std::min(w / 320, h / 200));
	int y = h / 6;
	auto line = [&](FFont* font, int color, const char* text, int gap = 0)
	{
		// A line too wide for the screen at the common scale is drawn smaller,
		// so every line fits with a margin at each side.
		const int sw = std::max(1, font->StringWidth(text));
		const double s = std::min(double(scale), w * 0.9 / sw);
		const int tw = int(sw * s);
		DrawText(twod, font, color, (w - tw) / 2, y, text, DTA_ScaleX, s, DTA_ScaleY, s, TAG_DONE);
		y += (font->GetHeight() + gap) * scale;
	};
	line(BigFont, CR_RED, host ? "Hosting a Co-op Game" : "Joining a Game", 8);
	if (status.IsNotEmpty()) line(NewSmallFont, CR_WHITE, status.GetChars(), 6);
	if (limit > 0) line(NewSmallFont, CR_GOLD, FStringf("Players: %d of %d", current, limit).GetChars(), 4);
	for (unsigned i = 0; i < names.Size(); ++i)
		line(NewSmallFont, ready[i] ? CR_GREEN : CR_GRAY, FStringf("%s%s", names[i].GetChars(), ready[i] ? "" : "  (connecting)").GetChars());
	y += 8 * scale;
	if (host)
	{
		const FString mine = I_LocalAddresses();
		if (mine.IsNotEmpty()) line(NewSmallFont, CR_WHITE, FStringf("Friends join at %s", mine.GetChars()).GetChars());
		line(NewSmallFont, CR_GRAY, "Over the internet: open port 5029 (UDP) on your router", 8);
		line(NewSmallFont, CR_GOLD, "A: start now with the players here     B or Menu: cancel");
	}
	else
	{
		// A host that never answers leaves UZDoom asking again and again;
		// after a while, say what to check.
		if (status.IndexOf("Contacting") >= 0 && std::chrono::steady_clock::now() - gLobbySince > std::chrono::seconds(10))
		{
			line(NewSmallFont, CR_WHITE, "No answer yet. Check the address, that the host is in", 0);
			line(NewSmallFont, CR_WHITE, "their lobby, and that their port 5029 (UDP) is open.", 8);
		}
		line(NewSmallFont, CR_GOLD, "B or Menu: cancel");
	}
	return true;
}

// From D_DoomMain, on a restart that carries -host or -join (the Multiplayer
// menu's): the lobby, in the game. True when the network game is set up; false
// when the player left it or it failed - the game then carries on alone.
bool DoomVR_RunNetLobby()
{
	// The lobby comes up on a restart, before any level: the last game's
	// actors are gone, but the players can still point at the old camera.
	// A screen resize in the lobby (the Quest's first frame) reloads the
	// crosshair, which reads that camera; clear it so nothing reads freed memory.
	for (auto& p : players) p.camera = nullptr;
	gLobbyUp = true;
	gLobbySince = std::chrono::steady_clock::now();
	const gamestate_t was = gamestate;
	gamestate = GS_FULLCONSOLE;
	I_SetGameLobby(DoomVR_NetLobbyFrame);
	bool ok = false;
	try { ok = D_CheckNetGame(); }
	catch (...) { gLobbyUp = false; I_SetGameLobby(nullptr); gamestate = was; throw; }
	const FString error = I_GameLobbyError();
	I_SetGameLobby(nullptr);
	gLobbyUp = false;
	gamestate = was;
	const FString joined = gJoinPending;
	gJoinPending = "";
	if (ok)
	{
		RememberAddress(joined);	// to the top of the list joined before
		return true;
	}

	// Alone again: the network closed, and no network arguments left for the
	// next restart (Switch Game, the style) to carry.
	Net_ClearBuffers();
	TArray<FString> args = ArgsWithoutNet();
	*Args = FArgs(int(args.Size()), args.Data());
	// The player who backed out with B needs no message; a failure says why.
	if (error.IsNotEmpty())
	{
		// The engine's file checks name the files; say what to do about them.
		FString why = error;
		if (strncmp(error.GetChars(), "MISSINGSAVE ", 12) == 0)
			why = FStringf("The host is carrying on from a saved game\n"
				"(%s) that isn't on this machine.\n"
				"Only players who were in that game have it.", error.GetChars() + 12);
		static const char* const fileErrors[] = { "Host found unknown files:", "Wrong file order. Expected:", "Host was expecting missing files:" };
		for (const char* head : fileErrors)
		{
			if (strncmp(error.GetChars(), head, strlen(head)) == 0)
			{
				why = FStringf("Your game files don't match the host's:%s\n"
					"Both players need the same copy of the game\n"
					"and the same mods, loaded in the same order.", error.GetChars() + strlen(head));
				break;
			}
		}
		gLobbyNotice = FStringf("Couldn't start the network game:\n%s", why.GetChars());
		AddCommandString("wait 10; vr_net_notice");
	}
	return false;
}

// The host's first map, sent to everyone in the lobby.
void DoomVR_NetStartMap()
{
	if (Args->CheckParm(FArg_host) && strlen(*vr_net_map) > 0 && P_CheckMapData(*vr_net_map))
		startmap = *vr_net_map;
}
