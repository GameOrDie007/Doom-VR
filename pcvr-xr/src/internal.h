// pcvr-xr - the internals both translation units share.
//
// Not a public header: a port includes pcvrxr/pcvrxr.h and nothing else.
//
// Copyright (c) 2026 Game Or Die
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pcvrxr/pcvrxr.h"

#ifdef __ANDROID__
// Android - the Quest 3, and the Steam Frame through Lepton: OpenGL ES through
// EGL, the Khronos loader told where the app lives, no Direct3D and no window.
// The same layer, the same seam; only the binding and the loader differ.
#include <jni.h>
#include <unistd.h>	// gettid, for XR_KHR_android_thread_settings
#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <dlfcn.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#ifndef APIENTRY
#define APIENTRY
#endif
#define PCVRXR_SEP "/"
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
// openxr_platform.h names IUnknown in its Win32 section whether or not any D3D
// graphics binding is asked for, and WIN32_LEAN_AND_MEAN is exactly what takes
// IUnknown away. The errors land inside the SDK header, which sends people off
// looking for a broken SDK.
#include <unknwn.h>
#include <GL/gl.h>
#include <d3d11.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#define XR_USE_GRAPHICS_API_D3D11
#define PCVRXR_D3D11 1
#define PCVRXR_SEP "\\"
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// The GL flavour the session binds: OpenGL ES on Android, OpenGL elsewhere.
// The swapchain image structures have the same shape (type, next, image).
#ifdef __ANDROID__
typedef XrSwapchainImageOpenGLESKHR XrSwapchainImageGL;
#define PCVRXR_SWAPCHAIN_IMAGE_GL_TYPE XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR
#define PCVRXR_GL_EXTENSION_NAME XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME
#else
typedef XrSwapchainImageOpenGLKHR XrSwapchainImageGL;
#define PCVRXR_SWAPCHAIN_IMAGE_GL_TYPE XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR
#define PCVRXR_GL_EXTENSION_NAME XR_KHR_OPENGL_ENABLE_EXTENSION_NAME
#endif

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

