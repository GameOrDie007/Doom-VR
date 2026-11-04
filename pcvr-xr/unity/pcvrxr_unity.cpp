// pcvr-xr for Unity - a flat C ABI over pcvrxr::Session, for a C# mod.
//
// Copyright (c) 2026 Game Or Die
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WHAT THIS IS
//
// A Unity game renders with Direct3D 11 and is driven from C#. This DLL is the
// whole of the VR layer as C# sees it: a handful of functions taking plain
// floats and ints, so the C# side needs nothing but [DllImport].
//
// Everything crossing this boundary is in UNITY'S coordinates, converted here
// once: Unity is left-handed (+Z forward), OpenXR right-handed (-Z forward).
// A position flips Z; a rotation flips the X and Y of its quaternion. Doing it
// here, in one place, means the C# never sees an OpenXR pose.
//
// THREADING: every call must come from the thread that owns the game's D3D11
// immediate context. With Unity that means single-threaded rendering
// (-force-gfx-direct), where it is the main thread. The C# checks for that and
// says so in the log when it is not.

#include "pcvrxr/pcvrxr.h"
#include "pcvrxr/spectator.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <cstdio>
#include <cstdarg>
#include <string>
#include <mutex>

using namespace pcvrxr;

namespace {

Session* g_session = nullptr;
FILE* g_log = nullptr;
std::mutex g_logLock;
std::string g_failure;

void LogLine(const char* line);

// A watchdog: every call records its name and start time, and a thread says
// in the log when one has been inside the runtime for more than two seconds.
// A hung frame loop otherwise leaves a log that simply stops.
volatile const char* g_call = nullptr;
volatile DWORD g_callStart = 0;
volatile bool g_watchStarted = false;
struct CallScope {
    CallScope(const char* n) { g_callStart = GetTickCount(); g_call = n; }
    ~CallScope() { g_call = nullptr; }
};
DWORD WINAPI Watchdog(void*) {
    const char* reported = nullptr;
    DWORD reportedAt = 0;
    for (;;) {
        Sleep(500);
        const char* c = (const char*)g_call;
        const DWORD start = g_callStart;
        if (c != nullptr && GetTickCount() - start > 2000 && !(c == reported && start == reportedAt)) {
            char line[256];
            snprintf(line, sizeof(line), "WATCHDOG: %s has not returned after %lu ms", c, (unsigned long)(GetTickCount() - start));
            LogLine(line);
            reported = c;
            reportedAt = start;
        }
    }
}

void LogLine(const char* line) {
    std::lock_guard<std::mutex> lk(g_logLock);
    if (g_log == nullptr) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line);
    fflush(g_log);
}

void StartWatchdog() {
    if (g_watchStarted) return;
    g_watchStarted = true;
    CloseHandle(CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr));
}

void ToUnity(const Pose& p, float* out) {
    out[0] = p.position.x;
    out[1] = p.position.y;
    out[2] = -p.position.z;
    out[3] = -p.orientation.x;
    out[4] = -p.orientation.y;
    out[5] = p.orientation.z;
    out[6] = p.orientation.w;
}

Pose FromUnity(const float* in) {
    Pose p;
    p.position = { in[0], in[1], -in[2] };
    p.orientation = { -in[3], -in[4], in[5], in[6] };
    p.tracked = true;
    return p;
}

int Bits(const HandState& h, bool Button::*field) {
    int b = 0;
    if (h.triggerClick.*field)    b |= 1;
    if (h.squeezeClick.*field)    b |= 2;
    if (h.thumbstickClick.*field) b |= 4;
    if (h.primary.*field)         b |= 8;
    if (h.secondary.*field)       b |= 16;
    if (h.menu.*field)            b |= 32;
    return b;
}

} // namespace

// The layout the C# mirrors with [StructLayout(LayoutKind.Sequential)].
struct PXUHand {
    int   active;
    float aim[7];
    float grip[7];
    float trigger, squeeze, stickX, stickY;
    int   down;       // bit 0 trigger, 1 grip, 2 stick click, 3 A/X, 4 B/Y, 5 menu
    int   pressed;
    int   released;
    int   velocityValid;
    float linearVelocity[3];
    float angularVelocity[3];
};

#define PXU_API extern "C" __declspec(dllexport)

PXU_API void PXU_OpenLog(const char* path) {
    std::lock_guard<std::mutex> lk(g_logLock);
    if (g_log != nullptr) return;
    g_log = fopen(path, "w");
    StartWatchdog();
}

PXU_API void PXU_Log(const char* line) { LogLine(line); }

