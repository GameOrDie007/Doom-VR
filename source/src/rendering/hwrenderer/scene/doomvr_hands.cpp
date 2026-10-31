/*
** doomvr_hands.cpp
**
** DoomVR: Game Or Die Hands - our own hands at the controllers, in Modern
** style, with the fingers following the trigger and the grip.
**
** vr_hands. The
** weapon hand is drawn while its gun is a 3D model, closed round it, the
** index finger on the trigger; with the fists out (Doom's Fist, Hexen's
** Fighter) the flat fist picture is put away and our hand is the fist, and
** with a spell out (Hexen's Cleric's flame, the Mage's frost and lightning -
** their first-person art is only the caster's hands) our hand is the open
** hand that casts it. Chex's Bootspoon is a kind of Fist but a spoon, and
** keeps its picture. The other hand is drawn while it holds nothing (a VR
** mod's off-hand weapon keeps its own art). Classic style stays flat: no
** hands.
**
** The hand models are ours: the Game Or Die Hands project's, from a CC0 base
** (runtime/gdh_hand.h, vendored beside this file). They are read from
** hands.pk3 beside the engine: doomvr/hands/hand_r.gdh, hand_l.gdh and a skin
** per game, doomvr/hands/skins/<family>.png - doom, heretic, hexen_fighter,
** hexen_cleric, hexen_mage, strife, chex - with doomvr/hands/skin.png standing
** in for the Doom games only. The skins are made on the player's PC from
** their own games and
** are never shipped. A game with no skin of its own gets no hands: Doomguy's
** hand on Corvus would be wrong. With no hands.pk3 nothing is drawn.
**
** Placement (doomvr_headset.cpp, DoomVR_HandsMatrix): a free hand or a fist
** at the controller's palm (the grip pose); a hand holding a gun on the gun's
** handle, turned with the gun (the aim pose the gun is drawn from).
**
** The hand is skinned on the CPU once a frame and drawn through the HUD model
** path, so it is lit as the gun is.
**
** Written for Doom VR.
**
** Copyright 2026 Game Or Die
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "v_video.h"
#include "doomstat.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "models.h"
#include "hw_weapon.h"
#include "texturemanager.h"
#include "filesystem.h"
#include "hw_models.h"
#include "hw_lighting.h"
#include "hw_cvars.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "flatvertices.h"
#include "hw_renderstate.h"
#include "doomvr_headset.h"
#include "gi.h"
#include "gdh_hand.h"
#include "v_font.h"
#include "sbar.h"
#include "i_time.h"
#include "a_weapons.h"
#include "c_dispatch.h"
#include "gameconfigfile.h"
#include "cmdlib.h"
#include <map>
#include <string>

// Doom VR ships with our hands and all of Physical Play on (each a switch in
// VR Options): the way it was played through its testing.
CVAR(Bool, vr_hands, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// The Doom games' hands (VR Options > Hands and Weapons): 0 the classic marine
// - padded fingerless gloves, as in Doom, Doom II and Doom 64 - 1 the Slayer's
// armored gloves. The
// other games keep their own hands.
CVAR(Int, vr_hands_style, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
EXTERN_CVAR(Int, vr_style)
EXTERN_CVAR(Bool, r_drawplayersprites)
EXTERN_CVAR(Bool, r_deathcamera)

namespace
{
	// One of our two hands, as a model the HUD model path can draw: its
	// vertex buffer is rewritten with the skinned hand once a frame.
	class FDoomVRHand : public FModel
	{
	public:
		gdh::Hand hand;
		gdh::FingerDriver fingers;
		int mesh = -1;
		uint64_t posedAt = 0;
		bool written = false;

		// The wrist's cut (the flat end of the cuff, where these floating
		// hands stop): found once from the model - the vertices at the far end
		// of the forearm (+z), and a ring a little further in for the
		// forearm's way - then followed through the pose as it is written.
		// Hand space, metres: its middle, its outward normal, two axes across
		// it and its half-widths along them.
		std::vector<unsigned> capIdx, ringIdx;
		bool capTried = false, capOk = false;
		gdh::Vec3 capC{}, capN{}, capA{}, capB{};
		float capRa = 0, capRb = 0, capOut = 0;	// capOut: the lip's height over the middle, along the normal

		void FindCap(const char* name)
		{
			if (capTried) return;
			capTried = true;
			const auto& verts = hand.meshes[mesh].verts;
			float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
			for (auto& v : verts) for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], v.pos[k]); hi[k] = std::max(hi[k], v.pos[k]); }
			for (unsigned i = 0; i < verts.size(); ++i)
			{
				const float z = verts[i].pos[2];
				if (z > hi[2] - 0.003f) capIdx.push_back(i);
				else if (z > hi[2] - 0.025f && z < hi[2] - 0.012f) ringIdx.push_back(i);
			}
			Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: hands - %s: x %.3f..%.3f y %.3f..%.3f z %.3f..%.3f m; wrist cut %u vertices, ring %u\n", name,
			       lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], unsigned(capIdx.size()), unsigned(ringIdx.size()));
		}

		bool Load(const char*, int, const char*, int) override { return false; }
		int FindFrame(const char*, bool) override { return 0; }
		void RenderFrame(FModelRenderer*, FGameTexture*, int, int, double, FTranslationID, const FTextureID*, int) override {}
		void AddSkins(uint8_t*, const FTextureID*) override {}

		unsigned NumVerts() const { return unsigned(hand.meshes[mesh].verts.size()); }
		unsigned NumIndices() const { return unsigned(hand.meshes[mesh].indices.size()); }

		void BuildVertexBuffer(FModelRenderer* renderer) override
		{
			if (GetVertexBuffer(renderer->GetType())) return;
			auto vbuf = renderer->CreateVertexBuffer(true, true);
			SetVertexBuffer(renderer->GetType(), vbuf);
			const auto& idx = hand.meshes[mesh].indices;
			unsigned* ip = vbuf->LockIndexBuffer(NumIndices());
			memcpy(ip, idx.data(), idx.size() * sizeof(unsigned));
			vbuf->UnlockIndexBuffer();
			written = false;
		}

		// The pose's vertices, in the hand's own space (metres).
		void WriteVertices(FModelRenderer* renderer)
		{
			hand.Skin();
			const auto& verts = hand.meshes[mesh].verts;
			if (!capIdx.empty()) posed.resize(verts.size());
			FModelVertex* vp = GetVertexBuffer(renderer->GetType())->LockVertexBuffer(NumVerts());
			for (size_t i = 0; i < verts.size(); ++i)
			{
				const gdh::Vertex& v = verts[i];
				const gdh::Vec3 p{ v.pos[0], v.pos[1], v.pos[2] };
				float x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 0;
				for (int k = 0; k < 4; ++k)
				{
					const float w = v.weight[k];
					if (w <= 0) continue;
					const gdh::Mat4& m = hand.skin[v.bone[k]];
					const gdh::Vec3 q = gdh::TransformPoint(m, p);
					x += q.x * w; y += q.y * w; z += q.z * w;
					nx += (m.m[0] * v.nrm[0] + m.m[4] * v.nrm[1] + m.m[8] * v.nrm[2]) * w;
					ny += (m.m[1] * v.nrm[0] + m.m[5] * v.nrm[1] + m.m[9] * v.nrm[2]) * w;
					nz += (m.m[2] * v.nrm[0] + m.m[6] * v.nrm[1] + m.m[10] * v.nrm[2]) * w;
				}
				const float nl = sqrtf(nx * nx + ny * ny + nz * nz);
				if (nl > 1e-6f) { nx /= nl; ny /= nl; nz /= nl; }
				if (!capIdx.empty()) posed[i] = { x, y, z };
				vp[i].Set(x, y, z, v.uv[0], v.uv[1]);
				vp[i].SetNormal(nx, ny, nz);
				vp[i].SetBoneSelector(0, 0, 0, 0);
				vp[i].SetBoneWeight(0, 0, 0, 0);
			}
			GetVertexBuffer(renderer->GetType())->UnlockVertexBuffer();
			written = true;
			UpdateCap();
		}

		std::vector<gdh::Vec3> posed;	// this pose's vertices (only while there is a cut to follow)

		void UpdateCap()
		{
			capOk = false;
			if (capIdx.size() < 6 || ringIdx.size() < 6 || posed.size() != hand.meshes[mesh].verts.size()) return;
			auto mean = [&](const std::vector<unsigned>& idx) {
				gdh::Vec3 c{ 0, 0, 0 };
				for (unsigned i : idx) { c.x += posed[i].x; c.y += posed[i].y; c.z += posed[i].z; }
				const float n = float(idx.size());
				return gdh::Vec3{ c.x / n, c.y / n, c.z / n };
			};
			capC = mean(capIdx);
			const gdh::Vec3 r = mean(ringIdx);
			gdh::Vec3 n{ capC.x - r.x, capC.y - r.y, capC.z - r.z };
			const float nl = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
			if (nl < 1e-4f) return;
			capN = { n.x / nl, n.y / nl, n.z / nl };
			// Across the cut: the hand's x laid flat on it, and the third way.
			gdh::Vec3 a{ 1.f - capN.x * capN.x, -capN.x * capN.y, -capN.x * capN.z };
			const float al = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
			if (al < 1e-4f) return;
			capA = { a.x / al, a.y / al, a.z / al };
			capB = { capN.y * capA.z - capN.z * capA.y, capN.z * capA.x - capN.x * capA.z, capN.x * capA.y - capN.y * capA.x };
			capRa = capRb = capOut = 0;
			for (unsigned i : capIdx)
			{
				const float dx = posed[i].x - capC.x, dy = posed[i].y - capC.y, dz = posed[i].z - capC.z;
				capRa = std::max(capRa, fabsf(dx * capA.x + dy * capA.y + dz * capA.z));
				capRb = std::max(capRb, fabsf(dx * capB.x + dy * capB.y + dz * capB.z));
				capOut = std::max(capOut, dx * capN.x + dy * capN.y + dz * capN.z);
			}
			capOk = capRa > 0.01f && capRb > 0.01f;
		}
	};

	/*
		A pair of hands and where its closed fist's middle is (right hand,
		metres, the hand's own space; the left mirrors x) - the point DoomVR
		puts on a gun's grip. The base pair is doomvr/hands/hand_{r,l}.gdh with
		a skin per game; a style is doomvr/hands/<style>/ with its own skin, in
		hands.pk3 from setup\gdh\<style>\ (gdh_setup). Numbers from the hands
		project's deliveries.
	*/
	struct HandSet
	{
		const char* dir;		// "" for the base pair
		float fist[3];
		FDoomVRHand* h[2] = {};	// [0] the right hand model, [1] the left
		bool tried = false, loaded = false;
	};
	HandSet gSets[] = {
		{ "", { -0.0330f, 0.0002f, -0.0810f } },
		{ "slayer", { -0.0331f, -0.0021f, -0.0768f } },
		{ "slayer_armor", { -0.03079905f, 0.00173175f, -0.07858501f } },
		{ "gameordie", { -0.0334f, -0.0014f, -0.0793f } },
	};

	// Whose hand this is: the game, and in Hexen the class being played.
	const char* SkinFamily(player_t* pl)
	{
		switch (gameinfo.gametype)
		{
		case GAME_Heretic: return "heretic";
		case GAME_Strife: return "strife";
		case GAME_Chex: return "chex";
		case GAME_Hexen:
		{
			AActor* mo = pl ? pl->mo : nullptr;
			static const char* classes[][2] = { { "ClericPlayer", "hexen_cleric" }, { "MagePlayer", "hexen_mage" }, { "FighterPlayer", "hexen_fighter" } };
			for (auto& c : classes)
			{
				PClass* cls = PClass::FindClass(c[0]);
				if (mo && cls && mo->IsKindOf(cls)) return c[1];
			}
			return "hexen_fighter";
		}
		default: return "doom";
		}
	}

	// The style the Doom games and Heretic wear (our Doom hands in
	// Heretic too, holding Game Or Die's Heretic guns), when hands.pk3 has
	// it; else null (the base pair, as before the styles).
	HandSet* StyleSet(player_t* pl)
	{
		const char* family = SkinFamily(pl);
		if (strcmp(family, "doom") != 0 && strcmp(family, "heretic") != 0) return nullptr;
		HandSet& s = gSets[vr_hands_style == 1 ? 2 : vr_hands_style == 2 ? 3 : 1];
		static bool said[4];
		const int lump = fileSystem.CheckNumForFullName(FStringf("doomvr/hands/%s/hand_r.gdh", s.dir).GetChars());
		if (lump < 0)
		{
			const int i = int(&s - gSets);
			if (!said[i]) { said[i] = true; Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: hands - no %s style in hands.pk3 (run SETUP.bat), the plain hands instead\n", s.dir); }
			return nullptr;
		}
		return &s;
	}

	// The skin a set is drawn with for this player: a style's own, or the
	// game's (each hero's hand, made at setup); invalid when there is none.
	std::map<std::string, FTextureID> gSkinCache;	// the textures go with a game switched in this process
	FTextureID Skin(player_t* pl, const HandSet* set)
	{
		std::map<std::string, FTextureID>& cache = gSkinCache;
		const std::string key = set && *set->dir ? std::string(set->dir) + "/skin.png" : std::string("skins/") + SkinFamily(pl) + ".png";
		auto it = cache.find(key);
		if (it != cache.end()) return it->second;
		const int flags = FTextureManager::TEXMAN_TryAny | FTextureManager::TEXMAN_ForceLookup;
		FTextureID id = TexMan.CheckForTexture(("doomvr/hands/" + key).c_str(), ETextureType::Any, flags);
		if (!id.isValid() && key == "skins/doom.png")
			id = TexMan.CheckForTexture("doomvr/hands/skin.png", ETextureType::Any, flags);
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: hands - %s %s\n", key.c_str(), id.isValid() ? "found" : "not found (hands.pk3), none drawn");
		cache[key] = id;
		return id;
	}

	bool LoadSet(HandSet& set)
	{
		if (set.tried) return set.loaded;
		set.tried = true;
		const FString prefix = *set.dir ? FStringf("doomvr/hands/%s/", set.dir) : FString("doomvr/hands/");
		const FString names[2] = { prefix + "hand_r.gdh", prefix + "hand_l.gdh" };
		const char* meshes[2] = { "Hand_low_R", "Hand_low_L" };
		for (int i = 0; i < 2; ++i)
		{
			const int lump = fileSystem.CheckNumForFullName(names[i].GetChars());
			if (lump < 0)
			{
				Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: hands - %s not found (hands.pk3 beside the engine), none drawn\n", names[i].GetChars());
				return false;
			}
			auto data = fileSystem.ReadFile(lump);
			// Never deleted: freed with the process, after the renderer, so
			// no GL call runs at exit. The buffers go with FlushModels.
			auto h = new FDoomVRHand;
			if (!h->hand.LoadMemory(data.bytes(), data.size()))
			{
				Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: hands - %s: %s\n", names[i].GetChars(), h->hand.error.c_str());
				delete h;
				return false;
			}
			h->mesh = h->hand.FindMesh(meshes[i]);
			if (h->mesh < 0 || !h->fingers.Bind(h->hand))
			{
				Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: hands - %s has no %s mesh or finger poses\n", names[i].GetChars(), meshes[i]);
				delete h;
				return false;
			}
			h->mFileName = names[i];
			set.h[i] = h;
		}
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: hands loaded%s%s (%u + %u triangles)\n", *set.dir ? " - style " : "", set.dir,
		       set.h[0]->NumIndices() / 3, set.h[1]->NumIndices() / 3);
		set.loaded = true;
		return true;
	}

	// The set this player's hands are drawn from (loaded), or null.
	HandSet* ActiveSet(player_t* pl)
	{
		if (HandSet* s = StyleSet(pl)) if (LoadSet(*s) && Skin(pl, s).isValid()) return s;
		return LoadSet(gSets[0]) ? &gSets[0] : nullptr;
	}

	bool HasModel(DPSprite* psp)
	{
		return psp && psp->GetState() && FindModelFrame(psp->Caller, psp->GetSprite(), psp->GetFrame(), false) != nullptr;
	}

	// A 3D weapon from one of our own gun packs: our hand holds it. A mod's own
	// 3D weapon brings its own hand, so ours is not drawn on it (models.cpp).
	bool OurModel(DPSprite* psp)
	{
		if (!psp || !psp->GetState()) return false;
		const FSpriteModelFrame* smf = FindModelFrame(psp->Caller, psp->GetSprite(), psp->GetFrame(), false);
		return smf && smf->doomvrOwn;
	}

	bool KindOf(AActor* a, const char* name)
	{
		PClass* cls = PClass::FindClass(name);
		return a && cls && a->IsKindOf(cls);
	}

	// Legacy of Rust gives its Incinerator and Calamity Blade to Doom's own
	// PlasmaRifle and BFG9000 (its DEHACKED rewrites weapon slots 5 and 6), and
	// they wear our models of those two there (doomvr-weapons-id24slots-b.pk3).
	// The gun in the hand is then held as the class its model was made for;
	// null when the weapon is drawn as itself.
	PClass* DrawnAs(AActor* weapon)
	{
		static const char* const kModelClass[][2] = { { "doomvr-b/incinerator", "ID24Incinerator" }, { "doomvr-b/calamityblade", "ID24CalamityBlade" } };
		AActor* owner = weapon ? weapon->PointerVar<AActor>(NAME_Owner) : nullptr;
		player_t* pl = owner ? owner->player : nullptr;
		if (!pl || pl->ReadyWeapon != weapon) return nullptr;
		DPSprite* psp = pl->FindPSprite(PSP_WEAPON);
		if (!psp || !psp->GetState()) return nullptr;
		const FSpriteModelFrame* smf = FindModelFrame(psp->Caller, psp->GetSprite(), psp->GetFrame(), false);
		if (!smf || smf->modelIDs.Size() == 0 || smf->modelIDs[0] < 0 || unsigned(smf->modelIDs[0]) >= Models.Size()) return nullptr;
		const FString& file = Models[smf->modelIDs[0]]->mFileName;
		for (auto& m : kModelClass)
		{
			if (file.IndexOf(m[0]) < 0) continue;
			PClass* cls = PClass::FindClass(m[1]);
			return cls != weapon->GetClass() ? cls : nullptr;
		}
		return nullptr;
	}

	// The name a held weapon's tune is found under: its model's class, else its own.
	FName TuneName(AActor* weapon)
	{
		PClass* drawn = DrawnAs(weapon);
		return drawn ? drawn->TypeName : weapon->GetClass()->TypeName;
	}

	// A weapon that takes another's place without being one of its kind
	// (DOOM CE's D64Shotgun replaces Shotgun) is held as the one it replaces:
	// 0 not that class, 1 that class, 2 replaces it.
	int IsOrReplaces(AActor* weapon, PClass* cls)
	{
		if (!weapon || !cls) return 0;
		if (PClass* drawn = DrawnAs(weapon)) return drawn == cls ? 1 : 0;
		if (weapon->GetClass() == cls) return 1;
		const FName doom = DoomVR_StandsFor(weapon->GetClass()->TypeName);
		return doom != NAME_None && PClass::FindClass(doom) == cls ? 2 : 0;
	}

	/*
		Two hands on the B guns (Game Or Die's own Doom guns): each long gun's
		front grip and the other hand's placement on it - the front grips from
		the hands project's holds (support_centre_aim, from the aim's origin,
		moved 2 cm left and 9 cm back to be from the gun's grip point), the
		placements kept as the FRONT-HAND table had them about their grips
		(the holds' own placement replaces them where the hand has the pose),
		classic hands, at each gun's MODELDEF as packed (Legacy of Rust's
		two with the shotgun's placement about its grip as their fallback). Point = the front grip
		from the gun's grip point; q, origin = the other hand's turn and place
		there; all in the aim's frame, metres, right-handed. Valid only for
		those exact models, so only when the gun drawn is from the B pack.
	*/
	struct SupportRow { const char* cls; DoomVR_Support s; };
	const SupportRow kSupport[] = {
		{ "Shotgun",        { { 0.000f, 0.048f, -0.569f }, { -0.3783f, -0.5629f, 0.5933f, 0.4336f }, { -0.037f, 0.021f, -0.567f } } },
		{ "SuperShotgun",   { { 0.000f, 0.016f, -0.337f }, { -0.3858f, -0.3546f, 0.7482f, 0.4069f }, { -0.046f, -0.028f, -0.300f } } },
		{ "Chaingun",       { { 0.000f, -0.009f, -0.173f }, { -0.4644f, -0.2427f, 0.6174f, 0.5867f }, { -0.060f, -0.046f, -0.130f } } },
		{ "RocketLauncher", { { 0.000f, 0.025f, -0.178f }, {  0.0583f,  0.0875f, -0.0518f, 0.9931f }, { -0.019f, 0.022f, -0.129f } } },
		{ "PlasmaRifle",    { { 0.000f, 0.043f, -0.385f }, {  0.0382f,  0.0883f, -0.0231f, 0.9951f }, { -0.044f, 0.042f, -0.303f } } },
		{ "BFG9000",        { { 0.000f, -0.009f, -0.155f }, { -0.4485f, -0.5087f, 0.5317f, 0.5073f }, { -0.060f, -0.072f, -0.153f } } },
		{ "Chainsaw",       { { 0.066f, 0.136f, -0.182f }, {  0.3555f,  0.3271f, -0.6253f, 0.6129f }, { 0.065f, 0.098f, -0.139f } } },
		{ "ID24Incinerator",   { { 0.000f, 0.073f, -0.347f }, { -0.3783f, -0.5629f, 0.5933f, 0.4336f }, { -0.037f, 0.046f, -0.345f } } },
		{ "ID24CalamityBlade", { { 0.000f, -0.064f, -0.497f }, { -0.3783f, -0.5629f, 0.5933f, 0.4336f }, { -0.037f, -0.091f, -0.495f } } },
		// Game Or Die's Unmaker and Rifle,
		// on each game's own class for them; placement fallback the shotgun's.
		{ "Unmaker",        { { 0.000f, 0.069f, -0.406f }, { -0.3783f, -0.5629f, 0.5933f, 0.4336f }, { -0.037f, 0.042f, -0.404f } } },
		{ "LaserGun",       { { 0.000f, 0.069f, -0.406f }, { -0.3783f, -0.5629f, 0.5933f, 0.4336f }, { -0.037f, 0.042f, -0.404f } } },
		{ "64Unmaker",      { { 0.000f, 0.069f, -0.406f }, { -0.3783f, -0.5629f, 0.5933f, 0.4336f }, { -0.037f, 0.042f, -0.404f } } },
		{ "D64LaserRifle",  { { 0.000f, 0.069f, -0.406f }, { -0.3783f, -0.5629f, 0.5933f, 0.4336f }, { -0.037f, 0.042f, -0.404f } } },
		{ "Rifle",          { { 0.000f, 0.096f, -0.389f }, { -0.3783f, -0.5629f, 0.5933f, 0.4336f }, { -0.037f, 0.069f, -0.387f } } },
		{ "64Rifle",        { { 0.000f, 0.096f, -0.389f }, { -0.3783f, -0.5629f, 0.5933f, 0.4336f }, { -0.037f, 0.069f, -0.387f } } },
		{ "64RifleTactical", { { 0.000f, 0.096f, -0.389f }, { -0.3783f, -0.5629f, 0.5933f, 0.4336f }, { -0.037f, 0.069f, -0.387f } } },
		// Heretic's Ethereal Crossbow,
		// with its Tome of Power class; placement fallback the shotgun's.
		{ "Crossbow",        { { 0.000f, -0.000f, -0.238f }, { -0.3783f, -0.5629f, 0.5933f, 0.4336f }, { -0.037f, -0.027f, -0.236f } } },
		{ "CrossbowPowered", { { 0.000f, -0.000f, -0.238f }, { -0.3783f, -0.5629f, 0.5933f, 0.4336f }, { -0.037f, -0.027f, -0.236f } } },
		// Heretic's Staff, the other hand further up the shaft (the second delivery that day).
		{ "Staff",           { { 0.000f, -0.000f, -0.280f }, { -0.3783f, -0.5629f, 0.5933f, 0.4336f }, { -0.037f, -0.027f, -0.278f } } },
		{ "StaffPowered",    { { 0.000f, -0.000f, -0.280f }, { -0.3783f, -0.5629f, 0.5933f, 0.4336f }, { -0.037f, -0.027f, -0.278f } } },
	};

	/*
		The hands' gun-holding poses on the B guns: per gun and family, the
		gun hand's hold (pose; the hand-space point that goes on the gun's grip
		point; the hand's turn in the aim) and the other hand's support hold
		(pose; the left hand's point that goes on the front grip; its turn).
		From the hands project's holds JSON (and every export of its
		Blender hold editor) by tools/holds-from-json.py: doomvrholds.txt in
		uzdoom.pk3, then doomvr-holds.txt beside the ini, whose lines replace
		the shipped ones - a new export is tried by dropping that file in.
		Families: [0] classic (slayer), [1] armor (slayer_armor).
	*/
	struct HoldEntry { FName cls; FString pose; DoomVR_GripHold h; };
	std::vector<HoldEntry> gHoldTable[2][2];	// [grip 0 / support 1][family]
	bool gHoldsLoaded = false;

	void ParseHolds(const char* text)
	{
		for (const char* line = text; line && *line; )
		{
			const char* end = strchr(line, '\n');
			FString one(line, end ? end - line : strlen(line));
			line = end ? end + 1 : nullptr;
			one.StripLeftRight();
			if (one.IsEmpty() || one[0] == '#') continue;
			char kind[16], family[32], cls[64], pose[64];
			HoldEntry e;
			float* hp = e.h.point;
			float* q = e.h.q;
			if (sscanf(one.GetChars(), "%15s %31s %63s %63s %f %f %f %f %f %f %f", kind, family, cls, pose,
			           &hp[0], &hp[1], &hp[2], &q[0], &q[1], &q[2], &q[3]) != 11) continue;
			const int k = !stricmp(kind, "grip") ? 0 : !stricmp(kind, "support") ? 1 : -1;
			const int f = !stricmp(family, "slayer") ? 0 : !stricmp(family, "slayer_armor") ? 1 : -1;
			if (k < 0 || f < 0) continue;
			e.cls = FName(cls);
			e.pose = pose;
			auto& list = gHoldTable[k][f];
			bool replaced = false;
			for (auto& old : list) if (old.cls == e.cls) { old = e; replaced = true; break; }
			if (!replaced) list.push_back(e);
		}
	}

	void LoadHolds()
	{
		if (gHoldsLoaded) return;
		gHoldsLoaded = true;
		const int lump = fileSystem.CheckNumForFullName("doomvrholds.txt");
		if (lump >= 0)
		{
			auto data = fileSystem.ReadFile(lump);
			FString text(data.string(), data.size());
			ParseHolds(text.GetChars());
		}
		FString path = GameConfig ? GameConfig->GetPathName() : "";
		FixPathSeperator(path);
		const ptrdiff_t slash = path.LastIndexOf('/');
		path = (slash >= 0 ? path.Left(slash + 1) : FString()) + "doomvr-holds.txt";
		if (FILE* fp = fopen(path.GetChars(), "rb"))
		{
			FString text;
			char buf[4096];
			size_t n;
			while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) text.AppendCStrPart(buf, n);
			fclose(fp);
			ParseHolds(text.GetChars());
			Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: hands - gun holds from %s\n", path.GetChars());
		}
	}

	// This weapon's hold (0 the gun hand's, 1 the support hand's) for a hand family, or null.
	const HoldEntry* FindHold(int kind, int family, AActor* weapon)
	{
		if (family < 0 || !weapon) return nullptr;
		LoadHolds();
		const HoldEntry* replaced = nullptr;
		for (auto& e : gHoldTable[kind][family])
		{
			const int m = IsOrReplaces(weapon, PClass::FindClass(e.cls));
			if (m == 1) return &e;
			if (m == 2 && !replaced) replaced = &e;
		}
		return replaced;
	}

	int HoldFamily(const char* setDir)
	{
		// The Game Or Die gloves have the marine's skeleton and its poses in
		// the same order, and the same palm point: the marine's holds fit them.
		if (!strcmp(setDir, "slayer") || !strcmp(setDir, "gameordie")) return 0;
		if (!strcmp(setDir, "slayer_armor")) return 1;
		return -1;	// the base hands have no hold poses
	}

	bool IsBGun(DPSprite* psp)
	{
		if (!psp || !psp->GetState()) return false;
		const FSpriteModelFrame* smf = FindModelFrame(psp->Caller, psp->GetSprite(), psp->GetFrame(), false);
		if (!smf || smf->modelIDs.Size() == 0 || smf->modelIDs[0] < 0 || unsigned(smf->modelIDs[0]) >= Models.Size()) return false;
		return Models[smf->modelIDs[0]]->mFileName.IndexOf("doomvr-b/") >= 0;
	}

	// The front grip of the gun up, as its fire frame has it (the pump slides
	// back; the Super Shotgun's barrels tip open about their hinge); false
	// when it has none we know. With a support hold (family >= 0, the hand
	// has the pose), the other hand's placement is the hold's: its left-hand
	// point on the front grip, turned as fitted; *pose gets the pose's name.
	bool SupportFor(AActor* weapon, DPSprite* psp, bool atRest, DoomVR_Support& out, int family = -1, const char** pose = nullptr)
	{
		if (pose) *pose = nullptr;
		if (!weapon || !IsBGun(psp)) return false;
		const SupportRow* row = nullptr;
		for (auto& r : kSupport) { const int m = IsOrReplaces(weapon, PClass::FindClass(r.cls)); if (m == 1 || (m == 2 && !row)) row = &r; if (m == 1) break; }
		if (!row) return false;
		out = row->s;
		if (const HoldEntry* he = FindHold(1, family, weapon))
		{
			const auto& hr = he->h;
				{
					// origin = front grip - R * hand point (aim frame, from the gun's grip point)
					// v' = R v, R = (x, y, z, w): v + 2w (r x v) + 2 r x (r x v)
					const double qx = hr.q[0], qy = hr.q[1], qz = hr.q[2], qw = hr.q[3];
					const double vx = hr.point[0], vy = hr.point[1], vz = hr.point[2];
					const double cx = qy * vz - qz * vy, cy = qz * vx - qx * vz, cz = qx * vy - qy * vx;
					const double rx = vx + 2 * (qw * cx + qy * cz - qz * cy);
					const double ry = vy + 2 * (qw * cy + qz * cx - qx * cz);
					const double rz = vz + 2 * (qw * cz + qx * cy - qy * cx);
					for (int i = 0; i < 4; ++i) out.q[i] = hr.q[i];
					out.origin[0] = float(out.point[0] - rx); out.origin[1] = float(out.point[1] - ry); out.origin[2] = float(out.point[2] - rz);
					if (pose) *pose = he->pose.GetChars();
				}
		}
		if (atRest) return true;
		const int frame = psp->GetFrame();
		if (!strcmp(row->cls, "Shotgun"))
		{
			// SHTG C, D: the pump 6.9 and 7.6 cm back (measured from the model's frames).
			const float back = frame == 2 ? 0.069f : frame == 3 ? 0.076f : 0.f;
			out.point[2] += back;
			out.origin[2] += back;
		}
		else if (!strcmp(row->cls, "SuperShotgun"))
		{
			// SHT2 A-H: the barrels tip open about the hinge pin, Rx(-tip).
			static const float kTip[8] = { 0, 0, 2.5f, 40, 42, 40, 37, 0 };
			const float tip = frame >= 0 && frame < 8 ? kTip[frame] : 0.f;
			if (tip != 0)
			{
				static const float h[3] = { 0.0f, 0.021f, -0.196f };	// the hinge, fitted to the model's frames
				const double a = -tip * 3.14159265358979 / 180.0, c = cos(a), s = sin(a);
				auto rot = [&](float* v) {
					const double y = v[1] - h[1], z = v[2] - h[2];
					v[1] = float(h[1] + y * c - z * s);
					v[2] = float(h[2] + y * s + z * c);
				};
				rot(out.point);
				rot(out.origin);
				// q' = Rx(a) q
				const double hx = sin(a / 2), hw = cos(a / 2);
				const float q[4] = { out.q[0], out.q[1], out.q[2], out.q[3] };
				out.q[0] = float(hw * q[0] + hx * q[3]);
				out.q[1] = float(hw * q[1] - hx * q[2]);
				out.q[2] = float(hw * q[2] + hx * q[1]);
				out.q[3] = float(hw * q[3] - hx * q[0]);
			}
		}
		return true;
	}

	// Heretic's Gauntlets of the Necromancer as our model: worn, one on each
	// hand - the left one the right one's mirror image, on the other hand in
	// the same hold.
	bool WornOnBoth(AActor* weapon, DPSprite* psp)
	{
		if (!KindOf(weapon, "Gauntlets") || !IsBGun(psp)) return false;
		const FSpriteModelFrame* smf = FindModelFrame(psp->Caller, psp->GetSprite(), psp->GetFrame(), false);
		if (!smf || Models[smf->modelIDs[0]]->mFileName.IndexOf("doomvr-b/gauntlets/") < 0) return false;
		AActor* owner = weapon->PointerVar<AActor>(NAME_Owner);
		return owner && owner->player && (!owner->player->OffhandWeapon || owner->player->OffhandWeapon == weapon);
	}

	// A weapon our hand IS: 1 a fist, 2 an open hand casting a spell, 0 not.
	int HandWeapon(AActor* weapon)
	{
		if (!weapon || KindOf(weapon, "Bootspoon")) return 0;
		if (KindOf(weapon, "Fist") || KindOf(weapon, "FWeapFist") || DoomVR_StandsFor(weapon->GetClass()->TypeName) == NAME_Fist) return 1;
		if (KindOf(weapon, "CWeapFlame") || KindOf(weapon, "MWeapFrost") || KindOf(weapon, "MWeapLightning")) return 2;
		return 0;
	}

	// Held in one hand: sidearms, wands and one-hand melee weapons (the rest
	// with a 3D model are long guns, two hands).
	const char* const kOneHand[] = { "Pistol", "GoldWand", "GoldWandPowered", "MWeapWand", "CWeapMace", "FWeapAxe",
	                                 "FWeapHammer", "PunchDagger", "Bootspoon", "MiniZorcher",
	                                 "Blaster", "BlasterPowered", "Gauntlets", "GauntletsPowered",
	                                 "SkullRod", "SkullRodPowered", "PhoenixRod", "PhoenixRodPowered", "Mace", "MacePowered",
	                                 "TempestWand", "TempestWandPowered" };

	/*
		The two holsters (the zones and the grab are
		doomvr_headset.cpp's). The hip holds the last one-handed gun used, the
		shoulder the last two-handed one; melee weapons, fists and spells are
		never holstered (they live in the weapon wheel). A gun from the wheel or
		the next-weapon button becomes its holster's gun. When that gun is gone
		or out of ammo (a new game, the game taking the guns away), the holster
		falls back to the best of its kind carried - the latest in the game's
		weapon order - or is empty.
	*/
	PClassActor* gHolsterLast[3];	// [1] the hip's, [2] the shoulder's

	// 1 the hip, 2 the shoulder, 0 never holstered.
	int HolsterKind(AActor* w)
	{
		if (!w || HandWeapon(w)) return 0;
		if (w->IntVar(NAME_WeaponFlags) & WIF_MELEEWEAPON) return 0;
		for (const char* n : kOneHand) if (KindOf(w, n)) return 1;
		if (DoomVR_StandsFor(w->GetClass()->TypeName) == NAME_Pistol) return 1;
		return 2;
	}

	// The gun itself, not its Tome of Power twin.
	AActor* Unpowered(AActor* w)
	{
		if (w && (w->IntVar(NAME_WeaponFlags) & WIF_POWERED_UP))
			if (AActor* sister = w->PointerVar<AActor>(NAME_SisterWeapon)) return sister;
		return w;
	}

	bool Usable(AActor* inv)
	{
		if (!inv || (inv->IntVar(NAME_WeaponFlags) & WIF_POWERED_UP)) return false;
		AActor* ammo = inv->PointerVar<AActor>(NAME_Ammo1);
		return !ammo || ammo->IntVar(NAME_Amount) > 0;
	}

	// The gun in holster `kind` now: the last of its kind used while it is
	// carried with ammo, else the latest of its kind in the weapon order.
	AActor* HolsterGun(player_t* pl, int kind)
	{
		if (!pl || !pl->mo) return nullptr;
		if (AActor* held = Unpowered(pl->ReadyWeapon))
			if (HolsterKind(held) == kind) gHolsterLast[kind] = held->GetClass();
		if (gHolsterLast[kind])
		{
			AActor* inv = pl->mo->FindInventory(gHolsterLast[kind]);
			if (Usable(inv)) return inv;
		}
		for (int k = NUM_WEAPON_SLOTS; k >= 1; --k)
		{
			const int slot = k % NUM_WEAPON_SLOTS;
			for (int j = pl->weapons.SlotSize(slot) - 1; j >= 0; --j)
			{
				PClassActor* cls = pl->weapons.GetWeapon(slot, j);
				AActor* inv = cls ? pl->mo->FindInventory(cls) : nullptr;
				if (Usable(inv) && HolsterKind(inv) == kind) return inv;
			}
		}
		return nullptr;
	}

	// The weapon a holstered gun is put away to: the bare hands or the first
	// melee weapon in the weapon order; none in a game never unarmed.
	AActor* PutAwayTo(player_t* pl)
	{
		for (int k = 1; k <= NUM_WEAPON_SLOTS; ++k)
		{
			const int slot = k % NUM_WEAPON_SLOTS;
			for (int j = 0; j < pl->weapons.SlotSize(slot); ++j)
			{
				PClassActor* cls = pl->weapons.GetWeapon(slot, j);
				AActor* inv = cls ? pl->mo->FindInventory(cls) : nullptr;
				if (inv && !(inv->IntVar(NAME_WeaponFlags) & WIF_POWERED_UP) &&
				    (HandWeapon(inv) == 1 || (inv->IntVar(NAME_WeaponFlags) & WIF_MELEEWEAPON))) return inv;
			}
		}
		return nullptr;
	}
}

