// OpenGL world renderer (ISSUES.md #108): SDL's "opengl" render backend owns the context; the world
// is drawn between SDL batches with fixed-function GL 1.5 (vertex buffers, depth test, linear fog),
// saving and restoring the state SDL caches.
#include "game/indep/World/TrackWorld.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <SDL_opengl.h>

#include "game/indep/Texture/TexturePack.h"
#include "game/indep/World/GLBridge.h"

namespace {
struct Vertex {
    float x, y, z, u, v;
    uint8_t r, g, b, a;
};
} // namespace

void TrackWorld::ReleaseGL()
{
    if (m_glState == 1) {
        for (auto &[key, cell] : m_glCells)
            if (cell.buffer)
                GLBridge::DeleteBuffers(1, &cell.buffer);
        for (auto &[hash, tex] : m_glTextures)
            glDeleteTextures(1, &tex);
        if (m_glPanorama.buffer)
            GLBridge::DeleteBuffers(1, &m_glPanorama.buffer);
    }
    m_glCells.clear();
    m_glPanorama = GLCell();
    m_glTextures.clear();
}

// Texture by name hash from the stream's texture packs, decoded and uploaded on first use;
// missing ones map to a white texture so the vertex colour shows.
unsigned TrackWorld::GLTexture(uint32_t hash)
{
    if (!m_glWhite) {
        const uint32_t white = 0xffffffff;
        glGenTextures(1, &m_glWhite);
        glBindTexture(GL_TEXTURE_2D, m_glWhite);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    if (!hash)
        return m_glWhite;
    // Skins composed at runtime (SetCarSkin): rebuilt when replaced.
    for (uint32_t stale : m_glTextureStale) {
        auto old = m_glTextures.find(stale);
        if (old != m_glTextures.end()) {
            if (old->second != m_glWhite)
                glDeleteTextures(1, &old->second);
            m_glTextures.erase(old);
        }
    }
    m_glTextureStale.clear();
    if (auto skin = m_skinPixels.find(hash); skin != m_skinPixels.end()) {
        auto have = m_glTextures.find(hash);
        if (have != m_glTextures.end())
            return have->second;
        unsigned id = 0;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, CarSkin::kSize, CarSkin::kSize, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     skin->second.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_glTextureMode[id] = 0;
        m_glTextures[hash] = id;
        return id;
    }
    auto it = m_glTextures.find(hash);
    if (it != m_glTextures.end())
        return it->second;
    unsigned id = m_glWhite;
    if (IsBeamTexture(hash) && m_textures.find(hash) == m_textures.end()) {
        // Procedural light beam: the solids map V = 1 at the lamp and V = 0 at the far end, U across the cone.
        constexpr int w = 32, h = 64;
        std::vector<uint32_t> px(w * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float fy = float(y) / (h - 1), fx = 2.0f * float(x) / (w - 1) - 1.0f;
                const float a = 0.42f * std::pow(fy, 1.6f) * std::max(0.0f, 1.0f - fx * fx);
                px[size_t(y) * w + x] = uint32_t(a * 255.0f) << 24 | 200u << 16 | 238u << 8 | 255u; // ABGR: warm white
            }
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_glTextureMode[id] = 2; // additive
        m_glTextures[hash] = id;
        return id;
    }
    auto src = m_textures.find(hash);
    DecodedTexture decoded;
    const TextureInfo *info = src != m_textures.end() ? &src->second.first->At(src->second.second) : nullptr;
    if (info && src->second.first->Decode(*info, decoded) && decoded.width > 0 && decoded.height > 0) {
        // Alpha is opacity only for punch-through and blended textures; force it opaque otherwise.
        const bool blended = info->alphaUsage == 2 && info->alphaBlend != 0;
        if (info->alphaUsage != 1 && !blended)
            for (uint32_t &px : decoded.rgba)
                px |= 0xff000000u;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, decoded.width, decoded.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     decoded.rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        m_glTextureMode[id] = !blended ? 0 : info->alphaBlend == 2 ? 2 : 1;
    }
    m_glTextures[hash] = id;
    return id;
}

bool TrackWorld::RenderGL(SDL_Renderer *renderer, const Camera &cam, int width, int height, const Inset *inset)
{
    const int windowHeight = height;
    if (m_glState == 0)
        m_glState = GLBridge::Available(renderer) ? 1 : -1;
    if (m_glState != 1)
        return false;

    constexpr float kDrawDistance = 900.0f, kNear = 0.3f, kFogStart = 200.0f;
    const float fogColour[4] = {0.55f, 0.62f, 0.72f, 1.0f};

    GLBridge::Scope scope(renderer);
    if (inset) {
        glViewport(inset->x, windowHeight - inset->y - inset->h, inset->w, inset->h);
        glEnable(GL_SCISSOR_TEST);
        glScissor(inset->x, windowHeight - inset->y - inset->h, inset->w, inset->h);
        width = inset->w;
        height = inset->h;
    } else {
        glViewport(0, 0, width, height);
    }
    glDisable(GL_TEXTURE_2D);
    glShadeModel(GL_FLAT);

    // Sky: vertical gradient over the whole view.
    glDisable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glShadeModel(GL_SMOOTH);
    glBegin(GL_QUADS);
    glColor3ub(70, 100, 150);
    glVertex2f(-1, 1);
    glVertex2f(1, 1);
    glColor3ub(140, 158, 184);
    glVertex2f(1, -1);
    glVertex2f(-1, -1);
    glEnd();
    glShadeModel(GL_FLAT);

    // Camera: right-handed view from the same basis as the software path.
    const float cy = std::cos(cam.yaw), sy = std::sin(cam.yaw);
    const float cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
    const float f[3] = {cy * cp, sy * cp, sp};
    const float r[3] = {sy, -cy, 0.0f};
    const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    const float c[3] = {cam.x, cam.y, cam.z};
    const float tanHalf = std::tan(35.0f * 3.14159265f / 180.0f); // 70 degree horizontal FOV
    const float aspect = float(height) / float(std::max(width, 1));
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    const float side = inset && inset->mirror ? -1.0f : 1.0f; // a mirror flips the image left-right
    glFrustum(-kNear * tanHalf * side, kNear * tanHalf * side, -kNear * tanHalf * aspect, kNear * tanHalf * aspect, kNear,
              kDrawDistance);
    const float view[16] = {r[0], u[0], -f[0], 0, r[1], u[1], -f[1], 0, r[2], u[2], -f[2], 0,
                            -(r[0] * c[0] + r[1] * c[1] + r[2] * c[2]), -(u[0] * c[0] + u[1] * c[1] + u[2] * c[2]),
                            f[0] * c[0] + f[1] * c[1] + f[2] * c[2], 1};
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(view);

    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, kFogStart);
    glFogf(GL_FOG_END, kDrawDistance);
    glFogfv(GL_FOG_COLOR, fogColour);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_ALPHA_TEST); // punch-through textures (trees, fences); opaque ones have alpha 255
    glAlphaFunc(GL_GREATER, 0.25f);
    glShadeModel(GL_SMOOTH);

    static std::vector<Vertex> scratch;

    // Upload render triangles once: grouped by texture, one draw range per texture.
    auto upload = [&](GLCell &cell, std::vector<RTri> &render) {
        if (render.empty())
            return;
        std::stable_sort(render.begin(), render.end(), [](const RTri &a, const RTri &b) { return a.tex < b.tex; });
        scratch.clear();
        cell.ranges.clear();
        for (size_t i = 0; i < render.size(); ++i) {
            if (i == 0 || render[i].tex != render[i - 1].tex) {
                unsigned tex = GLTexture(render[i].tex);
                auto mode = m_glTextureMode.find(tex);
                cell.ranges.push_back({tex, int(scratch.size()), 0, mode == m_glTextureMode.end() ? 0 : mode->second});
            }
            for (const RVertex &v : render[i].v)
                scratch.push_back({v.x, v.y, v.z, v.u, v.v, v.r, v.g, v.b, v.a});
            cell.ranges.back().count += 3;
        }
        if (!cell.buffer)
            GLBridge::GenBuffers(1, &cell.buffer);
        GLBridge::BindArrayBuffer(cell.buffer);
        GLBridge::ArrayBufferData(ptrdiff_t(scratch.size() * sizeof(Vertex)), scratch.data());
        cell.vertices = int(scratch.size());
        std::vector<RTri>().swap(render); // the GPU copy is the render data from now on
    };
    auto drawCell = [](const GLCell &cell, int pass) {
        GLBridge::BindArrayBuffer(cell.buffer);
        glVertexPointer(3, GL_FLOAT, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, x)));
        glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, u)));
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, r)));
        for (const GLRange &range : cell.ranges) {
            if ((range.mode != 0) != (pass == 1))
                continue;
            if (pass == 1)
                glBlendFunc(GL_SRC_ALPHA, range.mode == 2 ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
            glBindTexture(GL_TEXTURE_2D, range.texture);
            glDrawArrays(GL_TRIANGLES, range.first, range.count);
        }
    };

    // Panorama backdrop (PAN_* instances): far projection, no fog, no depth -- the world covers it.
    upload(m_glPanorama, m_panorama);
    if (m_glPanorama.buffer) {
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glFrustum(-kNear * tanHalf, kNear * tanHalf, -kNear * tanHalf * aspect, kNear * tanHalf * aspect, kNear * 4,
                  20000.0f);
        glMatrixMode(GL_MODELVIEW);
        glDisable(GL_FOG);
        glDisable(GL_DEPTH_TEST);
        drawCell(m_glPanorama, 0);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_FOG);
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
    }

    // World cells in range and inside the horizontal view cone.
    const int reach = int(std::ceil(kDrawDistance / kCellSize));
    const int ccx = int(std::floor(cam.x / kCellSize)), ccy = int(std::floor(cam.y / kCellSize));
    const float coneSlope = tanHalf * 1.05f;
    std::vector<const GLCell *> drawn;
    for (int gx = ccx - reach; gx <= ccx + reach; ++gx)
        for (int gy = ccy - reach; gy <= ccy + reach; ++gy) {
            auto it = m_cells.find(CellKey(gx, gy));
            if (it == m_cells.end() || it->second.tris.empty())
                continue;
            float mx = (gx + 0.5f) * kCellSize - cam.x, my = (gy + 0.5f) * kCellSize - cam.y;
            float along = mx * cy + my * sy, across = std::fabs(mx * sy - my * cy);
            if (!m_scene && (std::hypot(mx, my) > kDrawDistance + kCellSize || along < -kCellSize ||
                             across > along * coneSlope + kCellSize * 0.75f))
                continue;
            GLCell &cell = m_glCells[it->first];
            upload(cell, it->second.render);
            if (cell.buffer)
                drawn.push_back(&cell);
        }
    // Opaque and punch-through first, then blended/additive with depth writes off.
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            glEnable(GL_BLEND);
            glDepthMask(GL_FALSE);
        }
        for (const GLCell *cell : drawn)
            drawCell(*cell, pass);
    }
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);

    // Cars: rebuilt every frame from client memory, one draw per texture run.
    GLBridge::BindArrayBuffer(0);
    for (const Car &car : m_cars) {
        if (car.render.empty())
            continue;
        scratch.clear();
        const bool lit = car.gloss.size() == car.render.size() && !getenv("NFSU2_CAR_FLAT");
        for (size_t ti = 0; ti < car.render.size(); ++ti) {
            const RTri &t = car.render[ti];
            for (int k = 0; k < 3; ++k) {
                const RVertex &v = t.v[k];
                uint8_t cr = v.r, cg = v.g, cb = v.b;
                if (lit && car.gloss[ti]) {
                    // Per-vertex paint lighting: hemisphere ambient + sun diffuse + sun specular + a sky reflection with
                    // a Fresnel term (the original reflects an environment map on the body, ISSUES #253).
                    const float *n3 = &car.normals[ti * 9 + size_t(k) * 3];
                    float nx = n3[0], ny = n3[1], nz = n3[2];
                    const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (nl > 1e-6f)
                        nx /= nl, ny /= nl, nz /= nl;
                    float vx = cam.x - v.x, vy = cam.y - v.y, vz = cam.z - v.z;
                    const float vl = std::max(1e-6f, std::sqrt(vx * vx + vy * vy + vz * vz));
                    vx /= vl, vy /= vl, vz /= vl;
                    constexpr float lx = 0.40f, ly = 0.30f, lz = 0.866f;
                    const float ll = std::sqrt(lx * lx + ly * ly + lz * lz);
                    const float ndl = std::max(0.0f, (nx * lx + ny * ly + nz * lz) / ll);
                    const float ndv = std::max(0.0f, nx * vx + ny * vy + nz * vz);
                    const float hemi = 0.30f + 0.32f * (0.5f + 0.5f * nz);
                    const float gl = car.gloss[ti] == 2 ? 1.0f : 0.6f;
                    // Blinn specular
                    float hx = lx / ll + vx, hy = ly / ll + vy, hz = lz / ll + vz;
                    const float hl = std::max(1e-6f, std::sqrt(hx * hx + hy * hy + hz * hz));
                    const float ndh = std::max(0.0f, (nx * hx + ny * hy + nz * hz) / hl);
                    const float spec = std::pow(ndh, 48.0f) * 0.55f * gl;
                    // Reflection of a sky gradient
                    const float rz = 2.0f * ndv * nz - vz;
                    const float sky = std::clamp(0.5f + 0.5f * rz, 0.0f, 1.0f);
                    const float fres = (0.04f + 0.55f * std::pow(1.0f - ndv, 3.0f)) * gl;
                    const float er = 0.10f + 0.55f * sky, eg = 0.11f + 0.64f * sky, eb = 0.13f + 0.80f * sky;
                    const float diff = hemi + 0.62f * ndl;
                    const float *alb = &car.albedo[ti * 3];
                    auto mix = [&](float a, float e) {
                        return uint8_t(std::clamp((a * diff * (1.0f - fres) + e * fres + spec) * 255.0f, 0.0f, 255.0f));
                    };
                    cr = mix(alb[0], er), cg = mix(alb[1], eg), cb = mix(alb[2], eb);
                }
                scratch.push_back({v.x, v.y, v.z, v.u, v.v, cr, cg, cb, v.a});
            }
        }
        glVertexPointer(3, GL_FLOAT, sizeof(Vertex), &scratch[0].x);
        glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex), &scratch[0].u);
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), &scratch[0].r);
        for (size_t first = 0; first < car.render.size();) {
            size_t last = first;
            while (last < car.render.size() && car.render[last].tex == car.render[first].tex)
                ++last;
            glBindTexture(GL_TEXTURE_2D, GLTexture(car.render[first].tex));
            glDrawArrays(GL_TRIANGLES, GLint(first * 3), GLsizei((last - first) * 3));
            first = last;
        }
    }
    if (inset)
        glDisable(GL_SCISSOR_TEST);
    return true;
}
