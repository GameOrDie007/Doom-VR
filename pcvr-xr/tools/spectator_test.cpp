// pcvr-xr - tools/spectator_test.cpp: the spectator view's maths, proved with
// no headset. A simulated head at 90 frames a second, a real headset's
// lopsided field of view, and the spectator view's checks: the horizon level under roll and pitch, every corner inside
// the eye, a wobble reaching the view well under half strength, a turn caught
// up within a second, and no wind-up at the eye's edge.
//
//     spectator_test.exe      prints each check and PASS/FAIL; exit 0 = all pass
//
// Copyright (c) 2026 Game Or Die
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pcvrxr/spectator.h"

#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>

using namespace pcvrxr;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kW = 1852, kH = 2056;   // a Quest 3 eye at the runtime's size
constexpr float kAspect = 16.0f / 9.0f;
int gFails = 0;
template <class T> T Clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

void Check(bool ok, const char* what, const char* fmt, double a, double b = 0)
{
    std::printf("  [%s] %-44s ", ok ? "PASS" : "FAIL", what);
    std::printf(fmt, a, b);
    std::printf("\n");
    if (!ok) ++gFails;
}

Quat Axis(double ax, double ay, double az, double deg)
{
    const double h = deg * kPi / 360.0, s = std::sin(h);
    return { float(ax * s), float(ay * s), float(az * s), float(std::cos(h)) };
}
Quat Mul(const Quat& a, const Quat& b)
{
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
// Yaw about +Y, then pitch about +X, then roll about -Z (a lean to the right
// is a positive roll here).
View Head(double yaw, double pitch, double roll)
{
    View v;
    v.pose.orientation = Mul(Mul(Axis(0, 1, 0, yaw), Axis(1, 0, 0, pitch)), Axis(0, 0, 1, -roll));
    v.pose.tracked = true;
    v.fov = { -0.87f, 0.75f, 0.84f, -0.96f };   // left eye, radians, as reported
    v.width = kW; v.height = kH;
    return v;
}

// The reference-space direction the middle of the cut-out shows, as yaw and
// pitch in degrees.
void ViewYawPitch(const View& v, const SpectatorCrop& c, double& yaw, double& pitch)
{
    float px = 0, py = 0;
    for (auto& k : c.corner) { px += k.x * kW / 4; py += k.y * kH / 4; }
    const Vec3 d = SpectatorUnproject(v, kW, kH, px, py);
    yaw = std::atan2(-d.x, -d.z) * 180 / kPi;
    pitch = std::asin(Clamp(d.y, -1.0f, 1.0f)) * 180 / kPi;
}

double Amplitude(const std::vector<double>& s)
{
    // The residual about a straight line, half its peak-to-peak.
    const size_t n = s.size();
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; ++i) { sx += double(i); sy += s[i]; sxx += double(i) * i; sxy += double(i) * s[i]; }
    const double b = (n * sxy - sx * sy) / (n * sxx - sx * sx), a = (sy - b * sx) / n;
    double lo = 1e9, hi = -1e9;
    for (size_t i = 0; i < n; ++i) { const double r = s[i] - (a + b * i); lo = std::min(lo, r); hi = std::max(hi, r); }
    return (hi - lo) / 2;
}

} // namespace

