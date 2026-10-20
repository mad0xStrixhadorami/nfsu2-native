#pragma once
#include <cstdint>
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
    const Performance &GetPerformance() const { return m_perf; }
    int Gear() const { return m_gear; }  // 1.. forward, -1 reverse, 0 neutral
    float Rpm() const { return m_rpm; }
    // Engine torque (kN*m) at rpm from the curve.
    static float Torque(const Performance &p, float rpm);
    // Straight-line run with this drivetrain at full throttle: 0-100 km/h time (s) and top speed (m/s).
    static void Simulate(const Performance &p, float wheelRadius, float &zeroTo100, float &topSpeed);

    // Assemble the stock body (KIT00 + BASE, highest LOD) from CARS\<name>\GEOMETRY.BIN.
    // wheels (optional): placement from the car database; otherwise estimated from the body.
    bool LoadModel(const std::vector<uint8_t> &geometry, const Wheel *wheels = nullptr);
    // Height of the car origin above the ground (wheel radius - centre height; 0 if unknown).
    float RideHeight() const { return m_rideHeight; }
    const std::vector<SolidMesh::Triangle> &Model() const { return m_model; }
    // Texture name hash of each Model() triangle (0 = none): resolved from its part's 0x134012 list.
    const std::vector<uint32_t> &TriangleTextures() const { return m_triTex; }
    // 1 for a wheel's painted rim face, 0 for the tyre/backing/body (same size as Model()).
    const std::vector<uint8_t> &TriangleRims() const { return m_triRim; }
    // 1 for a brake caliper, 0 for every other triangle (same size as Model()).
    const std::vector<uint8_t> &TriangleCalipers() const { return m_triCaliper; }
    // Model() triangles from this index on are the four wheels.
    size_t WheelTrianglesStart() const { return m_wheelStart; }

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
    void Nudge(float dx, float dy) { m_x += dx, m_y += dy; }
    void ScaleSpeed(float f) { m_speed *= f; }
    // Shares another car's assembled model (opponents of the same make).
    void SetModel(const PlayerCar &other) { m_model = other.m_model, m_triTex = other.m_triTex, m_triRim = other.m_triRim,
                                                 m_triCaliper = other.m_triCaliper, m_rideHeight = other.m_rideHeight,
                                                 m_wheelStart = other.m_wheelStart, m_wheelRadius = other.m_wheelRadius; }

private:
    void UpdateEngine(float throttle, float brake, float dt, float power);

    std::vector<SolidMesh::Triangle> m_model;
    std::vector<uint32_t> m_triTex;
    std::vector<uint8_t> m_triRim;
    std::vector<uint8_t> m_triCaliper;
    float m_x = 0, m_y = 0, m_z = 0, m_yaw = 0;
    float m_pitch = 0, m_roll = 0;
    float m_speed = 0; // m/s along the heading
    float m_vz = 0;    // vertical speed while airborne
    float m_slip = 0;
    float m_rideHeight = 0;
    size_t m_wheelStart = 0;
    Performance m_perf;
    int m_gear = 1;
    float m_rpm = 0;
    UG2Physics::Vehicle m_ug2;
    bool m_hold = false;
    float m_wheelRadius = 0.32f;
};
