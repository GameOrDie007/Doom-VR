/*
** doomvr_headset.cpp
**
** The headset: DoomVR's link between UZDoom's stereo framework and pcvr-xr.
**
** Written for Doom VR from UZDoom's VRMode interface and
** render loop (hw_entrypoint.cpp, gl_stereo3d.cpp, gl_framebuffer.cpp),
** pcvr-xr's public API (pcvrxr.h) and the OpenXR 1.1 specification - the
** reference-space conventions in section 2.16 and the view/FOV definitions
** in 11.4.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>
#include <time.h>
#include <sys/stat.h>
#include "doomvr_headset.h"
#include "i_time.h"
#include "c_cvars.h"
#include "printf.h"
#include "c_dispatch.h"
#include "keydef.h"
#include "c_bind.h"
#include "d_eventbase.h"
#include "v_draw.h"
#include "v_video.h"
#include "m_joy.h"
#include "menustate.h"
#include "c_console.h"
#include "gamestate.h"

EXTERN_CVAR(Int, vr_mode)
EXTERN_CVAR(Float, vr_hunits_per_meter)

// Synthetic head rotation, added to whatever the runtime reports. A headset
// that is not being worn - or no headset at all - never turns, so without
// these there is no way to ask "does the view follow the head, and which way"
// from a desk. Not archived: a test value must not survive into a play session.
CVAR(Float, vr_debug_yaw, 0.f, 0)
CVAR(Float, vr_debug_pitch, 0.f, 0)
CVAR(Float, vr_debug_roll, 0.f, 0)
// The aim of an Index, Vive or Windows Mixed Reality controller laid along its
// grip as Quest Touch reports it (Hand(), below). Off = the runtime's own aim.
CVAR(Bool, vr_aim_from_grip, true, 0)
// A synthetic tilt of the HEAD, through the layer, so the eye pictures and
// the views the layer reports with them agree, as a real head's do (the
// spectator view's check needs that). vr_debug_pitch/roll above turn only the
// engine's camera. Up and right-ear-down are positive. Test hooks, not saved.
CVAR(Float, vr_debug_head_pitch, 0.f, 0)
CVAR(Float, vr_debug_head_roll, 0.f, 0)

// Log every call into the layer for the first N frames, then every 90th: which
// call, how long it took, and what the runtime said. For a run that stops
// moving - the last line printed is the call that did not come back.
CVAR(Int, vr_debug_trace, 0, 0)

// Turning with the right stick: 0 turns smoothly at vr_turn_speed degrees a
// second; anything else turns that many degrees at once, one step per push.
CVAR(Int, vr_snap_turn, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_turn_speed, 120.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// the house speed, every port
// What the left stick walks towards: 0 where the head looks, 1 where the left
// controller points.
CVAR(Int, vr_move_follows, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Synthetic head movement in metres (right, up, back - OpenXR's axes), added
// to every pose the runtime reports. A null headset never moves, so without
// this there is no way to ask whether leaning moves the view, and which way.
// Not archived.
CVAR(Float, vr_debug_head_x, 0.f, 0)
CVAR(Float, vr_debug_head_y, 0.f, 0)
CVAR(Float, vr_debug_head_z, 0.f, 0)

// Room-scale: the body walks where the head walks (p_user.cpp moves it, with
// collision, each tic). The head's walk the body has CARRIED, metres along
// the recenter frame's right and forward, now and at the start of this tic -
// the eyes and hands show only what it has not carried, blended across the
// tic as the body's own position is, so the view never jumps at a tic.
CVAR(Bool, vr_room_scale, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
static double gCarried[2] = {}, gCarriedPrev[2] = {};

// The wall fade: when the head goes into or through a wall (the body stopped
// at it, or a lean), the eye fades to black instead of showing the inside of
// the map. hw_entrypoint.cpp traces from the body's eye point to the real
// eye each frame and sets how far past the wall it is.
CVAR(Bool, vr_wall_fade, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// The comfort vignette: 0 off, 1 light, 2 strong. While the sticks move or
// turn the player, the edge of the view darkens (the motion the eyes see
// and the inner ear does not, narrowed). Off by default - Doom players are
// used to moving - and one row away for anyone it helps.
CVAR(Int, vr_comfort_vignette, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// The menu pointer: while a menu is up, the right controller points at the
// menu screen like a laser - a dot where it lands, the trigger clicks - as
// the mouse would. Declared here so the menu row exists in every build.
CVAR(Bool, vr_menu_pointer, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Crouch for real (DoomVR_RealCrouch, below) - declared here so the menu row exists in every build.
CVAR(Bool, vr_real_crouch, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
static float gEyeFade = 0;
bool DoomVR_WallFadeEnabled() { return vr_wall_fade; }
void DoomVR_SetEyeFade(float alpha) { gEyeFade = alpha < 0 ? 0 : (alpha > 1 ? 1 : alpha); }
float DoomVR_EyeFade() { return vr_wall_fade ? gEyeFade : 0.f; }

// The flat panel's width in metres, and how far away it hangs. A 2.4 m wide
// screen 2 m away is about 62 degrees across.
CVAR(Float, vr_panel_width, 2.4f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// A standalone headset's refresh rate, asked for when the session comes up
// (XR_FB_display_refresh_rate): 90 on a Quest 3, the rate the game is paced
// at. 0 leaves the runtime's default (a Quest starts every app at 72 Hz).
// Only the headset build uses it; the PC runs at the runtime's own rate.
CVAR(Int, vr_refresh_hz, 90, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_panel_distance, 2.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// How fast the left stick walks, as a share of Doom's running speed: 0.5 is
// Doom's walking speed. Holding the left stick in runs at full speed.
CVAR(Float, vr_move_speed, 0.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// The flat game's head bob, which in a headset moves the player's eyes
// without their head moving. Off by default; the weapon never bobs in the
// hand either way - it is where the controller is.
CVAR(Bool, vr_view_bob, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// The HUD's own panel while playing: its width in metres, how far away it
// hangs, and how many degrees below eye level its centre sits, tilted back to
// face the eyes. It hangs in one of two places: looking ahead, level with the
// eyes, so the pickup messages along its top are up and to the left of the
// view; glancing down at the status bar, lower, so the bar sits low in the
// view (with vr_hud_glance off, always the lower place).
CVAR(Float, vr_hud_width, 1.8f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hud_distance, 2.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hud_text_angle, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hud_bar_angle, 30.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// The status bar shows when the eyes glance down at it and is put away when
// they look back up, so it is out of the view while playing; pickup messages
// stay. Off: the status bar is always up.
CVAR(Bool, vr_hud_glance, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// How far down the eyes look before it shows, degrees below level; it goes
// again 8 degrees above that, so it does not flicker at the edge.
CVAR(Float, vr_hud_glance_angle, 28.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Eye height fine-tuning, metres, added on top of the recentered height:
// for a player whose view feels a little high or low (seated or standing,
// the game's eye height is where the head was at the last recenter).
CVAR(Float, vr_height_adjust, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Controller vibration: a kick in the weapon hand when it fires, a rumble in
// both when the player is hurt. On by default;
// vr_debug_haptics logs every pulse whether it is on or not.
CVAR(Bool, vr_haptics, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_haptics_strength, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_debug_haptics, false, 0)

// Settings VR mods read by name (tools\vrmod-usage.py): weapon recoil
// (Heretic Voidwalker pushes the player back when it is on), and which hand
// holds the weapon: 0 right-handed, 10 left-handed - the Leftorium. 10 is the
// value the benchmark swaps its hands at (measured: its Attack* then follow
// the left controller and Offhand* the right), so mods reading it agree.
CVAR(Bool, vr_recoil, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, vr_control_scheme, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// The Leftorium (left-handed play): pcvr-xr swaps the controllers under us,
// so everything that asks for the right hand - aim, trigger, buttons,
// haptics - gets the left controller. Only drawing needs to know: a model
// or sprite is mirrored when the hand holding it is PHYSICALLY the left.
static bool Leftorium() { return vr_control_scheme >= 10; }

// The sticks are not the weapon hand's: a left-handed player still walks
// with the left stick, as left-handed players asked. So the Leftorium's swap
// is undone for the sticks and their clicks, and "Swap sticks" is its own
// setting, with or without the Leftorium.
CVAR(Bool, vr_swap_sticks, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
static bool SticksSwapped() { return Leftorium() != bool(vr_swap_sticks); }

// The flat game's walking bob on the weapon. Off in a headset: the gun is in
// a tracked hand, which does its own moving. Some like it; VR Options.
CVAR(Bool, vr_weapon_bob, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// A 3D weapon model's size and place in the hand, on top of what its own
// MODELDEF says: a size factor, the model's origin as an offset in metres
// along the grip's own axes (right, up, back), and a tilt about the grip's
// right axis (degrees, barrel down is negative).
//
// The VR weapon mods were made for one convention - their MODELDEF offsets
// assume it - so the defaults are that convention, MEASURED, not read from
// anyone's code: tools\axis-model.py makes a pistol that is an axis marker,
// tools\model-ab.ps1 holds the same test hand still at seven poses in the
// benchmark and in DoomVR, and tools\axis-fit.py solves for these five
// numbers.
// Not saved: a saved value would outrank a better measured default later.
// The flat game's weapon models are built out in front of an origin at the
// eye; this puts that origin above and behind the hand, so the gun itself is
// in it.
CVAR(Bool, vr_phys_holsters, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_phys_holster_lift, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// m, seated reach: lifts both holsters, up to 0.2
CVAR(Bool, vr_weapon_wheel, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)		// hold the turning stick's click
CVAR(Float, vr_wheel_slowmo, 0.3f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// the game's speed while it is up (1: none)
CVAR(Int, vr_wheel_debug, 0, 0)	// desk: 1 holds the click, 2+ lights slice N-2
EXTERN_CVAR(Float, i_timescale)	// physical play: draw from the body
EXTERN_CVAR(Bool, vr_phys_use)	// physical play: press by hand, items to the mouth (p_user.cpp)
bool DoomVR_InventoryGame();	// p_user.cpp
bool DoomVR_PlayerAlive();	// p_user.cpp: the local player is alive and in a body
extern bool automapactive;
extern bool netgame;
CVAR(Float, vr_model_scale, 0.457f, 0)
CVAR(Float, vr_model_x, -0.006f, 0)
CVAR(Float, vr_model_y, 0.186f, 0)
CVAR(Float, vr_model_z, 0.228f, 0)
CVAR(Float, vr_model_pitch, -32.3f, 0)

// A test hook: the screen blend drawn over the 3D view, as in a headset, on
// the flat game too - so the path can be seen on a monitor.
CVAR(Bool, vr_debug_blend_in_scene, false, 0)

// The weapon's size in the hand: metres per sprite unit. A pistol sprite is
// about 57 units wide, so 0.003 holds it about 17 cm across.
CVAR(Float, vr_weapon_scale, 0.003f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Where the made-up right controller is, from the head, metres (right, up,
// back). Not archived.
CVAR(Float, vr_debug_hand_x, 0.15f, 0)
CVAR(Float, vr_debug_hand_y, -0.25f, 0)
CVAR(Float, vr_debug_hand_z, -0.35f, 0)

// Synthetic aim for the made-up right controller (vr_debug_input), degrees:
// yaw positive left, pitch positive UP. Not archived.
CVAR(Float, vr_debug_aim_yaw, 0.f, 0)
CVAR(Float, vr_debug_aim_pitch, 0.f, 0)

void DoomVR_UnsupportedBackend(const char* backend)
{
	static bool said = false;
	if (vr_mode == VR_HEADSET && !said)
	{
		said = true;
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: the headset is drawn with OpenGL only, and this is the %s renderer - "
		       "staying flat. Set vid_preferbackend 0 and restart.\n", backend);
	}
}

// The spectator view: the monitor shows a
// steady, level cut-out of the left eye that follows the head's turns but not
// its wobble, for streams and recordings. On by default; 0 (VR Options, or
// +vr_spectator 0 on a launcher) shows the plain eye. Menus keep the plain view.
CVAR(Bool, vr_spectator, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// The red dot crosshair: a red dot where
// the weapon's shot would land right now - on the wall, the floor or the
// monster it would hit - found along the very ray the shot takes (the
// weapon's own autoaim included), drawn in the world in each eye, so it is
// at the right depth and hidden by whatever is in front of it. The point is
// found once a frame (hw_entrypoint.cpp, DoomVR_CrosshairShot in p_user.cpp)
// and each eye draws it (HWDrawInfo::DrawEndScene2D). Hands and Weapons > Red
// dot crosshair. Written for Doom VR from that behaviour.
CVAR(Bool, vr_crosshair, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// How see-through the red dot is, 1 = solid (Hands and Weapons > Red dot opacity).
CUSTOM_CVAR(Float, vr_crosshair_alpha, 0.6f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.1f) self = 0.1f;
	else if (self > 1.f) self = 1.f;
}
// How big the red dot is, 1 = the default (Hands and Weapons > Red dot size).
CUSTOM_CVAR(Float, vr_crosshair_size, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.25f) self = 0.25f;
	else if (self > 3.f) self = 3.f;
}
static bool gDotValid = false;
static double gDot[3] = {};

void DoomVR_SetCrosshairPoint(bool valid, double x, double y, double z)
{
	gDotValid = valid;
	gDot[0] = x; gDot[1] = y; gDot[2] = z;
}

bool DoomVR_GetCrosshairPoint(double& x, double& y, double& z)
{
	if (!gDotValid) return false;
	x = gDot[0]; y = gDot[1]; z = gDot[2];
	return true;
}

// Test hook: where the dot is this frame, to compare with where a shot lands.
CCMD(vr_debug_crosshair)
{
	if (gDotValid) Printf("CROSSHAIR %.2f %.2f %.2f\n", gDot[0], gDot[1], gDot[2]);
	else Printf("CROSSHAIR none\n");
}

// The last runs' logs, kept: a session hops between games through Switch
// Game, and only the last two logs would survive.
// Every launcher renames the last run's log-doomvr.txt to .prev.txt before
// starting; here, at startup, that .prev (and its shutdown log) is copied
// into logs\ under the run's own date and time, and only the newest
// kDoomVRLogRuns are kept. Only files of these names in logs\ are removed.
// Flat and headset alike, whatever started the game.
static constexpr int kDoomVRLogRuns = 10;

void DoomVR_ArchiveLogs()
{
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path prev = "log-doomvr.prev.txt";
	if (!fs::exists(prev, ec)) return;
	struct stat st;
	if (stat("log-doomvr.prev.txt", &st) != 0) return;
	const time_t when = st.st_mtime;
	struct tm local;
#ifdef _WIN32
	localtime_s(&local, &when);
#else
	localtime_r(&when, &local);
#endif
	char stamp[32];
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
	fs::create_directories("logs", ec);
	const fs::path dest = fs::path("logs") / (std::string("log-doomvr-") + stamp + ".txt");
	if (fs::exists(dest, ec)) return;	// this run is already kept
	fs::copy_file(prev, dest, ec);
	if (ec) { Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: could not keep the last run's log in logs\\ (%s)\n", ec.message().c_str()); return; }
	if (fs::exists("log-doomvr-shutdown.prev.txt", ec))
		fs::copy_file("log-doomvr-shutdown.prev.txt", fs::path("logs") / (std::string("log-doomvr-shutdown-") + stamp + ".txt"), ec);

	std::vector<std::string> stamps;
	for (const auto& e : fs::directory_iterator("logs", ec))
	{
		const std::string n = e.path().filename().string();
		if (n.size() == 30 && n.rfind("log-doomvr-", 0) == 0 && n.compare(n.size() - 4, 4, ".txt") == 0 &&
		    n.compare(0, 20, "log-doomvr-shutdown-") != 0)
			stamps.push_back(n.substr(11, 15));
	}
	std::sort(stamps.begin(), stamps.end());
	for (size_t i = 0; i + kDoomVRLogRuns < stamps.size(); ++i)
	{
		fs::remove(fs::path("logs") / ("log-doomvr-" + stamps[i] + ".txt"), ec);
		fs::remove(fs::path("logs") / ("log-doomvr-shutdown-" + stamps[i] + ".txt"), ec);
	}
}

#ifdef DOOMVR_PCVRXR

#include <chrono>
#include "pcvrxr/pcvrxr.h"
#include "pcvrxr/spectator.h"
#include "cmdlib.h"
#include "hw_clock.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef __ANDROID__
// The Khronos loader needs the app's JavaVM and activity; SDL has both.
#include <SDL2/SDL.h>
#include <jni.h>
// The Game Tome's signal file, memory-mapped.
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#endif

/*
	The Game Tome: Game Or Die's book of the game's manual, an OpenXR API layer of its
	own beside the game (gametome\GameTomeLayer.dll + .json beside the exe;
	the book's pages in gametome\tome). The game only asks for it: the layer
	makes a 540-byte block named "Local\GameOrDie.GameTome.<process id>"; we
	write the tome's folder and the hand that turns pages, then open = 1; while
	it says showing, the menu screen is put away (the book is over it) and the
	layer has the controllers. Its layout, as the layer publishes it: magic
	"GTB1", size, ready, open, showing, main hand, floating, the folder.
*/
namespace
{
	// The layer's longs are Windows longs (32 bits); on a 64-bit Linux long is 64.
#ifdef _WIN32
	typedef long TomeLong;
#else
	typedef int32_t TomeLong;
#endif
	struct TomeSignal
	{
		unsigned magic, size;
		volatile TomeLong ready, open, showing;
		TomeLong mainHand, floating;
		char tomeDir[512];
	};
	static_assert(sizeof(TomeSignal) == 540, "the Game Tome block is 540 bytes");
	TomeSignal* gTome = nullptr;

	FString TomeHome() { return progdir + "gametome"; }

	// The book for the game being played: gametome\books.txt lists each
	// game (as Switch Game names it) and the folder of its book, one
	// "<game><tab><folder>" a line; a game with no line, or whose folder has
	// no tome.txt, gets the book with our pages alone (gametome\tome).
	FString BookFor(const char* game)
	{
		const FString home = TomeHome();
		FString dir = home + "/tome";
		if (!game || !*game) return dir;
		FILE* f = fopen((home + "/books.txt").GetChars(), "rb");
		if (!f) return dir;
		char line[512];
		while (fgets(line, sizeof(line), f))
		{
			char* tab = strchr(line, '\t');
			if (!tab || line[0] == '#') continue;
			*tab = 0;
			FString folder = tab + 1;
			folder.StripRight();
			if (stricmp(line, game) == 0 && FileExists((home + "/" + folder + "/tome.txt").GetChars()))
			{
				dir = home + "/" + folder;
				break;
			}
		}
		fclose(f);
		return dir;
	}

	TomeSignal* Tome()
	{
#if defined(__ANDROID__)
		// A standalone headset: the layer (in this app's APK, loaded by the
		// OpenXR loader, never by the game) keeps the same 540-byte block as
		// a file, $GAMETOME_HOME/signal, memory-mapped by both. It appears
		// when the OpenXR instance is made; until then there is no book.
		if (gTome) return gTome;
		const char* home = getenv("GAMETOME_HOME");
		if (!home || !*home) return nullptr;
		const int fd = open((std::string(home) + "/signal").c_str(), O_RDWR);
		if (fd < 0) return nullptr;
		void* v = mmap(nullptr, sizeof(TomeSignal), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		close(fd);
		if (v == MAP_FAILED) return nullptr;
		auto* s = static_cast<TomeSignal*>(v);
		if (s->magic != 0x31425447u || s->size != sizeof(TomeSignal)) { munmap(v, sizeof(TomeSignal)); return nullptr; }
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: Game Tome - the layer's signal is up (%s/signal)\n", home);
		return gTome = s;
#elif !defined(_WIN32)
		// No layer on this platform yet: no book, and the game says so when asked.
		return nullptr;
#else
		if (gTome) return gTome;
		char name[64];
		snprintf(name, sizeof(name), "Local\\GameOrDie.GameTome.%lu", (unsigned long)GetCurrentProcessId());
		HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
		if (!h) return nullptr;
		void* v = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TomeSignal));
		CloseHandle(h);
		auto* s = static_cast<TomeSignal*>(v);
		if (s && (s->magic != 0x31425447u || s->size != sizeof(TomeSignal))) { UnmapViewOfFile(v); s = nullptr; }
		return gTome = s;
#endif
	}
}

EXTERN_CVAR(String, vr_current_game)
EXTERN_CVAR(String, vr_book_game)	// a My WADs file: its game, for the book (doomvr_switchgame.cpp)
extern int paused;

namespace
{
	/*
		The open-book gesture (both palms up together for a second; the layer
		watches for it, in every build) opens
		the book the block names, so it always names this game's book and the
		hand that turns pages: from start-up, again when the game or the hand
		changes, never while the book shows. Opened in play, the game pauses
		under it and goes on when it closes; from the pause menu it already is.
	*/
	FString gToldGame;
	int gToldHand = -1;
	bool gTomePaused = false;
	bool gTomeWasShowing = false;

#ifdef _WIN32
	BOOL CALLBACK FindOurWindow(HWND w, LPARAM out)
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(w, &pid);
		if (pid != GetCurrentProcessId() || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
		*reinterpret_cast<HWND*>(out) = w;
		return FALSE;
	}

	/*
		The book takes the PC keyboard while it shows (the layer's keyboard
		hook), but Windows gives a program no keys typed into a window that runs
		as administrator (the hook was in, and not one key reached it). So when the book
		opens with another program's window in front, the game's own window is
		brought to the front, and the log says what was there.
	*/
	void TomeKeyboardToUs()
	{
		HWND fg = GetForegroundWindow();
		DWORD pid = 0;
		if (fg) GetWindowThreadProcessId(fg, &pid);
		if (pid == GetCurrentProcessId())
		{
			Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: Game Tome open - the game's window is in front: the keyboard types in the book\n");
			return;
		}
		char title[96] = "", exe[MAX_PATH] = "";
		const char* admin = "not looked at";
		if (fg) GetWindowTextA(fg, title, sizeof(title));
		if (HANDLE p = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr)
		{
			DWORD len = MAX_PATH;
			if (!QueryFullProcessImageNameA(p, 0, exe, &len)) exe[0] = 0;
			HANDLE token = nullptr;
			if (OpenProcessToken(p, TOKEN_QUERY, &token))
			{
				TOKEN_ELEVATION e = {};
				DWORD got = 0;
				admin = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &got) ? (e.TokenIsElevated ? "as administrator" : "as a normal program") : "not looked at";
				CloseHandle(token);
			}
			else admin = "as administrator (its token is closed to us)";
			CloseHandle(p);
		}
		const char* base = strrchr(exe, '\\');
		HWND ours = nullptr;
		EnumWindows(FindOurWindow, reinterpret_cast<LPARAM>(&ours));
		const bool moved = ours && SetForegroundWindow(ours) && GetForegroundWindow() == ours;
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: Game Tome open - in front was %s \"%s\" (runs %s); the game's window brought to the front: %s\n",
		       exe[0] ? (base ? base + 1 : exe) : "(no window)", title, admin, moved ? "yes" : "NO - Windows refused");
	}
#else
	void TomeKeyboardToUs() {}
#endif

	/*
		Typing in the book from the game's own keyboard input. The layer types
		in the book through a keyboard hook of its own; on some PCs that
		hook gets no key at all while the game read every one. So
		while the book shows, the keys it takes - a printable character,
		Backspace, Enter, Esc - are handed to it here as well, through the
		layer's key block ("Local\GameOrDie.GameTome.Keys.<process id>": magic
		"GTK1", size, head the game writes, tail the layer reads, 64 keys), and
		kept from the game. A key the hook took never reaches the game, so none
		goes over twice.
	*/
	struct TomeKeys
	{
		unsigned magic, size;
		volatile TomeLong head, tail;
		unsigned key[64];
	};
	static_assert(sizeof(TomeKeys) == 272, "the Game Tome's key block is 272 bytes");
	TomeKeys* gTomeKeys = nullptr;
	bool gTomeAte[256];
	bool gTomeKeysSaid = false;

	TomeKeys* TomeKeyBlock()
	{
#ifndef _WIN32
		return nullptr;
#else
		if (gTomeKeys) return gTomeKeys;
		char name[64];
		snprintf(name, sizeof(name), "Local\\GameOrDie.GameTome.Keys.%lu", (unsigned long)GetCurrentProcessId());
		HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
		if (!h) return nullptr;
		void* v = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TomeKeys));
		CloseHandle(h);
		auto* k = static_cast<TomeKeys*>(v);
		if (k && (k->magic != 0x314B5447u || k->size != sizeof(TomeKeys))) { UnmapViewOfFile(v); k = nullptr; }
		return gTomeKeys = k;
#endif
	}

	bool TomeShowing()
	{
		TomeSignal* s = Tome();
		return s && s->showing;
	}

	// What a key types in the book: Backspace 8, Enter 13, Esc 27, or a
	// printable character; 0 for a key the game keeps (Ctrl, Alt and Windows
	// combinations, F keys, arrows).
	unsigned TomeChar(unsigned vk, unsigned scan)
	{
#ifndef _WIN32
		(void)vk; (void)scan;
		return 0;
#else
		if (vk == VK_BACK) return 8;
		if (vk == VK_RETURN) return 13;
		if (vk == VK_ESCAPE) return 27;
		if ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU) | GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) return 0;
		BYTE state[256] = {};
		if (GetAsyncKeyState(VK_SHIFT) & 0x8000) state[VK_SHIFT] = 0x80;
		if (GetKeyState(VK_CAPITAL) & 1) state[VK_CAPITAL] = 1;
		WCHAR buf[4];
		const int n = ToUnicodeEx(vk, scan, state, buf, 4, 4 /* the keyboard's state left as it was */, GetKeyboardLayout(0));
		return n == 1 && buf[0] >= 32 ? unsigned(buf[0]) : 0;
