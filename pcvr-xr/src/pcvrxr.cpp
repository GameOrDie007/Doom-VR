// pcvr-xr - session, swapchains, spaces and the frame loop.
//
// Copyright (c) 2026 Game Or Die
// SPDX-License-Identifier: GPL-3.0-or-later

#include "internal.h"

namespace pcvrxr {


// ---------------------------------------------------------------------------
// Which runtime this launch gets. Windows' own choice (the ActiveRuntime key)
// stands, with two exceptions for SteamVR - the only way a Steam Frame is
// reached, and a headset whose app cannot set the system's runtime:
//  - SteamVR is RUNNING and nothing has picked a runtime for this process
//    (no XR_RUNTIME_JSON): this launch uses SteamVR. A game started from its
//    own folder gets the system's runtime, not SteamVR's (only a launch from
//    Steam is handed SteamVR's), so with Virtual Desktop as the system's
//    runtime a Frame owner got "no headset" and a flat game. Only this
//    process and its children; the system setting is never touched, and
//    with SteamVR not running nothing changes.
//  - A 32-bit build handed SteamVR's 64-bit manifest (SteamVR sets it for
//    every game it launches) takes the 32-bit one beside it; the 64-bit one
//    cannot load into it.
// Both say so in the log.

static bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string Narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring Widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

// The SteamVR folder of a running vrserver.exe (...\SteamVR\bin\win64\vrserver.exe), or "".
static std::wstring RunningSteamVR() {
    std::wstring found;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return found;
    PROCESSENTRY32W pe{ sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok && found.empty(); ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"vrserver.exe") != 0) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t path[MAX_PATH * 2];
        DWORD len = (DWORD)(sizeof(path) / sizeof(path[0]));
        if (QueryFullProcessImageNameW(h, 0, path, &len)) {
            std::wstring p(path, len);
            for (int up = 0; up < 3 && !p.empty(); ++up) {
                const size_t cut = p.find_last_of(L"\\/");
                p = cut == std::wstring::npos ? std::wstring() : p.substr(0, cut);
            }
            found = p;
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return found;
}

static bool Is32BitBuild() { return sizeof(void*) == 4; }

void Session::Impl::PickRuntime() {
    const bool is32 = Is32BitBuild();
    const wchar_t* ours = is32 ? L"steamxr_win32.json" : L"steamxr_win64.json";
    wchar_t cur[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableW(L"XR_RUNTIME_JSON", cur, (DWORD)(sizeof(cur) / sizeof(cur[0])));
    if (n > 0 && n < sizeof(cur) / sizeof(cur[0])) {
        std::wstring set(cur);
        const size_t at = set.rfind(L"steamxr_win64.json");
        if (is32 && at != std::wstring::npos) {
            std::wstring sibling = set.substr(0, at) + L"steamxr_win32.json";
            if (FileExists(sibling)) {
                SetEnvironmentVariableW(L"XR_RUNTIME_JSON", sibling.c_str());
                Log("pcvr-xr: XR_RUNTIME_JSON named SteamVR's 64-bit runtime (%s), which cannot load into this 32-bit game - using %s",
                    Narrow(set).c_str(), Narrow(sibling).c_str());
                return;
            }
        }
        Log("pcvr-xr: runtime chosen for this launch by XR_RUNTIME_JSON: %s", Narrow(set).c_str());
        return;
    }
    const std::wstring steamvr = RunningSteamVR();
    if (steamvr.empty()) return;
    const std::wstring json = steamvr + L"\\" + ours;
    if (!FileExists(json)) {
        Log("pcvr-xr: SteamVR is running but %s is not there - leaving the system's runtime", Narrow(json).c_str());
        return;
    }
    SetEnvironmentVariableW(L"XR_RUNTIME_JSON", json.c_str());
    Log("pcvr-xr: SteamVR is running - using it for this launch (%s); the system's runtime setting is unchanged", Narrow(json).c_str());
}

// ---------------------------------------------------------------------------

bool Session::Impl::CreateInstance() {
    PickRuntime();
    // The port's extra API layer: its folder on the loader's layer path,
    // before anything asks the loader for layers.
    if (!cfg.apiLayerDir.empty() && !cfg.apiLayerName.empty()) {
        std::wstring dir = Widen(cfg.apiLayerDir);
        wchar_t old[4096] = {};
        const DWORD n = GetEnvironmentVariableW(L"XR_API_LAYER_PATH", old, 4096);
        if (n > 0 && n < 4096) dir = std::wstring(old) + L";" + dir;
        SetEnvironmentVariableW(L"XR_API_LAYER_PATH", dir.c_str());
    }
    uint32_t count = 0;
    // The loader answers this itself when it cannot find or load a runtime,
    // and an empty list would otherwise be reported below as "does not
    // support OpenGL" - true of nothing, and a wrong lead for whoever reads it.
    const XrResult er = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    if (XR_FAILED(er)) {
        failure = er == XR_ERROR_RUNTIME_UNAVAILABLE
            ? "no OpenXR runtime could be loaded - is one installed and set active?"
            : "the OpenXR loader failed before reaching a runtime";
        Log("pcvr-xr: %s (xrEnumerateInstanceExtensionProperties returned %d)", failure.c_str(), (int)er);
        return false;
    }
    std::vector<XrExtensionProperties> props(count, { XR_TYPE_EXTENSION_PROPERTIES });
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data());

    auto has = [&props](const char* name) {
        for (const auto& p : props) if (strcmp(p.extensionName, name) == 0) return true;
        return false;
    };

    const char* graphicsExt = IsD3D() ? XR_KHR_D3D11_ENABLE_EXTENSION_NAME
                                      : XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
    if (!has(graphicsExt)) {
        failure = IsD3D() ? "this runtime does not support Direct3D 11"
                          : "this runtime does not support OpenGL";
        Log("pcvr-xr: %s", failure.c_str());
        return false;
    }

    // Enumerate first, then ask only for what is there. ONE unknown extension
    // name fails xrCreateInstance outright and takes all of VR with it, so a
    // port that asks unconditionally for something its own headset has will
    // simply not run in VR on anyone else's.
    std::vector<const char*> exts;
    exts.push_back(graphicsExt);

    haveRefreshRate = has("XR_FB_display_refresh_rate");
    if (haveRefreshRate) exts.push_back("XR_FB_display_refresh_rate");

    // The Steam Frame's own controller profile, where the runtime has it
    // (SteamVR 2.17.10+). Not in the Khronos registry, so never assumed.
    haveFrameControllers = has("XR_VALVE_frame_controller_interaction");
    if (haveFrameControllers) exts.push_back("XR_VALVE_frame_controller_interaction");

    for (const std::string& want : cfg.extraExtensions) {
        if (has(want.c_str())) {
            exts.push_back(want.c_str());
            Log("pcvr-xr: extension %s - available", want.c_str());
        } else {
            Log("pcvr-xr: extension %s - NOT available, carrying on without it", want.c_str());
        }
    }

    XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.enabledExtensionNames = exts.data();
    snprintf(ci.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "%s", cfg.appName.c_str());
    ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    // The port's extra API layer, when the loader can see it.
    const char* layerName = cfg.apiLayerName.c_str();
    bool withLayer = false;
    if (!cfg.apiLayerName.empty()) {
        uint32_t nl = 0;
        if (XR_SUCCEEDED(xrEnumerateApiLayerProperties(0, &nl, nullptr)) && nl > 0) {
            std::vector<XrApiLayerProperties> layers(nl, { XR_TYPE_API_LAYER_PROPERTIES });
            xrEnumerateApiLayerProperties(nl, &nl, layers.data());
            for (const auto& l : layers) if (strcmp(l.layerName, layerName) == 0) withLayer = true;
        }
        Log("pcvr-xr: API layer %s - %s", layerName, withLayer ? "found" : "NOT found, carrying on without it");
    }
    if (withLayer) {
        ci.enabledApiLayerCount = 1;
        ci.enabledApiLayerNames = &layerName;
        const XrResult lr = xrCreateInstance(&ci, &instance);
        if (XR_FAILED(lr)) {
            Log("pcvr-xr: xrCreateInstance with %s failed (%d) - again without it", layerName, (int)lr);
            ci.enabledApiLayerCount = 0;
            ci.enabledApiLayerNames = nullptr;
            withLayer = false;
        }
    }
    apiLayerOn = withLayer;
    if (!withLayer && !Check(xrCreateInstance(&ci, &instance), "xrCreateInstance")) return false;

    if (haveRefreshRate) {
        xrGetInstanceProcAddr(instance, "xrGetDisplayRefreshRateFB", &pfnGetDisplayRefreshRate);
        xrGetInstanceProcAddr(instance, "xrRequestDisplayRefreshRateFB", &pfnRequestDisplayRefreshRate);
    }

    XrInstanceProperties ip{ XR_TYPE_INSTANCE_PROPERTIES };
    if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &ip))) {
        runtimeName = ip.runtimeName;
        Log("pcvr-xr: runtime '%s' %d.%d.%d", ip.runtimeName,
            (int)XR_VERSION_MAJOR(ip.runtimeVersion),
            (int)XR_VERSION_MINOR(ip.runtimeVersion),
            (int)XR_VERSION_PATCH(ip.runtimeVersion));
    }

    XrSystemGetInfo sys{ XR_TYPE_SYSTEM_GET_INFO };
    sys.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = xrGetSystem(instance, &sys, &system);
    if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        // The ordinary case on a desk: a runtime is installed, no headset is
        // connected. Not an error in the code, and worth saying plainly.
        failure = "no headset is connected";
        Log("pcvr-xr: %s (xrGetSystem said the form factor is unavailable)", failure.c_str());
        return false;
    }
    if (!Check(r, "xrGetSystem")) return false;

    // The eye resolution is settled here rather than with the swapchains,
    // because it needs no graphics context - and an engine that sizes its own
    // framebuffers from it has to know it BEFORE it opens the window the
    // session will later bind to.
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(instance, system,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
    confViews.assign(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    if (!Check(xrEnumerateViewConfigurationViews(instance, system,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, confViews.data()),
            "xrEnumerateViewConfigurationViews")) {
        return false;
    }
    if (viewCount < 2) { failure = "this runtime is not stereo"; return false; }

    const float scale = cfg.resolutionScale > 0.01f ? cfg.resolutionScale : 1.0f;
    eyeW = (int)(confViews[0].recommendedImageRectWidth * scale);
    eyeH = (int)(confViews[0].recommendedImageRectHeight * scale);
    Log("pcvr-xr: headset found, %dx%d per eye", eyeW, eyeH);
    return true;
}

bool Session::Impl::CreateSession() {
    if (IsD3D()) {
        // Also mandatory before the session, and here the answer matters: the
        // runtime names the GPU the headset is driven from, and a device on a
        // different adapter (a laptop's integrated GPU) is refused.
        PFN_xrGetD3D11GraphicsRequirementsKHR getD3DReq = nullptr;
        xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR",
                              (PFN_xrVoidFunction*)&getD3DReq);
        if (getD3DReq != nullptr) {
            XrGraphicsRequirementsD3D11KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
            if (XR_SUCCEEDED(getD3DReq(instance, system, &req))) {
                LUID have{};
                IDXGIDevice* dxgi = nullptr;
                if (SUCCEEDED(d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi))) {
                    IDXGIAdapter* adapter = nullptr;
                    if (SUCCEEDED(dxgi->GetAdapter(&adapter))) {
                        DXGI_ADAPTER_DESC d{};
                        adapter->GetDesc(&d);
                        have = d.AdapterLuid;
                        char name[128] = {};
                        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
                        Log("pcvr-xr: the game renders on %s", name);
                        adapter->Release();
                    }
                    dxgi->Release();
                }
                if (have.LowPart != req.adapterLuid.LowPart || have.HighPart != req.adapterLuid.HighPart) {
                    Log("pcvr-xr: WARNING - the runtime drives the headset from a different GPU than the "
                        "game renders on; the session will probably be refused");
                }
            }
        }
        XrGraphicsBindingD3D11KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
        binding.device = d3dDevice;
        XrSessionCreateInfo ci{ XR_TYPE_SESSION_CREATE_INFO };
        ci.next = &binding;
        ci.systemId = system;
        return Check(xrCreateSession(instance, &ci, &session), "xrCreateSession");
    }

    // The graphics requirements call must be made before creating a session,
    // even when its answer is ignored: runtimes fail the session otherwise.
    PFN_xrGetOpenGLGraphicsRequirementsKHR getReq = nullptr;
    xrGetInstanceProcAddr(instance, "xrGetOpenGLGraphicsRequirementsKHR",
                          (PFN_xrVoidFunction*)&getReq);
    if (getReq != nullptr) {
        XrGraphicsRequirementsOpenGLKHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
        getReq(instance, system, &req);
        Log("pcvr-xr: runtime wants OpenGL %d.%d - %d.%d",
            (int)XR_VERSION_MAJOR(req.minApiVersionSupported),
            (int)XR_VERSION_MINOR(req.minApiVersionSupported),
            (int)XR_VERSION_MAJOR(req.maxApiVersionSupported),
            (int)XR_VERSION_MINOR(req.maxApiVersionSupported));
    }

    HDC dc = wglGetCurrentDC();
    HGLRC rc = wglGetCurrentContext();
    if (dc == nullptr || rc == nullptr) {
        failure = "no current OpenGL context on this thread";
        Log("pcvr-xr: %s - BringUp must be called where the context is current", failure.c_str());
        return false;
    }

    XrGraphicsBindingOpenGLWin32KHR binding{ XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
    binding.hDC = dc;
    binding.hGLRC = rc;

    XrSessionCreateInfo ci{ XR_TYPE_SESSION_CREATE_INFO };
    ci.next = &binding;
    ci.systemId = system;
    return Check(xrCreateSession(instance, &ci, &session), "xrCreateSession");
}

