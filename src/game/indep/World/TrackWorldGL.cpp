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

namespace {
typedef void(APIENTRY *PFNGENBUFFERS)(GLsizei, GLuint *);
typedef void(APIENTRY *PFNBINDBUFFER)(GLenum, GLuint);
typedef void(APIENTRY *PFNBUFFERDATA)(GLenum, ptrdiff_t, const void *, GLenum);
typedef void(APIENTRY *PFNUSEPROGRAM)(GLuint);
typedef void(APIENTRY *PFNDELETEBUFFERS)(GLsizei, const GLuint *);
PFNDELETEBUFFERS pglDeleteBuffers;
PFNGENBUFFERS pglGenBuffers;
PFNBINDBUFFER pglBindBuffer;
PFNBUFFERDATA pglBufferData;
PFNUSEPROGRAM pglUseProgram;

constexpr GLenum kArrayBuffer = 0x8892;       // GL_ARRAY_BUFFER
constexpr GLenum kStaticDraw = 0x88E4;        // GL_STATIC_DRAW
constexpr GLenum kCurrentProgram = 0x8B8D;    // GL_CURRENT_PROGRAM
constexpr GLenum kArrayBufferBinding = 0x8894; // GL_ARRAY_BUFFER_BINDING

struct Vertex {
    float x, y, z, u, v;
    uint8_t r, g, b, a;
};
} // namespace

