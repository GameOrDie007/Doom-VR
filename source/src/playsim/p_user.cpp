/*
** p_user.cpp
**
** Player related stuff
**
**---------------------------------------------------------------------------
**
** Copyright 1993-1996 id Software
** Copyright 1994-1996 Raven Software
** Copyright 1998-1998 Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
** Copyright 1999-2016 Marisa Heit
** Copyright 2002-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** For code that originates from ZDoom the following applies:
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
** Player related stuff.
** Bobbing POV/weapon, movement.
** Pending weapon.
*/

#include "a_keys.h"
#include "a_morph.h"
#include "actorinlines.h" // IWYU pragma: keep
#include "basics.h"
#include "c_dispatch.h"
#include "cmdlib.h"
#include "d_event.h"
#include "d_main.h"
#include "d_net.h"
#include "d_player.h"
#include "doomdef.h"
#include "doomstat.h"
#include "events.h"
#include "filesystem.h"
#include "g_game.h"
#include "g_levellocals.h"
#include "gi.h"
#include "gstrings.h"
#include "i_net.h"
#include "intermission/intermission.h"
#include "m_random.h"
#include "p_acs.h"
#include "p_blockmap.h"
#include "p_enemy.h"
#include "p_local.h"
#include "p_pspr.h"
#include "p_linetracedata.h"
#include "p_trace.h"
#include "p_spec.h"
#include "r_utility.h"
#include "s_music.h"
#include "s_sound.h"
#include "sbar.h"
#include "serialize_obj.h" // IWYU pragma: keep
#include "serializer_doom.h"
#include "vm.h"
#include "doomvr_headset.h"
#include "p_maputl.h"
#include "p_spec.h"

extern int paused;

static FRandom pr_skullpop ("SkullPop");

// [SP] Allows respawn in single player
CVAR(Bool, sv_singleplayerrespawn, false, CVAR_SERVERINFO | CVAR_CHEAT)
CVAR(Float, snd_footstepvolume, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Variables for prediction
CVAR(Bool, cl_predict_specials, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Deprecated
CVAR(Bool, cl_noprediction, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, cl_predict_lerpscale, 0.05f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, cl_predict_lerpthreshold, 2.00f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Float, cl_rubberband_scale, 0.3f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
		self = 0.0f;
	else if (self > 1.0f)
		self = 1.0f;
}
CUSTOM_CVAR(Float, cl_rubberband_threshold, 20.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.1f)
		self = 0.1f;
}
CUSTOM_CVAR(Float, cl_rubberband_minmove, 20.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.1f)
		self = 0.1f;
}
CUSTOM_CVAR(Float, cl_rubberband_limit, 756.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
		self = 0.0f;
}

EXTERN_CVAR (Int, cl_debugprediction)

ColorSetList ColorSets;
PainFlashList PainFlashes;

// [Nash] FOV cvar setting
CUSTOM_CVAR(Float, fov, 90.f, CVAR_ARCHIVE | CVAR_USERINFO | CVAR_NOINITCALL)
{
	player_t *p = &players[consoleplayer];
	p->SetFOV(fov);
}

// Iterate through the Actor's own link nodes, storing their index within the
// list. This isn't guaranteed to give correct results, but that means someone
// messed with the world in a way will desync things eventually regardless.
// TODO: Add support for TID links.
struct FPhysicsLinkBackup
{
	// Track found link sources to make sure any additional ones are released. We don't want to
	// accidentally fix linking bugs when doing the relink from unpredicting.
	TMap<const sector_t*, bool> ValidSectors = {};
	TArray<int> TouchingSectorsPos = {};
	TMap<int, bool> ValidBlockNodes = {};
	TArray<int> BlockmapPos = {};
	int ThingPos = -1;
	bool bInSector = false;

	FPhysicsLinkBackup() = default;
	FPhysicsLinkBackup(const AActor& mo)
	{
		for (auto node = mo.touching_sectorlist; node != nullptr; node = node->m_tnext)
		{
			int index = 0;
			for (auto sNode = node->m_sector->touching_thinglist; sNode != nullptr; sNode = sNode->m_snext)
			{
				if (sNode->m_thing == &mo)
					break;
				++index;
			}
			TouchingSectorsPos.Push(index - 1);
			ValidSectors[node->m_sector] = true;
		}

		if (mo.Sector != nullptr)
		{
			int index = 0;
			for (auto act = mo.Sector->thinglist; act != nullptr; act = act->snext)
			{
				if (act == &mo)
				{
					bInSector = true;
					break;
				}
				++index;
			}
			if (bInSector)
				ThingPos = index - 1;
		}

		for (auto node = mo.BlockNode; node != nullptr; node = node->NextBlock)
		{
			int index = 0;
			for (auto bNode = mo.Level->blockmap.blocklinks[node->BlockIndex]; bNode != nullptr; bNode = bNode->NextActor)
			{
				if (bNode->Me == &mo)
					break;
				++index;
			}
			BlockmapPos.Push(index - 1);
			ValidBlockNodes[node->BlockIndex] = true;
		}
	}

	void Restore(AActor& mo)
	{
		size_t count = 0u;
		for (auto node = mo.touching_sectorlist; node != nullptr;)
		{
			if (ValidSectors.CheckKey(node->m_sector) == nullptr)
			{
				// This doesn't get updated in the unlinking for some reason, so we have to fix it
				// manually...
				auto next = node->m_tnext;
				if (node == mo.touching_sectorlist)
					mo.touching_sectorlist = next;
				P_DelSecnode(node, &sector_t::touching_thinglist);
				node = next;
				continue;
			}

			if (count >= TouchingSectorsPos.Size())
			{
				// We still need to check for invalid sector nodes, so keep going.
				node = node->m_tnext;
				continue;
			}

			// Only the sector list needs to be relinked as the order of the thing list is
			// deterministically rebuilt by its nature.
			if (node->m_sprev == nullptr)
				node->m_sector->touching_thinglist = node->m_snext;
			else
				node->m_sprev->m_snext = node->m_snext;
			if (node->m_snext != nullptr)
				node->m_snext->m_sprev = node->m_sprev;

			int i = 0;
			msecnode_t* prev = nullptr, * next = nullptr;
			for (next = node->m_sector->touching_thinglist; next != nullptr; prev = next, next = next->m_snext)
			{
				if (i > TouchingSectorsPos[count])
					break;
				++i;
			}
			node->m_sprev = prev;
			node->m_snext = next;
			if (prev != nullptr)
				prev->m_snext = node;
			if (next != nullptr)
				next->m_sprev = node;
			if (prev == nullptr)
				node->m_sector->touching_thinglist = node;

			++count;
			node = node->m_tnext;
		}

		if (bInSector && mo.Sector != nullptr)
		{
			*mo.sprev = mo.snext;
			if (mo.snext != nullptr)
				mo.snext->sprev = mo.sprev;

			int i = 0;
			AActor** next = nullptr;
			for (next = &mo.Sector->thinglist; *next != nullptr; next = &(*next)->snext)
			{
				if (i > ThingPos)
					break;
				++i;
			}

			mo.snext = *next;
			if (*next != nullptr)
				(*next)->sprev = &mo.snext;
			mo.sprev = next;
			*mo.sprev = &mo;
		}

		count = 0u;
		for (auto node = mo.BlockNode; node != nullptr;)
		{
			if (ValidBlockNodes.CheckKey(node->BlockIndex) == nullptr)
			{
				auto next = node->NextBlock;
				if (next != nullptr)
					next->PrevBlock = node->PrevBlock;
				*(node->PrevBlock) = node->NextBlock;
				if (node->NextActor != nullptr)
					node->NextActor->PrevActor = node->PrevActor;
				*(node->PrevActor) = node->NextActor;
				node->Release();
				node = next;
				continue;
			}

			if (count >= BlockmapPos.Size())
			{
				// We still need to check for invalid blockmap nodes, so keep going.
				node = node->NextBlock;
				continue;
			}

			// Same as sector linking above.
			*node->PrevActor = node->NextActor;
			if (node->NextActor != nullptr)
				node->NextActor->PrevActor = node->PrevActor;

			int i = 0;
			FBlockNode** next = nullptr;
			for (next = &mo.Level->blockmap.blocklinks[node->BlockIndex]; *next != nullptr; next = &(*next)->NextActor)
			{
				if (i > BlockmapPos[count])
					break;
				++i;
			}

			node->NextActor = *next;
			if (*next != nullptr)
				(*next)->PrevActor = &node->NextActor;
			node->PrevActor = next;
			*node->PrevActor = node;

			++count;
			node = node->NextBlock;
		}
	}
};

struct FObjectBackup
{
private:
	TObjPtr<DObject*> _obj = MakeObjPtr<DObject*>(nullptr);
public:
	FObjectBackup() = default;
	FObjectBackup(DObject& obj)
	{
		_obj = &obj;
	}

	template<class T>
	T* GetObject()
	{
		return dyn_cast<T>(_obj);
	}
};
struct FActorBackup : public FObjectBackup
{
private:
	FPhysicsLinkBackup _link = {};
	int _statNum = -1;
	int _tid = INT_MAX;
public:
	FActorBackup(AActor& act) : FObjectBackup(act)
	{
		_link = { act };
	}

	void PostBackup()
	{
		auto act = GetObject<AActor>();
		if (act != nullptr)
		{
			// TODO: This needs to be handled properly in the rest of the physics code...
			act->flags &= ~MF_PICKUP;
			act->flags2 &= ~MF2_PUSHWALL;
		}
	}

	void PreRollback()
	{
		auto act = GetObject<AActor>();
		if (act == nullptr)
			return;

		// These won't relink properly on rollback so we need to keep the old values as
		// they were. This still needs to be serialized in case the Actor was destroyed so we
		// can relink it back in properly after it's been recreated.
		_statNum = act->GetStatNum();
		_tid = act->tid;
		act->UnlinkFromWorld(nullptr);
	}

	void PostRollback()
	{
		auto act = GetObject<AActor>();
		if (act == nullptr)
			return;

		if (_statNum == -1)
			act->ChangeStatNum(act->GetStatNum());
		else
			act->RollbackStatNum(_statNum);

		if (_tid == INT_MAX)
		{
			_tid = act->tid;
			act->tid = 0;
			act->SetTID(_tid);
		}
		else
		{
			act->tid = _tid;
		}

		act->LinkToWorld(nullptr);
		// TODO: This might cause issues with emulating undefined behavior regarding things like polyobject collisions? Will need to be
		// investigated further in case it breaks determinism.
		_link.Restore(*act);

		act->renderflags &= ~RF_NOINTERPOLATEVIEW;
		act->flags8 &= ~MF8_RECREATELIGHTS;
	}
};

struct FPredictionData
{
	bool bResetPrediction = false;
	int LastPredictedTic = 0;

	TArray<TObjPtr<DObject*>> RollbackObjectRefs = {};	// Try and reuse existing Objects when deserializing.
	TArray<FObjectBackup> RollbackObjects = {};			// If these Objects no longer exist, they must be recreated instead of left as a null pointer.
	TArray<FActorBackup> RollbackActors = {};
	TArray<size_t> RollbackPlayers = {};				// Store by index instead of pointer so it'll never be invalid when deserializing.
	FLevelLocals* RollbackLevel = nullptr;				// Save this for when opening the reader.
	std::string_view RollbackData;		// Snapshot of all saved Objects.
	FWriterBuffer RollbackWriterBuffer;
	char ParseBuffer[262144] = {};
	FReaderAllocator RollbackReaderAllocator;

	FPredictionData() : RollbackReaderAllocator(ParseBuffer, sizeof(ParseBuffer)) {}

	struct
	{
		DVector3 Pos = { 0.0, 0.0, 0.0 };
		int PortalGroup = 0;
		int Tic = -1;
	} LastPos = {};

	void ResetPos()
	{
		LastPos = {};
	}

	void ClearBackup()
	{
		RollbackObjectRefs.Clear();
		RollbackObjects.Clear();
		RollbackActors.Clear();
		RollbackPlayers.Clear();
		RollbackLevel = nullptr;
		RollbackData = "";
	}

	void Mark()
	{
		// Try and avoid losing references to any Objects being used by a backed up entity, that way
		// we can avoid pointers getting unnecessarily nulled. The fully rolled back Objects should
		// also be in here which, if they weren't destroyed manually, allows us to skip the step of
		// creating a new object should the reference get lost.
		for (DObject* obj : RollbackObjectRefs)
			GC::Mark(obj);
	}
} static PredictionData = {};

// [GRB] Custom player classes
TArray<FPlayerClass> PlayerClasses;

FPlayerClass::FPlayerClass ()
{
	Type = NULL;
	Flags = 0;
}

FPlayerClass::~FPlayerClass ()
{
}

bool FPlayerClass::CheckSkin (int skin)
{
	for (unsigned int i = 0; i < Skins.Size (); i++)
	{
		if (Skins[i] == skin)
			return true;
	}

	return false;
}

DEFINE_ACTION_FUNCTION(FPlayerClass, CheckSkin)
{
	PARAM_SELF_STRUCT_PROLOGUE(FPlayerClass);
	PARAM_INT(skin);
	ACTION_RETURN_BOOL(self->CheckSkin(skin));
}

//===========================================================================
//
// GetDisplayName
//
//===========================================================================

FString GetPrintableDisplayName(PClassActor *cls)
{
	// Fixme; This needs a decent way to access the string table without creating a mess.
	// [RH] ????
	return cls->GetDisplayName();
}

bool ValidatePlayerClass(PClassActor *ti, const char *name)
{
	if (ti == NULL)
	{
		Printf("Unknown player class '%s'\n", name);
		return false;
	}
	else if (!ti->IsDescendantOf(NAME_PlayerPawn))
	{
		Printf("Invalid player class '%s'\n", name);
		return false;
	}
	else if (ti->GetDisplayName().IsEmpty())
	{
		Printf ("Missing displayname for player class '%s'\n", name);
		return false;
	}
	return true;
}

