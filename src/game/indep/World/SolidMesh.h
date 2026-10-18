#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// PS2 eSolid mesh decoding: positions are Q15 values between per-strip bounds (VU addresses 2/3),
// in V4-16 SoA groups from VU address 5. Triangle topology is approximated (ISSUES.md #90).
namespace SolidMesh {
struct Point {
    float x, y, z;
};

struct Triangle {
    Point v[3];
};

// One strip entry's VIF block -> its NumVerts positions.
bool DecodeStrip(const uint8_t *d, size_t dataSize, size_t start, size_t size, unsigned nverts,
                 std::vector<Point> &out);
// NumVerts strip vertices -> NumPolys triangles (seam bridges dropped by longest edge).
void BuildStripTriangles(const std::vector<Point> &p, unsigned nverts, unsigned numpoly,
                         std::vector<Triangle> &out);
// One 0x80134100 mesh chunk body.
void DecodeMesh(const uint8_t *d, size_t dataSize, size_t body, size_t size, std::vector<Triangle> &out);
// Every 0x80134100 mesh under [begin, end), descending into container chunks.
void DecodeChunks(const uint8_t *d, size_t dataSize, size_t begin, size_t end, std::vector<Triangle> &out);
std::vector<Triangle> DecodeGeometry(const uint8_t *d, size_t size);
} // namespace SolidMesh