const void* DoomVR_ReadyWeaponId()
{
	return players[consoleplayer].ReadyWeapon;
}

// A grip at holster i (0 the hip, 1 the shoulder), from doomvr_headset.cpp:
// its gun comes out the way the weapon wheel switches; at the holster of the
// gun in hand, it is put away. False: refused (the holster is empty).
bool DoomVR_HolsterDraw(int i)
{
	player_t* pl = &players[consoleplayer];
	const int kind = i + 1;
	AActor* gun = HolsterGun(pl, kind);
	AActor* held = Unpowered(pl->ReadyWeapon);
	const char* where = kind == 1 ? "hip" : "shoulder";
	if (gun && held == gun)
	{
		AActor* to = PutAwayTo(pl);
		if (to && to != held) C_DoCommand(FStringf("use %s", to->GetClass()->TypeName.GetChars()).GetChars());
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: %s put back in the %s holster%s%s\n", gun->GetClass()->TypeName.GetChars(), where,
		       to && to != held ? ", in hand now: " : "", to && to != held ? to->GetClass()->TypeName.GetChars() : "");
		return true;
	}
	if (!gun)
	{
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: the %s holster is empty\n", where);
		return false;
	}
	C_DoCommand(FStringf("use %s", gun->GetClass()->TypeName.GetChars()).GetChars());
	Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: holster draw from the %s - %s\n", where, gun->GetClass()->TypeName.GetChars());
	return true;
}

