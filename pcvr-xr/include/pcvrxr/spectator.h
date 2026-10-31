// pcvr-xr - spectator.h
// Copyright (c) 2026 Game Or Die
// SPDX-License-Identifier: GPL-3.0-or-later
//
// THE SPECTATOR VIEW: what the monitor shows while someone plays, for a stream
// or a recording. The raw eye shakes with every small movement of the head and
// tilts with every lean, and nobody can watch that for long. This picks a
// smaller cut-out of the eye that follows the head's TURNS but not its wobble,
// turned so the world's horizon stays level on the screen.
//
// It needs nothing from the engine: only the eye picture's size and what the
// port already told the runtime about it - the orientation it was drawn from
// and its field of view (the View the frame was predicted with). It draws
// nothing either: it returns the cut-out's four corners in the eye picture,
// and the port draws that quad to its window however its renderer draws.
// No extra render and no engine camera.
//
// Written for pcvr-xr from the OpenXR 1.1 specification (XrFovf,
// XrQuaternionf, the view space's -Z forward and +Y up).

#pragma once

#include "pcvrxr/pcvrxr.h"

namespace pcvrxr {

// Where the monitor's picture comes from in the eye picture. Coordinates are
// fractions of the eye picture, x to the right and y DOWN from its top-left.
// The corners are the monitor's top-left, top-right, bottom-right and
// bottom-left, in that order. The quad is a rotated rectangle when steady.
struct SpectatorCrop {
    Vec2  corner[4];
    float rollDeg = 0;   // how far the cut-out is turned to keep the horizon level
    bool  steady = false;// false: the plain, centred cut-out
    bool  atEdge = false;// the followed direction was held at the eye's edge
};

class Spectator {
public:
    // Call once per picture shown on the monitor, AFTER the frame is submitted,
    // with the View that picture was drawn and submitted with - a frame-old
    // pose puts the head's own motion back in. eyeW/eyeH: the picture's size
    // in pixels; outAspect: the monitor's width / height; seconds: a steady
    // clock. plain: menus and pause screens, the same cut-out centred and
    // upright (the filter restarts from the head when steady resumes).
    SpectatorCrop Update(const View& view, int eyeW, int eyeH, float outAspect,
                         double seconds, bool plain);

    // Forget the followed direction: the next Update starts from the head.
    void Reset() { has_ = false; }

    // The cut-out's width as a fraction of the eye's.
    float widthFraction = 0.78f;
    // The speed-adaptive (one-euro) filter on where the head points. Heavy at
    // rest (minCutoffHz), lighter as the head turns (+ beta per rad/s of turn);
    // the turn rate itself is filtered AS A VECTOR at rateCutoffHz first, so a
    // wobble's rate averages out instead of reading as a fast turn.
    float minCutoffHz = 0.5f;
    float beta = 2.0f;
    float rateCutoffHz = 0.5f;
    // A jump bigger than this (recenter, teleport) is taken at once.
    float jumpDeg = 45.0f;

private:
    bool   has_ = false;
    Vec3   followed_;      // unit, reference space: where the view looks
    Vec3   lastHead_;      // unit, reference space: the head's last forward
    Vec3   rate_;          // filtered rate of the head's forward, per second
    double lastT_ = 0;
    float  lastRoll_ = 0;  // kept when the head looks straight up or down
};

// Test support: where a direction in the reference space lands in a view's
// picture, in pixels (x right, y down); false when it is behind the eye.
bool SpectatorProject(const View& view, int eyeW, int eyeH, const Vec3& dir, float& px, float& py);
// The reverse: the reference-space direction a pixel of the picture shows.
Vec3 SpectatorUnproject(const View& view, int eyeW, int eyeH, float px, float py);

} // namespace pcvrxr
