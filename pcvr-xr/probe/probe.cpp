// pcvr-xr probe - the smallest thing that can prove the layer works.
//
// Copyright (c) 2026 Game Or Die
// SPDX-License-Identifier: GPL-3.0-or-later
//
// No engine, no game data, no headset. It makes a GL context, brings up a
// session, draws a scene whose geometry is known in advance into each eye,
// submits it, dumps both eyes to PPM and shuts down. Everything it draws is
// there to answer a specific question that has cost a day somewhere:
//
//   the green border      does the render cover the whole eye buffer, or is it
//                         sitting in a corner of it?
//   the corner marker     cyan in the left eye, magenta in the right - colours
//                         nothing else in the scene has, so a dump can be
//                         identified without matching it against anything.
//   the centre pole       its horizontal position differs between the eyes by
//                         the stereo disparity. Equal positions mean one eye's
//                         pose never reached the renderer.
//   the floor grid        sits at y=0 in the reference space. If the floor is
//                         at your knees, the seated fallback drop is wrong.
//   the pillars           asymmetric per-eye FOV shows as different amounts of
//                         each pillar visible at the left and right edges.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <cmath>

#include "pcvrxr/pcvrxr.h"

static FILE* g_log = nullptr;

static void Say(const char* line) {
    printf("%s\n", line);
    fflush(stdout);
    if (g_log) { fprintf(g_log, "%s\n", line); fflush(g_log); }
}

static void Sayf(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Say(buf);
}

// ---------------------------------------------------------------------------
// A window and a GL context. The port would already own these; the probe makes
// its own so it depends on nothing.
// ---------------------------------------------------------------------------

struct GLWindow {
    HWND wnd = nullptr;
    HDC dc = nullptr;
    HGLRC rc = nullptr;

    bool Create() {
        WNDCLASSA wc = {};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "pcvrxr_probe";
        RegisterClassA(&wc);

        wnd = CreateWindowExA(0, "pcvrxr_probe", "pcvr-xr probe",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640, 360,
            nullptr, nullptr, wc.hInstance, nullptr);
        if (!wnd) return false;

        dc = GetDC(wnd);
        PIXELFORMATDESCRIPTOR pfd = {};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cDepthBits = 24;
        int fmt = ChoosePixelFormat(dc, &pfd);
        if (fmt == 0 || !SetPixelFormat(dc, fmt, &pfd)) return false;

        rc = wglCreateContext(dc);
        if (!rc || !wglMakeCurrent(dc, rc)) return false;
        return true;
    }

    void Pump() {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    void Destroy() {
        if (rc) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(rc); rc = nullptr; }
        if (dc && wnd) { ReleaseDC(wnd, dc); dc = nullptr; }
        if (wnd) { DestroyWindow(wnd); wnd = nullptr; }
    }
};

// ---------------------------------------------------------------------------
// The scene
// ---------------------------------------------------------------------------

static void Pillar(float x, float z, float r, float g, float b) {
    const float w = 0.15f, h = 2.0f;
    glColor3f(r, g, b);
    glBegin(GL_QUADS);
    glVertex3f(x - w, 0.0f, z + w); glVertex3f(x + w, 0.0f, z + w);
    glVertex3f(x + w, h,    z + w); glVertex3f(x - w, h,    z + w);

    glVertex3f(x - w, 0.0f, z - w); glVertex3f(x - w, h,    z - w);
    glVertex3f(x + w, h,    z - w); glVertex3f(x + w, 0.0f, z - w);

    glColor3f(r * 0.6f, g * 0.6f, b * 0.6f);
    glVertex3f(x - w, 0.0f, z - w); glVertex3f(x - w, 0.0f, z + w);
    glVertex3f(x - w, h,    z + w); glVertex3f(x - w, h,    z - w);

    glVertex3f(x + w, 0.0f, z - w); glVertex3f(x + w, h,    z - w);
    glVertex3f(x + w, h,    z + w); glVertex3f(x + w, 0.0f, z + w);
    glEnd();
}