// The weapon layer worn on both hands (Heretic's Gauntlets as our model):
// hw_weapon.cpp draws its mirror image on the other hand.
bool DoomVR_WornOnBothHands(DPSprite* psp)
{
	return psp && psp->GetID() == PSP_WEAPON && psp->Caller && WornOnBoth(psp->Caller, psp);
}

// The hand models' buffers, when the renderer goes (models.cpp FlushModels).
void DoomVR_FlushHands()
{
	for (auto& s : gSets) for (auto h : s.h) if (h) h->DestroyVertexBuffer();
}

// Which hands are drawn this frame: bit 0 the weapon hand, bit 1 the other.
// *fist is the weapon whose flat pictures our hand replaces (a fist or a
// spell), or null.
int DoomVR_HandsToDraw(AActor** fist)
{
	if (fist) *fist = nullptr;
	{
		// Physical play, two hands: is the gun up a long one (a 3D gun, not a
		// sidearm or a one-hand melee weapon), and its fit.
		AActor* camera = players[consoleplayer].camera;
		player_t* pl = camera ? camera->player : nullptr;
		AActor* w = pl ? pl->ReadyWeapon : nullptr;
		bool longGun = w != nullptr && HasModel(pl->FindPSprite(PSP_WEAPON)) && HandWeapon(w) == 0;
		for (const char* n : kOneHand) if (longGun && KindOf(w, n)) longGun = false;
		if (longGun && DoomVR_StandsFor(w->GetClass()->TypeName) == NAME_Pistol) longGun = false;
		DoomVR_Support rest;
		const bool hasSupport = longGun && SupportFor(w, pl->FindPSprite(PSP_WEAPON), true, rest);
		DoomVR_NoteHeldGun(longGun, w ? DoomVR_GetTune(TuneName(w)) : DoomVR_WeaponTune(), hasSupport ? &rest : nullptr);
	}
	if (!vr_hands || vr_style != 1 || !DoomVR_Active()) return 0;
	AActor* camera = players[consoleplayer].camera;
	player_t* pl = camera ? camera->player : nullptr;
	if (!pl || !r_drawplayersprites || (pl->cheats & CF_CHASECAM) || (r_deathcamera && camera->health <= 0)) return 0;
	HandSet* set = ActiveSet(pl);
	if (!set || !Skin(pl, set).isValid()) return 0;

	int bits = 0;
	DPSprite* wpn = pl->FindPSprite(PSP_WEAPON);
	if (HasModel(wpn)) { if (OurModel(wpn)) bits |= 1; }
	else if (wpn && HandWeapon(pl->ReadyWeapon))
	{
		bits |= 1;
		if (fist) *fist = pl->ReadyWeapon;
	}
	if (!pl->OffhandWeapon || pl->OffhandWeapon == pl->ReadyWeapon) bits |= 2;
	return bits;
}