#endif
	}

	void TomeKeep(TomeSignal* s)
	{
		if (!s) return;
		if (s->showing && !gTomeWasShowing) TomeKeyboardToUs();
		gTomeWasShowing = s->showing != 0;
		const int hand = Leftorium() ? 0 : 1;
		if (!s->showing && (hand != gToldHand || gToldGame.Compare(*vr_current_game) != 0))
		{
			const FString dir = BookFor(strlen(*vr_book_game) ? *vr_book_game : *vr_current_game);
			snprintf(s->tomeDir, sizeof(s->tomeDir), "%s", dir.GetChars());
			s->mainHand = hand;
			s->floating = 0;
			gToldGame = *vr_current_game;
			gToldHand = hand;
			Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: Game Tome - the book for \"%s\" (%s)\n", *vr_current_game, dir.GetChars());
		}
		if (s->showing && !gTomePaused && !paused && gamestate == GS_LEVEL && menuactive == MENU_Off)
		{
			AddCommandString("pause");
			gTomePaused = true;
			Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: Game Tome open in play - paused\n");
		}
		else if (!s->showing && gTomePaused)
		{
			if (paused && menuactive == MENU_Off) AddCommandString("pause");
			gTomePaused = false;
			Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: Game Tome closed - play goes on\n");
		}
	}
}

bool DoomVR_TomeKey(unsigned vk, unsigned scan, bool down)
{
	vk &= 255;
	if (!down)
	{
		// A key whose press the book took: its release too.
		if (!gTomeAte[vk]) return false;
		gTomeAte[vk] = false;
		return true;
	}
	if (!TomeShowing()) return false;
	const unsigned ch = TomeChar(vk, scan);
	TomeKeys* k = ch ? TomeKeyBlock() : nullptr;
	if (!k) return false;
	k->key[(unsigned long)k->head % 64] = ch;
#ifdef _WIN32
	InterlockedIncrement(&k->head);	// after the key: the layer reads up to head
#else
	__sync_fetch_and_add(&k->head, 1);
#endif
	gTomeAte[vk] = true;
	if (!gTomeKeysSaid)
	{
		gTomeKeysSaid = true;
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: Game Tome - the keyboard reached the game; its keys go to the book\n");
	}
	return true;
}

bool DoomVR_TomeOwnsKey(unsigned message, unsigned long long wparam)
{
#ifndef _WIN32
	(void)message; (void)wparam;
	return false;
#else
	switch (message)
	{
	case WM_CHAR:
		return TomeShowing() && wparam >= 8;
	case WM_KEYDOWN:
	case WM_KEYUP:
		return TomeShowing() && TomeChar(unsigned(wparam) & 255, 0) != 0;
	default:
		return false;
	}
#endif
}

bool DoomVR_InventoryGame();	// p_user.cpp: Heretic + Hexen's layout, or Doom's

namespace
{
	int gFrames = 0;         // frames submitted
	int gReportFrames = 0;   // gFrames at the last vr_report
	double gReportMs = 0;    // and when
	// Where the time went since the last report, in the layer's calls. The
	// rest of each frame is the engine's.
	double gWaitMs = 0, gEyeMs = 0, gSubmitMs = 0;
#ifdef __ANDROID__
	// The performance line's split of the engine's time (DoomVR_PerfAdd).
	constexpr int kPerfSlots = 20;	// 12-19: the end-of-scene pass, split (hw_drawinfo.cpp)
	double gPerf[kPerfSlots] = {};
	long long gPerfLines = 0, gPerfFlats = 0, gPerfSprites = 0;
#endif

	bool Tracing()
	{
		return vr_debug_trace > 0 && (gFrames < vr_debug_trace || gFrames % 90 == 0);
	}

	double NowMs()
	{
		using namespace std::chrono;
		return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
	}
	pcvrxr::Session* gSession = nullptr;
	bool gTried = false;     // one bring-up attempt per process
	bool gPanel = false;     // the panel's swapchain exists
	bool gUp = false;        // the session is up
	bool gFrameBegun = false;
	bool gRender = false;    // the runtime wants this frame drawn

	void LayerLog(const char* line)
	{
		// The layer's lines carry no newline; UZDoom's console wants one.
		const size_t n = strlen(line);
		if (n > 0 && line[n - 1] == '\n') Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "%s", line);
		else Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "%s\n", line);
	}

	void BringUp()
	{
		gTried = true;
		gSession = new pcvrxr::Session();
		gSession->SetLog(LayerLog);

		pcvrxr::Config cfg;
		cfg.appName = "Doom VR";	// what SteamVR and Virtual Desktop list
		// Eye level (OpenXR LOCAL), the posture standard: the player's eyes
		// are at the game's eye height however tall they are and whether
		// they sit or stand, and a streaming runtime's guessed floor never
		// comes into it. VirtualDesktopXR grants a "standing" space with a
		// play area of 0 x 0 m, whose floor is exactly that guess.
		cfg.space = pcvrxr::Space::Seated;
#ifdef __ANDROID__
		// A standalone headset: the loader needs the app's JavaVM and
		// activity to find the runtime. SDL has both.
		{
			JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
			JavaVM* vm = nullptr;
			if (env) env->GetJavaVM(&vm);
			cfg.androidVM = vm;
			cfg.androidActivity = SDL_AndroidGetActivity();
			if (const char* dev = getenv("DOOMVR_DEVICE")) Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: standalone on %s\n", dev);
		}
		// The compositor's quality supersampling and sharpening on the world:
		// a Quest's recommended eye buffer is below its panel, and this is how
		// its own apps stay crisp without paying for a bigger one.
		cfg.compositorQuality = true;
#endif
		// The Game Tome's layer, when it is installed beside the game.
#ifdef __ANDROID__
		// A standalone headset: the layer is in this APK (assets/openxr/1/api_layers/
		// explicit.d names it; the loader finds it there), asked for by name.
		cfg.apiLayerName = "XR_APILAYER_GAMEORDIE_game_tome";
#else
		if (FileExists((TomeHome() + "/GameTomeLayer.json").GetChars()))
		{
			cfg.apiLayerDir = TomeHome().GetChars();
			cfg.apiLayerName = "XR_APILAYER_GAMEORDIE_game_tome";
		}
#endif

		// A failed bring-up keeps no session object: there is nothing to shut
		// down later, and nothing that could claim to have been.
		if (!gSession->Connect(cfg))
		{
			Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: no headset (%s) - staying flat.\n", gSession->FailureReason());
			delete gSession;
			gSession = nullptr;
			return;
		}
		if (!gSession->BringUp(cfg))
		{
			Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: the headset session did not start (%s) - staying flat.\n",
			       gSession->FailureReason());
			gSession->Shutdown();
			delete gSession;
			gSession = nullptr;
			return;
		}

		int w = 0, h = 0;
		gSession->EyeBufferSize(w, h);
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: headset up on %s, %dx%d per eye.\n", gSession->RuntimeName(), w, h);
		gUp = true;
#ifdef __ANDROID__
		// A standalone headset starts every app at its lowest rate (a Quest 3:
		// 72 Hz) until the app asks; the PC over Link ran at the player's Link
		// setting, and 72 read as "a bit less smooth". vr_refresh_hz 0 leaves
		// the runtime's choice. The performance line in the log says what was
		// actually held.
		if (vr_refresh_hz > 0) gSession->SetRefreshRate((float)vr_refresh_hz);