bool Session::Impl::CreateSwapchains() {
    eyeCount = 2;

    if (IsD3D()) {
        uint32_t n = 0;
        xrEnumerateSwapchainFormats(session, 0, &n, nullptr);
        std::vector<int64_t> fmts(n);
        xrEnumerateSwapchainFormats(session, n, &n, fmts.data());
        // sRGB RGBA, because the port's textures are copied in byte for byte
        // and a copy only works within one format family. A runtime that
        // offers only BGRA would need a draw, not a copy - say so rather than
        // submit garbage.
        chosenFormat = 0;
        for (int64_t f : fmts) if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) chosenFormat = f;
        if (chosenFormat == 0) {
            for (int64_t f : fmts) if (f == DXGI_FORMAT_R8G8B8A8_UNORM) chosenFormat = f;
            if (chosenFormat != 0)
                Log("pcvr-xr: this runtime offers no sRGB RGBA swapchain - using linear RGBA, colours will be dark");
        }
        if (chosenFormat == 0) {
            failure = "this runtime offers no RGBA swapchain format";
            Log("pcvr-xr: %s", failure.c_str());
            return false;
        }
        for (int e = 0; e < eyeCount; ++e) {
            Eye& eye = eyes[e];
            eye.width = eyeW;
            eye.height = eyeH;
            XrSwapchainCreateInfo sci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
            sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                             XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            sci.format = chosenFormat;
            sci.sampleCount = 1;
            sci.width = eyeW;
            sci.height = eyeH;
            sci.faceCount = 1;
            sci.arraySize = 1;
            sci.mipCount = 1;
            if (!Check(xrCreateSwapchain(session, &sci, &eye.swapchain), "xrCreateSwapchain")) return false;
            uint32_t imageCount = 0;
            xrEnumerateSwapchainImages(eye.swapchain, 0, &imageCount, nullptr);
            eye.d3dImages.assign(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
            xrEnumerateSwapchainImages(eye.swapchain, imageCount, &imageCount,
                (XrSwapchainImageBaseHeader*)eye.d3dImages.data());
            Log("pcvr-xr: eye %d swapchain %dx%d (D3D11, DXGI format %d), %u images",
                e, eyeW, eyeH, (int)chosenFormat, imageCount);
        }
        xrViews.assign(eyeCount, { XR_TYPE_VIEW });
        projViews.assign(eyeCount, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW });
        return true;
    }

    uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data());
    // sRGB where it is offered: the compositor expects it, and a linear buffer
    // shows up as a washed-out picture that is easy to blame on the game.
    int64_t chosen = formats.empty() ? 0 : formats[0];
    for (int64_t f : formats) { if (f == 0x8C43 /*GL_SRGB8_ALPHA8*/) { chosen = f; break; } }
    chosenFormat = chosen;

    for (int e = 0; e < eyeCount; ++e) {
        Eye& eye = eyes[e];
        eye.width = eyeW;
        eye.height = eyeH;

        XrSwapchainCreateInfo sci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sci.format = chosen;
        sci.sampleCount = 1;
        sci.width = eyeW;
        sci.height = eyeH;
        sci.faceCount = 1;
        sci.arraySize = 1;
        sci.mipCount = 1;
        if (!Check(xrCreateSwapchain(session, &sci, &eye.swapchain), "xrCreateSwapchain")) return false;

        uint32_t imageCount = 0;
        xrEnumerateSwapchainImages(eye.swapchain, 0, &imageCount, nullptr);
        eye.images.assign(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR });
        xrEnumerateSwapchainImages(eye.swapchain, imageCount, &imageCount,
            (XrSwapchainImageBaseHeader*)eye.images.data());

        eye.fbos.resize(imageCount);
        gl.GenFramebuffers((GLsizei)imageCount, eye.fbos.data());
        gl.GenRenderbuffers(1, &eye.depth);
        gl.BindRenderbuffer(GL_RENDERBUFFER, eye.depth);
        gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, eyeW, eyeH);
        for (uint32_t i = 0; i < imageCount; ++i) {
            // The swapchain image is a texture the runtime created, and it
            // arrives with GL's defaults - which include a minification filter
            // that wants mipmaps. A texture with no mipmaps and that filter is
            // INCOMPLETE, and what a driver does with an incomplete texture
            // attached to a framebuffer is its own business. Set the filters
            // and the wrap mode, exactly as any renderer would.
            glBindTexture(GL_TEXTURE_2D, eye.images[i].image);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);

            gl.BindFramebuffer(GL_FRAMEBUFFER, eye.fbos[i]);
            gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                GL_TEXTURE_2D, eye.images[i].image, 0);
            gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                GL_RENDERBUFFER, eye.depth);
            if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                failure = "the eye framebuffer is incomplete";
                Log("pcvr-xr: %s", failure.c_str());
                return false;
            }
        }
        /*
            The target the port renders into, which is NOT the swapchain image.

            Rendering straight into a runtime-owned swapchain texture is legal
            and works for a test app, but a real engine does a great many
            things to the framebuffer it is handed, and a swapchain image is
            not an ordinary texture - it is owned by the compositor, acquired
            and released every frame, and re-used from a ring. Giving the
            engine an ordinary renderbuffer of our own and copying the finished
            picture across at EndEye costs one blit and removes that whole
            class of question. It is also where MSAA will go.
        */
        gl.GenFramebuffers(1, &eye.sceneFbo);
        gl.GenRenderbuffers(1, &eye.sceneColour);
        gl.BindRenderbuffer(GL_RENDERBUFFER, eye.sceneColour);
        // The SAME format as the swapchain image it is copied into. A blit
        // between a linear buffer and an sRGB one is a colour-space
        // conversion as well as a copy, and drivers are inconsistent about
        // whether they will do it at all - matching the formats removes the
        // question and makes the copy a straight move of bytes.
        gl.RenderbufferStorage(GL_RENDERBUFFER, (GLenum)chosen, eyeW, eyeH);
        gl.GenRenderbuffers(1, &eye.sceneDepth);
        gl.BindRenderbuffer(GL_RENDERBUFFER, eye.sceneDepth);
        gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, eyeW, eyeH);
        gl.BindRenderbuffer(GL_RENDERBUFFER, 0);

        gl.BindFramebuffer(GL_FRAMEBUFFER, eye.sceneFbo);
        gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_RENDERBUFFER, eye.sceneColour);
        gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
            GL_RENDERBUFFER, eye.sceneDepth);
        if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            failure = "the eye's render target is incomplete";
            Log("pcvr-xr: %s", failure.c_str());
            return false;
        }

        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        Log("pcvr-xr: eye %d swapchain %dx%d, %u images, rendering via its own target",
            e, eyeW, eyeH, imageCount);
    }

    xrViews.assign(eyeCount, { XR_TYPE_VIEW });
    projViews.assign(eyeCount, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW });
    return true;
}

