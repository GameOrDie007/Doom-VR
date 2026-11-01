/*
** doomvr_switchgame.cpp
**
** Switch Game: every game the install has a launcher for, from inside the
** headset.
**
** Written for Doom VR from UZDoom's own dynamic
** menu interface (M_CreateMenus, CreateOptionMenuItemCommand,
** CreateListMenuItemText as the episode menu uses it).
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string>
#include <vector>
#include "menu.h"
#include "c_dispatch.h"
#include "cmdlib.h"
#include "printf.h"
#include "fs_findfile.h"
#include "fs_filesystem.h"
#include "utf8.h"
#include "c_cvars.h"
#include "v_font.h"
#include "m_argv.h"
#include "gamestate.h"
#include "doomstat.h"
#include "doomvr_mywads.h"

bool DoomVR_ModernFits(const char* file, const std::vector<std::string>& tags);	// doomvr_build.cpp

// The game being played, as Switch Game labels it: each launcher sets it, and
// the page opens on that row (zscript/doomvr/gameselect.zs).
CVAR(String, vr_current_game, "", 0)

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/*
	The list is the .bat launchers in <install>\launchers\, not a scan for
	games: a launcher carries the IWAD, the mods and their load order and any
	switches that game needs, so switching lands in exactly what
	double-clicking that launcher does. The label is the file's name.
*/
static FString LaunchersDir()
{
	return progdir + "launchers/";
}

// The group a launcher says it belongs to - "rem DoomVR-group: Doom II" in
// its first lines, written by setup. A launcher without one is "Other".
static std::string LauncherGroup(const std::string& name)
{
	FString path = LaunchersDir() + name.c_str() + ".bat";
	FILE* f = fopen(path.GetChars(), "r");
	std::string group = "Other";
	if (f == nullptr) return group;
	char line[512];
	for (int n = 0; n < 6 && fgets(line, sizeof(line), f); ++n)
	{
		const char* tag = strstr(line, "DoomVR-group:");
		if (tag == nullptr) continue;
		std::string g = tag + 13;
		while (!g.empty() && (g.back() == '\n' || g.back() == '\r' || g.back() == ' ')) g.pop_back();
		while (!g.empty() && g.front() == ' ') g.erase(g.begin());
		if (!g.empty()) group = g;
		break;
	}
	fclose(f);
	return group;
}

/*
	My WADs: the player's own WADs and pk3s, in <install>\My WADs\. Each file
	is a row of its own in Switch Game and a piece in Build Your Own Game, on
	the game it was made for:
	  - in a folder named after a launcher (My WADs\Heretic\x.wad), that one;
	  - else by its maps: E1M1-style names on Doom, MAP01-style on Doom II
	    (also a pk3's maps\*.wad); a file with no maps (a gameplay mod) on
	    Doom II;
	  - the game's first launcher the install has: the original, then its
	    2024 re-release, then Freedoom.
	It plays as that launcher plus -file <the file>, with its own name as the
	game being played (+vr_current_game) and that game's manual in the Game
	Tome (+vr_book_game). An IWAD is not a WAD to add: it is left out.
*/
CVAR(String, vr_book_game, "", CVAR_NOSAVE)

static FString MyWadsDir() { return progdir + "My WADs/"; }

static bool LauncherExists(const std::string& name)
{
	return !name.empty() && FileExists(LaunchersDir() + name.c_str() + ".bat");
}

