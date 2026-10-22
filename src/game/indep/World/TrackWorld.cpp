#include <tuple>
#include <map>
#include "game/indep/World/TrackWorld.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "game/indep/Texture/TexturePack.h"
#include "game/indep/World/CarMaterial.h"
#include "game/indep/World/SolidMesh.h"

namespace {
constexpr uint32_t kChunkSolidList = 0x80134000;
constexpr uint32_t kChunkSolid = 0x80134010;
constexpr uint32_t kChunkSolidHeader = 0x134011; // name hash at +0x10
constexpr uint32_t kChunkScenerySection = 0x80034100;
constexpr uint32_t kChunkSceneryInfos = 0x34102;     // 68-byte records, name hash at +0x20
constexpr uint32_t kChunkSceneryInstances = 0x34103; // 64-byte records after a 0x11 pad
constexpr size_t kInfoSize = 68;
constexpr size_t kInstanceSize = 64;

uint32_t Read32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

template <typename F> void ForEachChunk(const uint8_t *d, size_t begin, size_t end, F &&f)
{
    for (size_t p = begin; p + 8 <= end;) {
        uint32_t id = Read32(d + p), len = Read32(d + p + 4);
        if (len > end - (p + 8))
            return;
        f(id, p + 8, size_t(len));
        p += 8 + len;
    }
}

enum Kind : uint8_t { kOther, kRoad, kTerrain, kBuilding, kSky, kBarrier, kPanorama };

Kind KindOf(const char *name)
{
    if (strncmp(name, "SKYDOME", 7) == 0) // the sky dome solid is not in the region bundles
        return kSky;
    if (strncmp(name, "PAN_", 4) == 0) // distant panorama backdrop (skyline, hills, hotels)
        return kPanorama;
    if (strstr(name, "TrackBarrier"))
        return kBarrier;
    if (strstr(name, "Road") || strstr(name, "ROAD"))
        return kRoad;
    if (strstr(name, "Terrain") || strstr(name, "TERRAIN") || strstr(name, "TRN_"))
        return kTerrain;
    if (strstr(name, "BLD") || strstr(name, "Bld") || strstr(name, "Building"))
        return kBuilding;
    return kOther;
}

void BaseColour(Kind k, float &r, float &g, float &b)
{
    switch (k) {
    case kRoad: r = 0.42f, g = 0.42f, b = 0.46f; break;
    case kTerrain: r = 0.34f, g = 0.44f, b = 0.30f; break;
    case kBuilding: r = 0.62f, g = 0.56f, b = 0.48f; break;
    default: r = 0.55f, g = 0.55f, b = 0.60f; break;
    }
}
} // namespace

bool TrackWorld::Shade(Tri &w, float r, float g, float b)
{
    constexpr float lx = 0.40f, ly = 0.30f, lz = 0.866f; // light direction (normalised)
    float ux = w.v[3] - w.v[0], uy = w.v[4] - w.v[1], uz = w.v[5] - w.v[2];
    float vx = w.v[6] - w.v[0], vy = w.v[7] - w.v[1], vz = w.v[8] - w.v[2];
    float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len < 1e-6f)
        return false;
    float lit = 0.35f + 0.65f * std::fabs((nx * lx + ny * ly + nz * lz) / len);
    w.ground = std::fabs(nz) > 0.8f * len;
    w.shade[0] = uint8_t(std::min(255.0f, r * lit * 255.0f));
    w.shade[1] = uint8_t(std::min(255.0f, g * lit * 255.0f));
    w.shade[2] = uint8_t(std::min(255.0f, b * lit * 255.0f));
    return true;
}

template <typename F> void TrackWorld::ForNearbyTris(float x, float y, F &&f) const
{
    // Triangles are bucketed by centroid, so a large one can overlap the neighbouring cells.
    const int gx = int(std::floor(x / kCellSize)), gy = int(std::floor(y / kCellSize));
    for (int cx = gx - 1; cx <= gx + 1; ++cx)
        for (int cy = gy - 1; cy <= gy + 1; ++cy) {
            auto it = m_cells.find(CellKey(cx, cy));
            if (it != m_cells.end())
                for (const Tri &t : it->second.tris)
                    if (t.phys)
                        f(t);
        }
}

template <typename F> void TrackWorld::ForNearbyCollision(float x, float y, F &&f) const
{
    const int gx = int(std::floor(x / kCellSize)), gy = int(std::floor(y / kCellSize));
    for (int cx = gx - 1; cx <= gx + 1; ++cx)
        for (int cy = gy - 1; cy <= gy + 1; ++cy) {
            auto it = m_collision.find(CellKey(cx, cy));
            if (it != m_collision.end())
                for (const CTri &t : it->second)
                    f(t);
        }
}

// 0x00336180 (ground query) over the collision mesh. The game looks the point up in the section's
// 32 m cell, walks the cell's face list (filtered by the 8 m sub-cell mask), skips faces of a barrier
// group that is not enabled (face byte 0) and the wall faces (flags & 0x0a), keeps those whose XY
// footprint holds the point (0x00335328: every edge function <= 0, i.e. an upward-facing,
// counter-clockwise face -- a ceiling or an underside never passes) and takes the highest one at or
// below the query height. A face's height is (n.v0 - nx x - ny y) / nz. Group faces stay off: the
// per-race enable table (DAT_00523ddc) is not ported.
void TrackWorld::DumpWalls(float x, float y, float r) const
{
    for (const CSection &sec : m_csections)
        for (const CFace &f : sec.faces) {
            if (!(f.flags & 0x02))
                continue;
            float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f, minz = 1e30f, maxz = -1e30f;
            for (int k = 0; k < (f.quad ? 4 : 3); ++k) {
                minx = std::min(minx, f.v[k * 3]), maxx = std::max(maxx, f.v[k * 3]);
                miny = std::min(miny, f.v[k * 3 + 1]), maxy = std::max(maxy, f.v[k * 3 + 1]);
                minz = std::min(minz, f.v[k * 3 + 2]), maxz = std::max(maxz, f.v[k * 3 + 2]);
            }
            if (maxx < x - r || minx > x + r || maxy < y - r || miny > y + r)
                continue;
            printf("[WallFace] g%u fl%02x s%u  (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f) z %.1f..%.1f n(%.2f,%.2f)\n", f.group, f.flags,
                   f.surface, f.v[0], f.v[1], f.v[3], f.v[4], f.v[6], f.v[7], f.v[9], f.v[10], minz, maxz, f.n[0], f.n[1]);
        }
}