void SetupPlayerClasses ()
{
	FPlayerClass newclass;

	PlayerClasses.Clear();
	for (unsigned i = 0; i < gameinfo.PlayerClasses.Size(); i++)
	{
		PClassActor *cls = PClass::FindActor(gameinfo.PlayerClasses[i]);
		if (ValidatePlayerClass(cls, gameinfo.PlayerClasses[i].GetChars()))
		{
			newclass.Flags = 0;
			newclass.Type = cls;
			if ((GetDefaultByType(cls)->flags6 & MF6_NOMENU))
			{
				newclass.Flags |= PCF_NOMENU;
			}
			PlayerClasses.Push(newclass);
		}
	}
}

CCMD (playerclasses)
{
	for (unsigned int i = 0; i < PlayerClasses.Size (); i++)
	{
		Printf ("%3d: Class = %s, Name = %s\n", i,
			PlayerClasses[i].Type->TypeName.GetChars(),
			PlayerClasses[i].Type->GetDisplayName().GetChars());
	}
}

//
// Movement.
//

player_t::~player_t()
{
	DestroyPSprites();
}

void player_t::CopyFrom(player_t &p)
{
	mo = p.mo;
	playerstate = p.playerstate;
	cmd = p.cmd;
	original_cmd = p.original_cmd;
	original_oldbuttons = p.original_oldbuttons;
	// Intentionally not copying userinfo!
	cls = p.cls;
	DesiredFOV = p.DesiredFOV;
	FOV = p.FOV;
	viewz = p.viewz;
	viewheight = p.viewheight;
	deltaviewheight = p.deltaviewheight;
	bob = p.bob;
	BobTimer = p.BobTimer;
	Vel = p.Vel;
	centering = p.centering;
	turnticks = p.turnticks;
	attackdown = p.attackdown;
	usedown = p.usedown;
	oldbuttons = p.oldbuttons;
	health = p.health;
	inventorytics = p.inventorytics;
	CurrentPlayerClass = p.CurrentPlayerClass;
	memcpy(frags, &p.frags, sizeof(frags));
	fragcount = p.fragcount;
	lastkilltime = p.lastkilltime;
	multicount = p.multicount;
	spreecount = p.spreecount;
	WeaponState = p.WeaponState;
	ReadyWeapon = p.ReadyWeapon;
	OffhandWeapon = p.OffhandWeapon;
	PendingWeapon = p.PendingWeapon;
	cheats = p.cheats;
	timefreezer = p.timefreezer;
	refire = p.refire;
	inconsistant = p.inconsistant;
	waiting = p.waiting;
	killcount = p.killcount;
	itemcount = p.itemcount;
	secretcount = p.secretcount;
	damagecount = p.damagecount;
	bonuscount = p.bonuscount;
	hazardcount = p.hazardcount;
	hazardtype = p.hazardtype;
	hazardinterval = p.hazardinterval;
	poisoncount = p.poisoncount;
	poisontype = p.poisontype;
	poisonpaintype = p.poisonpaintype;
	poisoner = p.poisoner;
	attacker = p.attacker;
	extralight = p.extralight;
	fixedcolormap = p.fixedcolormap;
	fixedlightlevel = p.fixedlightlevel;
	FullbrightMode = p.FullbrightMode;
	bForceFullbright = p.bForceFullbright;
	morphTics = p.morphTics;
	MorphedPlayerClass = p.MorphedPlayerClass;
	MorphStyle = p.MorphStyle;
	MorphExitFlash = p.MorphExitFlash;
	PremorphWeapon = p.PremorphWeapon;
	chickenPeck = p.chickenPeck;
	jumpTics = p.jumpTics;
	onground = p.onground;
	respawn_time = p.respawn_time;
	camera = p.camera;
	air_finished = p.air_finished;
	LastDamageType = p.LastDamageType;
	Bot = p.Bot;
	settings_controller = p.settings_controller;
	BlendR = p.BlendR;
	BlendG = p.BlendG;
	BlendB = p.BlendB;
	BlendA = p.BlendA;
	LogText = p.LogText;
	MinPitch = p.MinPitch;
	MaxPitch = p.MaxPitch;
	crouching = p.crouching;
	crouchdir = p.crouchdir;
	crouchfactor = p.crouchfactor;
	crouchoffset = p.crouchoffset;
	crouchviewdelta = p.crouchviewdelta;
	weapons = p.weapons;
	ConversationNPC = p.ConversationNPC;
	ConversationPC = p.ConversationPC;
	ConversationNPCAngle = p.ConversationNPCAngle;
	ConversationFaceTalker = p.ConversationFaceTalker;
	MUSINFOactor = p.MUSINFOactor;
	MUSINFOtics = p.MUSINFOtics;
	SoundClass = p.SoundClass;
	LastSafePos = p.LastSafePos;
	angleOffsetTargets = p.angleOffsetTargets;
	// This needs to transfer ownership completely.
	psprites = p.psprites;
	p.psprites = nullptr;
}

size_t player_t::PropagateMark()
{
	GC::Mark(mo);
	GC::Mark(poisoner);
	GC::Mark(attacker);
	GC::Mark(camera);
	GC::Mark(Bot);
	GC::Mark(ReadyWeapon);
	GC::Mark(OffhandWeapon);
	GC::Mark(ConversationNPC);
	GC::Mark(ConversationPC);
	GC::Mark(MUSINFOactor);
	GC::Mark(PremorphWeapon);
	GC::Mark(psprites);
	if (PendingWeapon != WP_NOCHANGE)
	{
		GC::Mark(PendingWeapon);
	}
	return sizeof(*this);
}

void player_t::SetLogNumber (int num)
{
	char lumpname[26];
	int lumpnum;

	// First look up TXT_LOGTEXT%d in the string table
	mysnprintf(lumpname, countof(lumpname), "$TXT_LOGTEXT%d", num);
	auto text = GStrings.CheckString(lumpname+1);
	if (text)
	{
		SetLogText(lumpname);	// set the label, not the content, so that a language change can be picked up.
		return;
	}

	mysnprintf (lumpname, countof(lumpname), "LOG%d", num);
	lumpnum = fileSystem.CheckNumForName (lumpname);
	if (lumpnum != -1)
	{
		auto fn = fileSystem.GetFileContainer(lumpnum);
		auto wadname = fileSystem.GetResourceFileName(fn);
		if (!stricmp(wadname, "STRIFE0.WAD") || !stricmp(wadname, "STRIFE1.WAD") || !stricmp(wadname, "SVE.WAD"))
		{
			// If this is an original IWAD text, try looking up its lower priority string version first.

			mysnprintf(lumpname, countof(lumpname), "$TXT_ILOG%d", num);
			auto text = GStrings.CheckString(lumpname + 1);
			if (text)
			{
				SetLogText(lumpname);	// set the label, not the content, so that a language change can be picked up.
				return;
			}
		}

		auto lump = fileSystem.ReadFile(lumpnum);
		SetLogText (lump.string());
	}
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, SetLogNumber)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	PARAM_INT(log);
	self->SetLogNumber(log);
	return 0;
}

void player_t::SetLogText (const char *text)
{
	 LogText = text;

	if (mo && mo->CheckLocalView())
	{
		// Print log text to console
		Printf(PRINT_NONOTIFY, TEXTCOLOR_GOLD "%s\n", LogText[0] == '$' ? GStrings.GetString(text + 1) : text);
	}
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, SetLogText)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	PARAM_STRING(log);
	self->SetLogText(log.GetChars());
	return 0;
}

void player_t::SetSubtitle(int num, FSoundID soundid)
{
	char lumpname[36];

	if (gameinfo.flags & GI_SHAREWARE) return;	// Subtitles are only for the full game.

	// Do we have a subtitle for this log entry's voice file?
	mysnprintf(lumpname, countof(lumpname), "$TXT_SUB_LOG%d", num);
	auto text = GStrings.GetLanguageString(lumpname+1, FStringTable::default_table);
	if (text != nullptr)
	{
		SubtitleText = lumpname;
		int sl = soundid == NO_SOUND ? 7000 : max<int>(7000, S_GetMSLength(soundid));
		SubtitleCounter = sl * TICRATE / 1000;
	}
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, SetSubtitleNumber)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	PARAM_INT(log);
	PARAM_SOUND(soundid);
	self->SetSubtitle(log, soundid);
	return 0;
}

int player_t::GetSpawnClass()
{
	const PClass * type = PlayerClasses[CurrentPlayerClass].Type;
	return GetDefaultByType(type)->IntVar(NAME_SpawnMask);
}

// [Nash] Set FOV
void player_t::SetFOV(float fov)
{
	player_t *p = &players[consoleplayer];
	if (p != nullptr && p->mo != nullptr)
	{
		if (dmflags & DF_NO_FOV)
		{
			if (consoleplayer == Net_Arbitrator)
			{
				Net_WriteInt8(DEM_MYFOV);
			}
			else
			{
				Printf("A setting controller has disabled FOV changes.\n");
				return;
			}
		}
		else
		{
			Net_WriteInt8(DEM_MYFOV);
		}
		Net_WriteFloat(clamp<float>(fov, 5.f, 179.f));
	}
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, SetFOV)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	PARAM_FLOAT(fov);
	self->SetFOV((float)fov);
	return 0;
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, SetSkin)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	PARAM_INT(skinIndex);
	if (skinIndex >= 0 && skinIndex < Skins.SSize())
	{
		// commented code - cvar_set calls this automatically, along with saving the skin selection.
		//self->userinfo.SkinNumChanged(skinIndex);
		cvar_set("skin", Skins[skinIndex].Name.GetChars());
		ACTION_RETURN_INT(self->userinfo.GetSkin());
	}
	else
	{
		ACTION_RETURN_INT(-1);
	}
}

//===========================================================================
//
// EnumColorsets
//
// Only used by the menu so it doesn't really matter that it's a bit
// inefficient.
//
//===========================================================================

static int intcmp(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

void EnumColorSets(PClassActor *cls, TArray<int> *out)
{
	TArray<int> deleteds;

	out->Clear();
	for (int i = ColorSets.Size() - 1; i >= 0; i--)
	{
		if (std::get<0>(ColorSets[i])->IsAncestorOf(cls))
		{
			int v = std::get<1>(ColorSets[i]);
			if (out->Find(v) == out->Size() && deleteds.Find(v) == deleteds.Size())
			{
				if (std::get<2>(ColorSets[i]).Name == NAME_None) deleteds.Push(v);
				else out->Push(v);
			}
		}
	}
	qsort(&(*out)[0], out->Size(), sizeof(int), intcmp);
}

DEFINE_ACTION_FUNCTION(FPlayerClass, EnumColorSets)
{
	PARAM_SELF_STRUCT_PROLOGUE(FPlayerClass);
	PARAM_POINTER(out, TArray<int>);
	EnumColorSets(self->Type, out);
	return 0;
}

//==========================================================================
//
//
//==========================================================================

FPlayerColorSet *GetColorSet(PClassActor *cls, int setnum)
{
	for (int i = ColorSets.Size() - 1; i >= 0; i--)
	{
		if (std::get<1>(ColorSets[i]) == setnum &&
			std::get<0>(ColorSets[i])->IsAncestorOf(cls))
		{
			auto c = &std::get<2>(ColorSets[i]);
			return c->Name != NAME_None ? c : nullptr;
		}
	}
	return nullptr;
}

DEFINE_ACTION_FUNCTION(FPlayerClass, GetColorSetName)
{
	PARAM_SELF_STRUCT_PROLOGUE(FPlayerClass);
	PARAM_INT(setnum);
	auto p = GetColorSet(self->Type, setnum);
	ACTION_RETURN_INT(p ? p->Name.GetIndex() : 0);
}

//==========================================================================
//
//
//==========================================================================

static int GetPainFlash(AActor *info, int type)
{
	// go backwards through the list and return the first item with a
	// matching damage type for an ancestor of our class.
	// This will always return the best fit because any parent class
	// must be processed before its children.
	for (int i = PainFlashes.Size() - 1; i >= 0; i--)
	{
		if (std::get<1>(PainFlashes[i]) == ENamedName(type) &&
			std::get<0>(PainFlashes[i])->IsAncestorOf(info->GetClass()))
		{
			return std::get<2>(PainFlashes[i]);
		}
	}
	return 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(APlayerPawn, GetPainFlashForType, GetPainFlash)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_INT(type);
	ACTION_RETURN_INT(GetPainFlash(self, type));
}

//===========================================================================
//
// player_t :: SendPitchLimits
//
// Ask the local player's renderer what pitch restrictions should be imposed
// and let everybody know. Only sends data for the consoleplayer, since the
// local player is the only one our data is valid for.
//
//===========================================================================

EXTERN_CVAR(Float, maxviewpitch)
EXTERN_CVAR(Bool, cl_oldfreelooklimit);

static int GetSoftPitch(bool down)
{
	int MAX_DN_ANGLE = min(56, (int)maxviewpitch); // Max looking down angle
	int MAX_UP_ANGLE = min(32, (int)maxviewpitch); // Max looking up angle
	return (down ? MAX_DN_ANGLE : ((cl_oldfreelooklimit) ? MAX_UP_ANGLE : MAX_DN_ANGLE));
}

void player_t::SendPitchLimits() const
{
	if (this - players == consoleplayer)
	{
		int uppitch, downpitch;

		if (!V_IsHardwareRenderer())
		{
			uppitch = GetSoftPitch(false);
			downpitch = GetSoftPitch(true);
		}
		else
		{
			uppitch = downpitch = (int)maxviewpitch;
		}

		Net_WriteInt8(DEM_SETPITCHLIMIT);
		Net_WriteInt8(uppitch);
		Net_WriteInt8(downpitch);
	}
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, SendPitchLimits)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	self->SendPitchLimits();
	return 0;
}

