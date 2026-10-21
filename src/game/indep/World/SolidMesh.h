#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// PS2 eSolid mesh decoding: positions are Q15 values between per-strip bounds (VU addresses 2/3),
// in V4-16 SoA groups from VU address 5. Triangle topology is approximated (ISSUES.md #90).
namespace SolidMesh {
struct Point {
    float x, y, z;
    float u = 0, v = 0;                         // VU 0x1d.. (two vertices per V4-16 qword), 1.0 = 4096
    uint8_t r = 128, g = 128, b = 128, a = 128; // pre-lit colour, VU 0x2d.. (V4-8 unsigned, 128 = 1.0)
};

struct Triangle {
    Point v[3];
    int8_t tex = -1; // strip TextureNumber: index into the solid's 0x134012 (hash, 0) pairs
};

// One strip entry's VIF block -> its NumVerts positions.
// seeds (optional): the four 32-bit words unpacked to VU address 1 (strip restart seeds, see
// StripRestartFlags; world solids keep the 1/256 m position step in word 1).
bool DecodeStrip(const uint8_t *d, size_t dataSize, size_t start, size_t size, unsigned nverts,
                 std::vector<Point> &out, uint32_t *seeds = nullptr);
// Strip restarts as the mesh VU program sets them (ISSUES.md #110): per vertex, 1 = the GS ADC
// ("no kick") bit, i.e. that vertex does not complete a triangle.
void StripRestartFlags(const uint32_t seeds[4], unsigned nverts, std::vector<uint8_t> &noKick);
// NumVerts strip vertices -> NumPolys triangles (seam bridges dropped by longest edge).
void BuildStripTriangles(const std::vector<Point> &p, unsigned nverts, unsigned numpoly,
                         std::vector<Triangle> &out, int tex = -1);
// One 0x80134100 mesh chunk body.
void DecodeMesh(const uint8_t *d, size_t dataSize, size_t body, size_t size, std::vector<Triangle> &out);
// Every 0x80134100 mesh under [begin, end), descending into container chunks.
void DecodeChunks(const uint8_t *d, size_t dataSize, size_t begin, size_t end, std::vector<Triangle> &out);
std::vector<Triangle> DecodeGeometry(const uint8_t *d, size_t size);
// Strips decoded with exact VU restart bits / with the fallback approximation (diagnostics).
extern unsigned long g_exactStrips, g_approxStrips;
} // namespace SolidMesh

namespace SolidMesh {
// One 0x13401a position marker: a name hash and a row-vector 4x4 matrix in car space.  The game
// keeps these on the body (ROOF_SCOOP, SPOILER) and on the bumpers (LEFT/RIGHT_EXHAUST) and uses
// them to place parts and effects (ISSUES.md #178).
struct PositionMarker {
    uint32_t hash = 0;
    float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};
// One 0x80134010 solid: its (possibly truncated, 27-char) name from the 0x134011 header and its
// triangles in solid space.
struct NamedSolid {
    std::string name;
    uint32_t hash = 0;
    float bboxMin[3] = {}, bboxMax[3] = {};
    // 0x134011 header +0x40: column-major local-to-car matrix (translation in [12..14]).  Most
    // body solids are identity; detached parts such as the stock spoiler carry their mount here.
    float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::vector<PositionMarker> markers; // 0x13401a records (0x50 bytes each)
    std::vector<Triangle> tris;
    std::vector<uint32_t> textures; // 0x134012 texture name hashes, indexed by Triangle::tex
    const PositionMarker *FindMarker(uint32_t markerHash) const
    {
        for (const PositionMarker &m : markers)
            if (m.hash == markerHash)
                return &m;
        return nullptr;
    }
};
// Body-panel styles present in a car's GEOMETRY.BIN (ISSUES.md #192).  The GLOBALB catalogue
// lists every body-kit design for every car, but a car ships only the panels it can mount: the
// SKYLINE has 25 of the 30 kits and the COROLLA only 10 of the 14 headlight/taillight styles.
// A missing style must be hidden from the shop and must never replace the stock panel.
struct BodyStyleSet {
    uint32_t frontBumper = 0; // part id 0x1: <CAR>_KIT<NN>_FRONT_BUMPER
    uint32_t rearBumper = 0;  // 0x2:       <CAR>_KIT<NN>_REAR_BUMPER
    uint32_t skirt = 0;       // 0xb:       <CAR>_KIT<NN>_SKIRT
    uint32_t hood = 0;        // 9:         <CAR>_STYLE<NN>_HOOD[_LEFT|_RIGHT]
    uint32_t headlight = 0;   // 0xe:       <CAR>_STYLE<NN>_HEADLIGHT
    uint32_t taillight = 0;   // 0xf:       <CAR>_STYLE<NN>_BRAKELIGHT
    uint32_t wideBody = 0;    // 6:         <CAR>_KITW<NN>_BODY_A..D
    bool Has(int partId, int style) const;
};
// Apply a row-vector matrix (translation in [12..14]) to a point.
Point TransformPoint(const float matrix[16], const Point &p);
// Apply NamedSolid::transform to a solid-space point.
Point TransformPoint(const NamedSolid &solid, const Point &p);
std::vector<NamedSolid> DecodeNamedSolids(const uint8_t *d, size_t size);
BodyStyleSet FindBodyStyles(const std::vector<NamedSolid> &solids);
} // namespace SolidMesh