bool TrackWorld::CollisionGround(float x, float y, float zq, float &z, float *normal) const
{
    const auto it = m_csectionGrid.find(CellKey(int(std::floor(x / kCellSize)), int(std::floor(y / kCellSize))));
    if (it == m_csectionGrid.end())
        return false;
    bool found = false;
    float best = -1e30f;
    for (uint32_t si : it->second) {
        const CSection &sec = m_csections[si];
        if (x < sec.ox || y < sec.oy || x >= sec.ox + sec.cols * 32.0f || y >= sec.oy + sec.rows * 32.0f)
            continue;
        const int col = std::clamp(int((x - sec.ox) * 0.03125f), 0, sec.cols - 1);
        const int row = std::clamp(int((y - sec.oy) * 0.03125f), 0, sec.rows - 1);
        const uint32_t cell = sec.cells[size_t(row) * sec.cols + col];
        const uint32_t first = cell & 0xffff, count = cell >> 16;
        const int xq = int((x - (sec.ox + col * 32.0f)) * 0.125f) & 31;
        const int yq = int((y - (sec.oy + row * 32.0f)) * 0.125f) & 7;
        const uint32_t want = (1u << xq) << (yq << 2);
        for (uint32_t k = first; k < first + count && k < sec.index.size(); ++k) {
            if (!((sec.index[k] >> 16) & want))
                continue;
            const uint32_t fi = sec.index[k] & 0xffff;
            if (fi >= sec.faces.size())
                continue;
            const CFace &f = sec.faces[fi];
            if (!GroupEnabled(f.group) || (f.flags & 0x0a) || f.n[2] < 1e-4f)
                continue;
            // 0x00335328: max of the edge functions of v0->v1, v1->v2, [v2->v3,] v3->v0 (a triangle's
            // last vertex repeats v2, so its closing edge is v2->v0).
            auto edge = [&](int a, int b) {
                return (f.v[b * 3 + 1] - f.v[a * 3 + 1]) * (x - f.v[a * 3]) -
                       (f.v[b * 3] - f.v[a * 3]) * (y - f.v[a * 3 + 1]);
            };
            float e = std::max(edge(0, 1), edge(1, 2));
            if (f.quad)
                e = std::max({e, edge(2, 3), edge(3, 0)});
            else
                e = std::max(e, edge(2, 0));
            if (e > 0.0f)
                continue;
            const float h = (f.d - f.n[0] * x - f.n[1] * y) / f.n[2];
            if (h <= zq && h > best) {
                best = h;
                found = true;
                if (normal)
                    normal[0] = f.n[0], normal[1] = f.n[1], normal[2] = f.n[2];
            }
        }
    }
    if (found)
        z = best;
    return found;
}

bool TrackWorld::GroundHeight(float x, float y, float zMax, float &z, float *normal) const
{
    // The game's own surface first (the collision mesh); the render triangles only where it has no
    // face at all (the ISSUES #133 hybrid let a tunnel roof count as ground, #145).
    if (m_groundSource != 1 && CollisionGround(x, y, zMax, z, normal))
        return true;
    if (m_groundSource == 2)
        return false;
    bool found = false;
    float best = -1e30f;
    float bestN[3] = {0, 0, 1};
    auto test = [&](const float *a, const float *b, const float *c) {
        // Quick reject on the XY bounds.
        if (x < std::min({a[0], b[0], c[0]}) || x > std::max({a[0], b[0], c[0]}) ||
            y < std::min({a[1], b[1], c[1]}) || y > std::max({a[1], b[1], c[1]}))
            return;
        float d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
        if (std::fabs(d) < 1e-6f)
            return; // vertical in XY
        float l0 = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / d;
        float l1 = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / d;
        float l2 = 1.0f - l0 - l1;
        if (l0 < 0 || l1 < 0 || l2 < 0)
            return;
        // Walkable: the normal points mostly up or down (winding is unknown).
        float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
        float vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        if (nz * nz < 0.5f * (nx * nx + ny * ny + nz * nz))
            return;
        float h = l0 * a[2] + l1 * b[2] + l2 * c[2];
        if (h <= zMax && h > best) {
            best = h;
            found = true;
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz), sgn = nz < 0 ? -1.0f : 1.0f;
            bestN[0] = sgn * nx / len, bestN[1] = sgn * ny / len, bestN[2] = sgn * nz / len;
        }
    };
    // SetGroundSource (dev aid): restrict the ground query to the render triangles or the collision mesh.
    const bool useRender = true, useColl = false; // collision faces are queried exactly above
    if (useRender)
        ForNearbyTris(x, y, [&](const Tri &t) { test(t.v, t.v + 3, t.v + 6); });
    // The collision mesh: faces are triangles, or quads (flags & 0x10) split v0-v1-v2 / v0-v2-v3.
    if (useColl)
        ForNearbyCollision(x, y, [&](const CTri &t) {
            test(t.v, t.v + 3, t.v + 6);
            if (t.flags & 0x10)
                test(t.v, t.v + 6, t.v + 9);
        });
    if (found) {
        z = best;
        if (normal)
            normal[0] = bestN[0], normal[1] = bestN[1], normal[2] = bestN[2];
    }
    return found;
}

// Chunk 0x3412f (see the header): the barrier-group records.
void TrackWorld::LoadBarrierGroups(const std::vector<uint8_t> &bundle)
{
    m_groupNames.clear();
    m_groupOn.clear();
    std::function<void(size_t, size_t)> walk = [&](size_t begin, size_t end) {
        ForEachChunk(bundle.data(), begin, end, [&](uint32_t id, size_t body, size_t len) {
            if (id == 0x3412f) {
                for (size_t o = 0; o + 32 <= len; o += 32) {
                    const uint8_t *r = bundle.data() + body + o;
                    m_groupOn.push_back(r[8]);
                    m_groupNames.emplace_back(reinterpret_cast<const char *>(r + 9),
                                              strnlen(reinterpret_cast<const char *>(r + 9), 23));
                }
            } else if (id & 0x80000000) {
                walk(body, body + len);
            }
        });
    };
    walk(0, bundle.size());
    printf("[World] %zu barrier groups\n", m_groupNames.size());
}

