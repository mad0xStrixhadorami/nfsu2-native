#pragma once
#include <array>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "game/indep/Physics/UG2Vehicle.h"
#include "game/indep/World/SolidMesh.h"

class TrackWorld;

// Native stand-in for the player vehicle: UG2's car physics/tuning model (0x0031d570 and friends,
// ISSUES.md #94) is not ported, so this is a simple arcade model that follows the world's road
// surface. Car space is +X forward, +Y left, +Z up (same as the world).
class PlayerCar {
public:
    // A wheel from the car database record (GLOBALB 0x34600, 4 x 0x30 bytes from +0x128):
    // centre x, y, z in car space, radius (+0x10) and width (+0x14). Order FL, FR, RR, RL.
    struct Wheel {
        float x, y, z, radius, width;
    };
    // Engine and chassis from the car database record (GLOBALB 0x34600, stock tuning level).
    struct Performance {
        float mass = 0;           // +0x228 tonnes
        float efficiency = 0.8f;  // +0x2c8 drivetrain
        float finalDrive = 0;     // +0x2d0
        float reverse = 0;        // +0x2e8 (negative)
        float gears[6] = {};      // +0x2f0.., 0 = no such gear
        int numGears = 0;
        float idleRpm = 0, redline = 0, maxRpm = 0; // +0x308 / +0x30c / +0x310
        float torque[9] = {};     // +0x318: kN*m at 9 evenly spaced points from idle to max rpm
        float aero = 0;           // +0x378: kN per (m/s)^2
        float grip = 1;           // +0x388 tyre friction
        float rolling = 0.015f;   // +0x3c8
        float limiter = 0;        // +0x3a8 top speed limit, mph (155 for most cars, Hummer 125)
        bool Valid() const { return mass > 0 && finalDrive > 0 && numGears > 0 && maxRpm > idleRpm; }
    };
    void SetPerformance(const Performance &p) { m_perf = p; m_gear = 1; m_rpm = p.idleRpm; }
    // UG2's real vehicle physics from the car's database record (ISSUES.md #129); when set up,
    // Update/Place/Transform run it instead of the arcade model.
    bool UseUG2Physics(const uint8_t *record) { return m_ug2.Setup(record); }
    bool UsingUG2Physics() const { return m_ug2.Valid(); }
    // Holds the car with the handbrake (race countdown: stops it rolling on slopes).
    void SetHold(bool hold) { m_hold = hold; }
    // The player's handbrake (X / Left Shift): releases the rear tyres' grip and starts a drift (ISSUES #291).
    void SetHandbrake(bool on) { m_handbrake = on; }
    // Drift state from the UG2 vehicle model (all zero for the arcade model).
    // Tyre state for the smoke and skid marks (0 FL, 1 FR, 2 RR, 3 RL; zero for the arcade model).
    bool WheelSliding(int i) const { return m_ug2.Valid() && m_ug2.WheelSliding(i); }
    float WheelSaturation(int i) const { return m_ug2.Valid() ? m_ug2.WheelSaturation(i) : 0.0f; }
    void WheelContact(int i, float &x, float &y, float &z) const
    {
        const UG2Physics::Vec3 c = m_ug2.WheelContactPoint(i);
        x = c.x, y = c.y, z = c.z;
    }
    bool Drifting() const { return m_ug2.Valid() && m_ug2.Drifting(); }
    float DriftAmount() const { return m_ug2.Valid() ? m_ug2.DriftAmount() : 0.0f; }
    float DriftAngleDeg() const { return m_ug2.Valid() ? m_ug2.BodySlipAngle() * 57.2958f : 0.0f; }
    float DriftChain() const { return m_ug2.Valid() ? m_ug2.DriftChain() : 0.0f; }
    int DriftMultiplier() const { return m_ug2.Valid() ? m_ug2.DriftMultiplier() : 1; }
    float DriftTotal() const { return m_ug2.Valid() ? m_ug2.DriftTotal() : 0.0f; }
    float DriftBanner() const { return m_ug2.Valid() ? m_ug2.DriftBanner() : 0.0f; }
    float DriftLastBank() const { return m_ug2.Valid() ? m_ug2.DriftLastBank() : 0.0f; }
    // Nitrous (UG2 physics only, ISSUES #217): the button state for the next Update, the tank
    // (fraction of capacity, 0..2) and whether a burst is running.
    void SetManual(bool manual) { if (m_ug2.Valid()) m_ug2.SetManual(manual); }
    void ManualShift(int dir) { if (m_ug2.Valid()) m_ug2.ManualShift(dir); }
    void SetNitrousButton(bool held) { m_nitrousButton = held; }
    bool HasNitrous() const { return m_ug2.Valid() && m_ug2.HasNitrous(); }
    float NitrousFraction() const { return m_ug2.NitrousFraction(); }
    bool NitrousActive() const { return m_ug2.NitrousActive(); }
    int NitrousLevel() const { return m_ug2.NitrousLevel(); }
    void AddNitrous(float fraction) { m_ug2.AddNitrous(fraction); }
    float TakeWallImpact() { return m_ug2.Valid() ? m_ug2.TakeWallImpact() : 0.0f; }
    // The UG2 rigid body, or null for the arcade stand-in (car-to-car contacts, #219).
    UG2Physics::Vehicle *Body() { return m_ug2.Valid() ? &m_ug2 : nullptr; }
    const Performance &GetPerformance() const { return m_perf; }
    int Gear() const { return m_gear; }  // 1.. forward, -1 reverse, 0 neutral
    // Lamp state for the renderer (ISSUES #284): the brake lights burn while the brake pedal or handbrake is held, the
    // reverse lamps while the car is in reverse gear.
    bool BrakeLamp() const { return m_lampBrake; }
    bool ReverseLamp() const { return m_gear == -1; }
    float Rpm() const { return m_rpm; }
    // Engine torque (kN*m) at rpm from the curve.
    static float Torque(const Performance &p, float rpm);
    // Straight-line run with this drivetrain at full throttle: 0-100 km/h time (s) and top speed (m/s).
    static void Simulate(const Performance &p, float wheelRadius, float &zeroTo100, float &topSpeed);
    // The same run with the Dyno Results figures: seconds to 60 mph and 100 mph (0 = not reached).
    struct RunTimes {
        float zeroTo60mph = 0, zeroTo100mph = 0, zeroTo100kmh = 0, topSpeed = 0; // topSpeed in m/s
    };
    static RunTimes SimulateRun(const Performance &p, float wheelRadius);