bool player_t::HasWeaponsInSlot(int slot) const
{
	for (int i = 0; i < weapons.SlotSize(slot); i++)
	{
		PClassActor *weap = weapons.GetWeapon(slot, i);
		if (weap != NULL && mo->FindInventory(weap)) return true;
	}
	return false;
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, HasWeaponsInSlot)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	PARAM_INT(slot);
	ACTION_RETURN_BOOL(self->HasWeaponsInSlot(slot));
}

bool player_t::Resurrect()
{
	if (mo == nullptr || mo->IsKindOf(NAME_PlayerChunk)) return false;
	mo->Revive();
	playerstate = PST_LIVE;
	health = mo->health = mo->GetDefault()->health;
	viewheight = DefaultViewHeight();
	mo->renderflags &= ~RF_INVISIBLE;
	mo->Height = mo->GetDefault()->Height;
	mo->radius = mo->GetDefault()->radius;
	mo->special1 = 0;	// required for the Hexen fighter's fist attack.
								// This gets set by AActor::Die as flag for the wimpy death and must be reset here.
	mo->SetState(mo->SpawnState);
	int pnum = mo->Level->PlayerNum(this);
	if (!(mo->flags2 & MF2_DONTTRANSLATE))
	{
		mo->Translation = TRANSLATION(TRANSLATION_Players, uint8_t(pnum));
	}
	if (ReadyWeapon != nullptr)
	{
		PendingWeapon = ReadyWeapon;
		P_BringUpWeapon(this);
	}

	if (mo->alternative != nullptr)
	{
		P_UnmorphActor(mo, mo);
	}

	// player is now alive.
	// fire E_PlayerRespawned and start the ACS SCRIPT_Respawn.
	mo->Level->localEventManager->PlayerRespawned(pnum);
	mo->Level->Behaviors.StartTypedScripts(SCRIPT_Respawn, mo, true);
	return true;
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, Resurrect)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_BOOL(self->Resurrect());
}

player_t* player_t::GetNextPlayer(player_t* p, bool noBots)
{
	int pNum = player_t::GetNextPlayerNumber(p == nullptr ? -1 : p - players, noBots);
	return pNum != -1 ? &players[pNum] : nullptr;
}

DEFINE_ACTION_FUNCTION_NATIVE(_PlayerInfo, GetNextPlayer, player_t::GetNextPlayer)
{
	PARAM_PROLOGUE;
	PARAM_POINTER(p, player_t);
	PARAM_BOOL(noBots);

	ACTION_RETURN_POINTER(player_t::GetNextPlayer(p, noBots));
}

int player_t::GetNextPlayerNumber(int pNum, bool noBots)
{
	int m = MAXPLAYERS;
	int i = max<int>(pNum + 1, 0);

	for (; i < m; ++i)
	{
		if (playeringame[i] && (!noBots || players[i].Bot == nullptr))
			break;
	}

	return i < m ? i : -1;
}

DEFINE_ACTION_FUNCTION_NATIVE(_PlayerInfo, GetNextPlayerNumber, player_t::GetNextPlayerNumber)
{
	PARAM_PROLOGUE;
	PARAM_INT(pNum);
	PARAM_BOOL(noBots);

	ACTION_RETURN_INT(player_t::GetNextPlayerNumber(pNum, noBots));
}

static int GetFullbrightMode(player_t* self)
{
	return self->GetFullbrightMode();
}

DEFINE_ACTION_FUNCTION_NATIVE(_PlayerInfo, GetFullbrightMode, GetFullbrightMode)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_INT(self->GetFullbrightMode());
}

static void SetFullbrightMode(player_t* self, int mode, bool force)
{
	self->SetFullbrightMode(static_cast<EFullbrightMode>(mode), force);
}

DEFINE_ACTION_FUNCTION_NATIVE(_PlayerInfo, SetFullbrightMode, SetFullbrightMode)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	PARAM_INT(mode);
	PARAM_BOOL(force);
	self->SetFullbrightMode(static_cast<EFullbrightMode>(mode), force);
	return 0;
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetUserName)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	PARAM_UINT(charLimit);
	ACTION_RETURN_STRING(self->userinfo.GetName(charLimit));
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetNeverSwitch)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_BOOL(self->userinfo.GetNeverSwitch());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetClassicFlight)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_BOOL(self->userinfo.GetClassicFlight());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetColor)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_INT(self->userinfo.GetColor());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetColorSet)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_INT(self->userinfo.GetColorSet());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetPlayerClassNum)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_INT(self->userinfo.GetPlayerClassNum());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetSkin)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_INT(self->userinfo.GetSkin());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetSkinCount)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_INT(Skins.SSize());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetGender)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_INT(self->userinfo.GetGender());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetAutoaim)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_FLOAT(self->userinfo.GetAutoaim());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetTeam)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_INT(self->userinfo.GetTeam());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetNoAutostartMap)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_INT(self->userinfo.GetNoAutostartMap());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetWBobSpeed)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_FLOAT(self->userinfo.GetWBobSpeed());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetWBobFire)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_FLOAT(self->userinfo.GetWBobFire());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetMoveBob)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_FLOAT(self->userinfo.GetMoveBob());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetFViewBob)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_BOOL(self->userinfo.GetFViewBob());
}

DEFINE_ACTION_FUNCTION(_PlayerInfo, GetStillBob)
{
	PARAM_SELF_STRUCT_PROLOGUE(player_t);
	ACTION_RETURN_FLOAT(self->userinfo.GetStillBob());
}

//===========================================================================
//
//
//
//===========================================================================

static int SetupCrouchSprite(AActor *self, int crouchsprite)
{
	// Check whether a PWADs normal sprite is to be combined with the base WADs
	// crouch sprite. In such a case the sprites normally don't match and it is
	// best to disable the crouch sprite.
	if (crouchsprite > 0)
	{
		// This assumes that player sprites always exist in rotated form and
		// that the front view is always a separate sprite. So far this is
		// true for anything that exists.
		FString normspritename = sprites[self->SpawnState->sprite].name;
		FString crouchspritename = sprites[crouchsprite].name;

		int spritenorm = fileSystem.CheckNumForName((normspritename + "A1").GetChars(), FileSys::ns_sprites);
		if (spritenorm == -1)
		{
			spritenorm = fileSystem.CheckNumForName((normspritename + "A0").GetChars(), FileSys::ns_sprites);
		}

		int spritecrouch = fileSystem.CheckNumForName((crouchspritename + "A1").GetChars(), FileSys::ns_sprites);
		if (spritecrouch == -1)
		{
			spritecrouch = fileSystem.CheckNumForName((crouchspritename + "A0").GetChars(), FileSys::ns_sprites);
		}

		if (spritenorm == -1 || spritecrouch == -1)
		{
			// Sprites do not exist so it is best to disable the crouch sprite.
			return false;
		}

		int wadnorm = fileSystem.GetFileContainer(spritenorm);
		int wadcrouch = fileSystem.GetFileContainer(spritenorm);

		if (wadnorm > fileSystem.GetMaxIwadNum() && wadcrouch <= fileSystem.GetMaxIwadNum())
		{
			// Question: Add an option / disable crouching or do what?
			return false;
		}
	}
	return true;
}

DEFINE_ACTION_FUNCTION_NATIVE(APlayerPawn, SetupCrouchSprite, SetupCrouchSprite)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_INT(crouchsprite);
	ACTION_RETURN_INT(SetupCrouchSprite(self, crouchsprite));
}

//===========================================================================
//
// Animations
//
//===========================================================================

void PlayIdle (AActor *player)
{
	IFVIRTUALPTRNAME(player, NAME_PlayerPawn, PlayIdle)
	{
		VMValue params[1] = { (DObject*)player };
		VMCall(func, params, 1, nullptr, 0);
	}
}

//===========================================================================
//
// A_PlayerScream
//
// try to find the appropriate death sound and use suitable
// replacements if necessary
//
//===========================================================================

DEFINE_ACTION_FUNCTION(AActor, A_PlayerScream)
{
	PARAM_SELF_PROLOGUE(AActor);

	FSoundID sound = NO_SOUND;
	int chan = CHAN_VOICE;

	if (self->player == NULL || self->DeathSound != NO_SOUND)
	{
		if (self->DeathSound != NO_SOUND)
		{
			S_Sound (self, CHAN_VOICE, CHANF_NORUMBLE, self->DeathSound, 1, ATTN_NORM);
		}
		else
		{
			S_Sound (self, CHAN_VOICE, CHANF_NORUMBLE, "*death", 1, ATTN_NORM);
		}
		return 0;
	}

	// Handle the different player death screams
	if ((((self->Level->flags >> 15) | (dmflags)) &
		(DF_FORCE_FALLINGZD | DF_FORCE_FALLINGHX)) &&
		self->Vel.Z <= -39)
	{
		sound = S_FindSkinnedSound (self, S_FindSound("*splat"));
		chan = CHAN_BODY;
	}

	if (!sound.isvalid() && self->special1<10)
	{ // Wimpy death sound
		sound = S_FindSkinnedSoundEx (self, "*wimpydeath", self->player->LastDamageType.GetChars());
	}
	if (!sound.isvalid() && self->health <= -50)
	{
		if (self->health > -100)
		{ // Crazy death sound
			sound = S_FindSkinnedSoundEx (self, "*crazydeath", self->player->LastDamageType.GetChars());
		}
		if (!sound.isvalid())
		{ // Extreme death sound
			sound = S_FindSkinnedSoundEx (self, "*xdeath", self->player->LastDamageType.GetChars());
			if (!sound.isvalid())
			{
				sound = S_FindSkinnedSoundEx (self, "*gibbed", self->player->LastDamageType.GetChars());
				chan = CHAN_BODY;
			}
		}
	}
	if (!sound.isvalid())
	{ // Normal death sound
		sound = S_FindSkinnedSoundEx (self, "*death", self->player->LastDamageType.GetChars());
	}

	if (chan != CHAN_VOICE)
	{
		for (int i = 0; i < 8; ++i)
		{ // Stop most playing sounds from this player.
		  // This is mainly to stop *land from messing up *splat.
			if (i != CHAN_WEAPON && i != CHAN_VOICE)
			{
				S_StopSound (self, i);
			}
		}
	}
	S_Sound (self, chan, CHANF_NORUMBLE, sound, 1, ATTN_NORM);
	return 0;
}

//===========================================================================
//
// P_CheckPlayerSprites
//
// Here's the place where crouching sprites are handled.
// R_ProjectSprite() calls this for any players.
//
//===========================================================================

void P_CheckPlayerSprite(AActor *actor, int &spritenum, DVector2 &scale)
{
	player_t *player = actor->player;
	int crouchspriteno;

	if (player->userinfo.GetSkin() != 0 && !(actor->flags4 & MF4_NOSKIN))
	{
		// Convert from default scale to skin scale.
		DVector2 defscale = actor->GetDefault()->Scale;
		scale.X *= Skins[player->userinfo.GetSkin()].Scale.X / double(defscale.X);
		scale.Y *= Skins[player->userinfo.GetSkin()].Scale.Y / double(defscale.Y);
	}

	// Set the crouch sprite?
	if (player->mo == actor && player->crouchfactor < 0.75)
	{
		int crouchsprite = player->mo->IntVar(NAME_crouchsprite);
		if (spritenum == actor->SpawnState->sprite || spritenum == crouchsprite)
		{
			crouchspriteno = crouchsprite;
		}
		else if (!(actor->flags4 & MF4_NOSKIN) &&
				(spritenum == Skins[player->userinfo.GetSkin()].sprite ||
				 spritenum == Skins[player->userinfo.GetSkin()].crouchsprite))
		{
			crouchspriteno = Skins[player->userinfo.GetSkin()].crouchsprite;
		}
		else
		{ // no sprite -> squash the existing one
			crouchspriteno = -1;
		}

		if (crouchspriteno > 0)
		{
			spritenum = crouchspriteno;
		}
		else if (player->playerstate != PST_DEAD && player->crouchfactor < 0.75)
		{
			scale.Y *= 0.5;
		}
	}
}

CUSTOM_CVAR (Float, sv_aircontrol, 0.00390625f, CVAR_SERVERINFO|CVAR_NOSAVE|CVAR_NOINITCALL)
{
	primaryLevel->aircontrol = self;
	primaryLevel->AirControlChanged ();
}

//==========================================================================
//
// P_FallingDamage
//
//==========================================================================

void P_FallingDamage (AActor *actor)
{
	int damagestyle;
	int damage;
	double vel;

	damagestyle = ((actor->Level->flags >> 15) | (dmflags)) &
		(DF_FORCE_FALLINGZD | DF_FORCE_FALLINGHX);

	if (damagestyle == 0)
		return;

	if (actor->floorsector->Flags & SECF_NOFALLINGDAMAGE)
		return;

	vel = fabs(actor->Vel.Z);

	// Since Hexen falling damage is stronger than ZDoom's, it takes
	// precedence. ZDoom falling damage may not be as strong, but it
	// gets felt sooner.

	switch (damagestyle)
	{
	case DF_FORCE_FALLINGHX:		// Hexen falling damage
		if (vel <= 23)
		{ // Not fast enough to hurt
			return;
		}
		if (vel >= 63)
		{ // automatic death
			damage = TELEFRAG_DAMAGE;
		}
		else
		{
			vel *= (16. / 23);
			damage = int((vel * vel) / 10 - 24);
			if (actor->Vel.Z > -39 && damage > actor->health
				&& actor->health != 1)
			{ // No-death threshold
				damage = actor->health-1;
			}
		}
		break;

	case DF_FORCE_FALLINGZD:		// ZDoom falling damage
		if (vel <= 19)
		{ // Not fast enough to hurt
			return;
		}
		if (vel >= 84)
		{ // automatic death
			damage = TELEFRAG_DAMAGE;
		}
		else
		{
			damage = int((vel*vel*(11 / 128.) - 30) / 2);
			if (damage < 1)
			{
				damage = 1;
			}
		}
		break;

	case DF_FORCE_FALLINGST:		// Strife falling damage
		if (vel <= 20)
		{ // Not fast enough to hurt
			return;
		}
		// The minimum amount of damage you take from falling in Strife
		// is 52. Ouch!
		damage = int(vel / (25000./65536.));
		break;

	default:
		return;
	}

	if (actor->player)
	{
		S_Sound (actor, CHAN_AUTO, 0, "*land", 1, ATTN_NORM);
		P_NoiseAlert (actor, actor, true);
		if (damage >= TELEFRAG_DAMAGE && ((actor->player->cheats & (CF_GODMODE | CF_BUDDHA) ||
			(actor->FindInventory(PClass::FindActor(NAME_PowerBuddha), true) != nullptr))))
		{
			damage = TELEFRAG_DAMAGE - 1;
		}
	}
	P_DamageMobj (actor, NULL, NULL, damage, NAME_Falling);
}