int main()
{
    std::printf("Spectator view, desk checks (%dx%d eye, %.2f monitor, 90 Hz)\n", kW, kH, kAspect);

    // 1. The horizon, and the corners, across rolled and pitched heads.
    {
        double worst = 0, worstOut = 0;
        for (int roll = -40; roll <= 40; roll += 5)
            for (int pitch = -50; pitch <= 50; pitch += 10)
            {
                Spectator sp;
                const View v = Head(30, pitch, roll);
                const SpectatorCrop c = sp.Update(v, kW, kH, kAspect, 0.0, false);
                float px = 0, py = 0;
                for (auto& k : c.corner) { px += k.x * kW / 4; py += k.y * kH / 4; }
                // The world's horizontal at the middle of the cut-out, by a
                // central difference in the picture, against its top edge.
                const Vec3 d = SpectatorUnproject(v, kW, kH, px, py);
                Vec3 h{ -d.z, 0, d.x };   // cross(d, up)
                const float hl = std::sqrt(h.x * h.x + h.z * h.z);
                h = { h.x / hl * 0.002f, 0, h.z / hl * 0.002f };
                float ax, ay, bx, by;
                SpectatorProject(v, kW, kH, { d.x + h.x, d.y, d.z + h.z }, ax, ay);
                SpectatorProject(v, kW, kH, { d.x - h.x, d.y, d.z - h.z }, bx, by);
                const double want = std::atan2(ay - by, ax - bx);
                const double got = std::atan2((c.corner[1].y - c.corner[0].y) * kH, (c.corner[1].x - c.corner[0].x) * kW);
                double e = std::fabs(want - got) * 180 / kPi;
                if (e > 180) e = 360 - e;
                worst = std::max(worst, e);
                for (auto& k : c.corner)
                    worstOut = std::max({ worstOut, double(-k.x * kW), double((k.x - 1) * kW),
                                          double(-k.y * kH), double((k.y - 1) * kH) });
            }
        Check(worst < 0.3, "horizon level (roll -40..40, pitch -50..50)", "worst %.3f deg", worst);
        Check(worstOut <= 0.5, "every corner inside the eye", "worst %.2f px outside", worstOut);
    }

    // 2. A wobble at rest: 2 degrees at 6 Hz in yaw, pitch and roll.
    auto wobble = [](double rate, double& ratio, double& lagEnd, float cutoff) {
        Spectator sp;
        if (cutoff > 0) sp.minCutoffHz = cutoff;
        std::vector<double> head, view;
        const int n = 90 * 6;
        double yaw = 0, pitch = 0;
        for (int i = 0; i < n; ++i)
        {
            const double t = i / 90.0;
            const double w = 2.0 * std::sin(2 * kPi * 6 * t);
            const View v = Head(rate * t + w, 0.5 * w, 0.7 * w);
            const SpectatorCrop c = sp.Update(v, kW, kH, kAspect, t, false);
            ViewYawPitch(v, c, yaw, pitch);
            if (i >= 90 * 3) { head.push_back(rate * t + w); view.push_back(yaw); }
            lagEnd = rate * t - yaw;
        }
        ratio = Amplitude(view) / Amplitude(head);
    };
    {
        double r, lag;
        wobble(0, r, lag, 1000.0f);
        Check(r > 0.8, "control: unfiltered, the wobble is seen", "%.0f%% of it", r * 100);
        wobble(0, r, lag, 0);
        Check(r < 0.25, "wobble at rest reaches the view", "%.0f%% of it", r * 100);
        wobble(20, r, lag, 0);
        Check(r < 0.5, "wobble while turning 20 deg/s", "%.0f%% of it", r * 100);
        Check(std::fabs(lag) < 5, "lag behind a 20 deg/s turn", "%.1f deg", lag);
    }

    // 3. A turn, then stop: caught up within a second.
    auto turnStop = [](double rate, double secs, double& after1s, bool& edge) {
        Spectator sp;
        double yaw = 0, pitch = 0, head = 0;
        edge = false;
        for (int i = 0; i < 90 * 4; ++i)
        {
            const double t = i / 90.0;
            head = rate * std::min(t, secs);
            const View v = Head(head, 0, 0);
            const SpectatorCrop c = sp.Update(v, kW, kH, kAspect, t, false);
            edge = edge || c.atEdge;
            ViewYawPitch(v, c, yaw, pitch);
            if (i == int(90 * (secs + 1.0))) after1s = head - yaw;
        }
    };
    {
        double off; bool edge;
        turnStop(20, 2, off, edge);
        Check(std::fabs(off) < 1, "20 deg/s turn, 1 s after it stops", "%.2f deg behind", off);
        turnStop(150, 0.6, off, edge);
        Check(edge, "a fast turn reaches the eye's edge", edge ? "held there" : "never", 0);
        Check(std::fabs(off) < 1, "...and does not wind up: 1 s after", "%.2f deg behind", off);
    }

    // 4. A recenter-sized jump is taken at once.
    {
        Spectator sp;
        double yaw, pitch;
        sp.Update(Head(0, 0, 0), kW, kH, kAspect, 0.0, false);
        const View v = Head(90, 0, 0);
        const SpectatorCrop c = sp.Update(v, kW, kH, kAspect, 1 / 90.0, false);
        ViewYawPitch(v, c, yaw, pitch);
        Check(std::fabs(yaw - 90) < 0.5, "a 90 deg jump is taken at once", "view at %.1f deg", yaw);
    }

    // 5. Plain: centred and upright whatever the head does.
    {
        Spectator sp;
        const SpectatorCrop c = sp.Update(Head(10, 20, 25), kW, kH, kAspect, 0.0, true);
        Check(!c.steady && std::fabs(c.rollDeg) < 1e-3, "plain view is upright", "roll %.3f", c.rollDeg);
    }

    std::printf("%s\n", gFails ? "SOME CHECKS FAILED" : "all checks passed");
    return gFails ? 1 : 0;
}
