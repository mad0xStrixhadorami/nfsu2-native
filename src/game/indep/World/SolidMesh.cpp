// PS2 eSolid mesh decoding (chunk 0x80134100: strip table 0x134a01 + VIF stream 0x134a02), shared by
// the car-select preview and the track world. See ISSUES.md #65 / #90 / #91.
#include "game/indep/World/SolidMesh.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cmath>
#include <cstring>

namespace SolidMesh {

static uint32_t ReadLE32(const uint8_t *d, size_t p)
{
    return uint32_t(d[p]) | (uint32_t(d[p + 1]) << 8) | (uint32_t(d[p + 2]) << 16) |
           (uint32_t(d[p + 3]) << 24);
}

// Decodes one strip entry's VIF block into its NumVerts Q15 positions (see tools/solid_mesh.py).
bool DecodeStrip(const uint8_t *d, size_t dataSize, size_t start, size_t size, unsigned nverts,
                       std::vector<Point> &out)
{
    struct Vec4s { int16_t v[4] = {}; };
    Vec4s positions[256];
    float bounds[2][3] = {};
    bool haveBounds[2] = {};
    float step = 0.0f; // VU address 1 .y: world solids 1/256 m per unit, car solids 0 (Q15 bounds)
    size_t p = start, end = std::min(dataSize, start + size);
    while (p + 4 <= end) {
        uint16_t imm = uint16_t(d[p]) | (uint16_t(d[p + 1]) << 8);
        unsigned num = d[p + 2];
        unsigned cmd = d[p + 3] & 0x7f;
        p += 4;
        if (cmd >= 0x60) {
            unsigned vn = (cmd >> 2) & 3, vl = cmd & 3;
            unsigned comps = vn + 1;
            unsigned bits = vl == 0 ? 32 : vl == 1 ? 16 : vl == 2 ? 8 : 5;
            size_t elem = bits == 5 ? 2 : bits * comps / 8;
            if (!elem || p + elem * num > end)
                break; // truncated final unpack: keep whatever decoded so far
            unsigned addr = imm & 0x3ff;
            if (bits == 32 && comps == 4 && addr == 1 && num > 0) {
                uint32_t word = ReadLE32(d, p + 4);
                memcpy(&step, &word, sizeof(word));
            } else if (bits == 32 && (comps == 3 || comps == 4) && addr >= 2 && addr <= 3 && num > 0) {
                for (unsigned k = 0; k < std::min(num, 2u); ++k) {
                    for (unsigned c = 0; c < 3; ++c) {
                        uint32_t word = ReadLE32(d, p + k * elem + c * 4);
                        memcpy(&bounds[addr - 2 + k][c], &word, sizeof(word));
                    }
                    haveBounds[addr - 2 + k] = true;
                }
            } else if (bits == 16 && comps == 4 && addr >= 5 && addr + num <= 5 + 192) {
                for (unsigned k = 0; k < num; ++k)
                    for (unsigned c = 0; c < 4; ++c) {
                        uint16_t word = uint16_t(d[p + k * elem + c * 2]) |
                                        (uint16_t(d[p + k * elem + c * 2 + 1]) << 8);
                        positions[addr + k - 5].v[c] = static_cast<int16_t>(word);
                    }
            } else if (bits == 16 && comps <= 3 && addr >= 5 && addr + num <= 256) {
                // VIF packs tail vertices as V1/V2-16 SoA or V3-16 AoS after full V4-16 groups.
                for (unsigned k = 0; k < num; ++k)
                    for (unsigned c = 0; c < comps; ++c) {
                        uint16_t word = uint16_t(d[p + k * elem + c * 2]) |
                                        (uint16_t(d[p + k * elem + c * 2 + 1]) << 8);
                        positions[addr + k - 5].v[c] = static_cast<int16_t>(word);
                    }
            }
            p += (elem * num + 3) & ~size_t(3);
        } else if (cmd == 0x20) {
            p += 4;
        } else if (cmd == 0x30 || cmd == 0x31) {
            p += 16;
        } else if (cmd == 0x14 || cmd == 0x15 || cmd == 0x17) {
            break;
        }
    }
    if (!haveBounds[0] || !haveBounds[1]) return false;
    // Raw quantized coordinates, in vertex order.
    std::vector<std::array<int, 3>> q(nverts);
    unsigned soaVertices = nverts & ~3u;
    for (unsigned i = 0; i < nverts; ++i) {
        if (i < soaVertices) {
            unsigned group = i / 4, lane = i & 3, base = group * 3;
            if (base + 2 >= 192) return false;
            for (unsigned a = 0; a < 3; ++a)
                q[i][a] = positions[base + a].v[lane];
        } else {
            // Tail vertices follow the full V4-16 groups at VU address 5+(soa/4)*3; positions[] is
            // indexed by (VU address - 5), so the first tail qword is index (soa/4)*3.
            unsigned tail = (soaVertices / 4) * 3;
            unsigned rem = nverts - soaVertices, lane = i - soaVertices;
            if (tail + (rem == 3 ? lane : 2) >= 256) return false;
            for (unsigned a = 0; a < 3; ++a) {
                unsigned qword = tail + (rem == 3 ? lane : a);
                unsigned component = rem == 1 ? 0 : (rem == 3 ? a : lane);
                q[i][a] = positions[qword].v[component];
            }
        }
    }
    if (step != 0.0f) {
        // World solids (ISSUES.md #104): fixed-point steps of `step` metres; the strip bounds at
        // VU address 2 are the exact minimum, reached by the smallest coordinate on each axis.
        int qmin[3] = {INT32_MAX, INT32_MAX, INT32_MAX};
        for (const auto &v : q)
            for (unsigned a = 0; a < 3; ++a)
                qmin[a] = std::min(qmin[a], v[a]);
        for (const auto &v : q)
            out.push_back({bounds[0][0] + (v[0] - qmin[0]) * step, bounds[0][1] + (v[1] - qmin[1]) * step,
                           bounds[0][2] + (v[2] - qmin[2]) * step});
    } else {
        // Car solids: Q15 fractions between the strip bounds.
        for (const auto &v : q) {
            Point pt;
            float *dst[3] = {&pt.x, &pt.y, &pt.z};
            for (unsigned a = 0; a < 3; ++a)
                *dst[a] = bounds[0][a] + (v[a] / 32768.0f) * (bounds[1][a] - bounds[0][a]);
            out.push_back(pt);
        }
    }
    return true;
}

// Topology approximation (ISSUES #65): a strip entry is one triangle strip, but NumPolys is
// always less than NumVerts-2 with an even gap, i.e. (NumVerts-NumPolys)/2 = run count R and 2
// "bridge" triangles are skipped at each internal run seam. The seams are not stored, so drop the
// (NumVerts-2)-NumPolys largest-longest-edge triangles as the bridges.
void BuildStripTriangles(const std::vector<Point> &p, unsigned nverts, unsigned numpoly,
                         std::vector<Triangle> &out)
{
    if (nverts < 3 || p.size() != nverts)
        return;
    struct Tri {
        float longest;
        unsigned a, b, c;
    };
    std::vector<Tri> tris;
    tris.reserve(nverts - 2);
    auto edge = [&p](unsigned i, unsigned j) {
        float dx = p[i].x - p[j].x, dy = p[i].y - p[j].y, dz = p[i].z - p[j].z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    for (unsigned k = 0; k + 2 < nverts; ++k) {
        unsigned a = k % 2 == 0 ? k : k + 1, b = k % 2 == 0 ? k + 1 : k, c = k + 2;
        tris.push_back({std::max(edge(a, b), std::max(edge(b, c), edge(a, c))), a, b, c});
    }
    unsigned keep = numpoly < tris.size() ? numpoly : static_cast<unsigned>(tris.size());
    std::nth_element(tris.begin(), tris.begin() + keep, tris.end(),
                     [](const Tri &l, const Tri &r) { return l.longest < r.longest; });
    for (unsigned i = 0; i < keep; ++i) {
        const Point &a = p[tris[i].a], &b = p[tris[i].b], &c = p[tris[i].c];
        out.push_back({{{a.x, a.y, a.z}, {b.x, b.y, b.z}, {c.x, c.y, c.z}}});
    }
}

void DecodeMesh(const uint8_t *d, size_t dataSize, size_t body, size_t size,
                       std::vector<Triangle> &out)
{
    size_t end = std::min(dataSize, body + size);
    size_t table = 0, tableSize = 0, vif = 0, vifSize = 0;
    for (size_t p = body; p + 8 <= end;) {
        uint32_t id = ReadLE32(d, p), len = ReadLE32(d, p + 4);
        size_t child = p + 8;
        if (len > end - child)
            return;
        if (id == 0x134a01) { table = child; tableSize = len; }
        if (id == 0x134a02) {
            // The VIF chunk body is prefixed with FLUSHE (0x11) bytes; strip DataQWordOffsets are
            // relative to the first UNPACK, so skip the prefix (matches tools/solid_preview.py).
            vif = child;
            vifSize = len;
            while (vifSize > 0 && d[vif] == 0x11) {
                ++vif;
                --vifSize;
            }
        }
        p = child + len;
    }
    if (!table || !vif || tableSize < 4)
        return;
    std::vector<Point> points;
    unsigned strips = (tableSize - 4) / 16;
    for (unsigned i = 0; i < strips; ++i) {
        size_t e = table + 4 + i * 16;
        unsigned nverts = d[e + 2], numpoly = d[e + 3];
        unsigned dq = uint16_t(d[e + 8]) | (uint16_t(d[e + 9]) << 8);
        unsigned qwords = d[e + 12];
        size_t offset = vif + size_t(dq) * 16, bytes = size_t(qwords) * 16;
        if (offset > vif + vifSize || bytes > vif + vifSize - offset)
            continue;
        points.clear();
        if (DecodeStrip(d, dataSize, offset, bytes, nverts, points))
            BuildStripTriangles(points, nverts, numpoly, out);
    }
}

void DecodeChunks(const uint8_t *d, size_t dataSize, size_t begin, size_t end,
                         std::vector<Triangle> &out)
{
    for (size_t p = begin; p + 8 <= end;) {
        uint32_t id = ReadLE32(d, p), len = ReadLE32(d, p + 4);
        size_t body = p + 8;
        if (len > end - body)
            return;
        if (id == 0x80134100)
            DecodeMesh(d, dataSize, body, len, out);
        else if (id & 0x80000000)
            DecodeChunks(d, dataSize, body, body + len, out);
        p = body + len;
    }
}

// Stand-in geometry decoder; the engine equivalent is UG2's solid loader 0x001bf3b8.
std::vector<Triangle> DecodeGeometry(const uint8_t *d, size_t size)
{
    std::vector<Triangle> tris;
    DecodeChunks(d, size, 0, size, tris);
    return tris;
}

} // namespace SolidMesh