void Session::Impl::CreateSpaces() {
    XrReferenceSpaceCreateInfo ci{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    ci.poseInReferenceSpace.orientation.w = 1.0f;

    // The head, always.
    ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    xrCreateReferenceSpace(session, &ci, &viewSpace);

    if (refSpace != XR_NULL_HANDLE) { xrDestroySpace(refSpace); refSpace = XR_NULL_HANDLE; }

    // STAGE is floor level, but only if this runtime has a boundary to measure
    // it from. A streaming runtime with a seated player has none, and asking
    // for STAGE anyway gives a space whose floor is a guess. Ask for the bounds
    // first and say which one is in use, because everything the port places
    // vertically depends on the answer.
    haveBounds = false;
    if (cfg.space == Space::Standing) {
        XrExtent2Df bounds{};
        if (XR_SUCCEEDED(xrGetReferenceSpaceBoundsRect(session,
                XR_REFERENCE_SPACE_TYPE_STAGE, &bounds))) {
            ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
            ci.poseInReferenceSpace.position.y = 0.0f;
            if (XR_SUCCEEDED(xrCreateReferenceSpace(session, &ci, &refSpace))) {
                haveBounds = true;
                Log("pcvr-xr: standing space, play area %.2f x %.2f m", bounds.width, bounds.height);
            }
        }
    }
    if (refSpace == XR_NULL_HANDLE) {
        ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        ci.poseInReferenceSpace.position.y = -cfg.seatedFloorDrop;
        xrCreateReferenceSpace(session, &ci, &refSpace);
        Log("pcvr-xr: seated space, floor assumed %.3f m below the head%s",
            cfg.seatedFloorDrop,
            cfg.space == Space::Standing ? " (this runtime reports no play area)" : "");
    }
}

void Session::Impl::PollEvents() {
    for (;;) {
        XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
        if (xrPollEvent(instance, &ev) != XR_SUCCESS) break;

        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& e = *(XrEventDataSessionStateChanged*)&ev;
            state = e.state;
            // Always logged. "Nothing is being drawn" has one cause nine times
            // out of ten - the session is not in a state that draws - and
            // without this line there is no way to tell that from a renderer
            // that is being asked to draw and failing.
            Log("pcvr-xr: session state -> %s", StateName(state));
            if (state == XR_SESSION_STATE_READY && !running) {
                XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (Check(xrBeginSession(session, &bi), "xrBeginSession")) {
                    running = true;
                    Log("pcvr-xr: session running");
                }
            } else if (state == XR_SESSION_STATE_STOPPING && running) {
                xrEndSession(session);
                running = false;
                Log("pcvr-xr: session stopped");
            } else if (state == XR_SESSION_STATE_EXITING ||
                       state == XR_SESSION_STATE_LOSS_PENDING) {
                // The runtime is asking the app to stop, or has lost the
                // device. Neither is an error and neither goes away by
                // carrying on; the port has to be told, or it spins forever
                // rendering nothing and looks hung.
                exitRequested = true;
                if (running) { xrEndSession(session); running = false; }
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            // Fires when a controller wakes, sleeps, or is swapped for another
            // kind. Until it fires the first time no profile is bound at all,
            // which is why this is read here rather than at bring-up.
            ReadInteractionProfiles();
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            // The runtime recentered itself. Rebuild, or every pose is measured
            // from a space that no longer exists where it used to - and tell
            // the port, whose own references were taken in the old one.
            CreateSpaces();
            ++spaceGeneration;
            Log("pcvr-xr: the runtime moved the reference space (generation %u)", spaceGeneration);
        }
    }
}

// ---------------------------------------------------------------------------

Session::Session() : m(new Impl) {}
Session::~Session() { Shutdown(); delete m; }

void Session::SetLog(LogFn log) { m->log = std::move(log); }

bool Session::Connect(const Config& cfg) {
    if (m->connected) return true;
    m->cfg = cfg;
    m->failure.clear();
    if (!m->CreateInstance()) { Shutdown(); return false; }
    m->connected = true;
    return true;
}

bool Session::IsConnected() const { return m->connected; }

bool Session::BringUp(const Config& cfg) {
    if (!m->connected && !Connect(cfg)) return false;
    m->cfg = cfg;
    m->failure.clear();
    m->useSceneTarget = cfg.renderToOwnTarget;

    if (m->IsD3D()) {
        m->d3dDevice = (ID3D11Device*)cfg.d3d11Device;
        if (m->d3dDevice == nullptr) {
            m->failure = "a D3D11 session needs the game's device";
            m->Log("pcvr-xr: %s", m->failure.c_str());
            return false;
        }
        if (m->d3dContext == nullptr) m->d3dDevice->GetImmediateContext(&m->d3dContext);
        if (!m->CreateSession())  { Shutdown(); return false; }
        if (!m->CreateSwapchains()){ Shutdown(); return false; }
        m->CreateSpaces();
        if (!m->CreateInput()) m->Log("pcvr-xr: carrying on without controllers");
        m->Log("pcvr-xr: up - %s, eye buffer %dx%d (D3D11)", m->runtimeName.c_str(), m->eyeW, m->eyeH);
        return true;
    }

    // The graphics functions, and the context behind them, are needed only
    // from here. Connect() deliberately asks for neither.
    if (!LoadGL(m->gl)) {
        m->failure = "this OpenGL context has no framebuffer objects";
        m->Log("pcvr-xr: %s", m->failure.c_str());
        return false;
    }
    // The copy into the swapchain image depends on it, and a silent `if the
    // pointer is there` around that copy is how a layer ends up submitting
    // black with nothing in the log.
    if (m->gl.BlitFramebuffer == nullptr) {
        m->failure = "this OpenGL context has no glBlitFramebuffer";
        m->Log("pcvr-xr: %s", m->failure.c_str());
        return false;
    }
    if (!m->CreateSession())  { Shutdown(); return false; }
    if (!m->CreateSwapchains()){ Shutdown(); return false; }
    m->CreateSpaces();
    // After the spaces: the hand poses are reported in the reference space, so
    // there has to be one first. Input failing is not fatal - a port that can
    // still be played with a gamepad is worth more than one that refuses to
    // start because a controller is flat.
    if (!m->CreateInput()) {
        m->Log("pcvr-xr: carrying on without controllers");
    }

    m->Log("pcvr-xr: up - %s, eye buffer %dx%d", m->runtimeName.c_str(), m->eyeW, m->eyeH);
    return true;
}

bool Session::IsUp() const { return m->session != XR_NULL_HANDLE; }
bool Session::ExitRequested() const { return m->exitRequested; }
bool Session::IsVisible() const {
    return m->state == XR_SESSION_STATE_VISIBLE || m->state == XR_SESSION_STATE_FOCUSED;
}
unsigned Session::SpaceGeneration() const { return m->spaceGeneration; }

void Session::SimulateSpaceChange() {
    // What a runtime's recenter looks like to the port, for a desk with no
    // headset: the same rebuild and the same bump.
    m->CreateSpaces();
    ++m->spaceGeneration;
    m->Log("pcvr-xr: SIMULATED reference space move (generation %u) - a test, not the runtime",
           m->spaceGeneration);
}
const char* Session::FailureReason() const { return m->failure.c_str(); }
const char* Session::RuntimeName() const { return m->runtimeName.c_str(); }

int Session::SwapchainTextures(int eye, uint32_t* out, int max) const {
    if (eye < 0 || eye > 1 || out == nullptr) return 0;
    const Eye& e = m->eyes[eye];
    int n = 0;
    for (const auto& img : e.images) {
        if (n >= max) break;
        out[n++] = (uint32_t)img.image;
    }
    return n;
}

Session::Native Session::GetNative() const {
    Native n;
    n.instance = (uint64_t)(uintptr_t)m->instance;
    n.session = (uint64_t)(uintptr_t)m->session;
    n.refSpace = (uint64_t)m->refSpace;
    n.viewSpace = (uint64_t)m->viewSpace;
    n.displayTime = (int64_t)m->frameState.predictedDisplayTime;
    return n;
}

float Session::RefreshRate() const {
    if (!m->haveRefreshRate || m->pfnGetDisplayRefreshRate == nullptr) return 0.0f;
    float hz = 0.0f;
    auto fn = (XrResult (XRAPI_PTR*)(XrSession, float*))m->pfnGetDisplayRefreshRate;
    if (XR_FAILED(fn(m->session, &hz))) return 0.0f;
    return hz;
}

bool Session::SetRefreshRate(float hz) {
    if (!m->haveRefreshRate || m->pfnRequestDisplayRefreshRate == nullptr) {
        m->Log("pcvr-xr: this runtime cannot change the refresh rate");
        return false;
    }
    auto fn = (XrResult (XRAPI_PTR*)(XrSession, float))m->pfnRequestDisplayRefreshRate;
    const bool ok = XR_SUCCEEDED(fn(m->session, hz));
    m->Log("pcvr-xr: refresh rate %.1f Hz %s", hz, ok ? "accepted" : "refused");
    return ok;
}
void Session::EyeBufferSize(int& w, int& h) const { w = m->eyeW; h = m->eyeH; }
int Session::EyeCount() const { return m->eyeCount; }
const View& Session::GetView(int eye) const { return m->views[eye < 0 ? 0 : (eye > 1 ? 1 : eye)]; }
const Pose& Session::Head() const { return m->head; }
void Session::SetDebugYaw(float degrees) { m->debugYaw = degrees; }
void Session::SetDebugTilt(float pitchUp, float rollRight) { m->debugPitchUp = pitchUp; m->debugRollRight = rollRight; }

void Session::RequestEyeDump(const char* dir) {
    m->dumpDir = dir ? dir : ".";
    m->dumpRequested = true;
    m->dumpWaited = 0;
}

void Session::Recentre() {
    if (m->session == XR_NULL_HANDLE) return;
    m->CreateSpaces();
}

// Everything a frame needs located once xrWaitFrame has answered: both
// views, the head and the hands, all for the frame's predicted display time,
// plus the debug turns. Shared by BeginFrame and WaitFrame. False (and
// shouldRender cleared) when the views cannot be located.
bool Session::Impl::LocateFrame() {
    XrViewLocateInfo li{ XR_TYPE_VIEW_LOCATE_INFO };
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = frameState.predictedDisplayTime;
    li.space = refSpace;
    XrViewState vs{ XR_TYPE_VIEW_STATE };
    uint32_t got = 0;
    if (!Check(xrLocateViews(session, &li, &vs, (uint32_t)xrViews.size(), &got,
            xrViews.data()), "xrLocateViews")) {
        shouldRender = false;
        return false;
    }
    const bool posesGood =
        (vs.viewStateFlags & (XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT))
        == (XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT);

    for (int e = 0; e < eyeCount; ++e) {
        View& v = views[e];
        v.pose = FromXr(xrViews[e].pose, posesGood
            ? (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT) : 0);
        // A test hook, never for play: PCVRXR_TEST_FOV="left right up down" in
        // degrees gives the LEFT eye that field of view and the right eye its
        // mirror image - a lopsided headset (a Quest 3 is wider outward than
        // inward) on a runtime whose eyes are square, so what is drawn round
        // straight ahead can be checked against where each eye puts it.
        {
            static int haveTest = -1;
            static float tf[4] = {};
            if (haveTest < 0) {
                const char* env = getenv("PCVRXR_TEST_FOV");
                haveTest = env && sscanf(env, "%f %f %f %f", &tf[0], &tf[1], &tf[2], &tf[3]) == 4 ? 1 : 0;
                if (haveTest) Log("pcvr-xr: TEST field of view %.0f %.0f %.0f %.0f (PCVRXR_TEST_FOV)", tf[0], tf[1], tf[2], tf[3]);
            }
            if (haveTest) {
                const float k = 3.14159265f / 180.f;
                XrFovf& f = xrViews[e].fov;
                if (e == 0) { f.angleLeft = tf[0] * k; f.angleRight = tf[1] * k; }
                else        { f.angleLeft = -tf[1] * k; f.angleRight = -tf[0] * k; }
                f.angleUp = tf[2] * k; f.angleDown = tf[3] * k;
            }
        }
        v.fov = { xrViews[e].fov.angleLeft, xrViews[e].fov.angleRight,
                  xrViews[e].fov.angleUp, xrViews[e].fov.angleDown };
        v.width = eyeW;
        v.height = eyeH;
    }
    // Each eye's field of view, once a session: which way it leans, and
    // how square its pixels are (a round thing drawn in the world comes out
    // this much taller than wide in the eye image).
    if (!fovLogged && posesGood) {
        fovLogged = true;
        for (int e = 0; e < eyeCount; ++e) {
            const XrFovf& f = xrViews[e].fov;
            const double k = 180.0 / 3.14159265358979;
            const double pxX = eyeW / (tan(f.angleRight) - tan(f.angleLeft));
            const double pxY = eyeH / (tan(f.angleUp) - tan(f.angleDown));
            Log("pcvr-xr: eye %d field of view left %.1f right %.1f up %.1f down %.1f deg; pixels %.3f times as tall as wide",
                e, f.angleLeft * k, f.angleRight * k, f.angleUp * k, f.angleDown * k, pxX / pxY);
        }
    }

    // Controllers, for the same predicted time as the eyes. Reading input at a
    // different time from the one the frame is drawn for puts the hands where
    // they were, in a world drawn for where the head will be.
    SyncInput(frameState.predictedDisplayTime);

    XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
    if (XR_SUCCEEDED(xrLocateSpace(viewSpace, refSpace,
            frameState.predictedDisplayTime, &loc))) {
        Pose p = FromXr(loc.pose, loc.locationFlags);
        // An untracked pose is not a pose: keep the last good one rather than
        // teleporting the player to the origin for a frame.
        if (p.tracked) head = p;
    }
    if (debugYaw != 0.0f) {
        // A null-driver HMD never moves, so without this there is no way to ask
        // whether the view follows the head with no headset on. It has to reach
        // the *views* and not only the reported head pose - a yaw that moves
        // nothing the renderer reads produces an identical picture every frame,
        // which looks exactly like the answer "no".
        //
        // The submitted pose in EndEye is deliberately left alone: it is what
        // the compositor knows about, and this is a measuring tool, not a
        // camera control.
        ApplyYaw(head.orientation, debugYaw);
        // ...and the eyes have to swing round the head with it, not only turn.
        // Turning just their direction left both eyes side by side across the
        // room's axis while the head faced 30 degrees away - a pose no headset
        // produces, and one that made a correct recenter measure wrong.
        const float a = debugYaw * 3.14159265f / 180.0f, c = cosf(a), s = sinf(a);
        for (int e = 0; e < eyeCount; ++e) {
            ApplyYaw(views[e].pose.orientation, debugYaw);
            Vec3& p = views[e].pose.position;
            const float dx = p.x - head.position.x, dz = p.z - head.position.z;
            p.x = head.position.x + dx * c + dz * s;
            p.z = head.position.z - dx * s + dz * c;
        }
    }
    if (debugPitchUp != 0.0f || debugRollRight != 0.0f) {
        // The tilt in the head's own frame (about its +X, then its -Z), and
        // the same turn in the reference space for the eyes: each eye's
        // orientation and its offset from the head turn with it.
        auto mul = [](const Quat& a, const Quat& b) {
            return Quat{ a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                         a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                         a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                         a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
        };
        auto axis = [](float x, float y, float z, float deg) {
            const float h = deg * 3.14159265f / 360.0f, s = sinf(h);
            return Quat{ x * s, y * s, z * s, cosf(h) };
        };
        const Quat hq = head.orientation;
        const Quat tilt = mul(axis(1, 0, 0, debugPitchUp), axis(0, 0, 1, -debugRollRight));
        const Quat delta = mul(mul(hq, tilt), Quat{ -hq.x, -hq.y, -hq.z, hq.w });
        head.orientation = mul(hq, tilt);
        for (int e = 0; e < eyeCount; ++e) {
            views[e].pose.orientation = mul(delta, views[e].pose.orientation);
            Vec3& p = views[e].pose.position;
            const Quat off{ p.x - head.position.x, p.y - head.position.y, p.z - head.position.z, 0.0f };
            const Quat r = mul(mul(delta, off), Quat{ -delta.x, -delta.y, -delta.z, delta.w });
            p = { head.position.x + r.x, head.position.y + r.y, head.position.z + r.z };
        }
    }
    return true;
}

bool Session::BeginFrame() {
    if (m->session == XR_NULL_HANDLE) return false;
    m->PollEvents();
    if (!m->running) return false;

    m->eyesDrawn = false;
    m->panelDrawn = false;
    m->suppressWorld = false;
    m->frameState = { XR_TYPE_FRAME_STATE };
    if (!m->Check(xrWaitFrame(m->session, nullptr, &m->frameState), "xrWaitFrame")) return false;

    XrFrameBeginInfo bi{ XR_TYPE_FRAME_BEGIN_INFO };
    if (!m->Check(xrBeginFrame(m->session, &bi), "xrBeginFrame")) return false;
    m->frameBegun = true;   // from here EndFrame MUST be reached
    m->shouldRender = m->frameState.shouldRender == XR_TRUE;
    if (!m->shouldRender) return false;

    if (!m->LocateFrame()) return false;
    ApplyTestHand();
    return true;
}

namespace {
Quat QMul(const Quat& a, const Quat& b) {
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
Quat QAxis(float x, float y, float z, float degrees) {
    const float h = degrees * 0.5f * 3.14159265358979f / 180.0f, s = sinf(h);
    return { x * s, y * s, z * s, cosf(h) };
}
Vec3 QRotate(const Quat& q, const Vec3& v) {
    const Quat p{ v.x, v.y, v.z, 0 };
    const Quat c{ -q.x, -q.y, -q.z, q.w };
    const Quat r = QMul(QMul(q, p), c);
    return { r.x, r.y, r.z };
}
} // namespace

/*
    A test hook for measuring, never for play: PCVRXR_TEST_RIGHT_HAND="x y z
    yaw pitch roll" holds the right controller still at that offset from the
    head - metres in the head's own frame (x right, y up, z back, as OpenXR
    has it), then degrees of yaw (left +), pitch (up +) and roll about the
    head's axes - for both its grip and aim poses, every frame. It is read
    inside the layer, so two engines on this layer given the same value see
    exactly the same hand, whatever their own input code does. Unset, it does
    nothing at all.
*/
void Session::ApplyTestHand() {
    // PCVRXR_TEST_LEFT_HAND does the same for the left controller (hands[0]).
    static int state[2] = { -1, -1 };
    static Vec3 off[2];
    static Quat rot[2], aimTilt[2];
    static float aimAhead[2] = {};
    static const char* const names[2] = { "PCVRXR_TEST_LEFT_HAND", "PCVRXR_TEST_RIGHT_HAND" };
    for (int hand = 0; hand < 2; ++hand) {
        if (state[hand] < 0) {
            state[hand] = 0;
            const char* e = getenv(names[hand]);
            float v[8] = {};
            const int n = e ? sscanf(e, "%f %f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) : 0;
            if (n >= 6) {
                state[hand] = 1;
                off[hand] = { v[0], v[1], v[2] };
                rot[hand] = QMul(QMul(QAxis(0, 1, 0, v[3]), QAxis(1, 0, 0, v[4])), QAxis(0, 0, 1, v[5]));
                // A seventh value pitches the aim pose away from the grip about the
                // grip's own right axis, as a real controller's are (they differ by
                // tens of degrees): which of the two an engine follows is then
                // something a test can see.
                aimTilt[hand] = QAxis(1, 0, 0, v[6]);
                // An eighth moves the aim pose's origin that far ahead of the
                // grip's (metres, along the grip's -Z), as it is on a controller.
                aimAhead[hand] = v[7];
                m->Log("pcvr-xr: TEST %s hand held at %.3f %.3f %.3f m, yaw %.1f pitch %.1f roll %.1f from the head, "
                       "aim %.1f from the grip (%s) - this is not real input", hand ? "right" : "left",
                       v[0], v[1], v[2], v[3], v[4], v[5], v[6], names[hand]);
            }
        }
        if (state[hand] != 1) continue;
        Pose p;
        const Vec3 d = QRotate(m->head.orientation, off[hand]);
        p.position = { m->head.position.x + d.x, m->head.position.y + d.y, m->head.position.z + d.z };
        p.orientation = QMul(m->head.orientation, rot[hand]);
        p.tracked = true;
        HandState& h = m->hands[hand];
        h.active = true;
        h.grip = p;
        h.aim = p;
        h.aim.orientation = QMul(p.orientation, aimTilt[hand]);
        const Vec3 ahead = QRotate(p.orientation, { 0, 0, -aimAhead[hand] });
        h.aim.position = { p.position.x + ahead.x, p.position.y + ahead.y, p.position.z + ahead.z };
    }
}

void Session::BeginEye(int eye) {
    if (!m->frameBegun || !m->shouldRender || m->IsD3D()) return;
    Eye& e = m->eyes[eye];

    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!m->Check(xrAcquireSwapchainImage(e.swapchain, &ai, &e.index), "xrAcquireSwapchainImage")) return;
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(e.swapchain, &wi);

    // Either into our own target, or straight into the swapchain image. The
    // second is one full-eye copy per eye per frame cheaper - about 1.4 GB a
    // second at this size and 90 Hz - and is what the layer does unless a port
    // asks otherwise.
    m->gl.BindFramebuffer(GL_FRAMEBUFFER,
                          m->useSceneTarget ? e.sceneFbo : e.fbos[e.index]);

    // Say where colour goes, every frame.
    //
    // glDrawBuffer and glReadBuffer are state of the FRAMEBUFFER OBJECT, not
    // of the context - so whatever an engine last set while this framebuffer
    // was bound is still set the next time it is bound. An engine that names
    // GL_BACK, which does not exist on a user framebuffer, leaves this one
    // accepting no colour at all: the binding still reads back correctly, a
    // clear still works because it is not a draw, and every triangle after it
    // goes nowhere. Nothing about that looks like a bug from the outside.
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);

    glViewport(0, 0, e.width, e.height);

    // The sRGB encode is deliberately NOT forced on here.
    //
    // The swapchain format is sRGB because that is what compositors want, but
    // whether the encode happens on write depends on what the port's renderer
    // puts in the buffer. An engine that already works in sRGB writes finished
    // values and must have the encode OFF, or everything is corrected twice
    // and comes out pale. An engine working in linear wants it on. The layer
    // cannot know which, and guessing makes a whole-picture colour bug that
    // gets blamed on the headset.
    //
    // The clear is the port's too: some engines draw over every pixel and a
    // clear is wasted work, and some rely on what a clear would remove.
}

void Session::EndEye(int eye) {
    if (!m->frameBegun || !m->shouldRender || m->IsD3D()) return;
    Eye& e = m->eyes[eye];

    // The finished picture into the swapchain image, when the port rendered
    // into our own target instead.
    if (m->useSceneTarget) {
        static bool toldOnce = false;
        if (!toldOnce) {
            toldOnce = true;
            m->Log("pcvr-xr: copying eye %d from render target %u into swapchain image %u",
                   eye, e.sceneFbo, e.fbos[e.index]);
        }
        m->gl.BindFramebuffer(GL_READ_FRAMEBUFFER, e.sceneFbo);
        m->gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, e.fbos[e.index]);

        // Name the buffers on BOTH sides, here and not only in BeginEye.
        // These belong to the framebuffer object, so the swapchain one still
        // holds whatever was last set on it - and a blit into a framebuffer
        // whose draw buffer names an attachment it does not have writes
        // nothing, raises an error nobody is checking, and leaves the
        // compositor showing black.
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);

        /*
            The copy sets its own state.

            A blit is not exempt from the pipeline's write controls: it is
            clipped by the scissor rectangle and masked by the colour mask,
            exactly like a draw. A port that leaves a scissor box of nothing
            behind - which a vignette or a letterbox does - makes this copy
            write nothing, with no error to find, and the compositor shows
            black while the port's own render target holds a perfect picture.
            That is a very confusing place to end up.
        */
        GLboolean hadScissor = 0;
        GLint hadMask[4] = { 1, 1, 1, 1 };
        glGetBooleanv(GL_SCISSOR_TEST, &hadScissor);
        glGetIntegerv(GL_COLOR_WRITEMASK, hadMask);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        static bool toldScissor = false;
        if (hadScissor && !toldScissor) {
            toldScissor = true;
            m->Log("pcvr-xr: the port left the scissor test on at EndEye - "
                   "the copy into the swapchain image would have been clipped");
        }

        // Drain whatever the port left pending. GL's error flag is sticky, so
        // reading it after the blit without clearing it first reports the
        // port's last mistake as ours.
        while (glGetError() != 0) {}

        /*
            Is the swapchain texture still the swapchain texture?

            An engine that assigns its OWN texture names rather than taking
            them from glGenTextures - which GoldSrc and its descendants do -
            can bind and respecify a name the runtime is using, because nothing
            tells it that name is taken. The framebuffer still reports COMPLETE
            because an attachment is still there; it is simply a different
            image now, of a different size, and the compositor reads the one it
            was given. Comparing the attached texture's dimensions against the
            ones we asked for catches it in one line.
        */
        m->gl.BlitFramebuffer(0, 0, e.width, e.height, 0, 0, e.width, e.height,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);

        const GLenum err = glGetError();
        if (err != 0) {
            static bool said = false;
            if (!said) {
                said = true;
                m->Log("pcvr-xr: the copy into the swapchain image failed, GL error 0x%x", err);
            }
        }
        // Put back what the port had, so nothing downstream sees a change it
        // did not make.
        if (hadScissor) glEnable(GL_SCISSOR_TEST);
        glColorMask((GLboolean)hadMask[0], (GLboolean)hadMask[1],
                    (GLboolean)hadMask[2], (GLboolean)hadMask[3]);

        m->gl.BindFramebuffer(GL_FRAMEBUFFER, e.fbos[e.index]);
    }

    // Before the release, not after. Once the image is released it belongs to
    // the runtime, and reading it back gets whatever the compositor has since
    // done with it - in practice a black frame, which reads as "the renderer
    // drew nothing" and sends you looking in entirely the wrong place.
    if (m->dumpRequested) m->DumpEye(eye);

    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(e.swapchain, &ri);

    m->projViews[eye].pose = m->xrViews[eye].pose;
    m->projViews[eye].fov = m->xrViews[eye].fov;
    m->projViews[eye].subImage.swapchain = e.swapchain;
    m->projViews[eye].subImage.imageRect.offset = { 0, 0 };
    m->projViews[eye].subImage.imageRect.extent = { e.width, e.height };
    m->projViews[eye].subImage.imageArrayIndex = 0;
    m->eyesDrawn = true;

    /*
        Is the swapchain image still the swapchain image?

        Checked once, on the first eye of the first frame, because the failure
        it catches is otherwise one of the most expensive in this whole layer.
        An engine that assigns its own graphics texture names rather than
        asking the driver for them - and engines of a certain age do, using a
        texture's index in their own array - will upload over the texture the
        runtime gave us. Afterwards the framebuffer still reports COMPLETE
        because something is still attached; it is simply a small game texture,
        so every write is clipped away and the compositor is shown whatever the
        engine last uploaded. No error, no wrong state, nothing to find.

        One check, and the port is told in a sentence what would otherwise take
        an evening. SwapchainTextures() is how it fixes it.
    */
    if (!m->checkedSwapchainTexture) {
        m->checkedSwapchainTexture = true;
        GLint tw = 0, th = 0;
        glBindTexture(GL_TEXTURE_2D, e.images[e.index].image);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (tw != e.width || th != e.height) {
            m->Log("pcvr-xr: THE PORT HAS OVERWRITTEN SWAPCHAIN TEXTURE %u - it is %dx%d, "
                   "not %dx%d. This engine assigns its own texture names; reserve the ones "
                   "SwapchainTextures() reports, or the compositor will be shown a game texture.",
                   e.images[e.index].image, tw, th, e.width, e.height);
        }
    }
}

