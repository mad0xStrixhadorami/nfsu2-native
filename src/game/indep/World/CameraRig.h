#pragma once
#include <algorithm>
#include <cmath>

#include "game/indep/World/GarageShot.h"
#include "game/indep/World/TrackWorld.h"

// Camera rigs (ISSUES #285): everything is smoothed with a critically damped spring (no frame-rate dependent exponential
// stepping), so cuts between presets glide, the chase camera has weight, and wall clearance never pops.
namespace CameraRig {

inline float Wrap(float a) { return std::remainder(a, 6.2831853f); }

// Critically damped spring towards `target` (Unity-style SmoothDamp): `vel` is the state, `time` roughly the time to settle.
inline float Damp(float cur, float target, float &vel, float time, float dt)
{
    time = std::max(1e-3f, time);
    const float w = 2.0f / time, x = w * dt, e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    const float change = cur - target, temp = (vel + w * change) * dt;
    vel = (vel - w * temp) * e;
    return target + (change + temp) * e;
}
inline float DampAngle(float cur, float target, float &vel, float time, float dt)
{
    return Damp(cur, cur + Wrap(target - cur), vel, time, dt);
}

// Keeps a camera out of walls: the fraction of the car->camera segment that is clear. It snaps inwards at once (never
// look through a wall) but eases back out slowly, so a passing pillar does not make the zoom jump.
struct Clearance {
    float frac = 1.0f, vel = 0.0f;
    void Reset() { frac = 1.0f, vel = 0.0f; }
    // a = the point looked at (car), b = the wanted camera position; returns the corrected position.
    void Apply(const TrackWorld &world, float ax, float ay, float az, float &bx, float &by, float &bz, float dt, float minFrac = 0.2f)
    {
        float k = 1.0f;
        if (world.HitsWall(ax, ay, az, bx, by, bz)) {
            float lo = minFrac, hi = 1.0f; // bisect the largest clear fraction
            for (int i = 0; i < 6; ++i) {
                const float mid = 0.5f * (lo + hi);
                if (world.HitsWall(ax, ay, az, ax + (bx - ax) * mid, ay + (by - ay) * mid, az + (bz - az) * mid))
                    hi = mid;
                else
                    lo = mid;
            }
            k = lo;
        }
        if (k < frac)
            frac = k, vel = 0.0f; // in: instantly
        else
            frac = Damp(frac, k, vel, 0.9f, dt); // out: slowly
        bx = ax + (bx - ax) * frac, by = ay + (by - ay) * frac, bz = az + (bz - az) * frac;
    }
};

// ---- Chase camera -------------------------------------------------------------------------------------------------------
// Behind and above the car with weight: the heading and distance are springs, the view leans into the car's real direction of
// travel in a slide, widens with speed and nitrous, dips with braking and gently shakes at speed.
struct Chase {
    struct In {
        float x, y, z, yaw, speed;       // the car (z = road under it)
        float back, up;                  // the Options > Camera distance / height
        float groundZ;                   // road height under the camera spot
        bool nitrous;
        bool rigid;                      // bumper / hood views: no lag, no widening
        float orbit;                     // dev aid: extra yaw
        float timeS;                     // seconds, for the shake
    };
    bool init = false;
    float yaw = 0, yawV = 0, dist = 0, distV = 0, zOff = 0, zV = 0, fov = 70, fovV = 0, lead = 0, leadV = 0;
    float pitch = -0.17f, pitchV = 0, nitro = 0, nitroV = 0, accel = 0, accelV = 0;
    float prevX = 0, prevY = 0, prevSpeed = 0;
    Clearance clear;

    void Reset() { init = false, clear.Reset(); }