// anyTexture: any ID3D11Texture2D the game made (a RenderTexture's native
// pointer) - the session is bound to the device it belongs to.
PXU_API int PXU_Init(void* anyTexture, float resolutionScale, int seated, const char* appName) {
    CallScope scope("PXU_Init");
    if (g_session != nullptr && g_session->IsUp()) return 1;
    if (anyTexture == nullptr) { g_failure = "no texture to find the device from"; return 0; }
    ID3D11Device* device = nullptr;
    ((ID3D11Texture2D*)anyTexture)->GetDevice(&device);
    if (device == nullptr) { g_failure = "the texture has no D3D11 device"; LogLine(g_failure.c_str()); return 0; }

    if (g_session == nullptr) {
        g_session = new Session();
        g_session->SetLog([](const char* line) { LogLine(line); });
    }
    Config cfg;
    cfg.appName = appName ? appName : "pcvr-xr unity";
    cfg.graphics = Graphics::D3D11;
    cfg.d3d11Device = device;
    cfg.resolutionScale = resolutionScale;
    cfg.space = seated ? Space::Seated : Space::Standing;
    const bool ok = g_session->BringUp(cfg);
    // GetDevice added a reference; the session holds the raw pointer and the
    // game keeps the device alive for as long as it runs.
    device->Release();
    if (!ok) {
        g_failure = g_session->FailureReason();
        return 0;
    }
    return 1;
}

PXU_API const char* PXU_Failure() { return g_failure.c_str(); }
PXU_API const char* PXU_RuntimeName() { return g_session ? g_session->RuntimeName() : ""; }
PXU_API const char* PXU_Profile(int hand) {
    return g_session ? g_session->InteractionProfile(hand ? Hand::Right : Hand::Left) : "";
}

PXU_API void PXU_EyeSize(int* w, int* h) {
    *w = 0; *h = 0;
    if (g_session) g_session->EyeBufferSize(*w, *h);
}

PXU_API int PXU_BeginFrame() {
    CallScope scope("PXU_BeginFrame");
    if (g_session == nullptr || !g_session->IsUp()) return 0;
    return g_session->BeginFrame() ? 1 : 0;
}

// out: position (3), rotation (4), then the four FOV half-angles in radians
// (left, right, up, down - left and down negative).
PXU_API void PXU_GetView(int eye, float* out) {
    if (g_session == nullptr) return;
    const View& v = g_session->GetView(eye);
    ToUnity(v.pose, out);
    out[7] = v.fov.left;
    out[8] = v.fov.right;
    out[9] = v.fov.up;
    out[10] = v.fov.down;
}

PXU_API int PXU_GetHead(float* out) {
    if (g_session == nullptr) return 0;
    const Pose& p = g_session->Head();
    ToUnity(p, out);
    return p.tracked ? 1 : 0;
}

PXU_API int PXU_GetHand(int hand, PXUHand* out) {
    if (g_session == nullptr || out == nullptr) return 0;
    const HandState& h = g_session->GetHand(hand ? Hand::Right : Hand::Left);
    out->active = h.active ? 1 : 0;
    ToUnity(h.aim, out->aim);
    ToUnity(h.grip, out->grip);
    out->trigger = h.trigger;
    out->squeeze = h.squeeze;
    out->stickX = h.thumbstick.x;
    out->stickY = h.thumbstick.y;
    out->down = Bits(h, &Button::down);
    out->pressed = Bits(h, &Button::pressed);
    out->released = Bits(h, &Button::released);
    out->velocityValid = h.velocityValid ? 1 : 0;
    out->linearVelocity[0] = h.linearVelocity.x;
    out->linearVelocity[1] = h.linearVelocity.y;
    out->linearVelocity[2] = -h.linearVelocity.z;
    out->angularVelocity[0] = -h.angularVelocity.x;
    out->angularVelocity[1] = -h.angularVelocity.y;
    out->angularVelocity[2] = h.angularVelocity.z;
    return out->active;
}

PXU_API int PXU_SubmitEye(int eye, void* texture) {
    CallScope scope("PXU_SubmitEye");
    return g_session && g_session->SubmitEyeTexture(eye, texture) ? 1 : 0;
}

PXU_API int PXU_CreatePanel(int width, int height, float widthMetres, float heightMetres) {
    CallScope scope("PXU_CreatePanel");
    if (g_session == nullptr) return 0;
    PanelConfig pc;
    pc.width = width;
    pc.height = height;
    pc.widthMetres = widthMetres;
    pc.heightMetres = heightMetres;
    return g_session->CreatePanel(pc) ? 1 : 0;
}

PXU_API int PXU_SubmitPanel(void* texture) {
    CallScope scope("PXU_SubmitPanel");
    return g_session && g_session->SubmitPanelTexture(texture) ? 1 : 0;
}