#endif

		// The 2D's panel. 1920x1080 holds any canvas sharply enough at 62
		// degrees; the canvas is drawn across all of it and the quad takes the
		// canvas's shape each frame, so a window of another shape is not shown
		// stretched.
		pcvrxr::PanelConfig pc;
		pc.width = 1920;
		pc.height = 1080;
		pc.widthMetres = vr_panel_width;
		gPanel = gSession->CreatePanel(pc);
		if (!gPanel) Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: no panel (%s) - menus and HUD stay in the eyes.\n", gSession->FailureReason());
	}

	/*
		A unit quaternion to yaw, pitch and roll in the order UZDoom's view
		matrix applies them: HWDrawInfo::SetViewMatrix rotates roll about Z,
		then pitch about X, then yaw about Y. The matching decomposition of
		R = Ry(yaw) Rx(pitch) Rz(roll), in OpenXR's axes (+X right, +Y up,
		-Z forward), is

			pitch = asin(-R[1][2])
			yaw   = atan2( R[0][2], R[2][2])
			roll  = atan2( R[1][0], R[1][1])

		with R written row-major from the quaternion below. A positive yaw
		turns the head to the LEFT, a positive pitch lifts it UP, a positive
		roll tilts it so the right ear goes UP.
	*/
	void ToYawPitchRoll(const pcvrxr::Quat& q, double& yaw, double& pitch, double& roll)
	{
		const double x = q.x, y = q.y, z = q.z, w = q.w;
		const double r02 = 2.0 * (x * z + w * y);
		const double r12 = 2.0 * (y * z - w * x);
		const double r22 = 1.0 - 2.0 * (x * x + y * y);
		const double r10 = 2.0 * (x * y + w * z);
		const double r11 = 1.0 - 2.0 * (x * x + z * z);

		const double s = r12 < -1.0 ? 1.0 : (r12 > 1.0 ? -1.0 : -r12);
		pitch = asin(s);
		yaw = atan2(r02, r22);
		roll = atan2(r10, r11);
	}

	constexpr double kToDeg = 180.0 / 3.14159265358979323846;
	constexpr double kToRad = 3.14159265358979323846 / 180.0;

	// ---- the controllers, as a gamepad ---------------------------------

	// The recenter point: where the head was, and which way it faced, when
	// the view was last recentered - automatically on the first tracked
	// frame, then by vr_recenter. Everything the player sees is relative to
	// it, so the height of the real head does not matter: the game's own
	// view height is wherever the head was at that moment.
	bool gCentered = false;
	bool gRecenterAsked = false;
	const char* gRecenterWhy = "";   // for the log line
	unsigned gSpaceGeneration = 0;   // the layer's, when the reference was taken
	double gOriginX = 0, gOriginY = 0, gOriginZ = 0, gOriginYaw = 0; // metres, radians

	// A room position (metres, OpenXR axes, with the synthetic offset) relative
	// to the recenter point, turned so that -Z is the recentered forward.
	void FromOrigin(const pcvrxr::Vec3& p, double& right, double& up, double& forward)
	{
		const double x = p.x + vr_debug_head_x - gOriginX;
		const double y = p.y + vr_debug_head_y - gOriginY;
		const double z = p.z + vr_debug_head_z - gOriginZ;
		// Undo the recenter yaw: rotate by -gOriginYaw about +Y.
		const double c = cos(gOriginYaw), sn = sin(gOriginYaw);
		const double xr = x * c - z * sn;
		const double zr = x * sn + z * c;
		right = xr;
		up = y;
		forward = -zr;
	}

	// The yaw walking follows, relative to the recenter point, degrees,
	// positive left: the head's, or the left controller's (vr_move_follows).
	double gMoveYaw = 0;

	// The head's yaw, and the right controller's aim, relative to the
	// recenter direction (so, to the body), in UZDoom's senses: degrees, yaw
	// positive left, pitch positive down. gAimValid is false when the right
	// controller is not there.
	double gLookYaw = 0, gAimYaw = 0, gAimPitch = 0;
	bool gAimValid = false;

	// The panel's placement: whether it is showing a screen (menu, console,
	// a non-level screen) or the HUD, and the head yaw it was placed at.
	enum { PANEL_NONE, PANEL_SCREEN, PANEL_HUD };
	int gPanelMode = PANEL_NONE;
	double gPanelYaw = 0; // radians, room

	// The eye being drawn, and the right hand's grip and aim for the weapon.
	int gDrawingEye = 0;
	pcvrxr::Pose gGrip, gAimPose;
	bool gGripValid = false;
	// The left controller's aim, and which hand the model being drawn is in
	// (0 right, 1 left - the off-hand weapon).
	pcvrxr::Pose gLeftAimPose, gLeftGrip;
	// The fit of the weapon model being drawn (DoomVR_SetDrawingTune).
	DoomVR_WeaponTune gDrawingTune;
	bool gLeftValid = false;
	int gDrawingHand = 0;

	// The left stick is held in: run at full speed.
	bool gRunHeld = false;

	// ---- 4x4 matrices, column-major, as GL and VSMatrix have them ----

	void MatMul(const float a[16], const float b[16], float out[16])
	{
		float r[16];
		for (int c = 0; c < 4; ++c)
			for (int rr = 0; rr < 4; ++rr)
				r[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0] + a[1 * 4 + rr] * b[c * 4 + 1]
				              + a[2 * 4 + rr] * b[c * 4 + 2] + a[3 * 4 + rr] * b[c * 4 + 3];
		memcpy(out, r, sizeof(r));
	}

	// A pose as the matrix that carries its local space into the space it is
	// expressed in (the inverse of pcvrxr::ViewMatrix).
	void PoseMatrix(const pcvrxr::Pose& p, float out[16])
	{
		const float x = p.orientation.x, y = p.orientation.y, z = p.orientation.z, w = p.orientation.w;
		out[0] = 1 - 2 * (y * y + z * z); out[4] = 2 * (x * y - w * z);     out[8] = 2 * (x * z + w * y);      out[12] = p.position.x;
		out[1] = 2 * (x * y + w * z);     out[5] = 1 - 2 * (x * x + z * z); out[9] = 2 * (y * z - w * x);      out[13] = p.position.y;
		out[2] = 2 * (x * z - w * y);     out[6] = 2 * (y * z + w * x);     out[10] = 1 - 2 * (x * x + y * y); out[14] = p.position.z;
		out[3] = 0;                       out[7] = 0;                       out[11] = 0;                       out[15] = 1;
	}

	// The right stick's turning, read every frame: its deflection for smooth
	// turning, and snap steps waiting to be turned, in degrees (+ left).
	float gTurnStick = 0;
	double gSnapPending = 0;
	bool gSnapArmed = true;
	double gLastTurnMs = 0;

	// A vector turned by a unit quaternion.
	pcvrxr::Vec3 Rotate(const pcvrxr::Quat& q, double x, double y, double z)
	{
		// v + 2w (q x v) + 2 q x (q x v)
		const double tx = 2 * (q.y * z - q.z * y), ty = 2 * (q.z * x - q.x * z), tz = 2 * (q.x * y - q.y * x);
		pcvrxr::Vec3 r;
		r.x = float(x + q.w * tx + (q.y * tz - q.z * ty));
		r.y = float(y + q.w * ty + (q.z * tx - q.x * tz));
		r.z = float(z + q.w * tz + (q.x * ty - q.y * tx));
		return r;
	}

	// a then b: the rotation b followed by a (Hamilton product a * b).
	pcvrxr::Quat QuatMul(const pcvrxr::Quat& a, const pcvrxr::Quat& b)
	{
		pcvrxr::Quat r;
		r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
		r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
		r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
		r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
		return r;
	}

	/*
		A hand, with its aim where Quest Touch puts it. Every fit in the game -
		the guns in the hand, the hands on the guns, the muzzles, the red dot -
		was measured on Touch, where the grip sits in the aim's frame pitched up
		60 degrees, 3 cm down and 9.5 cm back (VDXR and SteamVR alike). An Index
		reports 55.3 degrees with 5 degrees of yaw and roll and the grip 2.5 cm
		lower (a player's log), so the gun sat off in the hand and
		the shots left it at an angle. On the controllers we have not measured
		this way, the aim is laid along their grip as Touch would report it:
		the hand, where the player feels the controller, stays the runtime's.
		Touch, the Frame and anything unrecognised keep their own aim.
	*/
	bool AimFromGrip(const char* profile)
	{
		if (!vr_aim_from_grip || profile == nullptr) return false;
		return strcmp(profile, "/interaction_profiles/valve/index_controller") == 0
		    || strcmp(profile, "/interaction_profiles/htc/vive_controller") == 0
		    || strcmp(profile, "/interaction_profiles/microsoft/motion_controller") == 0;
	}

	pcvrxr::HandState Hand(pcvrxr::Hand which)
	{
		pcvrxr::HandState h = gSession->GetHand(which);
		if (h.active && AimFromGrip(gSession->InteractionProfile(which)))
		{
			// Touch: grip = aim x Rx(+60), at (0, -0.03, 0.095) in the aim's frame.
			const double half = 60.0 * 0.5 * 3.14159265358979323846 / 180.0;
			const pcvrxr::Quat undo{ float(-sin(half)), 0, 0, float(cos(half)) };
			h.aim.orientation = QuatMul(h.grip.orientation, undo);
			const pcvrxr::Vec3 t = Rotate(h.aim.orientation, 0, -0.03, 0.095);
			h.aim.position = { h.grip.position.x - t.x, h.grip.position.y - t.y, h.grip.position.z - t.z };
		}
		return h;
	}

	// Heretic and Hexen's inventory lives on a pad's D-pad, which Touch does
	// not have. A flick of the right stick up or down sends it instead: up is
	// the pad's D-pad down (invuse by default - use the selected item), down
	// is D-pad right (invnext - select the next one). One key press per flick.
	EKeyCodes gFlickKey = EKeyCodes(0);

	// The weapon tuner's clock and click edges (ReadControllers).
	double gTuneLastMs = 0;
	bool gTuneRClick = false, gTuneLClick = false;

	// Each digital control and its key. VR controllers are their own device
	// (KEY_VR_*, named VR_A, VR_RTrigger...), with their own bindings - the
	// defaults (DoomVR_VRDefaultBinds) are what a pad's are: A use, B crouch,
	// X reload, Y jump, grips switch weapon, the menu button opens the menu.
	struct PadButton { pcvrxr::Hand hand; pcvrxr::Button pcvrxr::HandState::* button; EKeyCodes key; };
	const PadButton kButtons[] = {
		{ pcvrxr::Hand::Right, &pcvrxr::HandState::primary,         KEY_VR_A },
		{ pcvrxr::Hand::Right, &pcvrxr::HandState::secondary,       KEY_VR_B },
		{ pcvrxr::Hand::Left,  &pcvrxr::HandState::primary,         KEY_VR_X },
		{ pcvrxr::Hand::Left,  &pcvrxr::HandState::secondary,       KEY_VR_Y },
		{ pcvrxr::Hand::Left,  &pcvrxr::HandState::menu,            KEY_VR_MENU },
		{ pcvrxr::Hand::Left,  &pcvrxr::HandState::squeezeClick,    KEY_VR_LGRIP },
		{ pcvrxr::Hand::Right, &pcvrxr::HandState::squeezeClick,    KEY_VR_RGRIP },
		{ pcvrxr::Hand::Left,  &pcvrxr::HandState::thumbstickClick, KEY_VR_LSTICK },
		{ pcvrxr::Hand::Right, &pcvrxr::HandState::thumbstickClick, KEY_VR_RSTICK },
	};
	constexpr int kNumButtons = int(sizeof(kButtons) / sizeof(kButtons[0]));

	// The Steam Frame, laid out as the gamepad it is (pcvr-xr's padLayout, the
	// Frame's own controller profile): its buttons send their own keys
	// (VRF_*), so its bindings and a Touch controller's never touch. The
	// defaults follow the official Steam releases, Doom + Doom II and Heretic
	// + Hexen (FrameDefaultBinds). Its grips and stick clicks send VRF keys
	// too; recentering, the menu pointer, photo mode and the tuner still read
	// the physical controls, whichever layout.
	bool gPadLayout = false;
	EKeyCodes EffectiveKey(int i)
	{
		if (!gPadLayout) return kButtons[i].key;
		switch (kButtons[i].key)
		{
		case KEY_VR_LGRIP: return KEY_VRF_LGRIP;
		case KEY_VR_RGRIP: return KEY_VRF_RGRIP;
		case KEY_VR_LSTICK: return KEY_VRF_LSTICK;
		case KEY_VR_RSTICK: return KEY_VRF_RSTICK;
		default: return EKeyCodes(0);	// A B X Y and the menu: the Frame's own below
		}
	}
	// hand: -1 either (a button only one hand has, wherever the Leftorium
	// puts it), 0 left, 1 right.
	struct FrameButton { pcvrxr::Button pcvrxr::HandState::* button; int hand; EKeyCodes key; };
	const FrameButton kFrame[] = {
		{ &pcvrxr::HandState::padA,      -1, KEY_VRF_A },
		{ &pcvrxr::HandState::padB,      -1, KEY_VRF_B },
		{ &pcvrxr::HandState::padX,      -1, KEY_VRF_X },
		{ &pcvrxr::HandState::padY,      -1, KEY_VRF_Y },
		{ &pcvrxr::HandState::shoulder,   0, KEY_VRF_LB },
		{ &pcvrxr::HandState::shoulder,   1, KEY_VRF_RB },
		{ &pcvrxr::HandState::dpadUp,    -1, KEY_VRF_DPADUP },
		{ &pcvrxr::HandState::dpadDown,  -1, KEY_VRF_DPADDOWN },
		{ &pcvrxr::HandState::dpadLeft,  -1, KEY_VRF_DPADLEFT },
		{ &pcvrxr::HandState::dpadRight, -1, KEY_VRF_DPADRIGHT },
		{ &pcvrxr::HandState::view,      -1, KEY_VRF_VIEW },
		{ &pcvrxr::HandState::padMenu,   -1, KEY_VRF_MENU },
	};
	constexpr int kNumFrame = int(sizeof(kFrame) / sizeof(kFrame[0]));
	bool gFrameDown[kNumFrame] = {};
	bool FrameDown(int i, const pcvrxr::HandState& l, const pcvrxr::HandState& r)
	{
		const auto b = kFrame[i].button;
		if (kFrame[i].hand == 0) return l.active && (l.*b).down;
		if (kFrame[i].hand == 1) return r.active && (r.*b).down;
		return (l.active && (l.*b).down) || (r.active && (r.*b).down);
	}
	// Run, as a command any button can be bound to (+vr_run): the Frame's
	// layouts put it on a button; on Touch it stays the left stick held in.
	bool gRunCmd = false;

	// One entry's state. The menu button is taken from either hand: Touch has
	// it on the left controller only, which the Leftorium hands to the port as
	// its right.
	bool ButtonDown(int i, const pcvrxr::HandState& l, const pcvrxr::HandState& r)
	{
		if (kButtons[i].button == &pcvrxr::HandState::menu)
			return (l.active && l.menu.down) || (r.active && r.menu.down);
		const pcvrxr::HandState& h = kButtons[i].hand == pcvrxr::Hand::Left ? l : r;
		return h.active && (h.*kButtons[i].button).down;
	}
	bool gButtonDown[kNumButtons] = {};
	// A held button never acts in the next screen:
	// held at one of those moments, it counts only once it has been seen up.
	bool gMustRelease[kNumButtons] = {};
	bool gFrameMustRelease[kNumFrame] = {};
	bool gFreshMenu = false, gFreshL = false, gFreshR = false;
	int gFreshState = -1;
	const void* gFreshWeapon = nullptr;

	// Photo mode, and a screenshot waiting for the next present. While the
	// menu is hidden the right stick turns the view (gPhotoYaw, degrees,
	// dropped when the menu comes back); any other control brings the menu
	// back, and that press is swallowed until everything is let go, so it
	// cannot pick a row.
	bool gPhotoMode = false;
	bool gPhotoClickDown = false;
	double gPhotoYaw = 0;
	double gPhotoLastMs = 0;
	bool gPhotoSnapArmed = true;
	bool gSwallowUntilReleased = false;
	// Both thumbsticks clicked together: recenter (latched until both are let go).
	// Both grips held: recenter (see ReadControllers). Frames each grip has
	// been down, a tap owed to a grip let go inside the wait, and the chord.
	// In milliseconds, not frames: 8 frames was 0.11 s at 72 Hz but only
	// 0.067 s at 120 Hz, short enough to split two grips squeezed together.
	constexpr double kGripChordWaitMs = 120.;
	constexpr double kGripTapOnlyMs = 250.;	// the other hand's tap, with a two-handed gun up
	int gGripHeld[2] = {}, gGripTap[2] = {};
	double gGripSince[2] = {};
	bool gGripChord = false;
	bool gScreenshotRequested = false;

	// Analog: left stick X/Y, right stick X/Y, left and right trigger, after
	// UZDoom's own dead zone and response curve. A stick's direction buttons
	// (lstickup and so on, which the menus use) live in the X entry's bits.
	enum { LX, LY, RX, RY, LT, RT, NUM_PAD_AXES };
	float gAxis[NUM_PAD_AXES] = {};
	uint8_t gAxisButtons[NUM_PAD_AXES] = {};
	const EAxisCodes kAxisCodes[NUM_PAD_AXES][2] = {
		{ AXIS_CODE_PAD_LTHUMB_RIGHT, AXIS_CODE_PAD_LTHUMB_LEFT },
		{ AXIS_CODE_PAD_LTHUMB_DOWN,  AXIS_CODE_PAD_LTHUMB_UP },
		{ AXIS_CODE_PAD_RTHUMB_RIGHT, AXIS_CODE_PAD_RTHUMB_LEFT },
		{ AXIS_CODE_PAD_RTHUMB_DOWN,  AXIS_CODE_PAD_RTHUMB_UP },
		{ AXIS_CODE_PAD_LTRIGGER,     AXIS_CODE_NULL },
		{ AXIS_CODE_PAD_RTRIGGER,     AXIS_CODE_NULL },
	};

	void Stick(float x, float y, int ax, int ay, EKeyCodes base)
	{
		double vx = x, vy = y;
		uint8_t bits = 0;
		Joy_ManageThumbstick(&vx, &vy, JOYDEADZONE_DEFAULT, JOYDEADZONE_DEFAULT,
		                     JOYTHRESH_STICK_X, JOYTHRESH_STICK_Y,
		                     JOYCURVE[JOYCURVE_DEFAULT], JOYCURVE[JOYCURVE_DEFAULT], &bits);
		gAxis[ax] = float(vx);
		gAxis[ay] = float(vy);
		Joy_GenerateButtonEvents(gAxisButtons[ax], bits, 4, base);
		gAxisButtons[ax] = bits;
	}

	void Trigger(float v, int a, EKeyCodes key)
	{
		uint8_t bits = 0;
		gAxis[a] = float(Joy_ManageSingleAxis(v, JOYDEADZONE_DEFAULT, JOYTHRESH_TRIGGER,
		                                      JOYCURVE[JOYCURVE_DEFAULT], &bits));
		Joy_GenerateButtonEvents(gAxisButtons[a], bits, 1, key);
		gAxisButtons[a] = bits;
	}

	// Everything to rest, with a key-up for whatever was held: when the
	// headset goes away mid-press, or a controller sleeps.
	void ReleasePad()
	{
		for (int i = 0; i < kNumButtons; ++i)
		{
			if (gButtonDown[i] && EffectiveKey(i) != 0) Joy_GenerateButtonEvent(false, EffectiveKey(i));
			gButtonDown[i] = false;
		}
		for (int i = 0; i < kNumFrame; ++i)
		{
			if (gFrameDown[i]) Joy_GenerateButtonEvent(false, kFrame[i].key);
			gFrameDown[i] = false;
		}
		Stick(0, 0, LX, LY, KEY_VR_LSTICK_RIGHT);
		Stick(0, 0, RX, RY, KEY_PAD_RTHUMB_RIGHT);
		if (gFlickKey != 0) Joy_GenerateButtonEvent(false, gFlickKey);
		gFlickKey = EKeyCodes(0);
		Trigger(0, LT, KEY_VR_LTRIGGER);
		Trigger(0, RT, KEY_VR_RTRIGGER);
	}

	// The VR controllers' default bindings - a pad's jobs, on the VR keys - put
	// in when NONE of the VR keys is bound: a new install gets them from
	// commonbinds.txt, but the engine applies default bindings only to an ini
	// that has none, so an ini from before the VR keys existed would leave the
	// controllers dead. Keep in step with commonbinds.txt. Once, on the first
	// controller read (the ini's bindings are loaded by then).
	/*
		The weapon wheel: hold the
		turning stick's click and a ring of the weapons carried opens round the
		gun hand, each the gun's own 3D model, side on, on a dark disc whose
		slice under the hand lights in the game's colour; the game runs at
		vr_wheel_slowmo meanwhile. Move the hand towards a gun and let go to
		take it. Let go with nothing chosen inside 0.25 s and the click goes to
		the game as it always did (+zoom). Here: the button, the ring's place
		in the room and the slow motion; doomvr_hands.cpp lists the guns,
		chooses and draws.
	*/
	bool gWheelClick = false;		// the stick click, taken by the wheel
	double gWheelClickAt = 0;
	int gWheelTap = 0;				// a tap passed on to the game: 2 press, 1 release
	bool gWheelOpen = false;
	int gWheelCount = 0, gWheelHighlight = -1;
	pcvrxr::Vec3 gWheelCentre, gWheelAcross, gWheelToward;	// room space; up is the room's
	double gWheelYaw = 0;			// turns a gun's barrel to point along gWheelAcross
	bool gWheelModelOn = false;
	float gWheelModelX = 0, gWheelModelY = 0, gWheelModelK = 1;
	// A holstered gun drawn on the body (DoomVR_SetHolsterModel): its middle
	// in the room and its turn.
	bool gPlacedModelOn = false;
	pcvrxr::Vec3 gPlacedAt;
	pcvrxr::Quat gPlacedQ{ 0, 0, 0, 1 };
	bool gWheelSlowed = false;
	float gWheelSavedScale = 1;

	// The game slowed while the ring is up: the engine's own time scale, with
	// the input clock re-based on each change, or the frame's turning reads
	// the jump in the scaled clock as one huge step (seen in another of our ports).
	void WheelSlow(bool on)
	{
		if (on && !gWheelSlowed && !netgame)
		{
			const float want = std::clamp(float(vr_wheel_slowmo), 0.05f, 1.0f);
			if (want >= 0.999f) return;
			gWheelSavedScale = i_timescale;
			i_timescale = want;
			I_ResetInputTime();
			gWheelSlowed = true;
		}
		else if (!on && gWheelSlowed)
		{
			i_timescale = gWheelSavedScale;
			I_ResetInputTime();
			gWheelSlowed = false;
		}
	}

	void WheelClose(bool choose)
	{
		if (!gWheelOpen) return;
		gWheelOpen = false;
		WheelSlow(false);
		if (choose && gWheelHighlight >= 0) DoomVR_WheelChoose(gWheelHighlight);
		Printf(PRINT_NONOTIFY, "DoomVR: weapon wheel closed%s\n", choose && gWheelHighlight >= 0 ? ", a weapon chosen" : "");
		gWheelHighlight = -1;
	}

	// The ring opens round the gun hand, square to where the head faces.
	bool WheelOpen()
	{
		if (!gSession) return false;
		const pcvrxr::HandState& r = Hand(pcvrxr::Hand::Right);
		if (!r.active) return false;
		gWheelCount = DoomVR_WheelGather();
		if (gWheelCount <= 0) return false;
		double hy, hp, hr;
		ToYawPitchRoll(gSession->Head().orientation, hy, hp, hr);
		const double fx = -sin(hy), fz = -cos(hy);	// the head's forward, level
		gWheelAcross = { float(-fz), 0.f, float(fx) };	// its right
		gWheelToward = { float(-fx), 0.f, float(-fz) };
		gWheelCentre = r.aim.position;
		// A turn about up by y takes -z to (-sin y, 0, -cos y): the barrel along the right.
		gWheelYaw = atan2(-double(gWheelAcross.x), -double(gWheelAcross.z));
		gWheelOpen = true;
		gWheelHighlight = -1;
		WheelSlow(true);
		Printf(PRINT_NONOTIFY, "DoomVR: weapon wheel open, %d weapons\n", gWheelCount);
		return true;
	}

	// The ring's slice under the gun hand: the nearest by angle, once the hand
	// is 6 cm from where the ring opened.
	void WheelPick()
	{
		if (vr_wheel_debug >= 2) { gWheelHighlight = (vr_wheel_debug - 2) % std::max(1, gWheelCount); return; }
		const pcvrxr::HandState& r = Hand(pcvrxr::Hand::Right);
		if (!r.active) return;
		const double dx = r.aim.position.x - gWheelCentre.x, dy = r.aim.position.y - gWheelCentre.y, dz = r.aim.position.z - gWheelCentre.z;
		const double x = dx * gWheelAcross.x + dz * gWheelAcross.z, y = dy;
		if (sqrt(x * x + y * y) < 0.06) { gWheelHighlight = -1; return; }
		const double ang = atan2(y, x) * kToDeg;
		double best = 1e9;
		for (int i = 0; i < gWheelCount; ++i)
		{
			const double diff = fabs(fmod(ang - DoomVR_WheelAngle(i, gWheelCount) + 540.0, 360.0) - 180.0);
			if (diff < best) { best = diff; gWheelHighlight = i; }
		}
	}

	/*
		The turning stick's click, for the wheel. True when the wheel has it
		(the game must not see it); false hands it back to the game as before.
		can: play, no menu, no tuner, no photo mode.
	*/
	bool WheelButton(bool down, EKeyCodes key, bool can)
	{
		// A tap passed on: pressed one frame, let go the next.
		if (gWheelTap == 2) { Joy_GenerateButtonEvent(true, key); gWheelTap = 1; return true; }
		if (gWheelTap == 1) { Joy_GenerateButtonEvent(false, key); gWheelTap = 0; return true; }
		can = can && vr_weapon_wheel && gamestate == GS_LEVEL && DoomVR_PlayerAlive() && automapactive == false;
		if (vr_wheel_debug > 0) down = true;
		if (!can)
		{
			WheelClose(false);
			const bool had = gWheelClick;
			if (!down) gWheelClick = false;
			return had;	// a click the wheel took stays the wheel's until let go
		}
		if (down && !gWheelClick)
		{
			gWheelClick = true;
			gWheelClickAt = NowMs();
			if (!WheelOpen()) { gWheelClick = false; return false; }	// nothing to show: the game's click
		}
		else if (!down && gWheelClick)
		{
			gWheelClick = false;
			const bool chose = gWheelHighlight >= 0;
			WheelClose(true);
			if (!chose && NowMs() - gWheelClickAt < 250) gWheelTap = 2;
			return true;
		}
		if (gWheelOpen) WheelPick();
		return gWheelClick;
	}

	void VRDefaultBinds()
	{
		for (int k = KEY_VR_A; k <= KEY_VR_RSTICK_DOWN; ++k)
			if (!Bindings.GetBinding(k).IsEmpty() || !AutomapBindings.GetBinding(k).IsEmpty()) return;
		static const struct { int key; const char* bind; } kGame[] = {
			{ KEY_VR_A, "+use" }, { KEY_VR_B, "crouch" }, { KEY_VR_X, "+reload" }, { KEY_VR_Y, "+jump" },
			{ KEY_VR_RTRIGGER, "+attack" }, { KEY_VR_LTRIGGER, "+altattack" },
			{ KEY_VR_LGRIP, "weapprev" }, { KEY_VR_RGRIP, "weapnext" }, { KEY_VR_MENU, "menu_main" },
			{ KEY_VR_RSTICK, "+zoom" }, { KEY_VR_RSTICK_UP, "invuse" }, { KEY_VR_RSTICK_DOWN, "invnext" },
		};
		static const struct { int key; const char* bind; } kMap[] = {
			{ KEY_VR_X, "am_togglefollow" }, { KEY_VR_A, "am_setmark" }, { KEY_VR_B, "am_clearmarks" },
			{ KEY_VR_LGRIP, "+am_zoomout" }, { KEY_VR_RGRIP, "+am_zoomin" },
		};
		for (auto& b : kGame) Bindings.SetBind(b.key, b.bind);
		for (auto& b : kMap) AutomapBindings.SetBind(b.key, b.bind);
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: VR controller bindings set to the defaults (none were bound)\n");
	}

	// The Steam Frame's defaults: the official Steam releases' own gamepad
	// layouts, where VR does not need the
	// button for something else - the left trigger stays the left-hand gun,
	// the grips recenter and point, crouch and reload go where those games
	// have nothing. Put in when none of the Frame's keys is bound, so an ini
	// from before the Frame keys existed gets them once, per game.
	//
	//              Doom + Doom II (official)    Heretic + Hexen (official)
	//   A          use                          jump
	//   B          crouch (ours)                run
	//   X          reload (ours)                use
	//   Y          automap                      automap
	//   LB / RB    previous / next weapon       previous / next weapon
	//   D-pad      weapons 2, 3, 4, 5           fly up / down, inventory left / right
	//   L stick    run (ours)                   crouch (ours)
	//   R stick    zoom (ours)                  use the inventory item
	//   View       scores                       scores
	//   Menu       menu                         menu
	//   Automap: the D-pad pans, LB / RB zoom.
	void FrameDefaultBinds()
	{
		for (int k = KEY_VRF_A; k <= KEY_VRF_RGRIP; ++k)
			if (!Bindings.GetBinding(k).IsEmpty() || !AutomapBindings.GetBinding(k).IsEmpty()) return;
		struct Bind { int key; const char* bind; };
		static const Bind kBoth[] = {
			{ KEY_VRF_Y, "togglemap" }, { KEY_VRF_LB, "weapprev" }, { KEY_VRF_RB, "weapnext" },
			{ KEY_VRF_VIEW, "+showscores" }, { KEY_VRF_MENU, "menu_main" },
		};
		static const Bind kDoom[] = {
			{ KEY_VRF_A, "+use" }, { KEY_VRF_B, "crouch" }, { KEY_VRF_X, "+reload" },
			{ KEY_VRF_DPADUP, "slot 2" }, { KEY_VRF_DPADRIGHT, "slot 3" },
			{ KEY_VRF_DPADDOWN, "slot 4" }, { KEY_VRF_DPADLEFT, "slot 5" },
			{ KEY_VRF_LSTICK, "+vr_run" }, { KEY_VRF_RSTICK, "+zoom" },
		};
		static const Bind kInventory[] = {
			{ KEY_VRF_A, "+jump" }, { KEY_VRF_B, "+vr_run" }, { KEY_VRF_X, "+use" },
			{ KEY_VRF_DPADUP, "+moveup" }, { KEY_VRF_DPADDOWN, "+movedown" },
			{ KEY_VRF_DPADLEFT, "invprev" }, { KEY_VRF_DPADRIGHT, "invnext" },
			{ KEY_VRF_LSTICK, "crouch" }, { KEY_VRF_RSTICK, "invuse" },
		};
		static const Bind kMap[] = {
			{ KEY_VRF_DPADUP, "+am_panup" }, { KEY_VRF_DPADDOWN, "+am_pandown" },
			{ KEY_VRF_DPADLEFT, "+am_panleft" }, { KEY_VRF_DPADRIGHT, "+am_panright" },
			{ KEY_VRF_LB, "+am_zoomout" }, { KEY_VRF_RB, "+am_zoomin" },
		};
		const bool inventory = DoomVR_InventoryGame();
		for (auto& b : kBoth) Bindings.SetBind(b.key, b.bind);
		if (inventory) for (auto& b : kInventory) Bindings.SetBind(b.key, b.bind);
		else for (auto& b : kDoom) Bindings.SetBind(b.key, b.bind);
		for (auto& b : kMap) AutomapBindings.SetBind(b.key, b.bind);
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: Steam Frame bindings set to the %s defaults (none were bound)\n", inventory ? "Heretic + Hexen" : "Doom + Doom II");
	}
	bool gBindsChecked = false;

	// The menu pointer (vr_menu_pointer): where the weapon hand's aim ray meets
	// the menu screen, in canvas pixels, and whether the trigger is holding a
	// click. Mouse events carry it to the menu - UZDoom's menus already take
	// the mouse, and ignore moves under 1% of the screen, which steadies a
	// hand. Only while a menu is up on the screen panel.
	bool gPointerOn = false, gPointerDown = false;
	int gPointerX = 0, gPointerY = 0;
	// The pointer is the right grip HELD, as in A Kingdom for Keflings VR (an
	// always-on pointer fought the stick's selection).
	// In a menu the grip's key is held back: let go within kGripHoldFrames and
	// it was a tap (Page Down, as before); hold it and the pointer is on.
	constexpr int kGripHoldFrames = 20;
	int gGripFrames = 0;
	bool gGripPointer = false;

	void MenuPointer(const pcvrxr::HandState& r)
	{
		bool on = false;
		int px = 0, py = 0;
		pcvrxr::Pose pp;
		float pw = 0, ph = 0;
		if (vr_menu_pointer && gGripPointer && menuactive != MENU_Off && gPanel && gPanelMode == PANEL_SCREEN && r.active &&
		    gSession->PanelPlacement(pp, pw, ph) && pw > 0 && ph > 0)
		{
			const pcvrxr::Vec3 o = r.aim.position;
			const pcvrxr::Vec3 d = Rotate(r.aim.orientation, 0, 0, -1);
			const pcvrxr::Vec3 n = Rotate(pp.orientation, 0, 0, 1);
			const pcvrxr::Vec3 rx = Rotate(pp.orientation, 1, 0, 0), uy = Rotate(pp.orientation, 0, 1, 0);
			const double den = d.x * n.x + d.y * n.y + d.z * n.z;
			if (fabs(den) > 1e-4)
			{
				const double t = ((pp.position.x - o.x) * n.x + (pp.position.y - o.y) * n.y + (pp.position.z - o.z) * n.z) / den;
				if (t > 0)
				{
					const double qx = o.x + d.x * t - pp.position.x, qy = o.y + d.y * t - pp.position.y, qz = o.z + d.z * t - pp.position.z;
					const double u = (qx * rx.x + qy * rx.y + qz * rx.z) / pw + 0.5;
					const double v = 0.5 - (qx * uy.x + qy * uy.y + qz * uy.z) / ph;
					if (u >= 0 && u <= 1 && v >= 0 && v <= 1)
					{
						on = true;
						px = int(u * screen->GetWidth());
						py = int(v * screen->GetHeight());
					}
				}
			}
		}
		if (on && (!gPointerOn || px != gPointerX || py != gPointerY))
		{
			event_t ev = {};
			ev.type = EV_GUI_Event;
			ev.subtype = EV_GUI_MouseMove;
			ev.data1 = px;
			ev.data2 = py;
			D_PostEvent(&ev);
		}
		// The trigger clicks: pressed past 0.7, let go under 0.4. The menus act
		// on the release, and only over the row that was pressed - so the
		// release goes where the press was: a hand drifts while it pulls a
		// trigger, and a mouse does not.
		static int pressX = 0, pressY = 0;
		const bool press = on && r.trigger > 0.7f, release = !on || r.trigger < 0.4f;
		if ((press && !gPointerDown) || (release && gPointerDown))
		{
			event_t ev = {};
			ev.type = EV_GUI_Event;
			const bool down = press && !gPointerDown;
			if (down) { pressX = px; pressY = py; }
			ev.subtype = down ? EV_GUI_LButtonDown : EV_GUI_LButtonUp;
			ev.data1 = pressX;
			ev.data2 = pressY;
			if (!down)
			{
				// Back to the pressed spot first, so the menu sees the release there.
				event_t mv = {};
				mv.type = EV_GUI_Event;
				mv.subtype = EV_GUI_MouseMove;
				mv.data1 = pressX;
				mv.data2 = pressY;
				D_PostEvent(&mv);
			}
			D_PostEvent(&ev);
			gPointerDown = down;
			DPrintf(DMSG_NOTIFY, "DoomVR: menu pointer %s at %d,%d\n", down ? "pressed" : "released", pressX, pressY);
		}
		if (on != gPointerOn) Printf(PRINT_NONOTIFY, "DoomVR: menu pointer %s\n", on ? "on (right grip held)" : "off");
		gPointerOn = on;
		if (on) { gPointerX = px; gPointerY = py; }
	}

	/*
		Physical play, two holsters on the body: the
		hip holster on the gun side holds the last one-handed gun used, the one
		on top of the gun-side shoulder the last two-handed gun (doomvr_hands.cpp
		chooses them and switches, the way the weapon wheel does). Reach the gun
		hand there and squeeze grip: the gun comes out; at the holster of the
		gun in hand, the grip puts it away. Every other weapon lives in the
		wheel.
		The zones are measured from the NECK (10 cm below and 8 cm behind the
		eyes, turning with the head), so looking down does not carry them
		forward, on a BODY that faces where the head faces once the head has
		turned more than 45 degrees (a dead zone, never a lag), yaw only, and
		that holds still while the gun hand is in a zone:
		  hip:      40-95 cm below the neck, 8 cm or more out to the gun side,
		            within 35 cm ahead or behind
		  shoulder: on top of it - 25 cm below to 20 cm above the neck, 10-45
		            cm out to the gun side, 15 cm ahead to 20 cm behind
		Seated reach (vr_phys_holster_lift) lifts both. Only the gun hand
		counts. A grab is a REACH: the grip must close within 1.5 s of the hand
		arriving, so an arm resting at the side does not draw; 2 cm of
		hysteresis at a zone's edge. The Leftorium puts them on the other side.
	*/
	struct HolsterZone
	{
		const char* name;
		double outFrom, outTo, downFrom, downTo, ahead, behind;	// m from the neck
		double spotOut, spotDown, spotBehind;	// where its ring is drawn
	};
	const HolsterZone kHolsterZones[] = {
		{ "hip",      0.08, 0.60,  0.40, 0.95, 0.35, 0.35,  0.22, 0.50, 0.00 },
		{ "shoulder", 0.10, 0.45, -0.20, 0.25, 0.15, 0.20,  0.20, 0.02, 0.03 },
	};
	constexpr int kHolsterCount = 2;
	constexpr double kNeckDown = 0.10, kNeckBack = 0.08;	// the neck from the eyes, in the head's frame
	constexpr double kBodyDeadZone = 45.0;	// degrees the head turns before the body follows
	bool gTwoHandGrip = false;	// the other hand holds the gun's front (and the frame it lets go)
	bool gMouthGrip = false, gMouthGripWas = false;	// the other hand's grip used an item at the mouth
	double gBodyYaw = 0;
	bool gBodyYawSet = false, gHolsterHeld = false, gHolsterGripWas = false;
	int gHolsterInside = -1, gHolstersLogged = 0;
	double gHolsterEnteredMs = 0;	// when the gun hand arrived in gHolsterInside
	bool gHolsterRearm = true;		// holsters were away: a hand found in one is resting there
	constexpr double kHolsterReachMs = 1500;
	// This frame's body, for the markers and the holstered guns: the neck in
	// the room, the body's yaw, the side the gun hand is on, and how far the
	// gun hand is from each holster's ring (valid only while holsters can be used).
	pcvrxr::Vec3 gNeck;
	double gHolsterDist[kHolsterCount];
	bool gHolstersValid = false;

	// The body's axes in the room: right, forward (yaw only).
	void BodyAxes(double& rx, double& rz, double& fx, double& fz)
	{
		rx = cos(gBodyYaw); rz = -sin(gBodyYaw);
		fx = -sin(gBodyYaw); fz = -cos(gBodyYaw);
	}

	// A point on the body (out to the gun side, down, behind; m) in the room.
	pcvrxr::Vec3 BodyPoint(double out, double down, double behind)
	{
		double rx, rz, fx, fz;
		BodyAxes(rx, rz, fx, fz);
		const double x = (Leftorium() ? -1.0 : 1.0) * out;
		return { float(gNeck.x + rx * x - fx * behind), float(gNeck.y - down), float(gNeck.z + rz * x - fz * behind) };
	}

	// Returns true while a holster grab holds the gun hand's grip, and on the
	// frame it is let go - so a quick grab is not also the grip's tap.
	bool Holsters(const pcvrxr::HandState& r, bool gripDown)
	{
		const bool pressed = gripDown && !gHolsterGripWas;
		gHolsterGripWas = gripDown;
		const bool wasHeld = gHolsterHeld;
		if (!gripDown) gHolsterHeld = false;
		const bool can = vr_phys_holsters && r.active && gamestate == GS_LEVEL && menuactive == MENU_Off
		              && ConsoleState != c_down && ConsoleState != c_falling && DoomVR_PlayerAlive() && !paused;
		gHolstersValid = can;
		// Away (a menu, the console, a pause, a new level, death), the hand is
		// not followed; on coming back, a hand already in a holster counts as
		// RESTING there, not arriving.
		if (!can) { gHolsterInside = -1; gHolsterRearm = true; return gHolsterHeld || wasHeld; }

		const pcvrxr::Pose& head = gSession->Head();
		double hy, hp, hr;
		ToYawPitchRoll(head.orientation, hy, hp, hr);
		if (!gBodyYawSet) { gBodyYaw = hy; gBodyYawSet = true; }
		// The body turns once the head is past the dead zone, at once; never
		// while the gun hand is in a holster (it would move under the hand).
		if (gHolsterInside < 0)
		{
			double lag = hy - gBodyYaw;
			while (lag > 3.14159265358979) lag -= 2 * 3.14159265358979;
			while (lag < -3.14159265358979) lag += 2 * 3.14159265358979;
			const double maxLag = kBodyDeadZone * kToRad;
			if (lag > maxLag) gBodyYaw = hy - maxLag;
			else if (lag < -maxLag) gBodyYaw = hy + maxLag;
		}
		const pcvrxr::Vec3 neckOff = Rotate(head.orientation, 0, -kNeckDown, kNeckBack);
		gNeck = { head.position.x + neckOff.x, head.position.y + neckOff.y, head.position.z + neckOff.z };

		// The gun hand from the neck, on the body's axes: out to the gun
		// side, down, behind.
		double rx, rz, fx, fz;
		BodyAxes(rx, rz, fx, fz);
		const double dx = r.grip.position.x - gNeck.x, dy = r.grip.position.y - gNeck.y, dz = r.grip.position.z - gNeck.z;
		const double out = (Leftorium() ? -1.0 : 1.0) * (dx * rx + dz * rz), down = -dy, behind = -(dx * fx + dz * fz);
		const double lift = std::clamp(double(vr_phys_holster_lift), 0.0, 0.20);

		int inside = -1;
		for (int i = 0; i < kHolsterCount; ++i)
		{
			const HolsterZone& z = kHolsterZones[i];
			// 2 cm more for the holster the hand is already in, so an edge does
			// not flicker, tick and restart the reach time.
			const double m = i == gHolsterInside ? 0.02 : 0.0;
			const bool in = out >= z.outFrom - m && out <= z.outTo + m &&
			                down >= z.downFrom - lift - m && down <= z.downTo - lift + m &&
			                behind >= -z.ahead - m && behind <= z.behind + m;
			const pcvrxr::Vec3 spot = BodyPoint(z.spotOut, z.spotDown - lift, z.spotBehind);
			const double ex = r.grip.position.x - spot.x, ey = r.grip.position.y - spot.y, ez = r.grip.position.z - spot.z;
			gHolsterDist[i] = in ? 0.0 : sqrt(ex * ex + ey * ey + ez * ez);
			if (in && inside < 0) inside = i;
		}
		if (gHolsterRearm)
		{
			gHolsterRearm = false;
			gHolsterInside = inside;
			gHolsterEnteredMs = -1e9;
		}
		if (inside >= 0 && inside != gHolsterInside) { DoomVR_Pulse(2, 0.02f, 0.3f, "holster in reach"); gHolsterEnteredMs = NowMs(); }
		gHolsterInside = inside;
		// Every grip on the body, so the zones can be fitted from a log.
		if (pressed && gHolstersLogged < 40 && down > 0.0)
		{
			++gHolstersLogged;
			Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: gun-hand grip %.2f m out, %.2f down, %.2f %s from the neck%s%s\n", out, down, fabs(behind),
			       behind >= 0 ? "behind" : "ahead", inside >= 0 ? " - in the " : "", inside >= 0 ? kHolsterZones[inside].name : "");
		}
		if (pressed && inside >= 0 && NowMs() - gHolsterEnteredMs > kHolsterReachMs && gHolstersLogged < 40)
		{
			++gHolstersLogged;
			Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: grip at the %s holster, but the hand %s - not a draw\n", kHolsterZones[inside].name,
			       gHolsterEnteredMs < 0 ? "was already there when play resumed" : "had rested there over 1.5 s");
		}
		if (pressed && inside >= 0 && NowMs() - gHolsterEnteredMs <= kHolsterReachMs)
		{
			gHolsterHeld = true;
			if (DoomVR_HolsterDraw(inside)) DoomVR_Pulse(2, 0.05f, 0.6f, "holster draw");
			else { DoomVR_Pulse(2, 0.03f, 0.25f, "holster refused"); DoomVR_Pulse(2, 0.03f, 0.25f, "holster refused"); }
		}
		return gHolsterHeld || wasHeld;
	}

	void ReadControllers()
	{
		if (!gBindsChecked) { gBindsChecked = true; VRDefaultBinds(); FrameDefaultBinds(); }
		gSession->SetSwapHands(Leftorium());
		TomeKeep(Tome());
		pcvrxr::HandState l = Hand(pcvrxr::Hand::Left);
		pcvrxr::HandState r = Hand(pcvrxr::Hand::Right);
		{
			const bool pad = (l.active && l.padLayout) || (r.active && r.padLayout);
			if (pad != gPadLayout)
			{
				// Everything held is let go under the keys it went down with.
				for (int i = 0; i < kNumButtons; ++i)
				{
					if (gButtonDown[i] && EffectiveKey(i) != 0) Joy_GenerateButtonEvent(false, EffectiveKey(i));
					gButtonDown[i] = false;
				}
				for (int i = 0; i < kNumFrame; ++i)
				{
					if (gFrameDown[i]) Joy_GenerateButtonEvent(false, kFrame[i].key);
					gFrameDown[i] = false;
				}
				gPadLayout = pad;
				Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: controller layout: %s\n", pad ? "Steam Frame (gamepad)" : "VR controllers");
			}
		}
		if (SticksSwapped())
		{
			// A controller that is off gives no stick, whichever hand it is read as.
			if (!l.active) { l.thumbstick = {}; l.thumbstickClick = {}; }
			if (!r.active) { r.thumbstick = {}; r.thumbstickClick = {}; }
			std::swap(l.thumbstick, r.thumbstick);
			std::swap(l.thumbstickClick, r.thumbstickClick);
		}

		// Both grips held together: recenter - in play AND in menus, where it
		// also puts the menu screen back in front of the eyes (Game Or Die
		// Features, section 9, as A Kingdom for Keflings VR and Episode I
		// Racer have it; never both stick clicks, which is Virtual Desktop's
		// overlay). A lone grip waits kGripChordWaitMs (0.12 s) for
		// the other before it reaches the game; let go sooner, it arrives as
		// a tap. While the chord is held neither grip does anything else.
		const bool holsterGrab = Holsters(r, r.active && r.squeezeClick.down);
		{
			/*
				Physical play, items to the mouth: in a
				game with an inventory (Heretic, Hexen, Strife), the other hand
				gripped at the mouth - within 13 cm of a point 8 cm below and 6 cm
				in front of the eyes - uses the selected item, the game's own
				'invuse': a flask is drunk where a person drinks. The grip's tap
				does not fire; the item buttons stay. Off until worn (vr_phys_use).
			*/
			const bool down = l.active && l.squeezeClick.down;
			const bool pressed = down && !gMouthGripWas;
			gMouthGripWas = down;
			const bool wasMouth = gMouthGrip;
			if (!down) gMouthGrip = false;
			if (pressed && vr_phys_use && DoomVR_InventoryGame() && gamestate == GS_LEVEL && menuactive == MENU_Off)
			{
				const pcvrxr::Pose& head = gSession->Head();
				const pcvrxr::Vec3 mouth = Rotate(head.orientation, 0, -0.08, -0.06);
				const double ex = head.position.x + mouth.x - l.grip.position.x, ey = head.position.y + mouth.y - l.grip.position.y,
				             ez = head.position.z + mouth.z - l.grip.position.z;
				if (sqrt(ex * ex + ey * ey + ez * ez) < 0.13)
				{
					AddCommandString("invuse");
					DoomVR_Pulse(1, 0.05f, 0.5f, "item at the mouth");
					Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: item used at the mouth\n");
					gMouthGrip = true;
				}
			}
			gMouthGrip = gMouthGrip || (wasMouth && !down);
		}
		{
			const bool down[2] = { l.active && l.squeezeClick.down, r.active && r.squeezeClick.down };
			bool eff[2] = {};
			const double gripNow = NowMs();
			auto heldMs = [&](int g) { return gGripHeld[g] > 0 ? gripNow - gGripSince[g] : 0.0; };
			if (!gGripChord && down[0] && down[1] && heldMs(0) < kGripChordWaitMs && heldMs(1) < kGripChordWaitMs)
			{
				gGripChord = true;
				gRecenterAsked = true;
				gRecenterWhy = "both grips held";
			}
			for (int g = 0; g < 2; ++g)
			{
				/*
					With a two-handed gun up, the other hand's grip held is the
					hold on the gun's front, never its binding (weapprev): a
					squeeze that missed the front grip switched weapons. Only a tap - let go inside 0.25 s -
					reaches the game then.
				*/
				const bool tapOnly = g == 0 && DoomVR_TwoHandGunHeld();
				const double tapMs = tapOnly ? kGripTapOnlyMs : kGripChordWaitMs;
				if (!down[g] && gGripHeld[g] > 0 && heldMs(g) <= tapMs && !gGripChord) gGripTap[g] = 2;
				if (down[g] && gGripHeld[g] == 0) gGripSince[g] = gripNow;
				gGripHeld[g] = down[g] ? gGripHeld[g] + 1 : 0;
				eff[g] = !gGripChord && ((!tapOnly && down[g] && heldMs(g) > kGripChordWaitMs) || gGripTap[g] > 0);
				if (gGripTap[g] > 0) --gGripTap[g];
			}
			if (gGripChord && !down[0] && !down[1]) gGripChord = false;
			if (holsterGrab) gGripTap[1] = 0;	// a holster grab is never also a tap
			if (gTwoHandGrip || gMouthGrip) gGripTap[0] = 0;	// nor is a hand on the gun's front, or at the mouth
			l.squeezeClick.down = eff[0] && !gTwoHandGrip && !gMouthGrip;
			r.squeezeClick.down = eff[1] && !holsterGrab;
		}
		MenuPointer(r);

		// Photo mode, as our other ports have it: with the game paused, a click of the
		// LEFT THUMBSTICK hides the pause menu so the player can look round
		// the frozen world and take their own screenshot; another click
		// brings it back. Read here, on the button's edge: a menu swallows
		// keys it does not know, so a binding would never arrive.
		const bool paused2D = menuactive != MENU_Off || ConsoleState == c_down || ConsoleState == c_falling;
		const bool click = l.active && l.thumbstickClick.down;
		const bool rclick = r.active && r.thumbstickClick.down;
		const bool wasPhoto = gPhotoMode;
		if (!paused2D) gPhotoMode = false;
		else if (click && !gPhotoClickDown) gPhotoMode = !gPhotoMode;
		gPhotoClickDown = click;

		// Any other control while the menu is hidden brings it straight back -
		// the grips, triggers, face buttons, menu button or the right stick's
		// click. The left stick does nothing and the right stick turns.
		bool anyOther = false;
		for (int i = 0; i < kNumButtons; ++i)
		{
			if (kButtons[i].key == KEY_VR_LSTICK) continue;
			if (ButtonDown(i, l, r)) anyOther = true;
		}
		if (gPadLayout)
			for (int i = 0; i < kNumFrame; ++i)
				if (FrameDown(i, l, r)) anyOther = true;
		if ((l.active && l.trigger > 0.5f) || (r.active && r.trigger > 0.5f)) anyOther = true;
		if (gPhotoMode && wasPhoto && anyOther)
		{
			gPhotoMode = false;
			gSwallowUntilReleased = true;
		}
		if (gSwallowUntilReleased && !anyOther) gSwallowUntilReleased = false;

		// Turning the frozen world with the right stick while the menu is
		// hidden: smooth at the turn speed, or one snap per push.
		{
			const double now = NowMs();
			const double dt = gPhotoLastMs > 0 ? std::clamp((now - gPhotoLastMs) / 1000.0, 0.0, 0.1) : 0.0;
			gPhotoLastMs = now;
			const float sx = r.active ? r.thumbstick.x : 0.f;
			if (!gPhotoMode) gPhotoYaw = 0;
			else if (vr_snap_turn > 0)
			{
				if (fabsf(sx) < 0.35f) gPhotoSnapArmed = true;
				else if (gPhotoSnapArmed && fabsf(sx) > 0.75f) { gPhotoSnapArmed = false; gPhotoYaw += sx > 0 ? -double(vr_snap_turn) : double(vr_snap_turn); }
			}
			else if (fabsf(sx) > 0.15f)
				gPhotoYaw -= (fabsf(sx) - 0.15f) / 0.85f * (sx > 0 ? 1.0 : -1.0) * double(vr_turn_speed) * dt;
		}

		// The weapon tuner (vr_tune, doomvr_tune.cpp): the right stick fits the
		// weapon in the hand instead of turning, its click steps the mode and
		// the left stick's click picks the other hand. Neither click reaches
		// the game meanwhile.
		const bool tuning = !paused2D && DoomVR_TuneActive();
		{
			const double now = NowMs();
			const double dt = gTuneLastMs > 0 ? std::clamp((now - gTuneLastMs) / 1000.0, 0.0, 0.1) : 0.0;
			gTuneLastMs = now;
			if (tuning)
				DoomVR_TuneInput(r.active ? r.thumbstick.x : 0.f, r.active ? r.thumbstick.y : 0.f,
				                 rclick && !gTuneRClick, click && !gTuneLClick, dt);
			gTuneRClick = rclick;
			gTuneLClick = click;
		}
		gRunHeld = !paused2D && !tuning && ((click && !gPadLayout) || gRunCmd);

		// A held button never acts in the next screen: after a menu opens or
		// closes, the game's screen changes (a new level, the intermission),
		// the weapon changes or a controller is found again, a button held
		// then counts only once it has been seen up - a trigger held while
		// closing the menu fires no shot, an A held through a level load
		// clicks nothing.
		{
			const bool menu = menuactive != MENU_Off;
			const void* weapon = DoomVR_ReadyWeaponId();
			const bool moment = menu != gFreshMenu || int(gamestate) != gFreshState || weapon != gFreshWeapon ||
			                    (l.active && !gFreshL) || (r.active && !gFreshR);
			gFreshMenu = menu; gFreshState = int(gamestate); gFreshWeapon = weapon; gFreshL = l.active; gFreshR = r.active;
			if (moment)
			{
				for (int i = 0; i < kNumButtons; ++i) if (ButtonDown(i, l, r)) gMustRelease[i] = true;
				for (int i = 0; i < kNumFrame; ++i) if (gPadLayout && FrameDown(i, l, r)) gFrameMustRelease[i] = true;
			}
		}
		for (int i = 0; i < kNumButtons; ++i)
		{
			bool down = ButtonDown(i, l, r);
			if (gMustRelease[i])
			{
				if (down) down = false;
				else gMustRelease[i] = false;
			}
			// While paused the left stick click is photo mode's alone; while
			// the menu is hidden nothing else reaches it either, or a press
			// would pick a menu row nobody can see - and the press that brings
			// the menu back is held off until it is released.
			if (paused2D && kButtons[i].key == KEY_VR_LSTICK) down = false;
			if (gPhotoMode || gSwallowUntilReleased) down = false;
			if (tuning && (kButtons[i].key == KEY_VR_LSTICK || kButtons[i].key == KEY_VR_RSTICK)) down = false;
			const EKeyCodes key = EffectiveKey(i);
			if (key == 0) { gButtonDown[i] = false; continue; }
			if (kButtons[i].key == KEY_VR_RSTICK && WheelButton(down, key, !paused2D && !tuning && !gPhotoMode))
			{
				if (gButtonDown[i]) Joy_GenerateButtonEvent(false, key);
				gButtonDown[i] = false;
				continue;
			}
			if (kButtons[i].key == KEY_VR_RGRIP && menuactive != MENU_Off && vr_menu_pointer && !gButtonDown[i])
			{
				// Tap or hold: the key waits until we know which.
				if (down)
				{
					if (++gGripFrames >= kGripHoldFrames) gGripPointer = true;
				}
				else
				{
					if (gGripFrames > 0 && !gGripPointer)
					{
						Joy_GenerateButtonEvent(true, key);
						Joy_GenerateButtonEvent(false, key);
					}
					gGripFrames = 0;
					gGripPointer = false;
				}
				continue;
			}
			if (kButtons[i].key == KEY_VR_RGRIP) { gGripFrames = 0; gGripPointer = false; }
			if (down != gButtonDown[i]) Joy_GenerateButtonEvent(down, key);
			gButtonDown[i] = down;
		}
		// The Frame's own buttons. Nothing reaches the hidden menu in photo
		// mode, and the press that brings it back waits to be let go.
		for (int i = 0; i < kNumFrame; ++i)
		{
			bool down = gPadLayout && FrameDown(i, l, r);
			if (gFrameMustRelease[i])
			{
				if (down) down = false;
				else gFrameMustRelease[i] = false;
			}
			if (gPhotoMode || gSwallowUntilReleased) down = false;
			if (down != gFrameDown[i]) Joy_GenerateButtonEvent(down, kFrame[i].key);
			gFrameDown[i] = down;
		}

		// OpenXR's stick Y is up-positive; the pad's is down-positive. In photo
		// mode the stick would move the hidden menu's cursor, so it rests.
		const bool lstick = l.active && !gPhotoMode;
		Stick(lstick ? l.thumbstick.x : 0.f, lstick ? -l.thumbstick.y : 0.f, LX, LY, KEY_VR_LSTICK_RIGHT);
		// The right stick is DoomVR's own turning (DoomVR_TurnThisTic), not a
		// pad stick: UZDoom's turn speed was never meant for a head that also
		// turns, and its up/down would pitch an aim nobody can see. So the pad
		// sees it at rest.
		Stick(0.f, 0.f, RX, RY, KEY_PAD_RTHUMB_RIGHT);
		const float x = r.active && !tuning ? r.thumbstick.x : 0.f;
		const bool inMenu = menuactive != MENU_Off || tuning;
		gTurnStick = inMenu ? 0.f : x;
		// The inventory flick: mostly vertical, past 0.75; released under 0.35.
		{
			const float fy = r.active && !inMenu ? r.thumbstick.y : 0.f;
			EKeyCodes want = EKeyCodes(0);
			if (fabsf(fy) > 0.75f && fabsf(fy) > 2.f * fabsf(x)) want = fy > 0 ? KEY_VR_RSTICK_UP : KEY_VR_RSTICK_DOWN;
			else if (gFlickKey != 0 && fabsf(fy) > 0.35f) want = gFlickKey; // still held
			if (want != gFlickKey)
			{
				if (gFlickKey != 0) Joy_GenerateButtonEvent(false, gFlickKey);
				if (want != 0) Joy_GenerateButtonEvent(true, want);
				gFlickKey = want;
			}
		}
		// A snap is one step per push: fire past 0.75, re-arm under 0.35.
		if (fabsf(x) < 0.35f) gSnapArmed = true;
		else if (gSnapArmed && fabsf(x) > 0.75f && vr_snap_turn > 0 && !inMenu)
		{
			gSnapArmed = false;
			gSnapPending += x > 0 ? -double(vr_snap_turn) : double(vr_snap_turn);
		}
		const bool triggersOff = gPhotoMode || gSwallowUntilReleased;
		Trigger(l.active && !triggersOff ? l.trigger : 0.f, LT, KEY_VR_LTRIGGER);
		Trigger(r.active && !triggersOff ? r.trigger : 0.f, RT, KEY_VR_RTRIGGER);
	}

	// ---- made-up controllers, for testing with none in the room ---------

	int gFakeFrames = 0;       // frames left to hold gFakeControl
	// vr_debug_key: a keyboard key held for some frames, through the game's
	// own event queue (so the console, the menus and the game all see it).
	int gDebugKey = 0, gDebugKeyFrames = 0;
	void ReleaseDebugKey()
	{
		if (gDebugKeyFrames <= 0 || --gDebugKeyFrames > 0) return;
		event_t ev = {};
		ev.type = EV_KeyUp;
		ev.data1 = gDebugKey;
		D_PostEvent(&ev);
	}
	bool gFakeOn = false;      // the layer's fake hands are switched on
	FString gFakeControl;
	double gFakeStartMs = 0;   // when the held control began

	void DriveFakeHands()
	{
		if (!gFakeOn) return;
		pcvrxr::HandState l, r;
		l.active = r.active = true;
		{
			// The right hand points where vr_debug_aim_yaw/pitch say: yaw about
			// +Y then pitch about +X, the order the decomposition expects.
			const double hy = vr_debug_aim_yaw * kToRad / 2, hp = vr_debug_aim_pitch * kToRad / 2;
			const double cy = cos(hy), sy = sin(hy), cp = cos(hp), sp = sin(hp);
			r.aim.orientation.w = float(cy * cp);
			r.aim.orientation.x = float(cy * sp);
			r.aim.orientation.y = float(sy * cp);
			r.aim.orientation.z = float(-sy * sp);
			r.aim.position.x = gSession->Head().position.x + vr_debug_hand_x;
			r.aim.position.y = gSession->Head().position.y + vr_debug_hand_y;
			r.aim.position.z = gSession->Head().position.z + vr_debug_hand_z;
			r.aim.tracked = true;
			r.grip = r.aim;
		}
		if (gFakeFrames > 0)
		{
			const FString& c = gFakeControl;
			if (c.CompareNoCase("a") == 0) r.primary.down = true;
			else if (c.CompareNoCase("b") == 0) r.secondary.down = true;
			else if (c.CompareNoCase("x") == 0) l.primary.down = true;
			else if (c.CompareNoCase("y") == 0) l.secondary.down = true;
			else if (c.CompareNoCase("menu") == 0) l.menu.down = true;
			else if (c.CompareNoCase("lgrip") == 0) { l.squeeze = 1; l.squeezeClick.down = true; }
			else if (c.CompareNoCase("rgrip") == 0) { r.squeeze = 1; r.squeezeClick.down = true; }
			else if (c.CompareNoCase("bothgrips") == 0) { l.squeeze = r.squeeze = 1; l.squeezeClick.down = r.squeezeClick.down = true; }
			// The left grip, and the right joining 90 ms later, as two hands squeeze.
			else if (c.CompareNoCase("gripsapart") == 0)
			{
				l.squeeze = 1; l.squeezeClick.down = true;
				if (NowMs() - gFakeStartMs >= 90.) { r.squeeze = 1; r.squeezeClick.down = true; }
			}
			else if (c.CompareNoCase("lclick") == 0) l.thumbstickClick.down = true;
			else if (c.CompareNoCase("bothclick") == 0) { l.thumbstickClick.down = true; r.thumbstickClick.down = true; }
			else if (c.CompareNoCase("rclick") == 0) r.thumbstickClick.down = true;
			else if (c.CompareNoCase("ltrigger") == 0) { l.trigger = 1; l.triggerClick.down = true; }
			else if (c.CompareNoCase("rtrigger") == 0) { r.trigger = 1; r.triggerClick.down = true; }
			else if (c.CompareNoCase("lup") == 0) l.thumbstick.y = 1;
			else if (c.CompareNoCase("lrun") == 0) { l.thumbstick.y = 1; l.thumbstickClick.down = true; }
			else if (c.CompareNoCase("ldown") == 0) l.thumbstick.y = -1;
			else if (c.CompareNoCase("lleft") == 0) l.thumbstick.x = -1;
			else if (c.CompareNoCase("lright") == 0) l.thumbstick.x = 1;
			else if (c.CompareNoCase("rleft") == 0) r.thumbstick.x = -1;
			else if (c.CompareNoCase("rright") == 0) r.thumbstick.x = 1;
			else if (c.CompareNoCase("rup") == 0) r.thumbstick.y = 1;
			else if (c.CompareNoCase("rdown") == 0) r.thumbstick.y = -1;
			// The Steam Frame's buttons (f_a ... f_menu, f_idle for the layout
			// alone): the hands are on the Frame's profile while it is held.
			else if (c.Len() > 2 && (c[0] == 'f' || c[0] == 'F') && c[1] == '_')
			{
				l.padLayout = r.padLayout = true;
				const FString b = c.Mid(2);
				if (b.CompareNoCase("a") == 0) r.padA.down = true;
				else if (b.CompareNoCase("b") == 0) r.padB.down = true;
				else if (b.CompareNoCase("x") == 0) r.padX.down = true;
				else if (b.CompareNoCase("y") == 0) r.padY.down = true;
				else if (b.CompareNoCase("lb") == 0) l.shoulder.down = true;
				else if (b.CompareNoCase("rb") == 0) r.shoulder.down = true;
				else if (b.CompareNoCase("dup") == 0) l.dpadUp.down = true;
				else if (b.CompareNoCase("ddown") == 0) l.dpadDown.down = true;
				else if (b.CompareNoCase("dleft") == 0) l.dpadLeft.down = true;
				else if (b.CompareNoCase("dright") == 0) l.dpadRight.down = true;
				else if (b.CompareNoCase("view") == 0) l.view.down = true;
				else if (b.CompareNoCase("menu") == 0) r.padMenu.down = true;
				else if (b.CompareNoCase("lstick") == 0) l.thumbstickClick.down = true;
				else if (b.CompareNoCase("rstick") == 0) r.thumbstickClick.down = true;
			}
			--gFakeFrames;
		}
		else
		{
			// One frame at rest, so every press is released, then the real
			// controllers are back. A debug driver left on is found holding
			// somebody's trigger down.
			gSession->SetFakeHand(pcvrxr::Hand::Left, l);
			gSession->SetFakeHand(pcvrxr::Hand::Right, r);
			gFakeOn = false;
			return;
		}
		gSession->SetFakeHand(pcvrxr::Hand::Left, l);
		gSession->SetFakeHand(pcvrxr::Hand::Right, r);
	}
}