// Segment a->b against the wall faces of the collision mesh (flags & 0x02, group 0). A wall face is
// a vertical polygon; flag 0x04 makes it unbounded in Z (the decoder 0x00335098 sets its Z to
// -2048 / +2048). Returns the face normal facing the segment start.
bool TrackWorld::CollisionWall(const float *a, const float *b, float *normal, int onlyGroup) const
{
    const float lo[2] = {std::min(a[0], b[0]) - 0.5f, std::min(a[1], b[1]) - 0.5f};
    const float hi[2] = {std::max(a[0], b[0]) + 0.5f, std::max(a[1], b[1]) + 0.5f};
    const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    for (int gx = int(std::floor(lo[0] / kCellSize)); gx <= int(std::floor(hi[0] / kCellSize)); ++gx)
        for (int gy = int(std::floor(lo[1] / kCellSize)); gy <= int(std::floor(hi[1] / kCellSize)); ++gy) {
            const auto it = m_csectionGrid.find(CellKey(gx, gy));
            if (it == m_csectionGrid.end())
                continue;
            for (uint32_t si : it->second) {
                const CSection &sec = m_csections[si];
                const int c0 = std::clamp(int((lo[0] - sec.ox) * 0.03125f), 0, sec.cols - 1);
                const int c1 = std::clamp(int((hi[0] - sec.ox) * 0.03125f), 0, sec.cols - 1);
                const int r0 = std::clamp(int((lo[1] - sec.oy) * 0.03125f), 0, sec.rows - 1);
                const int r1 = std::clamp(int((hi[1] - sec.oy) * 0.03125f), 0, sec.rows - 1);
                for (int r = r0; r <= r1; ++r)
                    for (int c = c0; c <= c1; ++c) {
                        const uint32_t cell = sec.cells[size_t(r) * sec.cols + c];
                        const uint32_t first = cell & 0xffff, count = cell >> 16;
                        for (uint32_t k = first; k < first + count && k < sec.index.size(); ++k) {
                            const uint32_t fi = sec.index[k] & 0xffff;
                            if (fi >= sec.faces.size())
                                continue;
                            const CFace &f = sec.faces[fi];
                            if (!(f.flags & 0x02))
                                continue;
                            if (onlyGroup >= 0 ? f.group != onlyGroup : !GroupEnabled(f.group))
                                continue;
                            float v[12];
                            memcpy(v, f.v, sizeof(v));
                            if (f.flags & 0x04)
                                v[2] = -2048.0f, v[5] = 2048.0f, v[8] = 2048.0f, v[11] = -2048.0f;
                            // The face's XY box must meet the segment's.
                            float fminx = 1e30f, fmaxx = -1e30f, fminy = 1e30f, fmaxy = -1e30f;
                            for (int q = 0; q < 4; ++q) {
                                fminx = std::min(fminx, v[q * 3]), fmaxx = std::max(fmaxx, v[q * 3]);
                                fminy = std::min(fminy, v[q * 3 + 1]), fmaxy = std::max(fmaxy, v[q * 3 + 1]);
                            }
                            if (fmaxx < lo[0] || fminx > hi[0] || fmaxy < lo[1] || fminy > hi[1])
                                continue;
                            const int tris[2][3] = {{0, 1, 2}, {0, 2, 3}};
                            for (int t = 0; t < (f.quad ? 2 : 1); ++t) {
                                const float *p0 = v + tris[t][0] * 3, *p1 = v + tris[t][1] * 3, *p2 = v + tris[t][2] * 3;
                                const float e1x = p1[0] - p0[0], e1y = p1[1] - p0[1], e1z = p1[2] - p0[2];
                                const float e2x = p2[0] - p0[0], e2y = p2[1] - p0[1], e2z = p2[2] - p0[2];
                                const float px = dy * e2z - dz * e2y, py = dz * e2x - dx * e2z, pz = dx * e2y - dy * e2x;
                                const float det = e1x * px + e1y * py + e1z * pz;
                                if (std::fabs(det) < 1e-8f)
                                    continue;
                                const float inv = 1.0f / det;
                                const float sx = a[0] - p0[0], sy = a[1] - p0[1], sz = a[2] - p0[2];
                                const float u = (sx * px + sy * py + sz * pz) * inv;
                                if (u < 0 || u > 1)
                                    continue;
                                const float qx = sy * e1z - sz * e1y, qy = sz * e1x - sx * e1z, qz = sx * e1y - sy * e1x;
                                const float w = (dx * qx + dy * qy + dz * qz) * inv;
                                if (w < 0 || u + w > 1)
                                    continue;
                                const float tt = (e2x * qx + e2y * qy + e2z * qz) * inv;
                                if (tt < 0 || tt > 1)
                                    continue;
                                if (getenv("NFSU2_WALL_DEBUG"))
                                    printf("[Wall] group %u (%s) flags %02x surface %u at %.1f,%.1f,%.1f\n", f.group,
                                           f.group < m_groupNames.size() ? m_groupNames[f.group].c_str() : "?", f.flags,
                                           f.surface, a[0], a[1], a[2]);
                                if (normal) {
                                    float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
                                    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                                    nx /= len, ny /= len, nz /= len;
                                    if (nx * dx + ny * dy + nz * dz > 0)
                                        nx = -nx, ny = -ny, nz = -nz;
                                    normal[0] = nx, normal[1] = ny, normal[2] = nz;
                                }
                                return true;
                            }
                        }
                    }
            }
        }
    return false;
}

bool TrackWorld::HitsWall(float ax, float ay, float az, float bx, float by, float bz, float *normal) const
{
    // Walls are the collision mesh's wall faces (0x00337888 collects the flag-0x02 faces of the cells
    // the move crosses; barrier groups only while the race enabled them, #147). The scenery's own steep render
    // triangles (kerbs, poles, tunnel linings) are not walls: they stopped the car at low ledges (#146).
    const float a[3] = {ax, ay, az}, b[3] = {bx, by, bz};
    return CollisionWall(a, b, normal);
}

void TrackWorld::ClearCars() { m_cars.clear(); }

void TrackWorld::AddToCells(const Tri &w, const RTri &r, bool physics, bool barrier)
{
    float cx = (w.v[0] + w.v[3] + w.v[6]) / 3.0f, cy = (w.v[1] + w.v[4] + w.v[7]) / 3.0f;
    int gx = int(std::floor(cx / kCellSize)), gy = int(std::floor(cy / kCellSize));
    Cell &cell = m_cells[CellKey(gx, gy)];
    cell.tris.push_back(w);
    cell.tris.back().phys = physics;
    cell.tris.back().barrier = barrier ? 1 : 0;
    cell.render.push_back(r);
    ++m_triangleCount;
}

void TrackWorld::EnableBarriers(const std::function<bool(const float *bbox)> &keep)
{
    size_t enabled = 0;
    for (Barrier &b : m_barriers)
        if (!b.tris.empty() && keep(b.bbox)) {
            for (size_t i = 0; i < b.tris.size(); ++i)
                AddToCells(b.tris[i], b.render[i], true, true);
            b.tris.clear(); // enabled once per load
            b.render.clear();
            ++enabled;
        }
    printf("[World] %zu of %zu race barriers enabled\n", enabled, m_barriers.size());
}

size_t TrackWorld::AddCar(const std::vector<SolidMesh::Triangle> *model, const std::vector<uint32_t> *textures,
                          SDL_Color paint, const std::vector<uint8_t> *rims, SDL_Color rimPaint,
                          const std::vector<uint8_t> *calipers, SDL_Color caliperPaint,
                          const std::vector<uint8_t> *mirrors, SDL_Color mirrorPaint,
                          const std::vector<uint8_t> *spoilers, SDL_Color spoilerPaint,
                          const std::vector<uint8_t> *roofs, SDL_Color roofPaint,
                          const std::vector<uint8_t> *engines, SDL_Color enginePaint,
                          const std::vector<uint8_t> *exhausts, SDL_Color exhaustPaint)
{
    Car car{model, textures, paint, rims, rimPaint, calipers, caliperPaint, mirrors, mirrorPaint,
            spoilers, spoilerPaint, roofs, roofPaint, engines, enginePaint, exhausts, exhaustPaint, {}, {}, {}};
    car.order.resize(model->size());
    for (size_t i = 0; i < car.order.size(); ++i)
        car.order[i] = i;
    if (textures && textures->size() == model->size())
        std::stable_sort(car.order.begin(), car.order.end(),
                         [textures](size_t a, size_t b) { return (*textures)[a] < (*textures)[b]; });
    m_cars.push_back(std::move(car));
    return m_cars.size() - 1;
}