PXU_API void PXU_SetPanelPose(const float* pose) {
    if (g_session) g_session->SetPanelPose(FromUnity(pose));
}

PXU_API void PXU_SetPanelShape(float widthMetres, float heightMetres) {
    if (g_session) g_session->SetPanelShape(widthMetres, heightMetres);
}

// The observer view: the steady cut-out of an eye for the monitor (pcvr-xr's
// Spectator, from the view this frame was drawn with). out: the four corners
// (top-left, top-right, bottom-right, bottom-left) as fractions of the eye
// picture, x right and y DOWN, then the roll in degrees. Returns 1 when steady.
// The cut-out is capped so it keeps about 8 degrees of room around the eye's
// forward axis: a headset's eye is lopsided (a Quest 3's left eye looks 54
// degrees out and 40 in), and a wider cut-out is held at the edge, where a
// wobble comes straight through.
static Spectator g_spect;
PXU_API int PXU_SpectatorCrop(int eye, float outAspect, double seconds, int plain, float* out) {
    if (g_session == nullptr || out == nullptr) return 0;
    const View& v = g_session->GetView(eye);
    int w = 0, h = 0;
    g_session->EyeBufferSize(w, h);
    const float tl = tanf(v.fov.left), tr = tanf(v.fov.right);
    const float room = (-tl < tr ? -tl : tr) - tanf(8.0f * 3.14159265f / 180.0f);
    float frac = (tr - tl) > 0.01f ? 2.0f * room / (tr - tl) : 0.78f;
    if (frac > 0.78f) frac = 0.78f;
    if (frac < 0.4f) frac = 0.4f;
    g_spect.widthFraction = frac;
    const SpectatorCrop c = g_spect.Update(v, w, h, outAspect, seconds, plain != 0);
    for (int i = 0; i < 4; ++i) { out[i * 2] = c.corner[i].x; out[i * 2 + 1] = c.corner[i].y; }
    out[8] = c.rollDeg;
    return c.steady ? 1 : 0;
}

PXU_API void PXU_EndFrame() { CallScope scope("PXU_EndFrame"); if (g_session) g_session->EndFrame(); }
PXU_API void PXU_Recenter() { if (g_session) g_session->Recentre(); }
PXU_API void PXU_Haptic(int hand, float seconds, float amplitude) {
    if (g_session) g_session->Haptic(hand ? Hand::Right : Hand::Left, seconds, amplitude);
}
PXU_API void PXU_SetSwapHands(int on) { if (g_session) g_session->SetSwapHands(on != 0); }
PXU_API int PXU_ExitRequested() { return g_session && g_session->ExitRequested() ? 1 : 0; }
PXU_API int PXU_IsVisible() { return g_session && g_session->IsVisible() ? 1 : 0; }
PXU_API unsigned PXU_SpaceGeneration() { return g_session ? g_session->SpaceGeneration() : 0; }
PXU_API void PXU_RequestDump(const char* dir) { if (g_session) g_session->RequestEyeDump(dir); }
PXU_API float PXU_RefreshRate() { return g_session ? g_session->RefreshRate() : 0.0f; }

// Which thread Unity renders on. GL.IssuePluginEvent(PXU_RenderEventFunc(), n)
// runs the callback on Unity's render thread; with single-threaded rendering
// that is the main thread. Compare PXU_RenderThreadId() with PXU_ThreadId()
// called from C# on the main thread. Works on every Unity version.
static volatile DWORD g_renderThread = 0;
static void __stdcall RenderEvent(int) { g_renderThread = GetCurrentThreadId(); }
PXU_API void* PXU_RenderEventFunc() { return (void*)&RenderEvent; }
PXU_API unsigned PXU_RenderThreadId() { return g_renderThread; }
PXU_API unsigned PXU_ThreadId() { return GetCurrentThreadId(); }

// ---- bring-up on the render thread ------------------------------------------
//
// Creating the session and its swapchains makes the runtime use the game's
// D3D11 device and immediate context. With Unity's render thread running, doing
// that from the main thread races the render thread for the context - it hung
// once creating the panel. So: PXU_SetInitParams, then
// GL.IssuePluginEvent(PXU_InitEventFunc(), 0), and the main thread polls
// PXU_InitState() (0 pending, 1 up, -1 failed). With single-threaded rendering
// the event runs straight away on the main thread.
static struct { void* tex; float scale; int seated; std::string app; int pw, ph; float pwm, phm; } g_initParams;
static volatile int g_initState = 0;
static volatile int g_panelOk = 0;