// Its maps - "doom1" (ExMy), "doom2" (MAPxx), "" (none) or "iwad" - and what
// it brings of its own (DoomVRMyWad::tags), from its directory alone.
static std::string MyWadScan(const std::string& path, std::vector<std::string>& tags)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (f == nullptr) return "";
	char head[4] = {};
	const bool iwad = fread(head, 1, 4, f) == 4 && memcmp(head, "IWAD", 4) == 0;
	fclose(f);
	if (iwad) return "iwad";
	auto res = FResourceFile::OpenResourceFile(path.c_str(), true);
	if (res == nullptr) return "";
	static const char* const kMonsters[] = {
		"POSS", "SPOS", "CPOS", "TROO", "SARG", "HEAD", "SKUL", "BOSS", "BOS2", "PAIN", "SKEL", "FATT", "BSPI", "CYBR", "SPID", "VILE", "SSWV", "KEEN",
		"IMPX", "KNIG", "WZRD", "MUMM", "BEAS", "CLNK", "SNKE", "MNTR", "SRCR", "SOR2",
		"ETTN", "CENT", "DEMN", "DEM2", "SERP", "WRTH", "BISH", "FDMN", "ICEY", "SORC", "KORX", "DRAG" };
	static const char* const kWeapons[] = { "PISG", "SHTG", "SHT2", "CHGG", "MISG", "PLSG", "BFGG", "SAWG" };
	auto isOne = [](const char* n, const char* const* list, size_t count) {
		for (size_t i = 0; i < count; ++i) if (strnicmp(n, list[i], 4) == 0) return true;
		return false;
	};
	auto tag = [&tags](const char* t) { if (std::find(tags.begin(), tags.end(), t) == tags.end()) tags.push_back(t); };
	std::string kind;
	std::vector<std::string> monsters;
	bool inSprites = false;
	for (uint32_t i = 0; i < res->EntryCountU(); ++i)
	{
		const char* full = res->getName(i);
		if (full == nullptr) continue;
		const char* slash = strrchr(full, '/');
		const char* base = slash ? slash + 1 : full;
		// A sprite: between a WAD's S_START and S_END, or under a pk3's sprites\.
		if (!stricmp(full, "S_START") || !stricmp(full, "SS_START")) { inSprites = true; continue; }
		if (!stricmp(full, "S_END") || !stricmp(full, "SS_END")) { inSprites = false; continue; }
		const bool sprite = (inSprites && slash == nullptr) || strnicmp(full, "sprites/", 8) == 0;
		if (sprite && strlen(base) >= 4)
		{
			if (isOne(base, kMonsters, countof(kMonsters)) && std::none_of(monsters.begin(), monsters.end(), [&](const std::string& m) { return strnicmp(m.c_str(), base, 4) == 0; }))
				monsters.push_back(std::string(base, 4));
			if (isOne(base, kWeapons, countof(kWeapons))) tag("ownweapons");
		}
		if (!strnicmp(full, "DECORATE", 8) || !strnicmp(full, "ZSCRIPT", 7) || !strnicmp(full, "DEHACKED", 8)) tag("ownactors");
		if (!strnicmp(full, "TEXTURE1", 8) || !strnicmp(full, "TEXTURE2", 8) || !strnicmp(full, "TEXTURES", 8) || !stricmp(full, "P_START") || !stricmp(full, "PP_START") || !stricmp(full, "TX_START")
		    || !strnicmp(full, "patches/", 8) || !strnicmp(full, "flats/", 6) || !strnicmp(full, "textures/", 9)) tag("owntextures");
		if ((slash == nullptr && (!strnicmp(full, "D_", 2) || !stricmp(full, "MUSINFO"))) || !strnicmp(full, "music/", 6)) tag("ownmusic");
		if ((slash == nullptr && (!strnicmp(full, "DS", 2) || !stricmp(full, "SNDINFO"))) || !strnicmp(full, "sounds/", 7)) tag("ownsounds");
		// Its maps: a WAD's map lumps, a pk3's maps\<name>.wad.
		if (!kind.empty()) continue;
		const char* n = full;
		if (strnicmp(n, "maps/", 5) == 0) n += 5;
		else if (slash != nullptr) continue;
		if ((n[0] == 'E' || n[0] == 'e') && isdigit((unsigned char)n[1]) && (n[2] == 'M' || n[2] == 'm') && isdigit((unsigned char)n[3]) && (n[4] == 0 || n[4] == '.'))
			kind = "doom1";
		else if (strnicmp(n, "MAP", 3) == 0 && isdigit((unsigned char)n[3]) && isdigit((unsigned char)n[4]) && (n[5] == 0 || n[5] == '.'))
			kind = "doom2";
	}
	delete res;
	if (monsters.size() >= 3) tag("ownmonsters");
	else if (!monsters.empty()) tag("somemonsters");
	if (kind.empty() && std::find(tags.begin(), tags.end(), "ownactors") != tags.end()) tag("gameplay");
	return kind;
}