void Session::EndFrame() {
    if (!m->frameBegun) return;

    /*
        A dump is spent only once it has written something.

        A frame the runtime told us not to render has no image to read, so
        clearing the request there throws the instrument away and answers the
        question with silence - and silence is indistinguishable from an empty
        picture, which sends you looking at the renderer when the session was
        never drawing in the first place. The request waits instead, and says
        so if it waits a long time.
    */
    if (m->dumpRequested) {
        if (m->dumpWrote) {
            m->dumpRequested = false;
            m->dumpWrote = false;
            m->dumpIndex++;
        } else if (++m->dumpWaited == 90) {
            m->Log("pcvr-xr: a dump has been waiting 90 frames with nothing to read - "
                   "the session is at %s and shouldRender is %s. Nothing is being "
                   "rendered, so there is no picture to dump.",
                   StateName(m->state), m->shouldRender ? "true" : "false");
        }
    }

    // Whatever was actually drawn this frame, in the order it composites: the
    // world underneath, the panel on top. Submitting a layer nothing was drawn
    // into shows the compositor a stale image, and submitting the world every
    // frame whether or not it was rendered is how a menu ends up with last
    // frame's level behind it.
    XrCompositionLayerProjection world{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    world.space = m->refSpace;
    world.viewCount = (uint32_t)m->projViews.size();
    world.views = m->projViews.data();

    XrCompositionLayerQuad panel{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    panel.space = m->refSpace;
    panel.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    panel.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    panel.subImage.swapchain = m->panelSwapchain;
    panel.subImage.imageRect.offset = { 0, 0 };
    panel.subImage.imageRect.extent = { m->panelW, m->panelH };
    panel.subImage.imageArrayIndex = 0;
    panel.pose.position = { m->panelPose.position.x, m->panelPose.position.y, m->panelPose.position.z };
    panel.pose.orientation = { m->panelPose.orientation.x, m->panelPose.orientation.y,
                               m->panelPose.orientation.z, m->panelPose.orientation.w };
    // Height from the TEXTURE's aspect, never from the game's canvas. Get this
    // wrong and a square texture on a 16:9-shaped quad stands 1.8x too tall
    // with everything on it stretched to match.
    panel.size.width = m->panelWidthMetres;
    panel.size.height = m->PanelHeightMetres();

    const XrCompositionLayerBaseHeader* layers[2] = {};
    uint32_t layerCount = 0;
    if (m->shouldRender && m->eyesDrawn && !m->suppressWorld)
        layers[layerCount++] = (XrCompositionLayerBaseHeader*)&world;
    if (m->shouldRender && m->panelDrawn) layers[layerCount++] = (XrCompositionLayerBaseHeader*)&panel;

    XrFrameEndInfo ei{ XR_TYPE_FRAME_END_INFO };
    ei.displayTime = m->frameState.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layerCount;
    ei.layers = layerCount ? layers : nullptr;

    m->Check(xrEndFrame(m->session, &ei), "xrEndFrame");
    m->frameBegun = false;
    if (!m->IsD3D()) m->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ---------------------------------------------------------------------------
// D3D11: the port renders each eye itself and hands over a finished texture.
// ---------------------------------------------------------------------------

bool Session::Impl::CopyIntoSwapchain(XrSwapchain sc, const std::vector<XrSwapchainImageD3D11KHR>& imgs,
                                      uint32_t& index, void* tex, int w, int h, const char* what) {
    if (tex == nullptr || d3dContext == nullptr || imgs.empty()) return false;
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!Check(xrAcquireSwapchainImage(sc, &ai, &index), "xrAcquireSwapchainImage")) return false;
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sc, &wi);

    ID3D11Texture2D* src = (ID3D11Texture2D*)tex;
    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);
    // The first few times: what the port is handing over. A size or format
    // mismatch makes CopySubresourceRegion do NOTHING, with no error from the
    // immediate context - the compositor is then shown a black eye while the
    // port's texture holds a perfect picture.
    static int said = 0;
    if (said < 3) {
        ++said;
        Log("pcvr-xr: %s texture %ux%u, DXGI format %d, %u samples -> swapchain %dx%d format %d",
            what, sd.Width, sd.Height, (int)sd.Format, sd.SampleDesc.Count, w, h, (int)chosenFormat);
    }
    if ((int)sd.Width < w || (int)sd.Height < h || sd.SampleDesc.Count != 1) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            Log("pcvr-xr: %s texture cannot be copied (too small, or multisampled) - nothing submitted", what);
        }
    } else {
        D3D11_BOX box{ 0, 0, 0, (UINT)w, (UINT)h, 1 };
        d3dContext->CopySubresourceRegion(imgs[index].texture, 0, 0, 0, 0, src, 0, &box);
    }
    return true;
}