// Each hand as DrawVRHands last drew it (this eye), for the HUD on its
// wrist's cut; DrawVRWrist uses them and clears them. The cut in the hand's
// own space (metres): the hand model's, or the cuff's of a gun worn over it.
namespace
{
	struct Cut
	{
		gdh::Vec3 capC{}, capN{}, capA{}, capB{};
		float capRa = 0, capRb = 0, capOut = 0;
		bool capOk = false;
	};
	struct DrawnHand { bool valid = false; float m[16]; Cut cut; };
	DrawnHand gDrawnHands[2];

	Cut HandCut(const FDoomVRHand* h)
	{
		Cut c;
		c.capC = h->capC; c.capN = h->capN; c.capA = h->capA; c.capB = h->capB;
		c.capRa = h->capRa; c.capRb = h->capRb; c.capOut = h->capOut; c.capOk = h->capOk;
		return c;
	}

	/*
		The cuff's end of a gun worn over the hand (Heretic's Gauntlets as our
		model): a flat disc, found from the model's own points brought into the
		hand's space (m: the hand's, hand metres to view). It is not square to
		the forearm (+z: a sliver, 7.6 x 1.4 cm, on the desk) and spikes stand
		round it (followed along, the end ran out to a spike's tip), so the
		disc is the plane through the most of the far end's points: tried
		through three of them at a time, the best kept. The gun sits still in
		the hand, so it is found again only when the hand style, the gun's
		model, the hand or a tune changes.
	*/
	struct WornCutCache
	{
		const void* set = nullptr;
		const FSpriteModelFrame* smf = nullptr;
		bool physLeft = false, tried = false;
		DoomVR_WeaponTune tune, handTune;
		Cut cut;
	};
	WornCutCache gWornCut[2];

	bool WornCut(int hand, const float* m, AActor* weapon, DPSprite* psp, const DoomVR_WeaponTune& tune, const DoomVR_WeaponTune* handTune,
	             const void* set, bool physLeft, Cut& out)
	{
		const FSpriteModelFrame* smf = psp ? FindModelFrame(psp->Caller, psp->GetSprite(), psp->GetFrame(), false) : nullptr;
		if (!smf) return false;
		WornCutCache& k = gWornCut[hand];
		const DoomVR_WeaponTune ht = handTune ? *handTune : DoomVR_WeaponTune();
		if (k.tried && k.set == set && k.smf == smf && k.physLeft == physLeft && !memcmp(&k.tune, &tune, sizeof(tune)) && !memcmp(&k.handTune, &ht, sizeof(ht)))
		{
			out = k.cut;
			return out.capOk;
		}
		k.tried = true; k.set = set; k.smf = smf; k.physLeft = physLeft; k.tune = tune; k.handTune = ht;
		k.cut = Cut();
		TArray<FVector3> pts;
		DoomVR_SetDrawingHand(hand);
		DoomVR_SetDrawingTune(tune);
		const bool placed = DoomVR_WornModelPoints(const_cast<FSpriteModelFrame*>(smf), weapon, pts);
		DoomVR_SetDrawingTune(DoomVR_WeaponTune());
		DoomVR_SetDrawingHand(0);
		VSMatrix hm, inv;
		hm.loadMatrix(m);
		if (!placed || pts.Size() < 12 || !hm.inverseMatrix(inv)) return false;
		std::vector<gdh::Vec3> p(pts.Size());
		gdh::Vec3 all{ 0, 0, 0 };
		float zmax = -1e9f;
		for (unsigned i = 0; i < pts.Size(); ++i)
		{
			float in[4] = { pts[i].X, pts[i].Y, pts[i].Z, 1.f }, o[4];
			inv.multMatrixPoint(in, o);
			p[i] = { o[0], o[1], o[2] };
			all.x += o[0]; all.y += o[1]; all.z += o[2];
			zmax = std::max(zmax, o[2]);
		}
		all = { all.x / p.size(), all.y / p.size(), all.z / p.size() };
		// The far end's points (the last 3 cm along the forearm) and the plane
		// through the most of them, facing along it (within 60 degrees).
		std::vector<unsigned> end;
		for (unsigned i = 0; i < p.size(); ++i)
			if (p[i].z > zmax - 0.03f) end.push_back(i);
		if (end.size() < 12) return false;
		auto sub3 = [](const gdh::Vec3& u, const gdh::Vec3& v) { return gdh::Vec3{ u.x - v.x, u.y - v.y, u.z - v.z }; };
		auto dot3 = [](const gdh::Vec3& u, const gdh::Vec3& v) { return u.x * v.x + u.y * v.y + u.z * v.z; };
		gdh::Vec3 bestN{ 0, 0, 1 };
		float bestD = 0;
		size_t bestCount = 0;
		uint32_t seed = 12345;
		auto pick = [&]() { seed = seed * 1664525u + 1013904223u; return end[(seed >> 8) % end.size()]; };
		for (int trial = 0; trial < 400; ++trial)
		{
			const gdh::Vec3& a = p[pick()];
			const gdh::Vec3 u = sub3(p[pick()], a), v = sub3(p[pick()], a);
			gdh::Vec3 n{ u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
			const float nl = sqrtf(dot3(n, n));
			if (nl < 1e-7f) continue;
			n = { n.x / nl, n.y / nl, n.z / nl };
			if (n.z < 0) n = { -n.x, -n.y, -n.z };
			if (n.z < 0.5f) continue;
			const float d = dot3(n, a);
			size_t count = 0;
			for (unsigned i : end) if (fabsf(dot3(n, p[i]) - d) < 0.0008f) ++count;
			if (count > bestCount) { bestCount = count; bestN = n; bestD = d; }
		}
		std::vector<unsigned> cap;
		for (unsigned i : end) if (fabsf(dot3(bestN, p[i]) - bestD) < 0.0008f) cap.push_back(i);
		if (cap.size() < 12) return false;
		Cut& c = k.cut;
		gdh::Vec3 cc{ 0, 0, 0 };
		for (unsigned i : cap) { cc.x += p[i].x; cc.y += p[i].y; cc.z += p[i].z; }
		c.capC = { cc.x / cap.size(), cc.y / cap.size(), cc.z / cap.size() };
		// Outward: away from the gun's middle.
		c.capN = dot3(bestN, sub3(c.capC, all)) >= 0 ? bestN : gdh::Vec3{ -bestN.x, -bestN.y, -bestN.z };
		gdh::Vec3 a{ 1.f - c.capN.x * c.capN.x, -c.capN.x * c.capN.y, -c.capN.x * c.capN.z };
		const float al = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
		if (al < 1e-4f) return false;
		c.capA = { a.x / al, a.y / al, a.z / al };
		c.capB = { c.capN.y * c.capA.z - c.capN.z * c.capA.y, c.capN.z * c.capA.x - c.capN.x * c.capA.z, c.capN.x * c.capA.y - c.capN.y * c.capA.x };
		for (unsigned i : cap)
		{
			const gdh::Vec3 d = sub3(p[i], c.capC);
			c.capRa = std::max(c.capRa, fabsf(dot3(d, c.capA)));
			c.capRb = std::max(c.capRb, fabsf(dot3(d, c.capB)));
		}
		// Anything of the gun standing out past the disc (its rim's lip).
		for (unsigned i : end)
		{
			const gdh::Vec3 d = sub3(p[i], c.capC);
			const float across = sqrtf(std::max(0.f, dot3(d, d) - dot3(d, c.capN) * dot3(d, c.capN)));
			if (across < std::max(c.capRa, c.capRb)) c.capOut = std::max(c.capOut, dot3(d, c.capN));
		}
		c.capOk = c.capRa > 0.01f && c.capRb > 0.01f;
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: worn gun's cuff (hand %d) - %u of %u end points on its disc, %.3f x %.3f m, %.1f deg off the forearm, lip %.1f mm%s\n",
		       hand, unsigned(cap.size()), unsigned(end.size()), 2 * c.capRa, 2 * c.capRb, acosf(std::min(1.f, fabsf(c.capN.z))) * 57.2958f, c.capOut * 1000.f,
		       c.capOk ? "" : " (too small: no wrist HUD)");
		out = c;
		return c.capOk;
	}
}

