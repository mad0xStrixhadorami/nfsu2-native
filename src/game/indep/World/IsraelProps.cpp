// Israeli street dressing placed over the whole loaded region (ISSUES #272/#273): trees, rooftop solar water heaters
// ("dud shemesh"), AC condensers, antennas, bus stops, road signs, e-scooters, cafe tables, and the triangle flags the
// world shader reads (vertex alpha 253 = kerb face -> red/white or blue/white paint, 252 = pavement strip -> green bike
// lane).  Everything is visual only: the triangles go to the render lists, not to the physics cells.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

#include "game/indep/Texture/TexturePack.h"
#include "game/indep/World/SignGen.h"
#include "game/indep/World/TrackWorld.h"

namespace {

struct V3 {
    float x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Len(V3 a) { return std::sqrt(Dot(a, a)); }
V3 Norm(V3 a)
{
    const float l = Len(a);
    return l > 1e-6f ? a * (1.0f / l) : V3{0, 0, 1};
}

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed * 2654435761u + 0x9e3779b9u) { f(), f(); }
    float f()
    {
        s ^= s << 13, s ^= s >> 17, s ^= s << 5;
        return float(s & 0xffffff) / float(0x1000000);
    }
};

uint32_t HashXY(int x, int y, uint32_t salt)
{
    uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ salt * 83492791u;
    h ^= h >> 15, h *= 0x2c1b3c6du, h ^= h >> 12;
    return h;
}

int64_t Key(int x, int y) { return (int64_t(x) << 32) ^ uint32_t(y); }

// The four districts of the theme (same centres as the world shader).
struct District {
    float tlv, jer, hai, bsh;
};
District DistrictAt(float x, float y)
{
    auto g = [&](float cx, float cy, float s2) { return std::exp(-((x - cx) * (x - cx) + (y - cy) * (y - cy)) / s2); };
    District d{g(-300, -420, 640000), g(-1050, 580, 490000), g(-1850, 1120, 490000), g(-250, -1650, 640000)};
    const float sum = std::max(d.tlv + d.jer + d.hai + d.bsh, 1e-4f);
    d.tlv /= sum, d.jer /= sum, d.hai /= sum, d.bsh /= sum;
    return d;
}

} // namespace

// Mesh builder over a render triangle list (flat shaded, alpha 254 = "prop").
struct PropBuilder {
    std::vector<TrackWorld::RTriPublic> *out;
    size_t cap;
    std::vector<TrackWorld::PropSolid> *solids = nullptr;
    uint8_t alpha = 254; // 254 = prop (skips the facade skin); 255 = an ordinary wall the world shader skins
    void Solid(float x, float y, float r, float z, float h)
    {
        if (solids)
            solids->push_back({x, y, r, z, z + h});
    }

    void Tri(V3 a, V3 b, V3 c, const uint8_t col[3])
    {
        if (out->size() >= cap)
            return;
        const V3 n = Norm(Cross(b - a, c - a));
        // two-sided flat light from the sun direction plus a sky term
        const float l = std::fabs(Dot(n, Norm({0.35f, 0.2f, 0.9f})));
        const float k = 0.52f + 0.55f * l;
        TrackWorld::RTriPublic t;
        t.tex = 0;
        const V3 p[3] = {a, b, c};
        for (int i = 0; i < 3; ++i) {
            t.v[i].x = p[i].x, t.v[i].y = p[i].y, t.v[i].z = p[i].z, t.v[i].u = 0, t.v[i].v = 0;
            t.v[i].r = uint8_t(std::min(255.0f, col[0] * k));
            t.v[i].g = uint8_t(std::min(255.0f, col[1] * k));
            t.v[i].b = uint8_t(std::min(255.0f, col[2] * k));
            t.v[i].a = alpha;
        }
        out->push_back(t);
    }
    void Quad(V3 a, V3 b, V3 c, V3 d, const uint8_t col[3]) { Tri(a, b, c, col), Tri(a, c, d, col); }
    // textured quad a(u0,v1) b(u1,v1) c(u1,v0) d(u0,v0) from the texture rows [v0, v1] (a = bottom left, b = bottom right)
    void TexQuad(V3 a, V3 b, V3 c, V3 d, uint32_t tex, float u0, float u1, float v0, float v1)
    {
        if (out->size() + 2 > cap)
            return;
        const V3 p[4] = {a, b, c, d};
        const float uv[4][2] = {{u0, v1}, {u1, v1}, {u1, v0}, {u0, v0}};
        const int idx[2][3] = {{0, 1, 2}, {0, 2, 3}};
        for (const auto &tri : idx) {
            TrackWorld::RTriPublic t;
            t.tex = tex;
            for (int i = 0; i < 3; ++i) {
                auto &v = t.v[i];
                v.x = p[tri[i]].x, v.y = p[tri[i]].y, v.z = p[tri[i]].z, v.u = uv[tri[i]][0], v.v = uv[tri[i]][1];
                v.r = v.g = v.b = 255, v.a = 254;
            }
            out->push_back(t);
        }
    }
    // box from corner o spanned by three vectors
    void Box(V3 o, V3 ax, V3 ay, V3 az, const uint8_t col[3])
    {
        const V3 p000 = o, p100 = o + ax, p010 = o + ay, p110 = o + ax + ay;
        const V3 p001 = o + az, p101 = o + ax + az, p011 = o + ay + az, p111 = o + ax + ay + az;
        Quad(p000, p100, p110, p010, col), Quad(p001, p101, p111, p011, col);
        Quad(p000, p100, p101, p001, col), Quad(p010, p110, p111, p011, col);
        Quad(p000, p010, p011, p001, col), Quad(p100, p110, p111, p101, col);
    }
    // upright box centred on (cx, cy), bottom at z, rotated by yaw
    void UBox(float cx, float cy, float z, float sx, float sy, float sz, float yaw, const uint8_t col[3])
    {
        const V3 u{std::cos(yaw), std::sin(yaw), 0}, v{-std::sin(yaw), std::cos(yaw), 0};
        Box(V3{cx, cy, z} - u * (sx * 0.5f) - v * (sy * 0.5f), u * sx, v * sy, {0, 0, sz}, col);
    }
    // tapered prism between two points
    void Prism(V3 p0, V3 p1, float r0, float r1, int sides, const uint8_t col[3])
    {
        const V3 ax = Norm(p1 - p0);
        V3 s = Cross(ax, std::fabs(ax.z) > 0.9f ? V3{1, 0, 0} : V3{0, 0, 1});
        s = Norm(s);
        const V3 t = Cross(ax, s);
        for (int i = 0; i < sides; ++i) {
            const float a0 = 6.2831853f * i / sides, a1 = 6.2831853f * (i + 1) / sides;
            const V3 d0 = s * std::cos(a0) + t * std::sin(a0), d1 = s * std::cos(a1) + t * std::sin(a1);
            Quad(p0 + d0 * r0, p0 + d1 * r0, p1 + d1 * r1, p1 + d0 * r1, col);
        }
        // caps
        for (int i = 0; i < sides; ++i) {
            const float a0 = 6.2831853f * i / sides, a1 = 6.2831853f * (i + 1) / sides;
            const V3 d0 = s * std::cos(a0) + t * std::sin(a0), d1 = s * std::cos(a1) + t * std::sin(a1);
            Tri(p1, p1 + d0 * r1, p1 + d1 * r1, col);
        }
    }
    void Cone(V3 base, float r, float h, int sides, const uint8_t col[3])
    {
        for (int i = 0; i < sides; ++i) {
            const float a0 = 6.2831853f * i / sides, a1 = 6.2831853f * (i + 1) / sides;
            Tri(base + V3{r * std::cos(a0), r * std::sin(a0), 0}, base + V3{r * std::cos(a1), r * std::sin(a1), 0},
                base + V3{0, 0, h}, col);
        }
    }
    // flat n-gon facing `normal`
    void Disc(V3 c, V3 normal, float r, int sides, const uint8_t col[3])
    {
        const V3 n = Norm(normal);
        const V3 s = Norm(Cross(n, std::fabs(n.z) > 0.9f ? V3{1, 0, 0} : V3{0, 0, 1}));
        const V3 t = Cross(n, s);
        for (int i = 0; i < sides; ++i) {
            const float a0 = 6.2831853f * i / sides, a1 = 6.2831853f * (i + 1) / sides;
            Tri(c, c + (s * std::cos(a0) + t * std::sin(a0)) * r, c + (s * std::cos(a1) + t * std::sin(a1)) * r, col);
        }
    }
    void Ellipsoid(V3 c, float rx, float rz, int segs, const uint8_t col[3])
    {
        const int rings = 4;
        for (int j = 0; j < rings; ++j) {
            const float p0 = 3.14159265f * (float(j) / rings - 0.5f), p1 = 3.14159265f * (float(j + 1) / rings - 0.5f);
            for (int i = 0; i < segs; ++i) {
                const float a0 = 6.2831853f * i / segs, a1 = 6.2831853f * (i + 1) / segs;
                auto P = [&](float a, float p) {
                    return c + V3{rx * std::cos(p) * std::cos(a), rx * std::cos(p) * std::sin(a), rz * std::sin(p)};
                };
                Quad(P(a0, p0), P(a1, p0), P(a1, p1), P(a0, p1), col);
            }
        }
    }
};

