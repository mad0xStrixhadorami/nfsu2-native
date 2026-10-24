#pragma once
#include <cstdint>

class TrackWorld;

// Port of UG2's vehicle physics (ISSUES.md #129, map in docs/physics.md): a rigid body on four
// probe suspensions with slip-curve tyres, driven by an engine / clutch / gearbox model, stepped
// in fixed 0.0044 s explicit-Euler substeps like Vehicle::Update 0x00317428. Every parameter comes
// from the car database record (GLOBALB 0x34600, 0x890 bytes; parameter block P = record+0x118).
// Units as in the game: metres, seconds, tonnes, kN, kN*m. Car space: +X forward, +Y left, +Z up.
//
// Not ported yet (stand-ins marked in the code): the 5-pass tyre/body consistency solve of
// 0x003183a0 (replaced by a per-step friction-budget longitudinal model), body/world collision
// (0x334110... replaced by a swept wall test that slides the car along barriers), the steering input
// filter (player controller), the drift grip assist 0x00317330, nitrous and turbo boost, surface
// types (all asphalt), engine rock 0x0030c2e8.
namespace UG2Physics {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

struct Controls {
    float throttle = 0, brake = 0, handbrake = 0; // 0..1
    float steer = 0;                              // -1 (right) .. 1 (left)
    bool nitrous = false;                         // the NOS button is held (0x0035e868 cases 0xa3 / 0xa4)
};

class Vehicle {
public:
    // record: the car's 0x890-byte database record.
    bool Setup(const uint8_t *record);
    bool Valid() const { return m_valid; }
    // Places the car with its model origin at (x, y, z) (z = road height + ride height) heading yaw.
    void Place(float x, float y, float z, float yaw);
    void Update(const TrackWorld &world, const Controls &in, float dt);

    // Model origin (not the centre of mass) and orientation, rows = forward, left, up axes.
    void Transform(float m[12]) const;
    Vec3 Origin() const;
    float Yaw() const;
    float ForwardSpeed() const;  // m/s along the heading (negative reversing)
    float Rpm() const { return m_engine.speed * kRadToRpm; }
    // Manual gearbox (Options > Player > Transmission): the automatic's up / down shifts are off and
    // ManualShift(+1 / -1) changes gear while the clutch is engaged.
    void SetManual(bool manual) { m_manual = manual; }
    void ManualShift(int dir);
    int Gear() const;            // 1.. forward, 0 neutral, -1 reverse
    float Slip() const;          // 0..1 tyre slide amount (sound/skids)
    float WheelRadius() const { return m_wheel[0].radius; }
    bool Airborne() const;
    // Car-to-car contact (resolved by the caller).
    void Nudge(float dx, float dy) { m_body.pos.x += dx, m_body.pos.y += dy; }
    void ScaleSpeed(float f);

    // Rigid-body access for car-to-car contacts (ISSUES #219).  Positions and velocities are world
    // space, mass in tonnes.
    Vec3 CogPosition() const { return m_body.pos; }
    Vec3 VelocityAt(const Vec3 &p) const;
    float Mass() const { return m_body.mass; }
    // Effective inverse mass along the unit direction n for an impulse at the world point p.
    float InverseMassAlong(const Vec3 &p, const Vec3 &n) const;
    // Adds the impulse J (tonne*m/s) at the world point p to the body's linear and angular momentum.
    void ApplyImpulse(const Vec3 &J, const Vec3 &p);
    // The car's rectangle in the ground plane: centre offset along the car's forward axis (from the
    // centre of mass) and half length / width, from the wheel hardpoints plus overhang.
    void Footprint(float &centreX, float &halfLength, float &halfWidth) const;
    // Speed (m/s) of the hardest wall hit since the last call, 0 if none (read by the crash sound).
    float TakeWallImpact()
    {
        const float v = m_wallImpact;
        m_wallImpact = 0.0f;
        return v;
    }
    Vec3 ForwardAxis() const { return m_body.rot.r[0]; }
    Vec3 LeftAxis() const { return m_body.rot.r[1]; }