void TrackWorld::ReleaseGL()
{
    if (m_glState == 1) {
        for (auto &[key, cell] : m_glCells)
            if (cell.buffer && pglDeleteBuffers)
                pglDeleteBuffers(1, &cell.buffer);
        for (auto &[hash, tex] : m_glTextures)
            glDeleteTextures(1, &tex);
    }
    m_glCells.clear();
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
    auto it = m_glTextures.find(hash);
    if (it != m_glTextures.end())
        return it->second;
    unsigned id = m_glWhite;
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

bool TrackWorld::RenderGL(SDL_Renderer *renderer, const Camera &cam, int width, int height)
{
    if (m_glState == 0) {
        SDL_RendererInfo info;
        m_glState = -1;
        if (!getenv("NFSU2_SOFTWARE_WORLD") && SDL_GetRendererInfo(renderer, &info) == 0 &&
            strcmp(info.name, "opengl") == 0) {
            pglGenBuffers = reinterpret_cast<PFNGENBUFFERS>(SDL_GL_GetProcAddress("glGenBuffers"));
            pglBindBuffer = reinterpret_cast<PFNBINDBUFFER>(SDL_GL_GetProcAddress("glBindBuffer"));
            pglBufferData = reinterpret_cast<PFNBUFFERDATA>(SDL_GL_GetProcAddress("glBufferData"));
            pglUseProgram = reinterpret_cast<PFNUSEPROGRAM>(SDL_GL_GetProcAddress("glUseProgram"));
            pglDeleteBuffers = reinterpret_cast<PFNDELETEBUFFERS>(SDL_GL_GetProcAddress("glDeleteBuffers"));
            int depth = 0;
            SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &depth);
            if (pglGenBuffers && pglBindBuffer && pglBufferData && pglUseProgram && depth >= 16)
                m_glState = 1;
            printf("[World] OpenGL renderer %s (%s, depth %d)\n", m_glState == 1 ? "on" : "unavailable",
                   glGetString(GL_RENDERER) ? reinterpret_cast<const char *>(glGetString(GL_RENDERER)) : "?", depth);
        }
    }
    if (m_glState != 1)
        return false;

    constexpr float kDrawDistance = 900.0f, kNear = 0.3f, kFogStart = 200.0f;
    const float fogColour[4] = {0.55f, 0.62f, 0.72f, 1.0f};

    // Let SDL submit its queued commands (the clear), then take over the GL state.
    SDL_RenderFlush(renderer);
    GLint program = 0, arrayBuffer = 0;
    glGetIntegerv(kCurrentProgram, &program);
    glGetIntegerv(kArrayBufferBinding, &arrayBuffer);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    pglUseProgram(0);

    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE); // strip winding is not known
    glDisable(GL_LIGHTING);
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
    glFrustum(-kNear * tanHalf, kNear * tanHalf, -kNear * tanHalf * aspect, kNear * tanHalf * aspect, kNear,
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

    auto toVertices = [](const std::vector<Tri> &tris, std::vector<Vertex> &out) {
        out.clear();
        out.reserve(tris.size() * 3);
        for (const Tri &t : tris)
            for (int k = 0; k < 3; ++k)
                out.push_back({t.v[k * 3], t.v[k * 3 + 1], t.v[k * 3 + 2], 0, 0, t.shade[0], t.shade[1], t.shade[2], 255});
    };
    static std::vector<Vertex> scratch;

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
            if (std::hypot(mx, my) > kDrawDistance + kCellSize || along < -kCellSize ||
                across > along * coneSlope + kCellSize * 0.75f)
                continue;
            GLCell &cell = m_glCells[it->first];
            std::vector<RTri> &render = it->second.render;
            if (!render.empty()) {
                // Upload once: triangles grouped by texture, one draw range per texture.
                std::stable_sort(render.begin(), render.end(),
                                 [](const RTri &a, const RTri &b) { return a.tex < b.tex; });
                scratch.clear();
                cell.ranges.clear();
                for (size_t i = 0; i < render.size(); ++i) {
                    if (i == 0 || render[i].tex != render[i - 1].tex)
                    {
                        unsigned tex = GLTexture(render[i].tex);
                        auto mode = m_glTextureMode.find(tex);
                        cell.ranges.push_back({tex, int(scratch.size()), 0, mode == m_glTextureMode.end() ? 0 : mode->second});
                    }
                    for (const RVertex &v : render[i].v)
                        scratch.push_back({v.x, v.y, v.z, v.u, v.v, v.r, v.g, v.b, v.a});
                    cell.ranges.back().count += 3;
                }
                if (!cell.buffer)
                    pglGenBuffers(1, &cell.buffer);
                pglBindBuffer(kArrayBuffer, cell.buffer);
                pglBufferData(kArrayBuffer, ptrdiff_t(scratch.size() * sizeof(Vertex)), scratch.data(), kStaticDraw);
                cell.vertices = int(scratch.size());
                std::vector<RTri>().swap(render); // the GPU copy is the render data from now on
            }
            if (!cell.buffer)
                continue;
            drawn.push_back(&cell);
        }
    // Opaque and punch-through first, then blended/additive with depth writes off.
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            glEnable(GL_BLEND);
            glDepthMask(GL_FALSE);
        }
        for (const GLCell *cell : drawn) {
            pglBindBuffer(kArrayBuffer, cell->buffer);
            glVertexPointer(3, GL_FLOAT, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, x)));
            glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, u)));
            glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, r)));
            for (const GLRange &range : cell->ranges) {
                if ((range.mode != 0) != (pass == 1))
                    continue;
                if (pass == 1)
                    glBlendFunc(GL_SRC_ALPHA, range.mode == 2 ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
                glBindTexture(GL_TEXTURE_2D, range.texture);
                glDrawArrays(GL_TRIANGLES, range.first, range.count);
            }
        }
    }
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);

    // Cars: rebuilt every frame from client memory (untextured: white texture).
    pglBindBuffer(kArrayBuffer, 0);
    glBindTexture(GL_TEXTURE_2D, GLTexture(0));
    glShadeModel(GL_FLAT);
    for (const Car &car : m_cars) {
        if (car.tris.empty())
            continue;
        toVertices(car.tris, scratch);
        glVertexPointer(3, GL_FLOAT, sizeof(Vertex), &scratch[0].x);
        glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex), &scratch[0].u);
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), &scratch[0].r);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(scratch.size()));
    }

    // Hand the state back to SDL.
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopClientAttrib();
    glPopAttrib();
    pglBindBuffer(kArrayBuffer, GLuint(arrayBuffer));
    pglUseProgram(GLuint(program));
    return true;
}