void TrackWorld::AddTexturePack(std::vector<uint8_t> data)
{
    auto owned = std::make_unique<std::vector<uint8_t>>(std::move(data));
    auto pack = std::make_unique<TexturePack>();
    if (!pack->Parse(owned->data(), owned->size()))
        return;
    for (size_t i = 0; i < pack->Count(); ++i)
        m_textures.emplace(pack->At(i).nameHash, std::make_pair(pack.get(), i));
    m_tpkData.push_back(std::move(owned));
    m_packs.push_back(std::move(pack));
}

// Fixed slots (windows, lights) get stand-in colours; slots with a texture in a loaded pack
// (badging, tires, misc) are textured; the paint skin and anything unresolved use the paint.
uint32_t TrackWorld::CarMaterial(uint32_t hash, SDL_Color paint, float rgb[3]) const
{
    if (CarMaterial::SlotColour(hash, rgb))
        return 0;
    if (hash && m_textures.count(hash)) {
        rgb[0] = rgb[1] = rgb[2] = 1.0f;
        return hash;
    }
    rgb[0] = paint.r / 255.0f, rgb[1] = paint.g / 255.0f, rgb[2] = paint.b / 255.0f;
    return 0;
}

void TrackWorld::SetCarTransform(size_t index, const float m[12])
{
    if (index >= m_cars.size())
        return;
    Car &car = m_cars[index];
    car.tris.clear();
    car.render.clear();
    car.tris.reserve(car.model->size());
    car.render.reserve(car.model->size());
    const bool textured = car.textures && car.textures->size() == car.model->size();
    const bool rimmed = car.rims && car.rims->size() == car.model->size();
    const bool calipered = car.calipers && car.calipers->size() == car.model->size();
    const bool mirrored = car.mirrors && car.mirrors->size() == car.model->size();
    const bool spoilered = car.spoilers && car.spoilers->size() == car.model->size();
    const bool roofed = car.roofs && car.roofs->size() == car.model->size();
    const bool engined = car.engines && car.engines->size() == car.model->size();
    const bool exhausted = car.exhausts && car.exhausts->size() == car.model->size();
    for (size_t i : car.order) {
        const SolidMesh::Triangle &t = (*car.model)[i];
        float rgb[3];
        uint32_t tex = CarMaterial(textured ? (*car.textures)[i] : 0, car.paint, rgb);
        if (spoilered && (*car.spoilers)[i]) {
            rgb[0] = car.spoilerPaint.r / 255.0f, rgb[1] = car.spoilerPaint.g / 255.0f,
            rgb[2] = car.spoilerPaint.b / 255.0f;
            tex = 0;
        } else if (mirrored && (*car.mirrors)[i]) {
            rgb[0] = car.mirrorPaint.r / 255.0f, rgb[1] = car.mirrorPaint.g / 255.0f,
            rgb[2] = car.mirrorPaint.b / 255.0f;
            tex = 0;
        } else if (rimmed && (*car.rims)[i]) {
            // The wheel texture's rim area is dark and would swallow the paint; draw the rim face
            // untextured so the chosen colour reads like the car-shop preview (#172).
            rgb[0] = car.rimPaint.r / 255.0f, rgb[1] = car.rimPaint.g / 255.0f, rgb[2] = car.rimPaint.b / 255.0f;
            tex = 0;
        } else if (calipered && (*car.calipers)[i]) {
            rgb[0] = car.caliperPaint.r / 255.0f, rgb[1] = car.caliperPaint.g / 255.0f,
            rgb[2] = car.caliperPaint.b / 255.0f;
            tex = 0;
        } else if (roofed && (*car.roofs)[i]) {
            rgb[0] = car.roofPaint.r / 255.0f, rgb[1] = car.roofPaint.g / 255.0f, rgb[2] = car.roofPaint.b / 255.0f;
            tex = 0;
        } else if (engined && (*car.engines)[i]) {
            rgb[0] = car.enginePaint.r / 255.0f, rgb[1] = car.enginePaint.g / 255.0f,
            rgb[2] = car.enginePaint.b / 255.0f;
            tex = 0;
        } else if (exhausted && (*car.exhausts)[i]) {
            rgb[0] = car.exhaustPaint.r / 255.0f, rgb[1] = car.exhaustPaint.g / 255.0f,
            rgb[2] = car.exhaustPaint.b / 255.0f;
            tex = 0;
        }
        Tri w;
        for (int k = 0; k < 3; ++k)
            for (int a = 0; a < 3; ++a)
                w.v[k * 3 + a] = t.v[k].x * m[a] + t.v[k].y * m[4 + a] + t.v[k].z * m[8 + a] + m[3 + 4 * a];
        if (!Shade(w, rgb[0], rgb[1], rgb[2]))
            continue;
        RTri r;
        r.tex = tex;
        for (int k = 0; k < 3; ++k)
            r.v[k] = {w.v[k * 3], w.v[k * 3 + 1], w.v[k * 3 + 2], t.v[k].u, t.v[k].v, w.shade[0], w.shade[1],
                      w.shade[2], 255};
        car.tris.push_back(w);
        car.render.push_back(r);
    }
}

// Markers of one race in TrackPosMarkersAll.bin (see TrackWorld.h), grouped by name hash.
static std::vector<std::vector<TrackWorld::Camera>> MarkerGroups(const std::vector<uint8_t> &markers, int raceNumber)
{
    std::vector<std::vector<TrackWorld::Camera>> groups;
    if (markers.size() < 8 || Read32(markers.data()) != 0x34146)
        return groups;
    size_t p = 8;
    while (p < markers.size() && markers[p] == 0x11)
        ++p;
    std::vector<uint32_t> hashes;
    for (; p + 0x30 <= markers.size(); p += 0x30) {
        uint16_t number, heading;
        memcpy(&number, markers.data() + p + 0x24, 2);
        if (number != raceNumber)
            continue;
        uint32_t hash = Read32(markers.data() + p + 8);
        float pos[3];
        memcpy(pos, markers.data() + p + 0x10, 12);
        memcpy(&heading, markers.data() + p + 0x20, 2);
        size_t g = std::find(hashes.begin(), hashes.end(), hash) - hashes.begin();
        if (g == hashes.size()) {
            hashes.push_back(hash);
            groups.emplace_back();
        }
        groups[g].push_back({pos[0], pos[1], pos[2], heading * (6.2831853f / 65536.0f), -0.12f});
    }
    return groups;
}