void HWDrawInfo::DrawVRHands(FRenderState& state, int bits, AActor* fist)
{
	// The light the gun is drawn in: the weapon layer's, else any layer's.
	HUDSprite* light = nullptr;
	for (auto& h : hudsprites)
	{
		if (h.weapon && h.weapon->GetID() == PSP_WEAPON) { light = &h; break; }
		if (!light) light = &h;
	}
	if (!light) return;

	const uint64_t now = screen->FrameTime;
	AActor* camera = players[consoleplayer].camera;
	player_t* pl = camera ? camera->player : nullptr;
	HandSet* set = ActiveSet(pl);
	if (!set) return;
	auto skinTex = TexMan.GetGameTexture(Skin(pl, set), true);
	if (!skinTex) return;
	// The base skins are made from each game's own pixel art and stay crisp;
	// a style's is a 2048 painted texture, which shimmers unfiltered (the hands
	// project's note: CLAMP_NOFILTER is nearest with no mipmaps).
	const bool crisp = set == &gSets[0];
	// The Gauntlets worn on both hands: the gauntlets are the hands (any
	// style shows through at the seams), the wrist HUD on
	// their cuffs.
	AActor* readyWeapon = camera && camera->player ? camera->player->ReadyWeapon : nullptr;
	const bool worn = !fist && readyWeapon && WornOnBoth(readyWeapon, camera->player->FindPSprite(PSP_WEAPON));

	SetColor(state, Level, lightmode, light->lightlevel, 0, isFullbrightScene(), light->cm, 1.f, true);
	state.SetLightIndex(-1);
	state.SetRenderStyle(DefaultRenderStyle());
	state.SetTextureMode(TM_NORMAL);
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(light->owner && light->owner->Sector ? light->owner->Sector->AdditiveColors[sector_t::sprites] | 0xff000000 : 0);
	state.SetDynLight(light->dynrgb[0], light->dynrgb[1], light->dynrgb[2]);
	state.EnableBrightmap(false);
	state.AlphaFunc(Alpha_GEqual, 0);

	for (int hand = 0; hand < 2; ++hand)
	{
		if (!(bits & (1 << hand))) continue;
		float m[16];
		DoomVR_HandInput in;
		// The weapon hand: on the gun's handle (and its recoil), or a fist that
		// punches, or - a spell - a free open hand.
		const int kind = hand == 0 ? (fist ? HandWeapon(fist) : 3) : 0;
		AActor* weapon = camera && camera->player ? camera->player->ReadyWeapon : nullptr;
		// The other hand, while both hold the gun: on its front grip.
		const int family = HoldFamily(set->dir);
		DPSprite* wpsp = camera && camera->player ? camera->player->FindPSprite(PSP_WEAPON) : nullptr;
		DoomVR_Support support;
		const char* supportPose = nullptr;
		const bool onFront = hand == 1 && !fist && DoomVR_TwoHandedNow() && camera && camera->player &&
		                     SupportFor(weapon, wpsp, false, support, family, &supportPose);
		const bool wornHere = worn && hand == 1;	// the other gauntlet, on this hand
		const int mode = onFront ? 3 : (kind == 3 || wornHere) ? 1 : kind == 1 ? 2 : 0;
		// The gun hand's own hold on a B gun, when these hands have it.
		const HoldEntry* gripHold = (hand == 0 || wornHere) && mode == 1 && IsBGun(wpsp) ? FindHold(0, family, weapon) : nullptr;
		const DoomVR_WeaponTune tune = weapon ? DoomVR_GetTune(TuneName(weapon)) : DoomVR_WeaponTune();
		// The pose must exist in this hand model, or the hold is not used.
		FDoomVRHand* hm = nullptr;
		{
			DoomVR_HandInput probe;
			float pm[16];
			if (DoomVR_HandsMatrix(hand, pm, probe, 0, nullptr)) hm = set->h[probe.physicalLeft ? 1 : 0];
		}
		const int holdPose = hm && gripHold ? hm->hand.FindPose(gripHold->pose.GetChars()) : -1;
		const int holdPull = hm && gripHold ? hm->hand.FindPose(FStringf("%s Pull", gripHold->pose.GetChars()).GetChars()) : -1;
		const int supportHoldPose = hm && supportPose ? hm->hand.FindPose(supportPose) : -1;
		if (supportPose && supportHoldPose < 0) SupportFor(weapon, wpsp, false, support);	// no such pose: the plain fist's placement
		// The player's own tuning of our hand on this weapon (the tuner's
		// HAND and SUPPORT modes).
		const DoomVR_WeaponTune* handTune = weapon && (mode == 1 || mode == 3) ? DoomVR_GetHandTune(weapon->GetClass()->TypeName, mode == 3 ? 1 : 0) : nullptr;
		if (!DoomVR_HandsMatrix(hand, m, in, mode, &tune, set->fist, onFront ? &support : nullptr, holdPose >= 0 ? &gripHold->h : nullptr, handTune)) continue;
		FDoomVRHand* h = set->h[in.physicalLeft ? 1 : 0];
		if (worn && (hand == 0 || wornHere))
		{
			// Not drawn: inside the gauntlet. Its cuff carries the HUD.
			Cut cut;
			if (WornCut(hand, m, weapon, wpsp, tune, handTune, set, in.physicalLeft, cut))
			{
				gDrawnHands[hand].valid = true;
				memcpy(gDrawnHands[hand].m, m, sizeof(m));
				gDrawnHands[hand].cut = cut;
			}
			continue;
		}

		FHWModelRenderer renderer(this, state, light->mframe ? light->lightindex : -1);
		h->BuildVertexBuffer(&renderer);
		h->FindCap(FStringf("%s%s%s", set->dir, *set->dir ? "/" : "", in.physicalLeft ? "hand_l" : "hand_r").GetChars());
		// The fingers, once a frame (both eyes see the same hand).
		if (h->posedAt != now || !h->written)
		{
			const float dt = h->posedAt ? std::min(float(now - h->posedAt) / 1000.f, 0.1f) : 0.f;
			h->posedAt = now;
			gdh::HandInput hi;
			hi.thumbTouch = true;	// resting on the controller
			if (mode == 3 && supportHoldPose >= 0)
			{
				// The support hold round the front grip, fully closed.
				hi.holdPose = supportHoldPose; hi.grip = 1;
			}
			else if (mode == 3)
			{
				// Closed round the front grip (the FRONT-HAND table's pose: a
				// full squeeze, trigger and thumb down).
				hi.trigger = 1; hi.triggerTouch = true; hi.grip = 1;
			}
			else if (holdPose >= 0)
			{
				// The gun's own hold: round the grip from 0.85 of the grip, the
				// index resting on the trigger and pressing it as the RAW
				// trigger goes down (the pull pose stops at the trigger's travel).
				hi.holdPose = holdPose; hi.holdPullPose = holdPull;
				hi.trigger = in.trigger; hi.triggerTouch = true; hi.grip = std::max(in.grip, 0.85f);
			}
			else if (hand == 0 && fist && HandWeapon(fist) == 1)
			{
				hi.trigger = 1; hi.triggerTouch = true; hi.grip = 1;
			}
			else if (hand == 0 && fist)
			{
				// Casting: the hand open, the index finger following the
				// trigger as the other hand's does.
				hi.trigger = in.trigger; hi.triggerTouch = in.trigger > 0.02f; hi.grip = std::min(in.grip, 0.3f);
			}
			else if (hand == 0)
			{
				// Round the gun: a hand holding a weapon is closed on it, the
				// index finger resting on the trigger and following it - only a
				// little: a full pull curled it through the trigger,
				// so the finger goes from resting (0.30) to
				// 0.52 of a fist.
				hi.trigger = in.trigger * 0.32f; hi.triggerTouch = true; hi.grip = std::max(in.grip, 0.85f);
			}
			else
			{
				hi.trigger = in.trigger; hi.triggerTouch = in.trigger > 0.02f; hi.grip = in.grip;
			}
			h->fingers.Update(hi, dt, h->hand);
			h->WriteVertices(&renderer);
		}

		VSMatrix world = renderer.GetViewToWorldMatrix();
		world.multMatrix(m);
		renderer.BeginDrawHUDModel(DefaultRenderStyle(), world, false, 0);
		renderer.SetMaterial(skinTex, crisp, NO_TRANSLATION);
		renderer.SetupFrame(h, 0, 0, h->NumVerts(), -1);
		renderer.DrawElements(h->NumIndices(), 0);
		renderer.EndDrawHUDModel(DefaultRenderStyle(), 0);
		gDrawnHands[hand].valid = true;
		memcpy(gDrawnHands[hand].m, m, sizeof(m));
		gDrawnHands[hand].cut = HandCut(h);
	}

	state.SetVertexBuffer(screen->mVertexData);
	state.SetTextureMode(TM_NORMAL);
	state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.SetDynLight(0, 0, 0);
}

// Desk tests: which weapon the player holds now (and is switching to).
CCMD(vr_debug_weapon)
{
	player_t* pl = &players[consoleplayer];
	// ...and how it is drawn this moment: its picture as a 3D model, or flat.
	DPSprite* psp = pl->FindPSprite(PSP_WEAPON);
	const char* drawn = !psp || !psp->GetState() ? "" : HasModel(psp) ? (OurModel(psp) ? " - our 3D model" : " - a 3D model") : " - flat";
	FString pic;
	if (psp && psp->GetState()) pic.Format(" (%s %c)", sprites[psp->GetSprite()].name, 'A' + psp->GetFrame());
	Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: weapon %s%s%s%s%s\n", pl->ReadyWeapon ? pl->ReadyWeapon->GetClass()->TypeName.GetChars() : "none",
	       pl->PendingWeapon && pl->PendingWeapon != WP_NOCHANGE ? ", switching to " : "",
	       pl->PendingWeapon && pl->PendingWeapon != WP_NOCHANGE ? pl->PendingWeapon->GetClass()->TypeName.GetChars() : "",
	       drawn, pic.GetChars());
}

