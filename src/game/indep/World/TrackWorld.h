#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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

    // Parse a STREAM<region>.BUN image. Returns false if no scenery was found. neighbours: other
    // regions' streams, used only for solids the region's scenery names but does not contain
    // (ISSUES.md #121); they are not kept after loading.
    bool Load(const std::vector<uint8_t> &stream, const std::vector<const std::vector<uint8_t> *> &neighbours = {});
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
    // Hardware path (TrackWorldGL.cpp): draws the world with OpenGL and a depth buffer between
    // SDL_Renderer batches. Returns false when the renderer is not SDL's OpenGL backend.
    bool RenderGL(SDL_Renderer *renderer, const Camera &cam, int width, int height);

    // Height of the highest walkable surface (normal mostly up) at (x, y) that is not above zMax.
    bool GroundHeight(float x, float y, float zMax, float &z, float *normal = nullptr) const;
    // Does the segment a->b cross a steep (wall) triangle? If hit, the surface normal (facing the
    // segment start) is written to normal when non-null.
    bool HitsWall(float ax, float ay, float az, float bx, float by, float bz, float *normal = nullptr) const;

    // Cars drawn with the world: car-space triangles (kept by pointer, must outlive the world's
    // use), body colour, and the row-major 3x4 car-to-world transform from PlayerCar::Transform.
    void ClearCars();
    size_t AddCar(const std::vector<SolidMesh::Triangle> *model, const std::vector<uint32_t> *textures,
                  SDL_Color paint, const std::vector<uint8_t> *rims = nullptr,
                  SDL_Color rimPaint = {38, 38, 38, 255},
                  const std::vector<uint8_t> *calipers = nullptr,
                  SDL_Color caliperPaint = {128, 128, 128, 255},
                  const std::vector<uint8_t> *mirrors = nullptr,
                  SDL_Color mirrorPaint = {60, 110, 210, 255},
                  const std::vector<uint8_t> *spoilers = nullptr,
                  SDL_Color spoilerPaint = {60, 110, 210, 255});
    // Car texture packs (CARS\<name>\TEXTURES.BIN, CARS\TEXTURES.BIN): their textures join the
    // world's by name hash.
    void AddTexturePack(std::vector<uint8_t> data);
    void SetCarTransform(size_t car, const float m[12]);

    // Race barriers (XO_TrackBarrier* scenery) are kept out of the world until a race enables the
    // ones it needs (UG2 selects barrier groups per race; see ISSUES.md #107). keep(bbox[6]).
    void EnableBarriers(const std::function<bool(const float *bbox)> &keep);

    // Number of collision-mesh faces loaded (0: physics falls back to the render triangles).
    size_t CollisionFaces() const { return m_collisionFaces; }
    // Dev aid: 0 = both (default), 1 = render triangles only, 2 = collision mesh only.
    void SetGroundSource(int source) { m_groundSource = source; }
    size_t TriangleCount() const { return m_triangleCount; }
    size_t InstanceCount() const { return m_instances; }

private:
    struct Tri {
        float v[9];
        uint8_t shade[3]; // base colour from the surface normal and the solid kind
        uint8_t ground;   // normal within ~37 deg of vertical: drawn before cars
        uint8_t phys = 1; // used by GroundHeight (0 once the collision mesh replaces the scenery)
        uint8_t barrier = 0; // an enabled race barrier: the only render geometry HitsWall still tests (#146)
    };
    // UG2's collision mesh (stream chunk 0x34134, 32-byte faces; ISSUES.md #133).
    struct CTri {
        float v[9];
        uint8_t surface, flags;
    };
    // The collision mesh as the game queries it (0x00336180, ISSUES #145): per scenery section a grid of
    // 32 m cells (0x34137: {u16 first, u16 count} into 0x34136: {u16 face, u16 8 m sub-cell mask}).
    struct CFace {
        float v[12];            // 4 vertices (a triangle repeats nothing: only 3 are used)
        float n[3], d;          // unit normal of (v1-v0)x(v2-v0) and n.v0
        uint8_t group, surface, flags;
        bool quad;
    };
    struct CSection {
        float ox, oy;
        int cols, rows;
        std::vector<CFace> faces;
        std::vector<uint32_t> index; // face | mask << 16
        std::vector<uint32_t> cells; // first | count << 16
    };
    // Render data (textured): world-space vertices with UV and pre-lit colour, and the texture
    // name hash (0 = untextured, drawn in the flat shade colour).
    struct RVertex {
        float x, y, z, u, v;
        uint8_t r, g, b, a;
    };
    struct RTri {
        RVertex v[3];
        uint32_t tex;
    };
    struct Cell {
        std::vector<Tri> tris;    // positions for physics and the software renderer
        std::vector<RTri> render; // consumed by the GL upload
    };
    static constexpr float kCellSize = 64.0f;
    static int64_t CellKey(int cx, int cy) { return (int64_t(cx) << 32) ^ uint32_t(cy); }
    // Fills t.shade from the triangle normal; false for a degenerate triangle.
    static bool Shade(Tri &t, float r, float g, float b);
    template <typename F> void ForNearbyCollision(float x, float y, F &&f) const;
    void AddToCells(const Tri &w, const RTri &r, bool physics, bool barrier = false);
    template <typename F> void ForNearbyTris(float x, float y, F &&f) const;

    std::unordered_map<int64_t, Cell> m_cells;
    std::unordered_map<int64_t, std::vector<CTri>> m_collision;
    std::vector<CSection> m_csections;
    std::vector<std::string> m_groupNames;
    std::vector<uint8_t> m_groupOn;
    std::unordered_map<int64_t, std::vector<uint32_t>> m_csectionGrid; // 64 m cell -> sections
    // The game's ground query over the collision mesh: the highest upward floor under (x, y) that is
    // not above zq, found only in the faces the world has enabled (group 0, not a wall).