PXU_API int PXU_Init(void* anyTexture, float resolutionScale, int seated, const char* appName);
PXU_API int PXU_CreatePanel(int width, int height, float widthMetres, float heightMetres);

PXU_API void PXU_SetInitParams(void* anyTexture, float resolutionScale, int seated, const char* appName,
                               int panelW, int panelH, float panelWidthMetres, float panelHeightMetres) {
    g_initParams.tex = anyTexture;
    g_initParams.scale = resolutionScale;
    g_initParams.seated = seated;
    g_initParams.app = appName ? appName : "";
    g_initParams.pw = panelW; g_initParams.ph = panelH;
    g_initParams.pwm = panelWidthMetres; g_initParams.phm = panelHeightMetres;
    g_initState = 0;
}

static void __stdcall InitEvent(int) {
    if (g_initState != 0) return;
    const int ok = PXU_Init(g_initParams.tex, g_initParams.scale, g_initParams.seated, g_initParams.app.c_str());
    if (ok && g_initParams.pw > 0) g_panelOk = PXU_CreatePanel(g_initParams.pw, g_initParams.ph, g_initParams.pwm, g_initParams.phm);
    g_initState = ok ? 1 : -1;
}
PXU_API void* PXU_InitEventFunc() { return (void*)&InitEvent; }
PXU_API int PXU_InitState() { return g_initState; }
PXU_API int PXU_PanelOk() { return g_panelOk; }

// ---- the split frame: wait on the game thread, submit on the render thread --
//
// PXU_WaitFrame on the main thread returns a ticket (0 = nothing to submit).
// PXU_QueueSubmit(ticket, eyes, panel) records what to hand over and returns a
// slot; GL.IssuePluginEvent(PXU_SubmitEventFunc(), slot) then runs the submit
// on Unity's render thread, in order after the camera renders that drew the
// textures. With single-threaded rendering the render thread is the main
// thread and the same path simply runs there.

static std::mutex g_frameLock;   // a submit and the shutdown never overlap
struct PendingSubmit { unsigned long long ticket; void* eye0; void* eye1; void* panel; };
static PendingSubmit g_pending[16];
static int g_pendingNext = 0;

PXU_API unsigned long long PXU_WaitFrame(int* shouldRender) {
    CallScope scope("PXU_WaitFrame");
    *shouldRender = 0;
    if (g_session == nullptr || !g_session->IsUp()) return 0;
    const uint64_t t = g_session->WaitFrame();
    *shouldRender = t != 0 && g_session->FrameWillRender(t) ? 1 : 0;
    return t;
}

PXU_API int PXU_QueueSubmit(unsigned long long ticket, void* eye0, void* eye1, void* panel) {
    const int slot = g_pendingNext;
    g_pendingNext = (g_pendingNext + 1) % 16;
    g_pending[slot] = { ticket, eye0, eye1, panel };
    return slot;
}

static volatile bool g_stopping = false;
static volatile bool g_shutdownDone = false;

static void __stdcall SubmitEvent(int slot) {
    if (slot < 0 || slot >= 16 || g_stopping) return;
    std::lock_guard<std::mutex> lk(g_frameLock);
    const PendingSubmit p = g_pending[slot];
    if (g_session == nullptr || p.ticket == 0) return;
    CallScope scope("SubmitFrame (render thread)");
    g_session->SubmitFrame(p.ticket, p.eye0, p.eye1, p.panel);
}

PXU_API void* PXU_SubmitEventFunc() { return (void*)&SubmitEvent; }

static void ShutdownNow() {
    std::lock_guard<std::mutex> lk(g_frameLock);
    CallScope scope("PXU_Shutdown");
    if (g_session != nullptr) {
        g_session->Shutdown();
        delete g_session;
        g_session = nullptr;
    }
    LogLine("pcvr-xr unity: shut down");
    g_shutdownDone = true;
}

// Shutting down while Unity's render thread may still be inside a submit:
// the shutdown is queued on the render thread itself (GL.IssuePluginEvent
// with PXU_ShutdownEventFunc), behind the last submit, so the two can never
// overlap and neither waits on the other across threads. The main thread then
// calls PXU_Shutdown, which waits a moment for that to happen and only does it
// itself if the render thread never got to it.
static void __stdcall ShutdownEvent(int) { g_stopping = true; ShutdownNow(); }
PXU_API void* PXU_ShutdownEventFunc() { return (void*)&ShutdownEvent; }

PXU_API void PXU_Shutdown() {
    for (int i = 0; i < 200 && !g_shutdownDone; ++i) Sleep(10);
    if (g_shutdownDone) return;
    LogLine("pcvr-xr unity: the render thread did not shut the session down - doing it here");
    g_stopping = true;
    ShutdownNow();
}
