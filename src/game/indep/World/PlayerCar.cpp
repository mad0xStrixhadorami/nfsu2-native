#include "game/indep/World/PlayerCar.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <cstring>

#include "game/indep/World/TrackWorld.h"

namespace {
bool EndsWith(const std::string &s, const char *suffix)
{
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool Contains(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }

// Wheel solids are modelled around their own hub (the attachment points come from the unported
// car attribute data), so they are placed at estimated axle positions from the body bounds.
void AddWheel(const SolidMesh::NamedSolid &wheel, float x, float y, float z, bool mirror,
              std::vector<SolidMesh::Triangle> &out, std::vector<uint32_t> &tex)
{
    for (const SolidMesh::Triangle &t : wheel.tris) {
        SolidMesh::Triangle w = t;
        for (int k = 0; k < 3; ++k) {
            w.v[k].x += x;
            w.v[k].y = (mirror ? -t.v[k].y : t.v[k].y) + y;
            w.v[k].z += z;
        }
        out.push_back(w);
        tex.push_back(t.tex >= 0 && size_t(t.tex) < wheel.textures.size() ? wheel.textures[t.tex] : 0);
    }
}
// Drop triangles reaching outside the part's header bounds (a guard against mis-decoded vertices;
// with the exact strip topology, ISSUES.md #110, none are expected).
void AddFiltered(const SolidMesh::NamedSolid &s, std::vector<SolidMesh::Triangle> &out, std::vector<uint32_t> &tex)
{
    auto inside = [&s](const SolidMesh::Point &p) {
        constexpr float m = 0.05f;
        return p.x >= s.bboxMin[0] - m && p.x <= s.bboxMax[0] + m && p.y >= s.bboxMin[1] - m &&
               p.y <= s.bboxMax[1] + m && p.z >= s.bboxMin[2] - m && p.z <= s.bboxMax[2] + m;
    };
    for (const SolidMesh::Triangle &t : s.tris)
        if (inside(t.v[0]) && inside(t.v[1]) && inside(t.v[2])) {
            out.push_back(t);
            tex.push_back(t.tex >= 0 && size_t(t.tex) < s.textures.size() ? s.textures[t.tex] : 0);
        }
}
} // namespace

bool PlayerCar::LoadModel(const std::vector<uint8_t> &geometry, const Wheel *wheels)
{
    m_model.clear();
    m_triTex.clear();
    std::vector<SolidMesh::NamedSolid> solids = SolidMesh::DecodeNamedSolids(geometry.data(), geometry.size());
    std::set<std::string> seen;
    const SolidMesh::NamedSolid *frontWheel = nullptr, *rearWheel = nullptr, *body = nullptr;
    for (const SolidMesh::NamedSolid &s : solids) {
        const std::string &n = s.name;
        if (!Contains(n, "_KIT00_") && !Contains(n, "_BASE_"))
            continue;
        // LOD suffix _A.._D; names are cut to 27 characters, and the A level comes first.
        if (EndsWith(n, "_B") || EndsWith(n, "_C") || EndsWith(n, "_D") || !seen.insert(n).second)
            continue;
        if (Contains(n, "FRONT_WHEEL")) { frontWheel = &s; continue; }
        if (Contains(n, "REAR_WHEEL")) { rearWheel = &s; continue; }
        // Parts modelled around their own origin, or hidden inside the body.
        if (Contains(n, "BRAKE_") || Contains(n, "SPOILER") || Contains(n, "EXHAUST") || Contains(n, "UNDER") ||
            Contains(n, "ENGINE") || Contains(n, "AUDIO") || Contains(n, "UNLIT"))
            continue;
        if (Contains(n, "_BODY_"))
            body = &s;
        AddFiltered(s, m_model, m_triTex);
    }
    if (!rearWheel)
        rearWheel = frontWheel;
    m_rideHeight = 0;
    m_wheelStart = m_model.size();
    if (frontWheel && wheels) {
        // Database placement: the wheel solid's inner face is at y = 0 and it extends outwards
        // (+y) by about its width, so the hub goes at |y| - width / 2; right-hand wheels mirror.
        for (int i = 0; i < 4; ++i) {
            const Wheel &w = wheels[i];
            const bool right = w.y < 0;
            const SolidMesh::NamedSolid &solid = (i < 2 || !rearWheel) ? *frontWheel : *rearWheel;
            float hub = std::fabs(w.y) - w.width * 0.5f;
            AddWheel(solid, w.x, right ? -hub : hub, w.z, right, m_model, m_triTex);
        }
        m_rideHeight = std::max(0.0f, wheels[0].radius - wheels[0].z);
        m_wheelRadius = wheels[0].radius;
    } else if (body && frontWheel) {
        float radius = (frontWheel->bboxMax[2] - frontWheel->bboxMin[2]) * 0.5f;
        float front = body->bboxMax[0] - 0.85f, rear = body->bboxMin[0] + 0.95f;
        float side = std::max(0.3f, body->bboxMax[1] - 0.2f);
        float z = radius;
        AddWheel(*frontWheel, front, side - 0.2f, z, false, m_model, m_triTex);
        AddWheel(*frontWheel, front, -side + 0.2f, z, true, m_model, m_triTex);
        AddWheel(*rearWheel, rear, side - 0.2f, z, false, m_model, m_triTex);
        AddWheel(*rearWheel, rear, -side + 0.2f, z, true, m_model, m_triTex);
    }
    printf("[Car] %zu solids, %zu model triangles\n", solids.size(), m_model.size());
    return !m_model.empty();
}

float PlayerCar::Torque(const Performance &p, float rpm)
{
    // 9 curve points spread evenly over idle..max rpm (the curve's shape -- rising to a peak and
    // falling at the top -- matches each car's real figures, e.g. 350Z peak 0.37 kN*m).
    float t = (rpm - p.idleRpm) / (p.maxRpm - p.idleRpm) * 8.0f;
    if (t <= 0)
        return p.torque[0];
    if (t >= 8)
        return rpm > p.maxRpm ? 0.0f : p.torque[8];
    int i = int(t);
    return p.torque[i] + (p.torque[i + 1] - p.torque[i]) * (t - i);
}

// Stand-in drivetrain from the database figures (UG2's own model, 0x0031d570 / 0x0031e0d0, is not
// ported): engine torque through gear, final drive and drivetrain efficiency to the wheels, limited
// by tyre grip; aero drag and rolling resistance from the record; an automatic gearbox shifting at
// the redline. power scales the engine (the drag-race gearbox, ISSUES.md #115).
void PlayerCar::UpdateEngine(float throttle, float brake, float dt, float power)
{
    const Performance &p = m_perf;
    const float mass = p.mass * 1000.0f, g = 9.81f;
    const float wheelRpm = std::fabs(m_speed) / (2.0f * 3.14159265f * m_wheelRadius) * 60.0f;
    // Gear: reverse when braking at rest, otherwise automatic between redline and the drop below it.
    if (m_speed < -0.5f || (brake > 0 && throttle <= 0 && m_speed < 0.5f))
        m_gear = -1;
    else if (m_gear < 1 && m_speed >= -0.1f && throttle > 0)
        m_gear = 1;
    if (m_gear >= 1) {
        float rpm = wheelRpm * p.gears[m_gear - 1] * p.finalDrive;
        if (rpm > p.redline && m_gear < p.numGears)
            ++m_gear;
        else if (m_gear > 1 && wheelRpm * p.gears[m_gear - 2] * p.finalDrive < p.redline * 0.62f)
            --m_gear;
    }
    const float ratio = (m_gear >= 1 ? p.gears[m_gear - 1] : std::fabs(p.reverse)) * p.finalDrive;
    m_rpm = std::clamp(wheelRpm * ratio, p.idleRpm, p.maxRpm);
    // Clutch slip at launch: the engine runs up to 70% of the redline while the wheels are slow.
    if (throttle > 0)
        m_rpm = std::max(m_rpm, p.idleRpm + (p.redline * 0.7f - p.idleRpm) * throttle * (m_rpm < p.redline * 0.7f));
    float drive = Torque(p, m_rpm) * 1000.0f * ratio * p.efficiency / m_wheelRadius * throttle * power;
    drive = std::min(drive, p.grip * mass * g * 0.55f); // traction (driven axle carries ~55%)
    if (m_gear == -1)
        drive = -std::min(drive, mass * 4.0f);
    float force = drive - p.aero * 1000.0f * m_speed * std::fabs(m_speed) -
                  (m_speed > 0 ? 1.0f : m_speed < 0 ? -1.0f : 0.0f) * p.rolling * mass * g;
    if (brake > 0 && m_gear != -1 && m_speed > 0.1f)
        force -= p.grip * mass * g * 0.9f * brake;
    float v = m_speed + force / mass * dt;
    if (throttle <= 0 && (m_speed > 0) != (v > 0) && std::fabs(m_speed) > 0)
        v = 0; // brakes and resistance stop the car rather than reverse it
    if (p.limiter > 0 && v > p.limiter * 0.44704f)
        v = std::max(m_speed - 2.0f * dt, p.limiter * 0.44704f); // speed limiter cuts the drive
    m_speed = v;
}

void PlayerCar::Simulate(const Performance &p, float wheelRadius, float &zeroTo100, float &topSpeed)
{
    PlayerCar car;
    car.SetPerformance(p);
    car.m_wheelRadius = wheelRadius;
    zeroTo100 = 0;
    const float dt = 1.0f / 60.0f;
    for (int step = 0; step < 60 * 120; ++step) {
        float before = car.m_speed;
        car.UpdateEngine(1.0f, 0.0f, dt, 1.0f);
        if (!zeroTo100 && car.m_speed >= 100.0f / 3.6f)
            zeroTo100 = (step + 1) * dt;
        if (step > 600 && car.m_speed - before < 1e-4f)
            break;
    }
    topSpeed = car.m_speed;
}

void PlayerCar::Place(float x, float y, float z, float yaw)
{
    m_x = x, m_y = y, m_z = z, m_yaw = yaw;
    m_speed = m_vz = m_pitch = m_roll = 0;
    m_gear = 1;
    m_rpm = m_perf.idleRpm;
}

void PlayerCar::Update(const TrackWorld &world, float throttle, float brake, float steer, float dt, bool walls,
                       float power)
{
    constexpr float kAccel = 9.0f, kBrake = 18.0f, kReverseMax = -12.0f, kTopSpeed = 62.0f;
    constexpr float kDrag = 0.0009f, kRolling = 0.6f, kWheelbase = 2.6f, kGravity = 19.6f;

    if (m_perf.Valid()) {
        UpdateEngine(throttle, brake, dt, power);
    } else {
        // Longitudinal (no database record): engine, brakes (reverse once stopped), drag, rolling.
        if (throttle > 0)
            m_speed += kAccel * power * throttle * (1.0f - std::max(0.0f, m_speed) / kTopSpeed) * dt;
        if (brake > 0)
            m_speed -= (m_speed > 0.5f ? kBrake : kAccel * 0.5f) * brake * dt;
        m_speed -= (kDrag * m_speed * std::fabs(m_speed) + (m_speed > 0 ? kRolling : m_speed < 0 ? -kRolling : 0)) * dt;
        if (throttle <= 0 && brake <= 0 && std::fabs(m_speed) < 0.3f)
            m_speed = 0;
        m_speed = std::clamp(m_speed, kReverseMax, kTopSpeed);
    }

    // Steering: bicycle model with less lock at speed.
    float lock = 0.55f / (1.0f + std::fabs(m_speed) * 0.05f);
    const float yawRate = m_speed / kWheelbase * std::tan(steer * lock);
    m_yaw += yawRate * dt;
    // Tyre slip for sound (0..1): cornering force against the grip limit, or hard braking at speed.
    if (dt > 0) {
        const float grip = (m_perf.Valid() ? m_perf.grip : 1.0f) * 9.81f;
        float slip = std::clamp((std::fabs(m_speed * yawRate) / grip - 0.55f) / 0.45f, 0.0f, 1.0f);
        if (brake > 0.5f && m_speed > 8.0f)
            slip = std::max(slip, std::min(1.0f, (m_speed - 8.0f) / 20.0f));
        m_slip += (slip - m_slip) * std::min(1.0f, dt * 8.0f);
    }

    float fx = std::cos(m_yaw), fy = std::sin(m_yaw);
    float nx = m_x + fx * m_speed * dt, ny = m_y + fy * m_speed * dt;

    // Walls: probe the move at 0.9 m against steep world triangles (lower kerbs and road-seam
    // skirts are stepped over by the ground follow).
    float probe = m_speed >= 0 ? 2.2f : -2.3f;
    if (walls && std::fabs(m_speed) > 0.01f &&
        world.HitsWall(m_x, m_y, m_z + 0.9f, nx + fx * probe, ny + fy * probe, m_z + 0.9f)) {
        m_speed = -m_speed * 0.2f;
        return;
    }
    // Ground: the highest surface not far above the car; a big step up counts as a wall.
    float ground;
    if (world.GroundHeight(nx, ny, m_z + 1.2f, ground)) {
        m_x = nx, m_y = ny;
        if (ground >= m_z - 0.05f || (m_vz < 0 && ground > m_z + m_vz * dt)) {
            m_z = ground;
            m_vz = 0;
        } else {
            m_vz -= kGravity * dt; // off a ledge: fall
            m_z = std::max(ground, m_z + m_vz * dt);
        }
    } else {
        // No surface: a hole left by the strip topology approximation (ISSUES.md #90), not the
        // edge of the world -- keep rolling at the current height.
        m_x = nx, m_y = ny;
    }
    // Body attitude from the surface under the axles.
    float hf, hr, hl, hrt;
    const float lx = -fy, ly = fx;
    if (world.GroundHeight(m_x + fx * 1.3f, m_y + fy * 1.3f, m_z + 1.2f, hf) &&
        world.GroundHeight(m_x - fx * 1.3f, m_y - fy * 1.3f, m_z + 1.2f, hr))
        m_pitch = std::atan2(hf - hr, 2.6f);
    if (world.GroundHeight(m_x + lx * 0.8f, m_y + ly * 0.8f, m_z + 1.2f, hl) &&
        world.GroundHeight(m_x - lx * 0.8f, m_y - ly * 0.8f, m_z + 1.2f, hrt))
        m_roll = std::atan2(hl - hrt, 1.6f);
}

void PlayerCar::Transform(float m[12]) const
{
    const float cy = std::cos(m_yaw), sy = std::sin(m_yaw);
    const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
    const float cr = std::cos(m_roll), sr = std::sin(m_roll);
    // forward (pitched up by m_pitch), left (rolled by m_roll), up = forward x left
    float f[3] = {cy * cp, sy * cp, sp};
    float l0[3] = {-sy, cy, 0};
    float u0[3] = {f[1] * l0[2] - f[2] * l0[1], f[2] * l0[0] - f[0] * l0[2], f[0] * l0[1] - f[1] * l0[0]};
    float l[3], u[3];
    for (int a = 0; a < 3; ++a) {
        l[a] = l0[a] * cr - u0[a] * sr;
        u[a] = u0[a] * cr + l0[a] * sr;
    }
    for (int a = 0; a < 3; ++a) {
        m[a] = f[a];
        m[4 + a] = l[a];
        m[8 + a] = u[a];
    }
    // m_z is the ground contact; the model origin sits RideHeight() above it.
    m[3] = m_x + u[0] * m_rideHeight, m[7] = m_y + u[1] * m_rideHeight, m[11] = m_z + u[2] * m_rideHeight;
}