bool DoomVR_Active()
{
	return gUp && vr_mode == VR_HEADSET;
}

// By the time the renderer is torn down on quit, UZDoom has already shut its
// console, so anything Printed from here on vanishes - including the layer's
// own account of the shutdown. It goes to stderr, which a harness captures,
// and to log-doomvr-shutdown.txt in the working directory, each line stamped
// with the time: a run that ends by Task Manager then shows how far the exit
// got, which stderr alone could not show on a tester's machine.
static FILE* gShutdownFile = nullptr;

static void ShutdownNote(const char* line)
{
	fprintf(stderr, "%s\n", line);
	fflush(stderr);
	if (gShutdownFile == nullptr) gShutdownFile = fopen("log-doomvr-shutdown.txt", "w");
	if (gShutdownFile)
	{
		fprintf(gShutdownFile, "%10.1f ms  %s\n", NowMs(), line);
		fflush(gShutdownFile);
	}
}

void DoomVR_Shutdown()
{
	if (gSession == nullptr) return;

	gSession->SetLog([](const char* line) { ShutdownNote(line); });
	ShutdownNote("DoomVR: shutting the headset session down");

	if (gFrameBegun) gSession->EndFrame();
	gFrameBegun = false;
	ShutdownNote("DoomVR: last frame ended");
	gSession->Shutdown();
	delete gSession;
	gSession = nullptr;
	gUp = false;
	char line[96];
	snprintf(line, sizeof(line), "DoomVR: headset session closed after %d frames.", gFrames);
	ShutdownNote(line);
	if (gShutdownFile) { fclose(gShutdownFile); gShutdownFile = nullptr; }
	// And when the process itself is gone - the rest of the engine's exit
	// runs after this - so a slow close shows up next to the successor's wait.
	static bool exitNoted = false;
	if (!exitNoted)
	{
		exitNoted = true;
		atexit([] {
			if (FILE* f = fopen("log-doomvr-shutdown.txt", "a"))
			{
				fprintf(f, "%10.1f ms  DoomVR: the process is exiting\n", NowMs());
				fclose(f);
			}
		});
	}
}