std::vector<DoomVRMyWad> DoomVR_MyWads()
{
	std::vector<DoomVRMyWad> out;
	std::vector<FileSys::FileListEntry> found;
	if (!FileSys::ScanDirectory(found, MyWadsDir().GetChars(), "*", false)) return out;
	std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return stricmp(a.FilePathRel.c_str(), b.FilePathRel.c_str()) < 0; });
	for (auto& f : found)
	{
		if (f.isDirectory) continue;
		std::string ext = f.FileName.substr(f.FileName.find_last_of('.') == std::string::npos ? f.FileName.size() : f.FileName.find_last_of('.'));
		for (auto& c : ext) c = char(tolower((unsigned char)c));
		if (ext != ".wad" && ext != ".pk3" && ext != ".pk7" && ext != ".zip") continue;
		DoomVRMyWad w;
		w.path = f.FilePath;
		const std::string kind = MyWadScan(w.path, w.tags);
		if (kind == "iwad") { Printf("DoomVR: My WADs: %s is a game's IWAD, not a WAD to add - left out\n", f.FilePathRel.c_str()); continue; }
		w.maps = !kind.empty();
		w.kind = kind == "doom1" ? "doom1" : "doom2";
		// Its folder, when that is a launcher's name.
		std::string rel = f.FilePathRel;
		for (auto& c : rel) if (c == '\\') c = '/';
		const size_t slash = rel.find('/');
		if (slash != std::string::npos && LauncherExists(rel.substr(0, slash))) w.launcher = rel.substr(0, slash);
		else
		{
			static const char* const kDoom1[] = { "Doom", "Doom (2024 re-release)", "Freedoom Phase 1" };
			static const char* const kDoom2[] = { "Doom II", "Doom II (2024 re-release)", "Freedoom Phase 2" };
			for (const char* n : (w.kind == "doom1" ? kDoom1 : kDoom2)) if (LauncherExists(n)) { w.launcher = n; break; }
		}
		if (w.launcher.empty()) { Printf("DoomVR: My WADs: no game here to play %s on - left out\n", f.FilePathRel.c_str()); continue; }
		// Its name, unless a launcher or another of these already has it.
		std::string label = f.FileName.substr(0, f.FileName.size() - ext.size());
		std::string tryLabel = label;
		for (int n = 2; LauncherExists(tryLabel) || std::any_of(out.begin(), out.end(), [&](const DoomVRMyWad& o) { return stricmp(o.label.c_str(), tryLabel.c_str()) == 0; }); ++n)
			tryLabel = label + " (" + std::to_string(n) + ")";
		w.label = tryLabel;
		out.push_back(w);
	}
	return out;
}

static const DoomVRMyWad* FindMyWad(const std::vector<DoomVRMyWad>& all, const char* label)
{
	for (auto& w : all) if (stricmp(w.label.c_str(), label) == 0) return &w;
	return nullptr;
}

static std::vector<std::string> FindLaunchers()
{
	std::vector<FileSys::FileListEntry> found;
	std::vector<std::string> names;
	if (FileSys::ScanDirectory(found, LaunchersDir().GetChars(), "*.bat", true))
	{
		for (auto& f : found)
		{
			if (f.isDirectory) continue;
			std::string n = f.FileName;
			if (n.size() > 4) n.resize(n.size() - 4);
			names.push_back(n);
		}
	}
	std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
		return stricmp(a.c_str(), b.c_str()) < 0;
	});
	return names;
}