    TrackWorld::Camera Update(const TrackWorld &world, const In &in, float dt)
    {
        dt = std::clamp(dt, 1e-3f, 0.1f);
        if (!init) {
            yaw = in.yaw, dist = in.back, zOff = in.up, fov = 70.0f, init = true;
            yawV = distV = zV = fovV = leadV = pitchV = nitroV = accelV = 0;
            prevX = in.x, prevY = in.y, prevSpeed = in.speed, lead = 0, nitro = 0, accel = 0, pitch = -0.17f;
            clear.Reset();
        }
        // True direction of travel from the position delta: in a slide the camera swings towards it.
        float vx = (in.x - prevX) / dt, vy = (in.y - prevY) / dt;
        const float vs = std::hypot(vx, vy);
        float wantLead = 0.0f;
        if (!in.rigid && vs > 9.0f && in.speed > 6.0f)
            wantLead = std::clamp(Wrap(std::atan2(vy, vx) - in.yaw) * 0.55f, -0.38f, 0.38f);
        const float a = (in.speed - prevSpeed) / dt;
        prevX = in.x, prevY = in.y, prevSpeed = in.speed;
        lead = Damp(lead, wantLead, leadV, 0.28f, dt);
        accel = Damp(accel, std::clamp(a, -30.0f, 20.0f), accelV, 0.3f, dt);
        nitro = Damp(nitro, in.nitrous && !in.rigid ? 1.0f : 0.0f, nitroV, 0.45f, dt);

        const float speedK = std::clamp(std::fabs(in.speed) / 60.0f, 0.0f, 1.0f);
        const float yawTime = in.rigid ? 0.02f : 0.22f;
        yaw = DampAngle(yaw, in.yaw + lead, yawV, yawTime, dt);
        const float wantDist = in.rigid ? in.back : in.back * (1.0f + 0.12f * speedK) + 0.9f * nitro;
        dist = Damp(dist, wantDist, distV, 0.35f, dt);
        const float wantFov = in.rigid ? 70.0f : 70.0f + 11.0f * speedK + 7.0f * nitro;
        fov = Damp(fov, wantFov, fovV, 0.3f, dt);
        // Braking noses the camera down, acceleration lifts it a touch.
        const float wantPitch = -0.17f - std::clamp(accel * 0.0016f, -0.03f, 0.05f);
        pitch = Damp(pitch, in.rigid ? -0.17f : wantPitch, pitchV, 0.35f, dt);

        const float viewYaw = yaw + in.orbit;
        float cx = in.x - std::cos(viewYaw) * dist, cy = in.y - std::sin(viewYaw) * dist;
        const float want = std::max(in.z, in.groundZ) + in.up;
        zOff = Damp(zOff, want - in.z, zV, in.rigid ? 0.03f : 0.2f, dt);
        float cz = in.z + zOff;
        if (in.groundZ + 0.45f > cz)
            cz = in.groundZ + 0.45f;
        if (!in.rigid)
            clear.Apply(world, in.x, in.y, in.z + 0.9f, cx, cy, cz, dt);

        TrackWorld::Camera c;
        c.x = cx, c.y = cy, c.z = cz, c.yaw = viewYaw, c.pitch = in.orbit == 0.0f ? pitch : -0.08f, c.fov = fov;
        // Speed / nitrous shake: a few incommensurate sines, small enough to read as vibration, not wobble.
        const float amp = in.rigid ? 0.0f : 0.0016f * speedK * speedK + 0.0042f * nitro;
        const float t = in.timeS;
        c.yaw += amp * (std::sin(t * 37.0f) + 0.6f * std::sin(t * 59.0f + 1.3f));
        c.pitch += amp * (std::sin(t * 43.0f + 0.5f) + 0.6f * std::sin(t * 71.0f));
        return c;
    }
};

// ---- Garage camera ------------------------------------------------------------------------------------------------------
// Pose relative to the car: the azimuth is measured from the car's nose, positive towards its left; distances and offsets are
// for a car of half-length 2.1 m and scale with the real one.
struct GaragePose {
    float az, dist, h, tf, tl, th, fov;
};
inline GaragePose PoseFor(GarageShot s)
{
    constexpr float d = 0.0174532925f;
    switch (s) {
    case GarageShot::FrontBumper: return {26 * d, 4.7f, 0.55f, 1.9f, 0.0f, 0.42f, 50};
    case GarageShot::RearBumper:  return {154 * d, 4.7f, 0.65f, -1.9f, 0.0f, 0.5f, 50};
    case GarageShot::Exhaust:     return {148 * d, 4.4f, 0.45f, -1.9f, 0.35f, 0.34f, 46};
    case GarageShot::Skirt:       return {88 * d, 5.0f, 0.4f, 0.0f, 0.9f, 0.3f, 52};
    case GarageShot::Spoiler:     return {152 * d, 5.4f, 1.55f, -1.7f, 0.0f, 0.98f, 50};
    case GarageShot::Hood:        return {34 * d, 5.0f, 2.05f, 1.1f, 0.0f, 0.62f, 52};
    case GarageShot::Roof:        return {62 * d, 5.5f, 2.4f, -0.2f, 0.0f, 1.2f, 50};
    case GarageShot::Headlight:   return {42 * d, 4.6f, 0.75f, 1.8f, 0.5f, 0.5f, 46};
    case GarageShot::Taillight:   return {138 * d, 4.6f, 0.9f, -1.8f, 0.5f, 0.62f, 46};
    case GarageShot::Wheel:       return {66 * d, 4.4f, 0.55f, 1.2f, 0.6f, 0.34f, 44};
    case GarageShot::Underglow:   return {84 * d, 5.4f, 0.25f, 0.0f, 0.0f, 0.18f, 56};
    case GarageShot::Side:        return {72 * d, 6.2f, 1.1f, 0.0f, 0.0f, 0.6f, 50};
    default:                      return {0, 7.0f, 1.7f, 0.0f, 0.0f, 0.55f, 62}; // Overview: the az is the world sway
    }
}

struct Garage {
    bool init = false;
    float fx = 0, fy = 0, fxV = 0, fyV = 0;
    float az = 0, azV = 0, dist = 7, distV = 0, h = 1.7f, hV = 0, tf = 0, tfV = 0, tl = 0, tlV = 0, th = 0.55f, thV = 0;
    float fov = 62, fovV = 0, tz = 0, tzV = 0;
    GarageShot shot = GarageShot::Overview;
    float shotTime = 0;
    Clearance clear;