void DoomVR_BeginFrame()
{
	// Nothing may stay held after the headset goes away.
	if (!DoomVR_Active() && gUp) ReleasePad();

	if (vr_mode != VR_HEADSET) return;
	if (!gTried) BringUp();
	if (!gUp || gFrameBegun) return;
	void DoomVR_SwitchGameTick();	// doomvr_switchgame.cpp: the picker, the game played last
	DoomVR_SwitchGameTick();
	extern double gSwitchStartMs;
	extern bool gSwitchRestarted;
	if (gSwitchStartMs > 0 && gSwitchRestarted)
	{
		gSwitchRestarted = false;
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: switched games in this process in %.1f s, the headset up throughout\n", (NowMs() - gSwitchStartMs) / 1000.0);
		gSwitchStartMs = 0;
	}

	// The synthetic yaw goes through the layer, so the eye poses turn with the
	// head and the stereo baseline turns with them.
	gSession->SetDebugYaw(vr_debug_yaw);
	gSession->SetDebugTilt(vr_debug_head_pitch, vr_debug_head_roll);

	const bool trace = Tracing();
	if (trace) Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR trace: frame %d BeginFrame...\n", gFrames);
	const double t0 = NowMs();
	DriveFakeHands();
	ReleaseDebugKey();
	gRender = gSession->BeginFrame();
	gFrameBegun = true;
	const double dt = NowMs() - t0;
	gWaitMs += dt;
	if (trace) Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR trace: frame %d BeginFrame -> render %d after %.2f ms\n", gFrames, (int)gRender, dt);

	// The layer has just read the controllers for this frame. (It has also
	// already turned these poses by vr_debug_yaw - adding it again here once
	// moved a test player twice as far round as the head.)
	double yaw, pitch, roll;
	const pcvrxr::Pose& head = gSession->Head();
	ToYawPitchRoll(head.orientation, yaw, pitch, roll);

	/*
		The recenter point is everything the player's height and facing are
		measured from, so it is only taken from a head that can be believed:
		tracked, AND the session visible to the player. VirtualDesktopXR
		flags its head valid at exactly (0, 0, 0) while the session is still
		READY; a reference taken there put a player a whole head height above
		the game's eyes, in the ceiling, and hung the menu on the floor.

		It is taken again when the runtime moves its space (its own recenter,
		or its floor settling - every pose after that is measured from
		somewhere else), at every level start (the standard: the player's eyes
		are at the game's eye height when play begins), and when asked.
	*/
	if (gSession->SpaceGeneration() != gSpaceGeneration)
	{
		gSpaceGeneration = gSession->SpaceGeneration();
		gRecenterAsked = true;
		gRecenterWhy = "the runtime moved its space";
	}
	const bool placeholder = head.position.x == 0 && head.position.y == 0 && head.position.z == 0
	                      && head.orientation.x == 0 && head.orientation.y == 0 && head.orientation.z == 0;
	const bool believable = head.tracked && gSession->IsVisible() && !placeholder;
	if (believable && (!gCentered || gRecenterAsked))
	{
		gOriginX = head.position.x + vr_debug_head_x;
		gOriginY = head.position.y + vr_debug_head_y;
		gOriginZ = head.position.z + vr_debug_head_z;
		gOriginYaw = yaw;
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: view %s at %.3f %.3f %.3f m, facing %.1f deg (%s)\n",
		       gCentered ? "recentered" : "centered", gOriginX, gOriginY, gOriginZ, yaw * kToDeg,
		       gCentered ? gRecenterWhy : "the first head the player sees");
		gCentered = true;
		gRecenterAsked = false;
		// A screen or HUD placed from the old reference is re-placed from
		// this one.
		gPanelMode = PANEL_NONE;
		// The body is where the head is now: nothing is left to carry.
		gCarried[0] = gCarried[1] = gCarriedPrev[0] = gCarriedPrev[1] = 0;
	}
	else if (!gCentered)
	{
		// No tracked head yet, so no recenter point: keep the view on the
		// head, or an absolute standing height (1.7 m, 70 units) would lift
		// the player into the ceiling until tracking starts.
		gOriginX = head.position.x + vr_debug_head_x;
		gOriginY = head.position.y + vr_debug_head_y;
		gOriginZ = head.position.z + vr_debug_head_z;
	}
	gMoveYaw = (yaw - gOriginYaw) * kToDeg;
	gLookYaw = gMoveYaw;
	{
		const pcvrxr::HandState& right = Hand(pcvrxr::Hand::Right);
		gAimValid = right.active;
		if (gAimValid)
		{
			double ay, ap, ar;
			ToYawPitchRoll(right.aim.orientation, ay, ap, ar);
			gAimYaw = (ay - gOriginYaw) * kToDeg;
			gAimPitch = -ap * kToDeg;
		}
	}
	{
		const pcvrxr::HandState& right = Hand(pcvrxr::Hand::Right);
		gGripValid = right.active;
		if (gGripValid)
		{
			gGrip = right.grip;
			gAimPose = right.aim;
		}
	}
	const pcvrxr::HandState& left = Hand(pcvrxr::Hand::Left);
	gLeftValid = left.active;
	if (gLeftValid) { gLeftAimPose = left.aim; gLeftGrip = left.grip; }
	// Once a run per hand: where this runtime puts the grip pose in the aim
	// pose's frame. Our hand hangs on the grip and the gun on the aim, so this
	// is what their meeting depends on (the hands project's
	// doom_held_preview.py --aim-to-grip takes these numbers). The runtime's own
	// numbers, before Hand() lays an unmeasured controller's aim along its grip.
	{
		static bool logged[2], laid[2];
		const pcvrxr::Hand which[2] = { pcvrxr::Hand::Right, pcvrxr::Hand::Left };
		const pcvrxr::HandState* hs[2] = { &gSession->GetHand(which[0]), &gSession->GetHand(which[1]) };
		for (int i = 0; i < 2; ++i)
		{
			if (!laid[i] && hs[i]->active && AimFromGrip(gSession->InteractionProfile(which[i])))
			{
				Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: %s on %s: its aim laid along its grip as Quest Touch reports it (vr_aim_from_grip)\n",
				       i == 0 ? "weapon hand" : "other hand", gSession->InteractionProfile(which[i]));
				laid[i] = true;
				logged[i] = false;	// and its own numbers again, now the profile is known
			}
			if (logged[i] || !hs[i]->active) continue;
			const pcvrxr::Quat& qa = hs[i]->aim.orientation;
			const pcvrxr::Quat qi{ -qa.x, -qa.y, -qa.z, qa.w };
			const pcvrxr::Vec3 p = Rotate(qi, hs[i]->grip.position.x - hs[i]->aim.position.x,
			                                  hs[i]->grip.position.y - hs[i]->aim.position.y,
			                                  hs[i]->grip.position.z - hs[i]->aim.position.z);
			double y, pt, r;
			ToYawPitchRoll(QuatMul(qi, hs[i]->grip.orientation), y, pt, r);
			Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: %s aim->grip: pitch %.1f yaw %.1f roll %.1f deg, x %.4f y %.4f z %.4f m (in the aim's frame)\n",
			       i == 0 ? "weapon hand" : "other hand", pt * kToDeg, y * kToDeg, r * kToDeg, p.x, p.y, p.z);
			logged[i] = true;
		}
	}
	if (vr_move_follows == 1 && left.active)
	{
		ToYawPitchRoll(left.aim.orientation, yaw, pitch, roll);
		gMoveYaw = (yaw - gOriginYaw) * kToDeg;
	}
	DoomVR_TwoHands();
	ReadControllers();
	DoomVR_UpdateSwings();
}

// Run while held, from any binding (the Steam Frame's layouts use it).
static void Cmd_VrRunDown(FCommandLine&, int) { gRunCmd = true; DPrintf(DMSG_NOTIFY, "DoomVR: +vr_run\n"); }
static void Cmd_VrRunUp(FCommandLine&, int) { gRunCmd = false; }
static FConsoleCommand VrRunDownRef("+vr_run", Cmd_VrRunDown);
static FConsoleCommand VrRunUpRef("-vr_run", Cmd_VrRunUp);

void DoomVR_AddAxes(float* axes)
{
	if (!DoomVR_Active()) return;
	// Walking speed: the left stick's share of Doom's running speed, unless
	// it is held in to run.
	const float walk = gRunHeld ? 1.f : std::clamp(float(vr_move_speed), 0.1f, 1.f);
	for (int i = 0; i < NUM_PAD_AXES; ++i)
	{
		const float v = gAxis[i] * (i == LX || i == LY ? walk : 1.f);
		const EAxisCodes code = v > 0 ? kAxisCodes[i][0] : v < 0 ? kAxisCodes[i][1] : AXIS_CODE_NULL;
		if (code != AXIS_CODE_NULL) axes[code] += fabsf(v);
	}
}

void DoomVR_RotateMove(int& forward, int& side)
{
	if (!DoomVR_Active() || (forward == 0 && side == 0)) return;
	// Head yaw is positive to the LEFT. A forward push with the head turned
	// 90 degrees left must move the player left: forward' = 0, side' = -1.
	const double t = gMoveYaw * kToRad, c = cos(t), s = sin(t);
	const double f = forward, sd = side;
	forward = int(lround(f * c + sd * s));
	side = int(lround(sd * c - f * s));
}

bool DoomVR_BeginPanel(int canvasWidth, int canvasHeight, int& width, int& height)
{
	if (!DoomVR_Active() || !gPanel || canvasWidth <= 0 || canvasHeight <= 0) return false;
	DoomVR_BeginFrame(); // a frame drawn without the engine's BeginFrame (a wipe)
	if (!DoomVR_ShouldRender()) return false;

	const bool level = gamestate == GS_LEVEL || gamestate == GS_TITLELEVEL;
	const bool screenShown = menuactive != MENU_Off || ConsoleState == c_down
	                      || ConsoleState == c_falling || !level;
	const int mode = screenShown ? PANEL_SCREEN : PANEL_HUD;

	double yaw, pitch, roll;
	ToYawPitchRoll(gSession->Head().orientation, yaw, pitch, roll);
	double off = yaw - gPanelYaw;
	while (off > 3.14159265358979) off -= 2 * 3.14159265358979;
	while (off < -3.14159265358979) off += 2 * 3.14159265358979;

	// A screen is placed when it appears and then stays put - one that follows
	// the head swims. The HUD follows lazily: only when the head has turned
	// well away from it, so it is there when you look for it - and it drops
	// to its lower place while the eyes glance down at the status bar.
	static bool panelBar = false;
	const bool bar = mode == PANEL_HUD && DoomVR_StatusBarShown();
	if (mode != gPanelMode || (mode == PANEL_HUD && (fabs(off) > 50.0 * kToRad || bar != panelBar)))
	{
		if (mode == PANEL_SCREEN) gSession->PlacePanelInFrontOfHead(vr_panel_distance);
		else
		{
			/*
				The HUD hangs below eye level, vr_hud_angle degrees down, and
				tilts back by the same angle so it faces the eyes: the head's
				yaw, then a turn of -angle about the panel's own X. Its front
				(+Z) then points up and back at the head.
			*/
			const double a = std::clamp(double(bar ? vr_hud_bar_angle : vr_hud_text_angle), -30.0, 60.0) * kToRad;
			const double d = std::clamp(double(vr_hud_distance), 0.5, 10.0);
			const pcvrxr::Pose& head = gSession->Head();
			pcvrxr::Pose p;
			p.position.x = float(head.position.x - sin(yaw) * cos(a) * d);
			p.position.y = float(head.position.y - sin(a) * d);
			p.position.z = float(head.position.z - cos(yaw) * cos(a) * d);
			const double sy = sin(yaw / 2), cy = cos(yaw / 2), sx = sin(-a / 2), cx = cos(-a / 2);
			p.orientation.x = float(cy * sx);
			p.orientation.y = float(sy * cx);
			p.orientation.z = float(-sy * sx);
			p.orientation.w = float(cy * cx);
			p.tracked = true;
			gSession->SetPanelPose(p);
		}
		gPanelYaw = yaw;
		gPanelMode = mode;
		panelBar = bar;
	}
	const float pw = mode == PANEL_HUD ? std::clamp(float(vr_hud_width), 0.3f, 6.f) : float(vr_panel_width);
	gSession->SetPanelShape(pw, pw * float(canvasHeight) / float(canvasWidth));

	// Not a level (title picture, intermission, text screens): there is no
	// world to show, and what is in the eyes is whatever was left there.
	if (!level) gSession->SuppressWorld();

	gSession->BeginPanel();
	gSession->PanelSize(width, height);
	return true;
}

void DoomVR_EndPanel()
{
	if (gPanel) gSession->EndPanel();
}

void DoomVR_SetDrawingEye(int eye)
{
	gDrawingEye = eye;
}

bool DoomVR_WeaponMatrix(float cx, float cy, float sx, float sy, float out[16], int handIndex, const DoomVR_WeaponTune* tune)
{
	if (!DoomVR_Active()) return false;
	const bool leftHand = handIndex == 1;
	static const DoomVR_WeaponTune kNoTune;
	const DoomVR_WeaponTune& t = tune ? *tune : kNoTune;

	/*
		Screen pixel -> sprite units -> metres on a card at the hand.

		u = (x - cx) / sx and v = (y - cy) / sy undo UZDoom's layout exactly
		(unit weapon scale: Doom's 1.2 stretch is in each weapon's own scale,
		so these come out in the proportions the flat game shows). At rest a
		weapon's bottom edge sits at v = +120; the grip is taken a little
		above that, at the centre, and put at the controller's grip.

		A weapon sprite is a painting of a gun seen from the eye, its barrel
		running up the picture into the distance. So the card faces the head
		(the same card for both eyes, or it would not be one object), and it
		is turned about its own normal so the picture's "up" is where the
		controller AIMS, as seen from the head: pointing ahead, the barrel
		runs up and away; pointing left, it runs left. The shot goes along the
		same aim ray. Only the grip's POSITION is used - its orientation is
		tilted along the controller's handle, which on Touch turned the
		picture round so the barrel pointed back at the player.
	*/
	const float k = vr_weapon_scale * t.scale;
	const float gripV = 84.f;
	float s2h[16] = {};
	// In the left hand (a VR mod's off-hand weapon drawn as a sprite) the
	// picture is mirrored, as a left-hand model is: the art is of a right hand.
	const float mx = (leftHand != Leftorium()) ? -1.f : 1.f;	// the PHYSICAL left hand
	s2h[0] = mx * k / sx;            // x metres per pixel
	s2h[5] = -k / sy;                // y: screen down is the card's down
	s2h[10] = 1;
	s2h[12] = -mx * cx * k / sx;     // centre the grip...
	s2h[13] = (cy / sy + gripV) * k; // ...and lift the sprite's grip point to it
	s2h[14] = -0.05f;                // a little beyond the hand, seen from the head
	s2h[15] = 1;

	// The hand and its aim, in the reference space - the controller, or a
	// fixed place below and in front of the head, aiming where it faces,
	// when there is no controller.
	const pcvrxr::Pose& head = gSession->Head();
	pcvrxr::Vec3 hand, aim;
	if (leftHand ? gLeftValid : gGripValid)
	{
		hand = leftHand ? gLeftGrip.position : gGrip.position;
		aim = Rotate(leftHand ? gLeftAimPose.orientation : gAimPose.orientation, 0, 0, -1);
	}
	else
	{
		const pcvrxr::Vec3 o = Rotate(head.orientation, (leftHand != Leftorium()) ? -0.15 : 0.15, -0.25, -0.35);
		hand.x = head.position.x + o.x; hand.y = head.position.y + o.y; hand.z = head.position.z + o.z;
		aim = Rotate(head.orientation, 0, 0, -1);
	}
	// This weapon's tuned offset, along the hand's own axes (mirrored in the
	// physical left hand).
	{
		const pcvrxr::Quat& frame = (leftHand ? gLeftValid : gGripValid) ? (leftHand ? gLeftAimPose.orientation : gAimPose.orientation) : head.orientation;
		const pcvrxr::Vec3 o = Rotate(frame, mx * t.right, t.up, -t.forward);
		hand.x += o.x; hand.y += o.y; hand.z += o.z;
	}

	// The card's normal: from the hand towards the head.
	double n[3] = { head.position.x - hand.x, head.position.y - hand.y, head.position.z - hand.z };
	double len = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (len < 1e-4) { n[0] = 0; n[1] = 0; n[2] = 1; len = 1; }
	for (double& c : n) c /= len;

	// The card's up: the aim, flattened onto the card. A little of the
	// world's up is added so that aiming straight along the line of sight -
	// where the aim flattens to nothing - still has an up, and the picture
	// does not spin.
	double up[3] = { aim.x, aim.y + 0.15, aim.z };
	const double along = up[0] * n[0] + up[1] * n[1] + up[2] * n[2];
	for (int i = 0; i < 3; ++i) up[i] -= along * n[i];
	len = sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
	if (len < 1e-4) { up[0] = 0; up[1] = 1; up[2] = 0; len = 1; }
	for (double& c : up) c /= len;

	// Right = up x normal, so right, up, normal is a right-handed frame.
	double rt[3] = { up[1] * n[2] - up[2] * n[1], up[2] * n[0] - up[0] * n[2], up[0] * n[1] - up[1] * n[0] };
	// The tuned roll: the card turned about its normal, clockwise as the
	// player sees it (mirrored in the physical left hand).
	if (t.roll != 0)
	{
		const double a = -mx * t.roll * kToRad, c = cos(a), sn = sin(a);
		double u2[3], r2[3];
		for (int i = 0; i < 3; ++i) { u2[i] = up[i] * c - rt[i] * sn; r2[i] = rt[i] * c + up[i] * sn; }
		for (int i = 0; i < 3; ++i) { up[i] = u2[i]; rt[i] = r2[i]; }
	}

	float h2r[16] = {
		float(rt[0]), float(rt[1]), float(rt[2]), 0,
		float(up[0]), float(up[1]), float(up[2]), 0,
		float(n[0]),  float(n[1]),  float(n[2]),  0,
		hand.x,       hand.y,       hand.z,       1,
	};

	float view[16], proj[16], m[16];
	pcvrxr::ViewMatrix(gSession->GetView(gDrawingEye).pose, view);
	// Near and far in metres: the weapon is drawn without depth testing, so
	// these only have to contain it.
	pcvrxr::ProjectionMatrix(gSession->GetView(gDrawingEye).fov, 0.05f, 100.f, proj);
	MatMul(h2r, s2h, m);
	MatMul(view, m, m);
	MatMul(proj, m, out);
	return true;
}

