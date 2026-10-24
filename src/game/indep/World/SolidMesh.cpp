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

unsigned long g_exactStrips = 0, g_approxStrips = 0;

static uint32_t ReadLE32(const uint8_t *d, size_t p)
{
    return uint32_t(d[p]) | (uint32_t(d[p + 1]) << 8) | (uint32_t(d[p + 2]) << 16) |
           (uint32_t(d[p + 3]) << 24);
}

// Decodes one strip entry's VIF block into its NumVerts Q15 positions (see tools/solid_mesh.py).
bool DecodeStrip(const uint8_t *d, size_t dataSize, size_t start, size_t size, unsigned nverts,
                       std::vector<Point> &out, uint32_t *seeds)
{
    struct Vec4s { int16_t v[4] = {}; };
    Vec4s positions[256];
    float bounds[2][3] = {};
    bool haveBounds[2] = {};
    float step = 0.0f; // VU address 1 .y: world solids 1/256 m per unit, car solids 0 (Q15 bounds)
    uint8_t colours[32][4] = {};
    bool haveColours = false;
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
                if (seeds)
                    for (unsigned c = 0; c < 4; ++c)
                        seeds[c] = ReadLE32(d, p + c * 4);
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
                // VIF packs the 1-3 tail vertices as V1/V2/V3-16 rows (one qword per axis).
                for (unsigned k = 0; k < num; ++k)
                    for (unsigned c = 0; c < comps; ++c) {
                        uint16_t word = uint16_t(d[p + k * elem + c * 2]) |
                                        (uint16_t(d[p + k * elem + c * 2 + 1]) << 8);
                        positions[addr + k - 5].v[c] = static_cast<int16_t>(word);
                    }
            }
            if (bits == 8 && comps == 4 && addr >= 0x2d && addr < 0x2d + 32) {
                for (unsigned k = 0; k < num && addr - 0x2d + k < 32; ++k)
                    memcpy(colours[addr - 0x2d + k], d + p + k * 4, 4);
                haveColours = true;
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
            if (tail + 2 >= 256) return false;
            for (unsigned a = 0; a < 3; ++a) {
                // Tails are axis-major like the full groups: one qword per axis (x, y, z rows);
                // with exact topology a vertex-major reading of the 3-vertex tail put 156 of 174
                // L4RA strips' last vertices outside their bounds (ISSUES.md #110).
                unsigned qword = tail + a;
                unsigned component = rem == 1 ? 0 : lane;
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
    // UVs: fixed VU layout, two vertices per qword from 0x1d (positions[] is indexed by address - 5).
    for (unsigned i = 0; i < nverts && i < out.size(); ++i) {
        Point &pt = out[out.size() - nverts + i];
        const Vec4s &uv = positions[0x1d - 5 + i / 2];
        // The odd vertex of each qword stores its pair the other way round: (u0, v0, v1, u1). Found on the garage
        // room (CRIB01: the same vertex read (u, v) in one strip and (v, u) in the next, ISSUES #252); it is the cause
        // of the "smeared" world textures of #208.
        const bool odd = (i & 1) != 0 && !getenv("NFSU2_UV_NOSWAP");
        pt.u = uv.v[odd ? 3 : (i & 1) * 2] / 4096.0f;
        pt.v = uv.v[odd ? 2 : (i & 1) * 2 + 1] / 4096.0f;
        if (haveColours && i < 32) {
            pt.r = colours[i][0], pt.g = colours[i][1], pt.b = colours[i][2], pt.a = colours[i][3];
        }
    }
    return true;
}

// The mesh VU program (uploaded by render setup 0x001cfbf0, entered by MSCAL 0) does not read a
// restart list: it seeds the VU random-number register R from VU address 1 and steps it once per
// vertex, and bit 15 of each step's result lands in bit 15 of the vertex's XYZ2 W word -- the GS
// ADC ("don't kick") bit. Dispatch (VU 0x1558-0x1618): a JR into an unrolled list of JALR calls,
// one per group of 4 vertices, starting at 0x1618 - 0x10 * (groups - 1); the delay slot of the JR
// is RINIT R, addr1.x, and the delay slots of the JALRs at 0x1568 / 0x15a8 / 0x15e8 are RINIT from
// .x / .z / .w, run before the call. The software-pipelined vertex routines execute RNEXT 7 times
// on the first call (routine at 0x1628, three of them for the pipeline prologue) and 4 times on
// every later call, so a seed switch before call c lands on vertex 7 + 4 * (c - 1). Verified: the
// resulting kicks equal eStripEntry::NumPolys for all 11,246 SKYLINE and 128,906 L4RA strips.
// RINIT: R = 0x3f800000 | (word & 0x7fffff); RNEXT: R = ((R << 1) ^ bit4 ^ bit22) & 0x7fffff | 0x3f800000.
void StripRestartFlags(const uint32_t seeds[4], unsigned nverts, std::vector<uint8_t> &noKick)
{
    noKick.clear();
    const unsigned groups = (nverts + 3) / 4;
    auto rinit = [](uint32_t w) { return 0x3f800000u | (w & 0x7fffffu); };
    uint32_t r = rinit(seeds[0]);
    for (unsigned call = 0; call < groups && noKick.size() < nverts; ++call) {
        const unsigned jalr = 0x1618 - 0x10 * (groups - 1) + 0x10 * call;
        if (jalr == 0x1568)
            r = rinit(seeds[0]);
        else if (jalr == 0x15a8)
            r = rinit(seeds[2]);
        else if (jalr == 0x15e8)
            r = rinit(seeds[3]);
        const unsigned steps = call == 0 ? 7 : 4;
        for (unsigned k = 0; k < steps; ++k) {
            uint32_t x = (r >> 4) & 1, y = (r >> 22) & 1;
            r = (((r << 1) ^ x ^ y) & 0x7fffffu) | 0x3f800000u;
            noKick.push_back(uint8_t((r >> 15) & 1));
        }
    }
    noKick.resize(nverts, 0);
}

// Topology approximation (ISSUES #65): a strip entry is one triangle strip, but NumPolys is
// always less than NumVerts-2 with an even gap, i.e. (NumVerts-NumPolys)/2 = run count R and 2
// "bridge" triangles are skipped at each internal run seam. The seams are not stored, so drop the
// (NumVerts-2)-NumPolys largest-longest-edge triangles as the bridges.
void BuildStripTriangles(const std::vector<Point> &p, unsigned nverts, unsigned numpoly,
                         std::vector<Triangle> &out, int tex)
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
        Triangle t;
        t.v[0] = a, t.v[1] = b, t.v[2] = c;
        t.tex = int8_t(tex);
        out.push_back(t);
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
    std::vector<uint8_t> noKick;
    unsigned strips = (tableSize - 4) / 16;
    for (unsigned i = 0; i < strips; ++i) {
        size_t e = table + 4 + i * 16;
        unsigned nverts = d[e + 2], numpoly = d[e + 3];
        unsigned dq = uint16_t(d[e + 8]) | (uint16_t(d[e + 9]) << 8);
        int tex = static_cast<int8_t>(d[e + 14]); // eStripEntry::TextureNumber
        unsigned qwords = d[e + 12];
        size_t offset = vif + size_t(dq) * 16, bytes = size_t(qwords) * 16;
        if (offset > vif + vifSize || bytes > vif + vifSize - offset)
            continue;
        points.clear();
        uint32_t seeds[4] = {};
        if (!DecodeStrip(d, dataSize, offset, bytes, nverts, points, seeds))
            continue;
        // Exact topology from the VU restart bits; the old approximation remains only as a guard
        // for data whose kick count disagrees with the strip table (none in SKYLINE or L4RA).
        StripRestartFlags(seeds, nverts, noKick);
        unsigned kicks = 0;
        for (unsigned k = 2; k < nverts; ++k)
            kicks += !noKick[k];
        if (kicks == numpoly) {
            for (unsigned k = 2; k < nverts; ++k)
                if (!noKick[k]) {
                    Triangle t;
                    // GS strips alternate winding; keep a consistent orientation.
                    t.v[0] = points[k % 2 ? k - 1 : k - 2];
                    t.v[1] = points[k % 2 ? k - 2 : k - 1];
                    t.v[2] = points[k];
                    t.tex = int8_t(tex);
                    out.push_back(t);
                }
            ++g_exactStrips;
        } else {
            BuildStripTriangles(points, nverts, numpoly, out, tex);
            ++g_approxStrips;
        }
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
static std::vector<NamedSolid> ParseModSolids(const uint8_t *d, size_t size);

std::vector<Triangle> DecodeGeometry(const uint8_t *d, size_t size)
{
    std::vector<Triangle> tris;
    if (size >= 12 && std::memcmp(d, "NFSU2MOD", 8) == 0) { // a car generated in code (ISSUES #295)
        for (const NamedSolid &s : ParseModSolids(d, size))
            tris.insert(tris.end(), s.tris.begin(), s.tris.end());
        return tris;
    }
    DecodeChunks(d, size, 0, size, tris);
    return tris;
}

namespace {
void CollectNamedSolids(const uint8_t *d, size_t dataSize, size_t begin, size_t end, std::vector<NamedSolid> &out)
{
    for (size_t p = begin; p + 8 <= end;) {
        uint32_t id = ReadLE32(d, p), len = ReadLE32(d, p + 4);
        size_t body = p + 8;
        if (len > end - body)
            return;
        if (id == 0x80134010) {
            NamedSolid s;
            for (size_t q = body; q + 8 <= body + len;) {
                uint32_t cid = ReadLE32(d, q), clen = ReadLE32(d, q + 4);
                if (clen > body + len - (q + 8))
                    break;
                if (cid == 0x134011 && clen >= 0xc0) {
                    // Solid header: name hash +0x10, bbox +0x20/+0x30, name +0xa4 (28 bytes).
                    size_t pad = 0; // 0x11 fill before an unaligned header (see TrackWorld::Load)
                    while (pad < clen - 0xc0 && d[q + 8 + pad] == 0x11)
                        ++pad;
                    const uint8_t *h = d + q + 8 + pad;
                    s.hash = ReadLE32(h, 0x10);
                    memcpy(s.bboxMin, h + 0x20, 12);
                    memcpy(s.bboxMax, h + 0x30, 12);
                    memcpy(s.transform, h + 0x40, sizeof(s.transform));
                    s.name.assign(reinterpret_cast<const char *>(h + 0xa4), strnlen(reinterpret_cast<const char *>(h + 0xa4), clen - pad - 0xa4));
                }
                if (cid == 0x134012) // (texture name hash, 0) pairs
                    for (size_t k = 0; k + 8 <= clen; k += 8)
                        s.textures.push_back(ReadLE32(d + q + 8, k));
                if (cid == 0x13401a) {
                    // Position markers: 8-byte chunk header, then 0x11 fill up to a 16-byte aligned
                    // record array of {u32 name hash, i32, f32, f32, fMatrix4}.  The game's solid
                    // loader FUN_001bf3b8 computes the count as (len - padding) / 0x50.
                    size_t rec = q + 8;
                    while (rec < q + 8 + clen && rec % 16 != 0 && d[rec] == 0x11)
                        ++rec;
                    for (; rec + 0x50 <= q + 8 + clen; rec += 0x50) {
                        PositionMarker m;
                        m.hash = ReadLE32(d, rec);
                        memcpy(m.matrix, d + rec + 0x10, sizeof(m.matrix));
                        s.markers.push_back(m);
                    }
                }
                q += 8 + clen;
            }
            DecodeChunks(d, dataSize, body, body + len, s.tris);
            out.push_back(std::move(s));
        } else if (id & 0x80000000) {
            CollectNamedSolids(d, dataSize, body, body + len, out);
        }
        p = body + len;
    }
}
} // namespace

std::vector<uint8_t> SerializeModSolids(const std::vector<NamedSolid> &solids)
{
    std::vector<uint8_t> out;
    auto put = [&out](const void *p, size_t n) { out.insert(out.end(), static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n); };
    auto u32 = [&put](uint32_t v) { put(&v, 4); };
    put("NFSU2MOD", 8);
    u32(uint32_t(solids.size()));
    for (const NamedSolid &s : solids) {
        u32(uint32_t(s.name.size()));
        put(s.name.data(), s.name.size());
        put(s.bboxMin, 12), put(s.bboxMax, 12);
        u32(uint32_t(s.textures.size()));
        for (uint32_t h : s.textures)
            u32(h);
        u32(uint32_t(s.tris.size()));
        for (const Triangle &t : s.tris) {
            const int32_t tex = t.tex;
            put(&tex, 4);
            for (const Point &p : t.v) {
                const float f[5] = {p.x, p.y, p.z, p.u, p.v};
                put(f, sizeof f);
            }
        }
    }
    return out;
}

static std::vector<NamedSolid> ParseModSolids(const uint8_t *d, size_t size)
{
    std::vector<NamedSolid> out;
    size_t at = 8;
    auto have = [&](size_t n) { return at + n <= size; };
    auto u32 = [&]() { uint32_t v = 0; if (have(4)) std::memcpy(&v, d + at, 4); at += 4; return v; };
    if (!have(4))
        return out;
    const uint32_t count = u32();
    for (uint32_t i = 0; i < count && have(4); ++i) {
        NamedSolid s;
        const uint32_t nameLen = u32();
        if (!have(nameLen + 24))
            break;
        s.name.assign(reinterpret_cast<const char *>(d + at), nameLen), at += nameLen;
        std::memcpy(s.bboxMin, d + at, 12), at += 12;
        std::memcpy(s.bboxMax, d + at, 12), at += 12;
        const uint32_t nTex = u32();
        for (uint32_t k = 0; k < nTex && have(4); ++k)
            s.textures.push_back(u32());
        const uint32_t nTris = u32();
        s.tris.reserve(nTris);
        for (uint32_t k = 0; k < nTris && have(4 + 60); ++k) {
            Triangle t;
            int32_t tex;
            std::memcpy(&tex, d + at, 4), at += 4;
            t.tex = int8_t(tex);
            for (Point &p : t.v) {
                float f[5];
                std::memcpy(f, d + at, sizeof f), at += sizeof f;
                p.x = f[0], p.y = f[1], p.z = f[2], p.u = f[3], p.v = f[4];
            }
            s.tris.push_back(t);
        }
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<NamedSolid> DecodeNamedSolids(const uint8_t *d, size_t size)
{
    if (size >= 12 && std::memcmp(d, "NFSU2MOD", 8) == 0)
        return ParseModSolids(d, size);
    std::vector<NamedSolid> out;
    CollectNamedSolids(d, size, 0, size, out);
    return out;
}

namespace {

// The two-digit style in a name such as SKYLINE_STYLE02_HOOD_LEFT_A or
// SKYLINE_KIT17_FRONT_BUMPER_A.  The suffix check keeps KIT00_HOOD_UNDER out of the hood set.
int TwoDigitStyle(const std::string &name, const char *prefix, const char *suffix)
{
    const size_t at = name.find(prefix);
    if (at == std::string::npos)
        return -1;
    const size_t digits = at + std::strlen(prefix);
    if (digits + 2 > name.size() || name[digits] < '0' || name[digits] > '9' ||
        name[digits + 1] < '0' || name[digits + 1] > '9')
        return -1;
    if (name.find(suffix, digits + 2) == std::string::npos)
        return -1;
    return (name[digits] - '0') * 10 + (name[digits + 1] - '0');
}

} // namespace

bool BodyStyleSet::Has(int partId, int style) const
{
    if (style < 1 || style > 31)
        return false;
    uint32_t mask = 0;
    switch (partId) {
    case 0x1: mask = frontBumper; break;
    case 0x2: mask = rearBumper; break;
    case 0xb: mask = skirt; break;
    case 9: mask = hood; break;
    case 0xe: mask = headlight; break;
    case 0xf: mask = taillight; break;
    case 6: mask = wideBody; break;
    default: return false;
    }
    return (mask & (1u << style)) != 0;
}

BodyStyleSet FindBodyStyles(const std::vector<NamedSolid> &solids)
{
    BodyStyleSet out;
    for (const NamedSolid &s : solids) {
        const std::string &n = s.name;
        int style = TwoDigitStyle(n, "_KIT", "_FRONT_BUMPER");
        if (style > 0)
            out.frontBumper |= 1u << style;
        style = TwoDigitStyle(n, "_KIT", "_REAR_BUMPER");
        if (style > 0)
            out.rearBumper |= 1u << style;
        style = TwoDigitStyle(n, "_KIT", "_SKIRT");
        if (style > 0)
            out.skirt |= 1u << style;
        style = TwoDigitStyle(n, "_STYLE", "_HOOD");
        if (style > 0)
            out.hood |= 1u << style;
        style = TwoDigitStyle(n, "_STYLE", "_HEADLIGHT");
        if (style > 0)
            out.headlight |= 1u << style;
        style = TwoDigitStyle(n, "_STYLE", "_BRAKELIGHT");
        if (style > 0)
            out.taillight |= 1u << style;
        style = TwoDigitStyle(n, "_KITW", "_BODY");
        if (style > 0)
            out.wideBody |= 1u << style;
    }
    return out;
}

Point TransformPoint(const float matrix[16], const Point &p)
{
    const float *m = matrix;
    Point out = p;
    out.x = p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12];
    out.y = p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13];
    out.z = p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14];
    return out;
}

Point TransformPoint(const NamedSolid &solid, const Point &p) { return TransformPoint(solid.transform, p); }

} // namespace SolidMesh