    // Aftermarket parts installed on this car (ISSUES.md #178).  The shops install whole parts:
    // roof scoops and exhaust tips come from the universal CARS\ROOF and CARS\EXHAUST geometry and
    // are mounted at the body's position markers, while engine accents are car-specific solids that
    // already live in car space.  Style 0 keeps the stock part (no extra solid).
    struct InstalledParts {
        int roof = 0;     // 1..17 -> ROOF_STYLE<NN>[_DUAL|_OFFSET][_CF], LOD _A
        int exhaust = 0;  // 1..10 -> EXHAUST_STYLE<NN>_LEVEL1_A
        int engine = 0;   // 1..3  -> <CAR>_STYLE<NN>_ENGINE_A
        int spoiler = 0;  // 1..40 -> SPOILER_STYLE<NN>[_CF]_A
        // Body panels from the car's own GEOMETRY.BIN (ISSUES #188): the style number is the
        // catalogue's model-table _STYLE<NN> (#188), not the shop list position.
        int hood = 0;      // 1..28 -> <CAR>_STYLE<NN>_HOOD[_LEFT|_RIGHT]_A
        int headlight = 0; // 1..14 -> <CAR>_STYLE<NN>_HEADLIGHT_LEFT_A / _RIGHT_A
        int taillight = 0; // 1..14 -> <CAR>_STYLE<NN>_BRAKELIGHT_A
        // Body panels that use the kit number instead (ISSUES #190): the catalogue's model table
        // names them _KIT<NN>_<PANEL>, and the car file has <CAR>_KIT<NN>_FRONT_BUMPER_A etc.
        int frontBumper = 0; // 1..30 -> <CAR>_KIT<NN>_FRONT_BUMPER_A
        int rearBumper = 0;  // 1..30 -> <CAR>_KIT<NN>_REAR_BUMPER_A
        int skirt = 0;       // 1..30 -> <CAR>_KIT<NN>_SKIRT_A
        int wideBody = 0;    // 1..4 -> <CAR>_KITW<NN>_BODY_A..D + door/sill panels (#193)
        bool roofDual = false, roofOffset = false, roofCarbon = false, spoilerCarbon = false;
        bool hoodCarbon = false; // the carbon hood reuses the painted solid (no _CF geometry)
        std::vector<uint8_t> roofGeometry;    // CARS\ROOF\GEOMETRY.BIN
        std::vector<uint8_t> exhaustGeometry; // CARS\EXHAUST\GEOMETRY.BIN
        std::vector<uint8_t> spoilerGeometry; // CARS\SPOILER\GEOMETRY.BIN
        // Rims (ISSUES #226): the brand's CARS\WHEELS\GEOMETRY_<KEY>.BIN, the solid prefix and the
        // style; the solid of that style whose size is nearest the car's wheel replaces the stock one.
        std::vector<uint8_t> rimGeometry;
        std::string rimBrand;
        int rimStyle = 0;
        int neon = 0; // underglow colour 1..10 (ISSUES #227)
        int tint = 0; // window tint 1..3
    };
    // Dev install: NFSU2_PART_ROOF=<n>[_DUAL|_OFFSET][_CF], NFSU2_PART_EXHAUST=<n>,
    // NFSU2_PART_ENGINE=<n>, NFSU2_PART_SPOILER=<n>[_CF], NFSU2_PART_HOOD=<n>[_CF],
    // NFSU2_PART_HEADLIGHT=<n>, NFSU2_PART_TAILLIGHT=<n>, NFSU2_PART_FRONT_BUMPER=<kit>,
    // NFSU2_PART_REAR_BUMPER=<kit>, NFSU2_PART_SKIRT=<kit> and NFSU2_PART_WIDEBODY=<kit>.
    // Loads the universal part files through `read` (game path -> bytes); the car-specific panels
    // need no extra file.
    static InstalledParts InstalledPartsFromEnvironment(
        const std::function<std::vector<uint8_t>(const char *)> &read);