/*
	A "Switch Game" row on the main menu itself, just above Quit, where the
	player looks for it - not only under Options. Added here, after every
	MENUDEF is parsed, because a mod that brings its own main menu replaces
	the whole definition and a row written in MENUDEF would vanish with it.

	It is a text row in the menu's own font and colors, like the text rows
	Heretic and Hexen already use; Quit moves down one line to make room. A
	main menu with no Quit row (a mod's own design) is left alone - Switch
	Game is still under Options.

	A mod's menu with no room at the bottom for one more row (Brutal Doom's:
	its rows already reach the bottom in the re-release's taller font, Quit
	half cut off) is drawn smaller, all of it - the logo too - by the same
	amount and still centered, until its last row fits. A menu that had room
	is left exactly as it was.
*/
static void AddToMainMenu(FName menuName)
{
	DMenuDescriptor** menu = MenuDescriptors.CheckKey(menuName);
	if (menu == nullptr || *menu == nullptr || !(*menu)->IsKindOf(RUNTIME_CLASS(DListMenuDescriptor))) return;
	auto ld = static_cast<DListMenuDescriptor*>(*menu);

	const FName ours("DoomVRSwitchGame"), quitMenu("QuitMenu");
	int quit = -1;
	for (unsigned i = 0; i < ld->mItems.Size(); ++i)
	{
		if (ld->mItems[i]->mAction == ours) return; // already there
		if (ld->mItems[i]->mAction == quitMenu) quit = int(i);
	}
	if (quit < 0 || ld->mFont == nullptr) return;

	const double x = ld->mItems[quit]->mXpos, y = ld->mItems[quit]->mYpos;
	const int step = ld->mLinespacing > 0 ? ld->mLinespacing : 16;
	const int height = ld->mVirtHeight > 0 ? ld->mVirtHeight : 200;
	const bool room = y + 2 * step <= height;

	for (unsigned i = unsigned(quit); i < ld->mItems.Size(); ++i)
		if (ld->mItems[i]->mYpos >= y) ld->mItems[i]->mYpos += step;
	DMenuItemBase* it = CreateListMenuItemText(x, y, step, 'w', "Switch Game", ld->mFont,
	                                           ld->mFontColor, ld->mFontColor2, ours, 0);
	ld->mItems.Insert(unsigned(quit), it);
	GC::WriteBarrier(ld, it);
	if (ld->mSelectedItem >= quit) ++ld->mSelectedItem;
	if (room) return;

	// No room: the menu's own size, or the 320x200 of a "clean" one, becomes a
	// taller one of the same shape - its lowest row's foot a little above the
	// bottom - with the rows moved right by half the width it gained.
	const bool clean = ld->mVirtWidth <= 0 || ld->mVirtHeight <= 0;
	const int w0 = clean ? 320 : ld->mVirtWidth, h0 = clean ? 200 : ld->mVirtHeight;
	double foot = 0;
	for (auto item : ld->mItems)
		if (item->mAction != NAME_None && item->mYpos + ld->mFont->GetHeight() > foot) foot = item->mYpos + ld->mFont->GetHeight();
	const int h = int(ceil(foot)) + 4;
	if (h <= h0) return;
	const int w = int(ceil(double(w0) * h / h0));
	const double dx = (w - w0) / 2.;
	for (auto item : ld->mItems) item->mXpos += dx;
	if (ld->mWLeft > 0) ld->mWLeft += int(dx);
	if (ld->mWRight > 0) ld->mWRight += int(dx);
	ld->mVirtWidth = w;
	ld->mVirtHeight = h;
	Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: %s drawn at %d%% to fit Switch Game and Quit\n", menuName.GetChars(), int(100. * h0 / h + 0.5));
}

/*
	The picker Doom VR starts with: the Doom VR shortcut
	passes +vr_open_picker 1, and Switch Game opens as soon as the title is up,
	on Doom, the original, every time. The
	game played last is noted once play has begun in it - a level, not the
	title's demos - for the log.
*/
CVAR(String, vr_last_game, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_open_picker, false, 0)

void DoomVR_SwitchGameTick()
{
	if (gamestate == GS_LEVEL && !demoplayback && strlen(*vr_current_game) > 0 && stricmp(*vr_last_game, *vr_current_game) != 0)
		vr_last_game = *vr_current_game;
	if (vr_open_picker && menuactive == MENU_Off && (gamestate == GS_DEMOSCREEN || gamestate == GS_TITLELEVEL || gamestate == GS_LEVEL))
	{
		vr_open_picker = false;
		M_StartControlPanel(false);
		M_SetMenu("DoomVRSwitchGame");
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: the game picker is up (last played: %s)\n", strlen(*vr_last_game) ? *vr_last_game : "none yet");
	}
}

void DoomVR_BuildBuildMenu();

