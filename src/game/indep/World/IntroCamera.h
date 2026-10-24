#pragma once
#include <algorithm>
#include <array>
#include <cmath>

#include "game/indep/World/TrackWorld.h"

// The pre-race cinematic camera (ISSUES #265). Three shots over `total` seconds, then a crane up into the normal chase camera:
//   1. a low, wide dolly along the grid from behind the field, gliding up to the player's car;
//   2. a slow orbit around the car from its rear quarter to its front quarter, pushing in;
//   3. a low front-quarter shot on the wheel and bumper, rising as the countdown begins.
// The car pose is the player's; `chase` is the camera the race will use (the last 0.9 s blend into it).
namespace IntroCamera {
struct Pose {
    float x, y, z, yaw;
};
struct Result {
    TrackWorld::Camera camera;
    float bars = 0.0f; // letterbox amount 0..1
};

inline float Smooth(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

inline float AngleLerp(float a, float b, float t)
{
    float d = std::remainder(b - a, 6.2831853f);
    return a + d * t;
}

// Camera at p looking at target.
inline TrackWorld::Camera LookAt(float px, float py, float pz, float tx, float ty, float tz, float fov)
{
    TrackWorld::Camera c;
    c.x = px, c.y = py, c.z = pz;
    const float dx = tx - px, dy = ty - py, dz = tz - pz;
    c.yaw = std::atan2(dy, dx);
    c.pitch = std::atan2(dz, std::max(1e-3f, std::hypot(dx, dy)));
    c.fov = fov;
    return c;
}

inline Result Evaluate(float t, float total, const Pose &car, const TrackWorld::Camera &chase)
{
    const float fx = std::cos(car.yaw), fy = std::sin(car.yaw); // forward
    const float lx = -fy, ly = fx;                              // left
    auto at = [&](float f, float l, float h) { return std::array<float, 3>{car.x + fx * f + lx * l, car.y + fy * f + ly * l, car.z + h}; };
    const float s1 = total * 0.30f, s2 = total * 0.58f, blendLen = std::min(2.4f, total * 0.34f);
    TrackWorld::Camera cam;
    if (t < s1) { // shot 1: the dolly
        const float u = t / s1; // constant velocity: a dolly that eases to a stop at the cut looks like a stutter
        const auto p = at(Lerp(-17.0f, -4.2f, u), Lerp(5.4f, 3.7f, u), Lerp(0.75f, 1.0f, u)); // stays well back: a gentle pan, not a whip
        const auto tg = at(Lerp(-3.0f, 0.4f, u), 0.0f, 0.55f);
        cam = LookAt(p[0], p[1], p[2], tg[0], tg[1], tg[2], Lerp(84.0f, 72.0f, u));
    } else if (t < s2) { // shot 2: the orbit
        const float u = (t - s1) / (s2 - s1), e = u;
        const float a = Lerp(-2.45f, -0.75f, e); // angle around the car: behind-right sweeping to front-right
        const float r = Lerp(5.6f, 4.3f, e), h = Lerp(0.8f, 1.15f, e);
        const auto p = at(std::cos(a) * r, std::sin(a) * r, h);
        const auto tg = at(Lerp(-0.2f, 0.6f, e), 0.0f, 0.55f);
        cam = LookAt(p[0], p[1], p[2], tg[0], tg[1], tg[2], Lerp(62.0f, 54.0f, e));
    } else { // shot 3: low front quarter pushing in slowly, then an orbit round the car into the chase camera
        const float u = (t - s2) / std::max(0.01f, total - s2), e = u;
        const auto p = at(Lerp(4.6f, 3.1f, e), Lerp(-2.3f, -2.5f, e), Lerp(0.36f, 0.52f, e));
        const auto tg = at(1.35f, -0.9f, 0.42f);
        cam = LookAt(p[0], p[1], p[2], tg[0], tg[1], tg[2], Lerp(54.0f, 50.0f, e));
        const float bt = (t - (total - blendLen)) / blendLen;
        if (bt > 0.0f) {
            // Sweep round the car (polar about it, so the path never cuts through the body) while the look target slides
            // from the wheel to the point the chase camera looks at; position, target and lens all ease with smootherstep.
            const float b = Smooth(bt); // eased start and finish, a peak of only 1.5x the average speed
            const float cp = std::cos(chase.pitch);
            const float lookX = chase.x + std::cos(chase.yaw) * cp * 12.0f, lookY = chase.y + std::sin(chase.yaw) * cp * 12.0f,
                        lookZ = chase.z + std::sin(chase.pitch) * 12.0f;
            const float x0 = cam.x - car.x, y0 = cam.y - car.y, x1 = chase.x - car.x, y1 = chase.y - car.y;
            const float r0 = std::hypot(x0, y0), r1 = std::hypot(x1, y1);
            const float a0 = std::atan2(y0, x0), ang = AngleLerp(a0, std::atan2(y1, x1), b), r = Lerp(r0, r1, b);
            const float px = car.x + std::cos(ang) * r, py = car.y + std::sin(ang) * r, pz = Lerp(cam.z, chase.z, b);
            cam = LookAt(px, py, pz, Lerp(tg[0], lookX, b), Lerp(tg[1], lookY, b), Lerp(tg[2], lookZ, b), Lerp(cam.fov, chase.fov, b));
        }
    }
    Result r;
    r.camera = cam;
    r.bars = Smooth(t / 0.45f) * (1.0f - Smooth((t - (total - 0.55f)) / 0.55f));
    return r;
}
} // namespace IntroCamera
