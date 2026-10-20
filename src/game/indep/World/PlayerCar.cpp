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
              std::vector<SolidMesh::Triangle> &out, std::vector<uint32_t> &tex, std::vector<uint8_t> &rim)
{
    // The wheel solid is a single face at local y ~= 0 (tyre sidewall plus rim) backed by a quad at
    // y ~= width.  Only the inner radial part of that face is the rim; the outer ring is the tyre
    // sidewall and must stay dark.  The same 0.68 radius split is used for the debug dump (#161).
    const float cx = (wheel.bboxMin[0] + wheel.bboxMax[0]) * 0.5f;
    const float cz = (wheel.bboxMin[2] + wheel.bboxMax[2]) * 0.5f;
    const float radius = 0.5f * std::max(wheel.bboxMax[0] - wheel.bboxMin[0], wheel.bboxMax[2] - wheel.bboxMin[2]);
    const float rimRadius = radius * 0.68f;
    for (const SolidMesh::Triangle &t : wheel.tris) {
        SolidMesh::Triangle w = t;
        for (int k = 0; k < 3; ++k) {
            w.v[k].x += x;
            w.v[k].y = (mirror ? -t.v[k].y : t.v[k].y) + y;
            w.v[k].z += z;
        }
        const float ty = (t.v[0].y + t.v[1].y + t.v[2].y) / 3.0f;
        const float tx = (t.v[0].x + t.v[1].x + t.v[2].x) / 3.0f - cx;
        const float tz = (t.v[0].z + t.v[1].z + t.v[2].z) / 3.0f - cz;
        const bool isRim = ty <= 0.02f && std::sqrt(tx * tx + tz * tz) <= rimRadius;
        out.push_back(w);
        tex.push_back(t.tex >= 0 && size_t(t.tex) < wheel.textures.size() ? wheel.textures[t.tex] : 0);
        rim.push_back(isRim ? 1 : 0);
    }
}
// Brake solids are authored around their own hub like the wheels; place a copy at each wheel
// centre, just inboard of the rim face.  Material slot 0 is the caliper, slot 1 the disc, so only
// slot 0 gets the Brakes paint (#173).
void AddBrake(const SolidMesh::NamedSolid &brake, float x, float y, float z, bool mirror,
              std::vector<SolidMesh::Triangle> &out, std::vector<uint32_t> &tex,
              std::vector<uint8_t> &rim, std::vector<uint8_t> &caliper)
{
    for (const SolidMesh::Triangle &t : brake.tris) {
        SolidMesh::Triangle b = t;
        for (int k = 0; k < 3; ++k) {
            b.v[k].x += x;
            b.v[k].y = (mirror ? -t.v[k].y : t.v[k].y) + y;
            b.v[k].z += z;
        }
        out.push_back(b);
        tex.push_back(t.tex >= 0 && size_t(t.tex) < brake.textures.size() ? brake.textures[t.tex] : 0);
        rim.push_back(0);
        caliper.push_back(t.tex == 0 ? 1 : 0);
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
// Detached car parts (the stock spoiler, #175) are modelled around their own origin and carry the
// mount transform in the 0x134011 header.  Apply it as the game's solid loader does.
void AddTransformed(const SolidMesh::NamedSolid &s, std::vector<SolidMesh::Triangle> &out, std::vector<uint32_t> &tex)
{
    for (const SolidMesh::Triangle &t : s.tris) {
        SolidMesh::Triangle o = t;
        for (int k = 0; k < 3; ++k)
            o.v[k] = SolidMesh::TransformPoint(s, t.v[k]);
        out.push_back(o);
        tex.push_back(t.tex >= 0 && size_t(t.tex) < s.textures.size() ? s.textures[t.tex] : 0);
    }
}
} // namespace

bool PlayerCar::LoadModel(const std::vector<uint8_t> &geometry, const Wheel *wheels)
{
    m_model.clear();
    m_triTex.clear();
    m_triRim.clear();
    m_triCaliper.clear();
    m_triMirror.clear();
    m_triSpoiler.clear();
    std::vector<SolidMesh::NamedSolid> solids = SolidMesh::DecodeNamedSolids(geometry.data(), geometry.size());
    std::set<std::string> seen;
    const SolidMesh::NamedSolid *frontWheel = nullptr, *rearWheel = nullptr, *body = nullptr;
    const SolidMesh::NamedSolid *frontBrake = nullptr, *rearBrake = nullptr;
    // Service/traffic vehicles (TAXI, BUS, ...) have no tuning kits: their parts are <NAME>_BODY_A and
    // <NAME>_TIRE_FRONT_A (ISSUES #130), so accept those when there is no KIT00 part at all.
    bool traffic = true;
    for (const SolidMesh::NamedSolid &s : solids)
        if (Contains(s.name, "_KIT00_"))
            traffic = false;
    for (const SolidMesh::NamedSolid &s : solids) {
        const std::string &n = s.name;
        if (traffic) {
            if (!Contains(n, "_BODY_") && !Contains(n, "_TIRE_"))
                continue;
        } else if (!Contains(n, "_KIT00_") && !Contains(n, "_BASE_"))
            continue;
        // LOD suffix _A.._D; names are cut to 27 characters, and the A level comes first.
        if (EndsWith(n, "_B") || EndsWith(n, "_C") || EndsWith(n, "_D") || !seen.insert(n).second)
            continue;
        if (Contains(n, "FRONT_WHEEL") || Contains(n, "TIRE_FRONT")) { frontWheel = &s; continue; }
        if (Contains(n, "REAR_WHEEL") || Contains(n, "TIRE_REAR")) { rearWheel = &s; continue; }
        // Parts modelled around their own origin, or hidden inside the body.
        if (Contains(n, "BRAKE_")) {
            if (Contains(n, "FRONT_BRAKE"))
                frontBrake = &s;
            else if (Contains(n, "REAR_BRAKE"))
                rearBrake = &s;
            continue;
        }
        if (Contains(n, "SPOILER")) {
            // The stock KIT00 spoiler is a detached solid whose header transform mounts it on the
            // trunk (#175).  It was previously skipped with the other origin-centred parts.
            const size_t before = m_model.size();
            AddTransformed(s, m_model, m_triTex);
            m_triSpoiler.resize(m_model.size(), 0);
            for (size_t i = before; i < m_model.size(); ++i)
                m_triSpoiler[i] = 1;
            continue;
        }
        if (Contains(n, "EXHAUST") || Contains(n, "UNDER") ||
            Contains(n, "ENGINE") || Contains(n, "AUDIO") || Contains(n, "UNLIT"))
            continue;
        if (Contains(n, "_BODY_"))
            body = &s;
        const size_t before = m_model.size();
        AddFiltered(s, m_model, m_triTex);
        if (Contains(n, "SIDE_MI")) {
            // The mirror solid has a 9-triangle glass in slot 0 and the painted housing in slot 1.
            m_triMirror.resize(m_model.size(), 0);
            for (size_t i = before; i < m_model.size(); ++i)
                m_triMirror[i] = (m_model[i].tex == 1) ? 1 : 0;
        }
    }
    if (frontWheel && getenv("NFSU2_WHEEL_DEBUG"))
        printf("[Wheel] %s: %zu tris bbox (%.2f %.2f %.2f)-(%.2f %.2f %.2f) textures %zu\n", frontWheel->name.c_str(),
               frontWheel->tris.size(), frontWheel->bboxMin[0], frontWheel->bboxMin[1], frontWheel->bboxMin[2],
               frontWheel->bboxMax[0], frontWheel->bboxMax[1], frontWheel->bboxMax[2], frontWheel->textures.size());
    if (frontWheel && getenv("NFSU2_WHEEL_DEBUG")) {
        float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
        for (const SolidMesh::Triangle &t : frontWheel->tris)
            for (int k = 0; k < 3; ++k)
                u0 = std::min(u0, t.v[k].u), u1 = std::max(u1, t.v[k].u), v0 = std::min(v0, t.v[k].v), v1 = std::max(v1, t.v[k].v);
        printf("[Wheel] uv range u %.3f..%.3f v %.3f..%.3f\n", u0, u1, v0, v1);
        // Detailed rim face (y <= 0.02), flat backing quad (y >= 0.2) and the tread in between.
        for (int region = 0; region < 3; ++region) {
            float a0 = 1e9f, a1 = -1e9f, b0 = 1e9f, b1 = -1e9f;
            int n = 0;
            for (const SolidMesh::Triangle &t : frontWheel->tris) {
                const float y = (t.v[0].y + t.v[1].y + t.v[2].y) / 3.0f;
                const int r = y >= 0.2f ? 0 : y <= 0.02f ? 1 : 2;
                if (r != region)
                    continue;
                ++n;
                for (int k = 0; k < 3; ++k)
                    a0 = std::min(a0, t.v[k].u), a1 = std::max(a1, t.v[k].u), b0 = std::min(b0, t.v[k].v), b1 = std::max(b1, t.v[k].v);
            }
            printf("[Wheel]  %s: %d tris u %.3f..%.3f v %.3f..%.3f\n", region == 0 ? "back" : region == 1 ? "rim" : "tread", n, a0, a1, b0, b1);
        }
        // Dev aid: NFSU2_WHEEL_DUMP=1 prints the rim/backing triangles, =2 every wheel triangle
        // (position, UV, material slot), to check which side of the solid carries the rim face.
        if (const char *wd = getenv("NFSU2_WHEEL_DUMP")) {
            const int mode = atoi(wd);
            for (const SolidMesh::Triangle &t : frontWheel->tris) {
                const float y = (t.v[0].y + t.v[1].y + t.v[2].y) / 3.0f;
                if (mode == 1 && y < 0.2f)
                    continue;
                printf("[Wheel] tri y %.3f tex %d", y, int(t.tex));
                for (int k = 0; k < 3; ++k)
                    printf(" (%.3f %.3f %.3f uv %.3f %.3f)", t.v[k].x, t.v[k].y, t.v[k].z, t.v[k].u, t.v[k].v);
                printf("\n");
            }
        }
    }
    if (frontWheel && getenv("NFSU2_WHEEL_DEBUG"))
        for (uint32_t t : frontWheel->textures)
            printf("[Wheel] texture slot hash %08x\n", t);
    if (!rearWheel)
        rearWheel = frontWheel;
    if (wheels && getenv("NFSU2_WHEEL_DEBUG"))
        for (int i = 0; i < 4; ++i)
            printf("[Wheel] slot %d x %.3f y %.3f z %.3f width %.3f radius %.3f\n", i, wheels[i].x, wheels[i].y,
                   wheels[i].z, wheels[i].width, wheels[i].radius);
    if (body && getenv("NFSU2_WHEEL_DEBUG"))
        printf("[Wheel] body bbox (%.2f %.2f %.2f)-(%.2f %.2f %.2f)\n", body->bboxMin[0], body->bboxMin[1],
               body->bboxMin[2], body->bboxMax[0], body->bboxMax[1], body->bboxMax[2]);
    m_triRim.resize(m_model.size(), 0);
    m_triCaliper.resize(m_model.size(), 0);
    m_rideHeight = 0;
    m_wheelStart = m_model.size();
    if (frontWheel && wheels) {
        // Database placement: the wheel solid's detailed rim face is at y ~= 0 (normal -y) and a
        // flat backing quad at y ~= width, so the solid is a right-hand wheel (outboard -y) as
        // authored. Mirror it on the left so the rim face stays outboard on both sides; the old
        // code mirrored the right side instead, which pointed the rim face into the car and left
        // the flat backing quad visible as a grey/black square (ISSUES #161). Place the wheel's
        // outboard edge at the database track position + half the tyre width using the solid's
        // own bounds, since its authored width differs slightly from the database figure.
        for (int i = 0; i < 4; ++i) {
            const Wheel &w = wheels[i];
            const bool left = w.y > 0;
            const SolidMesh::NamedSolid &solid = (i < 2 || !rearWheel) ? *frontWheel : *rearWheel;
            const float outboard = std::fabs(w.y) + w.width * 0.5f;
            const float yMin = solid.bboxMin[1];
            AddWheel(solid, w.x, left ? outboard + yMin : -outboard - yMin, w.z, left, m_model, m_triTex, m_triRim);
        }
        m_rideHeight = std::max(0.0f, wheels[0].radius - wheels[0].z);
        m_wheelRadius = wheels[0].radius;
    } else if (body && frontWheel) {
        float radius = (frontWheel->bboxMax[2] - frontWheel->bboxMin[2]) * 0.5f;
        float front = body->bboxMax[0] - 0.85f, rear = body->bboxMin[0] + 0.95f;
        // No database wheel positions: put the tyres just inside the widest body point and use the
        // same mirror/placement rule as above (rim face outboard on both sides).
        float outboard = std::max(0.3f, body->bboxMax[1] - 0.05f);
        float z = radius;
        AddWheel(*frontWheel, front, outboard + frontWheel->bboxMin[1], z, true, m_model, m_triTex, m_triRim);
        AddWheel(*frontWheel, front, -outboard - frontWheel->bboxMin[1], z, false, m_model, m_triTex, m_triRim);
        AddWheel(*rearWheel, rear, outboard + rearWheel->bboxMin[1], z, true, m_model, m_triTex, m_triRim);
        AddWheel(*rearWheel, rear, -outboard - rearWheel->bboxMin[1], z, false, m_model, m_triTex, m_triRim);
    }
    // AddWheel only appends the rim mask; pad the caliper mask with the wheel triangles before the
    // brake triangles are appended, otherwise the caliper flags shift onto the first wheel faces.
    m_triCaliper.resize(m_model.size(), 0);
    if (wheels && (frontBrake || rearBrake)) {
        // The wheel face sits at its placement y; brake solids span local y -0.05..0.03, so push
        // them 0.055 m inboard (opposite the rim face) to keep them behind the spokes.
        for (int i = 0; i < 4; ++i) {
            const SolidMesh::NamedSolid *brake = (i < 2 ? frontBrake : rearBrake);
            const SolidMesh::NamedSolid *wheelSolid = (i < 2) ? frontWheel : rearWheel;
            if (!brake || !wheelSolid)
                continue;
            const bool left = (i == 0 || i == 3);
            const PlayerCar::Wheel &w = wheels[i];
            const float outboard = std::fabs(w.y) + w.width * 0.5f;
            const float wheelY = left ? outboard + wheelSolid->bboxMin[1] : -outboard - wheelSolid->bboxMin[1];
            const float brakeY = left ? wheelY - 0.055f : wheelY + 0.055f;
            AddBrake(*brake, w.x, brakeY, w.z, left, m_model, m_triTex, m_triRim, m_triCaliper);
        }
    }
    m_triRim.resize(m_model.size(), 0);
    m_triCaliper.resize(m_model.size(), 0);
    m_triMirror.resize(m_model.size(), 0);
    m_triSpoiler.resize(m_model.size(), 0);
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
    if (m_ug2.Valid())
        m_ug2.Place(x, y, z + m_rideHeight, yaw);
    m_gear = 1;
    m_rpm = m_perf.idleRpm;
}

void PlayerCar::Update(const TrackWorld &world, float throttle, float brake, float steer, float dt, bool walls,
                       float power)
{
    constexpr float kAccel = 9.0f, kBrake = 18.0f, kReverseMax = -12.0f, kTopSpeed = 62.0f;
    constexpr float kDrag = 0.0009f, kRolling = 0.6f, kWheelbase = 2.6f, kGravity = 19.6f;

    if (m_ug2.Valid()) {
        UG2Physics::Controls in;
        in.throttle = std::clamp(throttle * power, 0.0f, 1.0f);
        in.brake = std::clamp(brake, 0.0f, 1.0f);
        in.steer = std::clamp(steer, -1.0f, 1.0f);
        in.handbrake = m_hold ? 1.0f : 0.0f;
        m_ug2.Update(world, in, dt);
        const UG2Physics::Vec3 o = m_ug2.Origin();
        m_x = o.x, m_y = o.y, m_z = o.z - m_rideHeight, m_yaw = m_ug2.Yaw();
        m_speed = m_ug2.ForwardSpeed();
        m_rpm = m_ug2.Rpm();
        m_gear = m_ug2.Gear();
        m_slip += (m_ug2.Slip() - m_slip) * std::min(1.0f, dt * 8.0f);
        (void)walls;
        return;
    }

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
    if (m_ug2.Valid()) {
        m_ug2.Transform(m);
        return;
    }
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