// Called from M_CreateMenus, after every MENUDEF has been parsed - so a mod
// that brings its own menus cannot leave this page empty.
// The simple Options menu (UZDoom's own, m_simpleoptions) by default: the full
// one is one row away inside it, and the full list overwhelms in a headset.
// Switched on once per ini, so a player who then turns it off keeps it off.
EXTERN_CVAR(Bool, m_simpleoptions)
CVAR(Bool, vr_simple_options_set, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

void DoomVR_BuildSwitchGameMenu()
{
	if (!vr_simple_options_set)
	{
		m_simpleoptions = true;
		vr_simple_options_set = true;
	}
	AddToMainMenu("MainMenu");
	AddToMainMenu("MainMenuTextOnly");
	DoomVR_BuildBuildMenu();

	DMenuDescriptor** menu = MenuDescriptors.CheckKey("DoomVRSwitchGame");
	if (menu == nullptr || *menu == nullptr || !(*menu)->IsKindOf(RUNTIME_CLASS(DOptionMenuDescriptor))) return;
	auto desc = static_cast<DOptionMenuDescriptor*>(*menu);
	// The page's own rows (the style, its restart) go to the bottom, below
	// every game: at the top of the picker a point or a stick nudge changed the
	// style by accident.
	// Moved once every row is in, in place: no allocation while a row is out
	// of the menu's own list, which the garbage collector sees.
	struct StyleRowsLast
	{
		DOptionMenuDescriptor* d; unsigned n;
		~StyleRowsLast()
		{
			if (n == 0) return;
			d->mItems.Push(CreateOptionMenuItemStaticText(" "));
			for (unsigned i = 0; i < n; ++i) d->mItems.Push(d->mItems[i]);
			d->mItems.Delete(0, n);
			FString last;
			for (unsigned i = d->mItems.Size() - n; i < d->mItems.Size(); ++i)
				if (d->mItems[i]->mAction != NAME_None) last.AppendFormat(" %s", d->mItems[i]->mAction.GetChars());
			Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: Switch Game ends with its own rows:%s\n", last.GetChars());
		}
	} styleLast{ desc, desc->mItems.Size() };

	const auto names = FindLaunchers();
	if (names.empty())
	{
		desc->mItems.Push(CreateOptionMenuItemStaticText("No launchers found in the launchers folder."));
		desc->mItems.Push(CreateOptionMenuItemStaticText("Run SETUP.bat to write one per game."));
		return;
	}
	// Build Your Own Game first, then the builds made there.
	if (FileExists(LaunchersDir() + "_build.txt"))
	{
		desc->mItems.Push(CreateOptionMenuItemSubmenu("Build your own game...", "DoomVRBuild", 0));
		desc->mItems.Push(CreateOptionMenuItemStaticText(" "));
	}
	// Grouped by game, in this order; any other group after them, "Other"
	// last. Each group's rows stay in name order.
	static const char* const kOrder[] = {
		"Your builds", "Doom", "Doom II", "Final Doom", "Doom 64 and PlayStation", "Heretic", "Hexen", "Strife",
		"Megawads", "Gameplay mods", "Total conversions", "Free games", "My WADs",
	};
	std::vector<std::pair<std::string, std::string>> rows; // group, name
	for (auto& n : names) rows.emplace_back(LauncherGroup(n), n);
	// The player's own (My WADs, above); the folder made the first time, so
	// there is one to find.
	if (!DirExists(MyWadsDir().GetChars()))
	{
		CreatePath(MyWadsDir().GetChars());
		if (FILE* f = fopen((MyWadsDir() + "Put your WADs here.txt").GetChars(), "wb"))
		{
			fputs("Put your own WAD and pk3 files in this folder. Each one shows up in Doom VR's game picker\r\n"
			      "under My WADs, and in Build Your Own Game.\r\n\r\n"
			      "Doom VR plays a WAD with E1M1-style maps on Doom, and anything else on Doom II.\r\n"
			      "For another game, put the file in a folder named after that game in the picker,\r\n"
			      "for example \"My WADs\\Heretic\" or \"My WADs\\Final Doom - Plutonia\".\r\n", f);
			fclose(f);
		}
	}
	const auto mine = DoomVR_MyWads();
	for (auto& w : mine) rows.emplace_back("My WADs", w.label);
	auto rank = [](const std::string& g) {
		for (int i = 0; i < int(countof(kOrder)); ++i) if (g == kOrder[i]) return i;
		return g == "Other" ? 1000 : 500;
	};
	// The group's own game first in its group (the picker opens on
	// Doom): a row the group is named for, or that carries
	// the group's name - "Doom" and "Doom - SIGIL" before "Brutal Doom".
	auto own = [](const std::pair<std::string, std::string>& r) {
		return strnicmp(r.second.c_str(), r.first.c_str(), r.first.size()) == 0 || strnicmp(r.first.c_str(), r.second.c_str(), r.second.size()) == 0;
	};
	std::stable_sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) {
		const int ra = rank(a.first), rb = rank(b.first);
		if (ra != rb) return ra < rb;
		if (ra == 500) { const int c = stricmp(a.first.c_str(), b.first.c_str()); if (c != 0) return c < 0; }
		return own(a) && !own(b);
	});
	std::string current, first;
	int groups = 0;
	for (auto& r : rows)
	{
		if (first.empty()) first = r.second;
		if (r.first != current)
		{
			if (!current.empty()) desc->mItems.Push(CreateOptionMenuItemStaticText(" "));
			desc->mItems.Push(CreateOptionMenuItemStaticText(r.first.c_str(), 1));
			current = r.first;
			++groups;
		}
		FString cmd;
		cmd.Format("vr_switchgame \"%s\"", r.second.c_str());
		desc->mItems.Push(CreateOptionMenuItemCommand(r.second.c_str(), cmd.GetChars(), true));
	}
	// None yet: where to put them, at the end of the list.
	if (mine.empty())
	{
		desc->mItems.Push(CreateOptionMenuItemStaticText(" "));
		desc->mItems.Push(CreateOptionMenuItemStaticText("My WADs", 1));
		desc->mItems.Push(CreateOptionMenuItemStaticText("Put your own WADs in the My WADs folder"));
		desc->mItems.Push(CreateOptionMenuItemStaticText("(in Doom VR's game folder) to see them here."));
	}
	Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: Switch Game lists %u launchers in %d groups, %s first; My WADs: %u\n", unsigned(names.size()), groups, first.c_str(), unsigned(mine.size()));
}