namespace {
constexpr uint32_t kShopTex = 0x5e500000u, kStreetTex = 0x5e500010u, kFlagTex = 0x5e500020u, kShutterTex = 0x5e500030u, kAwningTex = 0x5e500040u; // generated sign atlases (512 x 512)
bool g_signs = false;
const uint8_t kGreen[3] = {52, 112, 46}, kDarkGreen[3] = {36, 78, 42}, kTrunk[3] = {120, 98, 74}, kGrey[3] = {170, 170, 175},
              kLightGrey[3] = {205, 205, 205}, kDarkGrey[3] = {50, 52, 56}, kWhite[3] = {235, 235, 230},
              kPanel[3] = {28, 44, 82}, kTank[3] = {225, 225, 220}, kBlue[3] = {20, 70, 160}, kRed[3] = {190, 30, 30},
              kYellow[3] = {220, 190, 40}, kStone[3] = {222, 205, 170}, kOlive[3] = {104, 120, 78};

void Palm(PropBuilder &b, V3 base, Rng &r)
{
    const float h = 5.5f + 4.0f * r.f();
    const float lean = (r.f() - 0.5f) * 0.9f, la = r.f() * 6.28f;
    const V3 top = base + V3{lean * std::cos(la), lean * std::sin(la), h};
    b.Prism(base, top, 0.2f, 0.13f, 6, kTrunk);
    b.Solid(base.x, base.y, 0.32f, base.z, 3.0f);
    const int fronds = 9;
    for (int i = 0; i < fronds; ++i) {
        const float a = 6.2831853f * (i + 0.3f * r.f()) / fronds;
        const V3 d{std::cos(a), std::sin(a), 0}, side{-std::sin(a), std::cos(a), 0};
        const V3 mid = top + d * 1.5f + V3{0, 0, 0.55f}, tip = top + d * 3.0f + V3{0, 0, -0.55f};
        b.Tri(top, mid + side * 0.32f, mid - side * 0.32f, kGreen);
        b.Tri(mid + side * 0.32f, tip, mid - side * 0.32f, kDarkGreen);
    }
}

void Pine(PropBuilder &b, V3 base, Rng &r) // Aleppo / stone pine: bare trunk, layered dark crown
{
    const float h = 4.5f + 3.0f * r.f();
    b.Prism(base, base + V3{0, 0, h}, 0.22f, 0.12f, 6, kTrunk);
    b.Solid(base.x, base.y, 0.34f, base.z, 3.0f);
    const float rad = 1.9f + r.f() * 0.8f;
    for (int i = 0; i < 3; ++i)
        b.Cone(base + V3{0, 0, h * 0.55f + i * h * 0.17f}, rad * (1.0f - 0.27f * i), h * 0.34f, 8, i == 1 ? kGreen : kDarkGreen);
}

void Cypress(PropBuilder &b, V3 base, Rng &r)
{
    const float h = 5.0f + 3.0f * r.f();
    b.Prism(base, base + V3{0, 0, 0.8f}, 0.1f, 0.1f, 5, kTrunk);
    b.Solid(base.x, base.y, 0.55f, base.z, 3.0f);
    b.Cone(base + V3{0, 0, 0.5f}, 0.75f, h, 7, kDarkGreen);
}

void Olive(PropBuilder &b, V3 base, Rng &r) // olive / ficus: short trunk, wide grey-green crown
{
    const float h = 2.0f + r.f();
    b.Prism(base, base + V3{0, 0, h}, 0.2f, 0.14f, 6, kTrunk);
    b.Solid(base.x, base.y, 0.32f, base.z, 3.0f);
    b.Ellipsoid(base + V3{0, 0, h + 1.2f}, 2.0f + r.f() * 0.6f, 1.5f, 8, r.f() > 0.5f ? kOlive : kGreen);
}

void SolarHeater(PropBuilder &b, V3 o, float yaw) // dud shemesh: tilted panel with the tank on its high edge
{
    const V3 u{std::cos(yaw), std::sin(yaw), 0}, v{-std::sin(yaw), std::cos(yaw), 0};
    const float tilt = 0.52f;
    const V3 panelUp = v * (1.9f * std::cos(tilt)) + V3{0, 0, 1.9f * std::sin(tilt)};
    const V3 pc = o + V3{0, 0, 0.25f};
    b.Box(pc - u * 0.6f, u * 1.2f, panelUp, Cross(u, Norm(panelUp)) * -0.07f, kPanel);
    b.Box(o - u * 0.55f, u * 0.08f, v * 0.9f, {0, 0, 0.3f}, kGrey); // stand
    b.Box(o + u * 0.47f, u * 0.08f, v * 0.9f, {0, 0, 0.3f}, kGrey);
    const V3 tankC = pc + panelUp + V3{0, 0, 0.32f} + v * 0.1f;
    b.Prism(tankC - u * 0.62f, tankC + u * 0.62f, 0.3f, 0.3f, 8, kTank);
}

void Condenser(PropBuilder &b, V3 base, float yaw, bool onWall, V3 wallN)
{
    const V3 u{std::cos(yaw), std::sin(yaw), 0}, v{-std::sin(yaw), std::cos(yaw), 0};
    b.Box(base - u * 0.42f - v * 0.17f, u * 0.84f, v * 0.34f, {0, 0, 0.62f}, kLightGrey);
    // fan grille on the front face
    const V3 front = (onWall ? Norm(wallN) : v);
    b.Disc(base + V3{0, 0, 0.32f} + v * 0.175f + (onWall ? V3{0, 0, 0} : V3{0, 0, 0}), front, 0.2f, 8, kDarkGrey);
}

void Antenna(PropBuilder &b, V3 base, float yaw)
{
    const V3 u{std::cos(yaw), std::sin(yaw), 0};
    b.Prism(base, base + V3{0, 0, 3.0f}, 0.025f, 0.02f, 4, kGrey);
    for (int i = 0; i < 3; ++i)
        b.Prism(base + V3{0, 0, 1.8f + i * 0.45f} - u * (0.55f - i * 0.12f), base + V3{0, 0, 1.8f + i * 0.45f} + u * (0.55f - i * 0.12f),
                0.012f, 0.012f, 4, kGrey);
}

void WaterTank(PropBuilder &b, V3 base)
{
    for (int i = 0; i < 4; ++i)
        b.Prism(base + V3{(i & 1) ? 0.4f : -0.4f, (i & 2) ? 0.4f : -0.4f, 0}, base + V3{(i & 1) ? 0.4f : -0.4f, (i & 2) ? 0.4f : -0.4f, 0.8f}, 0.04f, 0.04f, 4, kGrey);
    b.Prism(base + V3{0, 0, 0.8f}, base + V3{0, 0, 1.9f}, 0.62f, 0.62f, 10, kTank);
}

void BusStop(PropBuilder &b, V3 base, float yaw)
{
    const V3 u{std::cos(yaw), std::sin(yaw), 0}, v{-std::sin(yaw), std::cos(yaw), 0};
    const uint8_t glass[3] = {150, 190, 205};
    b.Box(base - u * 1.7f - v * 0.7f + V3{0, 0, 2.4f}, u * 3.4f, v * 1.4f, {0, 0, 0.12f}, kStone); // roof slab
    for (int i = 0; i < 2; ++i)                                                                        // posts
        b.Prism(base + u * (i ? 1.65f : -1.65f) - v * 0.65f, base + u * (i ? 1.65f : -1.65f) - v * 0.65f + V3{0, 0, 2.4f}, 0.05f, 0.05f, 5, kGrey);
    b.Box(base - u * 1.65f - v * 0.68f + V3{0, 0, 0.3f}, u * 3.3f, v * 0.04f, {0, 0, 1.9f}, glass); // back glass
    b.Box(base - u * 1.1f - v * 0.45f + V3{0, 0, 0.45f}, u * 2.2f, v * 0.38f, {0, 0, 0.07f}, kStone); // bench
    for (int i = -1; i <= 1; ++i)
        b.Solid(base.x + u.x * 1.2f * i - v.x * 0.55f, base.y + u.y * 1.2f * i - v.y * 0.55f, 0.55f, base.z, 2.4f);
    // the blue-and-white stop pole with its sign
    const V3 pole = base + u * 2.2f + v * 0.3f;
    b.Prism(pole, pole + V3{0, 0, 3.0f}, 0.04f, 0.04f, 5, kGrey);
    b.Box(pole + V3{0, 0, 2.5f} - u * 0.3f - v * 0.02f, u * 0.6f, v * 0.03f, {0, 0, 0.5f}, kBlue);
    b.Box(pole + V3{0, 0, 2.58f} - u * 0.22f - v * 0.035f, u * 0.44f, v * 0.012f, {0, 0, 0.34f}, kWhite);
}

void RoadSign(PropBuilder &b, V3 base, float yaw, int type)
{
    const V3 u{std::cos(yaw), std::sin(yaw), 0}, v{-std::sin(yaw), std::cos(yaw), 0};
    b.Prism(base, base + V3{0, 0, 3.0f}, 0.04f, 0.04f, 5, kGrey);
    b.Solid(base.x, base.y, 0.14f, base.z, 3.0f);
    const V3 c = base + V3{0, 0, 2.7f} + v * 0.06f;
    const V3 n = v;
    switch (type) {
    case 0: // speed limit / prohibition: red ring on white
        b.Disc(c, n, 0.32f, 14, kRed);
        b.Disc(c + n * 0.012f, n, 0.23f, 14, kWhite);
        b.Box(c + n * 0.02f - u * 0.1f + V3{0, 0, -0.05f}, u * 0.2f, n * 0.004f, {0, 0, 0.1f}, kDarkGrey);
        break;
    case 1: // blue info / parking
        b.Box(c - u * 0.3f + V3{0, 0, -0.3f}, u * 0.6f, n * 0.03f, {0, 0, 0.6f}, kBlue);
        b.Box(c + n * 0.03f - u * 0.1f + V3{0, 0, -0.15f}, u * 0.2f, n * 0.004f, {0, 0, 0.3f}, kWhite);
        break;
    case 2: { // warning triangle: red border, white inside, point up
        const V3 a = c - u * 0.45f + V3{0, 0, -0.3f}, bb = c + u * 0.45f + V3{0, 0, -0.3f}, t = c + V3{0, 0, 0.5f};
        b.Tri(a, bb, t, kRed);
        const V3 ctr = (a + bb + t) * (1.0f / 3.0f);
        b.Tri(ctr + (a - ctr) * 0.62f + n * 0.012f, ctr + (bb - ctr) * 0.62f + n * 0.012f, ctr + (t - ctr) * 0.62f + n * 0.012f, kWhite);
        break;
    }
    default: // stop
        b.Disc(c, n, 0.36f, 8, kRed);
        b.Box(c + n * 0.014f - u * 0.22f + V3{0, 0, -0.04f}, u * 0.44f, n * 0.004f, {0, 0, 0.08f}, kWhite);
        break;
    }
}

// A street-name sign (blue, Hebrew over Latin) on a pole, readable from both sides.
void StreetSign(PropBuilder &b, V3 base, float yaw, int variant)
{
    const V3 u{std::cos(yaw), std::sin(yaw), 0}, v{-std::sin(yaw), std::cos(yaw), 0};
    b.Prism(base, base + V3{0, 0, 3.1f}, 0.045f, 0.045f, 5, kGrey);
    b.Solid(base.x, base.y, 0.14f, base.z, 3.1f);
    const int tex = variant / 4, row = variant % 4;
    const float v0 = row * 0.25f, v1 = v0 + 0.25f;
    const V3 c = base + V3{0, 0, 2.95f};
    b.TexQuad(c - u * 0.55f + v * 0.06f - V3{0, 0, 0.14f}, c + u * 0.55f + v * 0.06f - V3{0, 0, 0.14f}, c + u * 0.55f + v * 0.06f + V3{0, 0, 0.14f},
              c - u * 0.55f + v * 0.06f + V3{0, 0, 0.14f}, kStreetTex + tex, 0, 1, v0, v1);
    b.TexQuad(c + u * 0.55f - v * 0.06f - V3{0, 0, 0.14f}, c - u * 0.55f - v * 0.06f - V3{0, 0, 0.14f}, c - u * 0.55f - v * 0.06f + V3{0, 0, 0.14f},
              c + u * 0.55f - v * 0.06f + V3{0, 0, 0.14f}, kStreetTex + tex, 0, 1, v0, v1);
}

// The flag of Israel on a tall pole.
void FlagPole(PropBuilder &b, V3 base, float yaw, float h)
{
    const V3 u{std::cos(yaw), std::sin(yaw), 0};
    b.Prism(base, base + V3{0, 0, h}, 0.05f, 0.035f, 6, kGrey);
    b.Solid(base.x, base.y, 0.16f, base.z, h);
    const V3 top = base + V3{0, 0, h - 0.1f};
    const float fw = 1.65f, fh = fw * 8.0f / 11.0f;
    b.TexQuad(top + u * 0.05f - V3{0, 0, fh}, top + u * (0.05f + fw) - V3{0, 0, fh}, top + u * (0.05f + fw), top + u * 0.05f, kFlagTex, 0, 1, 0, 1);
    b.TexQuad(top + u * (0.05f + fw) - V3{0, 0, fh}, top + u * 0.05f - V3{0, 0, fh}, top + u * 0.05f, top + u * (0.05f + fw), kFlagTex, 0, 1, 0, 1);
}

// A shop sign hung on a facade, double-sided so it reads from the street whichever way the wall faces.
void ShopSign(PropBuilder &b, V3 c, V3 along, V3 out, int variant, float width)
{
    const int tex = variant / 4, row = variant % 4;
    const float v0 = row * 0.25f, v1 = v0 + 0.25f;
    const float hw = width * 0.5f, hh = width * 0.125f;
    for (int side = 0; side < 2; ++side) {
        const V3 n = out * (side ? -0.10f : 0.10f), a = along * (side ? -1.0f : 1.0f);
        b.TexQuad(c + n - a * hw - V3{0, 0, hh}, c + n + a * hw - V3{0, 0, hh}, c + n + a * hw + V3{0, 0, hh}, c + n - a * hw + V3{0, 0, hh}, kShopTex + tex, 0, 1, v0, v1);
    }
}

// A concrete balcony with a solid parapet, jutting out of the wall along `out`.
void Balcony(PropBuilder &b, V3 c, V3 along, V3 out, float width, int shade)
{
    static const uint8_t cols[3][3] = {{236, 232, 222}, {214, 200, 170}, {190, 190, 192}};
    const uint8_t *col = cols[shade % 3];
    const float depth = 1.25f;
    b.Box(c - along * (width * 0.5f) - out * 0.1f, along * width, out * (depth + 0.1f), {0, 0, 0.16f}, col); // slab
    b.Box(c + out * depth - along * (width * 0.5f) + V3{0, 0, 0.16f}, along * width, out * 0.08f, {0, 0, 0.95f}, col); // front parapet
    for (int s = -1; s <= 1; s += 2) // side parapets
        b.Box(c + along * (width * 0.5f * s) - out * 0.1f + V3{0, 0, 0.16f} - (s > 0 ? along * 0.08f : V3{0, 0, 0}), along * 0.08f, out * (depth + 0.1f), {0, 0, 0.95f}, col);
    if (shade % 3 == 1) // some have a sun shade stretched over them
        b.Box(c + V3{0, 0, 2.15f} - along * (width * 0.5f), along * width, out * (depth + 0.3f), {0, 0, 0.04f}, kStone);
}

// A roller shutter (closed) over a ground-floor opening: ribbed slats and its housing.
void Shutter(PropBuilder &b, V3 c, V3 along, V3 out, float width, float height, int variant)
{
    static const uint8_t housing[3] = {120, 122, 126};
    b.Box(c + V3{0, 0, height} - along * (width * 0.5f) - out * 0.02f, along * width, out * 0.34f, {0, 0, 0.3f}, housing);
    const V3 a = c - along * (width * 0.5f) + out * 0.09f, bb = c + along * (width * 0.5f) + out * 0.09f;
    b.TexQuad(a, bb, bb + V3{0, 0, height}, a + V3{0, 0, height}, kShutterTex + (variant & 3), 0, 1, 0, 1);
    b.TexQuad(bb, a, a + V3{0, 0, height}, bb + V3{0, 0, height}, kShutterTex + (variant & 3), 0, 1, 0, 1);
}

// A striped sun awning sloping out from the wall over a window or shop front.
void Awning(PropBuilder &b, V3 c, V3 along, V3 out, float width, int variant)
{
    const V3 a0 = c - along * (width * 0.5f), a1 = c + along * (width * 0.5f);
    const V3 b0 = a0 + out * 1.1f - V3{0, 0, 0.55f}, b1 = a1 + out * 1.1f - V3{0, 0, 0.55f};
    b.TexQuad(b0, b1, a1, a0, kAwningTex + (variant & 3), 0, 1, 0, 1);
    b.TexQuad(b1, b0, a0, a1, kAwningTex + (variant & 3), 0, 1, 0, 1);
    b.TexQuad(b0 - V3{0, 0, 0.25f}, b1 - V3{0, 0, 0.25f}, b1, b0, kAwningTex + (variant & 3), 0, 1, 0.85f, 1); // the valance
}

void Scooter(PropBuilder &b, V3 base, float yaw, Rng &r)
{
    const V3 u{std::cos(yaw), std::sin(yaw), 0}, v{-std::sin(yaw), std::cos(yaw), 0};
    static const uint8_t cols[3][3] = {{235, 235, 235}, {40, 40, 44}, {40, 190, 110}};
    const uint8_t *c = cols[int(r.f() * 2.99f)];
    b.Box(base - u * 0.28f - v * 0.07f + V3{0, 0, 0.1f}, u * 0.56f, v * 0.14f, {0, 0, 0.05f}, c);
    for (int i = 0; i < 2; ++i) {
        const V3 w = base + u * (i ? 0.3f : -0.3f) + V3{0, 0, 0.11f};
        b.Prism(w - v * 0.02f, w + v * 0.02f, 0.11f, 0.11f, 8, kDarkGrey);
    }
    const V3 top = base + u * 0.3f + V3{0, 0, 1.0f} - u * 0.12f;
    b.Prism(base + u * 0.3f + V3{0, 0, 0.1f}, top, 0.022f, 0.022f, 5, c);
    b.Prism(top - v * 0.23f, top + v * 0.23f, 0.018f, 0.018f, 5, kDarkGrey);
}

void CafeSet(PropBuilder &b, V3 base, float yaw, Rng &r)
{
    const V3 u{std::cos(yaw), std::sin(yaw), 0}, v{-std::sin(yaw), std::cos(yaw), 0};
    static const uint8_t umb[4][3] = {{236, 232, 220}, {196, 96, 62}, {40, 100, 160}, {70, 140, 90}};
    b.Solid(base.x, base.y, 0.95f, base.z, 1.0f);
    b.Prism(base + V3{0, 0, 0.74f}, base + V3{0, 0, 0.78f}, 0.38f, 0.38f, 10, kStone);
    b.Prism(base, base + V3{0, 0, 0.74f}, 0.04f, 0.04f, 5, kGrey);
    for (int i = 0; i < 4; ++i) {
        const float a = 1.5708f * i + 0.4f;
        const V3 p = base + (u * std::cos(a) + v * std::sin(a)) * 0.68f;
        b.UBox(p.x, p.y, base.z, 0.4f, 0.4f, 0.45f, yaw + a, kLightGrey);
        b.UBox(p.x + (std::cos(a)) * 0.15f * u.x, p.y, base.z + 0.45f, 0.4f, 0.05f, 0.4f, yaw + a, kLightGrey);
    }
    b.Prism(base, base + V3{0, 0, 2.3f}, 0.03f, 0.03f, 4, kGrey);
    b.Cone(base + V3{0, 0, 2.15f}, 1.45f, 0.5f, 8, umb[int(r.f() * 3.99f)]);
}
} // namespace