    void Reset() { init = false, clear.Reset(); }

    // (px, py, pz, pyaw) = the car being shown, s = its size relative to a 2.1 m half-length, now = seconds.
    TrackWorld::Camera Update(const TrackWorld &world, GarageShot want, float px, float py, float pz, float pyaw, float s, float now,
                              float dt)
    {
        dt = std::clamp(dt, 1e-3f, 0.1f);
        GaragePose p = PoseFor(want);
        const float sway = std::sin(now * 0.35f);
        float tAz = p.az;
        if (want == GarageShot::Overview)
            tAz = Wrap(-1.5707963f + 0.6f * std::sin(now * 0.35f * 1.0f) - pyaw); // the world-frame sway of the old showroom
        if (!init) {
            fx = px, fy = py, az = tAz, dist = p.dist * s, h = p.h, tf = p.tf * s, tl = p.tl * s, th = p.th, fov = p.fov, tz = pz;
            fxV = fyV = azV = distV = hV = tfV = tlV = thV = fovV = tzV = 0;
            init = true, shot = want, clear.Reset();
        }
        if (want != shot)
            shot = want;
        // Each move is a spring, so a change of shot glides and can be interrupted at any time. The focus point travels more
        // slowly when it jumps between cars.
        const float T = 0.6f;
        fx = Damp(fx, px, fxV, 0.5f, dt), fy = Damp(fy, py, fyV, 0.5f, dt), tz = Damp(tz, pz, tzV, 0.5f, dt);
        az = DampAngle(az, tAz, azV, T, dt);
        dist = Damp(dist, p.dist * s, distV, T, dt);
        h = Damp(h, p.h, hV, T, dt);
        tf = Damp(tf, p.tf * s, tfV, T, dt), tl = Damp(tl, p.tl * s, tlV, T, dt), th = Damp(th, p.th, thV, T, dt);
        fov = Damp(fov, p.fov, fovV, T, dt);

        // A slow breathing drift keeps the shot alive without being a sway of the whole room.
        const float a = pyaw + az + 0.045f * sway;
        const float dd = dist * (1.0f + 0.018f * std::sin(now * 0.5f + 1.0f));
        float cx = fx + std::cos(a) * dd, cy = fy + std::sin(a) * dd, cz = tz + h + 0.05f * std::sin(now * 0.7f);
        // Look target: a point beside / ahead of the car in its own frame (the focus point may still be travelling).
        const float fcx = std::cos(pyaw), fcy = std::sin(pyaw);
        const float lx = fx + fcx * tf - fcy * tl, ly = fy + fcy * tf + fcx * tl, lz = tz + th;
        clear.Apply(world, lx, ly, lz, cx, cy, cz, dt, 0.35f);
        TrackWorld::Camera c;
        c.x = cx, c.y = cy, c.z = cz;
        const float dx = lx - cx, dy = ly - cy, dz = lz - cz;
        c.yaw = std::atan2(dy, dx);
        c.pitch = std::atan2(dz, std::max(1e-3f, std::hypot(dx, dy)));
        c.fov = fov;
        return c;
    }
};
} // namespace CameraRig
