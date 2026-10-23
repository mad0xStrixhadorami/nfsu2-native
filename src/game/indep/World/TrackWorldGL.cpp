// OpenGL world renderer (ISSUES.md #108): SDL's "opengl" render backend owns the context; the world
// is drawn between SDL batches with fixed-function GL 1.5 (vertex buffers, depth test, linear fog),
// saving and restoring the state SDL caches.
#include "game/indep/World/TrackWorld.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <SDL_opengl.h>

#include "game/indep/Texture/TexturePack.h"
#include "game/indep/World/GLBridge.h"
#include "game/indep/World/GLShader.h"
#include "game/indep/World/Shaders.h"

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
    m_glShadow = 0;
    ReleasePost();
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
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
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
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, decoded.width, decoded.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     decoded.rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY_EXT */, 8.0f); // harmless where unsupported
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        m_glTextureMode[id] = !blended ? 0 : info->alphaBlend == 2 ? 2 : 1;
    }
    m_glTextures[hash] = id;
    return id;
}

// Which textures carry office / hotel / tower windows (ISSUES #266): by name. 2 = glass curtain walls (every texel is glass),
// 1 = facades with windows baked in (the shader finds the window texels). Awnings, canopies, doors and shop fronts are not rooms.
int TrackWorld::InteriorClassOf(uint32_t hash)
{
    auto it = m_interiorClass.find(hash);
    if (it != m_interiorClass.end())
        return it->second;
    int cls = 0;
    auto src = m_textures.find(hash);
    if (src != m_textures.end() && m_progInterior) {
        std::string n = src->second.first->At(src->second.second).name;
        auto has = [&](const char *k) { return n.find(k) != std::string::npos; };
        const bool excluded = has("AWNING") || has("CANOPY") || has("DOOR") || has("LOBBY") || has("SHOP") || has("STORE") || has("HIGHLIGHT") ||
                              has("DRAPES") || has("SHUTTER") || has("LV") || n.rfind("LOD_", 0) == 0 || n.rfind("SFX_", 0) == 0 ||
                              has("CORREG") || has("BESTBUY") || has("MUSEUM") || has("CAFE") || has("MALL") || has("THEATRE") || has("DRIVEIN");
        if (!excluded && (has("GLASSBLUEWIN") || has("GREENWINDOWTOWER") || has("WINDGLASS")))
            cls = 2;
        else if (!excluded && (has("OFFICE") || has("TOWER") || has("HOTEL") || has("CONDO") || has("APART") || has("WIN") || has("CT4STORY")))
            cls = 1;
    }
    m_interiorClass[hash] = cls;
    if (cls && getenv("NFSU2_INTERIOR_DEBUG"))
        printf("[Interior] class %d: %s\n", cls, m_textures.count(hash) ? m_textures[hash].first->At(m_textures[hash].second).name.c_str() : "?");
    return cls;
}

void TrackWorld::InitShaders()
{
    m_shadersTried = true;
    if (!GLShader::Load())
        return;
    m_progWorld = GLShader::Build("world", Shaders::kWorldVS, Shaders::kWorldFS);
    m_progInterior = getenv("NFSU2_NO_INTERIORS") ? 0 : GLShader::Build("interior", Shaders::kInteriorVS, Shaders::kInteriorFS);
    m_progCar = GLShader::Build("car", Shaders::kCarVS, Shaders::kCarFS, {{1, "aNormal"}, {2, "aGloss"}, {3, "aAO"}});
    m_progBright = GLShader::Build("bright", Shaders::kPostVS, Shaders::kBrightFS);
    m_progBlur = GLShader::Build("blur", Shaders::kPostVS, Shaders::kBlurFS);
    m_progComposite = GLShader::Build("composite", Shaders::kPostVS, Shaders::kCompositeFS);
    printf("[GLSL] programs: world %u, interior %u, car %u, post %u / %u / %u\n", m_progWorld, m_progInterior, m_progCar, m_progBright, m_progBlur, m_progComposite);
}

