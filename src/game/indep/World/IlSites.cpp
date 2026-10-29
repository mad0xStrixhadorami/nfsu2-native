// Imported intersections placed on the road mesh (ISSUES #336).  The sites come from tools/find_il_intersections.py (straight, flat,
// single-layer asphalt crossings and T-junctions of the city, with their measured half widths).  Each site is fitted with a rubber sheet
// (IlAssets::EmitOptions::rubber), the game's own carriageway there is repaved with the plain intersection asphalt so its painted lines do
// not fight the asset's markings, and everything that would stand off the existing pavements is left out.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "game/indep/World/IlAssets.h"
#include "game/indep/World/TrackWorld.h"

namespace {

struct Surf { // point-in-triangle index over a set of surface triangles (8 m buckets)
    std::vector<float> tris, zs;
    std::unordered_map<int64_t, std::vector<uint32_t>> cells;
    static int64_t K(int x, int y) { return (int64_t(x) << 32) ^ uint32_t(y); }
    void Add(const float *v, float z)
    {
        const uint32_t n = uint32_t(zs.size());
        tris.insert(tris.end(), v, v + 6), zs.push_back(z);
        const int x0 = int(std::floor(std::min({v[0], v[2], v[4]}) / 8.0f)), x1 = int(std::floor(std::max({v[0], v[2], v[4]}) / 8.0f));
        const int y0 = int(std::floor(std::min({v[1], v[3], v[5]}) / 8.0f)), y1 = int(std::floor(std::max({v[1], v[3], v[5]}) / 8.0f));
        for (int gx = x0; gx <= x1; ++gx)
            for (int gy = y0; gy <= y1; ++gy)
                cells[K(gx, gy)].push_back(n);
    }
    bool On(float x, float y, float zRef, float tol) const
    {
        auto it = cells.find(K(int(std::floor(x / 8.0f)), int(std::floor(y / 8.0f))));
        if (it == cells.end())
            return false;
        for (uint32_t n : it->second) {
            if (std::fabs(zs[n] - zRef) > tol)
                continue;
            const float *t = &tris[size_t(n) * 6];
            const float d1 = (x - t[2]) * (t[1] - t[3]) - (t[0] - t[2]) * (y - t[3]);
            const float d2 = (x - t[4]) * (t[3] - t[5]) - (t[2] - t[4]) * (y - t[5]);
            const float d3 = (x - t[0]) * (t[5] - t[1]) - (t[4] - t[0]) * (y - t[1]);
            if (!((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0)))
                return true;
        }
        return false;
    }
};

struct Site {
    char kind = 'X';
    float x = 0, y = 0, yawDeg = 0, hwU = 0, hwV = 0, z = 0;
};

// The half gaps of the assets' road (kerb to centre), measured from their kerb blocks: arms along model x have half width gy, along y gx.
struct Asset {
    const char *name;
    float gx, gy;
};
constexpr Asset kFour[3] = {{"IL_Intersection_01_Urban_4Way", 12.3f, 12.3f},
                            {"IL_Intersection_02_Rural_Hills_4Way", 8.2f, 11.9f},
                            {"IL_Intersection_03_Highway_Signalized", 8.8f, 13.0f}};

} // namespace

