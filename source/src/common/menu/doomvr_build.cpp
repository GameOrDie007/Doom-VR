/*
** doomvr_build.cpp
**
** Build Your Own Game: one of the games you own, plus maps, a gameplay mod,
** looks, sounds and music - from what setup found installed, with what does
** not fit your picks grayed and the reason on screen.
**
** Written for Doom VR. Our own design: setup writes the
** catalogue (launchers\_build.txt), saying what each piece needs, what it
** clashes with and what it is worth a warning next to - measured from the
** files where it can be (a megawad's own monsters, textures and music are
** read from its lumps). This page applies those rules and writes a launcher
** for the result.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <algorithm>
#include <set>
#include <string>
#include <vector>
#include "menu.h"
#include "c_dispatch.h"
#include "c_cvars.h"
#include "cmdlib.h"
#include "printf.h"
#include "vm.h"
#include "types.h"

// The picks, saved between sessions: the ids of the chosen pieces.
CVAR(String, vr_build, "", CVAR_ARCHIVE)
// For the page (zscript/doomvr/build.zs): the base game's label, for its
// cover.
CVAR(String, vr_build_cover, "", CVAR_NOSAVE)
EXTERN_CVAR(String, vr_current_game)

namespace
{

/*
	One line of launchers\_build.txt per piece, tab-separated:

		kind  id  label  has  needs  clashes  warns  args

	kind      base, maps, gameplay, looks, sounds or music
	has       tags this piece brings (a base: doom, doom2, heretic, kex...;
	          a megawad: ownmonsters, owntextures, ownmusic)
	needs     tags the rest of the build must bring; "a|b" is either
	clashes   tags it cannot share a game with
	warns     tags it works with, but not perfectly
	args      the command line it adds (a base: -iwad and everything its own
	          launcher has; a piece: its -file lines, in load order)

	Tag lists are space-separated, "-" for none. "template" lines are the
	launcher, with {NAME}, {GROUP} and {ARGS} to fill in.
*/
struct Piece
{
	std::string kind, id, label, args;
	std::vector<std::string> has, needs, clashes, warns;
};

std::vector<Piece> gPieces;
std::vector<std::string> gTemplate;
std::set<std::string> gChosen;

const char* const kKinds[] = { "base", "maps", "gameplay", "looks", "sounds", "music" };
const char* const kHeads[] = {
	"Game", "Maps", "Gameplay mod (one)", "Looks (any that fit)", "Sounds", "Music",
};
// Pick one (base, maps, gameplay, music) or any number (looks, sounds). A
// base is always chosen; the others may be left at "none".
bool Single(const std::string& kind) { return kind != "looks" && kind != "sounds"; }

std::vector<std::string> Split(const std::string& s, char sep)
{
	std::vector<std::string> out;
	size_t a = 0;
	while (a <= s.size())
	{
		size_t b = s.find(sep, a);
		if (b == std::string::npos) b = s.size();
		out.push_back(s.substr(a, b - a));
		a = b + 1;
	}
	return out;
}

std::vector<std::string> Tags(const std::string& s)
{
	std::vector<std::string> out;
	for (auto& t : Split(s, ' ')) if (!t.empty() && t != "-") out.push_back(t);
	return out;
}

FString CataloguePath() { return progdir + "launchers/_build.txt"; }

bool Load()
{
	gPieces.clear();
	gTemplate.clear();
	FILE* f = fopen(CataloguePath().GetChars(), "rb");
	if (f == nullptr) return false;
	std::string text;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
	fclose(f);
	for (auto line : Split(text, '\n'))
	{
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty() || line[0] == '#') continue;
		auto c = Split(line, '\t');
		if (c[0] == "template") { gTemplate.push_back(c.size() > 1 ? c[1] : ""); continue; }
		if (c.size() < 8) continue;
		Piece p;
		p.kind = c[0]; p.id = c[1]; p.label = c[2];
		p.has = Tags(c[3]); p.needs = Tags(c[4]); p.clashes = Tags(c[5]); p.warns = Tags(c[6]);
		p.args = c[7];
		gPieces.push_back(p);
	}
	return !gPieces.empty() && !gTemplate.empty();
}