bool Session::SubmitEyeTexture(int eye, void* texture) {
    if (!m->frameBegun || !m->shouldRender || !m->IsD3D() || eye < 0 || eye > 1) return false;
    Eye& e = m->eyes[eye];
    if (!m->CopyIntoSwapchain(e.swapchain, e.d3dImages, e.index, texture, e.width, e.height,
                              eye == 0 ? "eye 0" : "eye 1"))
        return false;
    if (m->dumpRequested) {
        char path[512];
        snprintf(path, sizeof(path), "%s\\eye%d_%03d.ppm", m->dumpDir.c_str(), eye, m->dumpIndex);
        m->DumpD3D(e.d3dImages[e.index].texture, e.width, e.height, path, nullptr);
        const XrPosef& p = m->xrViews[eye].pose;
        m->Log("pcvr-xr: eye %d pose pos %.4f %.4f %.4f  quat %.4f %.4f %.4f %.4f", eye,
               p.position.x, p.position.y, p.position.z,
               p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w);
    }
    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(e.swapchain, &ri);
    m->projViews[eye].pose = m->xrViews[eye].pose;
    m->projViews[eye].fov = m->xrViews[eye].fov;
    m->projViews[eye].subImage.swapchain = e.swapchain;
    m->projViews[eye].subImage.imageRect.offset = { 0, 0 };
    m->projViews[eye].subImage.imageRect.extent = { e.width, e.height };
    m->projViews[eye].subImage.imageArrayIndex = 0;
    m->eyesDrawn = true;
    return true;
}