// ---- Bauhaus ("White City") buildings (ISSUES #279): replace suitable XB_* buildings of the Tel Aviv district with
// generated white blocks of the same footprint and height: ribbon windows under concrete eyebrows, a rounded corner,
// a glazed stair tower, a recessed penthouse, pilotis on some, and a parapeted roof with solar heaters. ----
namespace {
struct P2 {
    float x, y;
};

// vertical wall strips along a convex footprint polygon (counter-clockwise), from z0 to z1
void Walls(PropBuilder &b, const std::vector<P2> &poly, float z0, float z1, const uint8_t col[3])
{
    for (size_t i = 0; i < poly.size(); ++i) {
        const P2 &p = poly[i], &q = poly[(i + 1) % poly.size()];
        b.Quad({p.x, p.y, z0}, {q.x, q.y, z0}, {q.x, q.y, z1}, {p.x, p.y, z1}, col);
    }
}

void RoofFan(PropBuilder &b, const std::vector<P2> &poly, float z, const uint8_t col[3])
{
    P2 c{0, 0};
    for (const P2 &p : poly)
        c.x += p.x / float(poly.size()), c.y += p.y / float(poly.size());
    for (size_t i = 0; i < poly.size(); ++i) {
        const P2 &p = poly[i], &q = poly[(i + 1) % poly.size()];
        b.Tri({c.x, c.y, z}, {p.x, p.y, z}, {q.x, q.y, z}, col);
    }
}

// Ribbon windows, mullions and eyebrows on every edge of the footprint for the floors [f0, f1).
void Ribbons(PropBuilder &b, const std::vector<P2> &poly, float zBase, float fh, int f0, int f1, bool pilotisBelow)
{
    static const uint8_t glass[3] = {46, 78, 108}, brow[3] = {236, 234, 226}, mull[3] = {240, 240, 236};
    (void)pilotisBelow;
    for (size_t i = 0; i < poly.size(); ++i) {
        const P2 &p = poly[i], &q = poly[(i + 1) % poly.size()];
        const float len = std::hypot(q.x - p.x, q.y - p.y);
        if (len < 0.5f)
            continue;
        const V3 u{(q.x - p.x) / len, (q.y - p.y) / len, 0}, n{u.y, -u.x, 0}; // CCW polygon: the outward side is to the right
        const bool arc = len < 2.6f;
        const float inset = arc ? 0.0f : std::min(0.9f, len * 0.12f);
        for (int f = f0; f < f1; ++f) {
            const float zf = zBase + f * fh;
            const V3 a{p.x + u.x * inset, p.y + u.y * inset, zf + 0.9f}, c{q.x - u.x * inset, q.y - u.y * inset, zf + 0.9f};
            b.Quad(a + n * 0.03f, c + n * 0.03f, c + n * 0.03f + V3{0, 0, 1.45f}, a + n * 0.03f + V3{0, 0, 1.45f}, glass);
            // eyebrow shade over the window band
            b.Box(V3{p.x + u.x * (inset - 0.1f), p.y + u.y * (inset - 0.1f), zf + 2.38f}, u * (len - 2 * inset + 0.2f), n * (arc ? 0.45f : 0.55f), {0, 0, 0.1f}, brow);
            // mullions every ~2.4 m
            if (!arc) {
                const int m = int((len - 2 * inset) / 2.4f);
                for (int k = 1; k <= m; ++k) {
                    const float t = inset + (len - 2 * inset) * float(k) / float(m + 1);
                    const V3 base{p.x + u.x * t, p.y + u.y * t, zf + 0.9f};
                    b.Quad(base + n * 0.05f - u * 0.04f, base + n * 0.05f + u * 0.04f, base + n * 0.05f + u * 0.04f + V3{0, 0, 1.45f}, base + n * 0.05f - u * 0.04f + V3{0, 0, 1.45f}, mull);
                }
            }
        }
    }
}
} // namespace

