#include "game/indep/World/TrackWorld.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

enum Kind : uint8_t { kOther, kRoad, kTerrain, kBuilding, kSky, kBarrier };

Kind KindOf(const char *name)
{
    if (strstr(name, "SKY") || strstr(name, "Sky"))
        return kSky;
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
                    f(t);
        }
}

bool TrackWorld::GroundHeight(float x, float y, float zMax, float &z) const
{
    bool found = false;
    float best = -1e30f;
    ForNearbyTris(x, y, [&](const Tri &t) {
        const float *a = t.v, *b = t.v + 3, *c = t.v + 6;
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
        }
    });
    if (found)
        z = best;
    return found;
}

bool TrackWorld::HitsWall(float ax, float ay, float az, float bx, float by, float bz) const
{
    const float dx = bx - ax, dy = by - ay, dz = bz - az;
    bool hit = false;
    ForNearbyTris(ax, ay, [&](const Tri &t) {
        if (hit)
            return;
        const float *a = t.v, *b = t.v + 3, *c = t.v + 6;
        float e1x = b[0] - a[0], e1y = b[1] - a[1], e1z = b[2] - a[2];
        float e2x = c[0] - a[0], e2y = c[1] - a[1], e2z = c[2] - a[2];
        float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
        float n2 = nx * nx + ny * ny + nz * nz;
        if (n2 < 1e-8f || nz * nz > 0.25f * n2)
            return; // not steep enough to be a wall
        // Moeller-Trumbore segment test.
        float px = dy * e2z - dz * e2y, py = dz * e2x - dx * e2z, pz = dx * e2y - dy * e2x;
        float det = e1x * px + e1y * py + e1z * pz;
        if (std::fabs(det) < 1e-8f)
            return;
        float inv = 1.0f / det;
        float sx = ax - a[0], sy = ay - a[1], sz = az - a[2];
        float u = (sx * px + sy * py + sz * pz) * inv;
        if (u < 0 || u > 1)
            return;
        float qx = sy * e1z - sz * e1y, qy = sz * e1x - sx * e1z, qz = sx * e1y - sy * e1x;
        float v = (dx * qx + dy * qy + dz * qz) * inv;
        if (v < 0 || u + v > 1)
            return;
        float tt = (e2x * qx + e2y * qy + e2z * qz) * inv;
        hit = tt >= 0 && tt <= 1;
        if (hit && getenv("NFSU2_WORLD_DEBUG"))
            printf("[World] wall (%.1f %.1f %.1f) (%.1f %.1f %.1f) (%.1f %.1f %.1f)\n", a[0], a[1], a[2], b[0], b[1], b[2],
                   c[0], c[1], c[2]);
    });
    return hit;
}

void TrackWorld::ClearCars() { m_cars.clear(); }

void TrackWorld::AddToCells(const Tri &w)
{
    float cx = (w.v[0] + w.v[3] + w.v[6]) / 3.0f, cy = (w.v[1] + w.v[4] + w.v[7]) / 3.0f;
    int gx = int(std::floor(cx / kCellSize)), gy = int(std::floor(cy / kCellSize));
    m_cells[CellKey(gx, gy)].tris.push_back(w);
    ++m_triangleCount;
}

void TrackWorld::EnableBarriers(const std::function<bool(const float *bbox)> &keep)
{
    size_t enabled = 0;
    for (Barrier &b : m_barriers)
        if (!b.tris.empty() && keep(b.bbox)) {
            for (const Tri &t : b.tris)
                AddToCells(t);
            b.tris.clear(); // enabled once per load
            ++enabled;
        }
    printf("[World] %zu of %zu race barriers enabled\n", enabled, m_barriers.size());
}

size_t TrackWorld::AddCar(const std::vector<SolidMesh::Triangle> *model, SDL_Color paint)
{
    m_cars.push_back({model, paint, {}});
    return m_cars.size() - 1;
}

void TrackWorld::SetCarTransform(size_t index, const float m[12])
{
    if (index >= m_cars.size())
        return;
    Car &car = m_cars[index];
    car.tris.clear();
    car.tris.reserve(car.model->size());
    for (const SolidMesh::Triangle &t : *car.model) {
        Tri w;
        for (int k = 0; k < 3; ++k)
            for (int a = 0; a < 3; ++a)
                w.v[k * 3 + a] = t.v[k].x * m[a] + t.v[k].y * m[4 + a] + t.v[k].z * m[8 + a] + m[3 + 4 * a];
        if (Shade(w, car.paint.r / 255.0f, car.paint.g / 255.0f, car.paint.b / 255.0f))
            car.tris.push_back(w);
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

bool TrackWorld::Load(const std::vector<uint8_t> &stream)
{
    const uint8_t *d = stream.data();
    const size_t n = stream.size();
    m_cells.clear();
    m_barriers.clear();
    m_triangleCount = m_instances = 0;

    // Solids by name hash (UG2 solid loader 0x001bf3b8 keeps the same per-solid chunk layout).
    struct SolidRef {
        size_t body, size;
        bool decoded = false;
        std::vector<SolidMesh::Triangle> tris;
    };
    std::unordered_map<uint32_t, SolidRef> solids;
    std::vector<std::pair<size_t, size_t>> sections;
    ForEachChunk(d, 0, n, [&](uint32_t id, size_t body, size_t len) {
        if (id == kChunkSolidList) {
            ForEachChunk(d, body, body + len, [&](uint32_t sid, size_t sbody, size_t slen) {
                if (sid != kChunkSolid)
                    return;
                ForEachChunk(d, sbody, sbody + slen, [&](uint32_t hid, size_t hbody, size_t hlen) {
                    if (hid == kChunkSolidHeader && hlen >= 0x14)
                        solids[Read32(d + hbody + 0x10)] = {sbody, slen, false, {}};
                });
            });
        } else if (id == kChunkScenerySection) {
            sections.emplace_back(body, len);
        }
    });

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
                ++unresolved;
                continue;
            }
            SolidRef &s = it->second;
            if (!s.decoded) {
                s.decoded = true;
                SolidMesh::DecodeChunks(d, n, s.body, s.body + s.size, s.tris);
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
                if (kind == kBarrier) {
                    if (m_barriers.empty() || memcmp(m_barriers.back().bbox, bbox, sizeof(bbox)) != 0) {
                        m_barriers.emplace_back();
                        memcpy(m_barriers.back().bbox, bbox, sizeof(bbox));
                    }
                    m_barriers.back().tris.push_back(w);
                    continue;
                }
                AddToCells(w);
            }
        }
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
