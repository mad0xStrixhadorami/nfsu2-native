#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "game/indep/World/SolidMesh.h"

class TrackWorld;

// Native stand-in for the player vehicle: UG2's car physics/tuning model (0x0031d570 and friends,
// ISSUES.md #94) is not ported, so this is a simple arcade model that follows the world's road
// surface. Car space is +X forward, +Y left, +Z up (same as the world).
class PlayerCar {
public:
    // Assemble the stock body (KIT00 + BASE, highest LOD) from CARS\<name>\GEOMETRY.BIN.
    bool LoadModel(const std::vector<uint8_t> &geometry);
    const std::vector<SolidMesh::Triangle> &Model() const { return m_model; }

    void Place(float x, float y, float z, float yaw);
    // throttle/brake/steer in [-1, 1]; dt in seconds.
    void Update(const TrackWorld &world, float throttle, float brake, float steer, float dt);

    // Row-major 3x4 car-to-world transform: rows are the forward, left, up axes; column 3 the position.
    void Transform(float m[12]) const;
    float X() const { return m_x; }
    float Y() const { return m_y; }
    float Z() const { return m_z; }
    float Yaw() const { return m_yaw; }
    float Speed() const { return m_speed; }

private:
    std::vector<SolidMesh::Triangle> m_model;
    float m_x = 0, m_y = 0, m_z = 0, m_yaw = 0;
    float m_pitch = 0, m_roll = 0;
    float m_speed = 0; // m/s along the heading
    float m_vz = 0;    // vertical speed while airborne
};
