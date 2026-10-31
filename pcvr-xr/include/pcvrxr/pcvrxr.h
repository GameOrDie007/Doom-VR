// pcvr-xr - the OpenXR layer these ports keep rewriting.
//
// Copyright (c) 2026 Game Or Die
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WHAT THIS IS
//
// Every PCVR port of a flat game needs the same thing underneath it: an OpenXR
// instance, a session bound to the game's graphics context, per-eye swapchains,
// a frame loop that begins and ends every frame, reference spaces that survive a
// runtime with no play boundary, controller bindings for the headsets people
// actually own, and a way to see what the eyes are being shown when nobody is
// wearing the headset. That layer had been written from scratch in each of
// our ports, and inherited by none of them. This is it written once.
//
// WHAT IT IS NOT
//
// It knows nothing about any engine. It does not own the window, the GL context,
// the game loop or the camera. The port keeps all of that and hands over two
// things: a graphics binding, and a callback that draws one eye. Everything a
// port must decide for itself - where the player is, what a controller button
// means in that game, how the HUD is placed - stays in the port, because that is
// where the game's knowledge lives.
//
// THE SEAM
//
//     Session s;
//     s.BringUp(cfg);                     // once, after the GL context exists
//     while (running) {
//         if (s.BeginFrame()) {           // false = nothing to draw this frame
//             for (int eye = 0; eye < s.EyeCount(); ++eye) {
//                 s.BeginEye(eye);        // binds that eye's framebuffer
//                 DrawWorld(s.GetView(eye));  // the port's renderer
//                 s.EndEye(eye);
//             }
//         }
//         s.EndFrame();                   // ALWAYS, even when nothing was drawn
//     }
//     s.Shutdown();                       // before the GL context goes
//
// The rules that are not obvious, each of which cost a day somewhere:
//
//   - Every frame that was begun must be ended. An early return between
//     BeginFrame and EndFrame wedges the runtime with no error.
//   - All of this belongs on the thread that owns the GL context. A session
//     bound to a context does not survive being driven from another thread; it
//     does not fail, it simply never returns.
//   - The session must be destroyed before the graphics context, and before the
//     process exits, or the runtime keeps threads alive and the process outlives
//     its own window.
//   - An engine restart cannot rebuild a session. Restart the process.
//   - The pose handed to the compositor must be the pose the frame was drawn
//     from, not a fresher one, or reprojection adds motion that was never there.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pcvrxr {

// ---------------------------------------------------------------------------
// Plain data, so a port never has to include openxr.h to talk to this.
// ---------------------------------------------------------------------------

struct Vec2 { float x = 0, y = 0; };
struct Vec3 { float x = 0, y = 0, z = 0; };
struct Quat { float x = 0, y = 0, z = 0, w = 1; };

struct Pose {
    Vec3 position;
    Quat orientation;
    // False when the runtime has not located this yet. A pose that is not
    // tracked is not a pose: the runtime hands back an identity, and taking
    // that as truth puts the player at the origin facing -Z.
    bool tracked = false;
};

// Half-angles in radians, as OpenXR reports them: left and down are negative.
// Asymmetric per eye on nearly every headset, so never average them and never
// reduce them to a single "FOV" number - that is what makes a flat game's
// camera code the wrong shape for VR.
struct Fov { float left = 0, right = 0, up = 0, down = 0; };

struct View {
    Pose pose;      // in the reference space, at the predicted display time
    Fov  fov;
    int  width = 0; // the swapchain image this eye draws into
    int  height = 0;
};

enum class Space {
    Seated,  // eye level at the recentre point; always available
    Standing // floor level, if the runtime has a boundary; falls back to Seated
};

// Which graphics API the session is bound to. OpenGL is the default and what
// every engine port so far has used. D3D11 is for engines that render with it
// (Unity's default on Windows): the port renders each eye into a texture of
// its own and hands it over with SubmitEyeTexture - there is no framebuffer to
// bind, so BeginEye/EndEye and BeginPanel/EndPanel do nothing under D3D11.
enum class Graphics { OpenGL, D3D11 };