bool Session::SubmitPanelTexture(void* texture) {
    if (!m->frameBegun || !m->shouldRender || !m->IsD3D() || !m->panelReady) return false;
    if (!m->CopyIntoSwapchain(m->panelSwapchain, m->panelD3DImages, m->panelIndex, texture,
                              m->panelW, m->panelH, "panel"))
        return false;
    if (m->dumpRequested) {
        char path[512], apath[512];
        snprintf(path, sizeof(path), "%s\\panel_%03d.ppm", m->dumpDir.c_str(), m->dumpIndex);
        snprintf(apath, sizeof(apath), "%s\\panel_%03d_alpha.pgm", m->dumpDir.c_str(), m->dumpIndex);
        m->DumpD3D(m->panelD3DImages[m->panelIndex].texture, m->panelW, m->panelH, path, apath);
    }
    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(m->panelSwapchain, &ri);
    m->panelDrawn = true;
    return true;
}

void Session::Impl::DumpD3D(ID3D11Texture2D* tex, int w, int h, const char* path, const char* alphaPath) {
    // Read back through a staging copy, BEFORE the image is released - after
    // that it belongs to the runtime and reads back as whatever it did next.
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(d3dDevice->CreateTexture2D(&d, nullptr, &staging))) {
        Log("pcvr-xr: could not make a staging texture to dump %s", path);
        return;
    }
    d3dContext->CopyResource(staging, tex);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (SUCCEEDED(d3dContext->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
        FILE* f = fopen(path, "wb");
        FILE* fa = alphaPath ? fopen(alphaPath, "wb") : nullptr;
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            if (fa) fprintf(fa, "P5\n%d %d\n255\n", w, h);
            std::vector<uint8_t> rgb((size_t)w * 3), a((size_t)w);
            const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
            for (int y = 0; y < h; ++y) {
                const uint8_t* row = (const uint8_t*)map.pData + (size_t)y * map.RowPitch;
                for (int x = 0; x < w; ++x) {
                    rgb[x * 3 + 0] = row[x * 4 + (bgra ? 2 : 0)];
                    rgb[x * 3 + 1] = row[x * 4 + 1];
                    rgb[x * 3 + 2] = row[x * 4 + (bgra ? 0 : 2)];
                    a[x] = row[x * 4 + 3];
                }
                fwrite(rgb.data(), 1, rgb.size(), f);
                if (fa) fwrite(a.data(), 1, a.size(), fa);
            }
            fclose(f);
            dumpWrote = true;
            Log("pcvr-xr: dumped %s (%dx%d)", path, w, h);
        } else {
            Log("pcvr-xr: could not write %s", path);
        }
        if (fa) fclose(fa);
        d3dContext->Unmap(staging, 0);
    }
    staging->Release();
}