/*
	Physical play, a wrist HUD: health, armor and
	ammo on the back of the other hand's wrist, read by turning the wrist to the
	face like a watch - it fades in as the wrist faces the eyes and is gone when
	it faces away. The game's own small font (each game's own), on a dark
	plate; the numbers are the game's. The screen HUD stays. Off until worn
	(vr_phys_wrist).

	Drawn in the other hand's own space (DoomVR_HandsMatrix, metres): the back
	of the hand is -x on the left hand and +x on the right, the forearm +z,
	the thumb +y. With the forearm across the chest the lines run left to right
	(towards the left hand's fingers, the right hand's elbow) and their top is
	away from the body, against the thumb.
*/
CVAR(Bool, vr_phys_wrist, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, vr_phys_wrist_place, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// 0 the watch, 1 the wrist's cut, 2 both

/*
	The HUD on the wrist's CUT: these hands float, and
	the flat end of the cuff is seen all the time - so it is a little screen.
	The other hand's shows the health and the armor and, in the Doom games,
	the status bar's own face (looking round, wincing, grinning, as it does
	there); the gun hand's the ready weapon's ammo. A gun held out to aim
	points its wrist's cut at the eyes, and a hand reaching forward (two hands
	on a gun) does the same with the other, so they are read in a fight
	without turning a wrist; a hand down at the side turns it away and it
	fades. Upright to the eyes, whatever the hand's roll; full bright, so it
	reads in a dark room; the game's own status bar numbers (HUDFONT_DOOM,
	HUDFONT_RAVEN) - the armor's in green or blue as its kind. The cut is
	found on whatever hand model is worn (FDoomVRHand::FindCap), so new hands
	need only end in a flat cap.
*/
namespace
{
	FVector3 HandPoint(const float* m, const gdh::Vec3& p)
	{
		return FVector3(m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
		                m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]);
	}
	FVector3 HandDir(const float* m, const gdh::Vec3& d)
	{
		return FVector3(m[0] * d.x + m[4] * d.y + m[8] * d.z, m[1] * d.x + m[5] * d.y + m[9] * d.z, m[2] * d.x + m[6] * d.y + m[10] * d.z);
	}

	FFont* CutFont()
	{
		const bool raven = gameinfo.gametype & (GAME_Heretic | GAME_Hexen);
		FFont* f = V_GetFont(raven ? "HUDFONT_RAVEN" : "HUDFONT_DOOM");
		return f ? f : SmallFont;
	}

	void DrawCut(FRenderState& state, VSMatrix viewMatrix, player_t* pl, int hand, const DrawnHand& d)
	{
		const Cut* h = &d.cut;
		if (!h->capOk) return;
		const float* m = d.m;
		const FVector3 cv = HandPoint(m, h->capC);
		FVector3 nv = HandDir(m, h->capN);
		const float s = nv.Length();	// map units a metre (and the hands' size)
		if (s < 1e-6f) return;
		nv /= s;
		FVector3 toEye = -cv;
		toEye.MakeUnit();
		const float alpha = std::clamp(((nv | toEye) - 0.30f) / 0.30f, 0.f, 1.f);
		if (alpha <= 0.02f) return;

		// What it shows.
		FString big[2];
		EColorRange color[2] = { CR_UNTRANSLATED, CR_UNTRANSLATED };
		int lines = 0;
		FGameTexture* face = nullptr;
		if (hand == 1)
		{
			const bool doom = (gameinfo.gametype & GAME_DoomChex) != 0;
			big[lines++].Format(doom ? "%d%%" : "%d", std::max(0, pl->health));
			if (AActor* armor = pl->mo->FindInventory(NAME_BasicArmor, true))
				if (armor->IntVar(NAME_Amount) > 0)
				{
					big[lines].Format(doom ? "%d%%" : "%d", armor->IntVar(NAME_Amount));
					color[lines++] = armor->FloatVar(NAME_SavePercent) > 0.4 ? CR_BLUE : CR_GREEN;
				}
			if ((gameinfo.gametype & GAME_Doom) && StatusBar) face = StatusBar->mugshot.GetFace(pl, "STF", 5);
		}
		else
		{
			AActor* w = pl->ReadyWeapon;
			AActor* ammo = w ? w->PointerVar<AActor>(NAME_Ammo1) : nullptr;
			if (!ammo) return;	// the fist, the chainsaw: nothing to count
			big[lines++].Format("%d", ammo->IntVar(NAME_Amount));
		}

		// Upright to the eyes: the view's up laid flat on the cut.
		FVector3 up = FVector3(0, 1, 0) - nv * nv.Y;
		if (up.Length() < 0.2f) up = HandDir(m, h->capB);
		up.MakeUnit();
		FVector3 right = up ^ nv;
		right.MakeUnit();
		// Just over the cuff's lip, so a lip standing proud of the cut never hides it.
		const FVector3 o = cv + nv * ((h->capOut + 0.0015f) * s);
		const float pm[16] = {
			right.X * s, right.Y * s, right.Z * s, 0,
			up.X * s, up.Y * s, up.Z * s, 0,
			nv.X * s, nv.Y * s, nv.Z * s, 0,
			o.X, o.Y, o.Z, 1,
		};
		VSMatrix world;
		viewMatrix.inverseMatrix(world);
		world.multMatrix(pm);
		state.mModelMatrix = world;
		state.EnableModelMatrix(true);

		auto quad = [&](float x0, float y0, float x1, float y1) {
			auto v = screen->mVertexData->AllocVertices(4);
			v.first[0].Set(x0, y1, 0, 0, 0);
			v.first[1].Set(x0, y0, 0, 0, 1);
			v.first[2].Set(x1, y1, 0, 1, 0);
			v.first[3].Set(x1, y0, 0, 1, 1);
			state.Draw(DT_TriangleStrip, v.second, 4);
		};

		// The screen: the cut's own oval, dark, a little inside its edge.
		{
			const FVector3 av = HandDir(m, h->capA) / s, bv = HandDir(m, h->capB) / s;
			const float ax = av | right, ay = av | up, bx = bv | right, by = bv | up;
			constexpr int kSeg = 32;
			auto v = screen->mVertexData->AllocVertices(kSeg + 2);
			v.first[0].Set(0, 0, 0, 0, 0);
			for (int i = 0; i <= kSeg; ++i)
			{
				const float th = float(i) / kSeg * 6.2831853f;
				const float ca = cosf(th) * h->capRa * 0.92f, sb = sinf(th) * h->capRb * 0.92f;
				v.first[i + 1].Set(ca * ax + sb * bx, ca * ay + sb * by, 0, 0, 0);
			}
			state.EnableTexture(false);
			state.SetColor(0.02f, 0.025f, 0.03f, 0.88f * alpha);
			state.Draw(DT_TriangleFan, v.second, kSeg + 2);
		}

		// The content fits the circle inside the cut.
		const float R = std::min(h->capRa, h->capRb) * 0.88f;
		FFont* font = CutFont();
		auto width = [&](const FString& str, float hgt) {
			const float k = hgt / std::max(1, font->GetHeight());
			float wsum = 0;
			for (const char* c = str.GetChars(); *c; ++c)
			{
				int w = 0;
				font->GetChar((unsigned char)*c, CR_UNTRANSLATED, &w);
				wsum += float(w > 0 ? w : font->GetCharWidth((unsigned char)*c)) * k;
			}
			return wsum;
		};
		auto text = [&](const FString& str, float x, float yTop, float hgt, EColorRange cr) {
			const float k = hgt / std::max(1, font->GetHeight());
			const FTranslationID tr = cr == CR_UNTRANSLATED ? NO_TRANSLATION : font->GetColorTranslation(cr);
			for (const char* c = str.GetChars(); *c; ++c)
			{
				int w = 0;
				FGameTexture* tex = font->GetChar((unsigned char)*c, CR_UNTRANSLATED, &w);
				if (tex)
				{
					state.SetMaterial(tex, UF_Font, 0, CLAMP_XY_NOMIP, tr, -1);
					quad(x, yTop - float(tex->GetDisplayHeight()) * k, x + float(tex->GetDisplayWidth()) * k, yTop);
				}
				x += float(w > 0 ? w : font->GetCharWidth((unsigned char)*c)) * k;
			}
		};
		state.EnableTexture(true);
		state.SetColor(1.f, 1.f, 1.f, alpha);
		state.AlphaFunc(Alpha_GEqual, 0.1f);
		if (hand == 1)
		{
			// The face on the left, the numbers on the right, a line each.
			if (face)
			{
				const float fh = 0.86f * R, fw = fh * float(face->GetDisplayWidth()) / std::max(1.f, float(face->GetDisplayHeight()));
				state.SetMaterial(face, UF_Sprite, 0, CLAMP_XY_NOMIP, NO_TRANSLATION, -1);
				quad(-0.06f * R - fw, -fh / 2, -0.06f * R, fh / 2);
			}
			// One height for both lines, as large as fits right of the face.
			float hgt = lines > 1 ? 0.32f * R : 0.42f * R;
			const float lx0 = face ? -0.02f * R : 0.f, room = face ? 0.90f * R : 1.6f * R;
			float widest = 0;
			for (int i = 0; i < lines; ++i) widest = std::max(widest, width(big[i], hgt));
			if (widest > room) hgt *= room / widest;
			for (int i = 0; i < lines; ++i)
			{
				const float yTop = lines > 1 ? (i == 0 ? 0.04f * R + hgt : -0.04f * R) : hgt / 2;
				const float lx = face ? lx0 : -width(big[i], hgt) / 2;
				text(big[i], lx, yTop, hgt, color[i]);
			}
		}
		else
		{
			const float hgt = 0.62f * R;
			float hh = hgt;
			const float w = width(big[0], hh);
			if (w > 1.7f * R) hh *= 1.7f * R / w;
			text(big[0], -width(big[0], hh) / 2, hh / 2, hh, CR_UNTRANSLATED);
		}
		state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
	}
}