static void DrawScene(const pcvrxr::View& view, int eye, int width, int height) {
    float proj[16], modelview[16];
    pcvrxr::ProjectionMatrix(view.fov, 0.05f, 100.0f, proj);
    pcvrxr::ViewMatrix(view.pose, modelview);

    // The layer no longer forces this, so the probe asks for what it wants:
    // it draws linear values and wants them encoded on the way in.
    glEnable(0x8DB9 /*GL_FRAMEBUFFER_SRGB*/);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_CULL_FACE);

    // A fresh target is never clean: whatever this eye's swapchain image held
    // two frames ago is still in it.
    glClearColor(0.05f, 0.06f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(proj);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(modelview);

    // Floor grid at y = 0 in the reference space.
    glColor3f(0.25f, 0.30f, 0.35f);
    glBegin(GL_LINES);
    for (int i = -10; i <= 10; ++i) {
        glVertex3f((float)i * 0.5f, 0.0f, -5.0f);
        glVertex3f((float)i * 0.5f, 0.0f,  5.0f);
        glVertex3f(-5.0f, 0.0f, (float)i * 0.5f);
        glVertex3f( 5.0f, 0.0f, (float)i * 0.5f);
    }
    glEnd();

    // Pillars at the edges, where the asymmetric FOV shows.
    Pillar(-2.0f, -2.0f, 0.9f, 0.7f, 0.2f);
    Pillar( 2.0f, -2.0f, 0.2f, 0.8f, 0.4f);
    Pillar(-2.0f,  2.0f, 0.8f, 0.4f, 0.2f);
    Pillar( 2.0f,  2.0f, 0.4f, 0.5f, 0.9f);

    // The disparity pole: white, dead ahead, 2 m out.
    glColor3f(1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glVertex3f(-0.03f, 0.6f, -2.0f); glVertex3f(0.03f, 0.6f, -2.0f);
    glVertex3f( 0.03f, 2.0f, -2.0f); glVertex3f(-0.03f, 2.0f, -2.0f);
    glEnd();

    // Overlay, in pixels: everything from here answers "is this eye buffer
    // actually being filled", so it must not be clipped by the world's depth.
    glDisable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width, 0, height, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // A border along every edge. If the render landed in a corner of the eye
    // buffer, three of these four edges are missing.
    const float t = 6.0f;
    glColor3f(0.0f, 1.0f, 0.0f);
    glBegin(GL_QUADS);
    glVertex2f(0, 0); glVertex2f((float)width, 0); glVertex2f((float)width, t); glVertex2f(0, t);
    glVertex2f(0, (float)height - t); glVertex2f((float)width, (float)height - t);
    glVertex2f((float)width, (float)height); glVertex2f(0, (float)height);
    glVertex2f(0, 0); glVertex2f(t, 0); glVertex2f(t, (float)height); glVertex2f(0, (float)height);
    glVertex2f((float)width - t, 0); glVertex2f((float)width, 0);
    glVertex2f((float)width, (float)height); glVertex2f((float)width - t, (float)height);
    glEnd();

    // Which eye this is, in a colour nothing else uses.
    if (eye == 0) glColor3f(0.0f, 1.0f, 1.0f); else glColor3f(1.0f, 0.0f, 1.0f);
    const float m = 64.0f;
    glBegin(GL_QUADS);
    glVertex2f(20, (float)height - 20 - m); glVertex2f(20 + m, (float)height - 20 - m);
    glVertex2f(20 + m, (float)height - 20); glVertex2f(20, (float)height - 20);
    glEnd();

    glEnable(GL_DEPTH_TEST);
}

// ---------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------
//
// Drawn FLAT: an orthographic projection over the texture and nothing else. No
// eye offset, no head transform, no perspective. The quad layer's pose IS the
// placement - applying a camera to the texture as well places it twice, and
// the result looks like a menu that swims when you move.

static void DrawPanel(int width, int height) {
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_CULL_FACE);

    glClearColor(0.10f, 0.10f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width, 0, height, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // Border on every edge - the same corner-blit question the eyes ask.
    const float t = 6.0f;
    glColor3f(1.0f, 0.55f, 0.0f);
    glBegin(GL_QUADS);
    glVertex2f(0, 0); glVertex2f((float)width, 0); glVertex2f((float)width, t); glVertex2f(0, t);
    glVertex2f(0, (float)height - t); glVertex2f((float)width, (float)height - t);
    glVertex2f((float)width, (float)height); glVertex2f(0, (float)height);
    glVertex2f(0, 0); glVertex2f(t, 0); glVertex2f(t, (float)height); glVertex2f(0, (float)height);
    glVertex2f((float)width - t, 0); glVertex2f((float)width, 0);
    glVertex2f((float)width, (float)height); glVertex2f((float)width - t, (float)height);
    glEnd();

    // A square, a quarter of the texture's SHORTER side, dead centre. On a
    // correctly sized quad it is square in the world too; on a quad sized from
    // the game's canvas instead of the texture it is visibly a rectangle - and
    // it is square in the dump either way, which is exactly why the dump alone
    // cannot answer that question and the logged metres have to.
    const float side = (width < height ? width : height) * 0.25f;
    const float cx = width * 0.5f, cy = height * 0.5f;
    glColor3f(0.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glVertex2f(cx - side * 0.5f, cy - side * 0.5f);
    glVertex2f(cx + side * 0.5f, cy - side * 0.5f);
    glVertex2f(cx + side * 0.5f, cy + side * 0.5f);
    glVertex2f(cx - side * 0.5f, cy + side * 0.5f);
    glEnd();
}

// ---------------------------------------------------------------------------
// The input test
// ---------------------------------------------------------------------------
//
// A headless run has no controllers, so "no button was pressed" is the only
// thing it can honestly report - and a null result with no positive control
// beside it proves nothing at all. This drives made-up controller state through
// exactly the same edge detection and the same port-facing struct a real
// controller uses, and checks each frame's answer against one worked out in
// advance. It cannot prove a Vive wand is bound correctly; it can prove that
// when a press arrives, the port is told once.

struct Step {
    bool hold;          // is the button held this frame
    bool wantPressed;
    bool wantDown;
    bool wantReleased;
    float trigger;      // analogue, passed straight through
    const char* what;
};

static const Step kSteps[] = {
    { false, false, false, false, 0.00f, "idle" },
    { true,  true,  true,  false, 1.00f, "pressed" },
    { true,  false, true,  false, 1.00f, "held - must not fire twice" },
    { true,  false, true,  false, 0.75f, "still held, analogue moved" },
    { false, false, false, true,  0.00f, "released" },
    { false, false, false, false, 0.00f, "idle again" },
    { true,  true,  true,  false, 1.00f, "tap down" },
    { false, false, false, true,  0.00f, "tap up on the very next frame" },
    { false, false, false, false, 0.00f, "idle" },
};
static const int kStepCount = (int)(sizeof(kSteps) / sizeof(kSteps[0]));

static void ApplyStep(pcvrxr::Session& s, int i) {
    if (i >= kStepCount) return;
    const Step& st = kSteps[i];

    // Left hand follows the script; the right hand never moves, which is the
    // control - a bug that writes both hands from one subaction path shows up
    // as the right hand pressing things nobody touched.
    pcvrxr::HandState left;
    left.active = true;
    left.triggerClick.down = st.hold;
    left.trigger = st.trigger;
    left.thumbstick.x = 0.5f;
    left.thumbstick.y = -0.25f;
    s.SetFakeHand(pcvrxr::Hand::Left, left);

    pcvrxr::HandState right;
    right.active = true;
    s.SetFakeHand(pcvrxr::Hand::Right, right);
}

static bool CheckStep(pcvrxr::Session& s, int i) {
    if (i >= kStepCount) return true;
    const Step& st = kSteps[i];
    const pcvrxr::HandState& L = s.GetHand(pcvrxr::Hand::Left);
    const pcvrxr::HandState& R = s.GetHand(pcvrxr::Hand::Right);

    bool ok = true;
    if (L.triggerClick.pressed != st.wantPressed) ok = false;
    if (L.triggerClick.down != st.wantDown) ok = false;
    if (L.triggerClick.released != st.wantReleased) ok = false;
    if (L.trigger != st.trigger) ok = false;
    if (L.thumbstick.x != 0.5f || L.thumbstick.y != -0.25f) ok = false;
    // The right hand must be untouched by any of it.
    if (R.triggerClick.down || R.triggerClick.pressed || R.triggerClick.released) ok = false;

    Sayf("  input %-34s %s  left: pressed=%d down=%d released=%d trigger=%.2f | right down=%d",
         st.what, ok ? "ok  " : "FAIL",
         (int)L.triggerClick.pressed, (int)L.triggerClick.down,
         (int)L.triggerClick.released, L.trigger, (int)R.triggerClick.down);
    return ok;
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    // Rendered frames, not loop iterations. A runtime takes a second or two to
    // promote a new application past SYNCHRONIZED into VISIBLE, and until it
    // does, every frame is a no-op that costs nothing - so a loop counted in
    // iterations finishes before the session has ever been asked to draw, and
    // reports that the layer renders nothing. That is the harness measuring
    // itself.
    int frames = 90;
    int seconds = 30;
    int dumpAt = 45;
    float yaw = 0.0f;
    float scale = 0.0f;
    bool seated = false;
    bool testInput = false;
    int panelW = 0, panelH = 0;
    float panelMetres = 0.0f;
    const char* dumpDir = ".";

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dumpDir = argv[++i];
        else if (!strcmp(argv[i], "--dump-at") && i + 1 < argc) dumpAt = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--yaw") && i + 1 < argc) yaw = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--seated")) seated = true;
        else if (!strcmp(argv[i], "--test-input")) testInput = true;
        else if (!strcmp(argv[i], "--panel") && i + 3 < argc) {
            panelW = atoi(argv[++i]);
            panelH = atoi(argv[++i]);
            panelMetres = (float)atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) g_log = fopen(argv[++i], "w");
    }

    GLWindow win;
    if (!win.Create()) { Say("probe: no OpenGL context - stopping"); return 2; }
    Sayf("probe: OpenGL %s on %s", (const char*)glGetString(GL_VERSION),
         (const char*)glGetString(GL_RENDERER));

    pcvrxr::Session session;
    session.SetLog(Say);

    pcvrxr::Config cfg;
    cfg.appName = "pcvr-xr probe";
    cfg.resolutionScale = scale;
    cfg.space = seated ? pcvrxr::Space::Seated : pcvrxr::Space::Standing;

    if (!session.BringUp(cfg)) {
        Sayf("probe: no VR this run - %s", session.FailureReason());
        win.Destroy();
        return 3;
    }
    session.SetDebugYaw(yaw);
    if (testInput) {
        Say("probe: driving made-up controller state - this is NOT real input");
        session.SetFakeHands(true);
    }

    if (panelMetres > 0.0f) {
        pcvrxr::PanelConfig pc;
        pc.width = panelW;
        pc.height = panelH;
        pc.widthMetres = panelMetres;
        if (!session.CreatePanel(pc)) {
            Sayf("probe: FAIL - the panel was not created: %s", session.FailureReason());
            session.Shutdown();
            win.Destroy();
            return 8;
        }
    }

    int w = 0, h = 0;
    session.EyeBufferSize(w, h);
    Sayf("probe: %s, eye buffer %dx%d", session.RuntimeName(), w, h);

    int rendered = 0;
    int spins = 0;
    int step = 0;
    int inputFailures = 0;
    bool dumped = false;
    bool inertChecked = false;
    bool inertOk = false;
    const DWORD deadline = GetTickCount() + (DWORD)seconds * 1000;

    while (rendered < frames) {
        if (GetTickCount() > deadline) {
            Sayf("probe: gave up after %d s with %d rendered frames and %d idle spins",
                 seconds, rendered, spins);
            break;
        }
        if (session.ExitRequested()) {
            Say("probe: the runtime asked us to stop");
            break;
        }
        spins++;
        win.Pump();
        if (testInput) ApplyStep(session, step);

        if (session.BeginFrame()) {
            if (rendered == 0) {
                for (int e = 0; e < session.EyeCount(); ++e) {
                    const pcvrxr::View& v = session.GetView(e);
                    Sayf("probe: eye %d fov L%.1f R%.1f U%.1f D%.1f deg, pose %.3f %.3f %.3f%s",
                        e, v.fov.left * 57.29578f, v.fov.right * 57.29578f,
                        v.fov.up * 57.29578f, v.fov.down * 57.29578f,
                        v.pose.position.x, v.pose.position.y, v.pose.position.z,
                        v.pose.tracked ? "" : " (NOT TRACKED)");
                }
                const pcvrxr::Pose& head = session.Head();
                Sayf("probe: head at %.3f %.3f %.3f%s - the floor is %.3f m below it",
                    head.position.x, head.position.y, head.position.z,
                    head.tracked ? "" : " (NOT TRACKED)", head.position.y);
            }

            if (testInput) {
                if (step < kStepCount) {
                    if (!CheckStep(session, step)) inputFailures++;
                    step++;
                } else if (!inertChecked) {
                    // A debug driver that is not completely inert when switched
                    // off will eventually be found holding somebody's trigger.
                    session.SetFakeHands(false);
                    inertChecked = true;
                } else {
                    const pcvrxr::HandState& L = session.GetHand(pcvrxr::Hand::Left);
                    inertOk = !L.active && !L.triggerClick.down &&
                              !L.triggerClick.pressed && L.trigger == 0.0f;
                    Sayf("  input %-34s %s  after switching fake hands off",
                         "inert when off", inertOk ? "ok  " : "FAIL");
                    if (!inertOk) inputFailures++;
                    break;
                }
            }

            if (rendered == 1) {
                for (int side = 0; side < 2; ++side) {
                    const pcvrxr::Hand hand = side == 0 ? pcvrxr::Hand::Left : pcvrxr::Hand::Right;
                    const pcvrxr::HandState& hs = session.GetHand(hand);
                    Sayf("probe: %s hand %s, profile %s",
                         side == 0 ? "left " : "right",
                         hs.active ? "active" : "not present",
                         session.InteractionProfile(hand));
                }
            }

            if (rendered == dumpAt) session.RequestEyeDump(dumpDir);

            for (int e = 0; e < session.EyeCount(); ++e) {
                session.BeginEye(e);
                DrawScene(session.GetView(e), e, w, h);
                session.EndEye(e);
            }

            // After the eyes, and after the dump has been asked for: the panel
            // is released at EndPanel and cannot be read back afterwards.
            if (panelMetres > 0.0f) {
                int pw = 0, ph = 0;
                session.PanelSize(pw, ph);
                if (rendered == 0) session.PlacePanelInFrontOfHead(2.5f);
                session.BeginPanel();
                DrawPanel(pw, ph);
                session.EndPanel();
            }
            if (rendered == dumpAt) dumped = true;
            rendered++;
        }
        // Always, whatever happened above.
        session.EndFrame();
    }

    Sayf("probe: %d of %d frames rendered over %d loop passes, eyes %s",
         rendered, frames, spins, dumped ? "dumped" : "NOT dumped");
    session.Shutdown();
    win.Destroy();

    if (testInput) {
        Sayf("probe: input test - %d of %d checks failed",
             inputFailures, kStepCount + 1);
        if (inputFailures != 0) { Say("probe: FAIL - the input test did not pass"); return 6; }
        if (!inertOk) { Say("probe: FAIL - the input test never finished"); return 7; }
    }
    if (rendered == 0) { Say("probe: FAIL - the session never rendered a frame"); return 4; }
    // An input run ends when its script does, which is usually before the dump
    // frame. Requiring a dump there fails the run for not doing something it
    // was never asked to do - and passing it depends on which frame the dump
    // was scheduled for, which is a test whose verdict comes from its own
    // arguments rather than from the subject.
    if (!dumped && !testInput) { Say("probe: FAIL - no eye dump was taken"); return 5; }
    Say("probe: OK");
    return 0;
}