struct Config {
    std::string appName = "pcvr-xr";
    Graphics graphics = Graphics::OpenGL;
    // D3D11 only: the ID3D11Device the game renders with. The session is bound
    // to it, and every texture submitted must belong to it.
    void* d3d11Device = nullptr;
    // 0 takes the runtime's recommendation. Anything else multiplies it, which
    // is how a port offers supersampling. A Quest-era constant is not a figure
    // for a PC eye: derive from what the runtime asks for.
    float resolutionScale = 0.0f;
    Space space = Space::Standing;
    // Render into a target of the layer's own and copy it into the swapchain
    // image, instead of rendering straight into the swapchain image. It costs
    // one full-eye copy per eye per frame - about 1.4 GB a second at 1852x2056
    // and 90 Hz - and is where MSAA will live when it exists. Off by default.
    bool renderToOwnTarget = false;
    // Extra OpenXR extensions this port wants. They are enumerated first and
    // only the ones the runtime actually has are requested, because ONE
    // unknown name fails xrCreateInstance outright and takes all of VR with
    // it. The log says which were found and which were not.
    std::vector<std::string> extraExtensions;
    // An explicit OpenXR API layer to run between the game and the runtime
    // (Game Or Die's Game Tome book): the folder holding its .json and .dll,
    // and its name. It is enabled only when the loader finds it there, and
    // if the instance cannot be made with it, it is made without - a missing
    // or broken extra never costs the game its headset.
    std::string apiLayerDir;
    std::string apiLayerName;
    // How far below the eye the floor is assumed to be when the runtime has no
    // boundary of its own. Seated players and streaming runtimes hit this.
    float seatedFloorDrop = 1.675f;
};

// There is deliberately no multisample setting here. MSAA is not implemented -
// it needs a multisampled framebuffer of its own and a resolve blit into the
// swapchain image - and a field that is read by nobody is worse than its
// absence, because a port sets it to 4, gets 1, and is told nothing.

// Where the layer talks. Wire it to the port's own logging; the default prints
// nothing, and a silent bring-up failure is the hardest kind to diagnose.
using LogFn = std::function<void(const char* line)>;

// ---------------------------------------------------------------------------
// Controllers
// ---------------------------------------------------------------------------
//
// The layer normalises the controllers people actually own into one vocabulary
// and binds them all. What any of it MEANS stays in the port: the layer has no
// idea whether the right trigger fires a weapon, and a layer that decided that
// would be wrong for the next game.
//
// The vocabulary is deliberately the smallest set every one of those
// controllers can express. A Vive wand has no A or B button, so on a Vive those
// two are simply never pressed - which is the honest answer, and better than a
// layer that invents a chord for them and surprises somebody.

enum class Hand { Left = 0, Right = 1 };

// Held state and both edges. The edges are computed here, per hand, every
// frame, because engines poll input - they ask "is it down?" once a frame from
// somewhere deep in a game loop, and there is nowhere to hang a callback. A
// port that has to derive its own edges will get it wrong in one place and
// spend an evening on a button that fires twice.
struct Button {
    bool down = false;      // held right now
    bool pressed = false;   // went down between the last frame and this one
    bool released = false;  // went up between the last frame and this one
    bool touched = false;   // resting on it, where the controller can tell
};

struct HandState {
    // False when that controller is off, asleep or out of range. Everything
    // below is stale when this is false - do not aim anything with it.
    bool active = false;

    // Two different poses, and using the wrong one is a bug you will see
    // rather than read about: `aim` is the ray a controller points along, for
    // pointing at things; `grip` is where a held object sits in the fist, for
    // putting a weapon in the hand. They differ by a real angle on every
    // controller, and by a lot on a Vive wand.
    Pose aim;
    Pose grip;