//----------------------------------------------------------------------------
//
// PROC P_CheckMusicChange
//
//----------------------------------------------------------------------------

void P_CheckMusicChange(player_t *player)
{
	// MUSINFO stuff
	if (player->MUSINFOtics >= 0 && player->MUSINFOactor != NULL)
	{
		if (--player->MUSINFOtics < 0)
		{
			if (player == player->mo->Level->GetConsolePlayer())
			{
				if (player->MUSINFOactor->args[0] != 0)
				{
					FName *music = player->MUSINFOactor->Level->info->MusicMap.CheckKey(player->MUSINFOactor->args[0]);

					if (music != NULL)
					{
						S_ChangeMusic(music->GetChars(), player->MUSINFOactor->args[1]);
					}
				}
				else
				{
					S_ChangeMusic("*");
				}
			}
			DPrintf(DMSG_NOTIFY, "MUSINFO change for player %d to %d\n", (int)player->mo->Level->PlayerNum(player), player->MUSINFOactor->args[0]);
		}
	}
}

//===========================================================================
//
// DoomVR: where the right controller points, and where the head looks,
// relative to the body, for PlayerThink to fire and use along.
// (yaw offset degrees, pitch degrees, 1) - or all zero with no headset or no
// right controller, which leaves the flat game's behaviour exactly as it was.
//
//===========================================================================

//===========================================================================
//
// DoomVR: shots from the hand (vr_shots_from_hand, off by default)
//
// The way the VR mods' engine fires: a player's hitscan (P_LineAttack) and
// missile (P_SpawnPlayerMissile) start at the hand - the off hand with
// LAF_ISOFFHAND / ALF_ISOFFHAND - and go the way AttackDir/OffhandDir give
// for the angle and pitch asked for: the request's offset from the pawn's own
// aim, turned with the hand. Measured against the benchmark with
// tools\hand-probe.py --dir (traces and missiles at four hand poses): no
// Doom 4-unit missile drop, no shooter height. The pawn's angles are then
// NOT turned to the controller while the weapon runs (DoomVRAim is off) -
// the hand does that. A hand with no controller shoots as the flat game.
// The math is actor.zs's DoomVRHandDir, kept identical.
//
//===========================================================================

// Not saved while it is being tested: the VR test launchers turn it on for their own run,
// so it can never leak into the normal launchers through the ini.
CVAR(Bool, vr_shots_from_hand, false, 0)

bool DoomVR_ShotFromHand(AActor* t1, bool offhand, DAngle& angle, DAngle& pitch, DVector3& origin)
{
	if (!vr_shots_from_hand || t1 == nullptr || t1->player == nullptr || t1->player->mo != t1) return false;
	// A shot made by the off-hand weapon's own action is the off hand's, flag or not.
	if (DoomVR_OffhandActionDepth > 0) offhand = true;
	static const FName tracked[2] = { "DoomVRAttackTracked", "DoomVROffhandTracked" };
	if (!t1->BoolVar(FName("OverrideAttackPosDir")) || !t1->BoolVar(tracked[offhand])) return false;
	origin = *(DVector3*)t1->ScriptVar(offhand ? FName("OffhandPos") : FName("AttackPos"), nullptr);
	const double yaw = (t1->FloatVar(offhand ? FName("OffhandAngle") : FName("AttackAngle")) + 90.0) * (M_PI / 180.0);
	const double pit = t1->FloatVar(offhand ? FName("OffhandPitch") : FName("AttackPitch")) * (M_PI / 180.0);
	const double rol = t1->FloatVar(offhand ? FName("OffhandRoll") : FName("AttackRoll")) * (M_PI / 180.0);
	double da = deltaangle(t1->Angles.Yaw, angle).Degrees() * (M_PI / 180.0);
	const double dp = (pitch - t1->Angles.Pitch).Degrees() * (M_PI / 180.0);
	if (offhand) da = -da;
	const DVector3 f(cos(pit) * cos(yaw), cos(pit) * sin(yaw), sin(pit));
	const DVector3 l0(-sin(yaw), cos(yaw), 0);
	const DVector3 u0(-sin(pit) * cos(yaw), -sin(pit) * sin(yaw), cos(pit));
	const DVector3 l = l0 * cos(rol) + u0 * sin(rol);
	const DVector3 u = u0 * cos(rol) - l0 * sin(rol);
	const DVector3 w = f * (cos(dp) * cos(da)) + l * (cos(dp) * sin(da)) + u * sin(dp);
	const double stretch = t1->Level->pixelstretch > 0 ? t1->Level->pixelstretch : 1.2;
	angle = DAngle::fromDeg(atan2(w.Y, w.X) * (180.0 / M_PI));
	pitch = DAngle::fromDeg(atan2(-w.Z, stretch * w.XY().Length()) * (180.0 / M_PI));
	return true;
}

//===========================================================================
//
// DoomVR: shots start at the gun (vr_shots_from_gun, off until worn)
//
// The stock games fire every shot from the pawn's middle, so with the gun
// held out at arm's length a rocket or a plasma ball came out of the player's chest
// or face. Here only the START moves, to the weapon
// hand (the controller's aim point, where the gun is drawn); the direction is
// the one the shot already had - the aim, the weapon's own spread and
// autoaim - so the red dot, which traces from the same start, still shows
// where it lands. Blows stay at the body (a punch reaches from where the game
// measures reach), and so does a shot whose hand is behind a wall or a closed
// door from the body: started there, it would hit what is on the far side.
// Single player only, as room-scale: the hand is local, a netgame or a demo
// would desync. The start is on the BARREL's line, not the controller's tip:
// vr_shots_up above it and vr_shots_ahead along it, in the gun's own frame
// (from the controller's tip, shots left a bit beneath the gun).
//
//===========================================================================

CVAR(Bool, vr_shots_from_gun, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_shots_up, 0.06f, CVAR_GLOBALCONFIG)		// m above the controller's aim point
CVAR(Float, vr_shots_ahead, 0.15f, CVAR_GLOBALCONFIG)	// m along the aim
EXTERN_CVAR(Float, vr_hunits_per_meter)
EXTERN_CVAR(Float, vr_height_adjust)

// The held gun's muzzle, measured from its 3D model as drawn (models.cpp,
// DoomVR_MeasureMuzzle): metres right, up and ahead of the controller's aim
// point, in the aim's own frame. One spot for every gun left a long gun's
// tracers starting under its barrel; laid
// along the hand's yaw and pitch - the mods' 30-degree tilted direction, no
// roll - they started under it and to its side. It is placed
// from the controller's whole turn instead (DoomVR_HandPointPose), as the
// hand itself is. Without a model, or with a measure that makes no sense,
// the fixed spot below.
CVAR(Bool, vr_shots_model_muzzle, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
static struct { const void* weapon = nullptr; double right = 0, up = 0, ahead = 0; } DoomVR_Muzzle;
bool DoomVR_HandPointPose(int hand, const double p[3], double& right, double& up, double& forward);

void DoomVR_NoteMuzzle(const void* weapon, double right, double up, double ahead, const char* name)
{
	if (weapon == DoomVR_Muzzle.weapon && fabs(ahead - DoomVR_Muzzle.ahead) < 0.002 && fabs(up - DoomVR_Muzzle.up) < 0.002 && fabs(right - DoomVR_Muzzle.right) < 0.002) return;
	DoomVR_Muzzle.weapon = weapon;
	DoomVR_Muzzle.right = right;
	DoomVR_Muzzle.up = up;
	DoomVR_Muzzle.ahead = ahead;
	static int logged = 0;
	if (logged < 40)
	{
		++logged;
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: %s's muzzle %.2f m ahead, %.2f m up, %.2f m right of the controller's aim (from its model)\n",
		       name, ahead, up, right);
	}
}

// The held gun's muzzle in the map, for a body at bodyXY with its eyes at
// eyeZ, turned yawDeg - as DoomVR_SetHands places the hand. False with no
// measured muzzle for the gun up now.
static bool DoomVR_MuzzleAt(const DVector3& bodyXY, double eyeZ, double yawDeg, DVector3& at)
{
	if (!vr_shots_model_muzzle || DoomVR_Muzzle.weapon == nullptr || DoomVR_Muzzle.weapon != players[consoleplayer].ReadyWeapon) return false;
	const double p[3] = { DoomVR_Muzzle.right, DoomVR_Muzzle.up, -DoomVR_Muzzle.ahead };
	double right, up, forward;
	if (!DoomVR_HandPointPose(0, p, right, up, forward)) return false;
	const double u = vr_hunits_per_meter, t = yawDeg * (M_PI / 180.0);
	at = bodyXY;
	at.X += (forward * cos(t) + right * sin(t)) * u;
	at.Y += (forward * sin(t) - right * cos(t)) * u;
	at.Z = eyeZ + (up + vr_height_adjust) * u;
	return true;
}

// The hand's aim point moved onto the barrel: yaw in the map's degrees,
// pitch in degrees up.
static DVector3 DoomVR_OnBarrel(DVector3 hand, double yawDeg, double pitchUpDeg)
{
	const double u = vr_hunits_per_meter;
	const double y = yawDeg * (M_PI / 180.0), p = pitchUpDeg * (M_PI / 180.0);
	const DVector3 ahead(cos(p) * cos(y), cos(p) * sin(y), sin(p));
	const DVector3 up(-sin(p) * cos(y), -sin(p) * sin(y), cos(p));
	return hand + ahead * (vr_shots_ahead * u) + up * (vr_shots_up * u);
}

// The start, given the hand's place in the map: false (the body's start)
// when the line from the pawn's shooting height to the hand meets a wall, a
// floor or a ceiling.
static bool DoomVR_GunStart(AActor* mo, const DVector3& hand, DVector3& origin)
{
	const DVector3 body = mo->PosAtZ(mo->Center() - mo->Floorclip + mo->AttackOffset());
	const DVector3 d = hand - body;
	const double len = d.Length();
	if (len > 4 * vr_hunits_per_meter) return false;	// a stale or wild pose
	if (len > 0.5)
	{
		const DAngle yaw = DAngle::fromDeg(atan2(d.Y, d.X) * (180.0 / M_PI));
		const DAngle pit = DAngle::fromDeg(atan2(-d.Z, d.XY().Length()) * (180.0 / M_PI));
		FLineTraceData t;
		if (P_LineTrace(mo, yaw, len, pit, TRF_ABSPOSITION | TRF_THRUACTORS, body.Z, body.X, body.Y, &t) &&
			t.HitType != TRACE_HitNone && t.Distance < len - 0.25)
			return false;
	}
	origin = hand;
	return true;
}

bool DoomVR_ShotFromGun(AActor* t1, bool offhand, bool melee, DVector3& origin)
{
	if (!vr_shots_from_gun || vr_shots_from_hand || melee || offhand) return false;
	if (netgame || demoplayback || demorecording) return false;
	if (t1 == nullptr || t1->player != &players[consoleplayer] || t1->player->mo != t1) return false;
	if (DoomVR_WeaponActionDepth == 0 || DoomVR_OffhandActionDepth > 0) return false;
	if (!t1->BoolVar(FName("DoomVRAttackTracked"))) return false;
	// The gun's measured muzzle; else the fixed spot along the hand - AttackAngle
	// is the hand's yaw less 90 (the VR mods' convention), AttackPitch up.
	DVector3 hand;
	if (!DoomVR_MuzzleAt(t1->Pos(), t1->player->viewz, t1->Angles.Yaw.Degrees(), hand))
		hand = DoomVR_OnBarrel(*(DVector3*)t1->ScriptVar(FName("AttackPos"), nullptr),
			t1->FloatVar(FName("AttackAngle")) + 90.0, t1->FloatVar(FName("AttackPitch")));
	const bool ok = DoomVR_GunStart(t1, hand, origin);
	// The first few, so a headset log shows where shots started.
	static int logged = 0;
	if (logged < 12)
	{
		++logged;
		const DVector3 body = t1->PosAtZ(t1->Center() - t1->Floorclip + t1->AttackOffset());
		if (ok) Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: shot from the gun, %.2f m from the body's start\n", (hand - body).Length() / vr_hunits_per_meter);
		else Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: shot from the body - the gun hand is behind a wall\n");
	}
	return ok;
}

// DoomVR: casings. The Brutal family throws its spent cases as "missiles"
// (A_FireCustomMissile("50CaseSpawn", 0, 0, -6, -18)), placed by offsets
// meant for a flat screen; started at the barrel like a shot, they came out
// in front of the gun, under the muzzle. A
// harmless missile named for a case, casing, shell or brass starts at the
// ejection port instead: a little ahead of the grip, above it and to its
// right, in the gun's own frame.
CVAR(Float, vr_eject_ahead, 0.05f, CVAR_GLOBALCONFIG)	// m along the aim from the controller's aim point
CVAR(Float, vr_eject_up, 0.07f, CVAR_GLOBALCONFIG)		// m above it
CVAR(Float, vr_eject_right, 0.03f, CVAR_GLOBALCONFIG)	// m to the gun's right