bool TrackWorld::BauhausReplace(const char *name, const std::vector<SolidMesh::Triangle> &tris, const float *pos, const float *m)
{
    if (strncmp(name, "XB_", 3) != 0 || tris.empty())
        return false;
    static size_t cXB = 0, cRot = 0, cSize = 0, cDist = 0, cTall = 0;
    ++cXB;
    if (getenv("NFSU2_PROP_DEBUG") && cXB % 400 == 0)
        printf("[BauhausStats] XB %zu rotation-rejected %zu size-rejected %zu (tall %zu) district-rejected %zu\n", cXB, cRot, cSize, cTall, cDist);
    // the instance's oriented footprint from its own geometry: rows 0 / 1 of the matrix are the world directions of the solid's
    // x / y axes (with any scale); the solid must stand upright and not be sheared or mirrored upside down
    const float sx = std::hypot(m[0], m[1]), sy = std::hypot(m[3], m[4]);
    if (sx < 0.2f || sy < 0.2f || m[8] < 0.2f || std::fabs(m[0] * m[3] + m[1] * m[4]) > 0.14f * sx * sy)
        return ++cRot, false;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const SolidMesh::Triangle &t : tris)
        for (const SolidMesh::Point &p : t.v) {
            const float c[3] = {p.x, p.y, p.z};
            for (int k = 0; k < 3; ++k)
                lo[k] = std::min(lo[k], c[k]), hi[k] = std::max(hi[k], c[k]);
        }
    float W = (hi[0] - lo[0]) * sx, D = (hi[1] - lo[1]) * sy, H = (hi[2] - lo[2]) * m[8];
    if (W < 9.0f || D < 9.0f || W > 65.0f || D > 65.0f || H < 7.0f || H > 48.0f)
        return ++cSize, cTall += H > 48.0f, false;
    const float ux = m[0] / sx, uy = m[1] / sx, vx = m[3] / sy, vy = m[4] / sy; // unit axes in the world
    const float ox = pos[0] + lo[0] * m[0] + lo[1] * m[3], oy = pos[1] + lo[0] * m[1] + lo[1] * m[4]; // world position of the local corner
    const float cx = ox + ux * W * 0.5f + vx * D * 0.5f, cy = oy + uy * W * 0.5f + vy * D * 0.5f;
    if (DistrictAt(cx, cy).tlv < 0.26f)
        return ++cDist, false;
    // companions (window / neon overlays of the same building) share the footprint: build once, hide the rest
    const int64_t key = Key(int(cx * 0.5f), int(cy * 0.5f)) ^ (int64_t(int(W)) << 50) ^ (int64_t(int(D)) << 40);
    if (!m_bauhausDone.insert(key).second)
        return true;
    if (m_bauhaus.size() > 1200000)
        return false;
    if (getenv("NFSU2_PROP_DEBUG") && m_bauhausDone.size() % 10 == 1)
        printf("[BauhausAt] %s %.1f %.1f z %.1f W %.1f D %.1f H %.1f yaw %.0f\n", name, cx, cy, pos[2] + lo[2] * m[8], W, D, H, std::atan2(uy, ux) * 57.3f);

    // generated in the footprint's own frame (x along W, y along D, z absolute), then carried into the world
    std::vector<RTri> local;
    PropBuilder b{&local, 4000000, nullptr};
    Rng rng(HashXY(int(cx * 10), int(cy * 10), 77));
    const float x0 = 0.3f, y0 = 0.3f, x1 = W - 0.3f, y1 = D - 0.3f, zb = pos[2] + lo[2] * m[8], zt = zb + H;
    W = x1 - x0, D = y1 - y0, H = zt - zb;
    const int floors = std::max(2, int(H / 3.3f + 0.5f));
    const float fh = H / float(floors);
    static const uint8_t white[3] = {244, 242, 234}, roofCol[3] = {160, 160, 156}, parapet[3] = {238, 236, 228}, col2[3] = {232, 229, 220}, shop[3] = {36, 56, 78};

    // footprint: a rectangle, one corner rounded
    std::vector<P2> poly;
    const bool rounded = rng.f() < 0.75f;
    const int corner = int(rng.f() * 3.99f);
    const float R = std::min(4.5f, std::min(W, D) * 0.28f);
    const P2 rc[4] = {{x1, y0}, {x1, y1}, {x0, y1}, {x0, y0}}; // CCW corners starting at the lower right
    for (int i = 0; i < 4; ++i) {
        if (rounded && i == corner) {
            const P2 c = rc[i];
            const float sx = c.x > (x0 + x1) * 0.5f ? -1.0f : 1.0f, sy = c.y > (y0 + y1) * 0.5f ? -1.0f : 1.0f;
            const P2 ctr{c.x + sx * R, c.y + sy * R};
            // arc from the edge before to the edge after (CCW)
            const float a0 = std::atan2(-sy * 0.0f + (i % 2 ? 0.0f : 0.0f), 1.0f);
            (void)a0;
            const P2 from = i == 0 ? P2{c.x, c.y + R} : i == 1 ? P2{c.x - R, c.y} : i == 2 ? P2{c.x, c.y - R} : P2{c.x + R, c.y};
            const P2 to = i == 0 ? P2{c.x - R, c.y} : i == 1 ? P2{c.x, c.y - R} : i == 2 ? P2{c.x + R, c.y} : P2{c.x, c.y + R};
            // the previous edge ends at `to`'s counterpart; walk the quarter circle about ctr
            const float t0 = std::atan2(from.y - ctr.y, from.x - ctr.x), t1 = std::atan2(to.y - ctr.y, to.x - ctr.x);
            // CCW polygon: the outer arc is traversed from the incoming edge's end to the outgoing edge's start
            float d = t1 - t0;
            while (d > 3.14159f) d -= 6.28318f;
            while (d < -3.14159f) d += 6.28318f;
            // the incoming edge arrives at `to`, the outgoing leaves from `from` (see the rectangle order): start at `to`
            const float s0 = t1, s1 = t0;
            float dd = s1 - s0;
            while (dd > 3.14159f) dd -= 6.28318f;
            while (dd < -3.14159f) dd += 6.28318f;
            for (int k = 0; k <= 5; ++k)
                poly.push_back({ctr.x + R * std::cos(s0 + dd * float(k) / 5.0f), ctr.y + R * std::sin(s0 + dd * float(k) / 5.0f)});
        } else {
            poly.push_back(rc[i]);
        }
    }
    const bool pilotis = H > 10.0f && rng.f() < 0.25f;
    const bool penthouse = floors >= 4 && rng.f() < 0.45f;
    const int topFloors = penthouse ? floors - 1 : floors;
    const float zGround = zb + (pilotis ? fh : 0.0f);

    b.alpha = 255; // walls: ordinary triangles, so the world shader gives them the Tel Aviv render
    Walls(b, poly, zGround, zb + topFloors * fh, white);
    b.alpha = 254;
    // ground floor: shop fronts, or the open pilotis with its columns and recessed glazing
    if (pilotis) {
        for (size_t i = 0; i < poly.size(); ++i) {
            const P2 &p = poly[i], &q = poly[(i + 1) % poly.size()];
            const float len = std::hypot(q.x - p.x, q.y - p.y);
            if (len < 3.0f)
                continue;
            const int cols = std::max(2, int(len / 3.6f));
            for (int k = 0; k <= cols; ++k) {
                const float t = len * float(k) / float(cols);
                const V3 base{p.x + (q.x - p.x) / len * t, p.y + (q.y - p.y) / len * t, zb};
                b.Prism(base, base + V3{0, 0, fh}, 0.24f, 0.24f, 8, parapet);
            }
        }
    } else {
        b.alpha = 255;
        Walls(b, poly, zb, zb + fh, white);
        b.alpha = 254;
        for (size_t i = 0; i < poly.size(); ++i) { // shop windows
            const P2 &p = poly[i], &q = poly[(i + 1) % poly.size()];
            const float len = std::hypot(q.x - p.x, q.y - p.y);
            if (len < 3.0f)
                continue;
            const V3 u{(q.x - p.x) / len, (q.y - p.y) / len, 0}, n{u.y, -u.x, 0};
            const V3 a{p.x + u.x * 0.8f, p.y + u.y * 0.8f, zb + 0.35f}, c{q.x - u.x * 0.8f, q.y - u.y * 0.8f, zb + 0.35f};
            b.Quad(a + n * 0.03f, c + n * 0.03f, c + n * 0.03f + V3{0, 0, 2.55f}, a + n * 0.03f + V3{0, 0, 2.55f}, shop);
            b.Box(V3{p.x + u.x * 0.6f, p.y + u.y * 0.6f, zb + 2.95f}, u * (len - 1.2f), n * 0.9f, {0, 0, 0.12f}, col2);
        }
    }
    Ribbons(b, poly, zb, fh, 1, topFloors, pilotis);
    // recessed penthouse under a wide slab
    float roofZ = zb + topFloors * fh;
    std::vector<P2> top = poly;
    if (penthouse) {
        b.Box(V3{x0 - 0.5f, y0 - 0.5f, roofZ}, {W + 1.0f, 0, 0}, {0, D + 1.0f, 0}, {0, 0, 0.4f}, parapet);
        roofZ += 0.4f;
        const float in = std::min(2.6f, std::min(W, D) * 0.18f);
        top = {{x1 - in, y0 + in}, {x1 - in, y1 - in}, {x0 + in, y1 - in}, {x0 + in, y0 + in}};
        std::vector<P2> ccw = {{x1 - in, y0 + in}, {x1 - in, y1 - in}, {x0 + in, y1 - in}, {x0 + in, y0 + in}};
        b.alpha = 255;
        Walls(b, ccw, roofZ, roofZ + fh, white);
        b.alpha = 254;
        Ribbons(b, ccw, roofZ, fh, 0, 1, false);
        top = ccw;
        roofZ += fh;
    }
    // roof: flat slab, parapet ring
    RoofFan(b, top, roofZ + 0.02f, roofCol);
    for (size_t i = 0; i < top.size(); ++i) {
        const P2 &p = top[i], &q = top[(i + 1) % top.size()];
        const float len = std::hypot(q.x - p.x, q.y - p.y);
        if (len < 0.3f)
            continue;
        const V3 u{(q.x - p.x) / len, (q.y - p.y) / len, 0}, n{u.y, -u.x, 0};
        b.Box(V3{p.x, p.y, roofZ} - n * 0.12f, u * len, n * 0.12f, {0, 0, 0.55f}, parapet);
    }
    // glazed stair tower on one street-facing facade, rising above the roof
    if (rng.f() < 0.55f && W > 12.0f) {
        const float tx = x0 + W * (0.3f + 0.4f * rng.f()), tw = 2.2f;
        const bool front = rng.f() < 0.5f;
        const float ty = front ? y0 : y1, dir = front ? -1.0f : 1.0f;
        b.Box(V3{tx - tw * 0.5f, ty - (front ? 0.7f : 0.0f), zb}, {tw, 0, 0}, {0, 0.7f, 0}, {0, 0, zt - zb + 1.8f}, white);
        const float gy = ty + dir * 0.72f;
        b.Quad({tx - 0.55f, gy, zb + 1.0f}, {tx + 0.55f, gy, zb + 1.0f}, {tx + 0.55f, gy, roofZ + 1.2f}, {tx - 0.55f, gy, roofZ + 1.2f}, shop);
    }
    // roof furniture: solar heaters, condensers, tanks, aerials
    {
        float rx0 = 1e9f, ry0 = 1e9f, rx1 = -1e9f, ry1 = -1e9f;
        for (const P2 &p : top)
            rx0 = std::min(rx0, p.x), ry0 = std::min(ry0, p.y), rx1 = std::max(rx1, p.x), ry1 = std::max(ry1, p.y);
        const int items = 2 + int(rng.f() * (std::min(W, D) / 5.0f));
        for (int i = 0; i < items; ++i) {
            const V3 q{rx0 + 1.6f + (rx1 - rx0 - 3.2f) * rng.f(), ry0 + 1.6f + (ry1 - ry0 - 3.2f) * rng.f(), roofZ + 0.02f};
            const float k = rng.f();
            if (k < 0.5f)
                SolarHeater(b, q, 1.5708f * int(rng.f() * 3.99f));
            else if (k < 0.72f)
                Condenser(b, q, rng.f() * 6.28f, false, {0, 0, 1});
            else if (k < 0.86f)
                Antenna(b, q, rng.f() * 3.14f);
            else
                WaterTank(b, q);
        }
    }
    // carry the local geometry to the world: (x, y) -> origin + u x + v y
    for (RTri &t : local)
        for (RVertex &v : t.v) {
            const float lx = v.x, ly = v.y;
            v.x = ox + ux * lx + vx * ly, v.y = oy + uy * lx + vy * ly;
        }
    m_bauhaus.insert(m_bauhaus.end(), local.begin(), local.end());
    return true;
}

