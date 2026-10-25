#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <algorithm>
#include <vector>

#include <SDL.h>

#include "game/indep/World/SolidMesh.h"
#include "game/indep/World/CarSkin.h"

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
        float fov = 70.0f;         // horizontal field of view in degrees (the pre-race cameras change it, ISSUES #265)
    };

    // Parse a STREAM<region>.BUN image. Returns false if no scenery was found. neighbours: other
    // regions' streams, used only for solids the region's scenery names but does not contain
    // (ISSUES.md #121); they are not kept after loading.
    // A texture pack (0xb3300000) joined to every following Load: the location's dynamic textures
    // (TRACKS\\LOC<n>DYNTEX.BIN), which the region streams do not carry (ISSUES #254). Must be set before Load,
    // because triangles resolve their texture at load time.
    void SetExtraPack(std::vector<uint8_t> data) { m_extraPacks.clear(); AddExtraPack(std::move(data)); }
    void AddExtraPack(std::vector<uint8_t> data) { if (!data.empty()) m_extraPacks.push_back(std::move(data)); }
    // The showroom stage (ISSUES #304): the menus' 3D car on a dark studio floor with a reflection, drawn through the same car shader as the
    // race.  The car is placed at the origin facing +X on z = 0 (use AddCar / SetCarTransform / ClearCars as for the world).
    struct StageParams {
        float orbit = 0.55f;    // camera azimuth around the car (rad, 0 = in front of the car)
        float distance = 6.6f;  // metres
        float height = 1.25f;   // camera height
        float lookZ = 0.55f;    // the point looked at
        float fov = 34.0f;      // horizontal degrees at 4:3 (Hor+ above)
        float shiftX = 0.0f;    // shifts the car sideways on screen (fraction of the half width; + = to the right)
        float time = 0.0f;      // seconds, drives the background animation
        float accent[3] = {0.10f, 0.45f, 1.00f};
        float accent2[3] = {1.00f, 0.50f, 0.12f};
        // overlay mode (ISSUES #309): only the car, drawn into a window rectangle (pixels, y down) over whatever is already on screen, lit by the
        // studio environment, with no backdrop, floor, reflection or post-processing
        bool overlay = false;
        int vx = 0, vy = 0, vw = 0, vh = 0;
    };
    // Writes the procedural old Israeli apartment block (ISSUES #306) as a coloured .obj / .ply pair (vertex colours), for use outside the game.
    static bool ExportShikun(const char *basePath, int floors, float width, float depth, unsigned seed);
    bool RenderStage(SDL_Renderer *renderer, int width, int height, const StageParams &p);
    // Progress reports (0..1) from Load, for the loading screen (ISSUES #301).
    void SetProgress(std::function<void(float)> cb) { m_progress = std::move(cb); }
    bool Load(const std::vector<uint8_t> &stream, const std::vector<const std::vector<uint8_t> *> &neighbours = {});
    // Replace the world with a standalone scene file (FRONTEND\\PLATFORMS\\*.BIN: an 0x80134000 solid list plus
    // its own 0xb3300000 texture pack, e.g. the career garage CRIB01.BIN, ISSUES #251). The solids keep their
    // 0x134011 header transforms; there is no collision mesh. Returns false when no solid decoded.
    bool LoadScene(const std::vector<uint8_t> &file);
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
    // inset (optional): draw into a sub-rectangle of the window (pixels, origin top-left), optionally mirrored
    // left-right; used for the rear-view mirror (ISSUES #245).
    struct Inset {
        int x, y, w, h;
        bool mirror;
    };
    bool RenderGL(SDL_Renderer *renderer, const Camera &cam, int width, int height, const Inset *inset = nullptr);
    void DrawCarLights(const Camera &cam, bool day) const;

    // Height of the highest walkable surface (normal mostly up) at (x, y) that is not above zMax.
    bool GroundHeight(float x, float y, float zMax, float &z, float *normal = nullptr) const;
    // A bare plane at height z with no walls: the physics benchmark's test strip (NFSU2_DYNO_BENCH, ISSUES #293).
    void SetFlatGround(float z) { m_flat = true, m_flatZ = z; }
    // Career hot-spot markers (ISSUES #269): translucent light columns standing on the ground at (x, y).
    struct Beacon {
        float x = 0, y = 0, radius = 5, z = 0;
        uint8_t r = 255, g = 255, b = 255;
    };
    void SetBeacons(const std::vector<Beacon> &beacons);
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
                  SDL_Color spoilerPaint = {60, 110, 210, 255},
                  const std::vector<uint8_t> *roofs = nullptr,
                  SDL_Color roofPaint = {60, 110, 210, 255},
                  const std::vector<uint8_t> *engines = nullptr,
                  SDL_Color enginePaint = {128, 64, 48, 255},
                  const std::vector<uint8_t> *exhausts = nullptr,
                  SDL_Color exhaustPaint = {150, 150, 150, 255});
    // Puts a composed 512 x 512 skin (CarSkin, R in the low byte) on the painted body slot of car
    // `index`; an empty vector removes it.  The car's triangles of that slot are then textured by the
    // planar side projection.
    void SetCarSkin(size_t index, uint32_t bodySlot, std::vector<uint32_t> rgba);
    // Car texture packs (CARS\<name>\TEXTURES.BIN, CARS\TEXTURES.BIN): their textures join the
    // world's by name hash.
    void AddTexturePack(std::vector<uint8_t> data);
    void SetCarTransform(size_t car, const float m[12]);
    // Lamp state (ISSUES #284): headlights on, brake pedal down, reverse gear. The lamps fade towards the target a little
    // every call (once per frame), so a tap on the brake does not flicker.
    // Tyre smoke and skid marks (ISSUES #294). EmitSmoke adds a puff (an emission rate is the caller's business); AddSkid appends a mark
    // segment between two contact points; UpdateEffects ages them (call once per frame with the frame time).
    void EmitSmoke(float x, float y, float z, float vx, float vy, float intensity)
    {
        if (m_smoke.size() >= 700)
            m_smoke.erase(m_smoke.begin(), m_smoke.begin() + 100);
        Smoke s;
        s.x = x, s.y = y, s.z = z + 0.15f, s.vx = vx * 0.25f, s.vy = vy * 0.25f, s.vz = 0.5f + 0.7f * intensity;
        s.size = 0.45f + 0.35f * intensity, s.life = 0.9f + 0.7f * intensity, s.a = 0.10f + 0.14f * intensity;
        m_smoke.push_back(s);
    }
    void AddSkid(float x0, float y0, float z0, float x1, float y1, float z1, float width, float strength)
    {
        Skid k{x0, y0, z0 + 0.03f, x1, y1, z1 + 0.03f, width, strength, SDL_GetTicks()};
        if (m_skids.size() < 8000)
            m_skids.push_back(k);
        else
            m_skids[m_skidNext++ % m_skids.size()] = k;
    }
    void UpdateEffects(float dt)
    {
        for (Smoke &s : m_smoke) {
            s.age += dt;
            s.x += s.vx * dt, s.y += s.vy * dt, s.z += s.vz * dt;
            s.vx *= 1.0f - 1.2f * dt, s.vy *= 1.0f - 1.2f * dt, s.vz *= 1.0f - 0.9f * dt;
        }
        m_smoke.erase(std::remove_if(m_smoke.begin(), m_smoke.end(), [](const Smoke &s) { return s.age >= s.life; }), m_smoke.end());
    }
    void DrawTyreEffects(const Camera &cam) const;
    // The speed effect (ISSUES #298): radial blur strength 0..1 (from the speed), a nitrous burst 0..1, and where the car is on screen
    // (uv, y up) so it can stay sharp; carRx = 0 for the rigid cameras where the car is not visible.
    void SetSpeedFx(float motion, float nitro, float carX, float carY, float carRx, float carRy)
    {
        m_fxMotion = motion, m_fxNitro = nitro, m_fxCar[0] = carX, m_fxCar[1] = carY, m_fxCar[2] = carRx, m_fxCar[3] = carRy;
    }
    void SetCarLamps(size_t car, bool head, bool brake, bool reverse)
    {
        if (car >= m_cars.size())
            return;
        Car &c = m_cars[car];
        auto fade = [](float &v, bool on) { v += ((on ? 1.0f : 0.0f) - v) * 0.4f; };
        fade(c.lampHead, head), fade(c.lampBrake, brake), fade(c.lampReverse, reverse);
    }
    // Wheel spin and steering (ISSUES #257 / #258): the per-triangle wheel and brake index of the car's model (-1 none; the
    // vectors must outlive the car), the four hubs' car-space (x, y, z) as 12 floats, the rolling angle and the front
    // wheels' steering angle, applied by the next SetCarTransform.
    void SetCarWheels(size_t car, const std::vector<int8_t> *wheelOf, const std::vector<int8_t> *brakeOf, const float *hubs,
                      float spin, float steer, float trueRate = 1.0e9f)
    {
        if (car < m_cars.size() && hubs) {
            Car &c = m_cars[car];
            c.wheelOf = wheelOf, c.brakeOf = brakeOf;
            for (int i = 0; i < 12; ++i)
                c.hub[i] = hubs[i];
            const unsigned now = SDL_GetTicks();
            if (c.prevTicks && now > c.prevTicks) { // angular speed from the angle change (the angle wraps at 2 pi)
                float d = spin - c.prevSpin;
                if (d > 3.1415927f) d -= 6.2831853f;
                if (d < -3.1415927f) d += 6.2831853f;
                const float rate = d / (float(now - c.prevTicks) * 0.001f);
                c.spinRate += ((trueRate < 1.0e8f ? trueRate : rate) - c.spinRate) * 0.25f; // the shown angle may be capped
            }
            c.prevSpin = spin, c.prevTicks = now;
            c.spin = spin, c.steer = steer;
        }
    }

    // Race barriers (XO_TrackBarrier* scenery) are kept out of the world until a race enables the
    // ones it needs (UG2 selects barrier groups per race; see ISSUES.md #107). keep(bbox[6]).
    void EnableBarriers(const std::function<bool(const float *bbox)> &keep);

    // Number of collision-mesh faces loaded (0: physics falls back to the render triangles).
    size_t CollisionFaces() const { return m_collisionFaces; }
    // Dev aid: 0 = both (default), 1 = render triangles only, 2 = collision mesh only.
    void SetGroundSource(int source) { m_groundSource = source; }
    // The road surface triangles of the loaded region (x0 y0 x1 y1 x2 y2 per triangle), for the minimap (ISSUES #259).
    const std::vector<float> &RoadTriangles() const { return m_roadTris; }
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
public:
    using RTriPublic = RTri; // IsraelProps.cpp's mesh builder
    // A solid vertical cylinder (a tree trunk, a pole, a bus shelter post): the wall query treats it as a wall (ISSUES #275).
    struct PropSolid {
        float x, y, r, z0, z1;
    };