/*
	The weapon hand's recoil and punch, so a shot and a punch are felt in
	the hand. The playsim says when the weapon hand's weapon shoots or
	strikes (DoomVR_WeaponKick, from DoomVR_NoteLineAttack/NoteMissile); each
	frame turns the time since into a shape. A shot kicks the gun back and its
	muzzle up about the point it is gripped at, and the hand holding it goes
	with it - the same rigid move for both, so the grip stays in the fist. A
	fist's strike throws the hand forward along the aim and back.

	The grip point: every held gun puts its handle at one place in the aim
	pose's frame (the Voxel Doom table and the made guns alike, from a
	headset fit of the pistol) - 2.6 cm right of the model origin's line and
	18.6 cm below and 13.8 cm ahead of the model origin, moved as the weapon's
	tuned fit moves the gun.
*/
CVAR(Bool, vr_weapon_kick, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

/*
	Physical play, melee by swinging: a hand moving
	fast enough swings the weapon in it. Each hand's speed is the runtime's own
	(the aim space's velocity), or worked out from its positions when the
	runtime marks it invalid; a swing is the PEAK over the last 80 ms reaching
	vr_phys_swing_speed, once per 0.3 s. The playsim's ticcmd builder
	(g_game.cpp) turns a weapon hand's swing into the attack button for a few
	tics while a melee weapon is up, so the game's own attack does the hit -
	its trace, damage type, sounds and animation, berserk included. The button
	way stays. On by default (vr_phys_melee).

	A harder swing hits harder: the blow a
	swing starts does the game's damage at vr_phys_swing_speed, rising evenly
	to vr_phys_swing_power times it at 5 m/s and over (P_LineAttack asks
	DoomVR_SwingPower). The least swing is 2.5 m/s: reaching for
	holsters makes 2.0-2.3 m/s "swings".
*/
CVAR(Bool, vr_phys_melee, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_phys_swing_speed, 2.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// metres a second
CVAR(Float, vr_phys_swing_power, 2.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// damage times this at a full swing
constexpr double kFullSwing = 5.0;	// m/s
namespace
{
	struct SpeedSample { double t, v; };
	std::vector<SpeedSample> gSpeeds[2];
	pcvrxr::Vec3 gLastHandPos[2];
	double gLastHandAt[2] = { -1, -1 };
	double gSwingAt[2] = { -1e9, -1e9 };
	double gSwingPeak[2];
	int gSwingTics[2];
	double gDebugSwingSpeed = 0, gDebugSwingUntil = -1;
	int gSwingsLogged;

	void UpdateSwings()
	{
		const double now = NowMs();
		for (int i = 0; i < 2; ++i)
		{
			const pcvrxr::HandState& h = Hand(i == 0 ? pcvrxr::Hand::Right : pcvrxr::Hand::Left);
			if (!h.active) { gLastHandAt[i] = -1; gSpeeds[i].clear(); continue; }
			double v = 0;
			const pcvrxr::Vec3 p = h.aim.position;
			if (h.velocityValid)
				v = sqrt(h.linearVelocity.x * h.linearVelocity.x + h.linearVelocity.y * h.linearVelocity.y + h.linearVelocity.z * h.linearVelocity.z);
			else if (gLastHandAt[i] >= 0 && now > gLastHandAt[i])
			{
				const double dx = p.x - gLastHandPos[i].x, dy = p.y - gLastHandPos[i].y, dz = p.z - gLastHandPos[i].z;
				v = sqrt(dx * dx + dy * dy + dz * dz) / ((now - gLastHandAt[i]) / 1000.0);
			}
			gLastHandPos[i] = p;
			gLastHandAt[i] = now;
			if (i == 0 && now < gDebugSwingUntil) v = std::max(v, gDebugSwingSpeed);
			auto& s = gSpeeds[i];
			s.push_back({ now, v });
			while (!s.empty() && now - s.front().t > 80) s.erase(s.begin());
			double peak = 0;
			for (auto& x : s) peak = std::max(peak, x.v);
			if (peak >= vr_phys_swing_speed && now - gSwingAt[i] > 300)
			{
				gSwingAt[i] = now;
				gSwingPeak[i] = peak;
				gSwingTics[i] = 3;
				if (vr_phys_melee && gSwingsLogged < 20)
				{
					++gSwingsLogged;
					Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: swing, %s hand, %.1f m/s\n", i == 0 ? "weapon" : "other", peak);
				}
			}
		}
	}
}

void DoomVR_UpdateSwings() { UpdateSwings(); }

/*
	Physical play, two hands on a gun: with a long
	gun up, the other hand gripping within 25 cm of its front grip (the gun's
	own, else 30 cm ahead of where it is held and 3 cm up) takes hold of it,
	and the gun then aims from the back hand towards the front one, keeping
	the back hand's roll: steadier, as a real long gun. Shots, the red dot and
	the drawn gun all follow, because they all read the aim. Letting go is
	one-handed again. With such a gun up, that grip HELD never reaches its
	binding (weapprev), caught or not; only a tap under 0.25 s does. On by default.
*/
CVAR(Bool, vr_phys_twohand, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Two-handed aim: Basic (from hand to hand, above) or Gun stock - the gun's
// back rests near the face, 6 cm below and 4 cm ahead of the gun-side eye,
// and it aims from there through the gun hand, as along a rifle's stock;
// only while the gun hand is within 35 cm of that point, else Basic. A style,
// not a part: Physical Play's presets leave it as it is.
CVAR(Bool, vr_phys_twohand_stock, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
namespace
{
	bool gHeldLongGun = false, gTwoHanded = false, gTwoHandGripWas = false;
	// How near the gun's front grip the other hand must close to take hold
	// (m): 15 cm missed most squeezes (30-47 cm off).
	constexpr double kTwoHandCatch = 0.25;
	DoomVR_WeaponTune gHeldTune;
	bool gHeldHasSupport = false;
	DoomVR_Support gHeldSupport;
	bool gDebugTwoHand = false;	// vr_debug_twohand: two hands, the other at the support point
	int gTwoHandLogged = 0;

	pcvrxr::Quat QuatFromBasis(const pcvrxr::Vec3& x, const pcvrxr::Vec3& y, const pcvrxr::Vec3& z)
	{
		const double m00 = x.x, m11 = y.y, m22 = z.z, tr = m00 + m11 + m22;
		pcvrxr::Quat q;
		if (tr > 0) { const double s = sqrt(tr + 1.0) * 2; q = { float((y.z - z.y) / s), float((z.x - x.z) / s), float((x.y - y.x) / s), float(0.25 * s) }; }
		else if (m00 > m11 && m00 > m22) { const double s = sqrt(1.0 + m00 - m11 - m22) * 2; q = { float(0.25 * s), float((y.x + x.y) / s), float((z.x + x.z) / s), float((y.z - z.y) / s) }; }
		else if (m11 > m22) { const double s = sqrt(1.0 + m11 - m00 - m22) * 2; q = { float((y.x + x.y) / s), float(0.25 * s), float((z.y + y.z) / s), float((z.x - x.z) / s) }; }
		else { const double s = sqrt(1.0 + m22 - m00 - m11) * 2; q = { float((z.x + x.z) / s), float((z.y + y.z) / s), float(0.25 * s), float((x.y - y.x) / s) }; }
		return q;
	}
}

// Physical play's preset (VR Options > Physical Play): 1 every part on, 0
// every part back to the buttons. A command, not a saved setting, so the
// player's own choice of parts is never overwritten when settings load.
EXTERN_CVAR(Bool, vr_phys_wrist)
CCMD(vr_physical)
{
	if (argv.argc() < 2) { Printf("vr_physical 1 (everything physical) or 0 (the buttons only)\n"); return; }
	const bool on = atoi(argv[1]) != 0;
	vr_phys_melee = on;
	vr_phys_holsters = on;
	vr_phys_twohand = on;
	vr_phys_use = on;
	vr_phys_wrist = on;
	Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: physical play %s\n", on ? "on (every part)" : "off (the buttons only)");
}

void DoomVR_NoteHeldGun(bool longGun, const DoomVR_WeaponTune& tune, const DoomVR_Support* support)
{
	gHeldLongGun = longGun;
	gHeldTune = tune;
	gHeldHasSupport = support != nullptr;
	if (support) gHeldSupport = *support;
}

bool DoomVR_TwoHandedNow()
{
	return gTwoHanded;
}

// A gun the other hand can take hold of is up (two hands on, a long gun).
bool DoomVR_TwoHandGunHeld()
{
	return vr_phys_twohand && gHeldLongGun;
}

// Desk test: hold the gun in two hands with the other hand exactly on its
// support point, so the rebuilt aim must equal the one-hand aim (logged).
CCMD(vr_debug_twohand)
{
	gDebugTwoHand = argv.argc() > 1 && atoi(argv[1]) != 0;
	Printf("vr_debug_twohand: %s\n", gDebugTwoHand ? "on" : "off");
}

namespace { pcvrxr::Vec3 GunGrip(bool physicalLeft, const DoomVR_WeaponTune& t); }	// with the recoil, below

void DoomVR_TwoHands()
{
	const pcvrxr::HandState& other = Hand(pcvrxr::Hand::Left);
	const bool gripDown = other.active && other.squeezeClick.down;
	const bool pressed = gripDown && !gTwoHandGripWas;
	gTwoHandGripWas = gripDown;
	const bool wasOn = gTwoHanded;
	const bool can = vr_phys_twohand && gHeldLongGun && gGripValid && gLeftValid && gamestate == GS_LEVEL && menuactive == MENU_Off;
	if (!can || !gripDown) gTwoHanded = false;
	gTwoHandGrip = gTwoHanded || (wasOn && !gTwoHanded && !gripDown);
	if (!can) return;

	const pcvrxr::Vec3 g = GunGrip(Leftorium(), gHeldTune);
	const pcvrxr::Quat qa = gAimPose.orientation;
	const pcvrxr::Vec3 gw0 = Rotate(qa, g.x, g.y, g.z);
	const pcvrxr::Vec3 back = { gAimPose.position.x + gw0.x, gAimPose.position.y + gw0.y, gAimPose.position.z + gw0.z };
	// The front grip: the gun's own (B guns, the hands project's FRONT-HAND
	// table), else 30 cm ahead of the hand. Mirrored in the Leftorium.
	const double smir = Leftorium() ? -1.0 : 1.0;
	const pcvrxr::Vec3 sp = gHeldHasSupport ? pcvrxr::Vec3{ float(smir * gHeldSupport.point[0]), gHeldSupport.point[1], gHeldSupport.point[2] }
	                                        : pcvrxr::Vec3{ 0.f, 0.03f, -0.30f };
	pcvrxr::Vec3 palm = other.grip.position;
	if ((gDebugTwoHand || DoomVR_TuneSupportShown()) && can)
	{
		const pcvrxr::Vec3 fs = Rotate(qa, g.x + sp.x, g.y + sp.y, g.z + sp.z);
		palm = { gAimPose.position.x + fs.x, gAimPose.position.y + fs.y, gAimPose.position.z + fs.z };
		if (!gTwoHanded) { gTwoHanded = true; gTwoHandGrip = true; }
	}
	if (pressed)
	{
		const pcvrxr::Vec3 fw = Rotate(qa, g.x + sp.x, g.y + sp.y, g.z + sp.z);
		const double ex = gAimPose.position.x + fw.x - palm.x, ey = gAimPose.position.y + fw.y - palm.y, ez = gAimPose.position.z + fw.z - palm.z;
		if (gTwoHandLogged < 20) Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: two-hand check: the other hand is %.3f m from the gun's front\n", sqrt(ex * ex + ey * ey + ez * ez));
		if (sqrt(ex * ex + ey * ey + ez * ez) < kTwoHandCatch)
		{
			gTwoHanded = true;
			gTwoHandGrip = true;
			DoomVR_Pulse(1, 0.04f, 0.5f, "two hands on the gun");
		}
	}
	if (!gTwoHanded) return;

	// Forward from the back hand to the front one; up from the back hand's own.
	double f[3] = { palm.x - back.x, palm.y - back.y, palm.z - back.z };
	double fl = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
	if (fl < 0.05) return;	// hands together: keep the one-hand aim
	for (double& c : f) c /= fl;
	// Gun stock: from the face through the gun hand, the barrel itself on that line.
	bool stock = false;
	if (vr_phys_twohand_stock)
	{
		const pcvrxr::Pose& head = gSession->Head();
		const pcvrxr::Vec3 ao = Rotate(head.orientation, smir * 0.032, -0.06, -0.04);
		const double sx = back.x - (head.position.x + ao.x), sy = back.y - (head.position.y + ao.y), sz = back.z - (head.position.z + ao.z);
		const double sl = sqrt(sx * sx + sy * sy + sz * sz);
		if (sl > 0.08 && sl < 0.35)
		{
			f[0] = sx / sl; f[1] = sy / sl; f[2] = sz / sl;
			stock = true;
		}
	}
	/*
		The front grip is not on the barrel's line (a carry bar below, a
		handle high above): the gun turns so the line from its grip to the
		front grip, not its barrel, runs from hand to hand. R takes the barrel
		(-z) to that line, and the hand-to-hand frame is built with the up of
		THAT frame (qa R), so with the hand exactly on the front grip the aim is
		unchanged (the chainsaw's high handle turned it 1.1 deg when the
		one-hand up was used); the aim is then q R^-1.
	*/
	pcvrxr::Quat r{ 0, 0, 0, 1 };
	if (!stock)
	{
		double d[3] = { sp.x, sp.y, sp.z };
		const double dl = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (dl > 1e-4)
		{
			for (double& c : d) c /= dl;
			const double rx = d[1], ry = -d[0], rz = 0, rw = 1.0 - d[2];	// (0,0,-1) x d, 1 + (0,0,-1).d
			const double rl = sqrt(rx * rx + ry * ry + rz * rz + rw * rw);
			if (rl > 1e-6) r = { float(rx / rl), float(ry / rl), float(rz / rl), float(rw / rl) };
		}
	}
	const pcvrxr::Vec3 upLocal = Rotate(r, 0, 1, 0);
	const pcvrxr::Vec3 upHint = Rotate(qa, upLocal.x, upLocal.y, upLocal.z);
	const pcvrxr::Vec3 z{ float(-f[0]), float(-f[1]), float(-f[2]) };	// the aim's +z points back
	pcvrxr::Vec3 x{ upHint.y * z.z - upHint.z * z.y, upHint.z * z.x - upHint.x * z.z, upHint.x * z.y - upHint.y * z.x };
	const double xl = sqrt(x.x * x.x + x.y * x.y + x.z * x.z);
	if (xl < 1e-4) return;
	x = { float(x.x / xl), float(x.y / xl), float(x.z / xl) };
	const pcvrxr::Vec3 y{ z.y * x.z - z.z * x.y, z.z * x.x - z.x * x.z, z.x * x.y - z.y * x.x };
	pcvrxr::Quat q = QuatFromBasis(x, y, z);
	q = QuatMul(q, pcvrxr::Quat{ -r.x, -r.y, -r.z, r.w });
	// Keep the back hand where it is: the gun turns about its grip.
	const pcvrxr::Vec3 gw1 = Rotate(q, g.x, g.y, g.z);
	gAimPose.orientation = q;
	gAimPose.position = { back.x - gw1.x, back.y - gw1.y, back.z - gw1.z };
	double ay, ap, ar;
	ToYawPitchRoll(q, ay, ap, ar);
	gAimYaw = (ay - gOriginYaw) * kToDeg;
	gAimPitch = -ap * kToDeg;
	if (!wasOn && gTwoHandLogged < 20)
	{
		++gTwoHandLogged;
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: two hands on the gun, aim yaw %+.1f pitch %+.1f deg\n", gAimYaw, gAimPitch);
		if (gDebugTwoHand)
		{
			// The other hand sits exactly on the support point: the rebuilt aim
			// must be the one-hand aim, give or take the up hint's roll.
			const double dot = fabs(double(q.x) * qa.x + double(q.y) * qa.y + double(q.z) * qa.z + double(q.w) * qa.w);
			Printf("vr_debug_twohand: rebuilt aim differs from the one-hand aim by %.2f deg\n", 2.0 * acos(std::min(1.0, dot)) * kToDeg);
		}
	}
}

bool DoomVR_SwingAttack(int hand)
{
	if (!vr_phys_melee || hand < 0 || hand > 1 || gSwingTics[hand] <= 0) return false;
	--gSwingTics[hand];
	return true;
}

double DoomVR_SwingPower(double& speed)
{
	speed = 0;
	// A blow lands a few tics after the swing presses attack (the fist's
	// A_Punch is its second frame): 0.4 s covers it, and no later trigger punch.
	if (!vr_phys_melee || NowMs() - gSwingAt[0] > 400) return 1.0;
	speed = gSwingPeak[0];
	const double lo = vr_phys_swing_speed;
	const double f = kFullSwing > lo ? std::clamp((speed - lo) / (kFullSwing - lo), 0.0, 1.0) : 1.0;
	return 1.0 + (std::max(1.0, double(vr_phys_swing_power)) - 1.0) * f;
}

// Desk test: the weapon hand moving at <m/s> for 80 ms (the test hands cannot move).
CCMD(vr_debug_swing)
{
	if (argv.argc() < 2) { Printf("vr_debug_swing <m/s>\n"); return; }
	gDebugSwingSpeed = atof(argv[1]);
	gDebugSwingUntil = NowMs() + 80;
	Printf("vr_debug_swing: weapon hand at %.1f m/s\n", gDebugSwingSpeed);
}
namespace
{
	double gKickAt = -1e9;
	bool gKickMelee = false;
	bool gKickThrust = false;

	// 0 at rest, 1 at the peak: up in riseMs, back down over fallMs.
	double KickShape(double riseMs, double fallMs)
	{
		const double t = NowMs() - gKickAt;
		if (t < 0 || t > riseMs + fallMs) return 0;
		if (t < riseMs) return t / riseMs;
		const double f = 1.0 - (t - riseMs) / fallMs;
		return f * f;
	}

	pcvrxr::Vec3 GunGrip(bool physicalLeft, const DoomVR_WeaponTune& t)
	{
		const double mir = physicalLeft ? -1.0 : 1.0;
		return { float(vr_model_x + mir * (0.026 + t.right)), float(vr_model_y + t.up - 0.186), float(vr_model_z - t.forward - 0.138) };
	}

	// The aim pose moved by the recoil, about the grip point g (aim frame).
	pcvrxr::Pose KickedAim(const pcvrxr::Pose& aim, const pcvrxr::Vec3& g)
	{
		if (!vr_weapon_kick) return aim;
		if (gKickMelee)
		{
			// A staff's thrust: out along the aim like a punch,
			// gun and hand together. A blow a swing started moves as the hand did.
			if (!gKickThrust || NowMs() - gSwingAt[0] <= 500) return aim;
			const double a = KickShape(70, 180);
			if (a <= 0) return aim;
			const pcvrxr::Vec3 f = Rotate(aim.orientation, 0, 0, -0.16 * a);
			pcvrxr::Pose k = aim;
			k.position = { aim.position.x + f.x, aim.position.y + f.y, aim.position.z + f.z };
			return k;
		}
		const double a = KickShape(25, 160);
		if (a <= 0) return aim;
		const double back = 0.035 * a, pitch = 7.0 * a * kToRad;
		const double c = cos(pitch), sn = sin(pitch);
		// R_k: a turn about the aim's right axis, muzzle (-z) up.
		const pcvrxr::Quat rk{ float(sin(pitch / 2)), 0, 0, float(cos(pitch / 2)) };
		const pcvrxr::Vec3 rg{ g.x, float(g.y * c - g.z * sn), float(g.y * sn + g.z * c) };
		const pcvrxr::Vec3 t = Rotate(aim.orientation, g.x - rg.x, g.y - rg.y, g.z - rg.z + back);
		pcvrxr::Pose k = aim;
		k.position = { aim.position.x + t.x, aim.position.y + t.y, aim.position.z + t.z };
		k.orientation = QuatMul(aim.orientation, rk);
		return k;
	}
}

void DoomVR_WeaponKick(bool melee, bool thrust)
{
	const double now = NowMs();
	// A shotgun's pellets are separate attacks in one tic: one kick.
	if (now - gKickAt < 40 && melee == gKickMelee) return;
	gKickAt = now;
	gKickMelee = melee;
	gKickThrust = melee && thrust;
}

/*
	A 3D weapon model (MODELDEF) held in the hand.

	The flat game draws one in the eye's own space - UZDoom's RenderHUDModel
	starts from the view-to-world matrix, so the model's origin is the eye and
	its forward is the view's. In a headset that origin becomes the right
	controller's grip, as seen from the eye being drawn: the grip's position
	relative to the eye in map units, and its orientation relative to the
	eye's. (Named grip below; it is the controller's aim pose, see there.) OpenXR's eye space and the renderer's view space share their axes
	(x right, y up, -z ahead), so this is one rigid transform. The model's own
	scale, offsets and angle offsets then apply exactly as its MODELDEF says.

	Written for Doom VR from UZDoom's RenderHUDModel and
	pcvr-xr's poses; the placement was then measured against the benchmark
	(tools\model-ab.ps1) rather than taken from anyone's code.
*/
bool DoomVR_HandModelMatrix(float out[16])
{
	if (!DoomVR_Active()) return false;
	const pcvrxr::Pose& eyePose = gSession->GetView(gDrawingEye).pose;
	const bool leftHand = gDrawingHand == 1;
	pcvrxr::Pose grip;
	// The AIM pose, not the grip: measured, the convention the mods were made
	// for places the model from where the controller points (both its origin
	// and its direction) - on a real controller the two differ by 30-40
	// degrees, and a gun on the grip points at the floor.
	if (leftHand ? gLeftValid : gGripValid)
	{
		grip = leftHand ? gLeftAimPose : gAimPose;
		if (!leftHand && !gWheelModelOn && !gPlacedModelOn) grip = KickedAim(grip, GunGrip(Leftorium(), gDrawingTune));
	}
	else
	{
		// No controller: below and in front of the head, facing where it
		// faces - to the right for the weapon hand, the left for the other.
		const pcvrxr::Pose& head = gSession->Head();
		const pcvrxr::Vec3 o = Rotate(head.orientation, (leftHand != Leftorium()) ? -0.15 : 0.15, -0.25, -0.35);
		grip.position = { head.position.x + o.x, head.position.y + o.y, head.position.z + o.z };
		grip.orientation = head.orientation;
	}
	const pcvrxr::Quat& qe = eyePose.orientation;
	const pcvrxr::Quat qi{ -qe.x, -qe.y, -qe.z, qe.w };
	// This weapon's own fit (the tuner), mirrored in the physical left hand
	// as the weapon is: sideways, turn and roll change sign.
	const DoomVR_WeaponTune& t = gDrawingTune;
	const float mir = (leftHand != Leftorium()) ? -1.f : 1.f;
	// On the weapon wheel: side on, barrel along the ring's right, the
	// model's origin on its spot of the ring (DoomVR_RenderWheelModel then
	// centres the model there and sizes it).
	if (gPlacedModelOn)
	{
		// In a holster on the body: the model's middle on its place
		// (DoomVR_RenderWheelModel centres it there, at its own size).
		grip.orientation = gPlacedQ;
		const pcvrxr::Vec3 o = Rotate(grip.orientation, vr_model_x + mir * t.right, vr_model_y + t.up, vr_model_z - t.forward);
		grip.position = { gPlacedAt.x - o.x, gPlacedAt.y - o.y, gPlacedAt.z - o.z };
	}
	else if (gWheelModelOn)
	{
		const double h = gWheelYaw * 0.5;
		grip.orientation = { 0.f, float(sin(h)), 0.f, float(cos(h)) };
		const pcvrxr::Vec3 o = Rotate(grip.orientation, vr_model_x + mir * t.right, vr_model_y + t.up, vr_model_z - t.forward);
		const pcvrxr::Vec3 spot{ gWheelCentre.x + gWheelAcross.x * gWheelModelX, gWheelCentre.y + gWheelModelY, gWheelCentre.z + gWheelAcross.z * gWheelModelX };
		grip.position = { spot.x - o.x, spot.y - o.y, spot.z - o.z };
	}
	// The grip, and the offset along its own axes, relative to the eye.
	const pcvrxr::Vec3 off = Rotate(grip.orientation, vr_model_x + mir * t.right, vr_model_y + t.up, vr_model_z - t.forward);
	const pcvrxr::Vec3 d = Rotate(qi, grip.position.x + off.x - eyePose.position.x,
	                                  grip.position.y + off.y - eyePose.position.y,
	                                  grip.position.z + off.z - eyePose.position.z);
	// The tilt, about the grip's own right axis, then the tuned turn (about
	// its up axis; barrel right is negative) and roll (about its forward,
	// -Z; clockwise is positive).
	auto axisQuat = [](double x, double y, double z, double deg) {
		const double h = deg * 0.5 * kToRad, sn = sin(h);
		return pcvrxr::Quat{ float(x * sn), float(y * sn), float(z * sn), float(cos(h)) };
	};
	const pcvrxr::Quat tilt = QuatMul(QuatMul(axisQuat(1, 0, 0, vr_model_pitch + t.pitch),
	                                          axisQuat(0, 1, 0, -mir * t.yaw)),
	                                  axisQuat(0, 0, -1, mir * t.roll));
	const pcvrxr::Quat q = QuatMul(QuatMul(qi, grip.orientation), tilt);
	const double u = vr_hunits_per_meter;
	const float s = vr_model_scale * t.scale;
	// Column-major, as VSMatrix and OpenGL keep it.
	const pcvrxr::Vec3 cx = Rotate(q, 1, 0, 0), cy = Rotate(q, 0, 1, 0), cz = Rotate(q, 0, 0, 1);
	const float m[16] = {
		float(cx.x * s), float(cx.y * s), float(cx.z * s), 0,
		float(cy.x * s), float(cy.y * s), float(cy.z * s), 0,
		float(cz.x * s), float(cz.y * s), float(cz.z * s), 0,
		float(d.x * u),  float(d.y * u),  float(d.z * u),  1,
	};
	memcpy(out, m, sizeof(m));
	return true;
}

// A point of the weapon hand's model (its own space, through its model chain)
// where DoomVR_HandModelMatrix puts it at rest - no recoil - in the controller's
// aim frame, metres: x right, y up, z back. For the gun's muzzle (models.cpp).
// False for anything but the gun in the weapon hand.
bool DoomVR_HandModelToAim(const float p[3], float out[3])
{
	if (!DoomVR_Active() || gWheelModelOn || gPlacedModelOn || gDrawingHand == 1) return false;
	const DoomVR_WeaponTune& t = gDrawingTune;
	const float mir = Leftorium() ? -1.f : 1.f;
	auto axisQuat = [](double x, double y, double z, double deg) {
		const double h = deg * 0.5 * kToRad, sn = sin(h);
		return pcvrxr::Quat{ float(x * sn), float(y * sn), float(z * sn), float(cos(h)) };
	};
	const pcvrxr::Quat tilt = QuatMul(QuatMul(axisQuat(1, 0, 0, vr_model_pitch + t.pitch),
	                                          axisQuat(0, 1, 0, -mir * t.yaw)),
	                                  axisQuat(0, 0, -1, mir * t.roll));
	const double k = vr_model_scale * t.scale / vr_hunits_per_meter;
	const pcvrxr::Vec3 r = Rotate(tilt, p[0] * k, p[1] * k, p[2] * k);
	out[0] = float(vr_model_x + mir * t.right + r.x);
	out[1] = float(vr_model_y + t.up + r.y);
	out[2] = float(vr_model_z - t.forward + r.z);
	return true;
}

// The weapon wheel, for doomvr_hands.cpp: whether it is up, how many and which is lit.
bool DoomVR_WheelState(int& count, int& highlight)
{
	count = gWheelCount;
	highlight = gWheelHighlight;
	return gWheelOpen && DoomVR_Active();
}

// Slice i of n: its middle's angle, degrees anticlockwise from the ring's
// right - the first at the top, the rest clockwise.
double DoomVR_WheelAngle(int i, int n)
{
	return 90.0 - 360.0 * i / std::max(1, n);
}

// The ring's own space (metres: x its right, y up, z towards the player,
// from where it opened) into the eye being drawn (map units).
bool DoomVR_WheelFrame(float out[16])
{
	if (!gWheelOpen || !DoomVR_Active()) return false;
	const pcvrxr::Pose& eyePose = gSession->GetView(gDrawingEye).pose;
	const pcvrxr::Quat& qe = eyePose.orientation;
	const pcvrxr::Quat qi{ -qe.x, -qe.y, -qe.z, qe.w };
	const float u = float(vr_hunits_per_meter);
	const pcvrxr::Vec3 cx = Rotate(qi, gWheelAcross.x, gWheelAcross.y, gWheelAcross.z), cy = Rotate(qi, 0, 1, 0),
	                   cz = Rotate(qi, gWheelToward.x, gWheelToward.y, gWheelToward.z);
	const pcvrxr::Vec3 d = Rotate(qi, gWheelCentre.x - eyePose.position.x, gWheelCentre.y - eyePose.position.y, gWheelCentre.z - eyePose.position.z);
	const float m[16] = {
		cx.x * u, cx.y * u, cx.z * u, 0,
		cy.x * u, cy.y * u, cy.z * u, 0,
		cz.x * u, cz.y * u, cz.z * u, 0,
		d.x * u,  d.y * u,  d.z * u,  1,
	};
	memcpy(out, m, sizeof(m));
	return true;
}

// The next model drawn goes on the ring at (x, y) metres, k metres long; off puts it back in the hand.
float DoomVR_WheelModelLength() { return gPlacedModelOn ? 0.f : gWheelModelK; }	// 0: its own size
void DoomVR_SetWheelModel(bool on, float x, float y, float k)
{
	gWheelModelOn = on;
	gWheelModelX = x;
	gWheelModelY = y;
	gWheelModelK = k;
}

/*
	A hand, for the VR mods' AttackPos/Angle/Pitch/Roll and Offhand* (filled
	in by P_PlayerThink): where the controller's aim is from the recenter
	point, in metres along the body's right, up and forward - the terms the
	eyes are placed in, so a hand lands where it is drawn - and which way it
	points, relative to the recenter yaw.

	The direction carries the weapon tilt the VR mods were made for: their
	numbers are the aim turned 30 degrees down about its own right axis
	(measured with tools\hand-probe.py against the benchmark: a level hand
	reads pitch -30, one raised 30 reads 0, one rolled 30 reads yaw -16.10,
	pitch -25.66, roll -33.69 - exactly aim x Rx(-30)).
*/
CVAR(Float, vr_weapon_tilt, -30.f, 0)

// A point fixed to a hand's controller - metres in its aim frame (x right,
// y up, z back), with the controller's whole turn, roll too - in the terms
// DoomVR_HandPose gives the hand's place. The held gun's muzzle (p_user.cpp):
// laid along the hand's yaw and pitch alone, through the mods' 30-degree tilt,
// it started Brutal Doom's rifle tracers under the barrel and, with any roll
// of the wrist, to its side.
bool DoomVR_HandPointPose(int hand, const double p[3], double& right, double& up, double& forward)
{
	if (!DoomVR_Active() || !gCentered) return false;
	const pcvrxr::HandState& h = Hand(hand == 0 ? pcvrxr::Hand::Right : pcvrxr::Hand::Left);
	if (!h.active) return false;
	const pcvrxr::Vec3 o = Rotate(h.aim.orientation, p[0], p[1], p[2]);
	FromOrigin({ h.aim.position.x + o.x, h.aim.position.y + o.y, h.aim.position.z + o.z }, right, up, forward);
	right -= gCarried[0];
	forward -= gCarried[1];
	return true;
}

bool DoomVR_HandPose(int hand, double& right, double& up, double& forward, double& yawDeg, double& pitchDeg, double& rollDeg)
{
	if (!DoomVR_Active() || !gCentered) return false;
	const pcvrxr::HandState& h = Hand(hand == 0 ? pcvrxr::Hand::Right : pcvrxr::Hand::Left);
	if (!h.active) return false;
	FromOrigin(h.aim.position, right, up, forward);
	// The body has carried part of the walk (room-scale): the hand is that
	// much nearer the pawn.
	right -= gCarried[0];
	forward -= gCarried[1];
	const double half = vr_weapon_tilt * 0.5 * kToRad;
	const pcvrxr::Quat tilt{ float(sin(half)), 0, 0, float(cos(half)) };
	double y, p, r;
	ToYawPitchRoll(QuatMul(h.aim.orientation, tilt), y, p, r);
	yawDeg = (y - gOriginYaw) * kToDeg;
	pitchDeg = p * kToDeg;
	rollDeg = -r * kToDeg;
	return true;
}

/*
	Game Or Die Hands: our hand at the controller's PALM - the OpenXR grip
	pose, not the aim, times the hand's attach: the palm's centre on the grip
	origin, pitched 45 degrees down the way runtimes report the grip (the
	fit worn and kept in the hands viewer; gdh::HandAttach) - for a free hand
	and a fist. A hand holding a gun goes on the gun's handle instead and
	turns with the gun (mode 1, below).

	The hand's space is OpenXR's (x right, y up, -z ahead, metres), as the
	renderer's view space is, so this is one rigid transform and the scale
	to map units. The tuning cvars are along the grip's own axes, mirrored in
	the physical left hand.

	Written for Doom VR from the hands project's
	docs/PORTING.md and pcvr-xr's poses.
*/
CVAR(Float, vr_hands_x, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)		// metres, right
CVAR(Float, vr_hands_y, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)		// metres, up
CVAR(Float, vr_hands_z, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)		// metres, back
CVAR(Float, vr_hands_pitch, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// degrees, on top of the attach's -45
CVAR(Float, vr_hands_size, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

/*
	Holster markers: holster i (0 the hip, 1 the shoulder) as a square facing
	the eye being drawn (view space, map units, 1 = a metre before the scale),
	for its ring and label; with the gun hand's distance from it (m, 0 inside
	its zone) and whether a grip now would draw. False when holsters cannot be
	used this frame (off, a menu, the console, a pause, no hand).
*/
bool DoomVR_HolsterMarker(int i, float out[16], double& handDist, bool& ready)
{
	if (!DoomVR_Active() || !gHolstersValid || i < 0 || i >= kHolsterCount) return false;
	handDist = gHolsterDist[i];
	ready = i == gHolsterInside && NowMs() - gHolsterEnteredMs <= kHolsterReachMs;
	const HolsterZone& z = kHolsterZones[i];
	const double lift = std::clamp(double(vr_phys_holster_lift), 0.0, 0.20);
	const pcvrxr::Vec3 p = BodyPoint(z.spotOut, z.spotDown - lift, z.spotBehind);
	const pcvrxr::Pose& eyePose = gSession->GetView(gDrawingEye).pose;
	const pcvrxr::Quat& qe = eyePose.orientation;
	const pcvrxr::Quat qi{ -qe.x, -qe.y, -qe.z, qe.w };
	const pcvrxr::Vec3 d = Rotate(qi, p.x - eyePose.position.x, p.y - eyePose.position.y, p.z - eyePose.position.z);
	const float u = float(vr_hunits_per_meter);
	const float m[16] = {
		u, 0, 0, 0,
		0, u, 0, 0,
		0, 0, u, 0,
		float(d.x * u), float(d.y * u), float(d.z * u), 1,
	};
	memcpy(out, m, sizeof(m));
	return true;
}

/*
	A holstered gun drawn on the body: the next model drawn goes there
	(DoomVR_HandModelMatrix, at its own size). The hip gun hangs at the belt
	line barrel down, its top to the front; the long gun is slung on the back,
	barrel down, across it a little, its stock at the top of the shoulder.
	False (and nothing set) when holsters cannot be used this frame; -1 puts
	the model back in the hand.
*/
bool DoomVR_SetHolsterModel(int i)
{
	if (i < 0 || !DoomVR_Active() || !gHolstersValid || i >= kHolsterCount) { gPlacedModelOn = false; return false; }
	const double lift = std::clamp(double(vr_phys_holster_lift), 0.0, 0.20);
	const double side = Leftorium() ? -1.0 : 1.0;
	auto axisQ = [](double x, double y, double z, double deg) {
		const double h = deg * 0.5 * kToRad, sn = sin(h);
		return pcvrxr::Quat{ float(x * sn), float(y * sn), float(z * sn), float(cos(h)) };
	};
	const pcvrxr::Quat body = axisQ(0, 1, 0, gBodyYaw / kToRad);
	if (i == 0)
	{
		gPlacedAt = BodyPoint(0.23, 0.62 - lift, 0.02);
		gPlacedQ = QuatMul(body, axisQ(1, 0, 0, -90));
	}
	else
	{
		gPlacedAt = BodyPoint(0.10, 0.32 - lift, 0.17);
		gPlacedQ = QuatMul(QuatMul(QuatMul(body, axisQ(0, 0, 1, side * 20)), axisQ(1, 0, 0, -90)), axisQ(0, 0, 1, 180));
	}
	gPlacedModelOn = true;
	return true;
}

// Checks the holsters against the grab test: each ring's place in the room
// and the gun hand's distance from it.
CCMD(vr_debug_holsters)
{
	if (!gHolstersValid) { Printf("vr_debug_holsters: holsters not in use this frame\n"); return; }
	const pcvrxr::HandState& r = Hand(Leftorium() ? pcvrxr::Hand::Left : pcvrxr::Hand::Right);
	const double lift = std::clamp(double(vr_phys_holster_lift), 0.0, 0.20);
	for (int i = 0; i < kHolsterCount; ++i)
	{
		const HolsterZone& z = kHolsterZones[i];
		const pcvrxr::Vec3 p = BodyPoint(z.spotOut, z.spotDown - lift, z.spotBehind);
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: %-8s holster ring at %.3f %.3f %.3f, gun hand at %.3f %.3f %.3f, %s\n", z.name,
		       p.x, p.y, p.z, r.grip.position.x, r.grip.position.y, r.grip.position.z,
		       gHolsterDist[i] == 0.0 ? "INSIDE the zone" : FStringf("%.3f m from the ring", gHolsterDist[i]).GetChars());
	}
}

bool DoomVR_HandsMatrix(int hand, float out[16], DoomVR_HandInput& in, int mode, const DoomVR_WeaponTune* tune, const float* fistCentre,
                        const DoomVR_Support* support, const DoomVR_GripHold* gripHold, const DoomVR_WeaponTune* handTune)
{
	if (!DoomVR_Active()) return false;
	const bool weaponHand = hand == 0;
	if (!(weaponHand ? gGripValid : gLeftValid)) return false;
	const pcvrxr::Pose& grip = weaponHand ? gGrip : gLeftGrip;
	const pcvrxr::Pose& aim = weaponHand ? gAimPose : gLeftAimPose;
	const pcvrxr::HandState& h = Hand(weaponHand ? pcvrxr::Hand::Right : pcvrxr::Hand::Left);
	in.trigger = h.trigger;
	in.grip = h.squeeze;
	in.physicalLeft = (hand == 1) != Leftorium();
	const double mir = in.physicalLeft ? -1.0 : 1.0;
	const double size = std::clamp(double(vr_hands_size), 0.5, 2.0);

	const double half = (-45.0 + vr_hands_pitch) * 0.5 * kToRad;
	const pcvrxr::Quat tilt{ float(sin(half)), 0, 0, float(cos(half)) };
	pcvrxr::Quat qh = QuatMul(grip.orientation, tilt);	// the hand's turn in the room
	pcvrxr::Vec3 origin;
	// Where the hand holds the gun and the gun's own turn, for the player's
	// hand tuning (turned about that point, moved along the gun's axes).
	bool tunable = false;
	double tuneMir = 1.0;
	pcvrxr::Vec3 pivot{ 0, 0, 0 };
	pcvrxr::Quat gunQ{ 0, 0, 0, 1 };
	if (!weaponHand && mode == 3 && support)
	{
		/*
			Two hands: the other hand on the gun's front grip - the pump, the
			fore-end, the front grip or the carry bar - placed by the hands
			project's FRONT-HAND table (its turn and origin, in the aim's frame
			from the gun's grip point), with the gun: the same aim, the same
			recoil. The Leftorium mirrors the table (x, and the turn's y z).
		*/
		static const DoomVR_WeaponTune kNone;
		const bool gl = Leftorium();
		const double sm = gl ? -1.0 : 1.0;
		const pcvrxr::Vec3 g = GunGrip(gl, tune ? *tune : kNone);
		const pcvrxr::Pose k = KickedAim(gAimPose, g);
		const pcvrxr::Quat qs{ support->q[0], float(sm * support->q[1]), float(sm * support->q[2]), support->q[3] };
		qh = QuatMul(k.orientation, qs);
		const pcvrxr::Vec3 o = Rotate(k.orientation, g.x + sm * support->origin[0], g.y + support->origin[1], g.z + support->origin[2]);
		origin = { k.position.x + o.x, k.position.y + o.y, k.position.z + o.z };
		const pcvrxr::Vec3 sp = Rotate(k.orientation, g.x + sm * support->point[0], g.y + support->point[1], g.z + support->point[2]);
		tunable = true; tuneMir = sm; gunQ = k.orientation;
		pivot = { k.position.x + sp.x, k.position.y + sp.y, k.position.z + sp.z };
	}
	else if (mode == 1 && gripHold)
	{
		/*
			Holding a B gun by its own hold pose (the hands project's PistolHold,
			StockHold, StockHoldSSG): the pose's hand point on the gun's grip
			point, the hand turned as the pose was fitted - hand = aim *
			T(grip point) * R * T(-hand point). With the recoil, as the gun.
			The other hand holds this way only inside a weapon worn on both
			(Heretic's Gauntlets): its mirror image, placed as the weapon
			model is in that hand, with no recoil.
		*/
		static const DoomVR_WeaponTune kNone;
		const pcvrxr::Vec3 g = GunGrip(in.physicalLeft, tune ? *tune : kNone);
		const pcvrxr::Pose k = weaponHand ? KickedAim(aim, g) : aim;
		const pcvrxr::Quat qr{ gripHold->q[0], float(mir * gripHold->q[1]), float(mir * gripHold->q[2]), gripHold->q[3] };
		qh = QuatMul(k.orientation, qr);
		const pcvrxr::Vec3 gp = Rotate(k.orientation, g.x, g.y, g.z);
		const pcvrxr::Vec3 hp = Rotate(qh, mir * gripHold->point[0] * size, gripHold->point[1] * size, gripHold->point[2] * size);
		const pcvrxr::Vec3 tweak = Rotate(qh, mir * vr_hands_x, vr_hands_y, vr_hands_z);
		origin = { k.position.x + gp.x - hp.x + tweak.x, k.position.y + gp.y - hp.y + tweak.y, k.position.z + gp.z - hp.z + tweak.z };
		tunable = true; tuneMir = mir; gunQ = k.orientation;
		pivot = { k.position.x + gp.x, k.position.y + gp.y, k.position.z + gp.z };
	}
	else if (weaponHand && mode == 1)
	{
		/*
			Holding a gun: the fist's middle on the gun's grip point, so the hand
			holds the handle whatever this runtime puts between the aim and grip
			poses (Touch over Virtual Desktop: 9.5 cm, 60 degrees - the hand on
			the grip alone missed the handle by 4.5 cm). The hand
			turns with the GUN - the aim pose, which the gun is drawn from - not
			the controller's grip: the grip turned it about the handle by the
			runtime's aim-to-grip angle, 60 degrees on Touch, fingers through the
			gun on every weapon. On the desk's test hands
			the two poses are one, which hid it. It moves with the recoil as the
			gun does.
		*/
		static const DoomVR_WeaponTune kNone;
		const pcvrxr::Vec3 g = GunGrip(in.physicalLeft, tune ? *tune : kNone);
		const pcvrxr::Pose k = KickedAim(aim, g);
		qh = QuatMul(k.orientation, tilt);
		const pcvrxr::Vec3 fist = Rotate(k.orientation, g.x, g.y, g.z);
		// The closed hand's middle, in its own space (the hands project's
		// fist_centre - each pair of hands has its own).
		static const float kBaseFist[3] = { -0.0330f, 0.0002f, -0.0810f };
		const float* fc = fistCentre ? fistCentre : kBaseFist;
		const pcvrxr::Vec3 c = Rotate(qh, mir * fc[0] * size, fc[1] * size, fc[2] * size);
		const pcvrxr::Vec3 tweak = Rotate(qh, mir * vr_hands_x, vr_hands_y, vr_hands_z);
		origin = { k.position.x + fist.x - c.x + tweak.x, k.position.y + fist.y - c.y + tweak.y, k.position.z + fist.z - c.z + tweak.z };
		tunable = true; tuneMir = mir; gunQ = k.orientation;
		pivot = { k.position.x + fist.x, k.position.y + fist.y, k.position.z + fist.z };
	}
	else
	{
		// gdh::HandAttach's defaults (right hand; the left mirrors x).
		const double ax = 0.0308 + vr_hands_x, ay = 0.0290 + vr_hands_y, az = 0.0275 + vr_hands_z;
		const pcvrxr::Vec3 off = Rotate(grip.orientation, mir * ax, ay, az);
		origin = { grip.position.x + off.x, grip.position.y + off.y, grip.position.z + off.z };
		if (weaponHand && mode == 2 && vr_weapon_kick && gKickMelee && NowMs() - gSwingAt[0] > 500)
		{
			// A punch: out along the aim fast, back slower.
			const double a = KickShape(70, 180);
			const pcvrxr::Vec3 f = Rotate(aim.orientation, 0, 0, -0.16 * a);
			origin.x += f.x; origin.y += f.y; origin.z += f.z;
		}
	}

	if (tunable && handTune)
	{
		// The player's tuning (the tuner's HAND and SUPPORT modes): a turn in
		// the gun's frame about where the hand holds - tilt about its right
		// axis, turn about its up (barrel right is negative), roll about its
		// forward, mirrored as the weapon's own - then a move along the gun's
		// axes (right, up, forward).
		auto axisQ = [](double x, double y, double z, double deg) {
			const double h = deg * 0.5 * kToRad, sn = sin(h);
			return pcvrxr::Quat{ float(x * sn), float(y * sn), float(z * sn), float(cos(h)) };
		};
		const pcvrxr::Quat rl = QuatMul(QuatMul(axisQ(1, 0, 0, handTune->pitch), axisQ(0, 1, 0, -tuneMir * handTune->yaw)),
		                                axisQ(0, 0, -1, tuneMir * handTune->roll));
		const pcvrxr::Quat gi{ -gunQ.x, -gunQ.y, -gunQ.z, gunQ.w };
		const pcvrxr::Quat rw = QuatMul(QuatMul(gunQ, rl), gi);	// the same turn in the room
		qh = QuatMul(rw, qh);
		const pcvrxr::Vec3 rel = Rotate(rw, origin.x - pivot.x, origin.y - pivot.y, origin.z - pivot.z);
		const pcvrxr::Vec3 mv = Rotate(gunQ, tuneMir * handTune->right, handTune->up, -handTune->forward);
		origin = { pivot.x + rel.x + mv.x, pivot.y + rel.y + mv.y, pivot.z + rel.z + mv.z };
	}

	const pcvrxr::Pose& eyePose = gSession->GetView(gDrawingEye).pose;
	const pcvrxr::Quat& qe = eyePose.orientation;
	const pcvrxr::Quat qi{ -qe.x, -qe.y, -qe.z, qe.w };
	const pcvrxr::Vec3 d = Rotate(qi, origin.x - eyePose.position.x, origin.y - eyePose.position.y, origin.z - eyePose.position.z);
	const pcvrxr::Quat q = QuatMul(qi, qh);
	const double u = vr_hunits_per_meter;
	const float s = float(u * size);
	const pcvrxr::Vec3 cx = Rotate(q, 1, 0, 0), cy = Rotate(q, 0, 1, 0), cz = Rotate(q, 0, 0, 1);
	const float m[16] = {
		float(cx.x * s), float(cx.y * s), float(cx.z * s), 0,
		float(cy.x * s), float(cy.y * s), float(cy.z * s), 0,
		float(cz.x * s), float(cz.y * s), float(cz.z * s), 0,
		float(d.x * u),  float(d.y * u),  float(d.z * u),  1,
	};
	memcpy(out, m, sizeof(m));
	return true;
}

void DoomVR_SetDrawingHand(int hand)
{
	gDrawingHand = hand;
}

void DoomVR_SetDrawingTune(const DoomVR_WeaponTune& tune)
{
	gDrawingTune = tune;
}

bool DoomVR_DrawingLeftHand()
{
	return DoomVR_Active() && ((gDrawingHand == 1) != Leftorium());
}

bool DoomVR_PhotoMode()
{
	return DoomVR_Active() && gPhotoMode;
}

bool DoomVR_TakeScreenshotRequest()
{
	if (!DoomVR_Active() || !gScreenshotRequested) return false;
	gScreenshotRequested = false;
	return true;
}

bool DoomVR_GetAim(double& yawOffset, double& pitch)
{
	if (!DoomVR_Active() || !gAimValid) return false;
	yawOffset = gAimYaw;
	pitch = gAimPitch;
	return true;
}

bool DoomVR_GetLookYaw(double& yawOffset)
{
	if (!DoomVR_Active() || !gCentered) return false;
	yawOffset = gLookYaw;
	return true;
}

double DoomVR_ViewBob()
{
	return DoomVR_Active() && !vr_view_bob ? 0.0 : 1.0;
}

bool DoomVR_WeaponBob()
{
	return !DoomVR_Active() || vr_weapon_bob;
}

bool DoomVR_BlendInScene()
{
	return DoomVR_Active() || vr_debug_blend_in_scene;
}

bool DoomVR_GameManualShowing()
{
	TomeSignal* s = gUp ? Tome() : nullptr;
	return s && s->showing;
}

class FFont;
void C_MidPrint(FFont* font, const char* message, bool bold);	// g_statusbar

// VR Options > Game Tome: the book in the other hand.
CCMD(vr_game_manual)
{
	TomeSignal* s = gUp ? Tome() : nullptr;
	if (!s || !s->ready)
	{
		C_MidPrint(nullptr, "The Game Tome needs the headset and Doom VR's book (gametome) beside the game.", false);
		return;
	}
	const FString dir = BookFor(strlen(*vr_book_game) ? *vr_book_game : *vr_current_game);
	snprintf(s->tomeDir, sizeof(s->tomeDir), "%s", dir.GetChars());
	s->mainHand = Leftorium() ? 0 : 1;
	s->floating = 0;
	s->open = 1;
	gToldGame = *vr_current_game;
	gToldHand = s->mainHand;
	Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: Game Tome opened (%s)\n", dir.GetChars());
}

int DoomVR_NotifyScale(int scale, int canvasHeight)
{
	// The messages' scale on the panel: the flat game's text there is about
	// half a degree tall; a canvas height over 240 makes it about 1.2.
	return DoomVR_Active() ? std::max(scale, canvasHeight / 240) : scale;
}

bool DoomVR_StatusBarShown()
{
	if (!DoomVR_Active() || !vr_hud_glance) return true;
	double yaw, pitch, roll;
	ToYawPitchRoll(gSession->Head().orientation, yaw, pitch, roll);
	// Shown from vr_hud_glance_angle below level, put away again 8 degrees
	// above that. Pitch is positive looking up.
	static bool shown = false;
	const double down = -pitch * kToDeg, at = std::clamp(double(vr_hud_glance_angle), 5.0, 60.0);
	if (down > at) shown = true;
	else if (down < at - 8.0) shown = false;
	return shown;
}

void DoomVR_Pulse(int hands, float seconds, float amplitude, const char* why)
{
	if (vr_debug_haptics)
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: haptic %s - %s, %.2f s at %.2f%s\n", why, hands == 3 ? "both hands" : hands == 2 ? "right hand" : "left hand",
		       seconds, amplitude, vr_haptics ? "" : " (vr_haptics is off, not sent)");
	if (!vr_haptics || !DoomVR_Active()) return;
	const float a = std::clamp(amplitude * (float)vr_haptics_strength, 0.f, 1.f);
	if (a <= 0.f) return;
	if (hands & 1) gSession->Haptic(pcvrxr::Hand::Left, seconds, a);
	if (hands & 2) gSession->Haptic(pcvrxr::Hand::Right, seconds, a);
}

unsigned DoomVR_TurnThisFrame()
{
	// The time since the last frame's turn: smooth turning is a speed, and a
	// frame is not a fixed length. Capped, so a hitch is not a lurch.
	const double now = NowMs();
	const double dt = gLastTurnMs > 0 ? std::clamp((now - gLastTurnMs) / 1000.0, 0.0, 0.1) : 0.0;
	gLastTurnMs = now;
	if (!DoomVR_Active()) { gSnapPending = 0; return 0; }
	double deg = 0;
	if (vr_snap_turn > 0)
	{
		deg = gSnapPending;
		gSnapPending = 0;
	}
	else
	{
		// A small dead zone, then linear: turn speed is what the player set.
		const float x = gTurnStick, dz = 0.15f;
		const float v = fabsf(x) < dz ? 0.f : (fabsf(x) - dz) / (1.f - dz) * (x > 0 ? 1.f : -1.f);
		deg = -v * double(vr_turn_speed) * dt;
	}
	// Degrees to BAM, positive left, wrapping as angles do.
	return unsigned(int64_t(llround(deg / 360.0 * 4294967296.0)));
}

bool DoomVR_ShouldRender()
{
	return DoomVR_Active() && gFrameBegun && gRender;
}

void DoomVR_EyeSize(int& width, int& height)
{
	width = height = 0;
	if (gUp) gSession->EyeBufferSize(width, height);
}

void DoomVR_EyeProjection(int eye, float nearZ, float farZ, float out[16])
{
	pcvrxr::ProjectionMatrix(gSession->GetView(eye).fov, nearZ, farZ, out);
}

// A test switch: the vignette full on, standing still, so a picture with it
// and one without show exactly what it darkens.
CVAR(Bool, vr_debug_vignette, false, 0)

float DoomVR_ComfortVignette(float& innerRadius, float& maxAlpha)
{
	static float level = 0;
	static double lastMs = 0;
	const double now = NowMs();
	const double dt = lastMs > 0 ? std::clamp((now - lastMs) / 1000.0, 0.0, 0.1) : 0.0;
	lastMs = now;
	if (!DoomVR_Active() || vr_comfort_vignette <= 0) { level = 0; return 0; }
	// How much the sticks are moving or turning the player now: the left
	// stick past its dead zone, the right stick turning smoothly, a snap.
	const bool playing = gamestate == GS_LEVEL && menuactive == MENU_Off && !gPhotoMode;
	float want = 0;
	if (playing)
	{
		const float mv = sqrtf(gAxis[LX] * gAxis[LX] + gAxis[LY] * gAxis[LY]);
		want = std::min(1.f, mv * 1.5f);
		if (fabsf(gTurnStick) > 0.15f || gSnapPending != 0) want = 1;
		if (vr_debug_vignette) want = 1;
	}
	// In quickly (a fifth of a second), out slowly (half a second).
	const float rate = want > level ? 5.f : 2.f;
	level += (want - level) * std::min(1.f, float(dt) * rate * 2.f);
	if (level < 0.01f) level = 0;
	const bool strong = vr_comfort_vignette >= 2;
	innerRadius = strong ? 18.f : 25.f;	// degrees of clear view round straight ahead
	maxAlpha = strong ? 0.97f : 0.8f;
	return level;
}

void DoomVR_DrawMenuPointer()
{
	if (!DoomVR_Active() || !gPointerOn || menuactive == MENU_Off || twod == nullptr) return;
	// A dot the size of a small button: dark rim, bright middle.
	const int s = std::max(3, screen->GetHeight() / 160);
	ClearRect(twod, gPointerX - s - 2, gPointerY - s - 2, gPointerX + s + 2, gPointerY + s + 2, -1, MAKEARGB(255, 20, 20, 20));
	ClearRect(twod, gPointerX - s, gPointerY - s, gPointerX + s, gPointerY + s, -1, gPointerDown ? MAKEARGB(255, 255, 80, 60) : MAKEARGB(255, 255, 255, 255));
}

// The spectator view's cut-out of the left eye for a window outW x outH,
// from the View this frame's eyes were drawn and submitted with. Corners are
// fractions of the eye picture, y down: the window's top-left, top-right,
// bottom-right, bottom-left. False: draw the eye the old way.
static pcvrxr::Spectator gSpectator;
static FString gSpectatorDump;

bool DoomVR_SpectatorCrop(int outW, int outH, float corners[8])
{
	if (!DoomVR_ShouldRender() || !vr_spectator || outW <= 0 || outH <= 0) return false;
	const bool plain = menuactive != MENU_Off || ConsoleState == c_down || ConsoleState == c_falling || gPhotoMode;
	int w, h;
	DoomVR_EyeSize(w, h);
	const pcvrxr::SpectatorCrop c = gSpectator.Update(gSession->GetView(0), w, h, float(outW) / float(outH), NowMs() / 1000.0, plain);
	for (int i = 0; i < 4; ++i) { corners[2 * i] = c.corner[i].x; corners[2 * i + 1] = c.corner[i].y; }
	static int said = -1;
	if (said != int(c.steady))
	{
		said = int(c.steady);
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: desktop view %s\n", c.steady ? "steady (spectator)" : "plain");
	}
	return true;
}

const char* DoomVR_TakeSpectatorDumpRequest()
{
	static FString taken;
	if (gSpectatorDump.IsEmpty()) return nullptr;
	taken = gSpectatorDump;
	gSpectatorDump = "";
	return taken.GetChars();
}

// Test hook: the window's picture, the eye picture it was cut from and the
// corners used, into <dir> on the next frame (spectator_window.ppm,
// spectator_eye.ppm, spectator_corners.txt).
CCMD(vr_dump_spectator)
{
	if (!gUp) { Printf("vr_dump_spectator: no headset session\n"); return; }
	gSpectatorDump = argv.argc() > 1 ? argv[1] : ".";
	Printf("vr_dump_spectator: requested into %s\n", gSpectatorDump.GetChars());
}

bool DoomVR_RoomPending(double& right, double& forward)
{
	gCarriedPrev[0] = gCarried[0];
	gCarriedPrev[1] = gCarried[1];
	if (!DoomVR_Active() || !gCentered || !vr_room_scale) return false;
	double up;
	FromOrigin(gSession->Head().position, right, up, forward);
	right -= gCarried[0];
	forward -= gCarried[1];
	return true;
}

void DoomVR_RoomCarry(double right, double forward)
{
	gCarried[0] += right;
	gCarried[1] += forward;
}

// Crouch for real (vr_real_crouch): the head 30 cm below where it was at the
// recenter crouches the body, so it fits under what a crouch fits under;
// back above 22 cm down stands it up (the gap keeps it from flickering at
// the edge). The game's crouch then lowers the body, not the eyes - the head
// already has (p_user.cpp puts the view back).
static bool gRealCrouch = false;
bool DoomVR_RealCrouch()
{
	if (!DoomVR_Active() || !gCentered || !vr_real_crouch) return gRealCrouch = false;
	double right, up, forward;
	FromOrigin(gSession->Head().position, right, up, forward);
	if (up < -0.30) gRealCrouch = true;
	else if (up > -0.22) gRealCrouch = false;
	return gRealCrouch;
}

DoomVR_EyeView DoomVR_GetEyeView(int eye, double playerYawDegrees, double ticFrac)
{
	DoomVR_EyeView ev;
	if (!DoomVR_Active()) return ev;

	const pcvrxr::View& v = gSession->GetView(eye);

	double yaw, pitchUp, roll;
	ToYawPitchRoll(v.pose.orientation, yaw, pitchUp, roll);

	// UZDoom's view matrix applies the INVERSE of the camera's rotation, so
	// its pitch and roll are the head's negated: a positive pitch looks down,
	// and a positive roll drops the right ear. Yaw goes through the player's
	// angle, which the engine inverts itself. Yaw is measured from the
	// recenter direction; turning the whole pose by a yaw leaves pitch and
	// roll alone in this decomposition, so only yaw needs it.
	ev.yaw = (yaw - gOriginYaw) * kToDeg + gPhotoYaw;
	ev.pitch = -pitchUp * kToDeg + vr_debug_pitch;
	ev.roll = -roll * kToDeg + vr_debug_roll;

	/*
		Where the eye is, from the recenter point, into the world: the eye's
		offset from the head AND the head's own movement - leaning, ducking,
		stepping - so the player's position is where the head was when the
		view was recentered.

		Relative to the recenter point, forward (-Z) is where the player's
		body faces and +X is its right. UZDoom's yaw runs anticlockwise from
		+X (east), so facing is (cos, sin) and right is (sin, -cos). Only the
		body's yaw is applied here: the head's is already in the position.

		The same map units per metre as the eye separation, so parallax and
		stereo agree about how far away things are.
	*/
	double right, up, forward;
	FromOrigin(v.pose.position, right, up, forward);
	// Less what the body has carried (room-scale), blended across the tic.
	const double f = ticFrac < 0 ? 0 : (ticFrac > 1 ? 1 : ticFrac);
	right -= gCarriedPrev[0] + (gCarried[0] - gCarriedPrev[0]) * f;
	forward -= gCarriedPrev[1] + (gCarried[1] - gCarriedPrev[1]) * f;
	// Photo mode's turn swings the eyes round the head with the view.
	const double t = (playerYawDegrees + gPhotoYaw) * kToRad;
	const double u = vr_hunits_per_meter;

	ev.dx = (forward * cos(t) + right * sin(t)) * u;
	ev.dy = (forward * sin(t) - right * cos(t)) * u;
	ev.dz = (up + vr_height_adjust) * u;
	return ev;
}

void DoomVR_BeginEye(int eye)
{
	if (!DoomVR_ShouldRender()) return;
	const bool trace = Tracing();
	if (trace) Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR trace: frame %d BeginEye %d...\n", gFrames, eye);
	const double t0 = NowMs();
	gSession->BeginEye(eye);
	const double dt = NowMs() - t0;
	gEyeMs += dt;
	if (trace) Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR trace: frame %d BeginEye %d after %.2f ms\n", gFrames, eye, dt);
}

void DoomVR_EndEye(int eye)
{
	if (!DoomVR_ShouldRender()) return;
	const bool trace = Tracing();
	if (trace) Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR trace: frame %d EndEye %d...\n", gFrames, eye);
	const double t0 = NowMs();
	gSession->EndEye(eye);
	const double dt = NowMs() - t0;
	gEyeMs += dt;
	if (trace) Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR trace: frame %d EndEye %d after %.2f ms\n", gFrames, eye, dt);
}

void DoomVR_EndFrame()
{
	if (!gUp || !gFrameBegun) return;
	const bool trace = Tracing();
	if (trace) Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR trace: frame %d EndFrame...\n", gFrames);
	const double t0 = NowMs();
	gSession->EndFrame();
	gFrameBegun = false;
	const double dt = NowMs() - t0;
	gSubmitMs += dt;
	if (trace) Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR trace: frame %d EndFrame after %.2f ms\n", gFrames, dt);
	++gFrames;
#ifdef __ANDROID__
	// Standalone: a performance line every 15 s, so a headset's log says what
	// each game held - the rate the runtime runs at, the frames delivered,
	// and where each frame's time went: waiting on the runtime (the frame
	// was early), the eye copies and the submit (the layer), the engine (the
	// game itself, CPU), and the GPU's own time for the frame. A second line
	// splits the engine: the game tics, events, the display (with the
	// renderer's own clocks: the scene's BSP walk and draw-list build, the
	// draw submission, post-processing, the 2D), the music update, and the
	// HUD panel; plus what the scene held.
	{
		static double perfAt = 0, perfWait = 0, perfEye = 0, perfSubmit = 0;
		static int perfFrames = 0;
		// The renderer's clocks are per frame: add this frame's.
		gPerf[5] += All.TimeMS(); gPerf[6] += Bsp.TimeMS(); gPerf[7] += ProcessAll.TimeMS(); gPerf[8] += RenderAll.TimeMS();
		gPerf[9] += PostProcess.TimeMS(); gPerf[10] += twoD.TimeMS(); gPerf[11] += Finish.TimeMS();
		gPerfLines += rendered_lines; gPerfFlats += rendered_flats; gPerfSprites += rendered_sprites;
		const double now = NowMs();
		if (perfAt == 0) { perfAt = now; perfFrames = gFrames; perfWait = gWaitMs; perfEye = gEyeMs; perfSubmit = gSubmitMs; }
		else if (now - perfAt >= 15000)
		{
			const int n = gFrames - perfFrames;
			if (n > 0)
			{
				const double ms = (now - perfAt) / n;
				const double wait = (gWaitMs - perfWait) / n, eyes = (gEyeMs - perfEye) / n, submit = (gSubmitMs - perfSubmit) / n;
				double gpuAvg = 0, gpuMax = 0; int gpuN = 0;
				bool DoomVR_GpuTimerStats(double& avgMs, double& maxMs, int& frames);
				const bool gpu = DoomVR_GpuTimerStats(gpuAvg, gpuMax, gpuN);
				char line[400];
				snprintf(line, sizeof(line),
				         "DoomVR: perf %.0f s: %.0f Hz; %d frames, %.1f fps; per frame %.2f ms = runtime wait %.2f + eyes %.2f + submit %.2f + engine %.2f",
				         (now - perfAt) / 1000.0, gSession->RefreshRate(), n, 1000.0 / ms, ms, wait, eyes, submit, ms - wait - eyes - submit);
				if (gpu) snprintf(line + strlen(line), sizeof(line) - strlen(line), "; GPU %.2f ms avg, %.2f max", gpuAvg, gpuMax);
				Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "%s\n", line);
				double p[kPerfSlots];
				for (int i = 0; i < kPerfSlots; ++i) p[i] = gPerf[i] / n;
				Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY),
				       "DoomVR: perf split: tics %.2f, events %.2f, display %.2f (scene %.2f: bsp %.2f, lists %.2f, draw %.2f, post %.2f; 2D %.2f; last swap %.2f), music %.2f, panel %.2f ms; scene per frame: %d lines, %d flats, %d sprites\n",
				       p[0], p[1], p[2], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[3], p[4],
				       (int)(gPerfLines / n), (int)(gPerfFlats / n), (int)(gPerfSprites / n));
				Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY),
				       "DoomVR: perf post split (both eyes): dot %.2f, 3D guns %.2f, hands %.2f, wrist %.2f, holsters %.2f, wheel %.2f, weapon sprites %.2f, blends %.2f ms\n",
				       p[12], p[13], p[14], p[15], p[16], p[17], p[18], p[19]);
			}
			perfAt = now; perfFrames = gFrames; perfWait = gWaitMs; perfEye = gEyeMs; perfSubmit = gSubmitMs;
			for (int i = 0; i < kPerfSlots; ++i) gPerf[i] = 0;
			gPerfLines = gPerfFlats = gPerfSprites = 0;
		}
	}
#endif
}

