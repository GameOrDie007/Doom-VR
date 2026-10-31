/*
** doomvr_tune.cpp
**
** The in-headset weapon tuner: each weapon's fit in the hand, set by the
** player while holding it, saved, and shipped as the defaults.
**
** vr_tune on (VR Options > Hands and Weapons, or the console). THE KEYBOARD,
** as a tuner in the headset uses it (NOLF, UT99): End steps to the next mode, the
** arrows and PgUp/PgDn move the weapon one step a press, Home changes the
** step (fine, normal, coarse), Insert puts this weapon back to the measured
** fit, [ and ] change to the previous or next weapon, and Delete switches the
** tuner off to play with the fit - and on again, in the headset, without the
** menu. While tuning those keys go to the tuner only. The right stick works
** too (its click: next mode; the left stick's click: the other hand's weapon,
** a VR mod's off-hand one). The modes:
**
**   MOVE           left/right: sideways   up/down: forward and back   PgUp/PgDn: up and down
**   HEIGHT, SIZE   up/down, PgUp/PgDn: height                          left/right: size
**   ANGLE          left/right: turn       up/down: tilt               PgUp/PgDn: roll
**   ROLL           left/right, PgUp/PgDn: roll
**   HAND MOVE      our hand on the gun: as MOVE (along the gun's own axes)
**   HAND ANGLE     our hand on the gun: as ANGLE, turning about where it holds
**   SUPPORT MOVE   the other hand on the gun's front grip (shown there while
**   SUPPORT ANGLE  tuning, so both hands are free for the keys): as above
**
** The hands' values are per weapon class too, saved as "Class@hand" and
** "Class@support" lines in the same file (scale unused), to put the hand models
** round the guns by hand.
**
** Values are per weapon class, added to the measured placement
** (doomvr_headset.cpp). They are read from doomvrtune.txt in uzdoom.pk3 (the
** shipped defaults - the tuned file, copied in once tuned) and then
** from doomvr-weapons.txt beside the player's ini, which the tuner writes each
** time the stick is let go. Each line: class right up forward pitch yaw roll
** scale (metres, degrees, a factor).
**
** Written for Doom VR. The way it works - a tuner in the
** hand, modes on a click, values saved to a file that ships - is the one
** used to tune No One Lives Forever's guns.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "doomvr_headset.h"
#include "d_player.h"
#include "doomstat.h"
#include "gameconfigfile.h"
#include "m_misc.h"
#include "filesystem.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "printf.h"
#include "sbar.h"
#include "tarray.h"
#include "zstring.h"
#include "cmdlib.h"
#include "d_eventbase.h"
#include "keydef.h"
#include "menustate.h"
#include <stdio.h>
#include <math.h>
#include <algorithm>

CVAR(Bool, vr_tune, false, 0)

namespace
{
	TMap<FName, DoomVR_WeaponTune> gTunes;
	bool gLoaded = false;
	bool gDirty = false;
	int gMode = 0;
	int gHand = 0;
	// How each hand's weapon was last drawn: a sprite's fit is kept apart
	// from the model's, under "Class@sprite".
	bool gDrawnAsSprite[2] = {};
	double gPrintWait = 0;

	FName TuneKey(FName cls, bool sprite)
	{
		return sprite ? FName(FStringf("%s@sprite", cls.GetChars())) : cls;
	}
	bool gWasActive = false;

	const char* const kModes[] = {
		"MOVE - left/right: sideways, up/down: forward and back, PgUp/PgDn: up and down",
		"HEIGHT, SIZE - up/down or PgUp/PgDn: height, left/right: size",
		"ANGLE - left/right: turn, up/down: tilt, PgUp/PgDn: roll",
		"ROLL - left/right or PgUp/PgDn: roll",
		"HAND MOVE - left/right: sideways, up/down: forward and back, PgUp/PgDn: up and down",
		"HAND ANGLE - left/right: turn, up/down: tilt, PgUp/PgDn: roll",
		"SUPPORT HAND MOVE - left/right: sideways, up/down: forward and back, PgUp/PgDn: up and down",
		"SUPPORT HAND ANGLE - left/right: turn, up/down: tilt, PgUp/PgDn: roll",
	};
	constexpr int kNumModes = 8;
	const char* const kTargetName[] = { "the weapon", "the weapon", "the weapon", "the weapon",
	                                    "your hand on it", "your hand on it", "the other hand on its front grip", "the other hand on its front grip" };

	// What the current mode moves: the weapon (its fit, as always), our hand
	// on it, or the other hand on its front grip.
	FName ModeKey(AActor* w)
	{
		const FName cls = w->GetClass()->TypeName;
		if (gMode >= 6) return FName(FStringf("%s@support", cls.GetChars()));
		if (gMode >= 4) return FName(FStringf("%s@hand", cls.GetChars()));
		return TuneKey(cls, gDrawnAsSprite[gHand]);
	}
	DoomVR_WeaponTune ModeTune(AActor* w)
	{
		const DoomVR_WeaponTune* t = gTunes.CheckKey(ModeKey(w));
		return t ? *t : DoomVR_WeaponTune();
	}

	// One key press: metres, degrees, and the size factor, per step (Home).
	int gStep = 1;
	const char* const kStepName[] = { "fine", "normal", "coarse" };
	const float kStepMetres[] = { 0.0025f, 0.01f, 0.05f };
	const float kStepDegrees[] = { 0.5f, 2.f, 10.f };
	const float kStepSize[] = { 1.01f, 1.05f, 1.2f };

	FString UserPath()
	{
		FString path = GameConfig ? GameConfig->GetPathName() : "";
		FixPathSeperator(path);
		const ptrdiff_t slash = path.LastIndexOf('/');
		path = slash >= 0 ? path.Left(slash + 1) : FString();
		return path + "doomvr-weapons.txt";
	}

	void Parse(const char* text)
	{
		for (const char* line = text; line && *line; )
		{
			const char* end = strchr(line, '\n');
			FString one(line, end ? end - line : strlen(line));
			line = end ? end + 1 : nullptr;
			one.StripLeftRight();
			if (one.IsEmpty() || one[0] == '#') continue;
			char name[128];
			DoomVR_WeaponTune t;
			if (sscanf(one.GetChars(), "%127s %f %f %f %f %f %f %f", name, &t.right, &t.up, &t.forward,
			           &t.pitch, &t.yaw, &t.roll, &t.scale) == 8 && t.scale > 0.05f)
				gTunes.Insert(FName(name), t);
		}
	}

	void Load()
	{
		if (gLoaded) return;
		gLoaded = true;
		const int lump = fileSystem.CheckNumForFullName("doomvrtune.txt");
		if (lump >= 0)
		{
			auto data = fileSystem.ReadFile(lump);
			FString text(data.string(), data.size());
			Parse(text.GetChars());
		}
		FILE* f = fopen(UserPath().GetChars(), "rb");
		if (f)
		{
			FString text;
			char buf[4096];
			size_t n;
			while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.AppendCStrPart(buf, n);
			fclose(f);
			Parse(text.GetChars());
		}
	}

	void Save()
	{
		const FString path = UserPath();
		FILE* f = fopen(path.GetChars(), "wb");
		if (!f)
		{
			Printf(PRINT_NONOTIFY, "vr_tune: could not write %s\n", path.GetChars());
			return;
		}
		fprintf(f, "# DoomVR weapon fits, written by the in-headset tuner (vr_tune).\n"
		           "# class right up forward (metres) pitch yaw roll (degrees) scale\n");
		TArray<FName> names;
		TMap<FName, DoomVR_WeaponTune>::Iterator it(gTunes);
		TMap<FName, DoomVR_WeaponTune>::Pair* p;
		while (it.NextPair(p)) names.Push(p->Key);
		std::sort(names.begin(), names.end(), [](const FName& a, const FName& b) { return stricmp(a.GetChars(), b.GetChars()) < 0; });
		for (const FName& n : names)
		{
			const DoomVR_WeaponTune& t = gTunes[n];
			fprintf(f, "%s %.4f %.4f %.4f %.2f %.2f %.2f %.3f\n", n.GetChars(), t.right, t.up, t.forward,
			        t.pitch, t.yaw, t.roll, t.scale);
		}
		fclose(f);
		Printf(PRINT_NONOTIFY, "vr_tune: saved %u weapon fits to %s\n", names.Size(), path.GetChars());
	}

	AActor* TunedWeapon()
	{
		player_t& pl = players[consoleplayer];
		if (gHand == 1 && pl.OffhandWeapon == nullptr) gHand = 0;
		return gHand == 1 ? pl.OffhandWeapon : pl.ReadyWeapon;
	}

	void Show(AActor* w, const DoomVR_WeaponTune& t)
	{
		FString s;
		s.Format("WEAPON TUNER - %s, %s (%s hand) - moving %s\n%s\n\n"
		         "sideways %+.1f cm   up %+.1f cm   forward %+.1f cm\n"
		         "tilt %+.1f   turn %+.1f   roll %+.1f   size x%.2f\n\n"
		         "arrows, PgUp/PgDn: move   Home: step (%s)   Insert: reset   End: next mode\n"
		         "[ ]: previous / next weapon   Delete: tuner off (Delete again: back on)\n"
		         "or the right stick (click: next mode); left stick click: other hand",
		         w ? w->GetClass()->TypeName.GetChars() : "no weapon", gDrawnAsSprite[gHand] ? "flat sprite" : "3D model",
		         gHand ? "left" : "right", kTargetName[gMode], kModes[gMode],
		         t.right * 100, t.up * 100, t.forward * 100, t.pitch, t.yaw, t.roll, t.scale, kStepName[gStep]);
		// Redrawn when it changes, and every 2 s so it stays up while the
		// weapon is held still (a mid-screen message fades on its own).
		static FString shown;
		static double age = 0;
		age += 0.1;
		if (s.Compare(shown) == 0 && age < 2.0) return;
		shown = s;
		age = 0;
		C_MidPrint(nullptr, s.GetChars());
	}
}

bool DoomVR_TuneActive()
{
	const bool on = vr_tune && gamestate == GS_LEVEL && players[consoleplayer].mo != nullptr;
	if (!on && gWasActive && gDirty) { Save(); gDirty = false; }
	if (!on && gWasActive) C_MidPrint(nullptr, "");
	gWasActive = on;
	return on;
}

// Our hand's tuning on a weapon (which 0: the gun hand, 1: the other hand on
// its front grip), or none.
// The Doom weapon a mod's weapon stands for: the one it replaces (DOOM CE's
// D64Shotgun replaces Shotgun), or the one a doomvrweaponas.txt in one of our
// gun packs names for it ("Shot_Gun Shotgun": Brutal Doom's shotgun is a
// weapon of its own, replacing nothing). NAME_None for neither. Our Doom
// guns reach such a weapon through that pack's MODELDEF; this lets its hold,
// its front grip, its fit and the fist and melee rules follow.
static TMap<FName, FName> gStandsFor;
static bool gStandsForLoaded = false;

// A game switched in this process: the next game's packs are read again.
void DoomVR_TuneForgetGame()
{
	gStandsFor.Clear();
	gStandsForLoaded = false;
}

FName DoomVR_StandsFor(FName weaponClass)
{
	TMap<FName, FName>& named = gStandsFor;
	bool& loaded = gStandsForLoaded;
	if (!loaded)
	{
		loaded = true;
		const int lump = fileSystem.CheckNumForFullName("doomvrweaponas.txt");
		if (lump >= 0)
		{
			auto data = fileSystem.ReadFile(lump);
			FString text(data.string(), data.size());
			for (const FString& line : text.Split("\n"))
			{
				char mine[128], doom[128];
				if (line.IndexOf("#") != 0 && sscanf(line.GetChars(), "%127s %127s", mine, doom) == 2) named.Insert(FName(mine), FName(doom));
			}
		}
	}
	if (const FName* n = named.CheckKey(weaponClass)) return *n;
	PClassActor* c = PClass::FindActor(weaponClass);
	return c && c->ActorInfo()->Replacee ? c->ActorInfo()->Replacee->TypeName : FName(NAME_None);
}

// A weapon with no fit of its own that stands for a Doom one has that one's.
static FName ReplacedName(FName weaponClass) { return DoomVR_StandsFor(weaponClass); }

const DoomVR_WeaponTune* DoomVR_GetHandTune(FName weaponClass, int which)
{
	Load();
	const char* part = which ? "support" : "hand";
	const DoomVR_WeaponTune* t = gTunes.CheckKey(FName(FStringf("%s@%s", weaponClass.GetChars(), part)));
	const FName was = t ? FName(NAME_None) : ReplacedName(weaponClass);
	if (was != NAME_None) t = gTunes.CheckKey(FName(FStringf("%s@%s", was.GetChars(), part)));
	return t;
}

// The tuner is moving the other hand: show it on the front grip.
bool DoomVR_TuneSupportShown()
{
	return vr_tune && gamestate == GS_LEVEL && gMode >= 6;
}

const DoomVR_WeaponTune& DoomVR_GetTune(FName weaponClass, bool sprite)
{
	static const DoomVR_WeaponTune none;
	Load();
	const DoomVR_WeaponTune* t = gTunes.CheckKey(TuneKey(weaponClass, sprite));
	const FName was = t ? FName(NAME_None) : ReplacedName(weaponClass);
	if (was != NAME_None) t = gTunes.CheckKey(TuneKey(was, sprite));
	return t ? *t : none;
}

void DoomVR_NoteWeaponDrawnAs(int hand, bool model)
{
	if (hand == 0 || hand == 1) gDrawnAsSprite[hand] = !model;
}

void DoomVR_TuneInput(float sx, float sy, bool modeClick, bool handClick, double seconds)
{
	Load();
	if (modeClick) { gMode = (gMode + 1) % kNumModes; gPrintWait = 0; }
	if (handClick && players[consoleplayer].OffhandWeapon != nullptr) { gHand ^= 1; gPrintWait = 0; }
	AActor* w = TunedWeapon();
	if (w == nullptr) return;
	const FName cls = ModeKey(w);
	DoomVR_WeaponTune t = ModeTune(w);

	// Past a dead zone, proportional: a light push is a fine adjustment.
	auto axis = [](float v) { return fabsf(v) < 0.15f ? 0.f : (v - (v > 0 ? 0.15f : -0.15f)) / 0.85f; };
	const float ax = axis(sx), ay = axis(sy);
	const float dt = float(seconds);
	const bool moving = ax != 0 || ay != 0;
	if (moving)
	{
		switch (gMode)
		{
		case 0: t.right += ax * 0.08f * dt; t.forward += ay * 0.08f * dt; break;
		case 1: t.up += ay * 0.08f * dt; t.scale = clamp(t.scale * (1.f + ax * 0.5f * dt), 0.2f, 5.f); break;
		case 2: t.pitch += ay * 30.f * dt; t.yaw += ax * 30.f * dt; break;
		case 3: t.roll += ax * 30.f * dt; break;
		case 4: case 6: t.right += ax * 0.08f * dt; t.forward += ay * 0.08f * dt; break;
		case 5: case 7: t.pitch += ay * 30.f * dt; t.yaw += ax * 30.f * dt; break;
		}
		gTunes.Insert(cls, t);
		gDirty = true;
	}
	else if (gDirty)
	{
		Save();	// each time the stick is let go
		gDirty = false;
	}
	gPrintWait -= seconds;
	if (gPrintWait <= 0) { Show(w, ModeTune(w)); gPrintWait = 0.1; }
}

// The keyboard while tuning: every key it uses goes to the tuner and nowhere
// else, down and up, so the arrows do not also walk and turn the player.
// Each press moves the weapon one step and saves.
bool DoomVR_TuneKey(const event_t* ev)
{
	if (ev == nullptr || (ev->type != EV_KeyDown && ev->type != EV_KeyUp)) return false;
	const int k = ev->data1;
	constexpr int kLeftBracket = 0x1a, kRightBracket = 0x1b;	// DIK_LBRACKET, DIK_RBRACKET

	// Delete: the tuner off, to play with the fit, and on again - in the
	// headset, in a level, with no menu up. (Its default binding, look down,
	// does nothing in a headset, where the head does the looking.)
	if (k == KEY_DEL && DoomVR_Active() && gamestate == GS_LEVEL && menuactive == MENU_Off &&
	    players[consoleplayer].mo != nullptr)
	{
		if (ev->type == EV_KeyDown)
		{
			vr_tune = !vr_tune;
			if (!vr_tune) Printf(PRINT_HIGH, "Weapon tuner off - press Delete to tune again\n");
			gPrintWait = 0;
		}
		return true;
	}

	const bool ours = k == KEY_LEFTARROW || k == KEY_RIGHTARROW || k == KEY_UPARROW || k == KEY_DOWNARROW ||
		k == KEY_PGUP || k == KEY_PGDN || k == KEY_HOME || k == KEY_END || k == KEY_INS ||
		k == kLeftBracket || k == kRightBracket;
	if (!ours || !DoomVR_TuneActive()) return false;
	if (ev->type == EV_KeyUp) return true;
	if (k == kLeftBracket || k == kRightBracket)
	{
		// The game's own weapon change; the tuner follows whatever is raised.
		AddCommandString(k == kRightBracket ? "weapnext" : "weapprev");
		gPrintWait = 0;
		return true;
	}
	Load();
	AActor* w = TunedWeapon();
	if (w == nullptr) return true;
	if (k == KEY_END) gMode = (gMode + 1) % kNumModes;
	const FName cls = ModeKey(w);
	if (k == KEY_END) {}
	else if (k == KEY_HOME) gStep = (gStep + 1) % 3;
	else if (k == KEY_INS)
	{
		gTunes.Remove(cls);
		Save();
	}
	else
	{
		DoomVR_WeaponTune t = ModeTune(w);
		const int dx = k == KEY_RIGHTARROW ? 1 : k == KEY_LEFTARROW ? -1 : 0;
		const int dy = k == KEY_UPARROW ? 1 : k == KEY_DOWNARROW ? -1 : 0;
		const int dz = k == KEY_PGUP ? 1 : k == KEY_PGDN ? -1 : 0;
		const float m = kStepMetres[gStep], a = kStepDegrees[gStep];
		switch (gMode)
		{
		case 0: t.right += dx * m; t.forward += dy * m; t.up += dz * m; break;
		case 1: t.up += (dy + dz) * m; if (dx) t.scale = clamp(t.scale * powf(kStepSize[gStep], float(dx)), 0.2f, 5.f); break;
		case 2: t.yaw += dx * a; t.pitch += dy * a; t.roll += dz * a; break;
		case 3: t.roll += (dx + dz) * a; break;
		case 4: case 6: t.right += dx * m; t.forward += dy * m; t.up += dz * m; break;
		case 5: case 7: t.yaw += dx * a; t.pitch += dy * a; t.roll += dz * a; break;
		}
		gTunes.Insert(cls, t);
		Save();
	}
	gDirty = false;
	gPrintWait = 0;
	Show(w, ModeTune(w));
	return true;
}

// Put the weapon in the tuned hand back to the measured placement.
CCMD(vr_tune_reset)
{
	Load();
	AActor* w = TunedWeapon();
	if (w == nullptr) { Printf("vr_tune_reset: no weapon in that hand\n"); return; }
	gTunes.Remove(TuneKey(w->GetClass()->TypeName, gDrawnAsSprite[gHand]));
	Save();
	Printf("vr_tune_reset: %s is back to the measured fit\n", w->GetClass()->TypeName.GetChars());
}