private:
    std::vector<PropSolid> m_propSolids;
    std::unordered_map<int64_t, std::vector<uint32_t>> m_propGrid; // 16 m cell -> indices into m_propSolids
    bool PropWall(const float *a, const float *b, float *normal) const;
    void AddIsraeliProps(); // ISSUES #273 (IsraelProps.cpp)
    bool BauhausReplace(const char *name, const std::vector<SolidMesh::Triangle> &tris, const float *pos, const float *rot); // ISSUES #279: true = the instance is replaced / hidden
    void FlushBauhaus();
    std::vector<RTri> m_bauhaus;
    std::unordered_set<int64_t> m_bauhausDone;
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
    std::function<void(float)> m_progress;
    void Progress(float f) const { if (m_progress) m_progress(f); }
    bool CollisionGround(float x, float y, float zq, float &z, float *normal) const;
    size_t m_collisionFaces = 0;
    int m_groundSource = 0;
    Camera m_start;
    size_t m_triangleCount = 0;
    std::vector<std::vector<uint8_t>> m_extraPacks;
    std::vector<float> m_roadTris;
    bool m_flat = false;
    float m_flatZ = 0;
    std::vector<float> m_roadTriZ; // one centroid height per road triangle (IsraelProps pavement test)
    bool m_scene = false; // LoadScene: one cell, always drawn
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
        const std::vector<uint8_t> *roofs = nullptr; // 1 = installed roof scoop (#178)
        SDL_Color roofPaint = {60, 110, 210, 255};
        const std::vector<uint8_t> *engines = nullptr; // 1 = installed engine accent (#178)
        SDL_Color enginePaint = {128, 64, 48, 255};
        const std::vector<uint8_t> *exhausts = nullptr; // 1 = installed muffler tip (#178)
        SDL_Color exhaustPaint = {150, 150, 150, 255};
        uint32_t skinSlot = 0, skinHash = 0; // vinyl skin on the painted body (SetCarSkin, #223)
        CarSkin::Projection skinProj;
        std::vector<size_t> order;  // model triangles grouped by texture
        std::vector<Tri> tris;      // model in world space, rebuilt by SetCarTransform
        std::vector<RTri> render;   // same, textured and lit, in `order`
        // Per-vertex lighting data parallel to `render` (ISSUES #253): world-space smooth normals (9 floats per
        // triangle), the unlit colour (3 floats) and the gloss class (0 none, 1 paint, 2 glass). RenderGL lights
        // glossy triangles with the camera known.
        std::vector<float> modelNormals; // car space, 9 floats per model triangle (built once)
        std::vector<float> modelAO;               // per model vertex, 3 per triangle (built with modelNormals)
        std::vector<float> normals, albedo, ao; // ao: per vertex ambient occlusion (wheels: depth inside the rim), 3 per triangle
        bool normalsHadWheels = false;           // modelNormals were built with the wheel hubs known (#264)
        float prevSpin = 0, spinRate = 0;        // rad/s, smoothed: the wheel shader blurs the spokes with it
        unsigned prevTicks = 0;
        float xf[12] = {};                 // the car-to-world transform of the last SetCarTransform (row-major 3x4)
        const std::vector<int8_t> *wheelOf = nullptr, *brakeOf = nullptr; // SetCarWheels
        float hub[12] = {}, spin = 0, steer = 0;
        float halfLen = 2.2f, halfWid = 1.0f; // model extent along the car's x (forward) and y (left), for the shadow
        float shadowGrid[7][5] = {};       // smoothed ground heights under the shadow (0 = unset)
        bool shadowGridSet = false;
        float shadowZ = 1e9f;               // low-passed ground height under the car (shadow fallback, no flicker)
        float groundZ = 0.0f;              // lowest point of the car in the world (the shadow's height)
        std::vector<uint8_t> gloss;
        // Lamps (#284): fade state (0..1) and the world-space centres of the lens triangles (gloss 6 head, 7 tail), left
        // (car +y) and right, rebuilt by SetCarTransform with the forward axis for the glows and beams.
        float lampHead = 0, lampBrake = 0, lampReverse = 0;
        float headPos[2][3] = {}, tailPos[2][3] = {}, fwd[3] = {1, 0, 0};
        bool haveHead[2] = {}, haveTail[2] = {};
    };
    // Car material slot -> texture (0 = untextured) and base colour.
    uint32_t CarMaterial(uint32_t hash, SDL_Color paint, float rgb[3]) const;
    std::vector<Car> m_cars;
    std::vector<Beacon> m_beacons;
    struct Smoke {
        float x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0, age = 0, life = 1, size = 1, a = 0.3f;
    };
    struct Skid {
        float x0, y0, z0, x1, y1, z1, width, strength;
        unsigned born;
    };
    std::vector<Smoke> m_smoke;
    std::vector<Skid> m_skids;
    size_t m_skidNext = 0;
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
        int interior = 0; // 1 = window facade (interior mapping), 2 = curtain-wall glass (the whole texture is glass)
    };
    struct GLCell {
        unsigned buffer = 0;
        int vertices = 0;
        std::vector<GLRange> ranges; // per texture
    };
    // Light-beam textures the packs do not carry (spotlights / light poles / bridge lights name 0x2e95ce7c and
    // 0x2e95ce7d): drawn with a procedural additive gradient (ISSUES #254).
    static bool IsBeamTexture(uint32_t hash) { return hash == 0x2e95ce7cu || hash == 0x2e95ce7du; }
    // Shader programs and the post-processing targets (ISSUES #263; TrackWorldGL.cpp).
    unsigned m_progInterior = 0;
    std::unordered_map<uint32_t, int> m_interiorClass; // texture hash -> 0 none / 1 windowed facade / 2 glass wall
    int InteriorClassOf(uint32_t hash);
    unsigned m_progWorldLite = 0; // the cheap world shader for far cells (ISSUES #282)
    unsigned m_progWorld = 0, m_progCar = 0, m_progBright = 0, m_progBlur = 0, m_progComposite = 0, m_progSky = 0, m_progStageBg = 0;
    unsigned m_postDepth = 0, m_postScene = 0, m_postBloom[2] = {0, 0}, m_postFboScene = 0, m_postFbo[2] = {0, 0};
    int m_postW = 0, m_postH = 0;
    bool m_shadersTried = false;
    void InitShaders();
    void PostProcess(int w, int h);
    float m_sunUV[2] = {0.5f, 2.0f}, m_raysK = 0.0f; // the sun on screen (uv) and the god-ray / lens-reflection strength (ISSUES #305)
    float m_stageEnv = 0.0f, m_stageA[3] = {}, m_stageB[3] = {}; // the car shader's studio environment (RenderStage)
    void DrawCarsGL(const Camera &cam, const float fogColour[4], bool day, float kFogStart, float kDrawDistance, bool reflect);
    float m_fxMotion = 0, m_fxNitro = 0, m_fxCar[4] = {0, 0, 0, 0}, m_fxCenter[2] = {0.5f, 0.6f};
    void ReleasePost();
    unsigned m_glShadow = 0; // soft contact-shadow texture (procedural)
    unsigned GLTexture(uint32_t hash); // decode + upload on first use (TrackWorldGL.cpp)
    std::vector<uint32_t> m_glTextureStale; // skin hashes whose GL texture must be rebuilt
    std::map<uint32_t, std::vector<uint32_t>> m_skinPixels; // composed 512 x 512 skins by texture hash
    void ReleaseGL();
    std::unordered_map<uint32_t, unsigned> m_glTextures;
    std::unordered_map<unsigned, int> m_glTextureMode; // GL texture -> GLRange::mode
    unsigned m_glWhite = 0;
    std::unordered_map<int64_t, GLCell> m_glCells;
    // One vertex arena for every cell (ISSUES #282): cells are sub-allocated lazily, drawn with batched multi-draws under one VAO.
    unsigned m_arena = 0, m_arenaVao = 0;
    size_t m_arenaCap = 0, m_arenaUsed = 0; // in vertices
    bool m_warmAll = true;                   // the next RenderGL uploads every cell once (no streaming hitches while driving)
    GLCell m_glPanorama;
    int m_glState = 0; // 0 unknown, 1 available, -1 unavailable
    // Texture packs (0xb3300000) copied out of the stream; textures resolved by name hash.
    std::vector<std::unique_ptr<std::vector<uint8_t>>> m_tpkData;
    std::vector<std::unique_ptr<class TexturePack>> m_packs;
    std::unordered_map<uint32_t, std::pair<const class TexturePack *, size_t>> m_textures;
};