const Piece* Find(const std::string& id)
{
	for (auto& p : gPieces) if (p.id == id) return &p;
	return nullptr;
}

bool Has(const Piece& p, const std::string& tag)
{
	return std::find(p.has.begin(), p.has.end(), tag) != p.has.end() || p.id == tag;
}

// The words for a tag, where a player reads it.
const char* NeedWords(const std::string& tag)
{
	if (tag == "doom") return "a Doom game";
	if (tag == "doom1") return "Doom (the first game)";
	if (tag == "doom2") return "Doom II";
	if (tag == "kexdoom2") return "Doom II (2024 re-release)";
	if (tag == "heretic") return "Heretic";
	if (tag == "hexen") return "Hexen";
	if (tag == "kexheretic") return "a 2025 Heretic";
	if (tag == "kexhexen") return "a 2025 Hexen";
	if (tag == "plainmaps") return "a game without extra maps of its own";
	if (tag == "nashgore") return "NashGore";
	return tag.c_str();
}

const char* HasWords(const std::string& tag)
{
	if (tag == "gameplay") return "brings its own monsters and weapons";
	if (tag == "ownmonsters") return "redraws the monsters";
	if (tag == "somemonsters") return "redraws a monster or two";
	if (tag == "ownactors") return "changes how monsters behave";
	if (tag == "owntextures") return "has textures of its own";
	if (tag == "ownsounds") return "has sounds of its own";
	if (tag == "ownmusic") return "has music of its own";
	if (tag == "hasmaps") return "has maps of its own";
	return "does not fit";
}

// The picks, as they would be with p added: a single-choice kind gives up
// its old pick.
std::vector<const Piece*> Others(const Piece& p)
{
	std::vector<const Piece*> out;
	for (auto& id : gChosen)
	{
		const Piece* q = Find(id);
		if (q == nullptr || q == &p) continue;
		if (Single(p.kind) && q->kind == p.kind) continue;
		out.push_back(q);
	}
	return out;
}

// An unmet need, as the reason, or empty.
std::string Unmet(const Piece& p, const std::vector<const Piece*>& others)
{
	for (auto& need : p.needs)
	{
		bool ok = false;
		for (auto& alt : Split(need, '|'))
			for (auto q : others) if (Has(*q, alt)) ok = true;
		if (!ok) return std::string("Needs ") + NeedWords(Split(need, '|')[0]);
	}
	return "";
}

// Whether p and q cannot share a game, and why - said of q.
bool Clash(const Piece& p, const Piece& q, std::string& why)
{
	for (auto& t : p.clashes) if (Has(q, t)) { why = std::string("it ") + HasWords(t); return true; }
	for (auto& t : q.clashes) if (Has(p, t)) { why = std::string("it does not work with anything that ") + HasWords(t); return true; }
	return false;
}

bool Warn(const Piece& p, const Piece& q, std::string& why)
{
	for (auto& t : p.warns) if (Has(q, t)) { why = "Works - but " + q.label + " " + HasWords(t) + ", and " + p.label + " may change some of it"; return true; }
	for (auto& t : q.warns) if (Has(p, t)) { why = std::string("Works - but this ") + HasWords(t) + ", and " + q.label + " may change some of it"; return true; }
	return false;
}

enum { Free = 0, Chosen = 1, Clashes = 2, Cannot = 3 };

int StateOf(const Piece& p, std::string& why)
{
	why.clear();
	auto others = Others(p);
	std::string unmet = Unmet(p, others);
	if (!unmet.empty()) { why = unmet; return Cannot; }
	// Everything picking it would take off, named; the reason given is the
	// first one's.
	std::vector<std::string> off;
	std::string first;
	for (auto q : others)
	{
		std::string w;
		if (!Clash(p, *q, w)) continue;
		// The game itself is never taken off for a piece.
		if (q->kind == "base") { why = "Not with " + q->label + " - " + w; return Cannot; }
		if (gChosen.count(p.id)) { why = "Does not fit " + q->label + " - " + w; return Chosen; }
		if (off.empty()) first = w;
		off.push_back(q->label);
	}
	if (!off.empty())
	{
		why = "Picking this takes off ";
		for (size_t i = 0; i < off.size(); ++i)
			why += (i == 0 ? "" : i + 1 == off.size() ? " and " : ", ") + off[i];
		why += off.size() == 1 ? " - " + first : "";
		return Clashes;
	}
	for (auto q : others) if (Warn(p, *q, why)) break;
	return gChosen.count(p.id) ? Chosen : Free;
}