namespace pcvrxr {

namespace detail {

// ---------------------------------------------------------------------------
// The handful of GL entry points this needs beyond OpenGL 1.1, which is all
// opengl32.lib exports on Windows. Loading them here rather than pulling in a
// loader library keeps the layer something a port can drop in beside whatever
// loader it already has.
// ---------------------------------------------------------------------------

typedef void (APIENTRY *PFNGENFRAMEBUFFERS)(GLsizei, GLuint*);
typedef void (APIENTRY *PFNDELETEFRAMEBUFFERS)(GLsizei, const GLuint*);
typedef void (APIENTRY *PFNBINDFRAMEBUFFER)(GLenum, GLuint);
typedef void (APIENTRY *PFNFRAMEBUFFERTEXTURE2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (APIENTRY *PFNGENRENDERBUFFERS)(GLsizei, GLuint*);
typedef void (APIENTRY *PFNDELETERENDERBUFFERS)(GLsizei, const GLuint*);
typedef void (APIENTRY *PFNBINDRENDERBUFFER)(GLenum, GLuint);
typedef void (APIENTRY *PFNRENDERBUFFERSTORAGE)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY *PFNFRAMEBUFFERRENDERBUFFER)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (APIENTRY *PFNCHECKFRAMEBUFFERSTATUS)(GLenum);
typedef void (APIENTRY *PFNBLITFRAMEBUFFER)(GLint, GLint, GLint, GLint, GLint, GLint,
                                            GLint, GLint, GLbitfield, GLenum);
typedef void (APIENTRY *PFNBINDBUFFER)(GLenum, GLuint);

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER            0x8D40
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER       0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER       0x8CA9
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0      0x8CE0
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT       0x8D00
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER           0x8D41
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24      0x81A6
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE   0x8CD5
#endif
#ifndef GL_RGBA8
#define GL_RGBA8                  0x8058
#endif
#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB       0x8DB9
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE          0x812F
#endif

// Where colour goes on a user framebuffer. Desktop GL names one buffer;
// OpenGL ES only has the plural form.
inline void DrawBuffer0() {
#ifdef __ANDROID__
    const GLenum b = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &b);
#else
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
#endif
}

struct GLFns {
    PFNGENFRAMEBUFFERS         GenFramebuffers = nullptr;
    PFNDELETEFRAMEBUFFERS      DeleteFramebuffers = nullptr;
    PFNBINDFRAMEBUFFER         BindFramebuffer = nullptr;
    PFNFRAMEBUFFERTEXTURE2D    FramebufferTexture2D = nullptr;
    PFNGENRENDERBUFFERS        GenRenderbuffers = nullptr;
    PFNDELETERENDERBUFFERS     DeleteRenderbuffers = nullptr;
    PFNBINDRENDERBUFFER        BindRenderbuffer = nullptr;
    PFNRENDERBUFFERSTORAGE     RenderbufferStorage = nullptr;
    PFNFRAMEBUFFERRENDERBUFFER FramebufferRenderbuffer = nullptr;
    PFNCHECKFRAMEBUFFERSTATUS  CheckFramebufferStatus = nullptr;
    PFNBLITFRAMEBUFFER         BlitFramebuffer = nullptr;
    PFNBINDBUFFER              BindBuffer = nullptr;
    bool ok = false;
};

inline void* GLGet(const char* name) {
#ifdef __ANDROID__
    // EGL answers for every GLES entry point, core ones included (EGL 1.5).
    void* p = (void*)eglGetProcAddress(name);
    if (p == nullptr) {
        static void* gles = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
        p = gles ? dlsym(gles, name) : nullptr;
    }
    return p;
#else
    void* p = (void*)wglGetProcAddress(name);
    if (p == nullptr || p == (void*)0x1 || p == (void*)0x2 ||
        p == (void*)0x3 || p == (void*)-1) {
        // Old entry points live in the DLL itself, not in wglGetProcAddress.
        static HMODULE gl = LoadLibraryA("opengl32.dll");
        p = gl ? (void*)GetProcAddress(gl, name) : nullptr;
    }
    return p;
#endif
}

inline bool LoadGL(GLFns& f) {
    f.GenFramebuffers = (PFNGENFRAMEBUFFERS)GLGet("glGenFramebuffers");
    f.DeleteFramebuffers = (PFNDELETEFRAMEBUFFERS)GLGet("glDeleteFramebuffers");
    f.BindFramebuffer = (PFNBINDFRAMEBUFFER)GLGet("glBindFramebuffer");
    f.FramebufferTexture2D = (PFNFRAMEBUFFERTEXTURE2D)GLGet("glFramebufferTexture2D");
    f.GenRenderbuffers = (PFNGENRENDERBUFFERS)GLGet("glGenRenderbuffers");
    f.DeleteRenderbuffers = (PFNDELETERENDERBUFFERS)GLGet("glDeleteRenderbuffers");
    f.BindRenderbuffer = (PFNBINDRENDERBUFFER)GLGet("glBindRenderbuffer");
    f.RenderbufferStorage = (PFNRENDERBUFFERSTORAGE)GLGet("glRenderbufferStorage");
    f.FramebufferRenderbuffer = (PFNFRAMEBUFFERRENDERBUFFER)GLGet("glFramebufferRenderbuffer");
    f.CheckFramebufferStatus = (PFNCHECKFRAMEBUFFERSTATUS)GLGet("glCheckFramebufferStatus");
    f.BlitFramebuffer = (PFNBLITFRAMEBUFFER)GLGet("glBlitFramebuffer");
    f.BindBuffer = (PFNBINDBUFFER)GLGet("glBindBuffer");
    f.ok = f.GenFramebuffers && f.BindFramebuffer && f.FramebufferTexture2D &&
           f.GenRenderbuffers && f.BindRenderbuffer && f.RenderbufferStorage &&
           f.FramebufferRenderbuffer && f.CheckFramebufferStatus;
    return f.ok;
}

struct Eye {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageGL> images;
#ifdef PCVRXR_D3D11
    std::vector<XrSwapchainImageD3D11KHR> d3dImages;   // D3D11 sessions only
#endif
    std::vector<GLuint> fbos;
    GLuint depth = 0;
    uint32_t index = 0;   // the image acquired this frame
    int width = 0, height = 0;
    int swapW = 0, swapH = 0;   // the swapchain image: bigger than the picture on a Steam Frame