#ifdef __ANDROID__
// d_main.cpp and the GLES Flush add their parts of the frame here:
// 0 tics, 1 events, 2 display, 3 music, 4 the HUD panel (5-11 the renderer's own clocks).
void DoomVR_PerfAdd(int slot, double ms)
{
	if (slot >= 0 && slot < kPerfSlots) gPerf[slot] += ms;
}
#endif

// Test hooks. None of them is a setting and none is in a menu.

// A keyboard key pressed and held for [frames] (default 3), for testing the
// weapon tuner's keys with nobody at the keyboard.
CCMD(vr_debug_key)
{
	static const struct { const char* name; int key; } kKeys[] = {
		{ "leftarrow", KEY_LEFTARROW }, { "rightarrow", KEY_RIGHTARROW }, { "uparrow", KEY_UPARROW },
		{ "downarrow", KEY_DOWNARROW }, { "pgup", KEY_PGUP }, { "pgdn", KEY_PGDN },
		{ "home", KEY_HOME }, { "end", KEY_END }, { "ins", KEY_INS }, { "del", KEY_DEL },
		{ "lbracket", 0x1a }, { "rbracket", 0x1b },
	};
	if (argv.argc() < 2) { Printf("vr_debug_key <leftarrow|rightarrow|uparrow|downarrow|pgup|pgdn|home|end|ins> [frames]\n"); return; }
	int code = 0;
	for (auto& k : kKeys) if (stricmp(argv[1], k.name) == 0) code = k.key;
	if (code == 0) { Printf("vr_debug_key: no key %s\n", argv[1]); return; }
	if (gDebugKeyFrames > 0) { gDebugKeyFrames = 1; ReleaseDebugKey(); }
	event_t ev = {};
	ev.type = EV_KeyDown;
	ev.data1 = code;
	D_PostEvent(&ev);
	gDebugKey = code;
	gDebugKeyFrames = argv.argc() > 2 ? std::max(1, atoi(argv[2])) : 3;
	Printf("vr_debug_key: %s down for %d frames\n", argv[1], gDebugKeyFrames);
}