bool DoomVR_IsCasing(PClassActor* type)
{
	if (type == nullptr) return false;
	const AActor* def = GetDefaultByType(type);
	if (def == nullptr || def->DamageVal > 0 || def->DamageFunc != nullptr) return false;
	FString name = type->TypeName.GetChars();
	name.ToLower();
	for (const char* w : { "case", "casing", "shell", "brass" })
		if (name.IndexOf(w) >= 0) return true;
	return false;
}

bool DoomVR_EjectFromGun(AActor* t1, DVector3& origin)
{
	if (!vr_shots_from_gun || vr_shots_from_hand) return false;
	if (netgame || demoplayback || demorecording) return false;
	if (t1 == nullptr || t1->player != &players[consoleplayer] || t1->player->mo != t1) return false;
	if (DoomVR_WeaponActionDepth == 0 || DoomVR_OffhandActionDepth > 0) return false;
	if (!t1->BoolVar(FName("DoomVRAttackTracked"))) return false;
	const DVector3 hand = *(DVector3*)t1->ScriptVar(FName("AttackPos"), nullptr);
	const double u = vr_hunits_per_meter;
	const double y = (t1->FloatVar(FName("AttackAngle")) + 90.0) * (M_PI / 180.0);
	const double p = t1->FloatVar(FName("AttackPitch")) * (M_PI / 180.0);
	const DVector3 ahead(cos(p) * cos(y), cos(p) * sin(y), sin(p));
	const DVector3 up(-sin(p) * cos(y), -sin(p) * sin(y), cos(p));
	const DVector3 right(sin(y), -cos(y), 0);
	return DoomVR_GunStart(t1, hand + ahead * (vr_eject_ahead * u) + up * (vr_eject_up * u) + right * (vr_eject_right * u), origin);
}

// The same start for the red dot, from the hand as this frame draws it: the
// drawn body place and yaw, and the drawn eye height, as DoomVR_SetHands
// places the hand from the pawn's.
static bool DoomVR_DrawnGunStart(AActor* mo, DAngle drawnYaw, const DVector3& drawnEye, DVector3& origin)
{
	if (!vr_shots_from_gun || vr_shots_from_hand || netgame || demoplayback || demorecording) return false;
	DVector3 muzzle;
	if (DoomVR_MuzzleAt(drawnEye, drawnEye.Z, drawnYaw.Degrees(), muzzle)) return DoomVR_GunStart(mo, muzzle, origin);
	double right, up, forward, hy, hp, hr;
	if (!DoomVR_HandPose(0, right, up, forward, hy, hp, hr)) return false;
	const double u = vr_hunits_per_meter;
	const double t = drawnYaw.Radians();
	DVector3 hand = drawnEye;
	hand.X += (forward * cos(t) + right * sin(t)) * u;
	hand.Y += (forward * sin(t) - right * cos(t)) * u;
	hand.Z = drawnEye.Z + (up + vr_height_adjust) * u;
	return DoomVR_GunStart(mo, DoomVR_OnBarrel(hand, drawnYaw.Degrees() + hy, hp), origin);
}

//===========================================================================
//
// DoomVR: the red dot crosshair's point (vr_crosshair; see doomvr_headset.cpp)
//
// Where the weapon's hitscan would land if it fired now: the same start (the
// pawn's shooting height, as P_LineAttack takes it), the same direction (the
// pawn turned to the controller's aim, as PlayerThink turns it around the
// weapon's actions), and the same autoaim (P_BulletSlope, as the weapons'
// own fire calls it - so the player's autoaim setting is honoured). The
// pawn's angles are put back at once; nothing else is touched. Not while
// shots come from the hand (the VR weapon mods, which bring their own
// sights). With vr_shots_from_gun the lines start where the shot will, at
// the drawn gun hand (DoomVR_DrawnGunStart).
//
//===========================================================================

EXTERN_CVAR(Bool, vr_crosshair)

// DoomVR: a game with an inventory (Heretic, Hexen, Strife and games built on
// them) takes the Steam Frame layout of the official Heretic + Hexen release;
// every other game the Doom + Doom II one (doomvr_headset.cpp).
bool DoomVR_InventoryGame()
{
	return (gameinfo.gametype & (GAME_Heretic | GAME_Hexen | GAME_Strife)) != 0;
}

// Physical play's holsters work only for a living player (a corpse's grip
// drew a weapon).
bool DoomVR_PlayerAlive()
{
	const player_t& p = players[consoleplayer];
	return p.mo != nullptr && p.playerstate == PST_LIVE;
}

// What each weapon was seen to fire, learned from its own shots (P_LineAttack
// and P_SpawnPlayerMissile call the two notes below), because nothing in a
// weapon's definition says whether it fires bullets, projectiles or blows -
// and they aim differently. A bullet keeps the hand's yaw and takes autoaim's
// pitch (P_BulletSlope); a projectile turns to the monster autoaim found, yaw
// and all (P_SpawnPlayerMissile); a blow reaches only so far; a projectile
// that falls or bounces lands where no straight line can show. A weapon that does both
// (Hexen's hammer and serpent staff swing close and shoot far) is shown by
// its ranged attack. Before a weapon's first shot: melee if it is flagged
// so, else a bullet.
enum class DoomVRRanged : uint8_t { None, Hitscan, Missile, Lobbed };
struct DoomVRWeaponShot
{
	DoomVRRanged ranged = DoomVRRanged::None;
	PClassActor* missile = nullptr;
	double meleeReach = 0;
};
static TMap<FName, DoomVRWeaponShot> DoomVR_WeaponShots;

// A game switched in this process: its weapons' classes are gone.
void DoomVR_ShotsForgetGame() { DoomVR_WeaponShots.Clear(); DoomVR_Muzzle.weapon = nullptr; }

// The local player's own main-hand weapon, firing now.
static DoomVRWeaponShot* DoomVR_ShotRecord(AActor* t1, bool offhand)
{
	if (t1 == nullptr || t1->player != &players[consoleplayer] || t1->player->mo != t1) return nullptr;
	if (t1->player->ReadyWeapon == nullptr || offhand || DoomVR_OffhandActionDepth > 0 || DoomVR_WeaponActionDepth == 0) return nullptr;
	return &DoomVR_WeaponShots[t1->player->ReadyWeapon->GetClass()->TypeName];
}

void DoomVR_NoteLineAttack(AActor* t1, double distance, bool melee, bool offhand)
{
	DoomVRWeaponShot* s = DoomVR_ShotRecord(t1, offhand);
	if (s == nullptr) return;
	// The recoil, or a fist's punch; Heretic's Staff (its model has no strike) thrusts.
	AActor* w = t1->player->ReadyWeapon;
	DoomVR_WeaponKick(melee || distance <= 3 * DEFMELEERANGE, w->IsKindOf(FName("Staff")) || w->IsKindOf(FName("StaffPowered")));
	if (melee || distance <= 3 * DEFMELEERANGE) s->meleeReach = distance;
	else { s->ranged = DoomVRRanged::Hitscan; s->missile = nullptr; }
}

void DoomVR_NoteMissile(AActor* source, PClassActor* type, bool offhand)
{
	DoomVRWeaponShot* s = DoomVR_ShotRecord(source, offhand);
	if (s == nullptr || type == nullptr) return;
	const AActor* def = GetDefaultByType(type);
	// Only a projectile that can hurt is the weapon's shot. Gameplay mods fire
	// effects the same way - Brutal Doom's shell casings, smoke and the
	// invisible 'Alerter' that wakes monsters - and a falling casing taught
	// the dot that the Minigun lobs its shots.
	if (def->DamageVal <= 0 && def->DamageFunc == nullptr) return;
	// Falling, or bouncing (Heretic's mace balls start level and are given
	// gravity in flight): no straight line shows where it ends up.
	const bool falls = !(def->flags & MF_NOGRAVITY) || (def->BounceFlags & BOUNCE_TypeMask);
	s->ranged = falls ? DoomVRRanged::Lobbed : DoomVRRanged::Missile;
	s->missile = type;
	DoomVR_WeaponKick(false);
}

// drawnYaw and drawnPos are the body's yaw and place as THIS frame draws them
// (the render viewpoint, eased between tics), not the pawn's, which move 35
// times a second: from the pawn's, the dot stood still for two or three frames
// of a smooth turn and then jumped, and two eyes saw three dots
// (RazeXR's v1.1 fix: the hand from the drawn yaw).
bool DoomVR_CrosshairShot(DVector3& hit, DAngle drawnYaw, DVector3 drawnEye)
{
	const DVector2 drawnPos = drawnEye.XY();
	if (!vr_crosshair || vr_shots_from_hand) return false;
	player_t* pl = &players[consoleplayer];
	AActor* mo = pl->mo;
	if (mo == nullptr || pl->playerstate != PST_LIVE || pl->ReadyWeapon == nullptr) return false;
	double yawOffset = 0, aimPitch = 0;
	if (!DoomVR_GetAim(yawOffset, aimPitch)) return false;

	DoomVRWeaponShot shot;
	if (auto seen = DoomVR_WeaponShots.CheckKey(pl->ReadyWeapon->GetClass()->TypeName)) shot = *seen;
	else if (pl->ReadyWeapon->IntVar(NAME_WeaponFlags) & WIF_MELEEWEAPON) shot.meleeReach = DEFMELEERANGE;
	if (shot.ranged == DoomVRRanged::Lobbed) return false;
	const bool melee = shot.ranged == DoomVRRanged::None && shot.meleeReach > 0;

	const DRotator saved = mo->Angles;
	mo->Angles.Yaw = drawnYaw + DAngle::fromDeg(yawOffset);
	mo->Angles.Pitch = DAngle::fromDeg(aimPitch);
	DAngle yaw = mo->Angles.Yaw;
	DAngle pitch;
	double range = 8192.;
	double sz = mo->Center() - mo->Z() + mo->AttackOffset();	// P_LineAttack's shootz, above the feet
	if (melee)
	{
		// As a punch aims: at what is within reach.
		range = shot.meleeReach;
		pitch = P_AimLineAttack(mo, yaw, range, nullptr, nullAngle, ALF_CHECK3D);
	}
	else if (shot.ranged == DoomVRRanged::Missile)
	{
		// As P_SpawnPlayerMissile aims (its caller's own nofreeaim and
		// noautoaim aside): straight ahead, then 5.625 degrees either side,
		// turning to whatever it finds.
		sz = mo->Center() - mo->Z() + mo->AttackOffset(-4);
		if (pl->ReadyWeapon->IntVar(NAME_WeaponFlags) & WIF_NOAUTOAIM)
		{
			pitch = mo->Angles.Pitch;
		}
		else
		{
			static const double angdiff[3] = { -5.625, 5.625, 0 };
			const AActor* def = shot.missile ? GetDefaultByType(shot.missile) : nullptr;
			const double targetRange = def && def->maxtargetrange > 0 ? def->maxtargetrange * 64 : 16 * 64.;
			FTranslatedLineTarget t;
			const DAngle ahead = yaw;
			int i = 2;
			do
			{
				yaw = ahead + DAngle::fromDeg(angdiff[i]);
				pitch = P_AimLineAttack(mo, yaw, targetRange, &t, nullAngle, 0);
				if (mo->Level->IsFreelookAllowed() && pl->userinfo.GetAimDist() <= 0.5) break;
			} while (t.linetarget == nullptr && --i >= 0);
			if (t.linetarget == nullptr)
			{
				yaw = ahead;
				if (!mo->Level->IsFreelookAllowed()) pitch = nullAngle;
			}
		}
	}
	else
	{
		pitch = P_BulletSlope(mo, nullptr, 0);
	}
	mo->Angles = saved;

	// A bullet passes through solid scenery that cannot be shot (Doom's lamps
	// and columns); a projectile bursts on it.
	// Every line starts from the drawn place (TRF_ABSPOSITION): x, y there,
	// the height above the pawn's feet as the shot takes it; 'side' moves it
	// that far to the right of the line.
	const double feet = mo->Z() - mo->Floorclip;
	// Where the lines start: the drawn body at the shot's height, or the gun.
	DVector3 start(drawnPos, feet + sz);
	DVector3 gun;
	const bool fromGun = !melee && DoomVR_DrawnGunStart(mo, drawnYaw, drawnEye, gun);
	if (fromGun) start = gun;
	auto trace = [&](int flags, double zUp, double side, FLineTraceData& out)
	{
		const DVector2 at = start.XY() + DVector2(yaw.Sin(), -yaw.Cos()) * side;
		P_LineTrace(mo, yaw, range, pitch, flags | TRF_ABSPOSITION, start.Z + zUp, at.X, at.Y, &out);
	};
	FLineTraceData d;
	const bool missile = shot.ranged == DoomVRRanged::Missile;
	const int traceFlags = missile ? TRF_SOLIDACTORS : 0;
	const AActor* def = missile && shot.missile ? GetDefaultByType(shot.missile) : nullptr;
	if (def == nullptr)
	{
		trace(traceFlags, 0., 0., d);
		if (d.HitType == TRACE_HitNone || d.HitType == TRACE_HitSky) return false;
		hit = d.HitLocation;
		return true;
	}
	// A projectile is as wide as its radius and as tall as its height, and
	// bursts on the first thing any of it touches: a wall corner a thin line
	// slips past by a unit stops a rocket 80 units out (Doom II MAP01's
	// start). So a line down its middle, one each side and one above and
	// below, and the dot where the first of them is stopped - on the middle
	// line, where the projectile is when it bursts.
	const double r = std::max(1., def->radius), h = std::max(1., def->Height);
	// The projectile's middle above where it starts: from the gun it starts
	// centred on the hand (P_SpawnPlayerMissile).
	const double zc = fromGun ? 0. : h * 0.5;
	const DVector3 dir(DVector2(yaw.Cos(), yaw.Sin()) * pitch.Cos(), -pitch.Sin());
	const double offs[5][2] = { { 0, 0 }, { r, 0 }, { -r, 0 }, { 0, h * 0.5 }, { 0, -h * 0.5 } };
	double nearest = -1;
	bool centreHit = false;
	for (int i = 0; i < 5; ++i)
	{
		FLineTraceData t;
		trace(traceFlags, zc + offs[i][1], offs[i][0], t);
		if (t.HitType == TRACE_HitNone) continue;
		if (i == 0) { d = t; centreHit = t.HitType != TRACE_HitSky; }
		if (nearest < 0 || t.Distance < nearest) nearest = t.Distance;
	}
	if (nearest < 0) return false;
	if (centreHit && nearest >= d.Distance - 0.5) { hit = d.HitLocation; return true; }
	// Stopped by an edge before the middle line reaches anything: the burst is
	// in the air beside that edge. A sky ahead of the middle line with an edge
	// stopping it first still bursts there.
	hit = start + DVector3(0, 0, zc) + dir * nearest;
	return true;
}

