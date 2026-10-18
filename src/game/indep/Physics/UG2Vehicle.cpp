#include "game/indep/Physics/UG2Vehicle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/indep/World/TrackWorld.h"

namespace UG2Physics {
namespace {
float F(const uint8_t *r, int off)
{
    float v;
    memcpy(&v, r + off, 4);
    return v;
}
int I(const uint8_t *r, int off)
{
    int v;
    memcpy(&v, r + off, 4);
    return v;
}

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(Vec3 a) { return std::sqrt(Dot(a, a)); }
Vec3 Norm(Vec3 a)
{
    const float l = Len(a);
    return l > 1e-9f ? a * (1.0f / l) : Vec3{0, 0, 1};
}
Vec3 ClampLen(Vec3 a, float max)
{
    const float l = Len(a);
    return l > max ? a * (max / l) : a;
}

// 0x002edf48: evenly spaced curve lookup with clamping and linear interpolation.
float Curve(const float *y, int n, float x0, float x1, float x)
{
    const float t = (x - x0) * float(n - 1) / (x1 - x0);
    if (t <= 0)
        return y[0];
    const int i = int(t);
    if (i >= n - 1)
        return y[n - 1];
    const float f = t - float(i);
    return y[i] * (1 - f) + y[i + 1] * f;
}

// Lateral force vs normalised load (0..10) for 7 slip-angle bands: runtime tables 0x0051e068 +
// 0x18*band built by 0x00328738 (recovered with tools/mips_init_run.py).
const float kLateral[7][6] = {
    {0, 0, 0, 0, 0, 0},           {0, 1.2f, 2.3f, 3, 3, 2.8f},    {0, 1.7f, 3.2f, 4.3f, 5.1f, 5.2f},
    {0, 1.8f, 3.5f, 4.9f, 5.8f, 6.1f}, {0, 1.83f, 3.6f, 5, 5.96f, 6.4f}, {0, 1.86f, 3.7f, 5.1f, 6.13f, 6.7f},
    {0, 1.9f, 3.8f, 5.2f, 6.3f, 7.1f},
};
// Rigid-body settling curves 0x0051df38 (by |omega|) and 0x0051df70 (by speed).
const float kRestDamp[6] = {0.05f, 0.05f, 0.25f, 0.7f, 2, 5};
const float kRestThreshold[8] = {1.5f, 1, 0.6f, 0.22f, 0.06f, 0.03f, 0.015f, 0};
// Clutch rate by drivetrain mode (0x0051dae0).
const float kClutchRate[4] = {-3.0f, 0.0f, 1.5f, 0.0f};
} // namespace

bool Vehicle::Setup(const uint8_t *r)
{
    m_valid = false;
    // Body (P+0x110 = rec+0x228): mass, inertia diagonal with the yaw term scaled by rec+0x278.
    Body &b = m_body;
    b = Body();
    b.mass = F(r, 0x228);
    if (!(b.mass > 0))
        return false;
    b.invMass = 1.0f / b.mass;
    b.inertia = {F(r, 0x238), F(r, 0x24c), F(r, 0x260) * F(r, 0x278)};
    if (!(b.inertia.x > 0 && b.inertia.y > 0 && b.inertia.z > 0))
        return false;
    b.invInertia = {1 / b.inertia.x, 1 / b.inertia.y, 1 / b.inertia.z};
    b.cog = {F(r, 0x118), F(r, 0x11c), F(r, 0x120)};

    // Wheels (rec+0x128 + 0x30i: FL, FR, RR, RL), tyres (rec+0x1e8 front / +0x208 rear),
    // suspension (rec+0x288 / +0x2a8), brakes (rec+0x388), brake bias rec+0x394 (rear share).
    const float rearBias = F(r, 0x394);
    for (int i = 0; i < 4; ++i) {
        Wheel &w = m_wheel[i];
        w = Wheel();
        const int wo = 0x128 + 0x30 * i, to = i < 2 ? 0x1e8 : 0x208, so = i < 2 ? 0x288 : 0x2a8;
        w.local = Vec3{F(r, wo), F(r, wo + 4), F(r, wo + 8)} - b.cog;
        w.radius = F(r, wo + 0x10), w.width = F(r, wo + 0x14);
        w.slipScale = F(r, to), w.loadNorm = F(r, to + 4), w.latScale = F(r, to + 8);
        w.circleMu = F(r, to + 0xc), w.longMu = F(r, to + 0x10);
        w.brakeBias = i < 2 ? 2 * (1 - rearBias) : 2 * rearBias;  // 0x003173e0
        w.handbrakeBias = i < 2 ? 0.0f : 1.0f;                     // 0x0051d7a0
        Suspension &s = m_susp[i];
        s = Suspension();
        s.progressive = F(r, so), s.k = F(r, so + 4), s.bump = F(r, so + 8), s.rebound = F(r, so + 0xc);
        s.antiRoll = F(r, so + 0x10), s.maxTravel = F(r, so + 0x14), s.minTravel = F(r, so + 0x18);
        s.other = i ^ 1;
        if (!(w.radius > 0))
            return false;
    }
    // Static load per wheel (0x00316fb0): weight split by the CoG between the axles.
    const float front = m_wheel[0].local.x, rear = m_wheel[3].local.x;
    const float f4 = front / (front - rear) * 0.5f, weight = b.mass * kGravity;
    for (int i = 0; i < 4; ++i)
        m_susp[i].preload = weight * (i < 2 ? 0.5f - f4 : f4);
    m_brakeTorque = F(r, 0x38c), m_handbrakeTorque = F(r, 0x390);
    m_maxSteer = F(r, 0x3a0) * 0.0174533f; // 17 degrees for every car (stand-in use, see Update)
    m_limiterSpeed = F(r, 0x3a8) * 0.44704f;
    m_dragCoef = F(r, 0x378), m_downFront = F(r, 0x37c), m_downRear = F(r, 0x380);

    // Engine (rec+0x308).
    Engine &e = m_engine;
    e = Engine();
    e.idle = F(r, 0x308), e.redline = F(r, 0x30c), e.maxRpm = F(r, 0x310), e.inertia = F(r, 0x314);
    for (int k = 0; k < 9; ++k)
        e.torque[k] = F(r, 0x318 + 4 * k), e.turbo[k] = F(r, 0x33c + 4 * k);
    for (int k = 0; k < 3; ++k)
        e.braking[k] = F(r, 0x360 + 4 * k);
    if (!(e.maxRpm > e.idle && e.inertia > 0))
        return false;
    e.speed = e.idle / kRadToRpm;

    // Drivetrain (rec+0x2c8): clutch gain, front/rear final drive, rear torque share, clutch
    // stiffness, gear count, clutch rate, ratios from rec+0x2e8 (reverse, neutral, 1st..).
    Drivetrain &d = m_drive;
    d = Drivetrain();
    d.clutchGain = F(r, 0x2cc), d.frontFinal = F(r, 0x2d0), d.rearFinal = F(r, 0x2d4);
    d.rearSplit = F(r, 0x2d8), d.clutchStiffness = F(r, 0x2dc), d.numGears = I(r, 0x2e0);
    d.clutchRate = F(r, 0x2e4);
    d.numGears = std::clamp(d.numGears, 1, 6);
    for (int k = 0; k < 8; ++k)
        d.ratio[k] = F(r, 0x2e8 + 4 * k);
    // Shift points (0x0031ef20 with the torque-matching option DAT_00568c04 off): up at
    // redline - 100 rpm, down where the next gear would land on that point.
    for (int g = 2; g < 2 + d.numGears; ++g) {
        d.shiftUp[g] = e.redline - 100.0f;
        d.shiftDown[g] = g > 2 && d.ratio[g - 1] > 0 ? (e.redline - 100.0f) * d.ratio[g] / d.ratio[g - 1] * 0.92f : 0;
    }
    for (int i = 0; i < 4; ++i)
        m_wheel[i].driven = i < 2 ? d.rearSplit < 0.999f : d.rearSplit > 0.001f;
    m_valid = true;
    if (getenv("NFSU2_PHYS_DEBUG"))
        for (int i = 0; i < 4; ++i)
            printf("[PhysSetup] w%d local %.2f %.2f %.2f r%.2f  axle slip %.3f loadN %.3f lat %.3f cmu %.3f lmu %.3f  "
                   "susp prog %.3f k %.3f bump %.3f reb %.3f ar %.3f max %.3f min %.3f pre %.3f\n",
                   i, m_wheel[i].local.x, m_wheel[i].local.y, m_wheel[i].local.z, m_wheel[i].radius,
                   m_wheel[i].slipScale, m_wheel[i].loadNorm, m_wheel[i].latScale, m_wheel[i].circleMu,
                   m_wheel[i].longMu, m_susp[i].progressive, m_susp[i].k, m_susp[i].bump, m_susp[i].rebound,
                   m_susp[i].antiRoll, m_susp[i].maxTravel, m_susp[i].minTravel, m_susp[i].preload);
    return true;
}

void Vehicle::Place(float x, float y, float z, float yaw)
{
    Body &b = m_body;
    const float c = std::cos(yaw), s = std::sin(yaw);
    b.rot.r[0] = {c, s, 0};
    b.rot.r[1] = {-s, c, 0};
    b.rot.r[2] = {0, 0, 1};
    b.pos = Vec3{x, y, z} + ToWorld(b.cog);
    b.momentum = b.angMomentum = Vec3{};
    b.rest = 0;
    for (int i = 0; i < 4; ++i) {
        m_wheel[i].spin = m_wheel[i].spinAngle = 0;
        m_susp[i].x = m_susp[i].v = 0;
    }
    m_drive.gear = m_drive.targetGear = 2;
    m_drive.mode = 3, m_drive.clutch = 1;
    m_engine.speed = m_engine.idle / kRadToRpm;
    m_lastGroundZ = b.pos.z;
    m_grounded = true;
    UpdateDerived();
}

Vec3 Vehicle::ToWorld(const Vec3 &v) const
{
    const Mat3 &m = m_body.rot;
    return m.r[0] * v.x + m.r[1] * v.y + m.r[2] * v.z;
}

Vec3 Vehicle::ToBody(const Vec3 &v) const
{
    const Mat3 &m = m_body.rot;
    return {Dot(m.r[0], v), Dot(m.r[1], v), Dot(m.r[2], v)};
}

// RigidBody post-step 0x00323030: velocities from momenta (|v| <= 200 m/s, |omega| <= 250).
// Body/world collision (stand-in for 0x334110... not yet ported): sweep the nose, centre and tail of
// the body against steep world triangles over the frame; on contact, cancel the velocity component
// into the wall so the car scrapes along barriers instead of passing through them.
void Vehicle::CollideWorld(const TrackWorld &world, float dt)
{
    Body &b = m_body;
    if (Len(b.vel) < 0.05f)
        return;
    const Vec3 fwd = b.rot.r[0];
    const Vec3 p1 = b.pos + b.vel * dt;
    for (float k : {-1.8f, 0.0f, 1.8f}) {
        const Vec3 a = b.pos + fwd * k;
        const Vec3 c = p1 + fwd * k;
        float n[3];
        if (world.HitsWall(a.x, a.y, a.z, c.x, c.y, c.z, n)) {
            const Vec3 nn = {n[0], n[1], n[2]};
            const float vn = Dot(b.momentum, nn);
            if (vn < 0)
                b.momentum = b.momentum - nn * vn;
            b.pos = b.pos + nn * 0.02f;
            UpdateDerived();
            return;
        }
    }
}

void Vehicle::UpdateDerived()
{
    Body &b = m_body;
    b.vel = ClampLen(b.momentum * b.invMass, 200.0f);
    const Vec3 lb = ToBody(b.angMomentum);
    const Vec3 wb = {lb.x * b.invInertia.x, lb.y * b.invInertia.y, lb.z * b.invInertia.z};
    b.omega = ClampLen(ToWorld(wb), 250.0f);
}

// 0x00322540: force (|F| <= 1200) at a world point; torque about the CoG (|T| <= 100).
void Vehicle::ApplyForce(const Vec3 &f, const Vec3 &at)
{
    const Vec3 fc = ClampLen(f, 1200.0f);
    m_body.force = m_body.force + fc;
    m_body.torque = m_body.torque + ClampLen(Cross(at - m_body.pos, fc), 100.0f);
}

float Vehicle::EngineTorque(float speed, float throttle) const
{
    // 0x0030c7a0 / 0x0030c748: torque curve x throttle; engine braking curve x (1 - throttle),
    // doubled on the limiter; negative torque scaled 0.85 (0.65 in some modes).
    const Engine &e = m_engine;
    const float rpm = speed * kRadToRpm;
    const bool limiter = e.limiter;
    const float t = limiter ? 0.0f : throttle;
    float torque = Curve(e.torque, 9, e.idle, e.maxRpm, rpm) * t;
    float brake = -Curve(e.braking, 3, e.idle, e.maxRpm, rpm) * (1 - t);
    if (limiter)
        brake *= 2;
    torque += brake;
    if (torque < 0 && !limiter)
        torque *= 0.85f;
    return torque;
}

void Vehicle::Update(const TrackWorld &world, const Controls &in, float dt)
{
    if (!m_valid)
        return;
    m_in = in;
    // Steering (TwoWheelAckermanSteering 0x00324d50): the inner wheel gets the input angle, the
    // outer one the Ackermann angle for the wheelbase / track. Stand-in for the player steering
    // filter: full lock (rec+0x3a0, 17 deg) at rest, reduced with speed.
    const float speed = Len(m_body.vel);
    const float lock = m_maxSteer / (1.0f + speed * 0.045f);
    const float a = std::fabs(in.steer) * lock;
    const float wheelbase = m_wheel[0].local.x - m_wheel[3].local.x;
    const float track = m_wheel[0].local.y - m_wheel[1].local.y;
    const float outer = a > 0 ? wheelbase * a / (wheelbase + track * a) : 0.0f;
    const float sign = in.steer < 0 ? -1.0f : 1.0f;
    // Turning left (+): the left wheel (0) is inner.
    m_wheel[0].steerAngle = sign * (in.steer >= 0 ? a : outer);
    m_wheel[1].steerAngle = sign * (in.steer >= 0 ? outer : a);
    m_wheel[2].steerAngle = m_wheel[3].steerAngle = 0;

    AutoGearbox();
    CollideWorld(world, dt);
    // Fixed substeps (0x00317428 splits dt into n equal steps of at most 0.0044 s).
    const int n = std::min(int(dt / kStep) + 1, 40);
    const float h = dt / float(n);
    for (int k = 0; k < n; ++k)
        Substep(world, h);

    static const bool debug = getenv("NFSU2_PHYS_DEBUG") != nullptr;
    m_accum += dt;
    if (debug && m_accum >= 0.25f) {
        m_accum = 0;
        printf("[Phys] v %.1f fwd %.1f gear %d mode %d clutch %.2f rpm %.0f z %.2f up %.2f yaw %.2f steer %.2f |",
               Len(m_body.vel), ForwardSpeed(), Gear(), m_drive.mode, m_drive.clutch, Rpm(), m_body.pos.z,
               m_body.rot.r[2].z, Yaw(), m_in.steer);
        for (int i = 0; i < 4; ++i) {
            const Wheel &w = m_wheel[i];
            printf(" %c sa%+.0f x%+.3f L%.1f s%.1f T%.2f Fl%.1f Ft%.1f(v%+.2f a%+.0f)", w.sliding ? 'S' : 'r',
                   w.steerAngle * 57.2958f, m_susp[i].x, w.load, w.spin * w.radius, w.torque, w.longForce, w.latForce,
                   w.dbgVy, w.dbgSa);
        }
        printf("\n");
    }
}

// Automatic gearbox (0x0030df18 not ported): shift up at the shift point, down below the down
// point; reverse when braking at a standstill, back to first on throttle.
void Vehicle::AutoGearbox()
{
    Drivetrain &d = m_drive;
    if (d.mode != 3)
        return;
    const float rpm = m_engine.speed * kRadToRpm;
    const float fwd = ForwardSpeed();
    int target = d.gear;
    if (d.gear == 0) {
        if (m_in.throttle > 0.1f && fwd > -1.0f)
            target = 2;
    } else if (m_in.brake > 0.1f && m_in.throttle < 0.1f && fwd < 0.5f) {
        target = 0;
    } else if (d.gear >= 2) {
        if (rpm > d.shiftUp[d.gear] && d.gear < 1 + d.numGears && m_in.throttle > 0.05f)
            target = d.gear + 1;
        else if (d.gear > 2 && rpm < d.shiftDown[d.gear])
            target = d.gear - 1;
    }
    if (target != d.gear) {
        d.targetGear = target;
        d.mode = 0; // disengage (0x0030d308)
    }
}

void Vehicle::Substep(const TrackWorld &world, float h)
{
    Body &b = m_body;
    b.force = b.torque = Vec3{};
    // Gravity (0x00323030 adds it through 0x00322540 at the CoG).
    b.force.z -= kGravity * b.mass;
    Aero();
    SuspensionAndContact(world, h);
    DriveTorque(h);
    TyreForces(h);

    // Clutch state machine (0x0030d308 / 0x0030d6c8).
    Drivetrain &d = m_drive;
    d.clutch += kClutchRate[d.mode] * d.clutchRate * h;
    if (d.mode == 0 && d.clutch <= 0) {
        d.clutch = 0;
        d.mode = 1;
    }
    if (d.mode == 1) {
        d.gear = d.targetGear;
        d.mode = 2;
    }
    if (d.mode == 2 && d.clutch >= 1) {
        d.clutch = 1;
        d.mode = 3;
    }
    d.clutch = std::clamp(d.clutch, 0.0f, 1.0f);

    // Settling (0x00322c98): angular damping and extra damping when nearly at rest.
    const float wl = Len(b.omega);
    b.torque = b.torque - b.angMomentum * Curve(kRestDamp, 6, 0, 10, wl);
    const float threshold = Curve(kRestThreshold, 8, 0, 1, std::max(Len(b.vel) * 0.25f, wl * 0.5f));
    b.rest += (b.rest < threshold ? 0.1f : -10.0f) * h;
    b.rest = std::max(0.0f, b.rest);
    b.torque = b.torque - b.angMomentum * (2 * b.rest);
    b.force = b.force - b.momentum * (8 * b.rest);

    // Euler step of the body state (0x0030a688): position, orientation, momenta.
    b.pos = b.pos + b.vel * h;
    const Vec3 w = b.omega;
    for (Vec3 &row : b.rot.r)
        row = row + Cross(w, row) * h;
    // Re-orthonormalise (0x001e0e40).
    b.rot.r[0] = Norm(b.rot.r[0]);
    b.rot.r[2] = Norm(Cross(b.rot.r[0], b.rot.r[1]));
    b.rot.r[1] = Cross(b.rot.r[2], b.rot.r[0]);
    b.momentum = b.momentum + b.force * h;
    b.angMomentum = b.angMomentum + b.torque * h;
    UpdateDerived();
    // World-floor recovery (stand-in for the body/world collision of 0x334110 not yet ported): if
    // the car has left the loaded terrain there is no ground to land on, so hold it near the last
    // grounded height instead of falling forever.
    if (!m_grounded && b.pos.z < m_lastGroundZ - 1.0f) {
        b.pos.z = m_lastGroundZ;
        if (b.momentum.z < 0)
            b.momentum.z = 0;
        UpdateDerived();
    }

    for (Wheel &wh : m_wheel) {
        wh.spinAngle = std::fmod(wh.spinAngle + wh.spin * h, 6.2831853f);
    }
}

// 0x00302620: drag -v|v| * cd and downforce at the front/rear axle points.
void Vehicle::Aero()
{
    Body &b = m_body;
    const float speed = Len(b.vel);
    ApplyForce(b.vel * (-speed * m_dragCoef), b.pos);
    if (speed < 1e-4f)
        return;
    float forward = std::max(0.0f, Dot(b.vel * (1.0f / speed), b.rot.r[0]));
    forward = std::max(0.4f, forward * forward);
    const float up = std::max(0.0f, b.rot.r[2].z);
    const float total = m_downFront + m_downRear;
    const float fFront = speed * 0.41f * forward * up * total * (m_downFront / std::max(total, 1e-6f));
    const float fRear = speed * 0.41f * forward * up * total * (m_downRear / std::max(total, 1e-6f));
    ApplyForce(b.rot.r[2] * -fFront, b.pos + b.rot.r[0]);
    ApplyForce(b.rot.r[2] * -fRear, b.pos - b.rot.r[0]);
}

// Suspension 0x00324ff8 + tyre load 0x00323a68: probe the ground under each hardpoint along the
// body's up axis, compression clamped to the travel, spring + damper + anti-roll, load from slope.
void Vehicle::SuspensionAndContact(const TrackWorld &world, float h)
{
    Body &b = m_body;
    const Vec3 up = b.rot.r[2];
    float raw[4];
    for (int i = 0; i < 4; ++i) {
        Wheel &w = m_wheel[i];
        Suspension &s = m_susp[i];
        const Vec3 hard = b.pos + ToWorld(w.local);
        float gz, n[3];
        const float prevX = s.x;
        if (world.GroundHeight(hard.x, hard.y, hard.z + 0.6f, gz, n) && up.z > 0.001f) {
            // Distance along up from the hardpoint to the ground, plus the radius (0x00323b60).
            raw[i] = (gz - hard.z) / up.z + w.radius;
            w.normal = {n[0], n[1], n[2]};
        } else {
            raw[i] = -100.0f;
        }
        if (raw[i] > s.maxTravel) {
            s.x = s.maxTravel;
            s.grounded = true;
        } else if (raw[i] < s.minTravel) {
            s.x = s.minTravel;
            s.grounded = false;
        } else {
            s.x = raw[i];
            s.grounded = true;
        }
        if (h > 1e-4f)
            s.v = (s.x - prevX) / h;
        w.contact = hard + up * (s.x - w.radius);
    }
    for (int i = 0; i < 4; ++i) {
        Suspension &s = m_susp[i];
        Wheel &w = m_wheel[i];
        float f = 0;
        if (s.grounded) {
            f = s.preload + s.x * s.k * (1 + s.progressive * std::max(s.x, 0.0f)) +
                s.v * (s.v > 0 ? s.bump : s.rebound) + s.antiRoll * (s.x - m_susp[s.other].x);
            // Bump stop (stand-in for the body/world collision not yet ported), bounded so a kerb or
            // crest cannot catapult the car: it can at most reach the car's own weight on one wheel.
            if (raw[i] > s.maxTravel)
                f += std::min((raw[i] - s.maxTravel) * s.k * 10.0f, b.mass * kGravity * 0.5f);
            f = std::max(0.0f, f);
            f = std::min(f, b.mass * kGravity);
        }
        s.force = f;
        // Load from the slope (0x00323a68): (n . up) * 4 - 3, at least 0.3.
        const float slope = std::max(0.3f, Dot(w.normal, up) * 4 - 3);
        w.load = s.grounded ? f * slope : 0.0f;
        if (s.grounded)
            ApplyForce(up * f, w.contact);
    }
    m_grounded = m_susp[0].grounded || m_susp[1].grounded || m_susp[2].grounded || m_susp[3].grounded;
    if (m_grounded)
        m_lastGroundZ = b.pos.z;
}

// Drivetrain: engine torque through the clutch to the driven wheels (0x0030da30 / 0x0030db88 /
// 0x0030d758). Engaged (mode 3), engine and driven wheels turn together (the game's rigid
// coupling); otherwise the clutch transmits a torque proportional to the slip speed.
void Vehicle::DriveTorque(float h)
{
    Engine &e = m_engine;
    Drivetrain &d = m_drive;
    for (Wheel &w : m_wheel)
        w.torque = 0;
    const float ratio = d.ratio[d.gear];
    float driven = 0, spinSum = 0;
    for (const Wheel &w : m_wheel)
        if (w.driven) {
            driven += 1;
            spinSum += w.spin;
        }
    const float axleFinal = d.rearSplit > 0.5f ? d.rearFinal : d.frontFinal;
    const float G = ratio * axleFinal;
    const float wheelSpeedAtEngine = driven > 0 ? spinSum / driven * G : 0.0f;

    // Rev limiter with hysteresis (0x0030c220).
    const float rpm = e.speed * kRadToRpm;
    if (!e.limiter && rpm > e.redline + 25.0f + (e.maxRpm - e.redline) * 0.5f)
        e.limiter = true;
    else if (e.limiter && rpm < e.redline - e.redline * 0.0125f)
        e.limiter = false;
    // Speed limiter (rec+0x3a8): no drive above it.
    const bool atLimit = ForwardSpeed() > m_limiterSpeed;
    const float throttle = d.gear == 0 ? m_in.brake : m_in.throttle;
    const float te = EngineTorque(e.speed, atLimit ? 0.0f : throttle);

    const float idleSpeed = e.idle / kRadToRpm;
    float toWheels = 0; // total torque at the driven wheels
    if (d.mode == 3 && std::fabs(G) > 1e-4f) {
        // Engaged: engine and driven wheels turn together (the game's rigid coupling); the engine
        // inertia is reflected onto the wheels by TyreForces.
        if (wheelSpeedAtEngine < idleSpeed * 0.9f) {
            // Below idle in gear: slip the clutch like a launch instead of stalling.
            e.speed = idleSpeed + throttle * (e.redline * 0.45f - e.idle) / kRadToRpm;
            toWheels = EngineTorque(e.speed, throttle) * G;
        } else {
            e.speed = std::min(wheelSpeedAtEngine, e.maxRpm / kRadToRpm);
            toWheels = te * G;
        }
    } else {
        // Disengaging / engaging (modes 0-2): the clutch transmits only its engagement fraction of
        // the engine torque, and the engine free-revs (so a shift coasts instead of locking the
        // driven wheels through the gearbox).
        toWheels = std::fabs(G) > 1e-4f ? te * G * d.clutch : 0.0f;
        e.speed += te / e.inertia * h;
        e.speed = std::clamp(e.speed, idleSpeed, e.maxRpm / kRadToRpm);
    }
    // Split front/rear by the torque share and 50/50 across each axle (open differential).
    for (int i = 0; i < 4; ++i) {
        Wheel &w = m_wheel[i];
        if (!w.driven)
            continue;
        const float share = i < 2 ? 1 - d.rearSplit : d.rearSplit;
        w.torque += toWheels * share * 0.5f;
    }
    // Brakes (0x00323eb0): 4 x brake x bias x rec+0x38c, 10 x handbrake x rec+0x390, opposing spin.
    for (Wheel &w : m_wheel) {
        const float brakeIn = d.gear == 0 ? m_in.throttle : m_in.brake;
        w.brakeInput = brakeIn * w.brakeBias;
        w.handbrakeInput = m_in.handbrake * w.handbrakeBias;
        float bt = 4 * w.brakeInput * m_brakeTorque + 10 * w.handbrakeInput * m_handbrakeTorque;
        // Cannot reverse the wheel within one step.
        const float stopT = std::fabs(w.spin) * 0.01f / h + std::fabs(w.torque);
        bt = std::min(bt, stopT);
        w.torque -= (w.spin >= 0 ? 1.0f : -1.0f) * bt;
        w.locked = w.handbrakeInput > 0.5f && std::fabs(w.spin) < 1.0f;
    }
    (void)h;
}

// Tyre model (0x00323eb0, 0x003242b0, friction circle 0x00324460, spin 0x00323ca0).
void Vehicle::TyreForces(float h)
{
    Body &b = m_body;
    const float reflected =
        m_drive.mode == 3 ? m_engine.inertia * std::pow(m_drive.ratio[m_drive.gear] *
                                                            (m_drive.rearSplit > 0.5f ? m_drive.rearFinal : m_drive.frontFinal), 2.0f)
                          : 0.0f;
    int drivenCount = 0;
    for (const Wheel &w : m_wheel)
        drivenCount += w.driven;
    for (int i = 0; i < 4; ++i) {
        Wheel &w = m_wheel[i];
        const float inertia = 0.01f + (w.driven && drivenCount ? reflected / float(drivenCount) : 0.0f);
        if (w.load <= 0) {
            // Airborne: spin follows the torque.
            w.spin += w.torque / inertia * h;
            w.longForce = w.latForce = 0;
            w.sliding = false;
            w.saturation = 0;
            continue;
        }
        // Wheel frame on the ground plane: heading rotated by the steering angle.
        const Vec3 fwdBody = {std::cos(w.steerAngle), std::sin(w.steerAngle), 0};
        Vec3 fwd = ToWorld(fwdBody);
        fwd = Norm(fwd - w.normal * Dot(fwd, w.normal));
        const Vec3 side = Cross(w.normal, fwd);
        const Vec3 pv = b.vel + Cross(b.omega, w.contact - b.pos);
        const float vx = Dot(pv, fwd), vy = Dot(pv, side);
        const float slip = w.spin * w.radius - vx; // +0x118
        const float load = w.load;
        const float mu = w.longMu; // surface 0 (asphalt) = 1
        const float slipAngle = std::atan2(std::fabs(vy), std::max(std::fabs(vx), 0.5f));
        // Lateral force from the slip-angle/load curves (0x003242b0).
        const float band = slipAngle * 57.2958f * (12.0f / w.slipScale) * 0.5f;
        const float x = load * 0.8f / w.loadNorm;
        const int bi = int(band);
        float latMag;
        if (bi < 6) {
            const float a0 = Curve(kLateral[bi], 6, 0, 10, x), a1 = Curve(kLateral[bi + 1], 6, 0, 10, x);
            latMag = a0 + (band - float(bi)) * (a1 - a0);
        } else {
            latMag = Curve(kLateral[6], 6, 0, 10, x);
        }
        latMag *= w.latScale * 2.5f;
        const float ft = vy > 0 ? -latMag : latMag;
        // Longitudinal: the force that would pin the wheel to the ground speed within this step
        // (stand-in for the wheel/body consistency solve of 0x003183a0). If it exceeds the friction
        // left over by the lateral force the tyre slides — wheelspin under power, lockup under
        // braking — and the wheel is left free to spin instead of being pinned to the ground.
        const float flRoll = w.torque / w.radius + inertia * slip / (w.radius * w.radius * h);
        const float limit = load * w.circleMu; // friction circle (0x00324460)
        const float flAvail = std::sqrt(std::max(0.0f, limit * limit - ft * ft));
        bool sliding;
        float fl;
        if (w.locked) {
            sliding = true;
            fl = -load * mu * (vx >= 0 ? 1.0f : -1.0f);
        } else if (std::fabs(flRoll) <= flAvail) {
            sliding = false;
            fl = flRoll;
        } else {
            sliding = true;
            fl = flAvail * (flRoll >= 0 ? 1.0f : -1.0f);
        }
        sliding = sliding || std::fabs(ft) >= limit;
        w.saturation = limit > 1e-3f ? std::min(1.0f, std::sqrt(fl * fl + ft * ft) / limit) : 1.0f;
        w.dbgVy = vy, w.dbgSa = slipAngle * 57.2958f, w.dbgLat = ft;
        // Stand-in for the 5-pass solve of 0x003183a0: lateral force cannot reverse the contact's
        // sideways velocity within a step (per-wheel quarter of the mass).
        const float maxFt = 0.25f * b.mass * std::fabs(vy) / h;
        const float ftClamped = std::clamp(ft, -maxFt, maxFt);
        w.longForce = fl, w.latForce = ftClamped, w.sliding = sliding;
        ApplyForce(fwd * fl + side * ftClamped, w.contact);

        // Spin (0x00323ca0): rolling wheels turn with the ground; sliding wheels spin up/down under
        // the driving/braking torque and the tyre reaction.
        if (!sliding) {
            w.spin = vx / w.radius;
        } else {
            float dspin = (w.torque - fl * w.radius - w.spin * 0.002f) / inertia;
            w.spin += dspin * h;
        }
    }
}

void Vehicle::Transform(float m[12]) const
{
    const Vec3 o = Origin();
    const Mat3 &r = m_body.rot;
    const float p[3] = {o.x, o.y, o.z};
    for (int row = 0; row < 3; ++row) {
        m[row * 4 + 0] = r.r[row].x;
        m[row * 4 + 1] = r.r[row].y;
        m[row * 4 + 2] = r.r[row].z;
        m[row * 4 + 3] = p[row];
    }
}

Vec3 Vehicle::Origin() const { return m_body.pos - ToWorld(m_body.cog); }
float Vehicle::Yaw() const { return std::atan2(m_body.rot.r[0].y, m_body.rot.r[0].x); }
float Vehicle::ForwardSpeed() const { return Dot(m_body.vel, m_body.rot.r[0]); }
int Vehicle::Gear() const { return m_drive.gear == 0 ? -1 : m_drive.gear - 1; }

float Vehicle::Slip() const
{
    float s = 0;
    for (const Wheel &w : m_wheel)
        if (w.load > 0)
            s = std::max(s, w.sliding ? std::min(1.0f, std::fabs(w.latForce) > 0 ? 1.0f : 0.5f) : 0.0f);
    return s;
}

bool Vehicle::Airborne() const
{
    for (const Suspension &s : m_susp)
        if (s.grounded)
            return false;
    return true;
}

void Vehicle::ScaleSpeed(float f)
{
    m_body.momentum = m_body.momentum * f;
    UpdateDerived();
}

} // namespace UG2Physics
