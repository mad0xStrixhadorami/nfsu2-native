#include "game/indep/World/IlAssets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

#include "stb_image.h"

namespace IlAssets {
namespace {

std::map<std::string, std::unique_ptr<Model>> g_models;
std::string g_dir;
bool g_dirTried = false;

bool Read(FILE *f, void *dst, size_t n) { return fread(dst, 1, n, f) == n; }

std::string FixedString(const char *p, size_t n)
{
    size_t len = 0;
    while (len < n && p[len])
        ++len;
    return std::string(p, len);
}

// The texture's world hash: a fixed prefix (clear of the generated sign atlases 0x5e5xxxxx) plus an FNV-1a of the name.
uint32_t TexHash(const std::string &name)
{
    uint32_t h = 2166136261u;
    for (char c : name)
        h = (h ^ uint8_t(c)) * 16777619u;
    return 0x5e600000u | (h & 0x000fffffu);
}

// Connected components of every sub-mesh (union-find over the triangles' vertices).
void BuildComponents(Model &m)
{
    for (uint32_t si = 0; si < m.subs.size(); ++si) {
        Sub &s = m.subs[si];
        std::vector<uint32_t> parent(s.v.size());
        for (uint32_t i = 0; i < parent.size(); ++i)
            parent[i] = i;
        auto find = [&](uint32_t a) {
            while (parent[a] != a)
                a = parent[a] = parent[parent[a]];
            return a;
        };
        for (size_t t = 0; t + 2 < s.idx.size(); t += 3) {
            const uint32_t a = find(s.idx[t]), b = find(s.idx[t + 1]), c = find(s.idx[t + 2]);
            parent[b] = a, parent[find(c)] = a;
        }
        std::vector<int64_t> map(s.v.size(), -1);
        std::vector<std::pair<std::array<float, 3>, std::array<float, 3>>> box; // lo, hi per local component
        s.comp.assign(s.v.size(), 0);
        std::vector<char> used(s.v.size(), 0);
        for (uint32_t i : s.idx)
            used[i] = 1;
        const uint32_t base = uint32_t(m.comps.size());
        for (uint32_t i = 0; i < s.v.size(); ++i) {
            if (!used[i])
                continue;
            const uint32_t r = find(i);
            if (map[r] < 0) {
                map[r] = int64_t(box.size());
                box.push_back({{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}});
            }
            auto &b = box[size_t(map[r])];
            for (int k = 0; k < 3; ++k)
                b.first[size_t(k)] = std::min(b.first[size_t(k)], s.v[i].p[k]), b.second[size_t(k)] = std::max(b.second[size_t(k)], s.v[i].p[k]);
            s.comp[i] = base + uint32_t(map[r]);
        }
        for (auto &b : box) {
            Component c;
            c.sub = si;
            for (size_t k = 0; k < 3; ++k)
                c.c[k] = 0.5f * (b.first[k] + b.second[k]), c.ext[k] = b.second[k] - b.first[k];
            c.zLo = b.first[2];
            m.comps.push_back(c);
        }
    }
}

} // namespace

size_t Model::Triangles() const
{
    size_t n = 0;
    for (const Sub &s : subs)
        n += s.idx.size() / 3;
    return n;
}

std::string Dir()
{
    if (g_dirTried)
        return g_dir;
    g_dirTried = true;
    std::vector<std::string> roots;
    if (const char *d = getenv("NFSU2_ASSETS"))
        roots.push_back(d);
    for (const char *c : {"assets/israel", "../assets/israel", "../../assets/israel"})
        roots.push_back(c);
    for (const std::string &r : roots) {
        const std::string probe = r + "/il/tex/IL_brick.png";
        if (FILE *f = fopen(probe.c_str(), "rb")) {
            fclose(f);
            g_dir = r + "/il";
            break;
        }
    }
    return g_dir;
}

const Model *Get(const std::string &name)
{
    if (getenv("NFSU2_NO_IL_ASSETS"))
        return nullptr;
    auto it = g_models.find(name);
    if (it != g_models.end())
        return it->second.get();
    g_models[name] = nullptr; // a failed load is remembered
    const std::string dir = Dir();
    if (dir.empty())
        return nullptr;
    FILE *f = fopen((dir + "/" + name + ".ilm").c_str(), "rb");
    if (!f)
        return nullptr;
    auto m = std::make_unique<Model>();
    m->name = name;
    char magic[4];
    uint32_t nMat = 0, nSub = 0;
    float bbox[6];
    bool ok = Read(f, magic, 4) && memcmp(magic, "ILM2", 4) == 0 && Read(f, &nMat, 4) && Read(f, &nSub, 4) && nMat < 256 &&
              nSub < 4096 && Read(f, bbox, sizeof(bbox));
    for (uint32_t i = 0; ok && i < nMat; ++i) {
        char nm[32], tx[32];
        float rec[8];
        uint32_t flags = 0;
        float mr[2];
        ok = Read(f, nm, 32) && Read(f, rec, sizeof(rec)) && Read(f, &flags, 4) && Read(f, tx, 32) && Read(f, mr, 8);
        if (!ok)
            break;
        Material mat;
        mat.name = FixedString(nm, 32), mat.tex = FixedString(tx, 32);
        memcpy(mat.rgb, rec, 12), memcpy(mat.emis, rec + 3, 12);
        mat.emisStrength = rec[6], mat.alpha = rec[7], mat.flags = flags, mat.metal = mr[0], mat.rough = mr[1];
        m->mats.push_back(mat);
    }
    for (uint32_t i = 0; ok && i < nSub; ++i) {
        Sub s;
        uint32_t nv = 0, ni = 0;
        char part[16];
        ok = Read(f, &s.mat, 4) && Read(f, part, 16) && Read(f, &nv, 4) && Read(f, &ni, 4) && nv < 4000000 && ni < 12000000 && s.mat < m->mats.size();
        if (!ok)
            break;
        s.part = FixedString(part, 16);
        s.v.resize(nv), s.idx.resize(ni);
        ok = Read(f, s.v.data(), size_t(nv) * sizeof(Vertex)) && Read(f, s.idx.data(), size_t(ni) * 4);
        for (uint32_t k = 0; ok && k < ni; ++k)
            ok = s.idx[k] < nv;
        m->subs.push_back(std::move(s));
    }
    fclose(f);
    if (!ok) {
        printf("[IL] %s: unreadable\n", name.c_str());
        return nullptr;
    }
    memcpy(m->lo, bbox, 12), memcpy(m->hi, bbox + 3, 12);
    BuildComponents(*m);
    printf("[IL] %s: %zu materials, %zu sub-meshes, %zu triangles, %.1f x %.1f x %.1f m\n", name.c_str(), m->mats.size(),
           m->subs.size(), m->Triangles(), m->hi[0] - m->lo[0], m->hi[1] - m->lo[1], m->hi[2] - m->lo[2]);
    g_models[name] = std::move(m);
    return g_models[name].get();
}

void RegisterTextures(TrackWorld &world, const Model &m)
{
    for (const Material &mat : m.mats) {
        if (mat.tex.empty())
            continue;
        const uint32_t hash = TexHash(mat.tex);
        if (world.HasCustomTexture(hash))
            continue;
        int w = 0, h = 0, n = 0;
        unsigned char *px = stbi_load((Dir() + "/tex/" + mat.tex + ".png").c_str(), &w, &h, &n, 4);
        if (!px) {
            printf("[IL] texture %s missing\n", mat.tex.c_str());
            continue;
        }
        std::vector<uint32_t> rgba(size_t(w) * h);
        memcpy(rgba.data(), px, rgba.size() * 4);
        stbi_image_free(px);
        world.AddCustomTexture(hash, w, h, std::move(rgba), 0);
    }
}

void ToWorld(const Instance &inst, float mx, float my, float mz, float &wx, float &wy, float &wz)
{
    const float c = std::cos(inst.yaw), s = std::sin(inst.yaw);
    const float ax = mx * inst.sx, ay = my * inst.sy;
    wx = inst.x + c * ax - s * ay;
    wy = inst.y + s * ax + c * ay;
    wz = inst.z + mz * inst.sz;
}

size_t Emit(TrackWorld &world, const Model &m, const Instance &inst, const EmitOptions &opt,
            std::vector<TrackWorld::RTriPublic> &out)
{
    RegisterTextures(world, m);
    const float c = std::cos(inst.yaw), s = std::sin(inst.yaw);
    // The same two-sided sun + sky term the procedural props use, taken from the smooth vertex normals.
    const float lx = 0.35f, ly = 0.2f, lz = 0.9f, ll = std::sqrt(lx * lx + ly * ly + lz * lz);
    const size_t before = out.size();
    const float shape = opt.rubber ? std::clamp(std::sqrt(inst.sx * inst.sy), 0.85f, 1.15f) : 1.0f;
    // Per component (rubber mode): keep decision and the ground height under its centre, decided once.
    struct CompState {
        int8_t keep = -1;
        float ground = 0;
        bool haveGround = false;
        bool small = false;
        float wx = 0, wy = 0;
    };
    std::vector<CompState> cs(m.comps.size());
    auto compState = [&](uint32_t ci) -> CompState & {
        CompState &st = cs[ci];
        if (st.keep >= 0)
            return st;
        const Component &cp = m.comps[ci];
        st.small = std::max(cp.ext[0], cp.ext[1]) < opt.smallLimit;
        const float mx = opt.rubber ? cp.c[0] * inst.sx : cp.c[0] * inst.sx, my = opt.rubber ? cp.c[1] * inst.sy : cp.c[1] * inst.sy;
        st.wx = inst.x + c * mx - s * my, st.wy = inst.y + s * mx + c * my;
        st.keep = 1;
        if (opt.keep && !(*opt.keep)(m, m.subs[cp.sub], cp, st.wx, st.wy))
            st.keep = 0;
        if (st.keep && opt.ground)
            st.haveGround = (*opt.ground)(st.wx, st.wy, inst.z + opt.anchorSlack, st.ground);
        return st;
    };
    struct WV {
        float x, y, z, u, v;
        uint8_t r, g, b;
    };
    std::vector<WV> wv;
    for (const Sub &sub : m.subs) {
        const Material &mat = m.mats[sub.mat];
        if (opt.dropBlend && (mat.flags & 2))
            continue;
        const bool emissive = (mat.flags & 1) != 0;
        const uint32_t tex = mat.tex.empty() ? 0 : TexHash(mat.tex);
        wv.resize(sub.v.size());
        for (size_t i = 0; i < sub.v.size(); ++i) {
            const Vertex &v = sub.v[i];
            float wx, wy, wz;
            if (opt.rubber) {
                const Component &cp = m.comps[sub.comp[i]];
                const CompState &st = compState(sub.comp[i]);
                float lx2, ly2;
                if (st.small)
                    lx2 = inst.sx * cp.c[0] + shape * (v.p[0] - cp.c[0]), ly2 = inst.sy * cp.c[1] + shape * (v.p[1] - cp.c[1]);
                else
                    lx2 = inst.sx * v.p[0], ly2 = inst.sy * v.p[1];
                wx = inst.x + c * lx2 - s * ly2, wy = inst.y + s * lx2 + c * ly2;
                wz = inst.z + v.p[2];
                if (opt.ground) {
                    float g;
                    if (v.p[2] <= opt.drapeBelow && !st.small && (*opt.ground)(wx, wy, inst.z + opt.anchorSlack, g))
                        wz = g + v.p[2];
                    else if (st.haveGround)
                        wz = st.ground + v.p[2];
                }
            } else {
                const float mz = opt.cutHi > opt.cutLo ? (v.p[2] <= opt.cutLo ? v.p[2] : v.p[2] >= opt.cutHi ? v.p[2] - (opt.cutHi - opt.cutLo) : opt.cutLo) : v.p[2];
                ToWorld(inst, v.p[0], v.p[1], mz, wx, wy, wz);
                if (opt.ground && v.p[2] * inst.sz <= opt.drapeBelow) {
                    float g;
                    if ((*opt.ground)(wx, wy, inst.z + opt.anchorSlack, g))
                        wz = g + v.p[2] * inst.sz;
                }
            }
            wz += opt.lift;
            // the normal turns with the yaw; a non-uniform scale tilts it slightly, which the flat-ish light ignores
            const float nx = c * v.n[0] - s * v.n[1], ny = s * v.n[0] + c * v.n[1], nz = v.n[2];
            const float lit = std::fabs(nx * lx + ny * ly + nz * lz) / ll;
            const float k = (0.52f + 0.55f * lit) * opt.lightScale;
            float r = mat.rgb[0], g = mat.rgb[1], b = mat.rgb[2];
            if (tex)
                r = g = b = 1.0f;
            if (emissive) { // lamps and lit lenses: the lit colour, no shading
                r = std::min(1.0f, r * 0.5f + mat.emis[0] * mat.emisStrength);
                g = std::min(1.0f, g * 0.5f + mat.emis[1] * mat.emisStrength);
                b = std::min(1.0f, b * 0.5f + mat.emis[2] * mat.emisStrength);
                wv[i] = {wx, wy, wz, v.uv[0], v.uv[1], uint8_t(r * 255.0f), uint8_t(g * 255.0f), uint8_t(b * 255.0f)};
            } else {
                wv[i] = {wx, wy, wz, v.uv[0], v.uv[1], uint8_t(std::min(255.0f, r * k * 255.0f)),
                         uint8_t(std::min(255.0f, g * k * 255.0f)), uint8_t(std::min(255.0f, b * k * 255.0f))};
            }
        }
        for (size_t t = 0; t + 2 < sub.idx.size(); t += 3) {
            if (opt.rubber && cs[sub.comp[sub.idx[t]]].keep == 0)
                continue;
            if (opt.cutHi > opt.cutLo) { // a triangle wholly inside the removed band is gone
                bool inBand = true;
                for (int k = 0; k < 3; ++k) {
                    const float z = sub.v[sub.idx[t + size_t(k)]].p[2];
                    inBand &= z >= opt.cutLo && z <= opt.cutHi;
                }
                if (inBand)
                    continue;
            }
            TrackWorld::RTriPublic tri;
            tri.tex = tex;
            for (int k = 0; k < 3; ++k) {
                const WV &w = wv[sub.idx[t + size_t(k)]];
                tri.v[k].x = w.x, tri.v[k].y = w.y, tri.v[k].z = w.z, tri.v[k].u = w.u, tri.v[k].v = w.v;
                tri.v[k].r = w.r, tri.v[k].g = w.g, tri.v[k].b = w.b, tri.v[k].a = 254;
            }
            out.push_back(tri);
        }
    }
    return out.size() - before;
}

} // namespace IlAssets