    // How fast the hand is moving, in the reference space. Ports need this for
    // anything thrown, swung or shaken - a melee swing is recognised by speed,
    // not by position - and it cannot be recovered from poses afterwards
    // without reinventing a filter the runtime already has.
    Vec3 linearVelocity;    // metres per second
    Vec3 angularVelocity;   // radians per second
    bool velocityValid = false;

    float trigger = 0.0f;   // 0..1
    float squeeze = 0.0f;   // 0..1, the grip
    Vec2  thumbstick;       // -1..1 each axis; the trackpad on a Vive wand

    Button triggerClick;    // the trigger as a button, with the runtime's own
                            // threshold rather than one you picked
    Button squeezeClick;
    Button thumbstickClick;
    Button primary;         // A on the right hand, X on the left
    Button secondary;       // B on the right hand, Y on the left
    Button menu;

    // The Steam Frame's gamepad buttons, one by one - only while this hand is
    // on the Frame's own profile (padLayout). A port that ignores these still
    // gets the Frame's buttons through primary / secondary / menu above, laid
    // out as SteamVR remaps a Touch controller; one that reads them can give
    // the Frame a full gamepad layout of its own.
    bool padLayout = false;
    Button padA, padB, padX, padY;                  // the right hand's face buttons
    Button dpadUp, dpadDown, dpadLeft, dpadRight;   // the left hand's D-pad
    Button shoulder;                                // LB on the left hand, RB on the right
    Button view;                                    // the left hand's View button
    Button padMenu;                                 // the right hand's Menu button
};

// A flat panel hanging in the world: a menu, a loading screen, a map, or the
// whole flat game while something is paused. It is a compositor layer of its
// own, which is why it stays sharp where the same thing drawn into the eye
// buffers goes soft.
//
// Two rules, both of which have cost a day:
//
//   - The panel IS the placement. Draw its texture FLAT - no perspective, no
//     eye offset, no head transform. The compositor puts it where the pose
//     says. Applying a camera to the texture as well places it twice.
//   - Size the quad from the shape of what the texture CONTAINS. When the
//     port draws its 2D at its own shape into a texture of that shape, that is
//     the texture's aspect, and the default does it: a square texture on a
//     quad sized for a 16:9 canvas stands about 1.8x too tall. When the port
//     STRETCHES its canvas across a texture of another shape - an eye-sized
//     texture is the usual one - the quad must take the canvas's shape
//     instead, or the stretch is shown to the player. Say so with
//     heightMetres or SetPanelShape.
struct PanelConfig {
    // Texture size. 0 takes the eye buffer's size, which is the right default
    // for a port that draws its 2D at the resolution it renders at.
    int width = 0;
    int height = 0;
    // How wide the panel is in the world.
    float widthMetres = 3.0f;
    // How tall. 0 derives it from the texture's aspect, which is right when
    // the content fills the texture at its own shape. Set it when the port
    // stretches a canvas of another shape across the texture - see the rule.
    float heightMetres = 0.0f;
};

// ---------------------------------------------------------------------------
// The two matrices every port ends up writing
// ---------------------------------------------------------------------------
//
// Column-major, ready for glLoadMatrixf or a uniform. Both are here because
// both have a sign in them that is wrong half the time when it is copied from
// somewhere else, and a wrong one does not look like a bug: a flipped frustum
// offset mirrors each eye and reads as ordinary double vision, which people
// then try to fix by adjusting the IPD.
//
// Derive, do not copy. The offset term is (tanRight + tanLeft) / (tanRight -
// tanLeft), and tanLeft is negative, so for a centred eye it is 0 and for an
// eye whose view is pushed outwards it follows the direction the eye looks.

// Perspective projection from OpenXR's asymmetric half-angles.
void ProjectionMatrix(const Fov& fov, float nearZ, float farZ, float out[16]);