const Piece* ChosenOf(const std::string& kind)
{
	for (auto& id : gChosen) { const Piece* q = Find(id); if (q && q->kind == kind) return q; }
	return nullptr;
}

void Save()
{
	std::string s;
	for (auto& p : gPieces) if (gChosen.count(p.id)) { if (!s.empty()) s += ' '; s += p.id; }
	vr_build = s.c_str();
	const Piece* base = ChosenOf("base");
	vr_build_cover = base ? base->label.c_str() : "";
}

// Take off whatever no longer fits, and say what went.
void Prune(std::string& note)
{
	for (bool again = true; again;)
	{
		again = false;
		for (auto& p : gPieces)
		{
			if (!gChosen.count(p.id) || p.kind == "base") continue;
			std::vector<const Piece*> others;
			for (auto& id : gChosen) { const Piece* q = Find(id); if (q && q != &p) others.push_back(q); }
			std::string why = Unmet(p, others);
			for (auto q : others)
			{
				std::string w;
				if (why.empty() && q->kind == "base" && Clash(p, *q, w)) why = "Does not fit " + q->label;
			}
			if (why.empty()) continue;
			gChosen.erase(p.id);
			why[0] = char(tolower(why[0]));
			note += (note.empty() ? "" : "  ") + p.label + " taken off - it " + why + ".";
			again = true;
		}
	}
}

std::string BuildName()
{
	std::string name;
	const Piece* base = ChosenOf("base");
	if (base) name = base->label;
	for (auto kind : kKinds)
	{
		if (!strcmp(kind, "base")) continue;
		for (auto& p : gPieces) if (p.kind == kind && gChosen.count(p.id)) name += " + " + p.label;
	}
	return name;
}

// Start from the saved picks, or the game being played, or the first game.
void Restore()
{
	gChosen.clear();
	for (auto& id : Tags(*vr_build)) if (Find(id)) gChosen.insert(id);
	if (!ChosenOf("base"))
	{
		for (auto& p : gPieces) if (p.kind == "base" && p.label == *vr_current_game) gChosen.insert(p.id);
		if (!ChosenOf("base")) for (auto& p : gPieces) if (p.kind == "base") { gChosen.insert(p.id); break; }
	}
	std::string note;
	Prune(note);
	Save();
}

DOptionMenuDescriptor* Page()
{
	DMenuDescriptor** menu = MenuDescriptors.CheckKey("DoomVRBuild");
	if (menu == nullptr || *menu == nullptr || !(*menu)->IsKindOf(RUNTIME_CLASS(DOptionMenuDescriptor))) return nullptr;
	return static_cast<DOptionMenuDescriptor*>(*menu);
}

DMenuItemBase* MakePick(const char* label, const FString& cmd)
{
	PClass* c = PClass::FindClass("OptionMenuItemDoomVRPick");
	if (c == nullptr) return CreateOptionMenuItemCommand(label, cmd.GetChars(), true);
	DObject* p = c->CreateNew();
	FString namestr = label;
	FIntCVar* gray = nullptr;
	VMValue params[] = { p, &namestr, FName(cmd.GetChars()).GetIndex(), true, false, gray, 0, FName(NAME_Hide).GetIndex() };
	auto f = dyn_cast<PFunction>(c->FindSymbol("Init", true));
	VMCall(f->Variants[0].Implementation, params, countof(params), nullptr, 0);
	// Trusted, like every row the engine itself makes: the command runs.
	auto unsafe = dyn_cast<PField>(c->FindSymbol("mUnsafe", true));
	unsafe->Type->SetValue(reinterpret_cast<uint8_t*>(p) + unsafe->Offset, 0);
	return (DMenuItemBase*)p;
}

