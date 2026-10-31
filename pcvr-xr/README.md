# pcvr-xr

The OpenXR layer that sits under a PCVR port of a flat game, written once
instead of once per port.

GPL-3.0-or-later. No third-party VR code.

## Using it

```cmake
add_subdirectory(pcvr-xr)
target_link_libraries(yourgame PRIVATE pcvrxr)
```

```cpp
#include "pcvrxr/pcvrxr.h"

pcvrxr::Session vr;
vr.SetLog([](const char* line) { Printf("%s\n", line); });

pcvrxr::Config cfg;
cfg.appName = "Your Game VR";
if (!vr.BringUp(cfg)) {
    Printf("no VR this run: %s\n", vr.FailureReason());   // carry on flat
}

// ... once per frame, on the thread that owns the GL context:
if (vr.BeginFrame()) {
    for (int eye = 0; eye < vr.EyeCount(); ++eye) {
        vr.BeginEye(eye);
        DrawWorld(vr.GetView(eye));
        vr.EndEye(eye);
    }
}
vr.EndFrame();           // ALWAYS, even when BeginFrame returned false

// ... at shutdown, before the GL context goes:
vr.Shutdown();
```

`GetView(eye)` hands back the pose and the asymmetric half-angles for that eye.
`ProjectionMatrix()` and `ViewMatrix()` turn those into the two matrices a
renderer wants, so no port has to re-derive the frustum offset sign.

Controllers are one vocabulary across five profiles - any OpenXR controller,
Meta Touch, Valve Index, HTC Vive wand and Windows Mixed Reality - with the
edges computed for you, because engines poll:

```cpp
const pcvrxr::HandState& right = vr.GetHand(pcvrxr::Hand::Right);
if (right.triggerClick.pressed) Fire();          // once, on the way down
if (right.primary.pressed) Reload();
Move(right.thumbstick.x, right.thumbstick.y);
PlaceWeapon(right.grip);                         // grip, not aim, for a held object
if (hitSomething) vr.Haptic(pcvrxr::Hand::Right, 0.05f, 0.6f);
```

What any of it means stays in the port. The layer does not know that the
trigger fires a weapon, and a layer that decided so would be wrong for the
next game.

Menus and 2D go on a flat panel, which is a compositor layer of its own and so
stays sharp where the same thing drawn into the eye buffers goes soft:

```cpp
pcvrxr::PanelConfig pc;
pc.width = 1280; pc.height = 720; pc.widthMetres = 3.0f;
vr.CreatePanel(pc);

vr.PlacePanelInFrontOfHead(2.5f);   // when it appears, not every frame
vr.BeginPanel();
DrawMenu();                          // FLAT - no camera, no eye offset
vr.EndPanel();
```

The panel's height comes from the texture's aspect, never from the game's
canvas: a square texture gives a square quad. Get that backwards and a 2D
screen on a square texture stands about 1.8x too tall.

## Building and proving it

Needs MSVC and CMake. The OpenXR loader is vendored in `extern/openxr`, so
there is nothing to fetch.

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

**32-bit matters here.** GoldSrc, Xash3D and anything that loads x86 game DLLs
is a 32-bit process, so the loader is vendored for both and CMake picks by
architecture:

```
cmake -S . -B build-x86 -A Win32
cmake --build build-x86 --config Release
powershell -ExecutionPolicy Bypass -File tools\headless.ps1 -Arch x86 -Seated
```

The runtime manifest is per-architecture too; pointing a 32-bit process at the
64-bit one fails inside the loader with nothing useful said about why. The
harness picks the right one from `-Arch`.

That produces `pcvrxr.lib` and `probe.exe`. The probe is the layer's proof: no
engine, no game data, no headset. It brings up a session, draws a scene whose
geometry is known in advance into each eye, submits it, dumps both eyes and
shuts down.

With a headset connected, just run it:

```
build\Release\probe.exe --dump out
python tools\checkdump.py out
```

With no headset, on SteamVR's synthetic HMD:

```
powershell -ExecutionPolicy Bypass -File tools\headless.ps1 -Seated
python tools\checkdump.py out
```

And for controllers, which a headless run otherwise cannot say anything about:

```
powershell -ExecutionPolicy Bypass -File tools\headless.ps1 -Seated -TestInput
```

That reports how many of the five profiles the runtime accepted - a mistyped
binding path is the usual cause of "that button does nothing on my headset",
and it is rejected silently - and then drives made-up controller state through
the same edge detection a real controller uses, checking each frame against an
answer worked out in advance.

`checkdump.py` does not summarise the images. Every check looks for something
the probe drew at a place it can be predicted:

```
eye 0  coverage  ok  top 1.00 bottom 1.00 left 1.00 right 1.00
eye 1  marker    ok  expected magenta, found magenta (cyan 0, magenta 441)
stereo    ok    disparity 29.0 px, which at 2 m is an eye separation of about 63 mm
RESULT    PASS
```

The stereo line is the useful one: the eye separation is reconstructed from
pixel positions, so it agrees with what the runtime reported only if the whole
chain - pose, frustum offset, viewport, submission - is right.

`tools\headless.ps1` changes `steamvr.vrsettings` and puts it back, verified by
hash. If it ever reports that the restore failed, put the `.qzdbak` file back
by hand before using a headset: a left-over `forcedDriver=null` makes SteamVR
ignore a real one.

## Under a real engine

It carries Half-Life PCVR and Gunman Chronicles on Xash3D at x86: both games
load, both eyes are submitted, input reaches the engine, and neither crashes
where the layer it replaced crashes in both. See `docs/gunman.md` for the seam
and `docs/verification.md` for what was measured.

One thing a port on an older engine must do: **reserve the runtime's texture
names**. `SwapchainTextures()` reports them. An engine that assigns its own GL
texture names - GoldSrc and its descendants use a texture's array index - will
otherwise upload over a swapchain image, and the failure is silent in every way
that matters.

## What is here

```
include/pcvrxr/pcvrxr.h   the whole public API, and why each rule exists
src/pcvrxr.cpp            instance, session, swapchains, spaces, frame loop
src/pcvrxr_input.cpp      actions, bindings for five controllers, haptics
src/pcvrxr_panel.cpp      the flat quad layer for menus and 2D
src/internal.h            what those two share; not a public header
probe/probe.cpp           the test app described above
tools/headless.ps1        run the probe with no headset
tools/checkdump.py        read the eye dumps and say what they prove
tools/steamvr_null.py     switch SteamVR's synthetic HMD on and off again
extern/openxr             OpenXR SDK 1.0.25 headers, loader for x86 and x64
```

## What it does not do

It knows nothing about any engine. It does not own the window, the graphics
context, the game loop or the camera. Where the player is, what a controller
button means in a particular game, and how a HUD is placed all stay in the
port, because that is where the game's knowledge lives.

Hand tracking without controllers, eye tracking and passthrough are not here.
Neither is a cylinder layer - SteamVR does not implement one, so a port that
wants a curved screen needs a quad fallback anyway.
Neither is anything that needs a runtime extension beyond OpenGL.