// ---------------------------------------------------------------------------
// The split frame: WaitFrame on the game thread, SubmitFrame on the render
// thread. See the header for why.
// ---------------------------------------------------------------------------

uint64_t Session::WaitFrame() {
    if (m->session == XR_NULL_HANDLE || !m->IsD3D()) return 0;
    m->PollEvents();
    if (!m->running) return 0;

    XrFrameState fs{ XR_TYPE_FRAME_STATE };
    if (!m->Check(xrWaitFrame(m->session, nullptr, &fs), "xrWaitFrame")) return 0;
    m->frameState = fs;
    m->shouldRender = fs.shouldRender == XR_TRUE;
    bool located = m->shouldRender && m->LocateFrame();
    if (located) ApplyTestHand();

    const uint64_t ticket = m->nextTicket++;
    Impl::FrameRecord& r = m->ring[ticket % Impl::kRing];
    r.ticket = ticket;
    r.state = fs;
    r.shouldRender = located;
    for (int e = 0; e < 2; ++e) r.views[e] = m->xrViews.size() > (size_t)e ? m->xrViews[e] : XrView{ XR_TYPE_VIEW };
    r.panelPose = m->panelPose;
    r.panelW = m->panelWidthMetres;
    r.panelH = m->PanelHeightMetres();
    r.dump = m->dumpRequested;
    return ticket;
}

bool Session::FrameWillRender(uint64_t ticket) const {
    const Impl::FrameRecord& r = m->ring[ticket % Impl::kRing];
    return r.ticket == ticket && r.shouldRender;
}

