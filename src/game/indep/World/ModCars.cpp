#include "game/indep/World/ModCars.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include "game/indep/World/CarMaterial.h"
#include "game/indep/World/SolidMesh.h"

namespace CM = CarMaterial;
namespace ModCars {
namespace {
using SolidMesh::NamedSolid;
using SolidMesh::Point;
using SolidMesh::Triangle;

const std::vector<Mod> kMods = {
    // Kia Picanto (TA, 2011-2017): 1.0 MPI 3-cylinder, 66 PS (49 kW), 0-100 in about 13.7 s, 155 km/h, 930 kg.
    {"PICANTO", "PEUGOT106", "KIA", 0.93f, 82.0f, 155.0f}, // peak 82 "game kW": calibrated with NFSU2_DYNO_BENCH to the real 0-100 km/h of 13.7 s
};

struct V3 {
    float x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
V3 Lerp(V3 a, V3 b, float t) { return a + (b + a * -1.0f) * t; }

struct Mesh {
    std::vector<Triangle> tris;
    void tri(V3 a, V3 b, V3 c, int tex = -1)
    {
        Triangle t;
        const V3 p[3] = {a, b, c};
        for (int k = 0; k < 3; ++k)
            t.v[k].x = p[k].x, t.v[k].y = p[k].y, t.v[k].z = p[k].z, t.v[k].u = p[k].x * 0.25f, t.v[k].v = p[k].z * 0.25f;
        t.tex = int8_t(tex);
        tris.push_back(t);
    }
    void quad(V3 a, V3 b, V3 c, V3 d, int tex = -1) { tri(a, b, c, tex), tri(a, c, d, tex); }
    // A box centred on c with half sizes h (used for mirrors, lamp bowls).
    void box(V3 c, V3 h, int tex = -1)
    {
        const V3 p[8] = {{c.x - h.x, c.y - h.y, c.z - h.z}, {c.x + h.x, c.y - h.y, c.z - h.z}, {c.x + h.x, c.y + h.y, c.z - h.z},
                         {c.x - h.x, c.y + h.y, c.z - h.z}, {c.x - h.x, c.y - h.y, c.z + h.z}, {c.x + h.x, c.y - h.y, c.z + h.z},
                         {c.x + h.x, c.y + h.y, c.z + h.z}, {c.x - h.x, c.y + h.y, c.z + h.z}};
        quad(p[0], p[3], p[2], p[1], tex), quad(p[4], p[5], p[6], p[7], tex), quad(p[0], p[1], p[5], p[4], tex);
        quad(p[2], p[3], p[7], p[6], tex), quad(p[1], p[2], p[6], p[5], tex), quad(p[3], p[0], p[4], p[7], tex);
    }
};

NamedSolid Finish(const std::string &name, const Mesh &m, const std::vector<uint32_t> &textures = {})
{
    NamedSolid s;
    s.name = name;
    s.tris = m.tris;
    s.textures = textures;
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (const Triangle &t : m.tris)
        for (const Point &p : t.v) {
            const float c[3] = {p.x, p.y, p.z};
            for (int a = 0; a < 3; ++a)
                lo[a] = std::min(lo[a], c[a]), hi[a] = std::max(hi[a], c[a]);
        }
    for (int a = 0; a < 3; ++a)
        s.bboxMin[a] = lo[a] - 0.01f, s.bboxMax[a] = hi[a] + 0.01f;
    return s;
}

float Piecewise(const float (*pts)[2], int n, float x)
{
    if (x <= pts[0][0])
        return pts[0][1];
    for (int i = 1; i < n; ++i)
        if (x <= pts[i][0]) {
            const float t = (x - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0]);
            return pts[i - 1][1] + (pts[i][1] - pts[i - 1][1]) * t;
        }
    return pts[n - 1][1];
}
float Smooth(float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }

// ---- the Picanto's shape (metres; x forward, y left, z up from the body floor; wheelbase 2.37 m like the donor) -----------------
constexpr float kRear = -1.78f, kFront = 1.81f, kHubFront = 1.19f, kHubRear = -1.18f, kHubZ = 0.11f;
const float kTop[][2] = {{-1.78f, 0.42f}, {-1.745f, 0.80f}, {-1.64f, 1.10f}, {-1.42f, 1.27f}, {-1.10f, 1.32f}, {0.00f, 1.33f},
                         {0.20f, 1.30f},  {0.80f, 0.89f},   {0.92f, 0.86f},  {1.50f, 0.74f},  {1.74f, 0.63f},  {1.81f, 0.47f}};
const float kWidth[][2] = {{-1.78f, 0.70f}, {-1.62f, 0.77f}, {-1.35f, 0.80f}, {1.35f, 0.80f}, {1.65f, 0.77f}, {1.81f, 0.70f}};
const float kFloor[][2] = {{-1.78f, 0.14f}, {-1.65f, 0.04f}, {-1.50f, 0.0f}, {1.45f, 0.0f}, {1.65f, 0.05f}, {1.81f, 0.15f}};

float Top(float x) { return Piecewise(kTop, int(sizeof kTop / sizeof kTop[0]), x); }
float Width(float x) { return Piecewise(kWidth, int(sizeof kWidth / sizeof kWidth[0]), x); }

struct Shape {
    float archR = 0.36f; // wheel arch radius (the donor's tyre plus a clearance)
    float SideBottom(float x) const
    {
        float z = Piecewise(kFloor, int(sizeof kFloor / sizeof kFloor[0]), x);
        for (float hub : {kHubFront, kHubRear}) {
            const float dx = x - hub;
            if (std::fabs(dx) < archR)
                z = std::max(z, kHubZ + std::sqrt(archR * archR - dx * dx));
        }
        return z;
    }
    // One side of the cross-section at x as (y, z) from the top centre down to the floor centre.
    void Ring(float x, float (*p)[2]) const
    {
        const float zt = Top(x), w = Width(x), zs = std::min(SideBottom(x), zt - 0.12f);
        const float cab = std::clamp((zt - 0.80f) / 0.30f, 0.0f, 1.0f); // 1 over the cabin, 0 over the bonnet and boot
        const float wr = (0.94f + (0.62f / 0.80f - 0.94f) * cab) * w;
        const float zRoofEdge = zt - (0.02f + 0.06f * cab);
        const float zBelt = std::min(zt - 0.09f, 0.80f) * cab + (zt - 0.09f) * (1.0f - cab);
        const float zMid = (zBelt + zs) * 0.5f;
        const float yl = std::min(0.62f, w - 0.05f);
        const float pts[9][2] = {{0, zt},           {wr * 0.55f, zt - 0.006f}, {wr, zRoofEdge}, {w, zBelt}, {w * 0.99f, zMid},
                                 {w * 0.955f, zs},  {yl, zs},                  {yl, 0.0f},      {0, 0.0f}};
        std::memcpy(p, pts, sizeof pts);
    }
};

std::vector<NamedSolid> BuildPicanto(const std::vector<NamedSolid> &donorSolids)
{
    std::vector<NamedSolid> out;
    Shape shape;
    // The wheel arch follows the donor's tyre.
    for (const NamedSolid &s : donorSolids)
        if (s.name.find("FRONT_WHEE") != std::string::npos) {
            shape.archR = 0.5f * std::max(s.bboxMax[0] - s.bboxMin[0], s.bboxMax[2] - s.bboxMin[2]) + 0.05f;
            break;
        }

    // ---- body shell: a loft of the cross-section along x ---------------------------------------------------------------------
    Mesh body;
    std::vector<float> xs;
    for (float x = kRear; x < kFront - 1e-4f; x += 0.04f)
        xs.push_back(x);
    xs.push_back(kFront);
    std::vector<std::array<std::array<float, 2>, 9>> rings(xs.size());
    for (size_t i = 0; i < xs.size(); ++i) {
        float p[9][2];
        shape.Ring(xs[i], p);
        for (int j = 0; j < 9; ++j)
            rings[i][size_t(j)] = {p[j][0], p[j][1]};
    }
    auto at = [&](size_t i, int j, float side) { return V3{xs[i], rings[i][size_t(j)][0] * side, rings[i][size_t(j)][1]}; };
    for (size_t i = 0; i + 1 < xs.size(); ++i)
        for (int j = 0; j < 8; ++j) {
            const V3 a = at(i, j, 1), b = at(i + 1, j, 1), c = at(i + 1, j + 1, 1), d = at(i, j + 1, 1);
            body.tri(a, b, c), body.tri(a, c, d);
            const V3 ra = at(i, j, -1), rb = at(i + 1, j, -1), rc = at(i + 1, j + 1, -1), rd = at(i, j + 1, -1);
            body.tri(ra, rc, rb), body.tri(ra, rd, rc);
        }
    // End caps: a fan over the cross-section loop (left side down, right side back up).
    auto cap = [&](size_t i, bool front) {
        std::vector<V3> loop;
        for (int j = 0; j < 9; ++j)
            loop.push_back(at(i, j, 1));
        for (int j = 7; j >= 1; --j)
            loop.push_back(at(i, j, -1));
        V3 c{xs[i], 0.0f, (rings[i][0][1] + 0.0f) * 0.5f};
        for (size_t k = 0; k < loop.size(); ++k) {
            const V3 p = loop[k], q = loop[(k + 1) % loop.size()];
            if (front)
                body.tri(c, q, p);
            else
                body.tri(c, p, q);
        }
    };
    cap(0, false);
    cap(xs.size() - 1, true);
    // Mirrors on the A-pillar bases.
    for (float side : {1.0f, -1.0f}) {
        body.box({0.36f, 0.875f * side, 0.90f}, {0.055f, 0.05f, 0.035f});
        body.box({0.36f, 0.82f * side, 0.88f}, {0.03f, 0.03f, 0.02f});
    }
    out.push_back(Finish("PICANTO_KIT00_BODY_A", body));

    // ---- glass ---------------------------------------------------------------------------------------------------------------
    using CM::NameHash;
    Mesh glass;
    const std::vector<uint32_t> glassSlots = {NameHash("WINDOW_FRONT"), NameHash("WINDOW_REAR"), NameHash("WINDOW_LEFT_FRONT"),
                                              NameHash("WINDOW_RIGHT_FRONT"), NameHash("WINDOW_LEFT_REAR"), NameHash("WINDOW_RIGHT_REAR")};
    auto roofPt = [&](float x, float sy) {
        float p[9][2];
        shape.Ring(x, p);
        return V3{x, p[1][0] * sy, p[1][1] + 0.004f};
    };
    auto strip = [&](float x0, float x1, int tex) { // a flat window across the roof surface between two stations
        const V3 a = roofPt(x0, 1), b = roofPt(x1, 1), c = roofPt(x1, -1), d = roofPt(x0, -1);
        glass.quad(a, b, c, d, tex);
        glass.quad(d, c, b, a, tex);
    };
    strip(0.16f, 0.74f, 0);   // windscreen
    strip(-1.58f, -1.30f, 1); // tailgate glass
    auto sideWindow = [&](float x0, float x1, float sy, int tex) {
        const int n = 6;
        for (int s = 0; s < n; ++s) {
            const float xa = x0 + (x1 - x0) * float(s) / n, xb = x0 + (x1 - x0) * float(s + 1) / n;
            float pa[9][2], pb[9][2];
            shape.Ring(xa, pa), shape.Ring(xb, pb);
            auto sp = [&](float (*p)[2], float x, float f) {
                const V3 u{x, p[2][0] * sy, p[2][1]}, v{x, p[3][0] * sy, p[3][1]};
                V3 r = Lerp(u, v, f);
                r.y += 0.004f * sy;
                return r;
            };
            const V3 a = sp(pa, xa, 0.14f), b = sp(pb, xb, 0.14f), c = sp(pb, xb, 0.86f), d = sp(pa, xa, 0.86f);
            if (sy > 0)
                glass.quad(a, b, c, d, tex), glass.quad(d, c, b, a, tex);
            else
                glass.quad(a, d, c, b, tex), glass.quad(b, c, d, a, tex);
        }
    };
    sideWindow(-0.18f, 0.42f, 1, 2), sideWindow(-0.18f, 0.42f, -1, 3);
    sideWindow(-1.20f, -0.30f, 1, 4), sideWindow(-1.20f, -0.30f, -1, 5);
    out.push_back(Finish("PICANTO_KIT00_GLASS_A", glass, glassSlots));

    // ---- black trim: grille, lower intake, door lines, sills ------------------------------------------------------------------
    Mesh trim;
    const std::vector<uint32_t> trimSlot = {NameHash("WINDOW_TINT3")};
    auto flatQuad = [&](float x, float y0, float y1, float z0, float z1, bool facingFront) { // on a vertical plane x = const
        if (facingFront)
            trim.quad({x, y0, z0}, {x, y1, z0}, {x, y1, z1}, {x, y0, z1}, 0);
        else
            trim.quad({x, y1, z0}, {x, y0, z0}, {x, y0, z1}, {x, y1, z1}, 0);
    };
    flatQuad(kFront + 0.004f, -0.44f, 0.44f, 0.17f, 0.28f, true);   // lower intake
    flatQuad(kFront + 0.004f, -0.22f, 0.22f, 0.33f, 0.41f, true);   // the grille
    flatQuad(kRear - 0.004f, -0.56f, 0.56f, 0.16f, 0.27f, false);   // rear diffuser strip
    for (float side : {1.0f, -1.0f})
        for (float x : {0.44f, -0.26f, -1.13f}) { // door shut lines
            float p[9][2];
            shape.Ring(x, p);
            const float y = p[3][0] * side + 0.0035f * side;
            const float z0 = p[5][1] + 0.06f, z1 = p[3][1] - 0.02f;
            const float w = 0.006f;
            if (side > 0)
                trim.quad({x - w, y, z0}, {x + w, y, z0}, {x + w, y, z1}, {x - w, y, z1}, 0);
            else
                trim.quad({x + w, y, z0}, {x - w, y, z0}, {x - w, y, z1}, {x + w, y, z1}, 0);
        }
    // Black plastic: the lower bumper bands, side sills and the wheel arch lips make the shell read as a car instead of a blob.
    auto sideY = [&](float x, float z) { // the side surface's half width at height z (between the mid-side and bottom-edge points)
        float p[9][2];
        shape.Ring(x, p);
        const float t = std::clamp((p[4][1] - z) / std::max(1e-3f, p[4][1] - p[5][1]), 0.0f, 1.0f);
        return p[4][0] + (p[5][0] - p[4][0]) * t;
    };
    for (float side : {1.0f, -1.0f}) {
        // sills between the arches
        for (float x = -0.70f; x < 0.70f; x += 0.1f) {
            const float xa = x, xb = x + 0.1f, ya = sideY(xa, 0.07f) + 0.004f, yb = sideY(xb, 0.07f) + 0.004f;
            if (side > 0)
                trim.quad({xa, ya, 0.01f}, {xb, yb, 0.01f}, {xb, yb, 0.12f}, {xa, ya, 0.12f}, 0);
            else
                trim.quad({xb, -yb, 0.01f}, {xa, -ya, 0.01f}, {xa, -ya, 0.12f}, {xb, -yb, 0.12f}, 0);
        }
        // arch lips
        for (float hub : {kHubFront, kHubRear}) {
            const int seg = 20;
            for (int k = 0; k < seg; ++k) {
                const float a0 = 3.14159265f * float(k) / seg, a1 = 3.14159265f * float(k + 1) / seg;
                auto pt = [&](float a, float r) {
                    const float x = hub + r * std::cos(a), z = kHubZ + r * std::sin(a);
                    return V3{x, side * (sideY(x, z) + 0.004f), z};
                };
                const V3 i0 = pt(a0, shape.archR), i1 = pt(a1, shape.archR), o0 = pt(a0, shape.archR + 0.05f), o1 = pt(a1, shape.archR + 0.05f);
                if (side > 0)
                    trim.quad(i0, i1, o1, o0, 0);
                else
                    trim.quad(i1, i0, o0, o1, 0);
            }
        }
    }
    // front and rear lower bumper bands on the faces
    flatQuad(kFront + 0.004f, -0.70f, 0.70f, 0.15f, 0.20f, true);
    flatQuad(kRear - 0.004f, -0.70f, 0.70f, 0.14f, 0.20f, false);
    out.push_back(Finish("PICANTO_KIT00_TRIM_A", trim, trimSlot));

    // ---- lamps: a lens (lit by the car shader), a clear cover in front of it -------------------------------------------------
    for (float side : {1.0f, -1.0f}) {
        Mesh lamp;
        const bool left = side > 0;
        const std::vector<uint32_t> slots = {NameHash(left ? "HEADLIGHT_LEFT" : "HEADLIGHT_RIGHT"), NameHash(left ? "HEADLIGHT_GLASS_LEFT" : "HEADLIGHT_GLASS_RIGHT")};
        const float y0 = 0.30f * side, y1 = 0.63f * side;
        auto face = [&](float x, int tex) {
            if (left)
                lamp.quad({x, y0, 0.26f}, {x, y1, 0.26f}, {x, y1, 0.44f}, {x, y0, 0.44f}, tex);
            else
                lamp.quad({x, y1, 0.26f}, {x, y0, 0.26f}, {x, y0, 0.44f}, {x, y1, 0.44f}, tex);
        };
        face(kFront + 0.003f, 0); // the lens / reflector
        lamp.box({kFront + 0.002f, (y0 + y1) * 0.5f, 0.35f}, {0.004f, 0.10f, 0.045f}, 0);
        face(kFront + 0.014f, 1); // the clear cover
        out.push_back(Finish(std::string("PICANTO_KIT00_HEADLIGHT_") + (left ? "LEFT" : "RIGHT") + "_A", lamp, slots));
    }
    {
        Mesh tail;
        const std::vector<uint32_t> slots = {NameHash("BRAKELIGHT_LEFT"), NameHash("BRAKELIGHT_RIGHT"), NameHash("BRAKELIGHT_GLASS_LEFT"),
                                             NameHash("BRAKELIGHT_GLASS_RIGHT")};
        for (float side : {1.0f, -1.0f}) {
            const bool left = side > 0;
            const float y0 = 0.38f * side, y1 = 0.72f * side;
            // rear faces look backwards, so the winding is the reverse of the front lamps'
            const float xl = kRear - 0.004f, xc = kRear - 0.014f;
            if (left)
                tail.quad({xl, y1, 0.50f}, {xl, y0, 0.50f}, {xl, y0, 0.76f}, {xl, y1, 0.76f}, left ? 0 : 1);
            else
                tail.quad({xl, y0, 0.50f}, {xl, y1, 0.50f}, {xl, y1, 0.76f}, {xl, y0, 0.76f}, 1);
            if (left)
                tail.quad({xc, y1, 0.50f}, {xc, y0, 0.50f}, {xc, y0, 0.76f}, {xc, y1, 0.76f}, 2);
            else
                tail.quad({xc, y0, 0.50f}, {xc, y1, 0.50f}, {xc, y1, 0.76f}, {xc, y0, 0.76f}, 3);
        }
        out.push_back(Finish("PICANTO_KIT00_BRAKELIGHT_A", tail, slots));
    }

    // ---- wheels and brakes of the donor, renamed ------------------------------------------------------------------------------
    for (const NamedSolid &s : donorSolids) {
        const std::string &n = s.name;
        const bool part = n.find("FRONT_WHEE") != std::string::npos || n.find("REAR_WHEE") != std::string::npos ||
                          n.find("FRONT_BRAK") != std::string::npos || n.find("REAR_BRAK") != std::string::npos;
        if (!part || n.size() < 2)
            continue;
        const std::string tail2 = n.substr(n.size() - 2);
        if (tail2 == "_B" || tail2 == "_C" || tail2 == "_D")
            continue; // the lower LODs
        NamedSolid c = s;
        const size_t kit = n.find("_KIT00_");
        c.name = kit == std::string::npos ? "PICANTO_KIT00_" + n : "PICANTO" + n.substr(kit);
        out.push_back(std::move(c));
    }
    return out;
}

void PutStr(std::vector<uint8_t> &r, size_t off, size_t len, const std::string &s)
{
    for (size_t i = 0; i < len && off + i < r.size(); ++i)
        r[off + i] = i < s.size() ? uint8_t(s[i]) : 0;
}
float GetF(const std::vector<uint8_t> &r, size_t off)
{
    float v;
    std::memcpy(&v, r.data() + off, 4);
    return v;
}
void SetF(std::vector<uint8_t> &r, size_t off, float v) { std::memcpy(r.data() + off, &v, 4); }
} // namespace

const std::vector<Mod> &All() { return kMods; }

const Mod *Find(const std::string &folderUpper)
{
    for (const Mod &m : kMods)
        if (folderUpper == m.folder)
            return &m;
    return nullptr;
}

bool IsModPath(const char *path)
{
    if (!path || strncasecmp(path, "CARS\\", 5) != 0)
        return false;
    for (const Mod &m : kMods) {
        const size_t n = std::strlen(m.folder);
        if (strncasecmp(path + 5, m.folder, n) == 0 && path[5 + n] == '\\')
            return true;
    }
    return false;
}

std::vector<uint8_t> Read(const char *path, const std::function<std::vector<uint8_t>(const char *)> &base)
{
    for (const Mod &m : kMods) {
        const std::string prefix = std::string("CARS\\") + m.folder + "\\";
        if (strncasecmp(path, prefix.c_str(), prefix.size()) != 0)
            continue;
        const std::string file = path + prefix.size();
        const std::string donorPrefix = std::string("CARS\\") + m.donor + "\\";
        if (strncasecmp(file.c_str(), "GEOMETRY", 8) == 0) {
            if (strncasecmp(file.c_str(), "GEOMETRY.BIN", 12) != 0)
                return {}; // no .LZC variant
            std::vector<uint8_t> donor = base((donorPrefix + "GEOMETRY.BIN").c_str());
            if (donor.empty())
                donor = base((donorPrefix + "GEOMETRY.LZC").c_str());
            const std::vector<NamedSolid> donorSolids = SolidMesh::DecodeNamedSolids(donor.data(), donor.size());
            return SolidMesh::SerializeModSolids(BuildPicanto(donorSolids));
        }
        return base((donorPrefix + file).c_str()); // TEXTURES, VINYLS, PARTS_ANIMATIONS: the donor's
    }
    return base(path);
}

std::vector<uint8_t> MakeRecord(const std::vector<uint8_t> &donor, const Mod &mod)
{
    std::vector<uint8_t> r = donor;
    if (r.size() != 0x890)
        return {};
    PutStr(r, 0x08, 0x20, mod.folder);
    PutStr(r, 0x28, 0x20, mod.folder);
    PutStr(r, 0x48, 0x20, std::string("CARS\\") + mod.folder + "\\GEOMETRY.BIN");
    PutStr(r, 0x68, 0x20, std::string("CARS\\") + mod.folder + "\\GEOMETRY.LZC");
    PutStr(r, 0xc8, 0x20, mod.manufacturer);
    // Mass, and everything that scales with it (inertia, springs and dampers, brake torque).
    const float oldMass = GetF(r, 0x228);
    const float ratio = oldMass > 0.05f ? mod.massTonnes / oldMass : 1.0f;
    SetF(r, 0x228, mod.massTonnes);
    for (size_t off : {size_t(0x238), size_t(0x24c), size_t(0x260), size_t(0x38c)})
        SetF(r, off, GetF(r, off) * ratio);
    for (size_t base : {size_t(0x288), size_t(0x2a8)})
        for (size_t k : {size_t(4), size_t(8), size_t(0xc), size_t(0x10)})
            SetF(r, base + k, GetF(r, base + k) * ratio);
    // Engine: scale the torque curves so the peak power is the car's (torque kN*m x rad/s = kW).
    const float idle = GetF(r, 0x308), maxRpm = GetF(r, 0x310);
    float peak = 0.0f;
    for (int k = 0; k < 9; ++k) {
        const float rpm = idle + (maxRpm - idle) * float(k) / 8.0f;
        peak = std::max(peak, GetF(r, 0x318 + 4 * size_t(k)) * rpm * 0.104720f);
    }
    if (peak > 1.0f) {
        const float scale = mod.peakKw / peak;
        for (int k = 0; k < 9; ++k) {
            SetF(r, 0x318 + 4 * size_t(k), GetF(r, 0x318 + 4 * size_t(k)) * scale);
            SetF(r, 0x33c + 4 * size_t(k), GetF(r, 0x33c + 4 * size_t(k)) * scale);
        }
    }
    SetF(r, 0x3a8, mod.topSpeedKmh / 1.609344f); // the limiter, in mph
    return r;
}
} // namespace ModCars