std::vector<std::vector<TrackWorld::Camera>> TrackWorld::GridsFromMarkers(const std::vector<uint8_t> &markers,
                                                                          int raceNumber) const
{
    std::vector<std::vector<Camera>> grids;
    for (auto &g : MarkerGroups(markers, raceNumber))
        if (g.size() >= 4)
            grids.push_back(std::move(g));
    return grids;
}

bool TrackWorld::LineFromMarkers(const std::vector<uint8_t> &markers, int raceNumber, float &x, float &y) const
{
    for (const auto &g : MarkerGroups(markers, raceNumber))
        if (g.size() == 1) {
            x = g[0].x, y = g[0].y;
            return true;
        }
    return false;
}

bool TrackWorld::Load(const std::vector<uint8_t> &stream, const std::vector<const std::vector<uint8_t> *> &neighbours)
{
    const uint8_t *d = stream.data();
    const size_t n = stream.size();
    ReleaseGL();
    m_cells.clear();
    m_panorama.clear();
    m_panoramaVertices = 0;
    m_barriers.clear();
    m_packs.clear();
    m_tpkData.clear();
    m_textures.clear();
    m_triangleCount = m_instances = 0;
    m_borrowedSolids = 0;

    // Solids by name hash (UG2 solid loader 0x001bf3b8 keeps the same per-solid chunk layout).
    struct SolidRef {
        size_t body, size;
        const uint8_t *data = nullptr; // stream holding the solid (the region's, or a neighbour's)
        size_t dataSize = 0;
        bool decoded = false;
        std::vector<SolidMesh::Triangle> tris;
        std::vector<uint32_t> textures; // 0x134012: (name hash, 0) pairs, indexed by TextureNumber
    };
    std::unordered_map<uint32_t, SolidRef> solids;
    std::vector<std::pair<size_t, size_t>> sections;
    // Solids of one stream; neighbours only fill in hashes the region itself lacks.
    auto indexSolids = [&](const uint8_t *buf, size_t size, uint32_t id, size_t body, size_t len) {
        if (id != kChunkSolidList)
            return;
        ForEachChunk(buf, body, body + len, [&](uint32_t sid, size_t sbody, size_t slen) {
            if (sid != kChunkSolid)
                return;
            SolidRef ref{sbody, slen, buf, size, false, {}, {}};
            uint32_t hash = 0;
            ForEachChunk(buf, sbody, sbody + slen, [&](uint32_t hid, size_t hbody, size_t hlen) {
                if (hid == kChunkSolidHeader && hlen >= 0x14) {
                    // A header that starts on an odd alignment is preceded by 0x11 fill bytes
                    // (length 0xc8 instead of 0xc0): the hash is then at +0x18 (ISSUES #132).
                    size_t pad = 0;
                    while (pad < hlen - 0x14 && buf[hbody + pad] == 0x11)
                        ++pad;
                    hash = Read32(buf + hbody + pad + 0x10);
                }
                if (hid == 0x134012)
                    for (size_t k = 0; k + 8 <= hlen; k += 8)
                        ref.textures.push_back(Read32(buf + hbody + k));
            });
            if (hash && (buf == d || !solids.count(hash)))
                solids[hash] = std::move(ref);
        });
    };
    auto addPack = [&](const uint8_t *buf, size_t body, size_t len) {
        // Texture pack: keep a copy (the stream buffers are freed after loading).
        auto data = std::make_unique<std::vector<uint8_t>>(buf + body - 8, buf + body + len);
        auto pack = std::make_unique<TexturePack>();
        if (pack->Parse(data->data(), data->size())) {
            for (size_t i = 0; i < pack->Count(); ++i)
                m_textures.emplace(pack->At(i).nameHash, std::make_pair(pack.get(), i));
            m_tpkData.push_back(std::move(data));
            m_packs.push_back(std::move(pack));
        }
    };
    ForEachChunk(d, 0, n, [&](uint32_t id, size_t body, size_t len) {
        indexSolids(d, n, id, body, len);
        if (id == kChunkScenerySection)
            sections.emplace_back(body, len);
        else if (id == 0xb3300000)
            addPack(d, body, len);
    });
    for (const std::vector<uint8_t> *extra : neighbours)
        ForEachChunk(extra->data(), 0, extra->size(), [&](uint32_t id, size_t body, size_t len) {
            indexSolids(extra->data(), extra->size(), id, body, len);
        });
    // Collision mesh (ISSUES #133): section chunk 0x80034130 holds 0x34134 = 32-byte faces
    // {u8 ?, u8 ?, u8 surface, u8 flags, s16 z-high, s16 ?, s16 x[4], s16 y[4], s16 z-low[4]}:
    // x = s / 8, y = s / 8, z = zhigh + zlow / 256 (decoder 0x00335098); flags 0x10 = quad.
    ForEachChunk(d, 0, n, [&](uint32_t id, size_t body, size_t len) {
        if (id != 0x80034130)
            return;
        // The section's grid (0x00336180 reads them through the object at +0x08..+0x3c).
        size_t hdr = 0, faceBody = 0, faceLen = 0, idxBody = 0, idxLen = 0, cellBody = 0, cellLen = 0;
        ForEachChunk(d, body, body + len, [&](uint32_t cid, size_t cbody, size_t clen) {
            if (cid == 0x34131 && clen >= 16) hdr = cbody;
            if (cid == 0x34134) faceBody = cbody, faceLen = clen;
            if (cid == 0x34136) idxBody = cbody, idxLen = clen;
            if (cid == 0x34137) cellBody = cbody, cellLen = clen;
        });
        if (hdr && faceBody) {
            CSection sec;
            uint16_t cols, rows;
            memcpy(&cols, d + hdr + 4, 2);
            memcpy(&rows, d + hdr + 6, 2);
            memcpy(&sec.ox, d + hdr + 8, 4);
            memcpy(&sec.oy, d + hdr + 12, 4);
            sec.cols = cols, sec.rows = rows;
            if (cols && rows && cellLen >= size_t(cols) * rows * 4 && idxLen) {
                for (size_t f = faceBody; f + 32 <= faceBody + faceLen; f += 32) {
                    CFace cf;
                    int16_t zh, sx[4], sy[4], sz[4];
                    memcpy(&zh, d + f + 4, 2);
                    memcpy(sx, d + f + 8, 8);
                    memcpy(sy, d + f + 16, 8);
                    memcpy(sz, d + f + 24, 8);
                    cf.group = d[f], cf.surface = d[f + 2], cf.flags = d[f + 3];
                    cf.quad = (cf.flags & 0x10) != 0;
                    for (int k = 0; k < 4; ++k) {
                        cf.v[k * 3] = sx[k] / 8.0f;
                        cf.v[k * 3 + 1] = sy[k] / 8.0f;
                        cf.v[k * 3 + 2] = float(zh) + sz[k] / 256.0f;
                    }
                    const float ax = cf.v[3] - cf.v[0], ay = cf.v[4] - cf.v[1], az = cf.v[5] - cf.v[2];
                    const float bx = cf.v[6] - cf.v[0], by = cf.v[7] - cf.v[1], bz = cf.v[8] - cf.v[2];
                    float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
                    const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (nl > 1e-9f)
                        nx /= nl, ny /= nl, nz /= nl;
                    cf.n[0] = nx, cf.n[1] = ny, cf.n[2] = nz;
                    cf.d = nx * cf.v[0] + ny * cf.v[1] + nz * cf.v[2];
                    sec.faces.push_back(cf);
                }
                for (size_t k = 0; k + 4 <= idxLen; k += 4) {
                    uint16_t fi, mask;
                    memcpy(&fi, d + idxBody + k, 2);
                    memcpy(&mask, d + idxBody + k + 2, 2);
                    sec.index.push_back(uint32_t(fi) | uint32_t(mask) << 16);
                }
                for (size_t k = 0; k < size_t(cols) * rows; ++k) {
                    uint16_t first, count;
                    memcpy(&first, d + cellBody + k * 4, 2);
                    memcpy(&count, d + cellBody + k * 4 + 2, 2);
                    sec.cells.push_back(uint32_t(first) | uint32_t(count) << 16);
                }
                const uint32_t si = uint32_t(m_csections.size());
                const int x0 = int(std::floor(sec.ox / kCellSize)), x1 = int(std::floor((sec.ox + cols * 32.0f) / kCellSize));
                const int y0 = int(std::floor(sec.oy / kCellSize)), y1 = int(std::floor((sec.oy + rows * 32.0f) / kCellSize));
                for (int gx = x0; gx <= x1; ++gx)
                    for (int gy = y0; gy <= y1; ++gy)
                        m_csectionGrid[CellKey(gx, gy)].push_back(si);
                m_csections.push_back(std::move(sec));
            }
        }
        ForEachChunk(d, body, body + len, [&](uint32_t cid, size_t cbody, size_t clen) {
            if (cid != 0x34134)
                return;
            for (size_t f = cbody; f + 32 <= cbody + clen; f += 32) {
                CTri t;
                int16_t zh, sx[4], sy[4], sz[4];
                memcpy(&zh, d + f + 4, 2);
                memcpy(sx, d + f + 8, 8);
                memcpy(sy, d + f + 16, 8);
                memcpy(sz, d + f + 24, 8);
                t.surface = d[f + 2], t.flags = d[f + 3];
                for (int k = 0; k < 4; ++k) {
                    t.v[k * 3] = sx[k] / 8.0f;
                    t.v[k * 3 + 1] = sy[k] / 8.0f;
                    t.v[k * 3 + 2] = float(zh) + sz[k] / 256.0f;
                }
                const float cx = (t.v[0] + t.v[3] + t.v[6]) / 3.0f, cy = (t.v[1] + t.v[4] + t.v[7]) / 3.0f;
                m_collision[CellKey(int(std::floor(cx / kCellSize)), int(std::floor(cy / kCellSize)))].push_back(t);
                ++m_collisionFaces;
            }
        });
    });
    printf("[World] %zu collision faces\n", m_collisionFaces);
    std::unordered_set<uint32_t> neededTextures; // referenced by neighbour solids actually placed

    struct RoadSpot {
        float cx, cy, z, yaw;
    };
    std::vector<RoadSpot> roads;
    double sumX = 0, sumY = 0;
    size_t unresolved = 0;

    for (auto [body, len] : sections) {
        size_t infos = 0, infosLen = 0, inst = 0, instLen = 0;
        ForEachChunk(d, body, body + len, [&](uint32_t id, size_t b, size_t l) {
            if (id == kChunkSceneryInfos) { infos = b; infosLen = l; }
            if (id == kChunkSceneryInstances) { inst = b; instLen = l; }
        });
        if (!infos || !inst)
            continue;
        size_t instEnd = inst + instLen;
        while (inst < instEnd && d[inst] == 0x11)
            ++inst;
        const size_t infoCount = infosLen / kInfoSize;
        for (size_t o = inst; o + kInstanceSize <= instEnd; o += kInstanceSize) {
            const uint8_t *r = d + o;
            uint16_t infoIndex;
            memcpy(&infoIndex, r + 0x18, 2);
            if (infoIndex >= infoCount) {
                ++unresolved;
                continue;
            }
            const uint8_t *info = d + infos + infoIndex * kInfoSize;
            char name[33] = {};
            memcpy(name, info, 32);
            auto it = solids.find(Read32(info + 0x20));
            Kind kind = KindOf(name);
            if (it == solids.end() || kind == kSky) {
                if (const char *dbg = getenv("NFSU2_SKIPPED_AT")) { // dev aid: "x,y" lists skipped instances there
                    float qx, qy;
                    float bb[6];
                    memcpy(bb, r, 24);
                    if (sscanf(dbg, "%f,%f", &qx, &qy) == 2 && qx >= bb[0] && qx <= bb[3] && qy >= bb[1] && qy <= bb[4])
                        printf("[World] skipped instance %s hash %08x kind %d bbox %.0f %.0f %.0f .. %.0f %.0f %.0f\n", name,
                               Read32(info + 0x20), int(kind), bb[0], bb[1], bb[2], bb[3], bb[4], bb[5]);
                }
                ++unresolved;
                continue;
            }
            SolidRef &s = it->second;
            if (!s.decoded) {
                s.decoded = true;
                SolidMesh::DecodeChunks(s.data, s.dataSize, s.body, s.body + s.size, s.tris);
                if (s.data != d) {
                    ++m_borrowedSolids;
                    for (uint32_t t : s.textures)
                        if (!m_textures.count(t))
                            neededTextures.insert(t);
                }
            }
            float bbox[6], pos[3];
            int16_t rot[9];
            memcpy(bbox, r, 24);
            memcpy(pos, r + 0x20, 12);
            memcpy(rot, r + 0x2c, 18);
            float m[9];
            for (int k = 0; k < 9; ++k)
                m[k] = rot[k] / 8192.0f;
            ++m_instances;
            sumX += (bbox[0] + bbox[3]) * 0.5;
            sumY += (bbox[1] + bbox[4]) * 0.5;
            if (kind == kRoad) {
                float ex = bbox[3] - bbox[0], ey = bbox[4] - bbox[1];
                roads.push_back({(bbox[0] + bbox[3]) * 0.5f, (bbox[1] + bbox[4]) * 0.5f, bbox[5],
                                 ex >= ey ? 0.0f : 1.5707963f});
            }
            float br, bg, bb;
            BaseColour(kind, br, bg, bb);
            for (const SolidMesh::Triangle &t : s.tris) {
                Tri w;
                for (int k = 0; k < 3; ++k) {
                    const SolidMesh::Point &p = t.v[k];
                    // Row vectors: world = p.x*row0 + p.y*row1 + p.z*row2 + pos
                    for (int a = 0; a < 3; ++a)
                        w.v[k * 3 + a] = p.x * m[a] + p.y * m[3 + a] + p.z * m[6 + a] + pos[a];
                }
                if (!Shade(w, br, bg, bb))
                    continue; // degenerate
                RTri rt;
                rt.tex = t.tex >= 0 && size_t(t.tex) < s.textures.size() && m_textures.count(s.textures[t.tex])
                             ? s.textures[t.tex]
                             : 0;
                for (int k = 0; k < 3; ++k) {
                    const SolidMesh::Point &p = t.v[k];
                    RVertex &v = rt.v[k];
                    v.x = w.v[k * 3], v.y = w.v[k * 3 + 1], v.z = w.v[k * 3 + 2];
                    v.u = p.u, v.v = p.v;
                    if (rt.tex) { // PS2 vertex colour: 128 = 1.0
                        v.r = uint8_t(std::min(255, p.r * 2)), v.g = uint8_t(std::min(255, p.g * 2));
                        v.b = uint8_t(std::min(255, p.b * 2));
                    } else {
                        v.r = w.shade[0], v.g = w.shade[1], v.b = w.shade[2];
                    }
                    v.a = 255;
                }
                if (kind == kPanorama) {
                    m_panorama.push_back(rt);
                    continue;
                }
                if (kind == kBarrier) {
                    if (m_barriers.empty() || memcmp(m_barriers.back().bbox, bbox, sizeof(bbox)) != 0) {
                        m_barriers.emplace_back();
                        memcpy(m_barriers.back().bbox, bbox, sizeof(bbox));
                    }
                    m_barriers.back().tris.push_back(w);
                    m_barriers.back().render.push_back(rt);
                    continue;
                }
                AddToCells(w, rt, true);
            }
        }
    }
    // Texture packs of the neighbours that hold textures the borrowed solids use.
    for (const std::vector<uint8_t> *extra : neighbours)
        ForEachChunk(extra->data(), 0, extra->size(), [&](uint32_t id, size_t body, size_t len) {
            if (id != 0xb3300000 || neededTextures.empty())
                return;
            TexturePack probe;
            if (!probe.Parse(extra->data() + body - 8, len + 8))
                return;
            bool wanted = false;
            for (size_t i = 0; i < probe.Count() && !wanted; ++i)
                wanted = neededTextures.count(probe.At(i).nameHash) != 0;
            if (!wanted)
                return;
            for (size_t i = 0; i < probe.Count(); ++i)
                neededTextures.erase(probe.At(i).nameHash);
            addPack(extra->data(), body, len);
        });
    printf("[World] %zu solids borrowed from neighbouring regions\n", m_borrowedSolids);
    printf("[World] %zu texture packs, %zu textures; strips: %lu exact topology, %lu approximated\n", m_packs.size(),
           m_textures.size(), SolidMesh::g_exactStrips, SolidMesh::g_approxStrips);
    // Dev aid: NFSU2_TEX_SCORE=1 compares the swizzled and the row-by-row decode of every world texture
    // (mean absolute difference of neighbouring pixels: natural images are smooth, a wrong layout is not)
    // and prints, per (bits, width, height) class, how many textures a linear decode would make smoother.
    if (getenv("NFSU2_TEX_SCORE")) {
        struct Tally {
            int total = 0, linearBetter = 0;
        };
        std::map<std::tuple<int, int, int>, Tally> classes;
        auto rough = [](const DecodedTexture &d) {
            double sum = 0;
            long n = 0;
            for (int y = 0; y < d.height; ++y)
                for (int x = 0; x + 1 < d.width; ++x) {
                    const uint32_t a = d.rgba[size_t(y) * d.width + x], b = d.rgba[size_t(y) * d.width + x + 1];
                    for (int c = 0; c < 24; c += 8)
                        sum += std::abs(int((a >> c) & 255) - int((b >> c) & 255));
                    ++n;
                }
            return n ? sum / double(n) : 0.0;
        };
        for (const auto &pack : m_packs)
            for (size_t i = 0; i < pack->Count(); ++i) {
                const TextureInfo &t = pack->At(i);
                DecodedTexture a, b;
                SetForceLinearDecode(false);
                if (!pack->Decode(t, a) || a.width < 8)
                    continue;
                SetForceLinearDecode(true);
                const bool ok = pack->Decode(t, b);
                SetForceLinearDecode(false);
                if (!ok)
                    continue;
                Tally &tally = classes[{t.bitsPerPixel, t.width, t.height}];
                ++tally.total;
                tally.linearBetter += rough(b) < rough(a) * 0.8;
            }
        for (const auto &c : classes)
            printf("[TexScore] %2d-bit %4dx%-4d : %3d textures, linear smoother for %d\n", std::get<0>(c.first),
                   std::get<1>(c.first), std::get<2>(c.first), c.second.total, c.second.linearBetter);
    }
    printf("[World] %zu solids, %zu scenery sections, %zu instances placed (%zu skipped), %zu triangles, %zu cells\n",
           solids.size(), sections.size(), m_instances, unresolved, m_triangleCount, m_cells.size());
    if (!m_instances)
        return false;

    // Start on the road instance closest to the middle of the map.
    float mx = float(sumX / m_instances), my = float(sumY / m_instances);
    m_start = {mx, my, 50.0f, 0.0f, -0.12f};
    float best = 1e30f;
    for (const RoadSpot &r : roads) {
        float dd = (r.cx - mx) * (r.cx - mx) + (r.cy - my) * (r.cy - my);
        if (dd < best) {
            best = dd;
            m_start = {r.cx, r.cy, r.z + 2.0f, r.yaw, -0.12f};
        }
    }
    return true;
}

