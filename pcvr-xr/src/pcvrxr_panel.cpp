// pcvr-xr - the flat panel: a quad compositor layer for menus and 2D.
//
// Copyright (c) 2026 Game Or Die
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Why this is a layer of its own rather than something drawn into the eyes:
// the compositor samples a quad layer once, at its own resolution, so text on
// it stays sharp. The same menu drawn into the eye buffers is resampled twice -
// once by the game's projection and once by the runtime's distortion - and
// looks soft at exactly the moment the player is trying to read it.

#include "internal.h"

namespace pcvrxr {

bool Session::CreatePanel(const PanelConfig& cfg) {
    if (m->session == XR_NULL_HANDLE) return false;
    if (m->panelReady) return true;

    m->panelW = cfg.width > 0 ? cfg.width : m->eyeW;
    m->panelH = cfg.height > 0 ? cfg.height : m->eyeH;
    m->panelWidthMetres = cfg.widthMetres > 0.01f ? cfg.widthMetres : 3.0f;
    m->panelHeightMetres = cfg.heightMetres > 0.01f ? cfg.heightMetres : 0.0f;

    uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(m->session, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    xrEnumerateSwapchainFormats(m->session, formatCount, &formatCount, formats.data());
    int64_t chosen = formats.empty() ? 0 : formats[0];
    for (int64_t f : formats) { if (f == 0x8C43 /*GL_SRGB8_ALPHA8*/) { chosen = f; break; } }
    if (m->IsD3D()) chosen = m->chosenFormat;   // the eyes' family: a byte copy

    XrSwapchainCreateInfo sci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    if (m->IsD3D()) sci.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = chosen;
    sci.sampleCount = 1;
    sci.width = m->panelW;
    sci.height = m->panelH;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    if (!m->Check(xrCreateSwapchain(m->session, &sci, &m->panelSwapchain), "xrCreateSwapchain (panel)"))
        return false;

    uint32_t imageCount = 0;
    xrEnumerateSwapchainImages(m->panelSwapchain, 0, &imageCount, nullptr);
#ifdef PCVRXR_D3D11
    if (m->IsD3D()) {
        m->panelD3DImages.assign(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        xrEnumerateSwapchainImages(m->panelSwapchain, imageCount, &imageCount,
            (XrSwapchainImageBaseHeader*)m->panelD3DImages.data());
    } else
#endif
    {
    m->panelImages.assign(imageCount, { PCVRXR_SWAPCHAIN_IMAGE_GL_TYPE });
    xrEnumerateSwapchainImages(m->panelSwapchain, imageCount, &imageCount,
        (XrSwapchainImageBaseHeader*)m->panelImages.data());

    m->panelFbos.resize(imageCount);
    m->gl.GenFramebuffers((GLsizei)imageCount, m->panelFbos.data());
    for (uint32_t i = 0; i < imageCount; ++i) {
        m->gl.BindFramebuffer(GL_FRAMEBUFFER, m->panelFbos[i]);
        m->gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D, m->panelImages[i].image, 0);
        // No depth attachment: a flat panel has nothing to sort. Giving it one
        // invites a port to leave the depth test on from the world pass and
        // then wonder why half the menu is missing.
        if (m->gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            m->failure = "the panel framebuffer is incomplete";
            m->Log("pcvr-xr: %s", m->failure.c_str());
            return false;
        }
    }
    m->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // Somewhere sensible until the port says otherwise: straight ahead, at the
    // height the reference space calls eye level.
    m->panelPose = Pose();
    m->panelPose.position = { 0.0f, m->cfg.space == Space::Standing ? 1.6f : m->cfg.seatedFloorDrop, -2.0f };
    m->panelPose.tracked = true;

    m->panelReady = true;
    const float h = m->PanelHeightMetres();
    m->Log("pcvr-xr: panel %dx%d, %.2f x %.2f m%s, %u images",
           m->panelW, m->panelH, m->panelWidthMetres, h,
           m->panelHeightMetres > 0.01f ? " (shape set by the port)" : "", imageCount);
    return true;
}

void Session::PanelSize(int& width, int& height) const {
    width = m->panelW;
    height = m->panelH;
}

void Session::SetPanelShape(float widthMetres, float heightMetres) {
    if (widthMetres > 0.01f) m->panelWidthMetres = widthMetres;
    m->panelHeightMetres = heightMetres > 0.01f ? heightMetres : 0.0f;
}

bool Session::PanelPlacement(Pose& pose, float& widthMetres, float& heightMetres) const {
    if (!m->panelReady) return false;
    pose = m->panelPose;
    widthMetres = m->panelWidthMetres;
    heightMetres = m->PanelHeightMetres();
    return true;
}

void Session::SetPanelPose(const Pose& pose) {
    m->panelPose = pose;
    m->panelPose.tracked = true;
}

void Session::PlacePanelInFrontOfHead(float distance) {
    const Pose& head = m->head;

    // Yaw only. Taking the head's full orientation hangs the menu at whatever
    // angle you happened to be looking, and then it tilts with every nod.
    const Quat& q = head.orientation;
    const float yaw = atan2f(2.0f * (q.w * q.y + q.x * q.z),
                             1.0f - 2.0f * (q.y * q.y + q.x * q.x));

    Pose p;
    p.position.x = head.position.x - sinf(yaw) * distance;
    p.position.y = head.position.y;
    p.position.z = head.position.z - cosf(yaw) * distance;
    p.orientation.x = 0.0f;
    p.orientation.y = sinf(yaw * 0.5f);
    p.orientation.z = 0.0f;
    p.orientation.w = cosf(yaw * 0.5f);
    p.tracked = true;
    m->panelPose = p;
}

void Session::SuppressWorld() { m->suppressWorld = true; }

void Session::BeginPanel() {
    if (!m->frameBegun || !m->shouldRender || !m->panelReady || m->IsD3D()) return;

    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!m->Check(xrAcquireSwapchainImage(m->panelSwapchain, &ai, &m->panelIndex),
                  "xrAcquireSwapchainImage (panel)")) {
        return;
    }
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(m->panelSwapchain, &wi);

    m->gl.BindFramebuffer(GL_FRAMEBUFFER, m->panelFbos[m->panelIndex]);
    // Its own viewport, every time. A 2D pass that does not set one inherits
    // whatever the last pass left - usually the desktop window's size - and
    // lands in a corner of a texture that is bigger than it expected.
    glViewport(0, 0, m->panelW, m->panelH);
#ifndef __ANDROID__
    // OpenGL ES encodes into an sRGB attachment by itself; desktop GL asks.
    glEnable(GL_FRAMEBUFFER_SRGB);
#endif
}

void Session::EndPanel() {
    if (!m->frameBegun || !m->shouldRender || !m->panelReady || m->IsD3D()) return;

    // Before the release, for the same reason the eyes are: once it is
    // released the image belongs to the runtime.
    if (m->dumpRequested) m->DumpPanel();

    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(m->panelSwapchain, &ri);
    m->panelDrawn = true;
}

void Session::Impl::DumpPanel() {
    // RGBA: the panel is composited with its alpha, so what is transparent is
    // as much a part of the picture as what is drawn. A panel whose empty
    // areas are opaque black hides the world behind it, and an RGB dump of it
    // looks exactly like one whose empty areas are clear.
    std::vector<uint8_t> px((size_t)panelW * panelH * 4);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, panelFbos[panelIndex]);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, panelW, panelH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl.BindFramebuffer(GL_FRAMEBUFFER, panelFbos[panelIndex]);

    char path[512], apath[512];
    snprintf(path, sizeof(path), "%s" PCVRXR_SEP "panel_%03d.ppm", dumpDir.c_str(), dumpIndex);
    snprintf(apath, sizeof(apath), "%s" PCVRXR_SEP "panel_%03d_alpha.pgm", dumpDir.c_str(), dumpIndex);
    FILE* f = fopen(path, "wb");
    FILE* fa = fopen(apath, "wb");
    if (f != nullptr && fa != nullptr) {
        fprintf(f, "P6\n%d %d\n255\n", panelW, panelH);
        fprintf(fa, "P5\n%d %d\n255\n", panelW, panelH);
        std::vector<uint8_t> rgb((size_t)panelW * 3), a((size_t)panelW);
        for (int y = panelH - 1; y >= 0; --y) {
            const uint8_t* row = px.data() + (size_t)y * panelW * 4;
            for (int x = 0; x < panelW; ++x) {
                rgb[x * 3 + 0] = row[x * 4 + 0];
                rgb[x * 3 + 1] = row[x * 4 + 1];
                rgb[x * 3 + 2] = row[x * 4 + 2];
                a[x] = row[x * 4 + 3];
            }
            fwrite(rgb.data(), 1, rgb.size(), f);
            fwrite(a.data(), 1, a.size(), fa);
        }
        fclose(f);
        fclose(fa);
        dumpWrote = true;
        const float h = PanelHeightMetres();
        Log("pcvr-xr: panel dumped to %s (%dx%d, hanging %.3f x %.3f m)",
            path, panelW, panelH, panelWidthMetres, h);
    } else {
        if (f) fclose(f);
        if (fa) fclose(fa);
        Log("pcvr-xr: could not write %s", path);
    }
}

} // namespace pcvrxr
