#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL.h>

#include "game/indep/World/SolidMesh.h"

// Native stand-in for UG2's in-game world: the RegionLoader (0x002ddbc0) streams
// TRACKS\STREAM<region>.BUN, whose 0x80034100 scenery sections place 0x80134000 solids
// (ISSUES.md #103). Here the whole stream is decoded up front into world-space triangles,
// bucketed on a ground grid, and drawn with a software perspective transform and painter's
// ordering of grid cells (the PS2 renderer and its VU mesh program are not ported).
class TrackWorld {
public:
    struct Camera {
        float x = 0, y = 0, z = 0; // world position (Z up, metres)
        float yaw = 0;             // radians, 0 = +X
        float pitch = -0.12f;      // radians, negative looks down
    };

    // Parse a STREAM<region>.BUN image. Returns false if no scenery was found.
    bool Load(const std::vector<uint8_t> &stream);
    // A drivable spot to start from: the centre of a road instance near the middle of the map.
    Camera StartCamera() const { return m_start; }
    // Race start grids from Tracks\Routes<region>\TrackPosMarkersAll.bin (chunk 0x34146, 48-byte
    // markers: name hash +8, float x/y/z +0x10, u16 heading +0x20 (65536 = 360 deg, 0 = +X),
    // u16 race number +0x24). Each race has an 8-car grid per direction under its own name hash,
    // plus single start/finish markers. Returns each grid's slots in file order.
    std::vector<std::vector<Camera>> GridsFromMarkers(const std::vector<uint8_t> &markers, int raceNumber) const;
    // The race's first single marker (start/finish line) position.
    bool LineFromMarkers(const std::vector<uint8_t> &markers, int raceNumber, float &x, float &y) const;
    void Render(SDL_Renderer *renderer, const Camera &cam, int width, int height);

    // Height of the highest walkable surface (normal mostly up) at (x, y) that is not above zMax.
    bool GroundHeight(float x, float y, float zMax, float &z) const;
    // Does the segment a->b cross a steep (wall) triangle?
    bool HitsWall(float ax, float ay, float az, float bx, float by, float bz) const;

    // Cars drawn with the world: car-space triangles (kept by pointer, must outlive the world's
    // use), body colour, and the row-major 3x4 car-to-world transform from PlayerCar::Transform.
    void ClearCars();
    size_t AddCar(const std::vector<SolidMesh::Triangle> *model, SDL_Color paint);
    void SetCarTransform(size_t car, const float m[12]);

    // Race barriers (XO_TrackBarrier* scenery) are kept out of the world until a race enables the
    // ones it needs (UG2 selects barrier groups per race; see ISSUES.md #107). keep(bbox[6]).
    void EnableBarriers(const std::function<bool(const float *bbox)> &keep);

    size_t TriangleCount() const { return m_triangleCount; }
    size_t InstanceCount() const { return m_instances; }

private:
    struct Tri {
        float v[9];
        uint8_t shade[3]; // base colour from the surface normal and the solid kind
        uint8_t ground;   // normal within ~37 deg of vertical: drawn before cars
    };
    struct Cell {
        std::vector<Tri> tris;
    };
    static constexpr float kCellSize = 64.0f;
    static int64_t CellKey(int cx, int cy) { return (int64_t(cx) << 32) ^ uint32_t(cy); }
    // Fills t.shade from the triangle normal; false for a degenerate triangle.
    static bool Shade(Tri &t, float r, float g, float b);
    template <typename F> void ForNearbyTris(float x, float y, F &&f) const;

    std::unordered_map<int64_t, Cell> m_cells;
    Camera m_start;
    size_t m_triangleCount = 0;
    size_t m_instances = 0;
    std::vector<SDL_Vertex> m_frame; // per-frame vertex buffer
    struct Car {
        const std::vector<SolidMesh::Triangle> *model;
        SDL_Color paint;
        std::vector<Tri> tris; // model in world space, rebuilt by SetCarTransform
    };
    std::vector<Car> m_cars;
    struct Barrier {
        float bbox[6];
        std::vector<Tri> tris;
    };
    std::vector<Barrier> m_barriers;
    void AddToCells(const Tri &t);
};