void TrackWorld::Render(SDL_Renderer *renderer, const Camera &cam, int width, int height)
{
    constexpr float kDrawDistance = 450.0f;
    constexpr float kNear = 0.5f;
    const float fogR = 0.55f, fogG = 0.62f, fogB = 0.72f;

    // Sky.
    SDL_Vertex sky[6];
    SDL_Color top{70, 100, 150, 255}, horizon{140, 158, 184, 255};
    float w = float(width), h = float(height);
    sky[0] = {{0, 0}, top, {0, 0}};
    sky[1] = {{w, 0}, top, {0, 0}};
    sky[2] = {{w, h}, horizon, {0, 0}};
    sky[3] = {{0, 0}, top, {0, 0}};
    sky[4] = {{w, h}, horizon, {0, 0}};
    sky[5] = {{0, h}, horizon, {0, 0}};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_RenderGeometry(renderer, nullptr, sky, 6, nullptr, 0);

    const float cy = std::cos(cam.yaw), sy = std::sin(cam.yaw);
    const float cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
    const float fx = cy * cp, fy = sy * cp, fz = sp;  // forward
    const float rx = sy, ry = -cy, rz = 0.0f;         // right
    const float ux = ry * fz - rz * fy, uy = rz * fx - rx * fz, uz = rx * fy - ry * fx; // up = right x forward
    const float focal = (w * 0.5f) / std::tan(35.0f * 3.14159265f / 180.0f);

    // Camera-space triangles, clipped against the near plane (big road triangles reach behind
    // the camera), then painter-sorted by mean depth.
    struct CamVert {
        float x, y, z;
    };
    struct Visible {
        float depth;
        float nearest; // smallest vertex depth
        int pass;      // 0 world, 1 car, 2 world ground
        int count;     // 3 or 4 (a clipped triangle can become a quad)
        CamVert v[4];
        SDL_Color colour;
    };
    static std::vector<Visible> visible;
    visible.clear();
    const int reach = int(std::ceil(kDrawDistance / kCellSize));
    const int ccx = int(std::floor(cam.x / kCellSize)), ccy = int(std::floor(cam.y / kCellSize));
    std::vector<const std::vector<Tri> *> lists;
    for (const Car &car : m_cars)
        lists.push_back(&car.tris);
    const size_t carLists = lists.size();
    for (int gx = ccx - reach; gx <= ccx + reach; ++gx)
        for (int gy = ccy - reach; gy <= ccy + reach; ++gy) {
            auto it = m_cells.find(CellKey(gx, gy));
            if (it == m_cells.end())
                continue;
            float mx = (gx + 0.5f) * kCellSize - cam.x, my = (gy + 0.5f) * kCellSize - cam.y;
            float dist = std::sqrt(mx * mx + my * my);
            if (dist > kDrawDistance + kCellSize || mx * cy + my * sy < -kCellSize)
                continue; // too far, or wholly behind the camera
            // Horizontal view cone (half angle atan(w/2 / focal)) widened by the cell radius.
            const float along = mx * cy + my * sy, across = std::fabs(mx * sy - my * cy);
            if (across > along * (w * 0.5f / focal) + kCellSize * 0.75f)
                continue;
            lists.push_back(&it->second.tris);
        }
    for (size_t li = 0; li < lists.size(); ++li) {
        const std::vector<Tri> *list = lists[li];
        {
            for (const Tri &t : *list) {
                CamVert in[3];
                int behind = 0;
                for (int k = 0; k < 3; ++k) {
                    float px = t.v[k * 3] - cam.x, py = t.v[k * 3 + 1] - cam.y, pz = t.v[k * 3 + 2] - cam.z;
                    in[k] = {px * rx + py * ry + pz * rz, px * ux + py * uy + pz * uz, px * fx + py * fy + pz * fz};
                    behind += in[k].z < kNear;
                }
                if (behind == 3)
                    continue;
                Visible vis;
                vis.count = 0;
                if (behind == 0) {
                    for (int k = 0; k < 3; ++k)
                        vis.v[vis.count++] = in[k];
                } else {
                    // Sutherland-Hodgman against z = kNear.
                    for (int k = 0; k < 3; ++k) {
                        const CamVert &a = in[k], &b = in[(k + 1) % 3];
                        bool ain = a.z >= kNear, bin = b.z >= kNear;
                        if (ain)
                            vis.v[vis.count++] = a;
                        if (ain != bin) {
                            float f = (kNear - a.z) / (b.z - a.z);
                            vis.v[vis.count++] = {a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, kNear};
                        }
                    }
                }
                float depth = 0;
                for (int k = 0; k < vis.count; ++k)
                    depth += vis.v[k].z;
                depth /= float(vis.count);
                if (depth > kDrawDistance)
                    continue;
                float fog = std::clamp((depth - 120.0f) / (kDrawDistance - 120.0f), 0.0f, 1.0f);
                vis.colour = {Uint8(t.shade[0] + (fogR * 255 - t.shade[0]) * fog),
                              Uint8(t.shade[1] + (fogG * 255 - t.shade[1]) * fog),
                              Uint8(t.shade[2] + (fogB * 255 - t.shade[2]) * fog), 255};
                vis.depth = depth;
                vis.nearest = 1e30f;
                float farthest = 0;
                for (int k = 0; k < vis.count; ++k) {
                    vis.nearest = std::min(vis.nearest, vis.v[k].z);
                    farthest = std::max(farthest, vis.v[k].z);
                }
                vis.pass = li < carLists ? 1 : t.ground ? 2 : 0;
                vis.nearest = vis.pass == 1 ? vis.nearest : farthest; // world: compare by its far end
                visible.push_back(vis);
            }
        }
    }
    // Painter's order by mean depth for the world; cars are merged in so that a car triangle is
    // drawn after every world triangle that is not wholly in front of it (the road triangle a car
    // sits on is often larger than the car and its centre nearer to the camera).
    // Without a depth buffer, ground (near-horizontal) triangles go first so that a road slab
    // between the camera and a car never covers the car.
    std::vector<Visible> cars, ground;
    auto split = std::partition(visible.begin(), visible.end(), [](const Visible &v) { return v.pass == 0; });
    for (auto it = split; it != visible.end(); ++it)
        (it->pass == 1 ? cars : ground).push_back(*it);
    visible.erase(split, visible.end());
    auto byDepth = [](const Visible &a, const Visible &b) { return a.depth > b.depth; };
    std::sort(visible.begin(), visible.end(), byDepth);
    std::sort(cars.begin(), cars.end(), byDepth);
    std::sort(ground.begin(), ground.end(), byDepth);
    if (!cars.empty() || !ground.empty()) {
        std::vector<Visible> merged = std::move(ground);
        merged.reserve(merged.size() + visible.size() + cars.size());
        size_t j = 0;
        for (const Visible &v : visible) {
            while (j < cars.size() && cars[j].depth >= v.nearest) // v.nearest holds the far end for world
                merged.push_back(cars[j++]);
            merged.push_back(v);
        }
        merged.insert(merged.end(), cars.begin() + j, cars.end());
        visible.swap(merged);
    }
    m_frame.clear();
    m_frame.reserve(visible.size() * 3);
    auto project = [&](const CamVert &v, SDL_Color c) {
        return SDL_Vertex{{w * 0.5f + v.x * focal / v.z, h * 0.5f - v.y * focal / v.z}, c, {0, 0}};
    };
    for (const Visible &vis : visible) {
        for (int k = 1; k + 1 < vis.count; ++k) { // fan
            m_frame.push_back(project(vis.v[0], vis.colour));
            m_frame.push_back(project(vis.v[k], vis.colour));
            m_frame.push_back(project(vis.v[k + 1], vis.colour));
        }
    }
    if (!m_frame.empty() && SDL_RenderGeometry(renderer, nullptr, m_frame.data(), int(m_frame.size()), nullptr, 0) != 0)
        printf("[World] SDL_RenderGeometry: %s\n", SDL_GetError());
}