    // Drifting (ISSUES #291): an arcade layer over the faithful tyre model. The handbrake and a power-over (full throttle with
    // lock on) release the tail, the car keeps its speed and gets counter-steer and spin-out help while it slides, and the
    // slide scores points and tops up the nitrous.
    // Per-wheel state for the tyre smoke and skid marks (ISSUES #294): FL, FR, RR, RL.
    bool WheelSliding(int i) const { return m_wheel[i].sliding && m_wheel[i].load > 0.0f; }
    float WheelSaturation(int i) const { return m_wheel[i].saturation; }
    Vec3 WheelContactPoint(int i) const { return m_wheel[i].contact; }
    float BodySlipAngle() const { return m_drift.beta; }   // rad, + = the car travels to the left of its nose
    float DriftAmount() const { return m_drift.amount; }   // 0..1, how deep into a drift (smoothed)
    bool Drifting() const { return m_drift.active; }
    float DriftChain() const { return m_drift.chain; }     // points of the running drift before the multiplier
    int DriftMultiplier() const { return m_drift.mult; }
    float DriftTotal() const { return m_drift.total; }     // banked points
    float DriftBanner() const { return m_drift.bankedShow; } // seconds the last banked chain stays on the HUD
    float DriftLastBank() const { return m_drift.lastBank; }

    // Nitrous (ISSUES #217).  The tank holds `capacity` seconds (record +0x3b8, 4000 ticks per
    // second in the original) and drains in real time while a burst runs; its fraction (0.305400)
    // can be filled past 1 (up to 2 by 0x003054a8), and the burst level 1..3 comes from the fraction
    // (0x00305150: > 0 / > 1.4 / > 1.7), scaling the extra torque by {1.0, 1.4, 1.7}.
    bool HasNitrous() const { return m_nos.capacity > 0.0f; }
    float NitrousFraction() const { return m_nos.capacity > 0.0f ? m_nos.fill / m_nos.capacity : 0.0f; }
    bool NitrousActive() const { return m_nos.active; }
    int NitrousLevel() const;
    // 0x003054a8: add (or remove) a fraction of the tank, clamped to 0..2.
    void AddNitrous(float fraction);

private:
    static constexpr float kStep = 0.0044f;       // DAT_00568b70
    static constexpr float kGravity = 9.8f;       // gp-0xbb8
    static constexpr float kRadToRpm = 9.5493f;   // DAT_00563490

    struct Mat3 {
        Vec3 r[3]; // rows: body X, Y, Z axes in world coordinates
    };
    struct Body {
        float mass = 1, invMass = 1;
        Vec3 inertia, invInertia; // diagonal, body frame
        Vec3 cog;                 // centre of mass in car space (P+0)
        Vec3 pos;                 // centre of mass, world
        Mat3 rot;
        Vec3 momentum, angMomentum; // world
        Vec3 vel, omega;            // derived
        Vec3 force, torque;         // accumulators (world)
        float rest = 0;             // +0x90 low-speed settling scalar
    };
    struct Suspension {
        float progressive, k, bump, rebound, antiRoll, maxTravel, minTravel; // rec+0x288/+0x2a8
        float preload = 0;      // +0x88 static load share (kN)
        float x = 0, v = 0;     // compression, rate (state +0x1c/+0x20)
        float force = 0;        // +0x24
        bool grounded = false;  // +0x80
        int other = 0;          // anti-roll partner
    };
    struct Wheel {
        Vec3 local;             // hardpoint relative to the CoG (car frame)
        float radius, width;
        float slipScale, loadNorm, latScale, circleMu, longMu; // axle params rec+0x1e8/+0x208
        float brakeBias = 1, handbrakeBias = 0;
        float steerAngle = 0;
        float spinAngle = 0, spin = 0;  // state (+0x24 / +0x28), rad and rad/s
        float torque = 0;               // +0x108 drive + brake torque this step
        float prevTorque = 0;           // +0x120 smoothing
        float load = 0;                 // +0x164
        float longForce = 0, latForce = 0; // +0x124 / +0x128
        float saturation = 0;           // +0x170
        bool sliding = false;           // +0x160
        bool locked = false;            // +0x10c
        bool driven = false;
        float brakeInput = 0, handbrakeInput = 0;
        Vec3 contact, normal{0, 0, 1};
        float dbgVy = 0, dbgSa = 0, dbgLat = 0; // NFSU2_PHYS_DEBUG
    };
    struct Engine {
        float idle, redline, maxRpm, inertia;
        float torque[9], turbo[9], braking[3]; // rec+0x318 / +0x33c / +0x360
        float speed = 0;     // rad/s
        float load = 0;      // +0x7c reaction torque from the clutch this step
        bool limiter = false;// +0x88
        float throttle = 0;
    };
    struct Drivetrain {
        float clutchGain, frontFinal, rearFinal, rearSplit, clutchStiffness; // rec+0x2c8..
        int numGears;
        float clutchRate;
        float ratio[8];         // rec+0x2e8: reverse, neutral, 1st..
        float shiftUp[8], shiftDown[8];
        int gear = 2, targetGear = 2; // +0x3c / +0x38 (0 reverse, 1 neutral, 2 first)
        int mode = 3;           // +0x44: 0 disengage, 1 select, 2 engage, 3 engaged
        float clutch = 1;       // state +0x14
    };