void TrackWorld::FlushBauhaus()
{
    for (const RTri &p : m_bauhaus) {
        const float cx = (p.v[0].x + p.v[1].x + p.v[2].x) / 3.0f, cy = (p.v[0].y + p.v[1].y + p.v[2].y) / 3.0f;
        m_cells[CellKey(int(std::floor(cx / kCellSize)), int(std::floor(cy / kCellSize)))].render.push_back(p);
    }
    if (!m_bauhaus.empty())
        printf("[Bauhaus] %zu buildings replaced, %zu triangles\n", m_bauhausDone.size(), m_bauhaus.size());
    m_bauhaus.clear();
    m_bauhaus.shrink_to_fit();
}

void TrackWorld::AddIsraeliProps()
{
    if (getenv("NFSU2_NO_PROPS") || m_scene)
        return;
    const unsigned t0 = SDL_GetTicks();
    unsigned gTicks = 0, gCalls = 0;
    const int savedSource = m_groundSource;
    if (!getenv("NFSU2_PROP_SLOWGROUND") && m_collisionFaces)
        m_groundSource = 2; // the collision mesh answers ground queries far faster than the render triangles
    // Generated Hebrew signage atlases (512 x 512, four 512 x 128 rows each) and the flag, registered as runtime textures.
    g_signs = !getenv("NFSU2_NO_SIGNS") && SignGen::Init();
    if (g_signs) {
        auto atlas = [&](int kind, int count, uint32_t base) {
            for (int t = 0; t * 4 < count; ++t) {
                std::vector<uint32_t> tex(512 * 512, 0xff000000u), row;
                for (int r = 0; r < 4; ++r)
                    if (SignGen::Make(kind, t * 4 + r, 512, 128, row))
                        std::copy(row.begin(), row.end(), tex.begin() + size_t(r) * 512 * 128);
                m_skinPixels[base + t] = std::move(tex);
            }
        };
        atlas(0, 16, kShopTex);
        atlas(1, 12, kStreetTex);
        std::vector<uint32_t> flag;
        if (SignGen::Make(2, 0, 512, 512, flag))
            m_skinPixels[kFlagTex] = std::move(flag);
        for (int v = 0; v < 4; ++v) {
            std::vector<uint32_t> t;
            if (SignGen::Make(3, v, 512, 512, t))
                m_skinPixels[kShutterTex + v] = std::move(t);
            if (SignGen::Make(4, v, 512, 512, t))
                m_skinPixels[kAwningTex + v] = std::move(t);
        }
    }
    const size_t propCap = getenv("NFSU2_PROP_CAP") ? size_t(atol(getenv("NFSU2_PROP_CAP"))) : 1600000;

    // Road proximity: 4 m cells hold road triangle centroids; 8 m cells (and their ring) mark "near a road".
    std::unordered_set<int64_t> road4;
    std::unordered_map<int64_t, std::pair<float, float>> near8; // 8 m cell (and its ring) -> lowest / highest road height
    for (size_t i = 0; i + 5 < m_roadTris.size(); i += 6) {
        const float cx = (m_roadTris[i] + m_roadTris[i + 2] + m_roadTris[i + 4]) / 3.0f;
        const float cy = (m_roadTris[i + 1] + m_roadTris[i + 3] + m_roadTris[i + 5]) / 3.0f;
        road4.insert(Key(int(std::floor(cx / 4.0f)), int(std::floor(cy / 4.0f))));
        const int gx = int(std::floor(cx / 8.0f)), gy = int(std::floor(cy / 8.0f));
        const float rz = i / 6 < m_roadTriZ.size() ? m_roadTriZ[i / 6] : 0.0f;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                auto ins = near8.insert({Key(gx + dx, gy + dy), {rz, rz}});
                if (!ins.second)
                    ins.first->second = {std::min(ins.first->second.first, rz), std::max(ins.first->second.second, rz)};
            }
    }

    std::vector<RTri> props;
    props.reserve(200000);
    std::vector<PropSolid> solids;
    PropBuilder b{&props, propCap, &solids};
    std::unordered_set<int64_t> busCells, signCells;
    const size_t kRoofBudget = 220000, kWallBudget = 520000;
    size_t roofTris = 0, wallTris = 0;
    size_t flatTris = 0, flatGround = 0, flatRoad = 0, flatNear = 0, flatNoGround = 0;
    size_t nShopSigns = 0, nStreetSigns = 0, nFlags = 0, nBalconies = 0, nShutters = 0, nAwnings = 0, facadeTris = 0;
    const size_t kFacadeBudget = 260000;
    std::unordered_set<int64_t> streetCells;
    // Shop signs only go on storefront facades (not freeway walls): by texture name.
    std::unordered_map<uint32_t, bool> storefront;
    auto isStorefront = [&](uint32_t hash) {
        auto it = storefront.find(hash);
        if (it != storefront.end())
            return it->second;
        bool ok = false;
        auto src = m_textures.find(hash);
        if (src != m_textures.end()) {
            const std::string &n = src->second.first->At(src->second.second).name;
            for (const char *k : {"STORE", "SHOP", "FRONT", "MALL", "CAFE", "MARKET", "DELI", "BAKERY", "BOUTIQUE", "RESTAURANT", "PLAZA"})
                ok |= n.find(k) != std::string::npos;
        }
        return storefront[hash] = ok;
    };
    size_t nTrees = 0, nRoofs = 0, nWalls = 0, nCurbs = 0, nBike = 0, nBus = 0, nSigns = 0, nScoot = 0, nCafe = 0;

    auto GroundHeightT = [&](float x, float y, float zmax, float &z) {
        const unsigned a0 = SDL_GetTicks();
        const bool r = GroundHeight(x, y, zmax, z);
        gTicks += SDL_GetTicks() - a0, ++gCalls;
        return r;
    };
    for (auto &entry : m_cells) {
        for (RTri &t : entry.second.render) {
            if (props.size() >= propCap)
                break;
            const V3 p0{t.v[0].x, t.v[0].y, t.v[0].z}, p1{t.v[1].x, t.v[1].y, t.v[1].z}, p2{t.v[2].x, t.v[2].y, t.v[2].z};
            const V3 nn = Cross(p1 - p0, p2 - p0);
            const float len = Len(nn);
            if (len < 1e-4f || t.tex == 0)
                continue;
            if (t.v[0].a != 255 || t.v[1].a != 255 || t.v[2].a != 255)
                continue;
            const float area = len * 0.5f, nz = std::fabs(nn.z) / len;
            const V3 c = (p0 + p1 + p2) * (1.0f / 3.0f);
            const float minz = std::min({p0.z, p1.z, p2.z}), maxz = std::max({p0.z, p1.z, p2.z});
            const int c4x = int(std::floor(c.x / 4.0f)), c4y = int(std::floor(c.y / 4.0f));
            Rng rng(HashXY(int(c.x * 10), int(c.y * 10), uint32_t(c.z * 7)));
            auto inTri = [&](float shrink) {
                float a = rng.f(), bb = rng.f();
                if (a + bb > 1.0f)
                    a = 1.0f - a, bb = 1.0f - bb;
                const V3 q = p0 + (p1 - p0) * a + (p2 - p0) * bb;
                return c + (q - c) * shrink;
            };
            const auto nearIt = near8.find(Key(int(std::floor(c.x / 8.0f)), int(std::floor(c.y / 8.0f))));
            const bool nearRoad = nearIt != near8.end();
            // at road height (kerb-high pavements, plazas): within 0.6 m below / 1.2 m above the nearby road surface
            const bool atRoadLevel = nearRoad && c.z > nearIt->second.first - 0.6f && c.z < nearIt->second.second + 1.2f;

            if (nz > 0.9f) {
                if (area < 1.5f)
                    continue;
                // Cheap rejects before the (slow) ground query: road triangles are neither roofs nor pavements, and only
                // big triangles can be roofs while only triangles near a road can be pavements.
                if ((area < 8.0f && road4.count(Key(c4x, c4y))) || (!nearRoad && area < 8.0f))
                    continue;
                float g;
                // the surface itself (within 0.7 m below) = ground level; otherwise the ground under a roof
                if (GroundHeightT(c.x, c.y, c.z + 0.3f, g) && std::fabs(c.z - g) >= 0.7f && !GroundHeightT(c.x, c.y, c.z - 2.0f, g))
                    g = c.z - 100.0f;
                else if (!GroundHeightT(c.x, c.y, c.z + 0.3f, g))
                    g = c.z - 100.0f;
                const float h = c.z - g;
                ++flatTris;
                if (std::fabs(h) < 0.7f) { ++flatGround; if (road4.count(Key(c4x, c4y))) ++flatRoad; else if (nearRoad) ++flatNear; }
                if (h > 5.0f && h < 140.0f && area > 8.0f && roofTris < kRoofBudget) { // rooftop dressing
                    const size_t before = props.size();
                    const District d = DistrictAt(c.x, c.y);
                    const float dens = area / 85.0f * (1.0f + 0.4f * d.jer + 0.3f * d.hai);
                    int n = int(dens + rng.f());
                    for (int i = 0; i < n && props.size() < propCap; ++i) {
                        const V3 q = inTri(0.7f);
                        const float k = rng.f();
                        if (k < 0.5f)
                            SolarHeater(b, q, 1.5708f * int(rng.f() * 3.99f)), ++nRoofs;
                        else if (k < 0.72f)
                            Condenser(b, q, rng.f() * 6.28f, false, {0, 0, 1}), ++nRoofs;
                        else if (k < 0.88f)
                            Antenna(b, q, rng.f() * 3.14f), ++nRoofs;
                        else if (k < 0.94f || !g_signs)
                            WaterTank(b, q), ++nRoofs;
                        else
                            FlagPole(b, q, rng.f() * 6.28f, 3.0f + 1.5f * rng.f()), ++nFlags;
                    }
                    roofTris += props.size() - before;
                } else if (atRoadLevel && !road4.count(Key(c4x, c4y)) && t.tex) { // pavement / plaza
                    const District d = DistrictAt(c.x, c.y);
                    const V3 e1 = p1 - p0, e2 = p2 - p1, e3 = p0 - p2;
                    V3 ed = e1;
                    if (Len(e2) > Len(ed)) ed = e2;
                    if (Len(e3) > Len(ed)) ed = e3;
                    const float alt = 2.0f * area / std::max(Len(ed), 0.01f);
                    const float yawE = std::atan2(ed.y, ed.x);
                    // Tel Aviv: green bike lane on the narrow pavement strips
                    if (d.tlv > 0.3f && alt < 3.4f && alt > 0.8f && (HashXY(c4x / 5, c4y / 5, 3) & 3) < 2) {
                        for (RVertex &v : t.v)
                            v.a = 252;
                        ++nBike;
                    }
                    // trees (kind by district)
                    if (area > 3.0f && rng.f() < area / 300.0f) {
                        const V3 q = inTri(0.6f);
                        const float k = rng.f();
                        if (d.hai > 0.45f)
                            k < 0.75f ? Pine(b, q, rng) : Cypress(b, q, rng);
                        else if (d.jer > 0.45f)
                            k < 0.45f ? Cypress(b, q, rng) : Olive(b, q, rng);
                        else if (d.bsh > 0.45f)
                            k < 0.8f ? Palm(b, q, rng) : Olive(b, q, rng);
                        else
                            k < 0.6f ? Palm(b, q, rng) : Olive(b, q, rng);
                        ++nTrees;
                    }
                    if (area > 4.0f && rng.f() < area / 2200.0f && busCells.insert(Key(c4x / 12, c4y / 12)).second) {
                        BusStop(b, inTri(0.5f), yawE), ++nBus;
                    }
                    if (g_signs && area > 3.0f && rng.f() < area / 1500.0f && streetCells.insert(Key(c4x / 15, c4y / 15)).second) {
                        StreetSign(b, inTri(0.6f), yawE + 1.5708f * int(rng.f() * 3.99f), int(rng.f() * 11.99f)), ++nStreetSigns;
                    }
                    if (g_signs && area > 3.0f && rng.f() < area / 2600.0f) {
                        FlagPole(b, inTri(0.6f), yawE + 1.5708f * int(rng.f() * 3.99f), 6.0f + 2.0f * rng.f()), ++nFlags;
                    }
                    if (rng.f() < area / 800.0f && signCells.insert(Key(c4x / 6, c4y / 6)).second) {
                        RoadSign(b, inTri(0.6f), yawE + 1.5708f * int(rng.f() * 3.99f), int(rng.f() * 3.99f)), ++nSigns;
                    }
                    if (rng.f() < area / 1500.0f * d.tlv * 1.6f) {
                        const int n = 1 + int(rng.f() * 3.0f);
                        for (int i = 0; i < n; ++i)
                            Scooter(b, inTri(0.7f), yawE + (rng.f() - 0.5f) * 0.6f, rng), ++nScoot;
                    }
                    if (area > 6.0f && rng.f() < area / 1500.0f * d.tlv) {
                        const V3 q = inTri(0.5f);
                        CafeSet(b, q, yawE, rng);
                        if (rng.f() < 0.7f)
                            CafeSet(b, q + V3{std::cos(yawE), std::sin(yawE), 0} * 2.6f, yawE, rng);
                        ++nCafe;
                    }
                }
            } else if (nz < 0.2f) {
                const float dz = maxz - minz;
                V3 hz{nn.x, nn.y, 0};
                const float hl = Len(hz);
                if (hl < 1e-4f)
                    continue;
                hz = hz * (1.0f / hl);
                if (nearRoad) {
                    if (g_signs && nShopSigns < 3500 && isStorefront(t.tex) && area > 4.0f && dz > 2.2f && rng.f() < area / 12.0f) { // hung above the entrance at street level
                        const V3 q = inTri(0.7f);
                        const float zs = nearIt->second.first + 3.3f;
                        if (zs > minz + 0.4f && zs < maxz - 0.4f) {
                            ShopSign(b, V3{q.x, q.y, zs}, V3{-hz.y, hz.x, 0}, hz, int(rng.f() * 15.99f), 2.4f + 1.6f * rng.f());
                            if (getenv("NFSU2_PROP_DEBUG") && nShopSigns < 40 && nShopSigns % 8 == 0)
                                printf("[ShopSign] %.1f %.1f %.1f out %.2f %.2f\n", q.x, q.y, zs, hz.x, hz.y);
                            ++nShopSigns;
                        }
                    }
                }
                // kerb face: low, long, beside a road
                if (dz > 0.06f && dz < 0.5f && nearRoad) {
                    const V3 e1 = p1 - p0, e2 = p2 - p1, e3 = p0 - p2;
                    const float l1 = std::hypot(e1.x, e1.y), l2 = std::hypot(e2.x, e2.y), l3 = std::hypot(e3.x, e3.y);
                    if (std::max({l1, l2, l3}) > 1.0f) {
                        if (minz > nearIt->second.first - 0.7f && minz < nearIt->second.second + 0.7f) {
                            for (RVertex &v : t.v)
                                v.a = 253;
                            ++nCurbs;
                        }
                    }
                } else if (dz > 3.0f && area > 12.0f && nearRoad && wallTris < kWallBudget) { // facade air conditioners
                    const size_t before = props.size();
                    float g;
                    if (!GroundHeightT(c.x, c.y, minz + 1.0f, g))
                        g = minz;
                    const float dens = area / 34.0f;
                    int n = int(dens + rng.f());
                    for (int i = 0; i < n && props.size() < propCap; ++i) {
                        const V3 q = inTri(0.8f);
                        if (q.z - g < 2.4f || rng.f() > 0.6f)
                            continue;
                        const float yaw = std::atan2(hz.y, hz.x) - 1.5708f;
                        Condenser(b, V3{q.x, q.y, q.z - 0.3f}, yaw, true, hz);
                        ++nWalls;
                    }
                    wallTris += props.size() - before;
                    // Israeli facade dressing: balconies on the upper floors, roller shutters and awnings at street level.
                    if (g_signs && facadeTris < kFacadeBudget) {
                        const size_t fbefore = props.size();
                        // which side of the wall is the street? the first road met walking out each way
                        auto reach = [&](float sgn) {
                            for (int d = 3; d <= 18; d += 3)
                                if (road4.count(Key(int(std::floor((c.x + hz.x * sgn * d) / 4.0f)), int(std::floor((c.y + hz.y * sgn * d) / 4.0f)))))
                                    return d;
                            return 99;
                        };
                        const int ra = reach(1.0f), rb = reach(-1.0f);
                        if (ra != rb && std::min(ra, rb) < 99) {
                            const V3 out = hz * (ra < rb ? 1.0f : -1.0f);
                            const V3 along = Norm(V3{-out.y, out.x, 0});
                            const float base = nearIt->second.first;
                            const int nb = int(area / 55.0f + rng.f());
                            for (int i = 0; i < nb; ++i) { // balconies
                                const V3 q = inTri(0.75f);
                                const int k = 1 + int(rng.f() * 4.0f);
                                const float zb = base + 3.4f * float(k);
                                if (zb > minz + 0.3f && zb < maxz - 1.4f && rng.f() < 0.8f) {
                                    Balcony(b, V3{q.x, q.y, zb}, along, out, 2.2f + 1.6f * rng.f(), int(rng.f() * 2.99f));
                                    if (getenv("NFSU2_PROP_DEBUG") && nBalconies % 400 == 0)
                                        printf("[Balcony] %.1f %.1f %.1f out %.2f %.2f\n", q.x, q.y, zb, out.x, out.y);
                                    ++nBalconies;
                                }
                            }
                            if (minz < base + 1.2f && maxz > base + 2.8f) { // ground floor: shutters and awnings
                                const int ns = int(area / 40.0f + rng.f());
                                for (int i = 0; i < ns; ++i) {
                                    const V3 q = inTri(0.75f);
                                    const float r = rng.f();
                                    if (r < 0.45f) {
                                        Shutter(b, V3{q.x, q.y, base}, along, out, 1.8f + 1.2f * rng.f(), 2.2f, int(rng.f() * 3.99f)), ++nShutters;
                                    } else if (r < 0.8f) {
                                        Awning(b, V3{q.x, q.y, base + 2.7f}, along, out, 2.0f + 1.5f * rng.f(), int(rng.f() * 3.99f)), ++nAwnings;
                                    }
                                }
                            }
                        }
                        facadeTris += props.size() - fbefore;
                    }
                }
            }
        }
    }

    // Solid props join the wall query (ISSUES #275).
    m_propSolids = std::move(solids);
    m_propGrid.clear();
    for (uint32_t i = 0; i < m_propSolids.size(); ++i)
        m_propGrid[CellKey(int(std::floor(m_propSolids[i].x / 16.0f)), int(std::floor(m_propSolids[i].y / 16.0f)))].push_back(i);
    if (getenv("NFSU2_PROP_DEBUG"))
        for (size_t i = 0; i < m_propSolids.size(); i += std::max<size_t>(1, m_propSolids.size() / 8))
            printf("[PropSolid] %.1f %.1f r %.2f z %.1f..%.1f\n", m_propSolids[i].x, m_propSolids[i].y, m_propSolids[i].r, m_propSolids[i].z0, m_propSolids[i].z1);
    // Distribute the props into their 64 m cells.
    for (const RTri &p : props) {
        const float cx = (p.v[0].x + p.v[1].x + p.v[2].x) / 3.0f, cy = (p.v[0].y + p.v[1].y + p.v[2].y) / 3.0f;
        m_cells[CellKey(int(std::floor(cx / kCellSize)), int(std::floor(cy / kCellSize)))].render.push_back(p);
    }
    printf("[Props] flat %zu, ground-level %zu (road %zu, near-road non-road %zu)\n", flatTris, flatGround, flatRoad, flatNear);
    m_groundSource = savedSource;
    printf("[Props] built in %u ms (%u ground queries, %u ms)\n", SDL_GetTicks() - t0, gCalls, gTicks);
    printf("[Props] %zu triangles: %zu trees, %zu rooftop items, %zu facade AC, %zu kerb faces, %zu bike-lane strips, %zu bus stops, "
           "%zu signs, %zu scooters, %zu cafes, %zu Hebrew shop signs, %zu street-name signs, %zu flags, %zu balconies, %zu shutters, %zu awnings\n",
           props.size(), nTrees, nRoofs, nWalls, nCurbs, nBike, nBus, nSigns, nScoot, nCafe, nShopSigns, nStreetSigns, nFlags, nBalconies, nShutters, nAwnings);
}
