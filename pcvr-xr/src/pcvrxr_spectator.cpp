// pcvr-xr - pcvrxr_spectator.cpp - see include/pcvrxr/spectator.h
// Copyright (c) 2026 Game Or Die
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Written for pcvr-xr from the OpenXR 1.1 specification.

#include "pcvrxr/spectator.h"

#include <algorithm>
#include <cmath>

namespace pcvrxr {
namespace {

constexpr double kPi = 3.14159265358979323846;

// pcvr-xr builds as C++14: no std::clamp.
template <class T> T Clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

Vec3 Add(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Vec3 Sub(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
Vec3 Mul(Vec3 a, float k) { return { a.x * k, a.y * k, a.z * k }; }
float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
float Len(Vec3 a) { return std::sqrt(Dot(a, a)); }
Vec3 Norm(Vec3 a) { const float l = Len(a); return l > 1e-9f ? Mul(a, 1.0f / l) : Vec3{ 0, 0, -1 }; }

// v rotated by q, and by q's inverse.
Vec3 Rotate(const Quat& q, Vec3 v)
{
    const Vec3 u{ q.x, q.y, q.z };
    const Vec3 t = Mul(Cross(u, v), 2.0f);
    return Add(Add(v, Mul(t, q.w)), Cross(u, t));
}
Vec3 Unrotate(const Quat& q, Vec3 v) { return Rotate(Quat{ -q.x, -q.y, -q.z, q.w }, v); }

// The share of the way a first-order filter at `hz` moves in `dt`.
float Alpha(float hz, double dt)
{
    const double tau = 1.0 / (2.0 * kPi * std::max(hz, 1e-3f));
    return float(1.0 / (1.0 + tau / dt));
}

struct Tangents { float l, r, u, d; };
Tangents TangentsOf(const Fov& f) { return { std::tan(f.left), std::tan(f.right), std::tan(f.up), std::tan(f.down) }; }

} // namespace

// The world's horizontal through picture point (px, py), as the picture draws
// it: the angle of the screen's right, in radians, y down. Nudge the point
// along the world's horizontal both ways and see which way it moves on the
// picture. Straight up or down there is no horizontal: keep `fallback`.
static float HorizonAt(const View& view, int eyeW, int eyeH, float px, float py, float fallback)
{
    const Vec3 d = SpectatorUnproject(view, eyeW, eyeH, px, py);
    Vec3 h = Cross(d, Vec3{ 0, 1, 0 });
    if (Len(h) < 0.05f) return fallback;
    h = Mul(Norm(h), 0.002f);
    float ax, ay, bx, by;
    if (!SpectatorProject(view, eyeW, eyeH, Add(d, h), ax, ay) ||
        !SpectatorProject(view, eyeW, eyeH, Sub(d, h), bx, by)) return fallback;
    return std::atan2(ay - by, ax - bx);
}

namespace {
} // namespace

bool SpectatorProject(const View& view, int eyeW, int eyeH, const Vec3& dir, float& px, float& py)
{
    const Vec3 v = Unrotate(view.pose.orientation, dir);
    if (v.z > -1e-4f) return false;                 // the view looks down -Z
    const Tangents t = TangentsOf(view.fov);
    const float tx = v.x / -v.z, ty = v.y / -v.z;
    px = (tx - t.l) / (t.r - t.l) * float(eyeW);
    py = (t.u - ty) / (t.u - t.d) * float(eyeH);    // picture rows run downward
    return true;
}

Vec3 SpectatorUnproject(const View& view, int eyeW, int eyeH, float px, float py)
{
    const Tangents t = TangentsOf(view.fov);
    const float tx = t.l + px / float(eyeW) * (t.r - t.l);
    const float ty = t.u - py / float(eyeH) * (t.u - t.d);
    return Norm(Rotate(view.pose.orientation, Vec3{ tx, ty, -1.0f }));
}

SpectatorCrop Spectator::Update(const View& view, int eyeW, int eyeH, float outAspect,
                                double seconds, bool plain)
{
    SpectatorCrop out;
    const float W = float(std::max(eyeW, 1)), H = float(std::max(eyeH, 1));
    if (!(outAspect > 0.01f)) outAspect = 16.0f / 9.0f;
    const Vec3 head = Norm(Rotate(view.pose.orientation, Vec3{ 0, 0, -1 }));

    float cx = W * 0.5f, cy = H * 0.5f, roll = 0;
    if (plain)
    {
        // Upright and centred; pick the head up afresh when steady resumes.
        has_ = false;
        float px, py;
        if (SpectatorProject(view, eyeW, eyeH, head, px, py)) { cx = px; cy = py; }
    }
    else
    {
        // ---- follow the head's turns, not its wobble ----------------------
        const double dt = has_ ? Clamp(seconds - lastT_, 1e-4, 0.1) : 0.0;
        const float jumpCos = std::cos(float(jumpDeg * kPi / 180.0));
        if (!has_ || Dot(head, followed_) < jumpCos)
        {
            followed_ = head;
            rate_ = {};
        }
        else
        {
            const Vec3 raw = Mul(Sub(head, lastHead_), float(1.0 / dt));
            rate_ = Add(rate_, Mul(Sub(raw, rate_), Alpha(rateCutoffHz, dt)));
            const float cutoff = minCutoffHz + beta * Len(rate_);
            followed_ = Norm(Add(followed_, Mul(Sub(head, followed_), Alpha(cutoff, dt))));
        }
        has_ = true;
        lastHead_ = head;
        lastT_ = seconds;

        // ---- where that direction lands in this picture -------------------
        float px, py;
        if (!SpectatorProject(view, eyeW, eyeH, followed_, px, py))
        {
            followed_ = head;
            SpectatorProject(view, eyeW, eyeH, head, px, py);
        }
        cx = px; cy = py;
        out.steady = true;
    }

    // ---- the cut-out: the monitor's shape, turned to the horizon, shrunk if
    // its turned corners would leave the eye, then kept inside it. When
    // keeping it inside moves its middle, the horizon is taken again where
    // the middle now is (twice is plenty: the turn changes by fractions of a
    // degree over the few pixels the size changes by).
    float cw = widthFraction * W, ch = cw / outAspect;
    if (ch > H) { ch = H; cw = ch * outAspect; }
    const float cw0 = cw, ch0 = ch;
    for (int pass = 0; pass < 3; ++pass)
    {
        if (out.steady) roll = HorizonAt(view, eyeW, eyeH, cx, cy, lastRoll_);
        const float c = std::fabs(std::cos(roll)), s = std::fabs(std::sin(roll));
        const float k = std::min({ 1.0f, W / (cw0 * c + ch0 * s), H / (cw0 * s + ch0 * c) });
        cw = cw0 * k; ch = ch0 * k;
        const float hx = 0.5f * (cw * c + ch * s), hy = 0.5f * (cw * s + ch * c);
        const float kx = Clamp(cx, hx, W - hx), ky = Clamp(cy, hy, H - hy);
        const bool moved = std::fabs(kx - cx) > 0.25f || std::fabs(ky - cy) > 0.25f;
        cx = kx; cy = ky;
        if (!moved) break;
        if (out.steady)
        {
            // Held at the edge: follow what is DRAWN, or the followed direction
            // winds up past the edge and the view stays pinned after the head stops.
            followed_ = SpectatorUnproject(view, eyeW, eyeH, cx, cy);
            out.atEdge = true;
        }
    }
    if (out.steady) lastRoll_ = roll;
    const float rc = std::cos(roll), rs = std::sin(roll);
    const Vec2 ax{ rc * 0.5f * cw, rs * 0.5f * cw };     // to the monitor's right edge
    const Vec2 ay{ -rs * 0.5f * ch, rc * 0.5f * ch };    // to the monitor's bottom edge
    const Vec2 p[4] = {
        { cx - ax.x - ay.x, cy - ax.y - ay.y },
        { cx + ax.x - ay.x, cy + ax.y - ay.y },
        { cx + ax.x + ay.x, cy + ax.y + ay.y },
        { cx - ax.x + ay.x, cy - ax.y + ay.y },
    };
    for (int i = 0; i < 4; ++i) out.corner[i] = { p[i].x / W, p[i].y / H };
    out.rollDeg = float(roll * 180.0 / kPi);
    return out;
}

} // namespace pcvrxr