// The rows show the picks: the marker, the color, and - as the engine's own
// tooltip, under the list - why a row is gray or what to know about it. The
// row just picked also says what the pick took off.
void Refresh(const std::string& pickedAction = "", const std::string& note = "")
{
	DOptionMenuDescriptor* desc = Page();
	if (desc == nullptr) return;
	PClass* pick = PClass::FindClass("OptionMenuItemDoomVRPick");
	for (auto item : desc->mItems)
	{
		if (pick == nullptr || !item->IsKindOf(pick)) continue;
		FString action = item->mAction.GetChars();
		std::string label, why;
		int state = Free;
		if (action.IndexOf("vr_build pick ") == 0)
		{
			const Piece* p = Find(action.Mid(14).GetChars());
			if (p == nullptr) continue;
			state = StateOf(*p, why);
			label = std::string(Single(p->kind) ? (state == Chosen ? "(*) " : "( ) ") : (state == Chosen ? "[x] " : "[ ] ")) + p->label;
		}
		else if (action.IndexOf("vr_build none ") == 0)
		{
			std::string kind = action.Mid(14).GetChars();
			const bool none = ChosenOf(kind) == nullptr;
			state = none ? Chosen : Free;
			label = std::string(none ? "(*) " : "( ) ") + (kind == "maps" ? "The game's own maps" : kind == "music" ? "The game's own music" : "None - the game as it was made");
		}
		else if (action.CompareNoCase("vr_build play") == 0)
		{
			label = "Play:  " + BuildName();
			why = "Saved to Switch Game as \"" + BuildName() + "\"";
		}
		else continue;
		item->StringVar("mLabel") = label.c_str();
		item->IntVar("mState") = state;
		if (!note.empty() && pickedAction == action.GetChars()) why = why.empty() ? note : note + "  " + why;
		item->mTooltip = why.c_str();
	}
}

} // namespace

// Called from DoomVR_BuildSwitchGameMenu, after every MENUDEF is parsed.
void DoomVR_BuildBuildMenu()
{
	DOptionMenuDescriptor* desc = Page();
	if (desc == nullptr) return;
	if (!Load())
	{
		desc->mItems.Push(CreateOptionMenuItemStaticText("Nothing to build from yet."));
		desc->mItems.Push(CreateOptionMenuItemStaticText("Run SETUP.bat - it lists the mods and maps you have here."));
		return;
	}
	Restore();
	desc->mItems.Push(CreateOptionMenuItemStaticText("Pick a game, then add to it. Gray means it does not fit your picks;"));
	desc->mItems.Push(CreateOptionMenuItemStaticText("highlight it to see why."));
	int rows = 0;
	for (int k = 0; k < int(countof(kKinds)); ++k)
	{
		const std::string kind = kKinds[k];
		bool any = false;
		for (auto& p : gPieces) if (p.kind == kind) any = true;
		if (!any) continue;
		desc->mItems.Push(CreateOptionMenuItemStaticText(" "));
		desc->mItems.Push(CreateOptionMenuItemStaticText(kHeads[k], 1));
		if (kind != "base" && Single(kind))
			desc->mItems.Push(MakePick("", FString("vr_build none ") + kind.c_str()));
		for (auto& p : gPieces)
		{
			if (p.kind != kind) continue;
			desc->mItems.Push(MakePick(p.label.c_str(), FString("vr_build pick ") + p.id.c_str()));
			++rows;
		}
	}
	desc->mItems.Push(CreateOptionMenuItemStaticText(" "));
	desc->mItems.Push(MakePick("Play", "vr_build play"));
	Refresh();
	Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: Build Your Own Game has %d pieces\n", rows);
}

