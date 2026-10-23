#include "game/indep/World/CarCollision.h"

#include <algorithm>
#include <cmath>

#include "game/indep/World/PlayerCar.h"

using UG2Physics::Vec3;

namespace {

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }

constexpr float kArcadeMass = 1.4f;                     // tonnes
constexpr float kArcadeHalfLength = 2.2f, kArcadeHalfWidth = 0.95f;

// One side of a contact: a UG2 rigid body, or an arcade car treated as a point mass that can only
// change its speed along its heading.
struct Side {
    PlayerCar *car = nullptr;
    UG2Physics::Vehicle *body = nullptr;
    float cx = 0, cy = 0, cz = 0;          // footprint centre
    float fx = 1, fy = 0, lx = 0, ly = 1;  // unit axes (XY)
    float hl = 0, hw = 0;
    Vec3 corner[4];

    bool Build(PlayerCar &c)
    {
        car = &c;
        body = c.Body();
        if (body) {
            float off;
            body->Footprint(off, hl, hw);
            const Vec3 f = body->ForwardAxis(), l = body->LeftAxis();
            const float fn = std::hypot(f.x, f.y), ln = std::hypot(l.x, l.y);
            if (fn < 1e-3f || ln < 1e-3f)
                return false;
            fx = f.x / fn, fy = f.y / fn, lx = l.x / ln, ly = l.y / ln;
            const Vec3 cog = body->CogPosition();
            cx = cog.x + fx * off, cy = cog.y + fy * off, cz = cog.z;
        } else {
            fx = std::cos(c.Yaw()), fy = std::sin(c.Yaw()), lx = -fy, ly = fx;
            hl = kArcadeHalfLength, hw = kArcadeHalfWidth;
            cx = c.X(), cy = c.Y(), cz = c.Z();
        }
        const float sx[4] = {1, 1, -1, -1}, sy[4] = {1, -1, -1, 1};
        for (int i = 0; i < 4; ++i)
            corner[i] = {cx + fx * hl * sx[i] + lx * hw * sy[i], cy + fy * hl * sx[i] + ly * hw * sy[i], cz};
        return true;
    }
    float Mass() const { return body ? body->Mass() : kArcadeMass; }
    Vec3 Velocity(const Vec3 &p) const
    {
        if (body)
            return body->VelocityAt(p);
        return {fx * car->Speed(), fy * car->Speed(), 0.0f};
    }
    float InverseMassAlong(const Vec3 &p, const Vec3 &n) const
    {
        if (body)
            return body->InverseMassAlong(p, n);
        const float along = fx * n.x + fy * n.y; // only the heading component can change
        return along * along / kArcadeMass;
    }
    void Apply(const Vec3 &J, const Vec3 &p)
    {
        if (body)
            body->ApplyImpulse(J, p);
        else
            car->AddSpeed((J.x * fx + J.y * fy) / kArcadeMass);
    }
    void Nudge(float dx, float dy) { body ? body->Nudge(dx, dy) : car->Nudge(dx, dy); }
};

// Projection interval of a footprint on the unit axis (ax, ay).
void Project(const Side &r, float ax, float ay, float &lo, float &hi)
{
    lo = 1e9f, hi = -1e9f;
    for (const Vec3 &c : r.corner) {
        const float p = c.x * ax + c.y * ay;
        lo = std::min(lo, p);
        hi = std::max(hi, p);
    }
}

} // namespace

float ResolveCarContact(PlayerCar &a, PlayerCar &b)
{
    if (!a.Body() && !b.Body())
        return -1.0f; // two arcade cars: the caller's fallback
    Side ra, rb;
    if (!ra.Build(a) || !rb.Build(b))
        return -1.0f;
    if (std::fabs(ra.cz - rb.cz) > 1.5f)
        return 0.0f;
    // Separating axis test over the four edge normals; keep the smallest overlap.
    const float axes[4][2] = {{ra.fx, ra.fy}, {ra.lx, ra.ly}, {rb.fx, rb.fy}, {rb.lx, rb.ly}};
    float bestPen = 1e9f, nx = 0, ny = 0;
    int bestAxis = -1;
    for (int i = 0; i < 4; ++i) {
        float alo, ahi, blo, bhi;
        Project(ra, axes[i][0], axes[i][1], alo, ahi);
        Project(rb, axes[i][0], axes[i][1], blo, bhi);
        const float pen = std::min(ahi, bhi) - std::max(alo, blo);
        if (pen <= 0.0f)
            return 0.0f; // a separating axis: no contact
        if (pen < bestPen) {
            bestPen = pen;
            bestAxis = i;
            nx = axes[i][0], ny = axes[i][1];
            if ((rb.cx - ra.cx) * nx + (rb.cy - ra.cy) * ny < 0)
                nx = -nx, ny = -ny; // from A to B
        }
    }
    // Contact point: the corner of the other car that is deepest into the reference car.
    Vec3 p{};
    if (bestAxis < 2) {
        float lo = 1e9f;
        for (const Vec3 &c : rb.corner)
            if (c.x * nx + c.y * ny < lo)
                lo = c.x * nx + c.y * ny, p = c;
    } else {
        float hi = -1e9f;
        for (const Vec3 &c : ra.corner)
            if (c.x * nx + c.y * ny > hi)
                hi = c.x * nx + c.y * ny, p = c;
    }
    p.z = (ra.cz + rb.cz) * 0.5f;
    const Vec3 n{nx, ny, 0.0f};
    const Vec3 vrel = rb.Velocity(p) - ra.Velocity(p);
    const float closing = -(vrel.x * nx + vrel.y * ny);
    // Positional correction in proportion to the inverse masses.
    const float ima = 1.0f / ra.Mass(), imb = 1.0f / rb.Mass();
    const float push = std::max(0.0f, bestPen - 0.02f);
    ra.Nudge(-nx * push * ima / (ima + imb), -ny * push * ima / (ima + imb));
    rb.Nudge(nx * push * imb / (ima + imb), ny * push * imb / (ima + imb));
    if (closing <= 0.0f)
        return 0.0f; // already separating
    const float denom = ra.InverseMassAlong(p, n) + rb.InverseMassAlong(p, n);
    if (denom < 1e-6f)
        return closing;
    const float j = 1.2f * closing / denom; // restitution 0.2
    ra.Apply(n * -j, p);
    rb.Apply(n * j, p);
    // Coulomb friction along the sliding direction (mu 0.3) for rigid bodies.
    if (ra.body || rb.body) {
        Vec3 vt = vrel - n * (vrel.x * nx + vrel.y * ny);
        vt.z = 0.0f;
        const float vtl = std::hypot(vt.x, vt.y);
        if (vtl > 1e-3f) {
            const Vec3 t{vt.x / vtl, vt.y / vtl, 0.0f};
            const float dt = ra.InverseMassAlong(p, t) + rb.InverseMassAlong(p, t);
            if (dt > 1e-6f) {
                const float jt = std::min(vtl / dt, 0.3f * j);
                ra.Apply(t * jt, p);
                rb.Apply(t * -jt, p);
            }
        }
    }
    return closing;
}
