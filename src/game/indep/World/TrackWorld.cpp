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

enum Kind : uint8_t { kOther, kRoad, kTerrain, kBuilding, kSky };

Kind KindOf(const char *name)
{
    if (strstr(name, "SKY") || strstr(name, "Sky"))
        return kSky;
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

bool TrackWorld::Load(const std::vector<uint8_t> &stream)
{
    const uint8_t *d = stream.data();
    const size_t n = stream.size();
    m_cells.clear();
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
    const float lx = 0.40f, ly = 0.30f, lz = 0.866f; // light direction (normalised)

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
                float ux = w.v[3] - w.v[0], uy = w.v[4] - w.v[1], uz = w.v[5] - w.v[2];
                float vx = w.v[6] - w.v[0], vy = w.v[7] - w.v[1], vz = w.v[8] - w.v[2];
                float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
                float len2 = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (len2 < 1e-6f)
                    continue; // degenerate
                float lit = 0.35f + 0.65f * std::fabs((nx * lx + ny * ly + nz * lz) / len2);
                w.shade[0] = uint8_t(std::min(255.0f, br * lit * 255.0f));
                w.shade[1] = uint8_t(std::min(255.0f, bg * lit * 255.0f));
                w.shade[2] = uint8_t(std::min(255.0f, bb * lit * 255.0f));
                float cx = (w.v[0] + w.v[3] + w.v[6]) / 3.0f, cy = (w.v[1] + w.v[4] + w.v[7]) / 3.0f;
                int gx = int(std::floor(cx / kCellSize)), gy = int(std::floor(cy / kCellSize));
                m_cells[CellKey(gx, gy)].tris.push_back(w);
                ++m_triangleCount;
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
        int count; // 3 or 4 (a clipped triangle can become a quad)
        CamVert v[4];
        SDL_Color colour;
    };
    static std::vector<Visible> visible;
    visible.clear();
    const int reach = int(std::ceil(kDrawDistance / kCellSize));
    const int ccx = int(std::floor(cam.x / kCellSize)), ccy = int(std::floor(cam.y / kCellSize));
    for (int gx = ccx - reach; gx <= ccx + reach; ++gx)
        for (int gy = ccy - reach; gy <= ccy + reach; ++gy) {
            auto it = m_cells.find(CellKey(gx, gy));
            if (it == m_cells.end())
                continue;
            float mx = (gx + 0.5f) * kCellSize - cam.x, my = (gy + 0.5f) * kCellSize - cam.y;
            float dist = std::sqrt(mx * mx + my * my);
            if (dist > kDrawDistance + kCellSize || mx * cy + my * sy < -kCellSize)
                continue; // too far, or wholly behind the camera
            for (const Tri &t : it->second.tris) {
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
                visible.push_back(vis);
            }
        }
    std::sort(visible.begin(), visible.end(), [](const Visible &a, const Visible &b) { return a.depth > b.depth; });

    if (getenv("NFSU2_WORLD_DEBUG")) {
        size_t near30 = 0;
        for (const Visible &v : visible)
            near30 += v.depth < 30.0f;
        printf("[World] visible %zu (depth<30: %zu)\n", visible.size(), near30);
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