    // Assemble the stock body (KIT00 + BASE, highest LOD) from CARS\<name>\GEOMETRY.BIN.
    // wheels (optional): placement from the car database; otherwise estimated from the body.
    // parts (optional): aftermarket parts to mount on top of the stock body (ISSUES.md #178).
    bool LoadModel(const std::vector<uint8_t> &geometry, const Wheel *wheels = nullptr,
                   const InstalledParts *parts = nullptr);
    // Height of the car origin above the ground (wheel radius - centre height; 0 if unknown).
    float RideHeight() const { return m_rideHeight; }
    const std::vector<SolidMesh::Triangle> &Model() const { return m_model; }
    // Texture name hash of each Model() triangle (0 = none): resolved from its part's 0x134012 list.
    const std::vector<uint32_t> &TriangleTextures() const { return m_triTex; }
    // 1 for a wheel's painted rim face, 0 for the tyre/backing/body (same size as Model()).
    const std::vector<uint8_t> &TriangleRims() const { return m_triRim; }
    // 1 for a brake caliper, 0 for every other triangle (same size as Model()).
    const std::vector<uint8_t> &TriangleCalipers() const { return m_triCaliper; }
    // 1 for a side-mirror housing (slot 1; the glass in slot 0 stays), 0 otherwise (#174).
    const std::vector<uint8_t> &TriangleMirrors() const { return m_triMirror; }
    // 1 for the stock spoiler mesh, 0 otherwise (#175).  The solid's header transform mounts it.
    const std::vector<uint8_t> &TriangleSpoilers() const { return m_triSpoiler; }
    // 1 for an installed aftermarket part (#178): roof scoop, engine accent and muffler tip.  The
    // three paint cards edit these parts, not the stock geometry.
    const std::vector<uint8_t> &TriangleRoofScoops() const { return m_triRoof; }
    const std::vector<uint8_t> &TriangleEngines() const { return m_triEngine; }
    const std::vector<uint8_t> &TriangleExhausts() const { return m_triExhaust; }
    // Wheel spin and steering (ISSUES #257 / #258): the wheel (0..3, order FL FR RR RL) each Model() triangle belongs to
    // (-1 = none; the tyre, rim and spinner pieces) and each brake triangle belongs to (-1 = none; it steers but does not
    // spin), the hubs' car-space (x, y, z) as 12 floats, the rolling angle in radians (speed / radius, integrated) and the
    // front wheels' steering angle (radians, positive = left).
    const std::vector<int8_t> &TriangleWheels() const { return m_triWheel; }
    const std::vector<int8_t> &TriangleBrakes() const { return m_triBrake; }
    const float *WheelHubs() const { return m_hub.data(); }
    float WheelAngle() const { return m_wheelAngle; }
    float WheelRate() const { return m_wheelRate; } // true rolling speed, rad/s (the shown angle is capped, see AdvanceWheels)
    float WheelSteer() const { return m_steerAngle; }
    void AdvanceWheels(float dt)
    {
        // The true roll rate is speed / radius; the SHOWN rate is soft-capped. A wheel turning half a spoke spacing per frame
        // (about 30 km/h at 60 fps) strobes: the spokes jitter and seem to turn backwards, a wagon-wheel effect the original
        // hides with its blur. Above ~14 rad/s the spokes keep turning forward only slowly while the shader's spoke blur
        // (driven by the true rate) takes over (ISSUES #289).
        m_wheelRate = m_speed / std::max(0.1f, m_wheelRadius);
        const float mag = std::fabs(m_wheelRate), shown = mag <= 14.0f ? mag : 14.0f + std::min(10.0f, (mag - 14.0f) * 0.12f);
        m_wheelAngle = std::fmod(m_wheelAngle + (m_wheelRate < 0 ? -shown : shown) * dt, 6.2831853f);
        // The road wheels follow the steering input with the same lock rule as the arcade model (less lock at speed),
        // eased so a keyboard tap does not snap the wheels.
        const float lock = 0.55f / (1.0f + std::fabs(m_speed) * 0.05f);
        const float target = std::clamp(m_steerIn, -1.0f, 1.0f) * lock;
        m_steerAngle += (target - m_steerAngle) * std::min(1.0f, std::max(dt, 0.0f) * 14.0f);
        if (const char *a = getenv("NFSU2_WHEEL_ANGLE")) // dev aid: hold the rolling angle (radians) to inspect the wheel
            m_wheelAngle = float(atof(a));
        if (const char *t = getenv("NFSU2_STEER_TEST")) // dev aid: a fixed road-wheel angle in radians
            m_steerAngle = float(atof(t));
    }
    // Model() triangles from this index on are the four wheels.
    size_t WheelTrianglesStart() const { return m_wheelStart; }
    // Body-panel styles actually present in the loaded car file (#192).  The shop hides catalogue
    // designs a car does not ship and LoadModel keeps the stock panel when a fitted style is absent.
    const SolidMesh::BodyStyleSet &BodyStyles() const { return m_bodyStyles; }