DEFINE_ACTION_FUNCTION(APlayerPawn, DoomVRAim)
{
	PARAM_PROLOGUE;
	double yaw = 0, pitch = 0;
	const bool ok = !vr_shots_from_hand && DoomVR_GetAim(yaw, pitch);
	ACTION_RETURN_VEC3(DVector3(ok ? yaw : 0., ok ? pitch : 0., ok ? 1. : 0.));
}

// In the headset the gun is in your hand: switching puts the next one there at
// once, with no dip out of view and back.
CVAR(Bool, vr_instant_switch, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

DEFINE_ACTION_FUNCTION(APlayerPawn, DoomVRInstantSwitch)
{
	PARAM_PROLOGUE;
	ACTION_RETURN_BOOL(vr_instant_switch && DoomVR_Active());
}

// DoomVR: in the headset now (the death camera keeps the player's own heading).
DEFINE_ACTION_FUNCTION(APlayerPawn, DoomVRHeadset)
{
	PARAM_PROLOGUE;
	ACTION_RETURN_BOOL(DoomVR_Active());
}

DEFINE_ACTION_FUNCTION(APlayerPawn, DoomVRLook)
{
	PARAM_PROLOGUE;
	double yaw = 0;
	const bool ok = DoomVR_GetLookYaw(yaw);
	ACTION_RETURN_VEC3(DVector3(ok ? yaw : 0., 0., ok ? 1. : 0.));
}

DEFINE_ACTION_FUNCTION(APlayerPawn, CheckMusicChange)
{
	PARAM_SELF_PROLOGUE(AActor);
	P_CheckMusicChange(self->player);
	return 0;
}

//----------------------------------------------------------------------------
//
// PROC P_CheckEnviroment
//
//----------------------------------------------------------------------------

void P_CheckEnvironment(player_t *player)
{
	if (player->mo->Vel.Z <= -player->mo->FloatVar(NAME_FallingScreamMinSpeed) &&
		player->mo->Vel.Z >= -player->mo->FloatVar(NAME_FallingScreamMaxSpeed) && player->mo->alternative == nullptr &&
		player->mo->waterlevel == 0)
	{
		auto id = S_FindSkinnedSound(player->mo, S_FindSound("*falling"));
		if (id != NO_SOUND && !S_IsActorPlayingSomething(player->mo, CHAN_VOICE, id))
		{
			S_Sound(player->mo, CHAN_VOICE, 0, id, 1, ATTN_NORM);
		}
	}
}

DEFINE_ACTION_FUNCTION(APlayerPawn, CheckEnvironment)
{
	PARAM_SELF_PROLOGUE(AActor);
	P_CheckEnvironment(self->player);
	return 0;
}

//----------------------------------------------------------------------------
//
// PROC P_CheckUse
//
//----------------------------------------------------------------------------

void P_CheckUse(player_t *player)
{
	// check for use
	if (player->cmd.buttons & BT_USE)
	{
		if (!player->usedown)
		{
			player->usedown = true;
			if (!P_TalkFacing(player->mo))
			{
				P_UseLines(player);
			}
		}
	}
	else
	{
		player->usedown = false;
	}
}

DEFINE_ACTION_FUNCTION(APlayerPawn, CheckUse)
{
	PARAM_SELF_PROLOGUE(AActor);
	P_CheckUse(self->player);
	return 0;
}

void FSafePosition::Update(AActor& mobj, bool force)
{
	FSafePosition test;
	test.Sector = mobj.Sector;
	test.Pos = mobj.Pos();
	test.Height = mobj.IsKindOf(NAME_PlayerPawn) ? mobj.FloatVar(NAME_FullHeight) : mobj.GetDefault()->Height;
	test.MaxStepHeight = mobj.MaxStepHeight;
	test.bValidPos = mobj.Sector != nullptr
					&& ((!(mobj.flags2 & MF2_ONMOBJ) && mobj.Z() <= mobj.floorz && mobj.Z() - mobj.dropoffz <= mobj.MaxDropOffHeight) || mobj.waterlevel >= 3);

	if (force || test.IsSafe(mobj.tid))
		memcpy(this, &test, sizeof(FSafePosition));
}

bool FSafePosition::IsSafe(int tid) const
{
	// These need to be validated in real-time as the sector itself could've changed, leading to a valid
	// position no longer being safe.
	return bValidPos
			&& NextHighestCeilingAt(Sector, Pos.X, Pos.Y, Pos.Z, Pos.Z + Height) - Pos.Z >= Height
			&& !Sector->IsDangerous(Pos, Height, tid);
}

FSerializer& FSafePosition::Serialize(FSerializer& arc, const char* name)
{
	if (arc.BeginObject(name))
	{
		arc("sector", Sector)
			("pos", Pos)
			("height", Height)
			("maxstepheight", MaxStepHeight)
			("bvalidpos", bValidPos);

		arc.EndObject();
	}

	return arc;
}

//----------------------------------------------------------------------------
//
// PROC P_PlayerThink
//
//----------------------------------------------------------------------------

// DoomVR: the hands, for VR mods - AttackPos/Angle/Pitch/Roll (the weapon
// hand) and Offhand* on the pawn, OverrideAttackPosDir when they come from
// controllers. The conventions are the ones the mods were written for,
// measured (tools\hand-probe.py): Angle is the hand's world yaw minus 90,
// Pitch is up-positive, both through a 30-degree weapon tilt. A hand with
// no controller - and every hand with no headset - is the pawn's own
// attack height and facing.
EXTERN_CVAR(Float, vr_hunits_per_meter)
EXTERN_CVAR(Float, vr_height_adjust)
// DoomVR: room-scale - the body walks where the head walks (vr_room_scale).
// Once a tic, the head's walk the body has not yet carried is tried as a
// move with the game's own collision, in steps no longer than 8 units (a
// move checks only where it ends, so one long jump - after a menu, or a
// tracking glitch - could pass a thin wall) and no more than 64 a tic; a
// blocked step tries each axis alone, so the body slides along a wall. What
// it managed is reported back, and the eyes and hands show only the rest:
// walk into a wall and the body stops there. Single player only - the head
// is local, and a move made from it would desync a netgame or a demo.
EXTERN_CVAR(Float, vr_hunits_per_meter)
static void DoomVR_RoomWalk(player_t* player)
{
	if (player != &players[consoleplayer] || netgame || demoplayback || demorecording) return;
	AActor* mo = player->mo;
	double right, forward;
	if (!DoomVR_RoomPending(right, forward)) return;
	if (mo->health <= 0 || (player->cheats & CF_TOTALLYFROZEN) || mo->isFrozen()) return;
	const double u = vr_hunits_per_meter;
	const double t = mo->Angles.Yaw.Radians(), c = cos(t), s = sin(t);
	double dx = (forward * c + right * s) * u;
	double dy = (forward * s - right * c) * u;
	double len = sqrt(dx * dx + dy * dy);
	if (len < 0.25) return;
	if (len > 64.0) { dx *= 64.0 / len; dy *= 64.0 / len; len = 64.0; }
	const int steps = int(ceil(len / 8.0));
	const DVector2 step(dx / steps, dy / steps);
	double mx = 0, my = 0;
	for (int i = 0; i < steps; ++i)
	{
		const DVector2 at = mo->Pos().XY();
		if (P_TryMove(mo, at + step, true)) { mx += step.X; my += step.Y; }
		else if (step.X != 0 && P_TryMove(mo, at + DVector2(step.X, 0), true)) mx += step.X;
		else if (step.Y != 0 && P_TryMove(mo, at + DVector2(0, step.Y), true)) my += step.Y;
		else break;
	}
	if (mx == 0 && my == 0) return;
	// Back into the recenter frame: forward = d . facing, right = d . (sin, -cos).
	DoomVR_RoomCarry((mx * s - my * c) / u, (mx * c + my * s) / u);
}

/*
	Physical play, using things by hand: a hand
	within 6 units (about 15 cm) of a usable line - a switch, a door - presses
	it, through the game's own line activation from the side the hand is on,
	once per touch (the hand must draw back past 12 units before that line
	answers it again). The use button stays. On by default
	(vr_phys_use). Hands come from DoomVR_SetHands' own positions (the tracked
	hands, in the map).

	A second press on a moving door turns it back, and walking up to a door
	puts a hand on it before the player reaches for the button. So a line a
	hand has just pressed ignores the other hand and the use button for a
	moment (kHandPressHold tics, about a second and a half).
*/
CVAR(Bool, vr_phys_use, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
static const int kHandPressHold = 52;
static const line_t* handPressedLine[2];
static int handPressedAt[2];
bool DoomVR_HandJustPressed(const line_t* line, int maptime)
{
	for (int hand = 0; hand < 2; ++hand)
		if (handPressedLine[hand] == line && maptime >= handPressedAt[hand] && maptime - handPressedAt[hand] < kHandPressHold)
			return true;
	return false;
}
static void DoomVR_TouchUse(player_t* player, int hand, const DVector3& pos)
{
	static line_t* touching[2];
	AActor* mo = player->mo;
	FBoundingBox box(pos.X, pos.Y, 12.);
	FBlockLinesIterator it(mo->Level, box);
	line_t* best = nullptr;
	double bestDist = 1e9;
	bool stillNear = false;
	while (line_t* ld = it.Next())
	{
		const DVector2 a = ld->v1->fPos(), d = ld->Delta();
		const double len2 = d.LengthSquared();
		if (len2 <= 0) continue;
		const double t = std::clamp(((pos.XY() - a) | d) / len2, 0.0, 1.0);
		const double dist = (pos.XY() - (a + d * t)).Length();
		if (ld == touching[hand] && dist < 12.) stillNear = true;
		if (ld->special == 0 || !(ld->activation & (SPAC_Use | SPAC_UseThrough))) continue;
		// The hand must be at a height the wall has there, below the ceiling
		// and above the floor of the side it is on.
		const int side = P_PointOnLineSide(pos.XY(), ld);
		sector_t* sec = side == 0 ? ld->frontsector : ld->backsector;
		if (sec == nullptr || pos.Z < sec->floorplane.ZatPoint(pos.XY()) || pos.Z > sec->ceilingplane.ZatPoint(pos.XY())) continue;
		if (dist < 6. && dist < bestDist) { best = ld; bestDist = dist; }
	}
	if (!stillNear) touching[hand] = nullptr;
	if (best == nullptr || best == touching[hand]) return;
	touching[hand] = best;
	if (DoomVR_HandJustPressed(best, mo->Level->maptime)) return;
	const int side = P_PointOnLineSide(pos.XY(), best);
	DVector3 at = pos;
	if (P_ActivateLine(best, mo, side, SPAC_Use, &at))
	{
		handPressedLine[hand] = best;
		handPressedAt[hand] = mo->Level->maptime;
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: %s hand pressed line %d (special %d)\n", hand == 0 ? "weapon" : "other", best->Index(), best->special);
	}
}

static void DoomVR_SetHands(player_t* player)
{
	AActor* mo = player->mo;
	const bool local = player == &players[consoleplayer];
	const double u = vr_hunits_per_meter;
	const double yaw = mo->Angles.Yaw.Degrees();
	const double eyez = player->viewz;
	static const FName names[2][4] = {
		{ "AttackPos", "AttackAngle", "AttackPitch", "AttackRoll" },
		{ "OffhandPos", "OffhandAngle", "OffhandPitch", "OffhandRoll" } };
	bool any = false;
	for (int hand = 0; hand < 2; ++hand)
	{
		DVector3 pos;
		double a, p, r;
		double right, up, forward, hy, hp, hr;
		const bool tracked = local && DoomVR_HandPose(hand, right, up, forward, hy, hp, hr);
		static const FName trackedNames[2] = { "DoomVRAttackTracked", "DoomVROffhandTracked" };
		mo->BoolVar(trackedNames[hand]) = tracked;
		if (tracked)
		{
			const double t = yaw * (M_PI / 180.0);
			pos = mo->Pos();
			pos.X += (forward * cos(t) + right * sin(t)) * u;
			pos.Y += (forward * sin(t) - right * cos(t)) * u;
			pos.Z = eyez + (up + vr_height_adjust) * u;
			a = yaw + hy - 90.0;
			p = hp;
			r = hr;
			any = true;
		}
		else
		{
			pos = mo->Pos() + DVector3(0, 0, mo->Height * 0.5 - mo->Floorclip + player->mo->FloatVar(FName("AttackZOffset")) * player->crouchfactor);
			a = yaw - 90.0;
			p = -mo->Angles.Pitch.Degrees();
			r = 0;
		}
		*(DVector3*)mo->ScriptVar(names[hand][0], nullptr) = pos;
		mo->FloatVar(names[hand][1]) = a;
		mo->FloatVar(names[hand][2]) = p;
		mo->FloatVar(names[hand][3]) = r;
	}
	mo->BoolVar("OverrideAttackPosDir") = any;
}

// DoomVR: controller pulses for the local player (see P_PlayerThink).
static void DoomVR_PlayerFeedback(player_t* player)
{
	// Only compared, never followed: a weapon swap is not a shot.
	static const AActor* lastWeapon = nullptr;
	static int lastAmmo[2] = { -1, -1 };
	static uint32_t lastDamage = 0;

	AActor* weapon = player->ReadyWeapon;
	int ammo[2] = { -1, -1 };
	if (weapon != nullptr)
	{
		AActor* a1 = weapon->PointerVar<AActor>(NAME_Ammo1);
		AActor* a2 = weapon->PointerVar<AActor>(NAME_Ammo2);
		if (a1 != nullptr) ammo[0] = a1->IntVar(NAME_Amount);
		if (a2 != nullptr && a2 != a1) ammo[1] = a2->IntVar(NAME_Amount);
	}
	if (weapon != nullptr && weapon == lastWeapon &&
	    ((ammo[0] >= 0 && ammo[0] < lastAmmo[0]) || (ammo[1] >= 0 && ammo[1] < lastAmmo[1])))
		DoomVR_Pulse(2, 0.06f, 0.8f, "shot");
	lastWeapon = weapon;
	lastAmmo[0] = ammo[0];
	lastAmmo[1] = ammo[1];

	// damagecount is set from the damage taken (up to 100) and counts down;
	// it rising is a new hit. Harder hits rumble harder and longer.
	if (player->damagecount > lastDamage)
	{
		const float hurt = std::min(1.f, float(player->damagecount - lastDamage) / 50.f);
		DoomVR_Pulse(3, 0.1f + 0.2f * hurt, 0.4f + 0.6f * hurt, "hurt");
	}
	lastDamage = player->damagecount;
}

void P_PlayerThink (player_t *player)
{
	usercmd_t *cmd = &player->cmd;

	if (player->mo == NULL)
	{
		I_Error ("No player %td start\n", player - players + 1);
	}

	for (unsigned int i = 0u; i < 3u; ++i)
	{
		if (fabs(player->angleOffsetTargets[i].Degrees()) >= EQUAL_EPSILON)
		{
			player->mo->Angles[i] += player->angleOffsetTargets[i];
			player->mo->PrevAngles[i] = player->mo->Angles[i];
		}

		player->angleOffsetTargets[i] = nullAngle;
	}

	// TODO: Should this be moved to the client-side logic like inventory tics?
	if (player->SubtitleCounter > 0)
	{
		player->SubtitleCounter--;
	}

	if (player->playerstate == PST_LIVE)
		player->LastSafePos.Update(*player->mo);

	++player->BobTimer;

	// Bots do not think in freeze mode.
	if (player->mo->Level->isFrozen() && player->Bot != nullptr)
	{
		return;
	}

	if (debugfile && !(player->cheats & CF_PREDICTING))
	{
		fprintf (debugfile, "tic %d for pl %d: (%f, %f, %f, %f) b:%02x p:%d y:%d f:%d s:%d u:%d\n",
			gametic, (int)(player-players), player->mo->X(), player->mo->Y(), player->mo->Z(),
			player->mo->Angles.Yaw.Degrees(), player->cmd.buttons,
			player->cmd.pitch, player->cmd.yaw, player->cmd.forwardmove,
			player->cmd.sidemove, player->cmd.upmove);
	}

	// Make unmodified copies for ACS's GetPlayerInput.
	player->original_oldbuttons = player->original_cmd.buttons;
	player->original_cmd = *cmd;
	// Don't interpolate the view for more than one tic
	player->cheats &= ~CF_INTERPVIEW;
	player->cheats &= ~CF_INTERPVIEWANGLES;
	player->cheats &= ~CF_SCALEDNOLERP;
	player->cheats &= ~CF_NOFOVINTERP;
	player->cheats &= ~CF_NOVIEWPOSINTERP;
	player->mo->FloatVar("prevBob") = player->bob;

	if (player->mo != nullptr) DoomVR_RoomWalk(player);
	if (player->mo != nullptr) DoomVR_SetHands(player);
	if (player->mo != nullptr && vr_phys_use && player == &players[consoleplayer] && player->playerstate == PST_LIVE)
		for (int hand = 0; hand < 2; ++hand)
			if (player->mo->BoolVar(hand == 0 ? FName("DoomVRAttackTracked") : FName("DoomVROffhandTracked")))
				DoomVR_TouchUse(player, hand, *(DVector3*)player->mo->ScriptVar(hand == 0 ? FName("AttackPos") : FName("OffhandPos"), nullptr));

	IFVIRTUALPTRNAME(player->mo, NAME_PlayerPawn, PlayerThink)
	{
		VMValue param = player->mo;
		VMCall(func, &param, 1, nullptr, 0);
	}

	// DoomVR: the headset player's eyes do not bob unless vr_view_bob is on.
	// Done here, after the pawn's own think, rather than in the stock
	// CalcHeight: mods replace the player class and its CalcHeight (DOOM 64
	// CE does), and the bob came back with them. The eye height is the
	// stock one without the bob - body, view height (crouching, landing),
	// floor clip, and the same 4-unit margins from floor and ceiling.
	if (player == &players[consoleplayer] && player->mo != nullptr && DoomVR_ViewBob() == 0.0)
	{
		AActor* mo = player->mo;
		double z = mo->Z() + player->viewheight;
		if (mo->Floorclip && player->playerstate != PST_DEAD && mo->Z() <= mo->floorz)
			z -= mo->Floorclip;
		if (z > mo->ceilingz - 4) z = mo->ceilingz - 4;
		if (z < mo->floorz + 4) z = mo->floorz + 4;
		player->viewz = z;
	}

	// DoomVR: crouching for real - the head is already down, so the game's
	// crouch lowers the body, not the eyes a second time (crouchoffset is
	// the view drop the crouch made; it is zero or less).
	if (player == &players[consoleplayer] && player->mo != nullptr && player->crouchoffset < 0 && DoomVR_RealCrouch())
		player->viewz = std::min(player->viewz - player->crouchoffset, player->mo->ceilingz - 4);

	// DoomVR: the controllers answer what this tic did - ammo spent is a
	// shot, a rise in the damage flash is a hit taken. Read from the result
	// rather than from the weapons' code, so a mod's weapons, and Heretic's
	// and Hexen's mana weapons, kick the same as Doom's.
	if (player == &players[consoleplayer] && player->mo != nullptr && !NetworkEntityManager::IsPredicting())
		DoomVR_PlayerFeedback(player);

	if (BobType == PSPB_2D)
		P_BobWeapon(player);
	else if (BobType == PSPB_3D)
		P_BobWeapon3D(player);

	// Moved this to directly after player thinking to get more accurate velocity values. Also takes
	// 3D vs 2D movement into account now.
	if (!NetworkEntityManager::IsPredicting() && player->mo != nullptr)
	{
		double spd = (player->mo->flags & MF_NOGRAVITY) ? player->mo->Vel.Length() : player->mo->Vel.XY().Length();
		player->mo->Level->velocities[player - players].SetVelocity(spd);
	}
}

void P_PredictionLerpReset()
{
	PredictionData.ResetPos();
}

void P_LerpCalculate(AActor* pmo, const DVector3& from, DVector3 &result, float scale, float threshold, float minMove)
{
	DVector3 diff = pmo->Pos() - from;
	diff.XY() += pmo->Level->Displacements.getOffset(pmo->Sector->PortalGroup, pmo->Level->PointInSector(from.XY())->PortalGroup);
	double dist = diff.Length();
	if (dist <= max<float>(threshold, minMove))
	{
		result = pmo->Pos();
		return;
	}

	diff /= dist;
	diff *= min<double>(dist * (1.0f - scale), dist - minMove);
	result = pmo->Vec3Offset(-diff.X, -diff.Y, -diff.Z);
}

void P_MarkRollbackObjects()
{
	PredictionData.Mark();
}

void P_ClearPredictionData()
{
	NetworkEntityManager::DisablePrediction();
	PredictionData.ResetPos();
	PredictionData.ClearBackup();
	PredictionData.bResetPrediction = false;
	PredictionData.LastPredictedTic = 0;
}

static void P_RollbackObject(DObject* obj, FSerializer& arc)
{
	if (!arc.MarkRollbackObject(obj))
		return;

	auto act = dyn_cast<AActor>(obj);
	if (act != nullptr)
	{
		PredictionData.RollbackActors.Push(FActorBackup{ *act });
		if (act->player != nullptr && act->player->mo == act)
			PredictionData.RollbackPlayers.Push(act->player - players);

		// TODO: In the future these will be automatically handled by the net owner system, but handle them
		// manually for now to increase stability.
		P_RollbackObject(act->ViewPos, arc);
		P_RollbackObject(act->modelData, arc);
	}
	else
	{
		PredictionData.RollbackObjects.Push({ *obj });
	}
}

static void P_RollbackPlayers(FSerializer& arc)
{
	if (!PredictionData.RollbackPlayers.Size() || !arc.BeginArray("players"))
		return;

	for (auto p : PredictionData.RollbackPlayers)
	{
		if (arc.BeginObject(nullptr))
		{
			players[p].Serialize(arc);
			arc.EndObject();
		}
	}

	arc.EndArray();
}

void P_PredictClient()
{
	if (gamestate != GS_LEVEL)
		return;

	player_t* player = &players[consoleplayer];
	if (!NetworkEntityManager::IsPredicting())
	{
		NetworkEntityManager::EnablePrediction();
		PredictionData.bResetPrediction = true;
		PredictionData.LastPredictedTic = gametic;

		FDoomSerializer writer = { player->mo->Level };
		if (writer.OpenWriter(false, true))
		{
			FRandom::RollbackRNGState(writer);
			P_RollbackObject(player->mo, writer);
			P_RollbackPlayers(writer);
			TArray<DObject*> fullRollback = {};
			for (auto& a : PredictionData.RollbackActors)
				fullRollback.Push(a.GetObject<DObject>());
			for (auto& o : PredictionData.RollbackObjects)
				fullRollback.Push(o.GetObject<DObject>());
			PredictionData.RollbackData = writer.GetOutput(nullptr, &PredictionData.RollbackObjectRefs, &fullRollback);
			PredictionData.RollbackLevel = player->mo->Level;
			PredictionData.RollbackWriterBuffer = writer.CloseAndGetBuffer();

			for (auto& a : PredictionData.RollbackActors)
				a.PostBackup();
			for (auto o : fullRollback)
				o->ObjectFlags |= OF_Predicting;
		}
	}

	player->cheats |= CF_PREDICTING; // This is only here for backwards compat.
	if (ClientTic <= PredictionData.LastPredictedTic || player->playerstate != PST_LIVE || (player->mo->ObjectFlags & OF_JustSpawned))
		return;

	// This essentially acts like a mini P_Ticker where only the stuff relevant to the client is actually
	// called. Call order is preserved.
	bool rubberband = false, rubberbandLimit = false;
	DVector3 rubberbandPos = {};
	const bool canRubberband = PredictionData.bResetPrediction && PredictionData.LastPos.Tic >= 0 && cl_rubberband_scale > 0.0f && cl_rubberband_scale < 1.0f;
	const double rubberbandThreshold = max<float>(cl_rubberband_minmove, cl_rubberband_threshold);
	for (int i = PredictionData.LastPredictedTic; i < ClientTic; ++i)
	{
		// Make sure any portal paths have been cleared from the previous movement.
		R_ClearInterpolationPath();
		r_NoInterpolate = false;
		player->mo->renderflags &= ~RF_NOINTERPOLATEVIEW;

		// Got snagged on something. Start correcting towards the player's final predicted position. We're
		// being intentionally generous here by not really caring how the player got to that position, only
		// that they ended up in the same spot on the same tick.
		if (canRubberband && PredictionData.LastPos.Tic == i)
		{
			DVector3 diff = player->mo->Pos() - PredictionData.LastPos.Pos;
			diff += player->mo->Level->Displacements.getOffset(player->mo->Sector->PortalGroup, PredictionData.LastPos.PortalGroup);
			double dist = diff.LengthSquared();
			if (dist >= EQUAL_EPSILON * EQUAL_EPSILON && dist > rubberbandThreshold * rubberbandThreshold)
			{
				rubberband = true;
				rubberbandPos = player->mo->Pos();
				rubberbandLimit = cl_rubberband_limit > 0.0f && dist > cl_rubberband_limit * cl_rubberband_limit;
			}
		}

		player->oldbuttons = player->cmd.buttons;
		player->cmd = LocalCmds[i % LOCALCMDTICS];
		if (paused)
			continue;

		player->mo->ClearInterpolation();
		player->mo->ClearFOVInterpolation();
		P_PlayerThink(player);
		player->mo->CallTick();
	}

	if (rubberband)
	{
		DPrintf(DMSG_NOTIFY, "Prediction mismatch at (%.3f, %.3f, %.3f)\nExpected: (%.3f, %.3f, %.3f)\nCorrecting to (%.3f, %.3f, %.3f)\n",
			PredictionData.LastPos.Pos.X, PredictionData.LastPos.Pos.Y, PredictionData.LastPos.Pos.Z,
			rubberbandPos.X, rubberbandPos.Y, rubberbandPos.Z,
			player->mo->X(), player->mo->Y(), player->mo->Z());

		if (rubberbandLimit)
		{
			// If too far away, instantly snap the player's view to their correct position.
			player->mo->renderflags |= RF_NOINTERPOLATEVIEW;
		}
		else
		{
			R_ClearInterpolationPath();
			player->mo->renderflags &= ~RF_NOINTERPOLATEVIEW;

			DVector3 snapPos = {};
			P_LerpCalculate(player->mo, PredictionData.LastPos.Pos, snapPos, cl_rubberband_scale, cl_rubberband_threshold, cl_rubberband_minmove);
			player->mo->PrevPortalGroup = PredictionData.LastPos.PortalGroup;
			player->mo->Prev = PredictionData.LastPos.Pos;
			const double zOfs = player->viewz - player->mo->Z();
			player->mo->SetXYZ(snapPos);
			player->viewz = snapPos.Z + zOfs;
		}
	}
	else if (paused)
	{
		r_NoInterpolate = true;
	}

	PredictionData.LastPredictedTic = ClientTic;
	PredictionData.bResetPrediction = false;

	// This is intentionally done after rubberbanding starts since it'll automatically smooth itself towards
	// the right spot until it reaches it.
	PredictionData.LastPos.Tic = ClientTic;
	PredictionData.LastPos.Pos = player->mo->Pos();
	PredictionData.LastPos.PortalGroup = player->mo->Level->PointInSector(PredictionData.LastPos.Pos)->PortalGroup;
}

void P_UnPredictClient()
{
	if (!NetworkEntityManager::IsPredicting())
		return;

	NetworkEntityManager::DisablePrediction();

	FDoomSerializer reader = { PredictionData.RollbackLevel };
	if (reader.OpenReader(PredictionData.RollbackReaderAllocator, PredictionData.RollbackData.data(), PredictionData.RollbackData.size(), true))
	{
		for (auto& a : PredictionData.RollbackActors)
			a.PreRollback();

		FRandom::RollbackRNGState(reader);
		reader.ReadObjectsFrom(PredictionData.RollbackObjectRefs);
		if (reader.mObjectErrors)
			I_Error("Failed to rollback game state");
		P_RollbackPlayers(reader);
		reader.Close();

		TArray<DObject*> fullRollback = {};
		for (auto& a : PredictionData.RollbackActors)
		{
			a.PostRollback();
			fullRollback.Push(a.GetObject<DObject>());
		}
		for (auto& o : PredictionData.RollbackObjects)
			fullRollback.Push(o.GetObject<DObject>());
		for (auto o : fullRollback)
		{
			if (o != nullptr)
				o->ObjectFlags &= ~OF_Predicting;
		}
	}

	PredictionData.ClearBackup();
}

void player_t::Serialize(FSerializer &arc)
{
	FString skinname;

	arc("class", cls)
		("mo", mo)
		("playerstate", playerstate)
		("cmd", cmd);

	if (!arc.IsRollback())
	{
		arc("camera", camera)
			("inventorytics", inventorytics)
			("settings_controller", settings_controller);

		if (arc.isReading())
		{
			userinfo.Reset(mo->Level->PlayerNum(this));
			ReadUserInfo(arc, userinfo, skinname);
		}
		else
		{
			WriteUserInfo(arc, userinfo);
		}
	}
	else
	{
		arc("attackdown", attackdown)
			("usedown", usedown);
	}

	int fbmode = FullbrightMode;
	arc("desiredfov", DesiredFOV)
		("fov", FOV)
		("viewz", viewz)
		("viewheight", viewheight)
		("deltaviewheight", deltaviewheight)
		("bob", bob)
		("bobtimer", BobTimer)
		("vel", Vel)
		("centering", centering)
		("health", health)
		("fragcount", fragcount)
		("spreecount", spreecount)
		("multicount", multicount)
		("lastkilltime", lastkilltime)
		("readyweapon", ReadyWeapon)
		("offhandweapon", OffhandWeapon)
		("pendingweapon", PendingWeapon)
		("cheats", cheats)
		("refire", refire)
		("killcount", killcount)
		("itemcount", itemcount)
		("secretcount", secretcount)
		("damagecount", damagecount)
		("bonuscount", bonuscount)
		("hazardcount", hazardcount)
		("poisoncount", poisoncount)
		("poisoner", poisoner)
		("attacker", attacker)
		("extralight", extralight)
		("fixedcolormap", fixedcolormap)
		("fixedlightlevel", fixedlightlevel)
		("fullbrightmode", fbmode)
		("bforcefullbright", bForceFullbright)
		("morphTics", morphTics)
		("morphedplayerclass", MorphedPlayerClass)
		("morphstyle", MorphStyle)
		("morphexitflash", MorphExitFlash)
		("premorphweapon", PremorphWeapon)
		("chickenpeck", chickenPeck)
		("jumptics", jumpTics)
		("respawntime", respawn_time)
		("airfinished", air_finished)
		("turnticks", turnticks)
		("oldbuttons", oldbuttons)
		("hazardtype", hazardtype)
		("hazardinterval", hazardinterval)
		("bot", Bot)
		("blendr", BlendR)
		("blendg", BlendG)
		("blendb", BlendB)
		("blenda", BlendA)
		("weaponstate", WeaponState)
		("logtext", LogText)
		("subtitletext", SubtitleText)
		("subtitlecounter", SubtitleCounter)
		("conversionnpc", ConversationNPC)
		("conversionpc", ConversationPC)
		("conversionnpcangle", ConversationNPCAngle)
		("conversionfacetalker", ConversationFaceTalker)
		.Array("frags", frags, MAXPLAYERS)
		("psprites", psprites)
		("currentplayerclass", CurrentPlayerClass)
		("crouchfactor", crouchfactor)
		("crouching", crouching)
		("crouchdir", crouchdir)
		("crouchoffset", crouchoffset)
		("crouchviewdelta", crouchviewdelta)
		("original_cmd", original_cmd)
		("original_oldbuttons", original_oldbuttons)
		("poisontype", poisontype)
		("poisonpaintype", poisonpaintype)
		("timefreezer", timefreezer)
		("onground", onground)
		("musinfoactor", MUSINFOactor)
		("musinfotics", MUSINFOtics)
		("soundclass", SoundClass)
		("angleoffsettargets", angleOffsetTargets)
		("lastdamagetype", LastDamageType);
		// Uses a slightly different name since the type was changed, otherwise it would
		// error out on loading older saves.
		LastSafePos.Serialize(arc, "safepos");

	if (!arc.IsRollback())
	{
		if (arc.isWriting())
		{
			// If the player reloaded because they pressed +use after dying, we
			// don't want +use to still be down after the game is loaded.
			oldbuttons = ~0;
			original_oldbuttons = ~0;
		}
		else
		{
			FullbrightMode = static_cast<EFullbrightMode>(fbmode);
		}
		if (skinname.IsNotEmpty())
		{
			userinfo.SkinChanged(skinname.GetChars(), CurrentPlayerClass);
		}
	}
}

bool P_IsPlayerTotallyFrozen(const player_t *player)
{
	return
		gamestate == GS_TITLELEVEL ||
		player->cheats & CF_TOTALLYFROZEN ||
		player->mo->isFrozen();
}

//==========================================================================
//
// native members
//
//==========================================================================

DEFINE_FIELD_X(PlayerInfo, player_t, mo)
DEFINE_FIELD_X(PlayerInfo, player_t, playerstate)
DEFINE_FIELD_X(PlayerInfo, player_t, original_oldbuttons)
DEFINE_FIELD_X(PlayerInfo, player_t, cls)
DEFINE_FIELD_X(PlayerInfo, player_t, DesiredFOV)
DEFINE_FIELD_X(PlayerInfo, player_t, FOV)
DEFINE_FIELD_X(PlayerInfo, player_t, viewz)
DEFINE_FIELD_X(PlayerInfo, player_t, viewheight)
DEFINE_FIELD_X(PlayerInfo, player_t, deltaviewheight)
DEFINE_FIELD_X(PlayerInfo, player_t, bob)
DEFINE_FIELD_X(PlayerInfo, player_t, BobTimer)
DEFINE_FIELD_X(PlayerInfo, player_t, Vel)
DEFINE_FIELD_X(PlayerInfo, player_t, centering)
DEFINE_FIELD_X(PlayerInfo, player_t, turnticks)
DEFINE_FIELD_X(PlayerInfo, player_t, attackdown)
DEFINE_FIELD_X(PlayerInfo, player_t, usedown)
DEFINE_FIELD_X(PlayerInfo, player_t, oldbuttons)
DEFINE_FIELD_X(PlayerInfo, player_t, health)
DEFINE_FIELD_X(PlayerInfo, player_t, inventorytics)
DEFINE_FIELD_X(PlayerInfo, player_t, CurrentPlayerClass)
DEFINE_FIELD_X(PlayerInfo, player_t, frags)
DEFINE_FIELD_X(PlayerInfo, player_t, fragcount)
DEFINE_FIELD_X(PlayerInfo, player_t, lastkilltime)
DEFINE_FIELD_X(PlayerInfo, player_t, multicount)
DEFINE_FIELD_X(PlayerInfo, player_t, spreecount)
DEFINE_FIELD_X(PlayerInfo, player_t, WeaponState)
DEFINE_FIELD_X(PlayerInfo, player_t, ReadyWeapon)
DEFINE_FIELD_X(PlayerInfo, player_t, OffhandWeapon)
DEFINE_FIELD_X(PlayerInfo, player_t, PendingWeapon)
DEFINE_FIELD_X(PlayerInfo, player_t, psprites)
DEFINE_FIELD_X(PlayerInfo, player_t, cheats)
DEFINE_FIELD_X(PlayerInfo, player_t, timefreezer)
DEFINE_FIELD_X(PlayerInfo, player_t, refire)
DEFINE_FIELD_NAMED_X(PlayerInfo, player_t, inconsistant, inconsistent)
DEFINE_FIELD_X(PlayerInfo, player_t, waiting)
DEFINE_FIELD_X(PlayerInfo, player_t, killcount)
DEFINE_FIELD_X(PlayerInfo, player_t, itemcount)
DEFINE_FIELD_X(PlayerInfo, player_t, secretcount)
DEFINE_FIELD_X(PlayerInfo, player_t, damagecount)
DEFINE_FIELD_X(PlayerInfo, player_t, bonuscount)
DEFINE_FIELD_X(PlayerInfo, player_t, hazardcount)
DEFINE_FIELD_X(PlayerInfo, player_t, hazardinterval)
DEFINE_FIELD_X(PlayerInfo, player_t, hazardtype)
DEFINE_FIELD_X(PlayerInfo, player_t, poisoncount)
DEFINE_FIELD_X(PlayerInfo, player_t, poisontype)
DEFINE_FIELD_X(PlayerInfo, player_t, poisonpaintype)
DEFINE_FIELD_X(PlayerInfo, player_t, poisoner)
DEFINE_FIELD_X(PlayerInfo, player_t, attacker)
DEFINE_FIELD_X(PlayerInfo, player_t, extralight)
DEFINE_FIELD_X(PlayerInfo, player_t, fixedcolormap)
DEFINE_FIELD_X(PlayerInfo, player_t, fixedlightlevel)
DEFINE_FIELD_X(PlayerInfo, player_t, morphTics)
DEFINE_FIELD_X(PlayerInfo, player_t, MorphedPlayerClass)
DEFINE_FIELD_X(PlayerInfo, player_t, MorphStyle)
DEFINE_FIELD_X(PlayerInfo, player_t, MorphExitFlash)
DEFINE_FIELD_X(PlayerInfo, player_t, PremorphWeapon)
DEFINE_FIELD_X(PlayerInfo, player_t, chickenPeck)
DEFINE_FIELD_X(PlayerInfo, player_t, jumpTics)
DEFINE_FIELD_X(PlayerInfo, player_t, onground)
DEFINE_FIELD_X(PlayerInfo, player_t, respawn_time)
DEFINE_FIELD_X(PlayerInfo, player_t, camera)
DEFINE_FIELD_X(PlayerInfo, player_t, air_finished)
DEFINE_FIELD_X(PlayerInfo, player_t, LastDamageType)
DEFINE_FIELD_X(PlayerInfo, player_t, MUSINFOactor)
DEFINE_FIELD_X(PlayerInfo, player_t, MUSINFOtics)
DEFINE_FIELD_X(PlayerInfo, player_t, settings_controller)
DEFINE_FIELD_X(PlayerInfo, player_t, crouching)
DEFINE_FIELD_X(PlayerInfo, player_t, crouchdir)
DEFINE_FIELD_X(PlayerInfo, player_t, Bot)
DEFINE_FIELD_X(PlayerInfo, player_t, BlendR)
DEFINE_FIELD_X(PlayerInfo, player_t, BlendG)
DEFINE_FIELD_X(PlayerInfo, player_t, BlendB)
DEFINE_FIELD_X(PlayerInfo, player_t, BlendA)
DEFINE_FIELD_X(PlayerInfo, player_t, LogText)
DEFINE_FIELD_X(PlayerInfo, player_t, MinPitch)
DEFINE_FIELD_X(PlayerInfo, player_t, MaxPitch)
DEFINE_FIELD_X(PlayerInfo, player_t, crouchfactor)
DEFINE_FIELD_X(PlayerInfo, player_t, crouchoffset)
DEFINE_FIELD_X(PlayerInfo, player_t, crouchviewdelta)
DEFINE_FIELD_X(PlayerInfo, player_t, ConversationNPC)
DEFINE_FIELD_X(PlayerInfo, player_t, ConversationPC)
DEFINE_FIELD_X(PlayerInfo, player_t, ConversationNPCAngle)
DEFINE_FIELD_X(PlayerInfo, player_t, ConversationFaceTalker)
DEFINE_FIELD_NAMED_X(PlayerInfo, player_t, cmd, cmd)
DEFINE_FIELD_X(PlayerInfo, player_t, original_cmd)
DEFINE_FIELD_X(PlayerInfo, player_t, userinfo)
DEFINE_FIELD_X(PlayerInfo, player_t, weapons)
DEFINE_FIELD_NAMED_X(PlayerInfo, player_t, cmd.buttons, buttons)
DEFINE_FIELD_X(PlayerInfo, player_t, SoundClass)

DEFINE_FIELD_X(UserCmd, usercmd_t, buttons)
DEFINE_FIELD_X(UserCmd, usercmd_t, pitch)
DEFINE_FIELD_X(UserCmd, usercmd_t, yaw)
DEFINE_FIELD_X(UserCmd, usercmd_t, roll)
DEFINE_FIELD_X(UserCmd, usercmd_t, forwardmove)
DEFINE_FIELD_X(UserCmd, usercmd_t, sidemove)
DEFINE_FIELD_X(UserCmd, usercmd_t, upmove)

DEFINE_FIELD(FPlayerClass, Type)
DEFINE_FIELD(FPlayerClass, Flags)
DEFINE_FIELD(FPlayerClass, Skins)
