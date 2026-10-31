/*
** doomvr_headset.h
**
** The headset: DoomVR's link between UZDoom's stereo framework (VRMode /
** VREyeInfo, hw_vrmodes.h) and pcvr-xr, our OpenXR layer.
**
** Written for Doom VR from UZDoom's VRMode interface and
** render loop (hw_entrypoint.cpp, gl_stereo3d.cpp, gl_framebuffer.cpp),
** pcvr-xr's public API (pcvrxr.h) and the OpenXR 1.1 specification.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#pragma once

#include "name.h"

// vr_mode value for a headset. UZDoom's own modes stop at 14; this one is
// deliberately clear of them.
enum { VR_HEADSET = 20 };

// True while a headset session is up and vr_mode asks for it. Everything else
// in this file is safe to call when it is false: the flat game is untouched.
bool DoomVR_Active();


// Before the GL context goes. A session left alive when its context is
// destroyed hangs the process on exit, unkillably.
void DoomVR_Shutdown();

// Once per displayed frame, before anything is rendered: waits for the
// runtime's pace and predicts the views. Safe to call twice in a frame.
//
// The session is brought up here, lazily, on the first frame that asks for the
// headset - the GL context is current on this thread by then, and it means
// vr_mode 20 can be switched on from the menu without a restart. With no
// headset it logs why, once, and the game stays flat.
void DoomVR_BeginFrame();

// From every renderer backend that cannot draw the headset (Vulkan, GLES):
// if vr_mode asks for it, say once which renderer it needs. Without this the
// game simply stays flat and nothing says why.
void DoomVR_UnsupportedBackend(const char* backend);

// At startup: the last run's log (log-doomvr.prev.txt, and its shutdown log)
// copied into logs\ under that run's date and time; the newest 10 kept.
void DoomVR_ArchiveLogs();

// Switch Game's handover (doomvr_switchgame.cpp): a game started by another
// waits for it to be gone before reaching for the headset, and says how long.
void DoomVR_WaitForPredecessor();
void DoomVR_ReportPredecessor();
void DoomVR_TestChainStart();	// desk test only: DOOMVR_TEST_CHAIN

// The red dot crosshair's point for this frame, in map units (x, y, z),
// set once a frame by the renderer and drawn by each eye.
void DoomVR_SetCrosshairPoint(bool valid, double x, double y, double z);
bool DoomVR_GetCrosshairPoint(double& x, double& y, double& z);

// Whether the runtime wants this frame drawn. False when idle or off the head.
bool DoomVR_ShouldRender();

// The eye buffer, in pixels. 0x0 when there is no session.
void DoomVR_EyeSize(int& width, int& height);

// Column-major projection for one eye, from the runtime's own asymmetric field
// of view - not the game's fov setting, which describes a monitor.
void DoomVR_EyeProjection(int eye, float nearZ, float farZ, float out[16]);

// How ONE eye looks and where it sits, for the render loop to apply to that
// eye's copy of the viewpoint - never to the player's own angles, which drive
// aim and movement.
//
//   yaw    degrees to ADD to the player's yaw (the head turned on the body)
//   pitch  the head's pitch, in UZDoom's sense: positive looks DOWN
//   roll   the head's roll, in UZDoom's sense: positive drops the RIGHT ear
//   dx/dy/dz  the eye's offset from the head, in map units, in the world,
//          given the player's yaw in degrees
struct DoomVR_EyeView {
    double yaw = 0, pitch = 0, roll = 0;
    double dx = 0, dy = 0, dz = 0;
};
// ticFrac: how far through the tic this frame is (the renderer's TicFrac),
// for the room-scale walk the body has carried (see DoomVR_RoomPending).
DoomVR_EyeView DoomVR_GetEyeView(int eye, double playerYawDegrees, double ticFrac = 1.0);

// Room-scale (vr_room_scale), once a tic from the playsim: the head's walk
// from the recenter point the body has not yet carried, metres along the
// recenter frame's right and forward - false when off, flat or not centered.
// The body then moves (with collision) and reports what it managed with
// RoomCarry; the eyes and hands show only the rest.
bool DoomVR_RoomPending(double& right, double& forward);
void DoomVR_RoomCarry(double right, double forward);

// The comfort vignette (vr_comfort_vignette): how strong it is now (0..1,
// eased), and for its shape the clear radius (degrees round straight ahead) and
// the darkest alpha at the edge. 0 when off, in a menu or standing still.
float DoomVR_ComfortVignette(float& innerRadius, float& maxAlpha);

// The menu pointer's dot (vr_menu_pointer), drawn over the menu on the 2D
// canvas after the menu itself.
void DoomVR_DrawMenuPointer();

// Crouch for real (vr_real_crouch): true while the head is down far enough
// that the body should crouch (G_BuildTiccmd holds crouch for it).
bool DoomVR_RealCrouch();

// The wall fade (vr_wall_fade): 0..1 black over the eye being drawn, set per
// eye from how far past a wall the eye is.
bool DoomVR_WallFadeEnabled();
void DoomVR_SetEyeFade(float alpha);
float DoomVR_EyeFade();

// The controllers, as the gamepad UZDoom already supports. Buttons arrive as
// the pad's own key events (pad_a, rtrigger, pad_start...), so the default
// gamepad bindings, the menus and the Controls menu all work unchanged; the
// sticks and triggers are added to the pad's analog axes here, from I_GetAxes.
// Axes is float[NUM_AXIS_CODES].
void DoomVR_AddAxes(float* axes);

// Stick movement goes where the head faces (or the left controller points,
// vr_move_follows 1), not where the body does: turns the move command by that
// yaw, from G_BuildTiccmd.
void DoomVR_RotateMove(int& forward, int& side);

// The right stick's turn for this frame, as a BAM angle to add to the
// player's (positive turns left): smooth at vr_turn_speed degrees a second
// over the time since the last frame, or one step of vr_snap_turn degrees per
// push. Called once per drawn frame, as a mouse's turn is, so the view turns
// at the headset's rate rather than in 35-a-second steps. The right stick is
// not handed to UZDoom's own turning, whose speed was never meant for a head
// that also turns.
unsigned DoomVR_TurnThisFrame();

// A level has just been loaded (a new game, the next map, a saved game): the
// player's height and forward are taken again from the head, at the first
// frame the head can be believed.
void DoomVR_LevelStarted();

// How much of the flat game's head bob the local player's view gets: 1 with
// no headset, 0 in one unless vr_view_bob is on.
double DoomVR_ViewBob();

// A controller pulse: hands 1 left, 2 right, 3 both; amplitude 0..1, scaled
// by vr_haptics_strength. Nothing unless vr_haptics is on; why is for the
// log (vr_debug_haptics).
void DoomVR_Pulse(int hands, float seconds, float amplitude, const char* why);

// Whether the screen blend (a hit's red, a pickup's flash, the radiation
// suit's green) is drawn over each eye's 3D view instead of on the 2D layer,
// which in a headset is the HUD panel.
bool DoomVR_BlendInScene();
bool DoomVR_StatusBarShown();
bool DoomVR_GameManualShowing();	// the Game Tome is open: the menu screen is put away
int DoomVR_NotifyScale(int scale, int canvasHeight);	// pickup messages' text scale on the panel	// in a headset, false while the eyes are not glancing down at it

// Whether the weapon bobs as the player walks: always with no headset, in one
// only when vr_weapon_bob is on.
bool DoomVR_WeaponBob();

// A 3D weapon model's place in the hand, for the eye being drawn: the right
// controller's grip in that eye's view space, map units, column-major. False
// with no headset (the model stays where the flat game puts it).
bool DoomVR_HandModelMatrix(float out[16]);

// Which hand the weapon model about to be drawn is in: 0 the weapon hand, 1
// the other (the off-hand weapon a VR mod gives).
void DoomVR_SetDrawingHand(int hand);
// True while the model being drawn is in the PHYSICAL left hand (the off hand,
// or the weapon hand in the Leftorium): it is drawn as a
// mirror image, as the VR mods' engine draws it (measured with the axis
// marker, tools\axis-model.py --offhand).
bool DoomVR_DrawingLeftHand();

// A hand for the VR mods (0 the weapon hand, 1 the other): its aim from the
// recenter point in metres (right, up, forward, as the eyes are placed) and
// its direction relative to the recenter yaw, degrees (yaw left positive,
// pitch up positive, roll right positive) with the 30-degree weapon tilt the
// mods expect. False with no headset or that controller not tracked.
bool DoomVR_HandPose(int hand, double& right, double& up, double& forward, double& yawDeg, double& pitchDeg, double& rollDeg);

// Game Or Die Hands (doomvr_hands.cpp): our own hand at a controller's palm.
// hand 0 is the weapon hand, 1 the other. out maps the hand's own space
// (OpenXR axes, metres) into the view space of the eye being drawn, map
// units, column-major. False with no headset or that controller not tracked.
struct DoomVR_HandInput
{
	float trigger = 0, grip = 0;
	bool physicalLeft = false;	// which of our two hand models to draw
};
// mode: 0 a free hand (on the grip pose), 1 holding the gun (the fist on its
// handle, moving with its recoil; tune = that weapon's fit), 2 a fist (punches).
struct DoomVR_WeaponTune;
struct DoomVR_Support;	// below
// The gun hand round a B gun's grip by one of the hands' gun-holding poses: the
// hand-space point that goes on the gun's grip point, and the hand's turn in the
// aim (x y z w), right-handed (the Leftorium mirrors them).
struct DoomVR_GripHold { float point[3]; float q[4]; };
// mode 3: the other hand on the gun's front grip (support, required).
// handTune: the player's own tuning of our hand on this weapon (the tuner's
// HAND / SUPPORT modes), applied about where the hand holds.
bool DoomVR_HandsMatrix(int hand, float out[16], DoomVR_HandInput& in, int mode = 0, const DoomVR_WeaponTune* tune = nullptr, const float* fistCentre = nullptr,
                        const DoomVR_Support* support = nullptr, const DoomVR_GripHold* gripHold = nullptr, const DoomVR_WeaponTune* handTune = nullptr);
// The two holsters (0 the hip, 1 the shoulder): a ring's place, the gun hand's
// distance from it (0 inside) and whether a grip now would draw; and the next
// model drawn put in holster i (-1 back in the hand).
bool DoomVR_HolsterMarker(int i, float out[16], double& handDist, bool& ready);
bool DoomVR_SetHolsterModel(int i);
// doomvr_hands.cpp: a grip at holster i - the draw, the put-away, or false
// when it is refused (empty, no ammo).
bool DoomVR_HolsterDraw(int i);
// doomvr_hands.cpp: the weapon in the player's hand now, only to see it change.
const void* DoomVR_ReadyWeaponId();
// The weapon hand's weapon just shot (or struck, melee): the recoil and punch.
// thrust: a held melee weapon whose model has no strike of its own (Heretic's
// Staff) goes out along the aim with the hand, as a fist punches.
void DoomVR_WeaponKick(bool melee, bool thrust = false);
// Physical play: this hand (0 the weapon hand) was just swung hard enough to
// strike - true for a few tics after the swing, while vr_phys_melee is on.
bool DoomVR_SwingAttack(int hand);
// The weapon wheel (doomvr_headset.cpp: the button, the ring's place, the slow
// motion; doomvr_hands.cpp: the guns, the choice, the look).
bool DoomVR_WheelState(int& count, int& highlight);	// true while it is up
double DoomVR_WheelAngle(int i, int n);
bool DoomVR_WheelFrame(float out[16]);
void DoomVR_SetWheelModel(bool on, float x, float y, float k);	// k: its length, metres
float DoomVR_WheelModelLength();
int DoomVR_WheelGather();		// the guns carried, into the ring's list; how many
void DoomVR_WheelChoose(int i);	// take gun i
// The damage times this for a blow the weapon hand's swing just started (1 if
// none), and that swing's speed (m/s).
double DoomVR_SwingPower(double& speed);
void DoomVR_UpdateSwings();	// each frame, after the controllers are read
// Physical play, two hands on a gun: the weapon up now (a long gun?, its fit),
// from the renderer each frame; the hold itself, each frame before the buttons.
// A long gun's front grip, for two hands (the hands project's FRONT-HAND
// table, B guns only): the support point relative to the gun's grip point and
// the other hand's placement there - turn q (x y z w) and origin - all in the
// aim's frame, metres, for a right-handed player (the Leftorium mirrors them).
struct DoomVR_Support { float point[3]; float q[4]; float origin[3]; };
void DoomVR_NoteHeldGun(bool longGun, const DoomVR_WeaponTune& tune, const DoomVR_Support* support = nullptr);
bool DoomVR_TwoHandedNow();
bool DoomVR_TwoHandGunHeld();	// the other hand's grip held is the hold, not its binding	// both hands on the gun this frame
void DoomVR_TwoHands();

// Where the right controller points, relative to the player's body: yaw in
// degrees to add to the body's (positive left) and pitch in UZDoom's sense
// (positive down). False with no headset or no right controller - the weapon
// then fires along the body as in the flat game.
//
// The playsim reads this while the weapon's states run. That is client state
// inside the simulation, which is fine for one player and would desync a
// network game or a demo; neither is something DoomVR offers yet.
bool DoomVR_GetAim(double& yawOffset, double& pitch);

// The head's yaw relative to the body, degrees, positive left; false with no
// headset. "Use" (doors, switches) acts along it: you press A at what you are
// looking at.
bool DoomVR_GetLookYaw(double& yawOffset);

// Which eye the render loop is drawing, for the weapon's matrix.
void DoomVR_SetDrawingEye(int eye);

// The matrix the weapon sprites are drawn through, instead of UZDoom's flat
// HUD projection: from the sprite's screen pixels to this eye's clip space,
// by way of the right controller. The layout is undone first - (cx, cy) is
// the view window's centre and (sx, sy) its pixels per sprite unit
// horizontally and vertically, the same factors UZDoom laid the sprite out
// with - so where the weapon sits never depends on the window's size or
// shape. With no right controller the weapon is held at a fixed place below
// and in front of the head. False with no headset.
// One weapon's fit in the hand, set with the in-headset tuner (vr_tune,
// doomvr_tune.cpp) and added to the measured placement: metres along the
// hand's right, up and forward, degrees of tilt (barrel up), turn (barrel
// right) and roll (clockwise), and a size factor. In the physical left hand
// it is mirrored, as the weapon is.
struct DoomVR_WeaponTune
{
	float right = 0, up = 0, forward = 0;
	float pitch = 0, yaw = 0, roll = 0;
	float scale = 1;
};

// The tune of the weapon about to be drawn (a model; the sprites pass theirs
// to DoomVR_WeaponMatrix). Set before each HUD model, reset after.
void DoomVR_SetDrawingTune(const DoomVR_WeaponTune& tune);

// Game side (doomvr_tune.cpp): whether the tuner is on, the tune of a weapon
// class, and the right stick and clicks while it is.
// A weapon drawn as a flat sprite has its own fit, apart from its 3D model's
// (Classic and Modern VR draw the same class both ways); the renderer notes
// how each hand's weapon was last drawn so the tuner edits that one.
bool DoomVR_TuneActive();
const DoomVR_WeaponTune& DoomVR_GetTune(FName weaponClass, bool sprite = false);
FName DoomVR_StandsFor(FName weaponClass);	// the Doom weapon a mod's weapon stands for (doomvr_tune.cpp), or NAME_None
const DoomVR_WeaponTune* DoomVR_GetHandTune(FName weaponClass, int which);	// 0 our gun hand, 1 the other on the front grip
bool DoomVR_TuneSupportShown();	// the tuner is moving the other hand: show it on the front grip
void DoomVR_NoteWeaponDrawnAs(int hand, bool model);
void DoomVR_TuneInput(float stickX, float stickY, bool modeClick, bool handClick, double seconds);
// The keyboard while tuning (arrows, PgUp/PgDn, Home, Insert, End): true when
// the tuner took the key, which then goes nowhere else.
struct event_t;
bool DoomVR_TuneKey(const event_t* ev);

// handIndex 1 is the left hand, for an off-hand weapon's sprites: its card is
// placed at the left controller and mirrored. tune, if given, is that
// weapon's fit (position, roll and size - a card faces the eye, so tilt and
// turn do not apply to it).
bool DoomVR_WeaponMatrix(float cx, float cy, float sx, float sy, float out[16], int handIndex = 0, const DoomVR_WeaponTune* tune = nullptr);

// The 2D - menus, console, HUD, title and intermission screens - goes on a
// flat panel in front of the player instead of into the eyes. BeginPanel
// places it (a big screen in front of the head when a menu, the console or a
// non-level screen appears; the HUD, see-through, ahead and following the
// head lazily otherwise), binds its texture and gives its size.
// False with no headset or no panel: draw the 2D the old way. The caller
// clears it to transparent: only the HUD itself should hide the world.
// The canvas size is the 2D drawer's, which gives the panel its shape.
// The Game Tome open: a key the player typed (the raw keyboard's virtual key and
// scan code) that the book takes is handed to it and kept from the game - true.
bool DoomVR_TomeKey(unsigned vk, unsigned scan, bool down);
// The same for the window's own key messages (the menus read those): true while
// the book has the key, so the menu under the book does not see it.
bool DoomVR_TomeOwnsKey(unsigned message, unsigned long long wparam);
bool DoomVR_BeginPanel(int canvasWidth, int canvasHeight, int& width, int& height);
void DoomVR_EndPanel();

// Photo mode: with the
// game paused, a click of the LEFT THUMBSTICK hides the pause menu, HUD and
// console, leaving the frozen, head-tracked world for the player to look
// round and take their own screenshot; another click brings the menu back.
// It hides; it does not unpause.
bool DoomVR_PhotoMode();

// A screenshot of what the headset shows was asked for (vr_screenshot).
// Returns true once per request: the present captures the left eye on that
// frame and hands it to UZDoom's own screenshot.
bool DoomVR_TakeScreenshotRequest();

// The spectator view (vr_spectator): the cut-out of the left eye to draw in a
// window outW x outH, as four corners (fractions of the eye picture, y down;
// the window's top-left, top-right, bottom-right, bottom-left). False when the
// window should show the eye the old way.
bool DoomVR_SpectatorCrop(int outW, int outH, float corners[8]);
// A folder asked for by vr_dump_spectator, once; null when none was asked for.
const char* DoomVR_TakeSpectatorDumpRequest();

// Present: bind this eye's swapchain image as the target, then release it.
void DoomVR_BeginEye(int eye);
void DoomVR_EndEye(int eye);

// Submit the frame. Always paired with DoomVR_BeginFrame, drawn or not.
void DoomVR_EndFrame();