    void Place(float x, float y, float z, float yaw);
    // throttle/brake/steer in [-1, 1]; dt in seconds. walls: collide with steep world triangles
    // (route-following opponents skip it: steering only at the route line, they snag on kerbs and
    // street furniture beside it -- a 2-lap test took 1:05 instead of 0:52 per lap with walls on).
    // power: engine torque scale, 1 normally and the drag gearbox's output in a drag race (ISSUES #115).
    void Update(const TrackWorld &world, float throttle, float brake, float steer, float dt, bool walls = true,
                float power = 1.0f);

    // Row-major 3x4 car-to-world transform: rows are the forward, left, up axes; column 3 the position.
    void Transform(float m[12]) const;
    float X() const { return m_x; }
    float Y() const { return m_y; }
    float Z() const { return m_z; }
    float Yaw() const { return m_yaw; }
    float Speed() const { return m_speed; }
    float Slip() const { return m_slip; } // tyre slip 0..1 (cornering at the limit / hard braking)
    // Car-to-car contact (resolved by the caller): move without physics, scale the speed.
    // With a UG2 body these act on the body (its pose is rewritten from it every update).
    void Nudge(float dx, float dy)
    {
        m_x += dx, m_y += dy;
        if (m_ug2.Valid())
            m_ug2.Nudge(dx, dy);
    }
    void ScaleSpeed(float f)
    {
        m_speed *= f;
        if (m_ug2.Valid())
            m_ug2.ScaleSpeed(f);
    }
    void AddSpeed(float dv) { m_speed += dv; } // arcade model: along the heading
    // Shares another car's assembled model (opponents of the same make).
    void SetModel(const PlayerCar &other) { m_model = other.m_model, m_triTex = other.m_triTex, m_triRim = other.m_triRim,
                                                 m_triCaliper = other.m_triCaliper, m_triMirror = other.m_triMirror,
                                                 m_triSpoiler = other.m_triSpoiler, m_triRoof = other.m_triRoof,
                                                 m_triEngine = other.m_triEngine, m_triExhaust = other.m_triExhaust,
                                                 m_bodyStyles = other.m_bodyStyles,
                                                 m_rideHeight = other.m_rideHeight,
                                                 m_wheelStart = other.m_wheelStart, m_wheelRadius = other.m_wheelRadius,
                                                 m_triWheel = other.m_triWheel, m_triBrake = other.m_triBrake, m_hub = other.m_hub; }

private:
    void UpdateEngine(float throttle, float brake, float dt, float power);