void TrackWorld::ReleasePost()
{
    if (m_postScene)
        glDeleteTextures(1, &m_postScene);
    for (unsigned &t : m_postBloom)
        if (t)
            glDeleteTextures(1, &t), t = 0;
    if (m_postFboScene)
        GLShader::DeleteFramebuffer(m_postFboScene);
    for (unsigned &f : m_postFbo)
        if (f)
            GLShader::DeleteFramebuffer(f), f = 0;
    m_postScene = m_postFboScene = 0;
    m_postW = m_postH = 0;
}

// Post-processing of the finished world frame (ISSUES #263): the window framebuffer is resolved into a texture, its bright
// parts are extracted at quarter size, blurred and added back, then a filmic curve, a touch of saturation and a vignette
// are applied while writing the result to the window. Runs between the world and SDL's HUD draws.
void TrackWorld::PostProcess(int w, int h)
{
    if (!m_progBright || !m_progBlur || !m_progComposite || w < 64 || h < 64 || getenv("NFSU2_NO_POST"))
        return;
    const int bw = std::max(16, w / 4), bh = std::max(16, h / 4);
    auto makeTexture = [](unsigned &tex, int W, int H) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    if (w != m_postW || h != m_postH) {
        ReleasePost();
        makeTexture(m_postScene, w, h);
        makeTexture(m_postBloom[0], bw, bh);
        makeTexture(m_postBloom[1], bw, bh);
        m_postFboScene = GLShader::GenFramebuffer();
        GLShader::BindFramebuffer(m_postFboScene);
        bool ok = GLShader::AttachColor(m_postScene);
        for (int i = 0; i < 2; ++i) {
            m_postFbo[i] = GLShader::GenFramebuffer();
            GLShader::BindFramebuffer(m_postFbo[i]);
            ok &= GLShader::AttachColor(m_postBloom[i]);
        }
        GLShader::BindFramebuffer(0);
        m_postW = w, m_postH = h;
        if (!ok) {
            fprintf(stderr, "[GLSL] post-processing framebuffers incomplete: disabled\n");
            ReleasePost();
            m_progComposite = 0;
            return;
        }
    }
    GLShader::BlitWindowTo(m_postFboScene, w, h); // resolves the multisampled frame
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    auto quad = [] {
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0), glVertex2f(-1, -1);
        glTexCoord2f(1, 0), glVertex2f(1, -1);
        glTexCoord2f(1, 1), glVertex2f(1, 1);
        glTexCoord2f(0, 1), glVertex2f(-1, 1);
        glEnd();
    };
    glEnable(GL_TEXTURE_2D);
    // 1. bright pass into the quarter-size target
    GLShader::BindFramebuffer(m_postFbo[0]);
    glViewport(0, 0, bw, bh);
    GLShader::Use(m_progBright);
    GLShader::Set1i(GLShader::Uniform(m_progBright, "uScene"), 0);
    GLShader::Set2f(GLShader::Uniform(m_progBright, "uTexel"), 1.0f / w, 1.0f / h);
    GLShader::Set1f(GLShader::Uniform(m_progBright, "uThreshold"), 0.62f);
    glBindTexture(GL_TEXTURE_2D, m_postScene);
    quad();
    // 2. two separable blur iterations (horizontal into fbo 1, vertical back into fbo 0)
    GLShader::Use(m_progBlur);
    GLShader::Set1i(GLShader::Uniform(m_progBlur, "uSrc"), 0);
    const GLint dirLoc = GLShader::Uniform(m_progBlur, "uDir");
    for (int it = 0; it < 2; ++it) {
        GLShader::BindFramebuffer(m_postFbo[1]);
        GLShader::Set2f(dirLoc, 1.4f / bw, 0.0f);
        glBindTexture(GL_TEXTURE_2D, m_postBloom[0]);
        quad();
        GLShader::BindFramebuffer(m_postFbo[0]);
        GLShader::Set2f(dirLoc, 0.0f, 1.4f / bh);
        glBindTexture(GL_TEXTURE_2D, m_postBloom[1]);
        quad();
    }
    // 3. composite into the window
    GLShader::BindFramebuffer(0);
    glViewport(0, 0, w, h);
    GLShader::Use(m_progComposite);
    GLShader::Set1i(GLShader::Uniform(m_progComposite, "uScene"), 0);
    GLShader::Set1i(GLShader::Uniform(m_progComposite, "uBloom"), 1);
    GLShader::Set1f(GLShader::Uniform(m_progComposite, "uBloomStrength"), getenv("NFSU2_BLOOM") ? float(atof(getenv("NFSU2_BLOOM"))) : 0.75f);
    GLShader::Set1f(GLShader::Uniform(m_progComposite, "uVignette"), 0.34f);
    GLShader::Set1f(GLShader::Uniform(m_progComposite, "uExposure"), 1.06f);
    GLShader::ActiveTexture(1);
    glBindTexture(GL_TEXTURE_2D, m_postBloom[0]);
    GLShader::ActiveTexture(0);
    glBindTexture(GL_TEXTURE_2D, m_postScene);
    quad();
    GLShader::ActiveTexture(1);
    glBindTexture(GL_TEXTURE_2D, 0);
    GLShader::ActiveTexture(0);
    GLShader::Use(0);
    glDepthMask(GL_TRUE);
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
    if (!m_shadersTried)
        InitShaders();
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
    const float tanHalf = std::tan(std::clamp(cam.fov, 20.0f, 120.0f) * 0.5f * 3.14159265f / 180.0f); // horizontal FOV (70 by default)
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

    // The world program: textured scenery with grading and fog (the fixed-function path stays as the fallback).
    float curFogStart = kFogStart, curFogEnd = kDrawDistance;
    bool onInterior = false;
    auto worldProgram = [&](float fogStart, float fogEnd) {
        curFogStart = fogStart, curFogEnd = fogEnd, onInterior = false;
        if (!m_progWorld)
            return;
        GLShader::Use(m_progWorld);
        GLShader::Set1i(GLShader::Uniform(m_progWorld, "uTex"), 0);
        GLShader::Set3f(GLShader::Uniform(m_progWorld, "uFogColor"), fogColour[0], fogColour[1], fogColour[2]);
        GLShader::Set1f(GLShader::Uniform(m_progWorld, "uFogStart"), fogStart);
        GLShader::Set1f(GLShader::Uniform(m_progWorld, "uFogEnd"), fogEnd);
        GLShader::Set1f(GLShader::Uniform(m_progWorld, "uGrade"), getenv("NFSU2_NO_GRADE") ? 0.0f : 1.0f);
        GLShader::Set3f(GLShader::Uniform(m_progWorld, "uCam"), cam.x, cam.y, cam.z);
        GLShader::Set1f(GLShader::Uniform(m_progWorld, "uWet"), getenv("NFSU2_NO_WET") ? 0.0f : (getenv("NFSU2_WET") ? float(atof(getenv("NFSU2_WET"))) : 0.8f));
    };

    // The interior-mapping program for the windowed facades (ISSUES #266).
    auto interiorProgram = [&]() {
        GLShader::Use(m_progInterior);
        onInterior = true;
        GLShader::Set1i(GLShader::Uniform(m_progInterior, "uTex"), 0);
        GLShader::Set3f(GLShader::Uniform(m_progInterior, "uFogColor"), fogColour[0], fogColour[1], fogColour[2]);
        GLShader::Set1f(GLShader::Uniform(m_progInterior, "uFogStart"), curFogStart);
        GLShader::Set1f(GLShader::Uniform(m_progInterior, "uFogEnd"), curFogEnd);
        GLShader::Set1f(GLShader::Uniform(m_progInterior, "uGrade"), getenv("NFSU2_NO_GRADE") ? 0.0f : 1.0f);
        GLShader::Set3f(GLShader::Uniform(m_progInterior, "uCam"), cam.x, cam.y, cam.z);
        GLShader::Set1f(GLShader::Uniform(m_progInterior, "uTime"), float(SDL_GetTicks()) * 0.001f);
        GLShader::Set1f(GLShader::Uniform(m_progInterior, "uDebug"), getenv("NFSU2_INTERIOR_DEBUG") ? float(atof(getenv("NFSU2_INTERIOR_DEBUG"))) : 0.0f);
        GLShader::Set1f(GLShader::Uniform(m_progInterior, "uLit"), getenv("NFSU2_LIT_ROOMS") ? float(atof(getenv("NFSU2_LIT_ROOMS"))) : 0.42f);
    };

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
                cell.ranges.push_back({tex, int(scratch.size()), 0, mode == m_glTextureMode.end() ? 0 : mode->second,
                                       render[i].tex ? InteriorClassOf(render[i].tex) : 0});
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
    auto drawCell = [&](const GLCell &cell, int pass) {
        GLBridge::BindArrayBuffer(cell.buffer);
        glVertexPointer(3, GL_FLOAT, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, x)));
        glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, u)));
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, r)));
        // Opaque ranges are drawn twice over: plain facades with the world program, then the window facades with the
        // interior program (one program switch per cell instead of one per texture).
        const int sweeps = (pass == 0 && m_progInterior) ? 2 : 1;
        for (int sweep = 0; sweep < sweeps; ++sweep) {
            if (sweep == 1)
                interiorProgram();
            else if (onInterior)
                worldProgram(curFogStart, curFogEnd);
            for (const GLRange &range : cell.ranges) {
                if ((range.mode != 0) != (pass == 1))
                    continue;
                if (pass == 0 && m_progInterior && (range.interior != 0) != (sweep == 1))
                    continue;
                if (sweep == 1)
                    GLShader::Set1f(GLShader::Uniform(m_progInterior, "uGlassOnly"), range.interior == 2 ? 1.0f : 0.0f);
                if (pass == 1)
                    glBlendFunc(GL_SRC_ALPHA, range.mode == 2 ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
                glBindTexture(GL_TEXTURE_2D, range.texture);
                glDrawArrays(GL_TRIANGLES, range.first, range.count);
            }
        }
        if (onInterior)
            worldProgram(curFogStart, curFogEnd);
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
        worldProgram(1.0e9f, 2.0e9f); // no fog on the backdrop
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
    worldProgram(kFogStart, kDrawDistance);
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

    GLShader::Use(0);
    // Contact shadows: a soft dark quad under each car, aligned with it and lying on the lowest point of its wheels
    // (stand-in for the original's SHD_SHADOWTILE projected shadow, ISSUES #255).
    if (!getenv("NFSU2_NO_SHADOWS")) {
        if (!m_glShadow) {
            constexpr int n = 64;
            std::vector<uint8_t> px(n * n * 4, 0);
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x) {
                    const float fx = (x + 0.5f) / n * 2.0f - 1.0f, fy = (y + 0.5f) / n * 2.0f - 1.0f;
                    const float d = std::pow(std::pow(std::fabs(fx), 3.0f) + std::pow(std::fabs(fy), 3.0f), 1.0f / 3.0f); // rounded box
                    const float a = std::clamp(1.0f - d, 0.0f, 1.0f);
                    px[size_t(y * n + x) * 4 + 3] = uint8_t(255.0f * std::sqrt(a) * 0.8f);
                }
            glGenTextures(1, &m_glShadow);
            glBindTexture(GL_TEXTURE_2D, m_glShadow);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        GLBridge::BindArrayBuffer(0);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-4.0f, -8.0f);
        glBindTexture(GL_TEXTURE_2D, m_glShadow);
        for (Car &car : m_cars) {
            if (car.render.empty() || car.groundZ > 1e8f)
                continue;
            const float *m = car.xf;
            if (std::hypot(m[3] - cam.x, m[7] - cam.y) > 70.0f)
                continue; // far cars: no shadow (keeps the per-vertex ground queries cheap)
            float fx = m[0], fy = m[1], lx = m[4], ly = m[5]; // car x (forward) and y (left) axes in the world, flattened
            const float fl = std::max(1e-4f, std::hypot(fx, fy)), ll = std::max(1e-4f, std::hypot(lx, ly));
            fx /= fl, fy /= fl, lx /= ll, ly /= ll;
            const float cx = m[3], cy = m[7];
            // The shadow lies on the ground, not on the car's lowest vertex: that point moves with pitch, roll, the spin of
            // a polygonal tyre and the steering, which made the shadow flicker in and out of the road. A low-passed
            // height is the fallback where the world has no ground query (the garage room).
            car.shadowZ = car.shadowZ > 1e8f ? car.groundZ : car.shadowZ + (car.groundZ - car.shadowZ) * 0.12f;
            const float hl = car.halfLen * 1.15f, hw = car.halfWid * 1.3f;
            constexpr int NX = 6, NY = 4;
            float gz[NX + 1][NY + 1];
            float centreGround = car.shadowZ;
            for (int i = 0; i <= NX; ++i)
                for (int k = 0; k <= NY; ++k) {
                    const float a = -hl + 2.0f * hl * float(i) / NX, b = -hw + 2.0f * hw * float(k) / NY;
                    const float wx = cx + fx * a + lx * b, wy = cy + fy * a + ly * b;
                    float z;
                    if (!GroundHeight(wx, wy, m[11] + 0.6f, z) || std::fabs(z - car.shadowZ) > 1.5f)
                        z = car.shadowZ;
                    // Light smoothing against single-frame steps of the collision mesh; a real jump (> 0.4 m) snaps.
                    if (car.shadowGridSet && std::fabs(z - car.shadowGrid[i][k]) < 0.4f)
                        z = car.shadowGrid[i][k] + (z - car.shadowGrid[i][k]) * 0.35f;
                    car.shadowGrid[i][k] = z;
                    gz[i][k] = z;
                    if (i == NX / 2 && k == NY / 2)
                        centreGround = z;
                }
            car.shadowGridSet = true;
            // Fade with the car's height above the road (jumps), full strength on the ground.
            const float lift = std::max(0.0f, car.groundZ - centreGround);
            const uint8_t alpha = uint8_t(255.0f * std::clamp(1.0f - lift / 1.6f, 0.0f, 1.0f));
            if (getenv("NFSU2_SHADOW_DEBUG") && &car == &m_cars[0])
                printf("[Shadow] old %.4f new %.4f lift %.3f\n", car.groundZ + 0.08f, centreGround + 0.06f, lift);
            if (alpha == 0)
                continue;
            std::vector<Vertex> q;
            auto vert = [&](int i, int k) {
                const float a = -hl + 2.0f * hl * float(i) / NX, b = -hw + 2.0f * hw * float(k) / NY;
                q.push_back({cx + fx * a + lx * b, cy + fy * a + ly * b, gz[i][k] + 0.06f, float(i) / NX, float(k) / NY, 0, 0, 0, alpha});
            };
            for (int i = 0; i < NX; ++i)
                for (int k = 0; k < NY; ++k) {
                    vert(i, k), vert(i + 1, k), vert(i + 1, k + 1);
                    vert(i, k), vert(i + 1, k + 1), vert(i, k + 1);
                }
            glVertexPointer(3, GL_FLOAT, sizeof(Vertex), &q[0].x);
            glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex), &q[0].u);
            glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), &q[0].r);
            glDrawArrays(GL_TRIANGLES, 0, GLsizei(q.size()));
        }
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    // Cars: rebuilt every frame from client memory, one draw per texture run. With the car program the paint is lit per
    // pixel (normals and the gloss class travel as vertex attributes); without it the CPU lights the vertices.
    GLBridge::BindArrayBuffer(0);
    if (m_progCar) {
        struct CarVertex {
            float x, y, z, u, v;
            uint8_t r, g, b, a;
            float nx, ny, nz, gloss, ao;
        };
        static std::vector<CarVertex> cv;
        GLShader::Use(m_progCar);
        GLShader::Set1i(GLShader::Uniform(m_progCar, "uTex"), 0);
        GLShader::Set3f(GLShader::Uniform(m_progCar, "uCam"), cam.x, cam.y, cam.z);
        GLShader::Set3f(GLShader::Uniform(m_progCar, "uFogColor"), fogColour[0], fogColour[1], fogColour[2]);
        GLShader::Set1f(GLShader::Uniform(m_progCar, "uFogStart"), kFogStart);
        GLShader::Set1f(GLShader::Uniform(m_progCar, "uFogEnd"), kDrawDistance);
        GLShader::EnableAttrib(1);
        GLShader::EnableAttrib(2);
        GLShader::EnableAttrib(3);
        const GLint blurLoc = GLShader::Uniform(m_progCar, "uBlur");
        for (const Car &car : m_cars) {
            if (car.render.empty())
                continue;
            cv.clear();
            const bool lit = car.gloss.size() == car.render.size() && car.normals.size() == car.render.size() * 9 && !getenv("NFSU2_CAR_FLAT");
            // Spoke blur: the faster the wheels turn the more the rim fades into a disc (full at ~12 turns per second).
            GLShader::Set1f(blurLoc, std::clamp((std::fabs(car.spinRate) - 25.0f) / 55.0f, 0.0f, 1.0f));
            for (size_t ti = 0; ti < car.render.size(); ++ti) {
                const RTri &t = car.render[ti];
                const uint8_t g = lit ? car.gloss[ti] : 0;
                for (int k = 0; k < 3; ++k) {
                    const RVertex &v = t.v[k];
                    CarVertex o{v.x, v.y, v.z, v.u, v.v, v.r, v.g, v.b, v.a, 0, 0, 1, float(g), 1.0f};
                    if (g) {
                        const float *a = &car.albedo[ti * 3], *n3 = &car.normals[ti * 9 + size_t(k) * 3];
                        o.r = uint8_t(std::clamp(a[0], 0.0f, 1.0f) * 255.0f), o.g = uint8_t(std::clamp(a[1], 0.0f, 1.0f) * 255.0f),
                        o.b = uint8_t(std::clamp(a[2], 0.0f, 1.0f) * 255.0f);
                        o.nx = n3[0], o.ny = n3[1], o.nz = n3[2];
                        if (car.ao.size() == car.render.size() * 3)
                            o.ao = car.ao[ti * 3 + size_t(k)];
                    }
                    cv.push_back(o);
                }
            }
            glVertexPointer(3, GL_FLOAT, sizeof(CarVertex), &cv[0].x);
            glTexCoordPointer(2, GL_FLOAT, sizeof(CarVertex), &cv[0].u);
            glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(CarVertex), &cv[0].r);
            GLShader::AttribPointer(1, 3, GL_FLOAT, false, sizeof(CarVertex), &cv[0].nx);
            GLShader::AttribPointer(2, 1, GL_FLOAT, false, sizeof(CarVertex), &cv[0].gloss);
            GLShader::AttribPointer(3, 1, GL_FLOAT, false, sizeof(CarVertex), &cv[0].ao);
            for (size_t first = 0; first < car.render.size();) {
                size_t last = first;
                while (last < car.render.size() && car.render[last].tex == car.render[first].tex)
                    ++last;
                glBindTexture(GL_TEXTURE_2D, GLTexture(car.render[first].tex));
                glDrawArrays(GL_TRIANGLES, GLint(first * 3), GLsizei((last - first) * 3));
                first = last;
            }
        }
        GLShader::DisableAttrib(1);
        GLShader::DisableAttrib(2);
        GLShader::DisableAttrib(3);
        GLShader::Use(0);
    } else {
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
                    if (lit && (car.gloss[ti] == 1 || car.gloss[ti] == 2)) {
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
    }
    if (!inset)
        PostProcess(width, height);
    if (inset)
        glDisable(GL_SCISSOR_TEST);
    return true;
}