void TrackWorld::PlaceIlIntersections(std::vector<RTri> &props, std::vector<std::pair<size_t, size_t>> &ownRanges,
                                      std::vector<std::array<float, 4>> &keepOut, std::vector<PropSolid> &solids)
{
    if (getenv("NFSU2_NO_IL_INTERSECTIONS") || getenv("NFSU2_NO_IL_ASSETS"))
        return;
    std::string file = IlAssets::Dir();
    if (const char *e = getenv("NFSU2_IL_SITES"))
        file = e;
    else if (!file.empty())
        file += "/intersections_L4RA.txt";
    std::ifstream in(file);
    if (!in)
        return;
    std::vector<Site> sites;
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ss(line);
        Site s;
        std::string k;
        if (ss >> k >> s.x >> s.y >> s.yawDeg >> s.hwU >> s.hwV >> s.z)
            s.kind = k[0], sites.push_back(s);
    }
    if (sites.empty())
        return;

    // Surface indices from the render triangles: asphalt carriageway (RDP_LANE*, RDP_INTERSECT*, RDP_ROADSTRIP_*) and sidewalks.
    Surf asphalt, surf;
    std::unordered_map<uint32_t, int> klass;
    uint32_t plainAsphalt = 0;
    for (const auto &kv : m_textures) {
        const std::string &n = kv.second.first->At(kv.second.second).name;
        if (n == "RDP_INTERSECTMIDTILE")
            plainAsphalt = kv.first;
    }
    auto classOf = [&](uint32_t tex) {
        auto it = klass.find(tex);
        if (it != klass.end())
            return it->second;
        int c = 0;
        auto src = m_textures.find(tex);
        if (src != m_textures.end()) {
            const std::string &n = src->second.first->At(src->second.second).name;
            if (n.compare(0, 8, "RDP_LANE") == 0 || n.compare(0, 13, "RDP_INTERSECT") == 0 || n.compare(0, 14, "RDP_ROADSTRIP_") == 0)
                c = 1;
            else if (n.compare(0, 12, "ARC_SIDEWALK") == 0)
                c = 2;
        }
        return klass[tex] = c;
    };
    for (auto &entry : m_cells)
        for (const RTri &t : entry.second.render) {
            if (!t.tex || t.v[0].a != 255)
                continue;
            const int c = classOf(t.tex);
            if (!c)
                continue;
            const float xy[6] = {t.v[0].x, t.v[0].y, t.v[1].x, t.v[1].y, t.v[2].x, t.v[2].y};
            const float z = (t.v[0].z + t.v[1].z + t.v[2].z) / 3.0f;
            surf.Add(xy, z);
            if (c == 1)
                asphalt.Add(xy, z);
        }

    IlAssets::GroundFn ground = [this](float x, float y, float zMax, float &z) { return GroundHeight(x, y, zMax, z); };
    size_t placed = 0, repaved = 0, skipped = 0;
    for (const Site &site : sites) {
        // the site must really be asphalt here (the list belongs to one map)
        if (!asphalt.On(site.x, site.y, site.z, 3.0f)) {
            ++skipped;
            continue;
        }
        // pick the asset and the quarter turn that need the least stretching
        struct Fit {
            const IlAssets::Model *m = nullptr;
            const Asset *a = nullptr;
            int rot = 0;
            float sx = 1, sy = 1, cost = 1e9f;
        } best;
        const bool tee = site.kind == 'T';
        for (const Asset &a : kFour) {
            if (tee && a.gx != a.gy)
                continue; // a T uses the square urban crossing with its missing arm left out
            const IlAssets::Model *m = IlAssets::Get(a.name);
            if (!m)
                continue;
            for (int rot = 0; rot < (tee ? 1 : 4); ++rot) {
                const bool swap = rot & 1;
                const float sy = (swap ? site.hwV : site.hwU) / a.gy, sx = (swap ? site.hwU : site.hwV) / a.gx;
                const float cost = std::max(std::fabs(std::log(sx)), std::fabs(std::log(sy))) + 0.3f * std::fabs(std::log(sx / sy)) + (a.gx != a.gy ? 0.05f : 0.0f);
                if (sx < 0.66f || sx > 1.45f || sy < 0.66f || sy > 1.45f)
                    continue;
                if (cost < best.cost)
                    best = {m, &a, rot, sx, sy, cost};
            }
        }
        if (!best.m) {
            ++skipped;
            printf("[IL] site %c %.0f %.0f: no asset fits (half widths %.1f / %.1f)\n", site.kind, site.x, site.y, site.hwU, site.hwV);
            continue;
        }
        IlAssets::Instance inst;
        inst.x = site.x, inst.y = site.y, inst.z = site.z;
        inst.yaw = (site.yawDeg + 90.0f * float(best.rot)) * 0.0174532925f;
        inst.sx = best.sx, inst.sy = best.sy;
        const float cu = std::cos(site.yawDeg * 0.0174532925f), su = std::sin(site.yawDeg * 0.0174532925f);

        // 1. repave: the carriageway inside the cross (arms 52 m, the measured width plus the gutter) takes the plain intersection asphalt
        if (plainAsphalt) {
            const float arm = 52.0f;
            for (auto &entry : m_cells)
                for (RTri &t : entry.second.render) {
                    if (t.v[0].a != 255 || t.tex == plainAsphalt || classOf(t.tex) != 1)
                        continue;
                    const float cx = (t.v[0].x + t.v[1].x + t.v[2].x) / 3.0f - site.x, cy = (t.v[0].y + t.v[1].y + t.v[2].y) / 3.0f - site.y;
                    if (std::fabs(cx) > arm + 4 || std::fabs(cy) > arm + 4)
                        continue;
                    const float u = cx * cu + cy * su, v = -cx * su + cy * cu;
                    const float cz = (t.v[0].z + t.v[1].z + t.v[2].z) / 3.0f;
                    if (std::fabs(cz - site.z) > 6.0f)
                        continue;
                    const bool onBar = std::fabs(v) < site.hwU + 0.8f && std::fabs(u) < arm;
                    const bool onStem = std::fabs(u) < site.hwV + 0.8f && (tee ? (v > -1.0f && v < arm) : std::fabs(v) < arm);
                    if (onBar || onStem)
                        t.tex = plainAsphalt, ++repaved;
                }
        }

        // 2. emit the asset: parts outside the straight 52 m, on the missing side of a T, or off the existing surfaces are left out
        const float gx = best.a->gx, gy = best.a->gy;
        IlAssets::KeepFn keep = [&](const IlAssets::Model &, const IlAssets::Sub &sub, const IlAssets::Component &c, float wx, float wy) {
            if (std::fabs(c.c[0] * best.sx) > 56.0f || std::fabs(c.c[1] * best.sy) > 56.0f)
                return false;
            if (tee && c.c[1] < -(gy + 1.0f))
                return false; // the arm that does not exist and the corner blocks beside it
            if (sub.part == "Markings")
                return asphalt.On(wx, wy, site.z, 3.5f);
            auto onSurf = [&](float dx, float dy) { return surf.On(wx + dx, wy + dy, site.z, 3.5f); };
            const bool large = std::max(c.ext[0], c.ext[1]) >= 6.0f;
            const float r = large ? 3.0f : 1.6f;
            return onSurf(0, 0) || onSurf(r, 0) || onSurf(-r, 0) || onSurf(0, r) || onSurf(0, -r);
        };
        (void)gx;
        IlAssets::EmitOptions eo;
        eo.rubber = true, eo.ground = &ground, eo.drapeBelow = 0.4f, eo.lift = 0.04f, eo.anchorSlack = 2.5f, eo.lightScale = 0.92f;
        eo.keep = &keep;
        const size_t t0 = props.size();
        IlAssets::Emit(*this, *best.m, inst, eo, props);
        ownRanges.push_back({t0, props.size()});
        // lamp, signal and sign poles (thin, tall components) stop a car like the other solid props
        size_t nPoles = 0;
        for (const IlAssets::Component &c : best.m->comps) {
            if (c.ext[0] > 0.7f || c.ext[1] > 0.7f || c.ext[2] < 3.0f)
                continue;
            const float mx = c.c[0] * best.sx, my = c.c[1] * best.sy;
            const float wx = site.x + std::cos(inst.yaw) * mx - std::sin(inst.yaw) * my, wy = site.y + std::sin(inst.yaw) * mx + std::cos(inst.yaw) * my;
            if (!keep(*best.m, best.m->subs[c.sub], c, wx, wy))
                continue;
            float g = site.z;
            if (!ground(wx, wy, site.z + 2.5f, g))
                g = site.z;
            solids.push_back({wx, wy, 0.16f, g, g + std::min(9.0f, c.ext[2])}), ++nPoles;
        }
        // everything that was scattered inside the pavement blocks is dropped (the keep-out box of every brick block)
        for (const IlAssets::Component &c : best.m->comps)
            if (best.m->subs[c.sub].part == "Islands" && std::max(c.ext[0], c.ext[1]) >= 6.0f && best.m->mats[best.m->subs[c.sub].mat].name == "IL_BRICK") {
                const float mx = c.c[0] * best.sx, my = c.c[1] * best.sy;
                const float wx = site.x + std::cos(inst.yaw) * mx - std::sin(inst.yaw) * my, wy = site.y + std::sin(inst.yaw) * mx + std::cos(inst.yaw) * my;
                if (tee && c.c[1] < -(gy + 1.0f))
                    continue;
                keepOut.push_back({wx, wy, 0.5f * std::max(c.ext[0] * best.sx, c.ext[1] * best.sy), site.z});
            }
        ++placed;
        printf("[IL] %c site at %.0f %.0f yaw %.0f: %s rot %d scale %.2f x %.2f (half widths %.1f / %.1f), %zu triangles, %zu poles\n", site.kind, site.x, site.y,
               site.yawDeg, best.a->name, best.rot * 90, best.sx, best.sy, site.hwU, site.hwV, props.size() - t0, nPoles);
    }
    printf("[IL] intersections: %zu placed, %zu skipped, %zu road triangles repaved\n", placed, skipped, repaved);
}