    // The target the port actually renders into, which is NOT the swapchain
    // image. See BeginEye for why.
    GLuint sceneFbo = 0;
    GLuint sceneColour = 0;
    GLuint sceneDepth = 0;
};

inline const char* StateName(XrSessionState s) {
    switch (s) {
    case XR_SESSION_STATE_IDLE:         return "IDLE";
    case XR_SESSION_STATE_READY:        return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED (not drawing yet)";
    case XR_SESSION_STATE_VISIBLE:      return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED:      return "FOCUSED";
    case XR_SESSION_STATE_STOPPING:     return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING:      return "EXITING";
    default:                            return "UNKNOWN";
    }
}

// q <- yaw(degrees) * q, about the reference space's up axis.
inline void ApplyYaw(Quat& q, float degrees) {
    const float half = degrees * 0.5f * 3.14159265358979f / 180.0f;
    const float s = sinf(half), c = cosf(half);
    const Quat b = q;
    q.w = c * b.w - s * b.y;
    q.x = c * b.x + s * b.z;
    q.y = c * b.y + s * b.w;
    q.z = c * b.z - s * b.x;
}

inline Pose FromXr(const XrPosef& p, XrSpaceLocationFlags flags) {
    Pose out;
    out.position = { p.position.x, p.position.y, p.position.z };
    out.orientation = { p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w };
    const XrSpaceLocationFlags need =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    out.tracked = (flags & need) == need;
    return out;
}

} // namespace detail

using namespace detail;

// ---------------------------------------------------------------------------

struct Session::Impl {
    LogFn log;
    Config cfg;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;      // between xrBeginSession and xrEndSession
    unsigned spaceGeneration = 0; // bumped when the runtime moves the space
    bool exitRequested = false;// the runtime asked the app to stop
    bool frameBegun = false;   // between xrBeginFrame and xrEndFrame
    bool shouldRender = false;

    XrSpace refSpace = XR_NULL_HANDLE;   // what poses are reported in
    XrSpace viewSpace = XR_NULL_HANDLE;  // the head
    bool haveBounds = false;
    bool connected = false;    // instance and system exist; session may not
    std::vector<XrViewConfigurationView> confViews;

    XrFrameState frameState{ XR_TYPE_FRAME_STATE };
    std::vector<XrView> xrViews;
    std::vector<XrCompositionLayerProjectionView> projViews;
    View views[2];
    Pose head;

    Eye eyes[2];
    int eyeCount = 2;
    int eyeW = 0, eyeH = 0;
    int64_t chosenFormat = 0;
    bool useSceneTarget = false;
    bool checkedSwapchainTexture = false;

    GLFns gl;
#ifdef PCVRXR_D3D11
    // D3D11 sessions: the game's device and its immediate context. Neither is
    // owned - the game made them and outlives the session.
    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;
    bool IsD3D() const { return cfg.graphics == Graphics::D3D11; }
    bool CopyIntoSwapchain(XrSwapchain sc, const std::vector<XrSwapchainImageD3D11KHR>& imgs,
                           uint32_t& index, void* tex, int w, int h, const char* what);
    void DumpD3D(ID3D11Texture2D* tex, int w, int h, const char* path, const char* alphaPath);
#else
    // No Direct3D on this platform: every session is a GL one.
    bool IsD3D() const { return false; }
#endif
    std::string runtimeName;
    std::string failure;

    float debugYaw = 0.0f;
    float debugPitchUp = 0.0f, debugRollRight = 0.0f;
    std::string dumpDir;
    bool dumpRequested = false;
    bool dumpWrote = false;      // something was actually written this frame
    int dumpWaited = 0;          // frames the request has gone unanswered
    int dumpIndex = 0;

    // ---- controllers -------------------------------------------------------
    //
    // One action per semantic input, each with two subaction paths, rather than
    // a left action and a right action. That is the idiomatic shape, and it
    // also sidesteps a trap worth naming: two actions sharing a localized name
    // makes the runtime drop the second one silently, and the symptom is a
    // button that simply never works with nothing anywhere saying why.

    XrActionSet actionSet = XR_NULL_HANDLE;
    XrPath handPath[2] = { XR_NULL_PATH, XR_NULL_PATH };

    XrAction aimPose = XR_NULL_HANDLE;
    XrAction gripPose = XR_NULL_HANDLE;
    XrAction triggerValue = XR_NULL_HANDLE;
    XrAction squeezeValue = XR_NULL_HANDLE;
    XrAction thumbstickXY = XR_NULL_HANDLE;
    XrAction triggerClick = XR_NULL_HANDLE;
    XrAction squeezeClick = XR_NULL_HANDLE;
    XrAction thumbstickClick = XR_NULL_HANDLE;
    XrAction primaryClick = XR_NULL_HANDLE;
    XrAction secondaryClick = XR_NULL_HANDLE;
    XrAction menuClick = XR_NULL_HANDLE;
    // The Steam Frame's buttons one by one (its own profile only).
    XrAction padA = XR_NULL_HANDLE, padB = XR_NULL_HANDLE, padX = XR_NULL_HANDLE, padY = XR_NULL_HANDLE;
    XrAction dpadUp = XR_NULL_HANDLE, dpadDown = XR_NULL_HANDLE, dpadLeft = XR_NULL_HANDLE, dpadRight = XR_NULL_HANDLE;
    XrAction shoulderClick = XR_NULL_HANDLE, viewClick = XR_NULL_HANDLE, padMenu = XR_NULL_HANDLE;
    XrAction hapticOut = XR_NULL_HANDLE;

