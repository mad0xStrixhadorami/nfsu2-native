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
              std::vector<SolidMesh::Triangle> &out)
{
    for (const SolidMesh::Triangle &t : wheel.tris) {
        SolidMesh::Triangle w;
        for (int k = 0; k < 3; ++k)
            w.v[k] = {t.v[k].x + x, (mirror ? -t.v[k].y : t.v[k].y) + y, t.v[k].z + z};
        out.push_back(w);
    }
}
// The strip topology approximation (ISSUES.md #90/#91) leaves long "bridge" slivers between
// separate runs; on a car they show as spikes. Drop triangles whose longest edge is far above the
// part's median edge.
void AddFiltered(const SolidMesh::NamedSolid &s, std::vector<SolidMesh::Triangle> &out)
{
    auto edge = [](const SolidMesh::Point &a, const SolidMesh::Point &b) {
        return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
    };
    std::vector<float> longest;
    longest.reserve(s.tris.size());
    for (const SolidMesh::Triangle &t : s.tris)
        longest.push_back(std::max({edge(t.v[0], t.v[1]), edge(t.v[1], t.v[2]), edge(t.v[0], t.v[2])}));
    if (longest.empty())
        return;
    std::vector<float> sorted = longest;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    static const float factor = getenv("NFSU2_CAR_EDGE") ? float(atof(getenv("NFSU2_CAR_EDGE"))) : 2.5f;
    const float limit = sorted[sorted.size() / 2] * factor;
    // Also drop triangles reaching outside the part's header bounds (mis-decoded vertices).
    auto inside = [&s](const SolidMesh::Point &p) {
        constexpr float m = 0.05f;
        return p.x >= s.bboxMin[0] - m && p.x <= s.bboxMax[0] + m && p.y >= s.bboxMin[1] - m &&
               p.y <= s.bboxMax[1] + m && p.z >= s.bboxMin[2] - m && p.z <= s.bboxMax[2] + m;
    };
    for (size_t i = 0; i < s.tris.size(); ++i)
        if (longest[i] <= limit && inside(s.tris[i].v[0]) && inside(s.tris[i].v[1]) && inside(s.tris[i].v[2]))
            out.push_back(s.tris[i]);
}
} // namespace

bool PlayerCar::LoadModel(const std::vector<uint8_t> &geometry)
{
    m_model.clear();
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
        AddFiltered(s, m_model);
    }
    if (!rearWheel)
        rearWheel = frontWheel;
    if (body && frontWheel) {
        float radius = (frontWheel->bboxMax[2] - frontWheel->bboxMin[2]) * 0.5f;
        float front = body->bboxMax[0] - 0.85f, rear = body->bboxMin[0] + 0.95f;
        float side = std::max(0.3f, body->bboxMax[1] - 0.2f);
        float z = radius;
        AddWheel(*frontWheel, front, side - 0.2f, z, false, m_model);
        AddWheel(*frontWheel, front, -side + 0.2f, z, true, m_model);
        AddWheel(*rearWheel, rear, side - 0.2f, z, false, m_model);
        AddWheel(*rearWheel, rear, -side + 0.2f, z, true, m_model);
    }
    printf("[Car] %zu solids, %zu model triangles\n", solids.size(), m_model.size());
    return !m_model.empty();
}

void PlayerCar::Place(float x, float y, float z, float yaw)
{
    m_x = x, m_y = y, m_z = z, m_yaw = yaw;
    m_speed = m_vz = m_pitch = m_roll = 0;
}

void PlayerCar::Update(const TrackWorld &world, float throttle, float brake, float steer, float dt)
{
    constexpr float kAccel = 9.0f, kBrake = 18.0f, kReverseMax = -12.0f, kTopSpeed = 62.0f;
    constexpr float kDrag = 0.0009f, kRolling = 0.6f, kWheelbase = 2.6f, kGravity = 19.6f;

    // Longitudinal: engine, brakes (reverse once stopped), aerodynamic drag and rolling resistance.
    if (throttle > 0)
        m_speed += kAccel * throttle * (1.0f - std::max(0.0f, m_speed) / kTopSpeed) * dt;
    if (brake > 0)
        m_speed -= (m_speed > 0.5f ? kBrake : kAccel * 0.5f) * brake * dt;
    m_speed -= (kDrag * m_speed * std::fabs(m_speed) + (m_speed > 0 ? kRolling : m_speed < 0 ? -kRolling : 0)) * dt;
    if (throttle <= 0 && brake <= 0 && std::fabs(m_speed) < 0.3f)
        m_speed = 0;
    m_speed = std::clamp(m_speed, kReverseMax, kTopSpeed);

    // Steering: bicycle model with less lock at speed.
    float lock = 0.55f / (1.0f + std::fabs(m_speed) * 0.05f);
    m_yaw += m_speed / kWheelbase * std::tan(steer * lock) * dt;

    float fx = std::cos(m_yaw), fy = std::sin(m_yaw);
    float nx = m_x + fx * m_speed * dt, ny = m_y + fy * m_speed * dt;

    // Walls: probe the move at bumper height against steep world triangles.
    float probe = m_speed >= 0 ? 2.2f : -2.3f;
    if (std::fabs(m_speed) > 0.01f &&
        world.HitsWall(m_x, m_y, m_z + 0.5f, nx + fx * probe, ny + fy * probe, m_z + 0.5f)) {
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
    m[3] = m_x, m[7] = m_y, m[11] = m_z;
}