    std::vector<SolidMesh::Triangle> m_model;
    std::vector<uint32_t> m_triTex;
    std::vector<uint8_t> m_triRim;
    std::vector<uint8_t> m_triCaliper;
    std::vector<uint8_t> m_triMirror;
    std::vector<uint8_t> m_triSpoiler;
    std::vector<uint8_t> m_triRoof;
    std::vector<uint8_t> m_triEngine;
    std::vector<uint8_t> m_triExhaust;
    SolidMesh::BodyStyleSet m_bodyStyles;
    float m_x = 0, m_y = 0, m_z = 0, m_yaw = 0;
    float m_pitch = 0, m_roll = 0;
    float m_speed = 0; // m/s along the heading
    float m_vz = 0;    // vertical speed while airborne
    float m_slip = 0;
    float m_rideHeight = 0;
    size_t m_wheelStart = 0;
    std::vector<int8_t> m_triWheel, m_triBrake;
    std::array<float, 12> m_hub{};
    float m_wheelAngle = 0, m_wheelRate = 0, m_steerAngle = 0, m_steerIn = 0;
    Performance m_perf;
    int m_gear = 1;
    float m_rpm = 0;
    UG2Physics::Vehicle m_ug2;
    bool m_nitrousButton = false;
    bool m_hold = false;
    bool m_handbrake = false;
    bool m_lampBrake = false;
    float m_wheelRadius = 0.32f;
};