/*
	vr_build pick <id>   choose a piece (a single-choice kind swaps; looks
	                     and sounds toggle)
	vr_build none <kind> no maps, gameplay mod or music of that kind
	vr_build play        write the launcher and switch to it
	vr_build write       write the launcher only (tests)
*/
CCMD(vr_build)
{
	if (argv.argc() < 2) { Printf("vr_build pick <id> | none <kind> | play\n"); return; }
	if (gPieces.empty() && !Load()) { Printf("vr_build: no launchers/_build.txt - run SETUP.bat\n"); return; }
	std::string note;
	const std::string verb = argv[1];
	if (verb == "pick" && argv.argc() > 2)
	{
		const Piece* p = Find(argv[2]);
		if (p == nullptr) { Printf("vr_build: no piece %s\n", argv[2]); return; }
		std::string why;
		int state = StateOf(*p, why);
		if (state == Cannot) return;
		if (state == Chosen && p->kind != "base")
		{
			// Picking it again takes it off - a single-choice kind goes
			// back to none.
			gChosen.erase(p->id);
		}
		else if (state != Chosen)
		{
			for (auto q : Others(*p))
			{
				std::string w;
				if (q->kind != "base" && Clash(*p, *q, w)) { gChosen.erase(q->id); note += (note.empty() ? "" : "  ") + q->label + " taken off - " + w + "."; }
			}
			if (Single(p->kind)) { const Piece* old = ChosenOf(p->kind); if (old) gChosen.erase(old->id); }
			gChosen.insert(p->id);
		}
		Prune(note);
	}
	else if (verb == "none" && argv.argc() > 2)
	{
		const Piece* old = ChosenOf(argv[2]);
		if (old && old->kind != "base") gChosen.erase(old->id);
		Prune(note);
	}
	else if (verb == "play" || verb == "write")
	{
		const Piece* base = ChosenOf("base");
		if (base == nullptr) return;
		std::string args = base->args;
		for (auto kind : kKinds)
		{
			if (!strcmp(kind, "base")) continue;
			for (auto& p : gPieces) if (p.kind == kind && gChosen.count(p.id)) args += " " + p.args;
		}
		std::string name = BuildName();
		// A file name: no characters Windows refuses, and not too long.
		for (auto& ch : name) if (strchr("<>:\"/\\|?*", ch)) ch = '-';
		if (name.size() > 150) name.resize(150);
		std::string bat;
		for (auto& line : gTemplate)
		{
			std::string l = line;
			for (auto rep : { std::make_pair("{NAME}", name), std::make_pair("{GROUP}", std::string("Your builds")), std::make_pair("{ARGS}", args) })
				for (size_t at; (at = l.find(rep.first)) != std::string::npos;) l.replace(at, strlen(rep.first), rep.second);
			bat += l + "\r\n";
		}
		FString path = progdir + "launchers/" + name.c_str() + ".bat";
		FILE* f = fopen(path.GetChars(), "wb");
		if (f == nullptr) { Printf("vr_build: cannot write %s\n", path.GetChars()); return; }
		fwrite(bat.data(), 1, bat.size(), f);
		fclose(f);
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: built %s\n", path.GetChars());
		FString cmd;
		cmd.Format("vr_switchgame \"%s\"", name.c_str());
		if (verb == "play") AddCommandString(cmd.GetChars());
		return;
	}
	else if (verb == "list")
	{
		for (auto& p : gPieces)
		{
			std::string why;
			int s = StateOf(p, why);
			Printf("%-9s %-24s %s%s%s\n", p.kind.c_str(), p.id.c_str(), s == Chosen ? "CHOSEN " : s == Clashes ? "clashes " : s == Cannot ? "cannot " : "", p.label.c_str(), why.empty() ? "" : ("  - " + why).c_str());
		}
		Printf("build: %s\n", BuildName().c_str());
		return;
	}
	Save();
	// Into the log only: the page shows it, and a notify line would float in
	// front of the headset.
	if (!note.empty()) Printf(PRINT_NONOTIFY, "DoomVR: %s\n", note.c_str());
	std::string action = std::string("vr_build ") + verb;
	for (int i = 2; i < argv.argc(); ++i) action += std::string(" ") + argv[i];
	Refresh(action, note);
}