void Session::SubmitFrame(uint64_t ticket, void* eye0, void* eye1, void* panel) {
    if (ticket == 0 || m->session == XR_NULL_HANDLE || !m->IsD3D()) return;
    Impl::FrameRecord& r = m->ring[ticket % Impl::kRing];
    if (r.ticket != ticket) {
        m->Log("pcvr-xr: SubmitFrame for ticket %llu, which the ring no longer holds - frame dropped",
               (unsigned long long)ticket);
        return;
    }
    if (!m->running) return;

    XrFrameBeginInfo bi{ XR_TYPE_FRAME_BEGIN_INFO };
    if (!m->Check(xrBeginFrame(m->session, &bi), "xrBeginFrame")) return;

    XrCompositionLayerProjectionView pv[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
                                               { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
    bool eyes = false, panelDrawn = false;
    if (r.shouldRender && eye0 != nullptr && eye1 != nullptr) {
        void* tex[2] = { eye0, eye1 };
        eyes = true;
        for (int e = 0; e < 2; ++e) {
            Eye& eye = m->eyes[e];
            if (!m->CopyIntoSwapchain(eye.swapchain, eye.d3dImages, eye.index, tex[e], eye.width, eye.height,
                                      e == 0 ? "eye 0" : "eye 1")) { eyes = false; break; }
            if (r.dump) {
                char path[512];
                snprintf(path, sizeof(path), "%s\\eye%d_%03d.ppm", m->dumpDir.c_str(), e, m->dumpIndex);
                m->DumpD3D(eye.d3dImages[eye.index].texture, eye.width, eye.height, path, nullptr);
                const XrPosef& p = r.views[e].pose;
                m->Log("pcvr-xr: eye %d pose pos %.4f %.4f %.4f  quat %.4f %.4f %.4f %.4f", e,
                       p.position.x, p.position.y, p.position.z,
                       p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w);
            }
            XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(eye.swapchain, &ri);
            pv[e].pose = r.views[e].pose;
            pv[e].fov = r.views[e].fov;
            pv[e].subImage.swapchain = eye.swapchain;
            pv[e].subImage.imageRect.offset = { 0, 0 };
            pv[e].subImage.imageRect.extent = { eye.width, eye.height };
            pv[e].subImage.imageArrayIndex = 0;
        }
    }
    if (r.shouldRender && panel != nullptr && m->panelReady) {
        if (m->CopyIntoSwapchain(m->panelSwapchain, m->panelD3DImages, m->panelIndex, panel,
                                 m->panelW, m->panelH, "panel")) {
            if (r.dump) {
                char path[512], apath[512];
                snprintf(path, sizeof(path), "%s\\panel_%03d.ppm", m->dumpDir.c_str(), m->dumpIndex);
                snprintf(apath, sizeof(apath), "%s\\panel_%03d_alpha.pgm", m->dumpDir.c_str(), m->dumpIndex);
                m->DumpD3D(m->panelD3DImages[m->panelIndex].texture, m->panelW, m->panelH, path, apath);
            }
            XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(m->panelSwapchain, &ri);
            panelDrawn = true;
        }
    }

    XrCompositionLayerProjection world{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    world.space = m->refSpace;
    world.viewCount = 2;
    world.views = pv;

    XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    quad.space = m->refSpace;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.subImage.swapchain = m->panelSwapchain;
    quad.subImage.imageRect.offset = { 0, 0 };
    quad.subImage.imageRect.extent = { m->panelW, m->panelH };
    quad.subImage.imageArrayIndex = 0;
    quad.pose.position = { r.panelPose.position.x, r.panelPose.position.y, r.panelPose.position.z };
    quad.pose.orientation = { r.panelPose.orientation.x, r.panelPose.orientation.y,
                              r.panelPose.orientation.z, r.panelPose.orientation.w };
    quad.size.width = r.panelW;
    quad.size.height = r.panelH;

    const XrCompositionLayerBaseHeader* layers[2] = {};
    uint32_t n = 0;
    if (eyes) layers[n++] = (XrCompositionLayerBaseHeader*)&world;
    if (panelDrawn) layers[n++] = (XrCompositionLayerBaseHeader*)&quad;

    XrFrameEndInfo ei{ XR_TYPE_FRAME_END_INFO };
    ei.displayTime = r.state.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = n;
    ei.layers = n ? layers : nullptr;
    m->Check(xrEndFrame(m->session, &ei), "xrEndFrame");

    if (r.dump && m->dumpWrote) {
        m->dumpRequested = false;
        m->dumpWrote = false;
        m->dumpIndex++;
    }
    r.ticket = 0;   // spent
}

void Session::Impl::DumpEye(int e) {
    std::vector<uint8_t> px((size_t)eyeW * eyeH * 3);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, eyes[e].fbos[eyes[e].index]);
    // Name the attachment. Read-buffer state belongs to the framebuffer
    // object, so whatever was last set on this one is still set - and reading
    // an attachment it does not have returns zeros from a perfectly good
    // picture, which makes the instrument the bug.
    glReadBuffer(GL_COLOR_ATTACHMENT0);

    /*
        A readback has to set its own pack state.

        If the port left a buffer bound to GL_PIXEL_PACK_BUFFER, glReadPixels
        treats the pointer it is given as an OFFSET INTO THAT BUFFER and writes
        nothing to memory at all. The destination stays as it was allocated,
        and a fresh allocation from the operating system is zero-filled - so
        the dump comes out perfectly black, with no error, from a picture that
        is perfectly fine. Row length and skip do the same in subtler ways.
    */
    {
        GLint packBuffer = 0, rowLength = 0, skipPixels = 0, skipRows = 0;
        glGetIntegerv(0x88ED /*GL_PIXEL_PACK_BUFFER_BINDING*/, &packBuffer);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
        if (packBuffer != 0 || rowLength != 0 || skipPixels != 0 || skipRows != 0) {
            Log("pcvr-xr: the port left pack state set (buffer %d, row %d, skip %d/%d)"
                " - the dump would have been blank", packBuffer, rowLength, skipPixels, skipRows);
        }
        if (packBuffer != 0 && gl.BindBuffer != nullptr) {
            gl.BindBuffer(0x88EB /*GL_PIXEL_PACK_BUFFER*/, 0);
        }
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    }

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, eyeW, eyeH, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    // Put the draw target back: a diagnostic that changes the state the caller
    // is drawing in stops being a diagnostic.
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl.BindFramebuffer(GL_FRAMEBUFFER, eyes[e].fbos[eyes[e].index]);

    char path[512];
    snprintf(path, sizeof(path), "%s\\eye%d_%03d.ppm", dumpDir.c_str(), e, dumpIndex);
    FILE* f = fopen(path, "wb");
    if (f != nullptr) {
        fprintf(f, "P6\n%d %d\n255\n", eyeW, eyeH);
        // GL's origin is bottom-left and a PPM's is top-left: write the rows
        // backwards or every dump comes out upside down.
        for (int y = eyeH - 1; y >= 0; --y) {
            fwrite(px.data() + (size_t)y * eyeW * 3, 1, (size_t)eyeW * 3, f);
        }
        fclose(f);
        dumpWrote = true;
        /*
            The pose the picture was drawn from, beside the picture.

            Two eyes that differ by more than parallax is one of the few
            picture faults a dump alone cannot tell apart from a scene: a
            rotated second eye and a genuinely different view look identical in
            a still. The numbers separate them in one line - the orientations
            should be equal and the positions should differ by the IPD along
            the head's right, and nothing else.
        */
        const XrPosef& p = xrViews[e].pose;
        Log("pcvr-xr: eye %d dumped to %s (%dx%d), pose pos %.4f %.4f %.4f  "
            "quat %.4f %.4f %.4f %.4f",
            e, path, eyeW, eyeH,
            p.position.x, p.position.y, p.position.z,
            p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w);
    } else {
        Log("pcvr-xr: could not write %s", path);
    }
}

void Session::Shutdown() {
    if (m == nullptr) return;

    // A frame that was begun has to be ended even now, or the runtime is left
    // mid-frame and the next process to ask for a session finds it busy.
    if (m->frameBegun) {
        XrFrameEndInfo ei{ XR_TYPE_FRAME_END_INFO };
        ei.displayTime = m->frameState.predictedDisplayTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        xrEndFrame(m->session, &ei);
        m->frameBegun = false;
    }
    if (m->running) { xrEndSession(m->session); m->running = false; }

    for (int e = 0; e < 2; ++e) {
        Eye& eye = m->eyes[e];
        if (!eye.fbos.empty() && m->gl.DeleteFramebuffers) {
            m->gl.DeleteFramebuffers((GLsizei)eye.fbos.size(), eye.fbos.data());
            eye.fbos.clear();
        }
        if (eye.depth != 0 && m->gl.DeleteRenderbuffers) {
            m->gl.DeleteRenderbuffers(1, &eye.depth);
            eye.depth = 0;
        }
        if (eye.sceneFbo != 0 && m->gl.DeleteFramebuffers) {
            m->gl.DeleteFramebuffers(1, &eye.sceneFbo);
            eye.sceneFbo = 0;
        }
        if (eye.sceneColour != 0 && m->gl.DeleteRenderbuffers) {
            m->gl.DeleteRenderbuffers(1, &eye.sceneColour);
            eye.sceneColour = 0;
        }
        if (eye.sceneDepth != 0 && m->gl.DeleteRenderbuffers) {
            m->gl.DeleteRenderbuffers(1, &eye.sceneDepth);
            eye.sceneDepth = 0;
        }
        if (eye.swapchain != XR_NULL_HANDLE) { xrDestroySwapchain(eye.swapchain); eye.swapchain = XR_NULL_HANDLE; }
        eye.images.clear();
    }
    for (int h = 0; h < 2; ++h) {
        if (m->aimSpace[h] != XR_NULL_HANDLE) { xrDestroySpace(m->aimSpace[h]); m->aimSpace[h] = XR_NULL_HANDLE; }
        if (m->gripSpace[h] != XR_NULL_HANDLE) { xrDestroySpace(m->gripSpace[h]); m->gripSpace[h] = XR_NULL_HANDLE; }
    }
    if (!m->panelFbos.empty() && m->gl.DeleteFramebuffers) {
        m->gl.DeleteFramebuffers((GLsizei)m->panelFbos.size(), m->panelFbos.data());
        m->panelFbos.clear();
    }
    if (m->panelSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m->panelSwapchain);
        m->panelSwapchain = XR_NULL_HANDLE;
    }
    m->panelImages.clear();
    m->panelReady = false;
    if (m->actionSet != XR_NULL_HANDLE) { xrDestroyActionSet(m->actionSet); m->actionSet = XR_NULL_HANDLE; }
    m->inputReady = false;
    if (m->refSpace != XR_NULL_HANDLE) { xrDestroySpace(m->refSpace); m->refSpace = XR_NULL_HANDLE; }
    if (m->viewSpace != XR_NULL_HANDLE) { xrDestroySpace(m->viewSpace); m->viewSpace = XR_NULL_HANDLE; }

    // Session before instance, and both before the graphics context the caller
    // owns. Out of order leaves threads alive and the process outlives its
    // window with nothing on screen and nothing in Task Manager to kill.
    if (m->session != XR_NULL_HANDLE) { xrDestroySession(m->session); m->session = XR_NULL_HANDLE; }
    for (int e = 0; e < 2; ++e) m->eyes[e].d3dImages.clear();
    m->panelD3DImages.clear();
    if (m->d3dContext != nullptr) { m->d3dContext->Release(); m->d3dContext = nullptr; }
    if (m->instance != XR_NULL_HANDLE) {
        xrDestroyInstance(m->instance);
        m->instance = XR_NULL_HANDLE;
        m->connected = false;
        m->Log("pcvr-xr: shut down");
    }
}

// ---------------------------------------------------------------------------
// Matrices
// ---------------------------------------------------------------------------

void ProjectionMatrix(const Fov& fov, float nearZ, float farZ, float out[16]) {
    const float tanL = tanf(fov.left);    // negative
    const float tanR = tanf(fov.right);
    const float tanU = tanf(fov.up);
    const float tanD = tanf(fov.down);    // negative
    const float w = tanR - tanL;
    const float h = tanU - tanD;
    const float d = farZ - nearZ;

    memset(out, 0, sizeof(float) * 16);
    out[0]  = 2.0f / w;
    out[5]  = 2.0f / h;
    out[8]  = (tanR + tanL) / w;   // the offset that is wrong half the time
    out[9]  = (tanU + tanD) / h;
    out[10] = -(farZ + nearZ) / d;
    out[11] = -1.0f;
    out[14] = -(2.0f * farZ * nearZ) / d;
}

void ViewMatrix(const Pose& pose, float out[16]) {
    const Quat& q = pose.orientation;
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

    // Rotation, transposed: a view matrix is the inverse of the pose, and for a
    // unit quaternion the inverse rotation is the transpose.
    const float r[9] = {
        1 - 2 * (yy + zz),     2 * (xy + wz),     2 * (xz - wy),
            2 * (xy - wz), 1 - 2 * (xx + zz),     2 * (yz + wx),
            2 * (xz + wy),     2 * (yz - wx), 1 - 2 * (xx + yy)
    };
    memset(out, 0, sizeof(float) * 16);
    out[0] = r[0]; out[4] = r[1]; out[8]  = r[2];
    out[1] = r[3]; out[5] = r[4]; out[9]  = r[5];
    out[2] = r[6]; out[6] = r[7]; out[10] = r[8];

    const Vec3& p = pose.position;
    out[12] = -(out[0] * p.x + out[4] * p.y + out[8]  * p.z);
    out[13] = -(out[1] * p.x + out[5] * p.y + out[9]  * p.z);
    out[14] = -(out[2] * p.x + out[6] * p.y + out[10] * p.z);
    out[15] = 1.0f;
}

} // namespace pcvrxr