void HWDrawInfo::DrawVRWrist(FRenderState& state)
{
	// Each hand's last drawing is this eye's only: used here, then gone.
	DrawnHand drawn[2] = { gDrawnHands[0], gDrawnHands[1] };
	gDrawnHands[0].valid = gDrawnHands[1].valid = false;
	if (!vr_phys_wrist || !DoomVR_Active() || SmallFont == nullptr) return;
	AActor* camera = players[consoleplayer].camera;
	player_t* pl = camera ? camera->player : nullptr;
	if (!pl || !pl->mo || (pl->cheats & CF_CHASECAM)) return;

	if (vr_phys_wrist_place >= 1)
	{
		state.SetDepthMask(false);
		state.SetRenderStyle(STYLE_Translucent);
		state.SetTextureMode(TM_NORMAL);
		state.SetLightIndex(-1);
		state.SetObjectColor(0xffffffff);
		state.SetAddColor(0);
		for (int hand = 0; hand < 2; ++hand)
			if (drawn[hand].valid) DrawCut(state, VPUniforms.mViewMatrix, pl, hand, drawn[hand]);
		state.SetDepthMask(true);
		state.EnableModelMatrix(false);
		state.ResetColor();
		state.SetVertexBuffer(screen->mVertexData);
		if (vr_phys_wrist_place == 1) return;
	}

	float m[16];
	DoomVR_HandInput in;
	if (!DoomVR_HandsMatrix(1, m, in, 0, nullptr)) return;
	const float back = in.physicalLeft ? -1.f : 1.f;		// the back of the hand
	const float along = in.physicalLeft ? -1.f : 1.f;		// the text runs to the fingers (left hand) or the elbow (right)

	// Facing: the plate's normal against the way to the eye, in view space.
	const FVector3 c(back * 0.042f, 0.f, 0.055f);			// on the back of the wrist
	auto xform = [&](const FVector3& p) {
		return FVector3(m[0] * p.X + m[4] * p.Y + m[8] * p.Z + m[12], m[1] * p.X + m[5] * p.Y + m[9] * p.Z + m[13],
		                m[2] * p.X + m[6] * p.Y + m[10] * p.Z + m[14]);
	};
	const FVector3 cv = xform(c);
	FVector3 nv = xform(FVector3(back, 0.f, 0.f)) - xform(FVector3(0.f, 0.f, 0.f));
	nv.MakeUnit();
	FVector3 toEye = -cv;
	toEye.MakeUnit();
	const float facing = nv | toEye;
	const float alpha = std::clamp((facing - 0.35f) / 0.35f, 0.f, 1.f);
	if (alpha <= 0.02f) return;

	// The lines, as the game counts them.
	FString lines[3];
	int n = 0;
	lines[n++].Format("HP %d", pl->health);
	if (AActor* armor = pl->mo->FindInventory(NAME_BasicArmor, true))
		if (armor->IntVar(NAME_Amount) > 0) lines[n++].Format("AR %d", armor->IntVar(NAME_Amount));
	if (AActor* w = pl->ReadyWeapon)
		if (AActor* ammo = w->PointerVar<AActor>(NAME_Ammo1)) lines[n++].Format("AMMO %d", ammo->IntVar(NAME_Amount));

	// Plate space: u along the forearm (towards the fingers), v towards the
	// thumb, both in metres from the plate's middle, lifted off the skin.
	const float lineH = 0.013f, k = lineH / std::max(1, SmallFont->GetHeight());
	const float plateW = 0.075f, plateH = lineH * 1.25f * n + 0.008f;
	// Up on the plate is AWAY from the body, which is against the thumb: a
	// watch read with the forearm across the chest has its top away from you.
	auto P = [&](float u, float v) { return FVector3(c.X + back * 0.002f, c.Y - v, c.Z + along * u); };

	VSMatrix world;
	VPUniforms.mViewMatrix.inverseMatrix(world);
	world.multMatrix(m);
	state.mModelMatrix = world;
	state.EnableModelMatrix(true);
	state.SetDepthMask(false);
	state.SetRenderStyle(STYLE_Translucent);
	state.SetTextureMode(TM_NORMAL);
	state.SetLightIndex(-1);
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);

	auto quad = [&](float u0, float v0, float u1, float v1, float s0, float t0, float s1, float t1) {
		auto v = screen->mVertexData->AllocVertices(4);
		const FVector3 a = P(u0, v1), b = P(u0, v0), d = P(u1, v1), e = P(u1, v0);
		v.first[0].Set(a.X, a.Y, a.Z, s0, t0);
		v.first[1].Set(b.X, b.Y, b.Z, s0, t1);
		v.first[2].Set(d.X, d.Y, d.Z, s1, t0);
		v.first[3].Set(e.X, e.Y, e.Z, s1, t1);
		state.Draw(DT_TriangleStrip, v.second, 4);
	};
	state.EnableTexture(false);
	state.SetColor(0.02f, 0.02f, 0.03f, 0.6f * alpha);
	quad(-plateW / 2, -plateH / 2, plateW / 2, plateH / 2, 0, 0, 1, 1);
	state.EnableTexture(true);
	state.SetColor(1.f, 1.f, 1.f, alpha);
	state.AlphaFunc(Alpha_GEqual, 0.1f);
	for (int i = 0; i < n; ++i)
	{
		float u = -plateW / 2 + 0.006f;
		const float vTop = plateH / 2 - 0.004f - lineH * 1.25f * i;
		for (const char* s = lines[i].GetChars(); *s; ++s)
		{
			int w = 0;
			FGameTexture* tex = SmallFont->GetChar((unsigned char)*s, CR_UNTRANSLATED, &w);
			if (tex)
			{
				state.SetMaterial(tex, UF_Font, 0, CLAMP_XY_NOMIP, NO_TRANSLATION, -1);
				const float tw = float(tex->GetDisplayWidth()) * k, th = float(tex->GetDisplayHeight()) * k;
				quad(u, vTop - th, u + tw, vTop, 0, 0, 1, 1);
			}
			u += float(w > 0 ? w : SmallFont->GetSpaceWidth()) * k;
		}
	}
	state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
	state.SetDepthMask(true);
	state.EnableModelMatrix(false);
	state.ResetColor();
	state.SetVertexBuffer(screen->mVertexData);
}