// A screenshot of what the headset shows, into UZDoom's screenshots folder.
CCMD(vr_screenshot)
{
	if (!gUp) { Printf("vr_screenshot: no headset session\n"); return; }
	gScreenshotRequested = true;
}

// Make where the head is now, and the way it faces, the player's position
// and forward. In the VR Options menu.
CCMD(vr_recenter)
{
	if (!gUp) { Printf("vr_recenter: no headset session\n"); return; }
	gRecenterAsked = true;
	gRecenterWhy = "asked";
}

void DoomVR_LevelStarted()
{
	if (!gUp) return;
	gRecenterAsked = true;
	gRecenterWhy = "a level started";
}

// Test hook: what a runtime's own recenter looks like to DoomVR - the layer
// rebuilds its space and bumps its generation. With vr_debug_head_y changed
// at the same moment, the next reference must be taken at the new height.
CCMD(vr_debug_space_change)
{
	if (!gUp) { Printf("vr_debug_space_change: no headset session\n"); return; }
	gSession->SimulateSpaceChange();
}

// Hold one control on made-up controllers for N frames (default 20), then
// release it: vr_debug_input rtrigger 30. Controls: a b x y menu lgrip rgrip
// lclick rclick ltrigger rtrigger lup lrun ldown lleft lright rleft rright rup
// (lrun: the left stick forward and clicked in).
CCMD(vr_debug_input)
{
	if (!gUp) { Printf("vr_debug_input: no headset session\n"); return; }
	if (argv.argc() < 2) { Printf("vr_debug_input <control> [frames]\n"); return; }
	gFakeControl = argv[1];
	gFakeFrames = argv.argc() > 2 ? atoi(argv[2]) : 20;
	gFakeStartMs = NowMs();
	gFakeOn = true;
	gSession->SetFakeHands(true);
	Printf("vr_debug_input: holding %s for %d frames\n", gFakeControl.GetChars(), gFakeFrames);
}

// Both eyes as the runtime received them, to <dir>/eye0_NNN.ppm and
// eye1_NNN.ppm, on the next frame that is drawn.
CCMD(vr_dump_eyes)
{
	if (!gUp) { Printf("vr_dump_eyes: no headset session\n"); return; }
	const char* dir = argv.argc() > 1 ? argv[1] : ".";
	gSession->RequestEyeDump(dir);
	Printf("vr_dump_eyes: requested into %s\n", dir);
}

// What each eye is given, as numbers: the runtime's field of view, the angles
// derived from the eye's pose, and the eye's offset in map units for a player
// facing east (yaw 0). With the head level and square, the left eye's offset
// must point north (+y) and the right eye's south.
CCMD(vr_report)
{
	if (!gUp) { Printf("vr_report: no headset session\n"); return; }
	int w, h;
	gSession->EyeBufferSize(w, h);
	Printf("vr_report: %s, eye %dx%d, vr_mode %d, active %d\n",
	       gSession->RuntimeName(), w, h, (int)vr_mode, (int)DoomVR_Active());

	// Pacing: what the runtime runs at, and what we actually delivered since
	// the last report. Below the refresh rate means something other than
	// xrWaitFrame is holding the frame - the desktop swap's vsync, typically.
	const double now = NowMs();
	if (gReportMs > 0 && gFrames > gReportFrames)
	{
		const double ms = (now - gReportMs) / (gFrames - gReportFrames);
		const int n = gFrames - gReportFrames;
		Printf("vr_report: runtime %.1f Hz; %d frames since the last report, %.2f ms each (%.1f fps)\n",
		       gSession->RefreshRate(), n, ms, 1000.0 / ms);
		const double wait = gWaitMs / n, eyes = gEyeMs / n, submit = gSubmitMs / n;
		Printf("vr_report: per frame: waiting on the runtime %.2f, eye copies %.2f, submit %.2f, "
		       "engine (UNACCOUNTED by the layer) %.2f ms\n", wait, eyes, submit, ms - wait - eyes - submit);
	}
	else
	{
		Printf("vr_report: runtime %.1f Hz\n", gSession->RefreshRate());
	}
	gReportMs = now;
	gReportFrames = gFrames;
	gWaitMs = gEyeMs = gSubmitMs = 0;
	for (int hand = 0; hand < 2; ++hand)
	{
		const pcvrxr::HandState& h = Hand(pcvrxr::Hand(hand));
		Printf("vr_report: %s hand %s, %s\n", hand ? "right" : "left",
		       h.active ? "active" : "not active", gSession->InteractionProfile(pcvrxr::Hand(hand)));
	}
	for (int eye = 0; eye < 2; ++eye)
	{
		const pcvrxr::View& v = gSession->GetView(eye);
		const DoomVR_EyeView ev = DoomVR_GetEyeView(eye, 0.0);
		Printf("vr_report: eye %d fov L%.2f R%.2f U%.2f D%.2f deg, "
		       "yaw %.2f pitch %.2f roll %.2f, offset %.3f %.3f %.3f\n",
		       eye, v.fov.left * kToDeg, v.fov.right * kToDeg, v.fov.up * kToDeg, v.fov.down * kToDeg,
		       ev.yaw, ev.pitch, ev.roll, ev.dx, ev.dy, ev.dz);
	}

	// The decomposition, on rotations whose answer is known. OpenXR: a
	// positive angle about +Y turns the head left, about +X lifts it, about +Z
	// raises the right ear. In UZDoom's sense those are yaw +30, pitch -20
	// (up is negative) and roll -20 (a raised right ear is negative).
	struct { const char* what; double ax, ay, az, deg, yaw, pitch, roll; } known[] = {
		{ "turn left 30", 0, 1, 0, 30, 30, 0, 0 },
		{ "look up 20", 1, 0, 0, 20, 0, -20, 0 },
		{ "right ear up 20", 0, 0, 1, 20, 0, 0, -20 },
	};
	for (auto& k : known)
	{
		const double h = k.deg * kToRad / 2;
		pcvrxr::Quat q;
		q.x = float(k.ax * sin(h)); q.y = float(k.ay * sin(h)); q.z = float(k.az * sin(h)); q.w = float(cos(h));
		double y, p, r;
		ToYawPitchRoll(q, y, p, r);
		const double gy = y * kToDeg, gp = -p * kToDeg, gr = -r * kToDeg;
		const bool ok = fabs(gy - k.yaw) < 0.01 && fabs(gp - k.pitch) < 0.01 && fabs(gr - k.roll) < 0.01;
		Printf("vr_report: %s -> yaw %.2f pitch %.2f roll %.2f (want %.0f %.0f %.0f) %s\n",
		       k.what, gy, gp, gr, k.yaw, k.pitch, k.roll, ok ? "OK" : "WRONG");
	}
}

/*
	Switch Game in this process (doomvr_switchgame.cpp): the engine restarts
	into the next game without closing, so the headset session stays up.
	Before the restart the VR side lets go of everything that belongs to the
	old game (its classes, models, textures and files, here and in the hands,
	the weapon aliases and the learned shots), and while the engine loads, a
	frame now and then keeps the headset fed: nothing of the world in it, so
	the player sees black instead of the runtime's own screen.
*/
void DoomVR_HandsForgetGame();	// doomvr_hands.cpp
void DoomVR_TuneForgetGame();	// doomvr_tune.cpp
void DoomVR_ShotsForgetGame();	// p_user.cpp
double gSwitchStartMs = 0;	// when an in-process switch began (its time is logged when play resumes)
bool gSwitchRestarted = false;	// the engine has begun the restart (its frames were sent)
void DoomVR_ForgetGame()
{
	gSwitchStartMs = NowMs();
	WheelClose(false);
	gTwoHanded = false;
	gHeldLongGun = false;
	gHolsterInside = -1;
	gHolsterRearm = true;
	gToldGame = "";
	gToldHand = -1;
	gTomePaused = false;
	gTomeWasShowing = false;
	DoomVR_HandsForgetGame();
	DoomVR_TuneForgetGame();
	DoomVR_ShotsForgetGame();
	Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: the old game let go; the headset stays up through the restart\n");
}

// The engine restarting now (D_DoomLoop returned): the caches let go again.
// The old game drew on after vr_switchgame let go, and its last frame filled
// them from the old game - the hands' skin a texture number that named
// another texture in the next game (half visible, black and white), the
// grips keyed on the old game's guns (the hands off them).
void DoomVR_ForgetGameAtRestart()
{
	DoomVR_HandsForgetGame();
	DoomVR_TuneForgetGame();
	DoomVR_ShotsForgetGame();
}

void DoomVR_RestartFrame()
{
	if (!gUp || gFrameBegun || vr_mode != VR_HEADSET) return;
	gSwitchRestarted = true;
	gSession->BeginFrame();
	gSession->SuppressWorld();
	gSession->EndFrame();
}

// A long load with no frames in it: DOOM 64 CE's own shaders take seconds on
// an RTX 5080 the first time after a driver update (seen 23 Sep and 31 Oct;
// a quarter of a second on an RTX 3080 Ti). Windows calls a window that has not
// looked at its messages for 5 s hung, and the headset gets nothing. Called
// between shaders: every 50 ms at most the queue is looked at (nothing taken
// from it) and, once the headset is up, it gets the restart's black frame.
void DoomVR_LoadKeepAlive()
{
	static double last = 0;
	const double now = NowMs();
	if (now - last < 50) return;
	last = now;
#ifdef _WIN32
	MSG msg;
	PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
#endif
	DoomVR_RestartFrame();
}

#else // built without pcvr-xr: the flat game, and nothing else

bool DoomVR_SpectatorCrop(int, int, float*) { return false; }
void DoomVR_ForgetGame() {}
void DoomVR_ForgetGameAtRestart() {}
void DoomVR_RestartFrame() {}
void DoomVR_LoadKeepAlive() {}
const char* DoomVR_TakeSpectatorDumpRequest() { return nullptr; }

bool DoomVR_Active() { return false; }
void DoomVR_Shutdown() {}
void DoomVR_BeginFrame()
{
	static bool said = false;
	if (vr_mode == VR_HEADSET && !said)
	{
		said = true;
		Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: this build has no headset support (built without pcvr-xr) - staying flat.\n");
	}
}
bool DoomVR_ShouldRender() { return false; }
void DoomVR_EyeSize(int& width, int& height) { width = height = 0; }
void DoomVR_EyeProjection(int, float, float, float out[16]) { memset(out, 0, 16 * sizeof(float)); }
DoomVR_EyeView DoomVR_GetEyeView(int, double, double) { return DoomVR_EyeView(); }
bool DoomVR_RoomPending(double&, double&) { return false; }
bool DoomVR_RealCrouch() { return false; }
void DoomVR_DrawMenuPointer() {}
float DoomVR_ComfortVignette(float&, float&) { return 0; }
void DoomVR_RoomCarry(double, double) {}
void DoomVR_BeginEye(int) {}
void DoomVR_EndEye(int) {}
void DoomVR_EndFrame() {}
void DoomVR_AddAxes(float*) {}
void DoomVR_RotateMove(int&, int&) {}
unsigned DoomVR_TurnThisFrame() { return 0; }
double DoomVR_ViewBob() { return 1.0; }
void DoomVR_Pulse(int, float, float, const char*) {}
bool DoomVR_BlendInScene() { return vr_debug_blend_in_scene; }
bool DoomVR_StatusBarShown() { return true; }
bool DoomVR_GameManualShowing() { return false; }
CCMD(vr_game_manual) { Printf("vr_game_manual: built without pcvr-xr\n"); }
int DoomVR_NotifyScale(int scale, int) { return scale; }
bool DoomVR_WeaponBob() { return true; }
bool DoomVR_GetAim(double&, double&) { return false; }
bool DoomVR_PhotoMode() { return false; }
bool DoomVR_TakeScreenshotRequest() { return false; }
CCMD(vr_screenshot) { Printf("vr_screenshot: built without pcvr-xr\n"); }
void DoomVR_SetDrawingEye(int) {}
bool DoomVR_BeginPanel(int, int, int&, int&) { return false; }
bool DoomVR_TomeKey(unsigned, unsigned, bool) { return false; }
bool DoomVR_TomeOwnsKey(unsigned, unsigned long long) { return false; }
void DoomVR_EndPanel() {}
bool DoomVR_WeaponMatrix(float, float, float, float, float[16], int, const DoomVR_WeaponTune*) { return false; }
bool DoomVR_HandModelMatrix(float[16]) { return false; }
bool DoomVR_HandModelToAim(const float[3], float[3]) { return false; }
bool DoomVR_HandPointPose(int, const double[3], double&, double&, double&) { return false; }
bool DoomVR_WheelState(int& count, int& highlight) { count = 0; highlight = -1; return false; }
double DoomVR_WheelAngle(int i, int n) { return 90.0 - 360.0 * i / (n > 0 ? n : 1); }
bool DoomVR_WheelFrame(float[16]) { return false; }
void DoomVR_SetWheelModel(bool, float, float, float) {}
float DoomVR_WheelModelLength() { return 0.15f; }
void DoomVR_SetDrawingHand(int) {}
void DoomVR_SetDrawingTune(const DoomVR_WeaponTune&) {}
bool DoomVR_DrawingLeftHand() { return false; }
bool DoomVR_HandPose(int, double&, double&, double&, double&, double&, double&) { return false; }
bool DoomVR_HandsMatrix(int, float[16], DoomVR_HandInput&, int, const DoomVR_WeaponTune*, const float*, const DoomVR_Support*, const DoomVR_GripHold*, const DoomVR_WeaponTune*) { return false; }
bool DoomVR_HolsterMarker(int, float[16], double&, bool&) { return false; }
bool DoomVR_SetHolsterModel(int) { return false; }
void DoomVR_WeaponKick(bool) {}
bool DoomVR_SwingAttack(int) { return false; }
double DoomVR_SwingPower(double& speed) { speed = 0; return 1.0; }
void DoomVR_NoteHeldGun(bool, const DoomVR_WeaponTune&, const DoomVR_Support*) {}
bool DoomVR_TwoHandedNow() { return false; }
bool DoomVR_TwoHandGunHeld() { return false; }
bool DoomVR_GetLookYaw(double&) { return false; }
CCMD(vr_dump_eyes) { Printf("vr_dump_eyes: built without pcvr-xr\n"); }
CCMD(vr_debug_input) { Printf("vr_debug_input: built without pcvr-xr\n"); }
CCMD(vr_recenter) { Printf("vr_recenter: built without pcvr-xr\n"); }
void DoomVR_LevelStarted() {}
CCMD(vr_report) { Printf("vr_report: built without pcvr-xr\n"); }

#endif