    XrSpace aimSpace[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    XrSpace gripSpace[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };

    HandState hands[2];
    HandState fake[2];
    bool fakeOn = false;
    // Left-handed play (SetSwapHands): the port's Right is the left controller.
    bool swapHands = false;
    int PortHand(Hand hand) const { return ((hand == Hand::Right) != swapHands) ? 1 : 0; }
    bool inputReady = false;
    // Until the runtime sends its first interaction-profile event there is no
    // controller bound at all, and an empty string in a log reads as a bug in
    // the logging rather than the truth.
    std::string profileName[2] = { "(none yet)", "(none yet)" };

    // ---- the flat panel ----------------------------------------------------
    XrSwapchain panelSwapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageGL> panelImages;
#ifdef PCVRXR_D3D11
    std::vector<XrSwapchainImageD3D11KHR> panelD3DImages;
#endif
    std::vector<GLuint> panelFbos;
    uint32_t panelIndex = 0;
    int panelW = 0, panelH = 0;
    float panelWidthMetres = 3.0f;
    float panelHeightMetres = 0.0f;     // 0: from the texture's aspect
    float PanelHeightMetres() const {
        if (panelHeightMetres > 0.01f) return panelHeightMetres;
        return panelW > 0 ? panelWidthMetres * (float)panelH / (float)panelW : panelWidthMetres;
    }
    Pose panelPose;
    bool panelReady = false;
    bool panelDrawn = false;    // this frame
    bool suppressWorld = false; // this frame
    bool eyesDrawn = false;     // this frame

    // XR_FB_display_refresh_rate, when the runtime has it.
    bool haveRefreshRate = false;
    bool fovLogged = false;
    bool haveFrameControllers = false;
    bool haveLayerSettings = false;	// XR_FB_composition_layer_settings (Quest-class runtimes)
    bool layerSettingsLogged = false;
    // A standalone headset's clocks and thread scheduling (Android only):
    // XR_EXT_performance_settings and XR_KHR_android_thread_settings.
    bool havePerfSettings = false;
    bool haveThreadSettings = false;
    PFN_xrVoidFunction pfnSetPerfLevel = nullptr;
    PFN_xrVoidFunction pfnSetAndroidThread = nullptr;
    bool apiLayerOn = false;	// the port's extra API layer is in the instance
    PFN_xrVoidFunction pfnGetDisplayRefreshRate = nullptr;
    PFN_xrVoidFunction pfnRequestDisplayRefreshRate = nullptr;

    void PickRuntime();
    bool CreateInput();
    void SuggestAllBindings();
    void SyncInput(XrTime time);
    void ReadInteractionProfiles();

    void Log(const char* fmt, ...) {
        if (!log) return;
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        log(buf);
    }

    bool Check(XrResult r, const char* what) {
        if (XR_SUCCEEDED(r)) return true;
        char name[XR_MAX_RESULT_STRING_SIZE] = {};
        if (instance != XR_NULL_HANDLE) xrResultToString(instance, r, name);
        else snprintf(name, sizeof(name), "%d", (int)r);
        failure = std::string(what) + " failed: " + name;
        Log("pcvr-xr: %s", failure.c_str());
        return false;
    }

    bool LocateFrame();

    // The split frame (WaitFrame / SubmitFrame): what each waited frame was
    // predicted with, kept until the render thread submits it.
    struct FrameRecord {
        uint64_t ticket = 0;
        XrFrameState state{ XR_TYPE_FRAME_STATE };
        bool shouldRender = false;
        XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
        Pose panelPose;
        float panelW = 0, panelH = 0;
        bool dump = false;
    };
    static const int kRing = 8;
    FrameRecord ring[kRing];
    uint64_t nextTicket = 1;

    bool CreateInstance();
    bool CreateSession();
    bool CreateSwapchains();
    void CreateSpaces();
    void PollEvents();
    void DumpEye(int eye);
    void DumpPanel();
};

} // namespace pcvrxr