public:
    // Wall faces (flags & 0x02) of the collision mesh crossed by the segment (0x00337888 collects them).
    // onlyGroup >= 0 tests just that barrier group, enabled or not (dev aid for reading the groups).
    bool CollisionWall(const float *a, const float *b, float *normal, int onlyGroup = -1) const;
    // The barrier-group table (chunk 0x3412f of the region bundle TRACKS\<region>.BUN, loader
    // 0x00338078 keeps its payload at DAT_00523ddc: 0x20-byte records, +8 = enabled byte, +9 = name
    // "BARRIERS_<race>"). A collision face's group byte indexes it; group 0 is always on.
    void DumpWalls(float x, float y, float r) const; // dev aid
    void LoadBarrierGroups(const std::vector<uint8_t> &regionBundle);
    size_t BarrierGroupCount() const { return m_groupNames.size(); }
    const std::string &BarrierGroupName(size_t g) const { return m_groupNames[g]; }
    void SetBarrierGroupEnabled(size_t g, bool on) { if (g < m_groupOn.size()) m_groupOn[g] = on; }
    bool GroupEnabled(uint8_t g) const { return g == 0 || (g < m_groupOn.size() && m_groupOn[g]); }
private:
    bool CollisionGround(float x, float y, float zq, float &z, float *normal) const;
    size_t m_collisionFaces = 0;
    int m_groundSource = 0;
    Camera m_start;
    size_t m_triangleCount = 0;
    size_t m_instances = 0;
    size_t m_borrowedSolids = 0;
    std::vector<SDL_Vertex> m_frame; // per-frame vertex buffer
    struct Car {
        const std::vector<SolidMesh::Triangle> *model;
        const std::vector<uint32_t> *textures; // per model triangle
        SDL_Color paint;
        const std::vector<uint8_t> *rims = nullptr; // 1 = painted rim face (#172)
        SDL_Color rimPaint = {38, 38, 38, 255};
        const std::vector<uint8_t> *calipers = nullptr; // 1 = painted brake caliper (#173)
        SDL_Color caliperPaint = {128, 128, 128, 255};
        const std::vector<uint8_t> *mirrors = nullptr; // 1 = painted mirror housing (#174)
        SDL_Color mirrorPaint = {60, 110, 210, 255};
        const std::vector<uint8_t> *spoilers = nullptr; // 1 = stock spoiler (#175)
        SDL_Color spoilerPaint = {60, 110, 210, 255};
        std::vector<size_t> order;  // model triangles grouped by texture
        std::vector<Tri> tris;      // model in world space, rebuilt by SetCarTransform
        std::vector<RTri> render;   // same, textured and lit, in `order`
    };
    // Car material slot -> texture (0 = untextured) and base colour.
    uint32_t CarMaterial(uint32_t hash, SDL_Color paint, float rgb[3]) const;
    std::vector<Car> m_cars;
    struct Barrier {
        float bbox[6];
        std::vector<Tri> tris;
        std::vector<RTri> render;
    };
    std::vector<Barrier> m_barriers;
    // PAN_* panorama instances: drawn first as a far backdrop without fog (GL path only).
    std::vector<RTri> m_panorama;
    unsigned m_panoramaBuffer = 0;
    int m_panoramaVertices = 0;
    // OpenGL vertex buffers per cell, built on first use (TrackWorldGL.cpp).
    struct GLRange {
        unsigned texture; // GL texture name (a white 1x1 for untextured triangles)
        int first, count;
        int mode;         // 0 opaque / punch-through, 1 alpha blend, 2 additive
    };
    struct GLCell {
        unsigned buffer = 0;
        int vertices = 0;
        std::vector<GLRange> ranges; // per texture
    };
    unsigned GLTexture(uint32_t hash); // decode + upload on first use (TrackWorldGL.cpp)
    void ReleaseGL();
    std::unordered_map<uint32_t, unsigned> m_glTextures;
    std::unordered_map<unsigned, int> m_glTextureMode; // GL texture -> GLRange::mode
    unsigned m_glWhite = 0;
    std::unordered_map<int64_t, GLCell> m_glCells;
    GLCell m_glPanorama;
    int m_glState = 0; // 0 unknown, 1 available, -1 unavailable
    // Texture packs (0xb3300000) copied out of the stream; textures resolved by name hash.
    std::vector<std::unique_ptr<std::vector<uint8_t>>> m_tpkData;
    std::vector<std::unique_ptr<class TexturePack>> m_packs;
    std::unordered_map<uint32_t, std::pair<const class TexturePack *, size_t>> m_textures;
};