// The inverse of a pose: what a renderer wants as its view matrix.
void ViewMatrix(const Pose& pose, float out[16]);

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void SetLog(LogFn log);

    // Creates instance, system, session, swapchains and spaces. The GL context
    // must already exist and be current on this thread. Returns false when
    // there is no runtime, no headset, or the runtime refused - all of which
    // are ordinary on a PC, so a port must be able to carry on flat.
    // Bring-up in two halves, because engines need them apart.
    //
    // Connect() creates the instance and finds the headset, and needs NO
    // graphics context. After it, EyeBufferSize() and RuntimeName() answer -
    // which matters, because an engine that sizes its own framebuffers from
    // the eye resolution has to know it BEFORE it opens a window, and the
    // session cannot exist before that window does. An engine that does not
    // care can ignore this and call BringUp, which does both.
    bool Connect(const Config& cfg);
    bool IsConnected() const;

    // Creates the session and everything that hangs off it. The graphics
    // context must exist and be current on this thread. Calls Connect() first
    // if it has not happened yet.
    bool BringUp(const Config& cfg);
    bool IsUp() const;

    // True once the runtime has asked the application to stop - the player
    // quit from the runtime's own menu, or the headset went away. A port that
    // ignores this spins forever rendering nothing and looks hung.
    bool ExitRequested() const;

    // The session is VISIBLE or FOCUSED: the runtime is showing this
    // application's frames to the player, so the head pose is the player's.
    // Before that a runtime may flag a placeholder pose as valid -
    // VirtualDesktopXR reports the head at exactly (0, 0, 0) while READY - and
    // a port that takes its height reference from it puts the player's eyes a
    // whole head height too high, and hangs its menus on the floor.
    bool IsVisible() const;

    // Starts at 0 and goes up by one every time the runtime moves the
    // reference space (its own recenter, or a streaming runtime settling its
    // floor): every pose from then on is measured from somewhere new. A port
    // that keeps a reference of its own (a recenter point, a panel's place)
    // must take it again when this changes. It changes before the first pose
    // in the new space is located, so that pose already carries the new
    // number.
    unsigned SpaceGeneration() const;
    // Test hook: rebuild the spaces and bump the generation, as a runtime's
    // recenter would. For proving a port re-references with no headset.
    void SimulateSpaceChange();

    // The last reason BringUp failed, for the log and for the player.
    const char* FailureReason() const;

    // Destroys everything, in the order the runtime requires. Safe to call
    // when nothing was ever brought up.
    void Shutdown();

    // ---- the frame loop ----------------------------------------------------

    // Waits for the runtime's pace, begins the frame and predicts the views.
    // Returns false when this frame should not be rendered - the session is
    // idle, or the headset is off the head. EndFrame() must still be called.
    bool BeginFrame();

    int  EyeCount() const;
    const View& GetView(int eye) const;

    // Binds that eye's swapchain image as the draw target and sets the
    // viewport to it. The port renders, then calls EndEye.
    void BeginEye(int eye);
    void EndEye(int eye);

    // D3D11 only, in place of BeginEye/EndEye: copies a finished picture of
    // that eye (an ID3D11Texture2D on the session's device, at least the eye
    // buffer's size, and of the R8G8B8A8 family) into the eye's swapchain
    // image and hands it over. The bytes are copied as they are and the
    // swapchain image is sRGB, so the texture must hold sRGB-encoded colour -
    // which is what a gamma-space renderer writes, and what a linear-space one
    // writes into an sRGB target. Call it on the thread that owns the device's
    // immediate context, between BeginFrame and EndFrame.
    bool SubmitEyeTexture(int eye, void* d3d11Texture);
    // The same for the flat panel, in place of BeginPanel/EndPanel. The
    // texture's alpha is the panel's transparency.
    bool SubmitPanelTexture(void* d3d11Texture);

    // Submits what was drawn, with the pose the frame was predicted for.
    void EndFrame();

    // ---- the split frame (D3D11 only) --------------------------------------
    //
    // For an engine that renders on a thread of its own (Unity by default):
    // the game thread waits for the headset and places its cameras, and the
    // render thread - the one that owns the D3D11 immediate context, and that
    // runs the camera renders some time later - hands the pictures over. One
    // thread cannot do both: the game thread has no business touching the
    // context, and the render thread learns the pose too late to draw with it.
    //
    //     game thread:    t = WaitFrame();   // 0 = nothing to submit
    //                     ...GetView / Head / GetHand, draw with them...
    //     render thread:  SubmitFrame(t, eye0, eye1, panel_or_null);
    //
    // Every non-zero ticket must be submitted exactly once, in order - pass
    // null eyes to submit an empty frame - or the next WaitFrame blocks for
    // ever. Each frame is submitted with the views it was waited with. The
    // BeginFrame/EndFrame loop above is untouched and still the one for
    // single-threaded engines.
    uint64_t WaitFrame();
    bool FrameWillRender(uint64_t ticket) const;
    void SubmitFrame(uint64_t ticket, void* eye0, void* eye1, void* panel);

    // ---- where the player is ----------------------------------------------

    // The head, in the reference space, from the frame that is being drawn.
    const Pose& Head() const;

    // Recentres the reference space on the current head pose, keeping the
    // floor. Ports expose this as a button; runtimes also ask for it.
    void Recentre();

    // ---- instruments -------------------------------------------------------

    // Writes both eyes to <dir>/eye0_NNN.ppm and eye1_NNN.ppm on the next
    // frame. A readback stalls the GPU for the frame, so this is a diagnostic
    // and not something to leave on.
    void RequestEyeDump(const char* dir);

    // Degrees of synthetic yaw added to the head pose. A headless runtime's
    // HMD never moves, so without this there is no way to ask "does the view
    // still follow the head" with no headset on.
    void SetDebugYaw(float degrees);
    // And a synthetic tilt of the head, after the yaw: pitchUp looks up, and
    // rollRight drops the right ear. It turns the head and every eye about the
    // head, so the pictures and the views reported with them agree, as a real
    // head's do. Zero is inert.
    void SetDebugTilt(float pitchUpDegrees, float rollRightDegrees);

    // ---- the flat panel ----------------------------------------------------

    // Creates the panel's own swapchain. Call once, after BringUp. A port that
    // never shows a panel never needs to call it.
    bool CreatePanel(const PanelConfig& cfg);

    // Binds the panel's texture as the draw target and sets the viewport to
    // it. Draw 2D, flat, then EndPanel. Between BeginFrame and EndFrame, like
    // the eyes - and it can be used with or without drawing the eyes at all,
    // which is how a port shows a menu over a paused world, or on its own.
    void BeginPanel();
    void EndPanel();

    // Do not submit the world this frame, even though eyes were drawn into.
    // Ports that show a flat screen INSTEAD of the world - a menu, a map, a
    // cutscene - still run their eye loop, because the engine's frame is one
    // shape and does not care; this says "what I drew there is not what the
    // player should see". Cleared at the start of every frame.
    void SuppressWorld();
    void PanelSize(int& width, int& height) const;

    // Where the panel hangs, in the reference space. The port decides, because
    // where a menu belongs is a thing only the game knows.
    void SetPanelPose(const Pose& pose);

    // The panel's size in the world, changeable every frame. heightMetres 0
    // derives the height from the texture's aspect. A port whose canvas can
    // change shape - a window resized, a video mode changed - reshapes the
    // quad here without rebuilding the texture.
    void SetPanelShape(float widthMetres, float heightMetres);

    // The common case, made easy: upright, facing the player, at `distance`
    // metres in front of where the head is now. Takes the head's yaw only -
    // pitch and roll would hang the menu at an angle and follow every nod.
    // Call it when the panel appears, not every frame, or it swims.
    void PlacePanelInFrontOfHead(float distance);

    // Where the panel hangs now and its size in metres (the quad the
    // compositor draws) - for pointing at it with a controller. The panel's
    // front is its +Z; its texture's top-left is at (-w/2, +h/2) in its own
    // X/Y. False before CreatePanel.
    bool PanelPlacement(Pose& pose, float& widthMetres, float& heightMetres) const;

    // ---- controllers -------------------------------------------------------

    // The state of one hand for the frame currently being drawn. Refreshed
    // inside BeginFrame, so it is safe to poll from anywhere in the frame and
    // every read within one frame agrees with every other.
    const HandState& GetHand(Hand hand) const;

    // A buzz. Duration in seconds; 0 frequency lets the runtime pick, which is
    // what you want unless there is a reason. Amplitude 0..1.
    void Haptic(Hand hand, float seconds, float amplitude, float frequency = 0.0f);

    // Which controller the runtime actually bound for this hand, as an
    // interaction profile path. This is the first thing to look at when a
    // button does nothing: if the profile is not the one you thought, your
    // bindings for it were never used.
    const char* InteractionProfile(Hand hand) const;

    // Left-handed play: with it on, everything the port asks of Hand::Right -
    // pose, buttons, sticks, trigger, haptics, profile - comes from the LEFT
    // controller, and Hand::Left from the right: a mirror image of the
    // controls, with no change in the port. SetFakeHand and the test hands
    // stay physical (they stand in for real controllers). Off by default.
    void SetSwapHands(bool on);
    bool SwapHands() const;

    // Feed made-up controller state instead of the runtime's, for testing with
    // no controllers in the room. It runs through exactly the same edge
    // detection and the same port-facing state as a real controller, so it can
    // answer "would a press arrive correctly" on a desk - which is the one
    // question a headless run otherwise cannot ask at all.
    //
    // Inert unless SetFakeHands(true) has been called. A debug driver that is
    // not completely inert when switched off will eventually be found holding
    // somebody's trigger down.
    void SetFakeHands(bool on);
    bool FakeHands() const;
    void SetFakeHand(Hand hand, const HandState& state);

    // Measuring only: the right hand held at PCVRXR_TEST_RIGHT_HAND from the
    // head (see pcvrxr.cpp). Called by the layer itself each frame.
    void ApplyTestHand();

    // The graphics texture names the runtime allocated for one eye's
    // swapchain, written into `out`; returns how many there are.
    //
    // A port whose engine assigns its OWN texture names instead of asking the
    // driver for them - GoldSrc and its descendants use the texture's array
    // index as its GL name - will eventually bind one of these and upload over
    // it. The framebuffer still reports COMPLETE, because an attachment is
    // still there; it is simply a 256x256 game texture now, so every write is
    // clipped away and the compositor is shown whatever the engine last
    // uploaded. Such a port must reserve these names.
    int SwapchainTextures(int eye, uint32_t* out, int max) const;

    // ---- the way out -------------------------------------------------------

    // The handles underneath, for the one thing a port needs that this layer
    // does not wrap. Hiding OpenXR is the point, but a layer with no escape
    // hatch at all is worse: it turns one missing wrapper into a fork.
    //
    // Everything is uint64_t deliberately. OpenXR's non-dispatchable handles
    // (XrSpace, XrSwapchain, XrAction) are uint64_t on 32-bit and pointers on
    // 64-bit, so a void* here would be wrong on exactly the architecture the
    // GoldSrc ports need. Cast to the real type at the call site.
    struct Native {
        uint64_t instance = 0;   // XrInstance
        uint64_t session = 0;    // XrSession
        uint64_t refSpace = 0;   // XrSpace, what poses are reported in
        uint64_t viewSpace = 0;  // XrSpace, the head
        int64_t  displayTime = 0;// XrTime the current frame is predicted for
    };
    Native GetNative() const;

    // The headset's refresh rate, and a request to change it. Both need the
    // runtime to support XR_FB_display_refresh_rate; without it the getter
    // returns 0 and the setter returns false, rather than pretending.
    float RefreshRate() const;
    bool  SetRefreshRate(float hz);

    // ---- what the runtime is -----------------------------------------------

    // What the runtime calls itself, and the eye buffer size, for the log and
    // for bug reports. Empty until BringUp succeeds.
    const char* RuntimeName() const;
    void        EyeBufferSize(int& width, int& height) const;

private:
    struct Impl;
    Impl* m;
};

} // namespace pcvrxr