// The game being played again, from its launcher: after changing the style,
// which is read only when a game starts.
CCMD(vr_restartgame)
{
	if (strlen(*vr_current_game) == 0) { Printf("vr_restartgame: this game was not started from a launcher\n"); return; }
	FString cmd;
	cmd.Format("vr_switchgame \"%s\" again", *vr_current_game);
	AddCommandString(cmd.GetChars());
}

/*
	Start the chosen launcher once this process is gone, then leave.

	- A headset session cannot be rebuilt inside the process that owns the
	  graphics context it was made for, so the game restarts as a new process.
	  The successor waits a few seconds first, so this one has released the
	  session before the next one asks for it.
	- The quit is queued, not run here: this runs from a menu item, and a quit
	  thrown through the script VM's frames has crashed a switch before. The
	  delayed-command queue runs it from the game's own ticker instead.
*/
#ifdef _WIN32
static FString gPredecessorNote;
static double SteadyMs()
{
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
#endif

// Started by Switch Game (DOOMVR_WAIT_PID names the game that started us):
// wait, up to 20 seconds, for it to be gone before this one reaches for the
// headset. Called first thing in startup, before the log is open; what
// happened is printed once it is (DoomVR_ReportPredecessor). The times are
// on the same clock as log-doomvr-shutdown.txt.
void DoomVR_WaitForPredecessor()
{
#ifdef _WIN32
	wchar_t buf[32] = {};
	if (GetEnvironmentVariableW(L"DOOMVR_WAIT_PID", buf, 32) == 0) return;
	SetEnvironmentVariableW(L"DOOMVR_WAIT_PID", nullptr);	// not for our own children
	const DWORD pid = wcstoul(buf, nullptr, 10);
	const double start = SteadyMs();
	HANDLE h = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
	if (h == nullptr)
	{
		gPredecessorNote.Format("DoomVR: the previous game (pid %lu) had already closed (at %.1f ms)\n", pid, start);
		return;
	}
	const DWORD r = WaitForSingleObject(h, 20000);
	CloseHandle(h);
	const double end = SteadyMs();
	if (r == WAIT_OBJECT_0)
		gPredecessorNote.Format("DoomVR: waited %.0f ms for the previous game (pid %lu) to close (%.1f to %.1f ms)\n", end - start, pid, start, end);
	else
		gPredecessorNote.Format("DoomVR: the previous game (pid %lu) was STILL running after %.0f ms - starting anyway (%.1f ms)\n", pid, end - start, end);
#endif
}

void DoomVR_ReportPredecessor()
{
#ifdef _WIN32
	if (gPredecessorNote.IsNotEmpty()) Printf("%s", gPredecessorNote.GetChars());
	gPredecessorNote = "";
#endif
}

// A desk test of Switch Game, never for play: DOOMVR_TEST_CHAIN="A|B|C" (launcher
// names) makes each game, once started, wait about 8 seconds and switch to the
// next, handing the rest of the list on in its environment; the last one
// quits. Inert unless the variable is set.
void DoomVR_TestChainStart()
{
#ifdef _WIN32
	if (GetEnvironmentVariableW(L"DOOMVR_TEST_CHAIN", nullptr, 0) == 0) return;
	Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: TEST chain running (DOOMVR_TEST_CHAIN)\n");
	AddCommandString("wait 300; vr_debug_chain_next");
#endif
}

CCMD(vr_debug_chain_next)
{
#ifdef _WIN32
	wchar_t buf[2048] = {};
	const DWORD n = GetEnvironmentVariableW(L"DOOMVR_TEST_CHAIN", buf, 2048);
	std::wstring rest(buf, n < 2048 ? n : 0);
	if (rest.empty()) { Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: TEST chain done - quitting\n"); AddCommandString("quit"); return; }
	const size_t bar = rest.find(L'|');
	const std::wstring next = rest.substr(0, bar);
	rest = bar == std::wstring::npos ? std::wstring() : rest.substr(bar + 1);
	// An empty variable is the last game's cue to quit; set it to a single
	// '|' is not needed - a set-but-empty value reads as length 0 as well.
	SetEnvironmentVariableW(L"DOOMVR_TEST_CHAIN", rest.empty() ? L" " : rest.c_str());
	FString name = FString(next.c_str());
	name.StripLeftRight();
	if (name.IsEmpty()) { Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: TEST chain done - quitting\n"); AddCommandString("quit"); return; }
	Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: TEST chain -> %s\n", name.GetChars());
	FString cmd;
	cmd.Format("vr_switchgame \"%s\"", name.GetChars());
	AddCommandString(cmd.GetChars());
#endif
}

/*
	Switching in the same process: leaving the headset for the runtime's
	home between games looks janky. The engine's own
	restart reloads a game's files without closing the window, so the
	graphics context and the headset session stay up: the chosen launcher's
	command line becomes this process's, and the engine restarts into it
	while the headset shows black (DoomVR_RestartFrame) instead of the
	runtime's own screen. The launcher is read, not run: its uzdoom.exe line,
	%~dp0 as the launchers folder, without the -config and +logfile the
	running game already has and without the %* for extra files. Off
	(vr_switch_inprocess 0), the game restarts as a new process as before.
*/
CVAR(Bool, vr_switch_inprocess, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
EXTERN_FARG(config);
EXTERN_FARG(deh);
EXTERN_FARG(bex);
EXTERN_FARG(exec);
EXTERN_FARG(playdemo);
EXTERN_FARG(file);
EXTERN_FARG(optfile);
EXTERN_CVAR(Int, vr_mode)
extern bool wantToRestart;
void DoomVR_ForgetGame();	// doomvr_headset.cpp: the VR side's game-bound state, before the restart

static bool LauncherArgs(const FString& bat, TArray<FString>& out)
{
	FILE* f = fopen(bat.GetChars(), "rb");
	if (!f) return false;
	FString line;
	char buf[4096];
	while (fgets(buf, sizeof(buf), f))
	{
		FString l = buf;
		FString low = l;
		low.ToLower();
		const ptrdiff_t at = low.IndexOf("uzdoom.exe");
		if (at < 0 || low.IndexOf("rem ") == 0) continue;
		line = l.Mid(at + 10);
		if (line.Len() > 0 && line[0] == '"') line = line.Mid(1);	// the end of a quoted path to the exe
		break;
	}
	fclose(f);
	if (line.IsEmpty()) return false;
	FString dir = LaunchersDir();
	dir.Substitute("/", "\\");
	line.Substitute("%~dp0", dir.GetChars());
	line.Substitute("%*", "");
	line.StripRight();
	// Split on spaces outside quotes; the quotes go.
	TArray<FString> words;
	FString cur;
	bool quoted = false, any = false;
	for (const char* c = line.GetChars(); *c; ++c)
	{
		if (*c == '"') { quoted = !quoted; any = true; continue; }
		if (!quoted && (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n'))
		{
			if (any) { words.Push(cur); cur = ""; any = false; }
			continue;
		}
		cur += *c;
		any = true;
	}
	if (any) words.Push(cur);
	for (unsigned i = 0; i < words.Size(); ++i)
	{
		if (words[i].CompareNoCase("-config") == 0 || words[i].CompareNoCase("+logfile") == 0) { ++i; continue; }
		out.Push(words[i]);
	}
	return out.Size() > 0;
}

CCMD(vr_switchgame)
{
	if (argv.argc() < 2) { Printf("vr_switchgame <launcher name>\n"); return; }
	FString bat = LaunchersDir() + argv[1] + ".bat";
	// One of the player's own (My WADs): its game's launcher, and the file.
	std::vector<DoomVRMyWad> mine;
	const DoomVRMyWad* my = nullptr;
	if (!FileExists(bat))
	{
		mine = DoomVR_MyWads();
		my = FindMyWad(mine, argv[1]);
		if (my != nullptr) bat = LaunchersDir() + my->launcher.c_str() + ".bat";
	}
	if (!FileExists(bat)) { Printf("vr_switchgame: no launcher %s\n", bat.GetChars()); return; }
	// The Game Tome's book: the game's own, unless a My WADs file names another.
	vr_book_game = "";

	if (argv.argc() < 3 && stricmp(argv[1], *vr_current_game) == 0 && gamestate != GS_STARTUP)
	{
		// Already this game (Continue on the picker): just back to it.
		M_ClearMenus();
		return;
	}
	vr_open_picker = false;
	TArray<FString> launch;
	if (vr_switch_inprocess && LauncherArgs(bat, launch))
	{
		TArray<FString> args;
		args.Push(Args->GetArg(0));
		if (const char* cfg = Args->CheckValue(FArg_config)) { args.Push("-config"); args.Push(cfg); }
		FString shown;
		FString dropped;
		for (unsigned i = 0; i < launch.Size(); ++i)
		{
			// A My WADs file keeps only the Modern add-ons that fit what it brings.
			if (my != nullptr && launch[i].CompareNoCase("-modern") == 0 && i + 1 < launch.Size() && !DoomVR_ModernFits(launch[i + 1].GetChars(), my->tags))
			{
				dropped.AppendFormat(" %s", ExtractFileBase(launch[i + 1].GetChars(), true).GetChars());
				++i;
				continue;
			}
			args.Push(launch[i]);
			shown.AppendFormat(" %s", launch[i].GetChars());
		}
		if (my != nullptr)
		{
			FString brings;
			for (auto& t : my->tags) brings.AppendFormat(" %s", t.c_str());
			Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: My WADs: %s on %s; it brings:%s; Modern add-ons left out:%s\n",
			       my->label.c_str(), my->launcher.c_str(), brings.IsEmpty() ? " nothing of its own" : brings.GetChars(), dropped.IsEmpty() ? " none" : dropped.GetChars());
			for (const char* a : { "-file", my->path.c_str(), "+vr_current_game", my->label.c_str(), "+vr_book_game", my->launcher.c_str() })
			{
				args.Push(a);
				shown.AppendFormat(" %s", a);
			}
		}
		// Every launcher asks for the headset (+vr_mode 20); a game played on
		// the monitor stays on the monitor.
		if (vr_mode != 20) { args.Push("+vr_mode"); args.Push(FStringf("%d", *vr_mode)); }
		*Args = FArgs(int(args.Size()), args.Data());
		// As D_DoomInit does at the first start: each kind of file argument
		// gathered under one switch. Without it only the first -file counted -
		// Brutal Doom 64 started with its game and without its maps.
		Args->CollectFiles(FArg_deh, ".deh");
		Args->CollectFiles(FArg_bex, ".bex");
		Args->CollectFiles(FArg_exec, ".cfg");
		Args->CollectFiles(FArg_playdemo, ".lmp");
		Args->CollectFiles(FArg_file, nullptr);
		Args->CollectFiles(FArg_optfile, nullptr);
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: switching to %s in this process:%s\n", argv[1], shown.GetChars());
		DoomVR_ForgetGame();
		wantToRestart = true;
		return;
	}

#ifdef _WIN32
	FString winBat = bat;
	winBat.Substitute("/", "\\");
	// Start in the install folder, so a launcher that expects to find the
	// game beside it, or in the folder above, finds it either way.
	FString dir = progdir;
	dir.Substitute("/", "\\");
	FString line;
	// A My WADs file goes on the end, where the launcher's -file %* takes it.
	FString extra;
	if (my != nullptr)
	{
		FString p = my->path.c_str();
		p.Substitute("/", "\\");
		extra.Format(" \"%s\" +vr_current_game \"%s\" +vr_book_game \"%s\"", p.GetChars(), my->label.c_str(), my->launcher.c_str());
	}
	line.Format("cmd.exe /c \"ping -n 4 127.0.0.1 >nul & call \"%s\"%s\"", winBat.GetChars(), extra.GetChars());
	// The successor waits for THIS process to be gone before it starts
	// (DoomVR_WaitForPredecessor): the few seconds above are not always
	// enough, and two processes holding the headset and the graphics card at
	// once froze a whole PC for three and a half minutes (Build Your Own
	// Game on Virtual Desktop).
	SetEnvironmentVariableW(L"DOOMVR_WAIT_PID", std::to_wstring(GetCurrentProcessId()).c_str());
	Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: relaunch line: %s (in %s)\n", line.GetChars(), dir.GetChars());

	std::wstring wline = WideString(line.GetChars());
	std::wstring wdir = WideString(dir.GetChars());
	STARTUPINFOW si = { sizeof(si) };
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW(nullptr, &wline[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
	                    nullptr, wdir.c_str(), &si, &pi))
	{
		Printf("vr_switchgame: could not start %s (error %lu)\n", bat.GetChars(), GetLastError());
		return;
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: switching to %s\n", argv[1]);
	AddCommandString("wait 1; quit");
#else
	Printf("vr_switchgame: not built for this platform\n");
#endif
}