/*
	Physical play, the two holsters as the player sees them (Game Or Die
	Features 23.1): the gun in each is drawn on the body, its own 3D model at
	its own size (the hip gun at the belt line, the long gun slung on the
	back); as the gun hand comes within 35 cm a ring fades in where it is
	worn - turquoise while near, orange while a grip would draw - with the
	gun's own name beside it, or EMPTY, and the gun glows. The first time
	holsters are in use, one card says where everything is (once per install:
	vr_phys_holster_card). Only with holsters on, never in a menu.
*/
CVAR(Bool, vr_phys_holster_card, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// the first-time card is still to show

namespace
{
	// The game's own name for a gun, upper case, or EMPTY.
	FString HolsterLabel(AActor* gun)
	{
		FString t = gun ? FString(gun->GetTag()) : FString("EMPTY");
		t.ToUpper();
		return t;
	}
}

void HWDrawInfo::DrawVRHolsters(FRenderState& state)
{
	if (!DoomVR_Active() || SmallFont == nullptr) return;
	AActor* camera = players[consoleplayer].camera;
	player_t* pl = camera ? camera->player : nullptr;
	if (!pl || !pl->mo || (pl->cheats & CF_CHASECAM) || pl->playerstate != PST_LIVE || camera != pl->mo) return;

	float marks[2][16];
	double dist[2];
	bool ready[2];
	for (int i = 0; i < 2; ++i)
		if (!DoomVR_HolsterMarker(i, marks[i], dist[i], ready[i])) return;
	AActor* guns[2] = { HolsterGun(pl, 1), HolsterGun(pl, 2) };
	AActor* held = Unpowered(pl->ReadyWeapon);

	if (vr_phys_holster_card && gamestate == GS_LEVEL)
	{
		vr_phys_holster_card = false;
		C_MidPrint(nullptr, guns[0] && guns[1]
			? FStringf("Your %s is on your hip. Your %s is over your shoulder.\nReach there and squeeze grip. Squeeze there again to put it back.", guns[0]->GetTag(), guns[1]->GetTag()).GetChars()
			: "Holsters: your one-handed gun is on your hip, your two-handed gun\nover your shoulder. Reach there and squeeze grip.");
	}

	// The guns on the body, lit as where the player stands; glowing in reach.
	FColormap cm;
	cm.Clear();
	const int light = pl->mo->Sector ? pl->mo->Sector->lightlevel : 192;
	for (int i = 0; i < 2; ++i)
	{
		AActor* gun = guns[i];
		if (!gun || gun == held) continue;
		FState* readyState = gun->FindState(NAME_Ready);
		FSpriteModelFrame* smf = readyState ? FindModelFrame(gun, readyState->sprite, readyState->Frame, false) : nullptr;
		if (!smf || !DoomVR_SetHolsterModel(i)) continue;
		const bool glow = dist[i] <= 0.0;
		SetColor(state, Level, lightmode, glow ? 255 : std::max(light, 96), 0, false, cm, 1.f, true);
		state.SetRenderStyle(DefaultRenderStyle());
		state.SetTextureMode(TM_NORMAL);
		state.SetLightIndex(-1);
		state.SetObjectColor(0xffffffff);
		state.SetAddColor(glow ? 0xff302010 : 0);
		state.AlphaFunc(Alpha_GEqual, 0);
		FHWModelRenderer renderer(this, state, -1);
		DoomVR_SetDrawingHand(0);
		DoomVR_SetDrawingTune(DoomVR_GetTune(gun->GetClass()->TypeName));
		DoomVR_RenderWheelModel(&renderer, gun, smf, readyState);
		DoomVR_SetHolsterModel(-1);
		DoomVR_SetDrawingTune(DoomVR_WeaponTune());
		state.SetAddColor(0);
		state.SetVertexBuffer(screen->mVertexData);
	}

	// The rings and the labels.
	bool begun = false;
	for (int i = 0; i < 2; ++i)
	{
		const float near = 0.35f;
		const float alpha = dist[i] <= 0.0 ? 1.f : std::clamp(float((near - dist[i]) / near) * 1.4f, 0.f, 1.f);
		if (alpha <= 0.02f) continue;
		if (!begun)
		{
			begun = true;
			state.EnableModelMatrix(true);
			state.SetDepthMask(false);
			state.SetRenderStyle(STYLE_Translucent);
			state.SetTextureMode(TM_NORMAL);
			state.SetLightIndex(-1);
			state.SetObjectColor(0xffffffff);
			state.SetAddColor(0);
		}
		VSMatrix world;
		VPUniforms.mViewMatrix.inverseMatrix(world);
		world.multMatrix(marks[i]);
		state.mModelMatrix = world;

		// The ring: 7 cm across, 6 mm thick, facing the eye.
		const float ro = 0.035f, ri = 0.029f;
		const int seg = 32;
		state.EnableTexture(false);
		if (ready[i]) state.SetColor(1.f, 0.459f, 0.094f, 0.9f * alpha);		// Game Or Die orange #FF7518
		else state.SetColor(0.012f, 0.757f, 0.796f, 0.75f * alpha);			// Game Or Die turquoise #03C1CB
		auto v = screen->mVertexData->AllocVertices(2 * (seg + 1));
		for (int k = 0; k <= seg; ++k)
		{
			const float a = float(k) * 6.2831853f / seg, c = cosf(a), sn = sinf(a);
			v.first[2 * k].Set(c * ro, sn * ro, 0.f, 0, 0);
			v.first[2 * k + 1].Set(c * ri, sn * ri, 0.f, 0, 0);
		}
		state.Draw(DT_TriangleStrip, v.second, 2 * (seg + 1));

		// The label above the ring: the game's own small font, 2 cm tall.
		const FString label = HolsterLabel(guns[i]);
		const float k = 0.020f / std::max(1, SmallFont->GetHeight());
		float width = 0;
		for (const char* c = label.GetChars(); *c; ++c) { int w = 0; SmallFont->GetChar((unsigned char)*c, CR_UNTRANSLATED, &w); width += w * k; }
		float x = -width / 2;
		const float y = ro + 0.018f;
		state.EnableTexture(true);
		state.SetColor(1.f, 1.f, 1.f, alpha);
		state.AlphaFunc(Alpha_GEqual, 0.1f);
		for (const char* c = label.GetChars(); *c; ++c)
		{
			int w = 0;
			FGameTexture* tex = SmallFont->GetChar((unsigned char)*c, CR_UNTRANSLATED, &w);
			if (tex)
			{
				const float tw = float(tex->GetDisplayWidth()) * k, th = float(tex->GetDisplayHeight()) * k;
				state.SetMaterial(tex, UF_Font, 0, CLAMP_XY_NOMIP, NO_TRANSLATION, -1);
				auto q = screen->mVertexData->AllocVertices(4);
				q.first[0].Set(x, y + th, 0.f, 0, 0);
				q.first[1].Set(x, y, 0.f, 0, 1);
				q.first[2].Set(x + tw, y + th, 0.f, 1, 0);
				q.first[3].Set(x + tw, y, 0.f, 1, 1);
				state.Draw(DT_TriangleStrip, q.second, 4);
			}
			x += w * k;
		}
		state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
	}
	if (begun)
	{
		state.SetDepthMask(true);
		state.EnableModelMatrix(false);
		state.ResetColor();
		state.EnableTexture(true);
		state.SetVertexBuffer(screen->mVertexData);
	}
}

/*
	The weapon wheel's guns and its look (the button, the ring's place and the
	slow motion are doomvr_headset.cpp's). The guns carried, in the game's own
	slot order (1-9, then 0), at most 12; a Tome of Power's other half is left
	out. Each is its own 3D model where the game has one (its Ready frame, the
	held gun's model and fit, side on), else
	its pickup sprite, flat (the fist, which has none: its own picture). Every
	model is centred on its spot and made vr_wheel_model_length long from its
	measured extent (models.cpp), so big and small guns sit alike; the ring
	widens with more than eight. The disc behind them: a dark slice per gun, the
	one under the hand lit in the game's colour; in the hub, slowly turning,
	a symbol from the game's own art (Doom's soulsphere, Heretic's Tome of
	Power). A gun with no ammo is drawn dim.
*/
CVAR(Float, vr_wheel_ring_radius, 0.22f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// m, at the least; 2.6 cm a gun past 8
CVAR(Float, vr_wheel_model_length, 0.15f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// m, each gun's longest side

namespace
{
	struct WheelGun
	{
		PClassActor* cls;
		FState* ready;
		FSpriteModelFrame* smf;
		FTextureID icon;
		bool current, loaded;
	};
	TArray<WheelGun> gWheelGuns;

	float WheelRadius(int n) { return std::max(float(vr_wheel_ring_radius), 0.026f * n); }

	// The game's colour, and its hub symbol's sprite.
	void WheelLook(float rgb[3], const char*& hub)
	{
		hub = nullptr;
		auto set = [&](int r, int g, int b) { rgb[0] = r / 255.f; rgb[1] = g / 255.f; rgb[2] = b / 255.f; };
		switch (gameinfo.gametype)
		{
		case GAME_Heretic: set(70, 175, 80); hub = "PWBKA0"; break;	// emerald; the Tome of Power
		case GAME_Hexen: set(120, 90, 215); break;						// violet
		case GAME_Strife: set(215, 155, 45); break;						// amber
		case GAME_Chex: set(90, 200, 110); break;						// Chex green
		default: set(215, 45, 25); hub = "SOULA0"; break;				// Doom red; the soulsphere
		}
	}
}

int DoomVR_WheelGather()
{
	gWheelGuns.Clear();
	player_t* pl = &players[consoleplayer];
	if (!pl->mo) return 0;
	for (int k = 1; k <= NUM_WEAPON_SLOTS && gWheelGuns.Size() < 12; ++k)
	{
		const int slot = k % NUM_WEAPON_SLOTS;
		for (int j = 0; j < pl->weapons.SlotSize(slot) && gWheelGuns.Size() < 12; ++j)
		{
			PClassActor* cls = pl->weapons.GetWeapon(slot, j);
			AActor* inv = cls ? pl->mo->FindInventory(cls) : nullptr;
			if (!inv || (inv->IntVar(NAME_WeaponFlags) & WIF_POWERED_UP)) continue;
			WheelGun g{};
			g.cls = cls;
			g.ready = inv->FindState(NAME_Ready);
			g.smf = g.ready ? FindModelFrame(inv, g.ready->sprite, g.ready->Frame, false) : nullptr;
			// The pickup's picture; a weapon never lying about (the fist) shows its own.
			auto pictureOf = [](FState* st) {
				FTextureID id;
				if (st && unsigned(st->sprite) < sprites.Size() && strnicmp(sprites[st->sprite].name, "TNT1", 4) != 0)
				{
					bool mirror;
					id = sprites[st->sprite].GetSpriteFrame(st->GetFrame(), 0, nullAngle, &mirror);
				}
				return id;
			};
			if (const AActor* def = GetDefaultByType(cls)) g.icon = pictureOf(def->SpawnState);
			if (!g.icon.isValid()) g.icon = pictureOf(g.ready);
			AActor* ready = pl->ReadyWeapon;
			g.current = ready && (ready == inv || ready->PointerVar<AActor>(NAME_SisterWeapon) == inv);
			AActor* ammo = inv->PointerVar<AActor>(NAME_Ammo1);
			g.loaded = !ammo || ammo->IntVar(NAME_Amount) > 0;
			if (!g.smf && !g.icon.isValid()) continue;	// nothing to show; the slots still reach it
			// The same gun twice: Legacy of Rust's Incinerator is Doom's
			// PlasmaRifle drawing its art, and idkfa also gives id24res's own
			// ID24Incinerator, which looks the same. One place on the ring,
			// the one in hand if either is.
			bool twin = false;
			for (auto& o : gWheelGuns)
			{
				if (!o.ready || !g.ready || o.ready->sprite != g.ready->sprite || o.ready->Frame != g.ready->Frame) continue;
				if (g.current) o = g;
				twin = true;
				break;
			}
			if (twin) continue;
			gWheelGuns.Push(g);
		}
	}
	return int(gWheelGuns.Size());
}

void DoomVR_WheelChoose(int i)
{
	if (i < 0 || i >= int(gWheelGuns.Size()) || gWheelGuns[i].current) return;	// the gun in hand stays
	C_DoCommand(FStringf("use %s", gWheelGuns[i].cls->TypeName.GetChars()).GetChars());
	Printf(PRINT_NONOTIFY, "DoomVR: weapon wheel - %s\n", gWheelGuns[i].cls->TypeName.GetChars());
}

CCMD(vrwheel)
{
	const int n = DoomVR_WheelGather();
	Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: weapon wheel would show %d\n", n);
	for (int i = 0; i < n; ++i)
		Printf("  %2d %-24s %s%s%s\n", i, gWheelGuns[i].cls->TypeName.GetChars(), gWheelGuns[i].smf ? "3D model" : "sprite",
		       gWheelGuns[i].current ? ", in hand" : "", gWheelGuns[i].loaded ? "" : ", no ammo");
}

void HWDrawInfo::DrawVRWheel(FRenderState& state)
{
	int n, lit;
	if (!DoomVR_WheelState(n, lit) || n <= 0 || n != int(gWheelGuns.Size())) return;
	float fm[16];
	if (!DoomVR_WheelFrame(fm)) return;

	state.Clear(CT_Depth);
	VSMatrix ring;
	VPUniforms.mViewMatrix.inverseMatrix(ring);
	ring.multMatrix(fm);
	auto ringSpace = [&]() {
		state.mModelMatrix = ring;
		state.EnableModelMatrix(true);
		state.SetRenderStyle(STYLE_Translucent);
		state.SetTextureMode(TM_NORMAL);
		state.SetLightIndex(-1);
		state.SetObjectColor(0xffffffff);
		state.SetAddColor(0);
		state.SetDynLight(0, 0, 0);
	};
	ringSpace();
	state.SetDepthMask(false);
	state.EnableTexture(false);

	float rgb[3];
	const char* hubName;
	WheelLook(rgb, hubName);
	const float R = WheelRadius(n), outer = R + 0.07f, inner = outer * 0.22f, zDisc = -0.03f;
	const float d2r = float(M_PI / 180.0);
	// One slice: a band from inner to outer between two angles (degrees).
	auto band = [&](float a0, float a1, float r0, float r1, float z) {
		const int steps = std::max(2, int((a1 - a0) / 3.f));
		auto v = screen->mVertexData->AllocVertices(2 * (steps + 1));
		for (int k = 0; k <= steps; ++k)
		{
			const float a = (a0 + (a1 - a0) * k / steps) * d2r;
			v.first[2 * k].Set(cosf(a) * r1, sinf(a) * r1, z, 0, 0);
			v.first[2 * k + 1].Set(cosf(a) * r0, sinf(a) * r0, z, 0, 0);
		}
		state.Draw(DT_TriangleStrip, v.second, 2 * (steps + 1));
	};
	for (int i = 0; i < n; ++i)
	{
		const float mid = float(DoomVR_WheelAngle(i, n)), half = 180.f / n, gap = n > 1 ? 1.2f : 0.f;
		const bool on = i == lit;
		if (on) state.SetColor(rgb[0], rgb[1], rgb[2], 0.92f);
		else state.SetColor(0.094f, 0.094f, 0.11f, 0.78f);
		band(mid - half + gap, mid + half - gap, inner, outer, zDisc);
		// A thin rim, so the disc reads against a busy level.
		if (on) state.SetColor(std::min(1.f, rgb[0] * 1.15f), std::min(1.f, rgb[1] * 1.15f), std::min(1.f, rgb[2] * 1.15f), 1.f);
		else state.SetColor(0.35f, 0.35f, 0.38f, 0.9f);
		band(mid - half + gap, mid + half - gap, outer - 0.006f, outer, zDisc + 0.0005f);
	}

	// A flat picture facing the player, square, turned by roll (degrees).
	auto picture = [&](FGameTexture* tex, float x, float y, float z, float size, float roll, float shade, float alpha) {
		if (!tex) return;
		const float w = float(tex->GetDisplayWidth()), h = float(tex->GetDisplayHeight());
		const float sx = w >= h ? size / 2 : size / 2 * w / h, sy = w >= h ? size / 2 * h / w : size / 2;
		const float c = cosf(roll * d2r), s = sinf(roll * d2r);
		auto P = [&](float u, float v) { return FVector2(x + u * c - v * s, y + u * s + v * c); };
		const FVector2 a = P(-sx, sy), b = P(-sx, -sy), d = P(sx, sy), e = P(sx, -sy);
		state.EnableTexture(true);
		state.SetColor(shade, shade, shade, alpha);
		state.SetMaterial(tex, UF_Sprite, 0, CLAMP_XY_NOMIP, NO_TRANSLATION, -1);
		auto v = screen->mVertexData->AllocVertices(4);
		v.first[0].Set(a.X, a.Y, z, 0, 0);
		v.first[1].Set(b.X, b.Y, z, 0, 1);
		v.first[2].Set(d.X, d.Y, z, 1, 0);
		v.first[3].Set(e.X, e.Y, z, 1, 1);
		state.Draw(DT_TriangleStrip, v.second, 4);
	};
	state.AlphaFunc(Alpha_GEqual, 0.1f);
	if (hubName)
	{
		const FTextureID id = TexMan.CheckForTexture(hubName, ETextureType::Sprite);
		const double secs = I_msTime() / 1000.0;	// steady: the slow motion scales game time, not this
		if (id.isValid()) picture(TexMan.GetGameTexture(id, true), 0, 0, 0.005f, 0.07f, float(fmod(secs * 90.0, 360.0)), 1.f, 1.f);
	}

	// The guns.
	FColormap cm;
	cm.Clear();
	state.SetDepthMask(true);
	for (int i = 0; i < n; ++i)
	{
		const WheelGun& g = gWheelGuns[i];
		const float a = float(DoomVR_WheelAngle(i, n)) * d2r;
		const float x = cosf(a) * R, y = sinf(a) * R;
		const bool on = i == lit;
		const int level = on ? 255 : !g.loaded ? 110 : g.current ? 230 : 190;
		if (g.smf)
		{
			SetColor(state, Level, lightmode, level, 0, false, cm, 1.f, true);
			state.SetRenderStyle(DefaultRenderStyle());
			state.SetTextureMode(TM_NORMAL);
			state.SetLightIndex(-1);
			state.SetObjectColor(0xffffffff);
			state.SetAddColor(0);
			state.AlphaFunc(Alpha_GEqual, 0);
			FHWModelRenderer renderer(this, state, -1);
			DoomVR_SetDrawingHand(0);
			DoomVR_SetDrawingTune(DoomVR_GetTune(g.cls->TypeName));
			DoomVR_SetWheelModel(true, x, y, float(vr_wheel_model_length) * (on ? 1.3f : 1.f));
			AActor* inv = players[consoleplayer].mo->FindInventory(g.cls);
			if (inv) DoomVR_RenderWheelModel(&renderer, inv, g.smf, g.ready);
			DoomVR_SetWheelModel(false, 0, 0, 1);
			DoomVR_SetDrawingTune(DoomVR_WeaponTune());
			state.SetVertexBuffer(screen->mVertexData);
		}
		else
		{
			ringSpace();
			state.AlphaFunc(Alpha_GEqual, 0.1f);
			picture(TexMan.GetGameTexture(g.icon, true), x, y, 0.01f, 0.10f * (on ? 1.35f : 1.f), 0, level / 255.f, g.loaded ? 1.f : 0.6f);
		}
	}

	state.EnableTexture(true);
	state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
	state.SetDepthMask(true);
	state.EnableModelMatrix(false);
	state.ResetColor();
	state.SetVertexBuffer(screen->mVertexData);
	state.SetTextureMode(TM_NORMAL);
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
}

// A game switched in this process (Switch Game): everything here that holds
// the old game's classes, models or textures, or was read from its files, is
// let go, to be found again in the next.
void DoomVR_HandsForgetGame()
{
	gSkinCache.clear();
	for (auto& t : gHoldTable) for (auto& v : t) v.clear();
	gHoldsLoaded = false;
	for (auto& c : gWornCut) c = WornCutCache();
	gDrawnHands[0].valid = gDrawnHands[1].valid = false;
	gWheelGuns.Clear();
	for (auto& g : gHolsterLast) g = nullptr;
}
