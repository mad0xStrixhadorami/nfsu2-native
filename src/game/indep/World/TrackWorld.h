#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL.h>

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
    void Render(SDL_Renderer *renderer, const Camera &cam, int width, int height);

    size_t TriangleCount() const { return m_triangleCount; }
    size_t InstanceCount() const { return m_instances; }

private:
    struct Tri {
        float v[9];
        uint8_t shade[3]; // base colour from the surface normal and the solid kind
    };
    struct Cell {
        std::vector<Tri> tris;
    };
    static constexpr float kCellSize = 64.0f;
    static int64_t CellKey(int cx, int cy) { return (int64_t(cx) << 32) ^ uint32_t(cy); }

    std::unordered_map<int64_t, Cell> m_cells;
    Camera m_start;
    size_t m_triangleCount = 0;
    size_t m_instances = 0;
    std::vector<SDL_Vertex> m_frame; // per-frame vertex buffer
};