    void Substep(const TrackWorld &world, float h);
    void CollideWorld(const TrackWorld &world, float dt);
    void UpdateDerived();
    void ApplyForce(const Vec3 &f, const Vec3 &at);
    void SuspensionAndContact(const TrackWorld &world, float h);
    void TyreForces(float h);
    void DriveTorque(float h);
    bool m_manual = false;
    void AutoGearbox();
    void Aero();
    float EngineTorque(float speed, float throttle) const;
    Vec3 ToWorld(const Vec3 &v) const;
    Vec3 ToBody(const Vec3 &v) const;

    bool m_valid = false;
    Body m_body;
    Suspension m_susp[4];
    Wheel m_wheel[4];
    Engine m_engine;
    Drivetrain m_drive;
    float m_dragCoef = 0, m_downFront = 0, m_downRear = 0;
    float m_brakeTorque = 0, m_handbrakeTorque = 0, m_maxSteer = 0;
    float m_limiterSpeed = 0; // m/s, rec+0x3a8 mph
    struct Nitrous {
        float capacity = 0; // s, rec +0x3b8
        float blend = 0;    // rec +0x3c4 (frame +0x2ac), the torque scale
        float fill = 0;     // s
        bool active = false;
    };
    Nitrous m_nos;
    struct Drift {
        float beta = 0;                     // body slip angle (rad)
        float amount = 0;                   // 0..1 smoothed depth of the drift
        bool active = false;
        bool deliberate = false;            // latched by the handbrake: the strong drift assists only apply then
        float rearMul = 1, frontMul = 1;    // lateral grip multipliers on the tyre curves
        float assist = 0;                   // extra road-wheel angle from the counter-steer assist (rad)
        float time = 0, idle = 0;           // seconds in this chain, since the last scoring
        float chain = 0, total = 0, lastBank = 0, bankedShow = 0;
        int mult = 1;
    };
    Drift m_drift;
    int m_shiftDbgGear = -1;
    bool m_driftHbWas = false; // handbrake state last update (edge for the flick)
    void UpdateDrift(float dt);
    void DriftForces(float h);
    float m_wallImpact = 0.0f;
    void UpdateNitrous(float dt);
    Controls m_in;
    float m_accum = 0;
    bool m_groundBelow = true; // some wheel found terrain beneath it (grounded or not)
    bool m_grounded = false;   // any wheel touching this substep
    float m_lastGroundZ = 0;   // body CoG z at the last grounded substep (world-floor recovery)
};

} // namespace UG2Physics
