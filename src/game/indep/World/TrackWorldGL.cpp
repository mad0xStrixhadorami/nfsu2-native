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
#include "game/indep/World/MaterialLibrary.h"
#include "game/indep/Perf.h"
#include "game/indep/World/SignGen.h"
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
        if (m_arena)
            GLBridge::DeleteBuffers(1, &m_arena);
        GLBridge::DeleteVertexArray(m_arenaVao);
        for (auto &[hash, tex] : m_glTextures)
            glDeleteTextures(1, &tex);
    }
    m_arena = m_arenaVao = 0;
    m_arenaCap = m_arenaUsed = 0;
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
        // American signage -> Hebrew / Israeli replacements (ISSUES #277); the original's blend mode is kept.
        if (!getenv("NFSU2_NO_SIGNS")) {
            int sw = decoded.width, sh = decoded.height;
            std::vector<uint32_t> repl;
            if (SignGen::Replacement(info->name, sw, sh, repl) && sw > 0 && sh > 0) {
                decoded.rgba = std::move(repl);
                decoded.width = sw, decoded.height = sh;
            }
        }
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
    {
        const std::string full = Shaders::WorldFSSource(true), lite = Shaders::WorldFSSource(false);
        m_progWorld = GLShader::Build("world", Shaders::kWorldVS, getenv("NFSU2_SIMPLE_WORLD_FS") ? Shaders::kWorldFSMin : full.c_str());
        m_progWorldLite = getenv("NFSU2_NO_LOD_SHADER") ? m_progWorld : GLShader::Build("world-lite", Shaders::kWorldVS, getenv("NFSU2_SIMPLE_WORLD_FS") ? Shaders::kWorldFSMin : lite.c_str());
    }
    m_progInterior = getenv("NFSU2_NO_INTERIORS") ? 0 : GLShader::Build("interior", Shaders::kInteriorVS, Shaders::kInteriorFS);
    m_progCar = GLShader::Build("car", Shaders::kCarVS, Shaders::kCarFS, {{1, "aNormal"}, {2, "aGloss"}, {3, "aAO"}});
    m_progBright = GLShader::Build("bright", Shaders::kPostVS, Shaders::kBrightFS);
    m_progBlur = GLShader::Build("blur", Shaders::kPostVS, Shaders::kBlurFS);
    m_progComposite = GLShader::Build("composite", Shaders::kPostVS, Shaders::kCompositeFS);
    m_progSky = getenv("NFSU2_CLASSIC_SKY") ? 0 : GLShader::Build("sky", Shaders::kSkyVS, Shaders::kSkyFS);
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
    GLShader::Set1f(GLShader::Uniform(m_progComposite, "uMotion"), getenv("NFSU2_NO_SPEEDFX") ? 0.0f : m_fxMotion);
    GLShader::Set1f(GLShader::Uniform(m_progComposite, "uNitro"), m_fxNitro);
    GLShader::Set2f(GLShader::Uniform(m_progComposite, "uFxCenter"), m_fxCenter[0], m_fxCenter[1]);
    GLShader::Set2f(GLShader::Uniform(m_progComposite, "uCarPos"), m_fxCar[0], m_fxCar[1]);
    GLShader::Set2f(GLShader::Uniform(m_progComposite, "uCarRad"), m_fxCar[2], m_fxCar[3]);
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
    // Israeli daylight (ISSUES #277): bright azure sky, pale warm haze; NFSU2_NIGHT=1 keeps the original dusk.
    const bool day = !getenv("NFSU2_NIGHT");
    const float fogColour[4] = {day ? 0.82f : 0.55f, day ? 0.87f : 0.62f, day ? 0.93f : 0.72f, 1.0f};

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

    // Sky (ISSUES #302): drawn after the camera basis is known (below); the classic gradient is the fallback.
    // Camera: right-handed view from the same basis as the software path.
    const float cy = std::cos(cam.yaw), sy = std::sin(cam.yaw);
    const float cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
    const float f[3] = {cy * cp, sy * cp, sp};
    const float r[3] = {sy, -cy, 0.0f};
    const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    const float c[3] = {cam.x, cam.y, cam.z};
    // Horizontal FOV (70 by default) is authored for 4:3; wider windows keep the vertical FOV (Hor+) so the picture is
    // widened, not stretched or cropped.
    const float aspect = float(height) / float(std::max(width, 1));
    const float tanHalf = std::tan(std::clamp(cam.fov, 20.0f, 120.0f) * 0.5f * 3.14159265f / 180.0f) *
                          std::max(1.0f, (1.0f / aspect) * 0.75f);
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

    // Sky: the procedural shader sky, or (NFSU2_CLASSIC_SKY=1 / no shaders) the old vertical gradient, drawn with identity matrices.
    {
        glDisable(GL_DEPTH_TEST);
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        if (m_progSky) {
            GLShader::Use(m_progSky);
            GLShader::Set3f(GLShader::Uniform(m_progSky, "uF"), f[0], f[1], f[2]);
            GLShader::Set3f(GLShader::Uniform(m_progSky, "uR"), r[0], r[1], r[2]);
            GLShader::Set3f(GLShader::Uniform(m_progSky, "uU"), u[0], u[1], u[2]);
            GLShader::Set2f(GLShader::Uniform(m_progSky, "uTan"), tanHalf * side, tanHalf * aspect);
            const float sl = std::sqrt(0.62f * 0.62f + 0.45f * 0.45f + 0.64f * 0.64f);
            GLShader::Set3f(GLShader::Uniform(m_progSky, "uSun"), 0.62f / sl, 0.45f / sl, 0.64f / sl);
            GLShader::Set3f(GLShader::Uniform(m_progSky, "uHorizon"), fogColour[0], fogColour[1], fogColour[2]);
            GLShader::Set1f(GLShader::Uniform(m_progSky, "uTime"), float(SDL_GetTicks()) * 0.001f);
            GLShader::Set1f(GLShader::Uniform(m_progSky, "uDay"), getenv("NFSU2_SKY_DEBUG") ? 2.0f : (day ? 1.0f : 0.0f));
            glBegin(GL_QUADS);
            glVertex2f(-1, 1);
            glVertex2f(1, 1);
            glVertex2f(1, -1);
            glVertex2f(-1, -1);
            glEnd();
            GLShader::Use(0);
        } else {
            glShadeModel(GL_SMOOTH);
            glBegin(GL_QUADS);
            if (day)
                glColor3ub(52, 128, 214);
            else
                glColor3ub(70, 100, 150);
            glVertex2f(-1, 1);
            glVertex2f(1, 1);
            if (day)
                glColor3ub(206, 224, 242);
            else
                glColor3ub(140, 158, 184);
            glVertex2f(1, -1);
            glVertex2f(-1, -1);
            glEnd();
            glShadeModel(GL_FLAT);
        }
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
    }

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
    if (scratch.capacity() < 400000)
        scratch.reserve(400000); // the largest cell fits: the upload never reallocates

    // The world programs: FULL (theme, photographic materials, wet roads) for cells near the camera, LITE for the far ones.  Uniform
    // locations are looked up once per frame and the per-frame constants are set once; switching only re-uses a program and
    // updates the fog range when it changed (it used to re-set ~35 uniforms and re-bind 13 textures on every switch).
    struct WorldUniforms {
        int dbgPave = -1, glow = -1, tex = -1, fogColor = -1, fogStart = -1, fogEnd = -1, grade = -1, cam = -1, theme = -1, day = -1, matOn = -1, wet = -1;
        int mc[MaterialLibrary::kColours], mn[MaterialLibrary::kNormals];
        float lastFogStart = -1.0f, lastFogEnd = -1.0f;
    };
    WorldUniforms wuFull, wuLite;
    auto queryWorldUniforms = [&](unsigned prog, WorldUniforms &u) {
        u.glow = GLShader::Uniform(prog, "uGlow");
        u.tex = GLShader::Uniform(prog, "uTex"), u.fogColor = GLShader::Uniform(prog, "uFogColor");
        u.fogStart = GLShader::Uniform(prog, "uFogStart"), u.fogEnd = GLShader::Uniform(prog, "uFogEnd");
        u.grade = GLShader::Uniform(prog, "uGrade"), u.cam = GLShader::Uniform(prog, "uCam"), u.theme = GLShader::Uniform(prog, "uTheme");
        u.day = GLShader::Uniform(prog, "uDay"), u.dbgPave = GLShader::Uniform(prog, "uDebugPave"), u.matOn = GLShader::Uniform(prog, "uMatOn"), u.wet = GLShader::Uniform(prog, "uWet");
        for (int i = 0; i < MaterialLibrary::kColours; ++i) {
            char name[8];
            snprintf(name, sizeof(name), "uMC%d", i);
            u.mc[i] = GLShader::Uniform(prog, name);
        }
        for (int i = 0; i < MaterialLibrary::kNormals; ++i) {
            char name[8];
            snprintf(name, sizeof(name), "uMN%d", i);
            u.mn[i] = GLShader::Uniform(prog, name);
        }
    };
    const bool mats = m_progWorld && MaterialLibrary::Load();
    auto setFrameUniforms = [&](unsigned prog, WorldUniforms &u) {
        GLShader::Use(prog);
        ++Perf::g.programSwitches;
        GLShader::Set1i(u.tex, 0);
        GLShader::Set1f(u.glow, 1.0f);
        GLShader::Set3f(u.fogColor, fogColour[0], fogColour[1], fogColour[2]);
        GLShader::Set1f(u.grade, getenv("NFSU2_NO_GRADE") ? 0.0f : 1.0f);
        GLShader::Set3f(u.cam, cam.x, cam.y, cam.z);
        GLShader::Set1f(u.theme, getenv("NFSU2_NO_THEME") ? 0.0f : 1.0f);
        GLShader::Set1f(u.day, day ? 1.0f : 0.0f);
        GLShader::Set1f(u.matOn, mats ? 1.0f : 0.0f);
        for (int i = 0; i < MaterialLibrary::kColours; ++i)
            GLShader::Set1i(u.mc[i], 2 + i);
        for (int i = 0; i < MaterialLibrary::kNormals; ++i)
            GLShader::Set1i(u.mn[i], 10 + i);
        GLShader::Set1f(u.wet, getenv("NFSU2_NO_WET") ? 0.0f : (getenv("NFSU2_WET") ? float(atof(getenv("NFSU2_WET"))) : (day ? 0.0f : 0.8f)));
        GLShader::Set1f(u.dbgPave, getenv("NFSU2_DEBUG_PAVE") ? 1.0f : 0.0f);
    };
    if (m_progWorld) {
        queryWorldUniforms(m_progWorld, wuFull);
        queryWorldUniforms(m_progWorldLite, wuLite);
        setFrameUniforms(m_progWorld, wuFull);
        if (m_progWorldLite != m_progWorld)
            setFrameUniforms(m_progWorldLite, wuLite);
        if (mats) { // the material textures stay bound on units 2..14 for the whole frame (ISSUES #274)
            for (int i = 0; i < MaterialLibrary::kColours; ++i) {
                GLShader::ActiveTexture(2 + i);
                glBindTexture(GL_TEXTURE_2D, MaterialLibrary::Colour(i));
            }
            for (int i = 0; i < MaterialLibrary::kNormals; ++i) {
                GLShader::ActiveTexture(10 + i);
                glBindTexture(GL_TEXTURE_2D, MaterialLibrary::Normal(i));
            }
            GLShader::ActiveTexture(0);
        }
    }
    float curFogStart = kFogStart, curFogEnd = kDrawDistance;
    unsigned lastProg = 0;
    WorldUniforms *curU = &wuFull;
    auto worldProgram = [&](float fogStart, float fogEnd, bool lite) {
        curFogStart = fogStart, curFogEnd = fogEnd;
        if (!m_progWorld)
            return;
        const unsigned prog = lite ? m_progWorldLite : m_progWorld;
        WorldUniforms &u = (lite && m_progWorldLite != m_progWorld) ? wuLite : wuFull;
        curU = &u;
        if (prog != lastProg) {
            GLShader::Use(prog);
            ++Perf::g.programSwitches;
            lastProg = prog;
        }
        if (u.lastFogStart != fogStart || u.lastFogEnd != fogEnd) {
            GLShader::Set1f(u.fogStart, fogStart);
            GLShader::Set1f(u.fogEnd, fogEnd);
            u.lastFogStart = fogStart, u.lastFogEnd = fogEnd;
        }
    };

    // The interior-mapping program for the windowed facades (ISSUES #266).
    auto interiorProgram = [&]() {
        GLShader::Use(m_progInterior);
        lastProg = m_progInterior;
        ++Perf::g.programSwitches;
        GLShader::Set1i(GLShader::Uniform(m_progInterior, "uTex"), 0);
        GLShader::Set3f(GLShader::Uniform(m_progInterior, "uFogColor"), fogColour[0], fogColour[1], fogColour[2]);
        GLShader::Set1f(GLShader::Uniform(m_progInterior, "uFogStart"), curFogStart);
        GLShader::Set1f(GLShader::Uniform(m_progInterior, "uFogEnd"), curFogEnd);
        GLShader::Set1f(GLShader::Uniform(m_progInterior, "uGrade"), getenv("NFSU2_NO_GRADE") ? 0.0f : 1.0f);
        GLShader::Set3f(GLShader::Uniform(m_progInterior, "uCam"), cam.x, cam.y, cam.z);
        GLShader::Set1f(GLShader::Uniform(m_progInterior, "uTime"), float(SDL_GetTicks()) * 0.001f);
        GLShader::Set1f(GLShader::Uniform(m_progInterior, "uDebug"), getenv("NFSU2_INTERIOR_DEBUG") ? float(atof(getenv("NFSU2_INTERIOR_DEBUG"))) : 0.0f);
        GLShader::Set1f(GLShader::Uniform(m_progInterior, "uLit"), getenv("NFSU2_LIT_ROOMS") ? float(atof(getenv("NFSU2_LIT_ROOMS"))) : (day ? 0.12f : 0.42f));
        GLShader::Set1f(GLShader::Uniform(m_progInterior, "uDay"), day ? 1.0f : 0.0f);
    };

    // Upload render triangles once: grouped by texture, one draw range per texture.
    // The vertex arena: one GL buffer sized for every cell's triangles (known now: the render lists are still on the CPU), plus a VAO
    // holding the fixed-function array state.  Cells are written into it lazily the first time they are drawn.
    if (!m_arena) {
        size_t total = m_panorama.size() * 3;
        for (const auto &kv : m_cells)
            total += kv.second.render.size() * 3;
        m_arenaCap = total + total / 20 + 1024;
        GLBridge::GenBuffers(1, &m_arena);
        GLBridge::BindArrayBuffer(m_arena);
        GLBridge::ArrayBufferData(ptrdiff_t(m_arenaCap * sizeof(Vertex)), nullptr);
        m_arenaVao = GLBridge::GenVertexArray();
        GLBridge::BindVertexArray(m_arenaVao);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glVertexPointer(3, GL_FLOAT, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, x)));
        glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, u)));
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, r)));
        GLBridge::BindVertexArray(0);
        printf("[GL] vertex arena %.0f MB for %zu vertices\n", double(m_arenaCap * sizeof(Vertex)) / 1048576.0, m_arenaCap);
    }
    auto upload = [&](GLCell &cell, std::vector<RTri> &render) {
        if (render.empty())
            return;
        ++Perf::g.bufferBinds; // counts cell uploads (should be 0 in steady state)
        if (!getenv("NFSU2_NIGHT")) // daylight: the street lamps' light cones are a night effect, they show as glowing shards by day
            render.erase(std::remove_if(render.begin(), render.end(), [](const RTri &t) { return IsBeamTexture(t.tex); }), render.end());
        if (render.empty())
            return;
        std::stable_sort(render.begin(), render.end(), [](const RTri &a, const RTri &b) { return a.tex < b.tex; });
        scratch.clear();
        cell.ranges.clear();
        const int base = int(m_arenaUsed);
        for (size_t i = 0; i < render.size(); ++i) {
            if (i == 0 || render[i].tex != render[i - 1].tex) {
                unsigned tex = GLTexture(render[i].tex);
                auto mode = m_glTextureMode.find(tex);
                cell.ranges.push_back({tex, base + int(scratch.size()), 0, mode == m_glTextureMode.end() ? 0 : mode->second,
                                       render[i].tex ? InteriorClassOf(render[i].tex) : 0});
            }
            for (const RVertex &v : render[i].v)
                scratch.push_back({v.x, v.y, v.z, v.u, v.v, v.r, v.g, v.b, v.a});
            cell.ranges.back().count += 3;
        }
        if (m_arenaUsed + scratch.size() > m_arenaCap) { // cannot happen (the arena is sized for everything); skip safely
            cell.ranges.clear();
            std::vector<RTri>().swap(render);
            return;
        }
        GLBridge::BindArrayBuffer(m_arena);
        GLBridge::ArrayBufferSubData(ptrdiff_t(m_arenaUsed * sizeof(Vertex)), ptrdiff_t(scratch.size() * sizeof(Vertex)), scratch.data());
        m_arenaUsed += scratch.size();
        cell.buffer = m_arena;
        cell.vertices = int(scratch.size());
        std::vector<RTri>().swap(render); // the GPU copy is the render data from now on
    };
    // Draw batches: every visible cell's ranges are grouped by (group, texture, mode) and each batch is one glMultiDrawArrays.  The
    // vectors are static: their capacity survives from frame to frame, so the loop does not allocate.
    struct Batch {
        unsigned tex;
        int mode, interior;
        std::vector<GLint> first;
        std::vector<GLsizei> count;
    };
    enum { kOpaqueNear, kOpaqueFar, kInterior, kBlendNear, kBlendFar, kGroups };
    static std::vector<Batch> batches[kGroups];
    static std::unordered_map<uint64_t, int> batchIndex[kGroups];
    auto resetBatches = [&] {
        for (int g = 0; g < kGroups; ++g) {
            for (Batch &b : batches[g])
                b.first.clear(), b.count.clear();
        }
    };
    auto addRanges = [&](const GLCell &cell, bool far) {
        for (const GLRange &r : cell.ranges) {
            const int g = r.mode != 0 ? (far ? kBlendFar : kBlendNear) : (m_progInterior && r.interior != 0) ? kInterior : (far ? kOpaqueFar : kOpaqueNear);
            const uint64_t key = (uint64_t(r.texture) << 8) | uint64_t(r.mode << 2) | uint64_t(r.interior);
            auto it = batchIndex[g].find(key);
            if (it == batchIndex[g].end()) {
                batches[g].push_back({r.texture, r.mode, r.interior, {}, {}});
                it = batchIndex[g].emplace(key, int(batches[g].size()) - 1).first;
            }
            Batch &b = batches[g][size_t(it->second)];
            b.first.push_back(r.first);
            b.count.push_back(r.count);
        }
    };
    auto drawBatches = [&](int g) {
        unsigned boundTex = ~0u;
        for (const Batch &b : batches[g]) {
            if (b.first.empty())
                continue;
            if (g == kInterior)
                GLShader::Set1f(GLShader::Uniform(m_progInterior, "uGlassOnly"), b.interior == 2 ? 1.0f : 0.0f);
            if (g == kBlendNear || g == kBlendFar) {
                glBlendFunc(GL_SRC_ALPHA, b.mode == 2 ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
                GLShader::Set1f(curU->glow, b.mode == 2 && day ? 0.2f : 1.0f); // daylight: additive light pools / glows are faint
            }
            if (b.tex != boundTex) {
                glBindTexture(GL_TEXTURE_2D, b.tex);
                boundTex = b.tex;
                ++Perf::g.textureBinds;
            }
            GLBridge::MultiDrawArrays(GL_TRIANGLES, b.first.data(), b.count.data(), GLsizei(b.first.size()));
            ++Perf::g.drawCalls, ++Perf::g.multiDraws;
            for (GLsizei c : b.count)
                Perf::g.triangles += uint32_t(c / 3);
        }
    };

    // First frame of a loaded world: upload every cell now, during loading, instead of the first time it comes into view.
    if (m_warmAll && !m_scene) {
        const unsigned t0 = SDL_GetTicks();
        for (auto &kv : m_cells)
            upload(m_glCells[kv.first], kv.second.render);
        m_warmAll = false;
        printf("[GL] all %zu cells uploaded in %u ms (%.0f MB of the %.0f MB arena)\n", m_cells.size(), SDL_GetTicks() - t0,
               double(m_arenaUsed * sizeof(Vertex)) / 1048576.0, double(m_arenaCap * sizeof(Vertex)) / 1048576.0);
    }
    // Panorama backdrop (PAN_* instances): far projection, no fog, no depth -- the world covers it.
    upload(m_glPanorama, m_panorama);
    GLBridge::BindVertexArray(m_arenaVao);
    GLBridge::BindArrayBuffer(m_arena);
    if (m_glPanorama.buffer) {
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glFrustum(-kNear * tanHalf, kNear * tanHalf, -kNear * tanHalf * aspect, kNear * tanHalf * aspect, kNear * 4,
                  20000.0f);
        glMatrixMode(GL_MODELVIEW);
        glDisable(GL_FOG);
        glDisable(GL_DEPTH_TEST);
        worldProgram(1.0e9f, 2.0e9f, true); // no fog on the backdrop; the cheap shader
        resetBatches();
        addRanges(m_glPanorama, true);
        drawBatches(kOpaqueFar);
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
    // Reused every frame (capacity kept): no heap allocation in the render loop.
    static std::vector<const GLCell *> nearCells, farCells;
    nearCells.clear();
    farCells.clear();
    const float kFullShaderRange = getenv("NFSU2_FULL_RANGE") ? float(atof(getenv("NFSU2_FULL_RANGE"))) : 150.0f;
    for (int gx = ccx - reach; gx <= ccx + reach; ++gx)
        for (int gy = ccy - reach; gy <= ccy + reach; ++gy) {
            auto it = m_cells.find(CellKey(gx, gy));
            if (it == m_cells.end() || it->second.tris.empty())
                continue;
            float mx = (gx + 0.5f) * kCellSize - cam.x, my = (gy + 0.5f) * kCellSize - cam.y;
            float along = mx * cy + my * sy, across = std::fabs(mx * sy - my * cy);
            const float dist = std::hypot(mx, my);
            if (!m_scene && (dist > kDrawDistance + kCellSize || along < -kCellSize ||
                             across > along * coneSlope + kCellSize * 0.75f))
                continue;
            GLCell &cell = m_glCells[it->first];
            upload(cell, it->second.render);
            if (cell.buffer)
                (m_scene || dist < kFullShaderRange ? nearCells : farCells).push_back(&cell);
        }
    resetBatches();
    for (const GLCell *cell : nearCells)
        addRanges(*cell, false);
    for (const GLCell *cell : farCells)
        addRanges(*cell, true);
    // Opaque and punch-through first (the full shader near, the lite one far, then the window facades with the interior program),
    // then blended / additive with depth writes off.
    worldProgram(kFogStart, kDrawDistance, false);
    drawBatches(kOpaqueNear);
    worldProgram(kFogStart, kDrawDistance, true);
    drawBatches(kOpaqueFar);
    if (m_progInterior) {
        interiorProgram();
        drawBatches(kInterior);
        lastProg = 0;
    }
    glEnable(GL_BLEND);
    glDepthMask(GL_FALSE);
    worldProgram(kFogStart, kDrawDistance, false);
    drawBatches(kBlendNear);
    worldProgram(kFogStart, kDrawDistance, true);
    drawBatches(kBlendFar);
    GLBridge::BindVertexArray(0);
    Perf::g.cellsDrawn = uint32_t(nearCells.size() + farCells.size());
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

    // Hot-spot beacons: a translucent light column per marker, brighter at the foot, within 700 m.
    if (!m_beacons.empty()) {
        GLShader::Use(0);
        glDisable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        constexpr int kSides = 20;
        constexpr float kHeight = 38.0f;
        for (const Beacon &b : m_beacons) {
            if (std::hypot(b.x - cam.x, b.y - cam.y) > 700.0f)
                continue;
            glBegin(GL_QUAD_STRIP);
            for (int i = 0; i <= kSides; ++i) {
                const float a = float(i % kSides) * 6.2831853f / kSides;
                const float px = b.x + std::cos(a) * b.radius, py = b.y + std::sin(a) * b.radius;
                glColor4ub(b.r, b.g, b.b, 110);
                glVertex3f(px, py, b.z);
                glColor4ub(b.r, b.g, b.b, 0);
                glVertex3f(px, py, b.z + kHeight);
            }
            glEnd();
        }
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
        const GLint lampLoc = GLShader::Uniform(m_progCar, "uLamp");
        const GLint glassLoc = GLShader::Uniform(m_progCar, "uGlass");
        GLShader::Set1f(glassLoc, 0.0f);
        GLShader::Set1f(GLShader::Uniform(m_progCar, "uDay"), day ? 1.0f : 0.0f);
        for (const Car &car : m_cars) {
            if (car.render.empty())
                continue;
            cv.clear();
            const bool lit = car.gloss.size() == car.render.size() && car.normals.size() == car.render.size() * 9 && !getenv("NFSU2_CAR_FLAT");
            // Spoke blur: the faster the wheels turn the more the rim fades into a disc (full at ~12 turns per second).
            GLShader::Set1f(blurLoc, std::clamp((std::fabs(car.spinRate) - 25.0f) / 55.0f, 0.0f, 1.0f));
            if (lampLoc >= 0)
                GLShader::Set3f(lampLoc, car.lampHead, car.lampBrake, car.lampReverse);
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
            // The lamp covers (gloss 8 / 9): translucent, after everything opaque of this car, no depth writes.
            static std::vector<CarVertex> glassV;
            glassV.clear();
            if (lit)
                for (size_t ti = 0; ti < car.render.size(); ++ti)
                    if (car.gloss[ti] == 8 || car.gloss[ti] == 9)
                        glassV.insert(glassV.end(), cv.begin() + ptrdiff_t(ti * 3), cv.begin() + ptrdiff_t(ti * 3 + 3));
            if (!glassV.empty()) {
                glVertexPointer(3, GL_FLOAT, sizeof(CarVertex), &glassV[0].x);
                glTexCoordPointer(2, GL_FLOAT, sizeof(CarVertex), &glassV[0].u);
                glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(CarVertex), &glassV[0].r);
                GLShader::AttribPointer(1, 3, GL_FLOAT, false, sizeof(CarVertex), &glassV[0].nx);
                GLShader::AttribPointer(2, 1, GL_FLOAT, false, sizeof(CarVertex), &glassV[0].gloss);
                GLShader::AttribPointer(3, 1, GL_FLOAT, false, sizeof(CarVertex), &glassV[0].ao);
                GLShader::Set1f(glassLoc, 1.0f);
                glDisable(GL_ALPHA_TEST);
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);
                glBindTexture(GL_TEXTURE_2D, GLTexture(0));
                glDrawArrays(GL_TRIANGLES, 0, GLsizei(glassV.size()));
                glDepthMask(GL_TRUE);
                glDisable(GL_BLEND);
                glEnable(GL_ALPHA_TEST);
                GLShader::Set1f(glassLoc, 0.0f);
            }
        }
        GLShader::DisableAttrib(1);
        GLShader::DisableAttrib(2);
        GLShader::DisableAttrib(3);
        GLShader::Use(0);
        DrawCarLights(cam, day);
        DrawTyreEffects(cam);
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

// Glows and light on the road for the car lamps (ISSUES #284): a soft additive halo around each lit lens (facing the
// viewer, so a tail lamp does not glow through the car), and at night warm headlight pools and a red wash behind a braking
// car. The lens triangles themselves are emissive in the car shader.
void TrackWorld::DrawCarLights(const Camera &cam, bool day) const
{
    if (getenv("NFSU2_NO_LAMP_GLOW"))
        return;
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_CURRENT_BIT | GL_POLYGON_BIT);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_CULL_FACE);
    glDisable(GL_FOG);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    const float dayK = day ? 0.55f : 1.0f;
    auto halo = [&](const float p[3], float r, float g, float b, float radius, float alpha) {
        if (alpha <= 0.01f)
            return;
        float dx = p[0] - cam.x, dy = p[1] - cam.y, dz = p[2] - cam.z;
        const float dl = std::max(1e-4f, std::sqrt(dx * dx + dy * dy + dz * dz));
        dx /= dl, dy /= dl, dz /= dl;
        float rx = dy, ry = -dx; // right = d x up(0,0,1)
        const float rl = std::max(1e-4f, std::sqrt(rx * rx + ry * ry));
        rx /= rl, ry /= rl;
        const float ux = ry * dz, uy = -rx * dz, uz = rx * dy - ry * dx; // up = right x d
        const float k = std::min(1.0f, dl / 4.0f); // far lamps keep a visible minimum size
        const float R = radius * (0.6f + 0.4f * k) + dl * 0.004f;
        const float cx = p[0] - dx * 0.18f, cy = p[1] - dy * 0.18f, cz = p[2] - dz * 0.18f; // toward the viewer, off the lens
        // Soft falloff: four rings (radius, alpha) from the centre out.
        constexpr float kRing[4][2] = {{0.0f, 1.0f}, {0.28f, 0.42f}, {0.6f, 0.10f}, {1.0f, 0.0f}};
        for (int ring = 0; ring < 3; ++ring) {
            glBegin(GL_TRIANGLE_STRIP);
            for (int i = 0; i <= 16; ++i) {
                const float a = float(i) * 6.2831853f / 16.0f, ca = std::cos(a), sa = std::sin(a);
                for (int e = 0; e < 2; ++e) {
                    const float rr = kRing[ring + e][0] * R;
                    glColor4f(r, g, b, kRing[ring + e][1] * alpha);
                    glVertex3f(cx + (rx * ca + ux * sa) * rr, cy + (ry * ca + uy * sa) * rr, cz + uz * sa * rr);
                }
            }
            glEnd();
        }
    };
    for (const Car &car : m_cars) {
        if (car.render.empty())
            continue;
        const float fl = std::max(1e-4f, std::sqrt(car.fwd[0] * car.fwd[0] + car.fwd[1] * car.fwd[1]));
        const float fx = car.fwd[0] / fl, fy = car.fwd[1] / fl;
        for (int side = 0; side < 2; ++side) {
            if (car.haveHead[side]) {
                const float *p = car.headPos[side];
                const float vx = cam.x - p[0], vy = cam.y - p[1];
                const float facing = (fx * vx + fy * vy) / std::max(1e-4f, std::sqrt(vx * vx + vy * vy));
                const float seen = 0.12f + 0.88f * std::clamp(facing * 1.6f, 0.0f, 1.0f);
                halo(p, 1.0f, 0.95f, 0.78f, 0.42f, 0.55f * car.lampHead * seen * dayK);
            }
            if (car.haveTail[side]) {
                const float *p = car.tailPos[side];
                const float vx = cam.x - p[0], vy = cam.y - p[1];
                const float facing = -(fx * vx + fy * vy) / std::max(1e-4f, std::sqrt(vx * vx + vy * vy));
                const float seen = 0.12f + 0.88f * std::clamp(facing * 1.6f, 0.0f, 1.0f);
                const float b = car.lampBrake;
                halo(p, 1.0f, 0.06f + 0.08f * b, 0.03f + 0.04f * b, 0.14f + 0.30f * b, (0.16f + 0.40f * b) * seen * dayK);
            }
        }
        if (day)
            continue;
        // Night: headlight pools ahead of the car and the red wash behind a braking one, flat on the road.
        const float carZ = car.xf[11];
        // The road under a point of the pool (the car may be pitched or on a kerb, so its lowest point is no guide).
        auto roadZ = [&](float x, float y) {
            float z = carZ;
            return (GroundHeight(x, y, carZ + 1.5f, z) ? z : carZ - 0.3f) + 0.05f;
        };
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-2.0f, -2.0f);
        auto pool = [&](const float p[3], float dirx, float diry, float len, float w0, float w1, float r, float g, float b, float a0) {
            const float nx = -diry, ny = dirx;
            constexpr int kSlices = 8;
            glBegin(GL_TRIANGLE_STRIP);
            for (int i = 0; i <= kSlices; ++i) {
                const float u = float(i) / kSlices, d = u * len, w = w0 + (w1 - w0) * u;
                const float a = a0 * std::pow(1.0f - u, 1.4f);
                const float cx = p[0] + dirx * d, cy = p[1] + diry * d;
                glColor4f(r, g, b, 0.0f);
                glVertex3f(cx + nx * w, cy + ny * w, roadZ(cx + nx * w, cy + ny * w));
                glColor4f(r, g, b, a);
                glVertex3f(cx, cy, roadZ(cx, cy));
            }
            glEnd();
            glBegin(GL_TRIANGLE_STRIP);
            for (int i = 0; i <= kSlices; ++i) {
                const float u = float(i) / kSlices, d = u * len, w = w0 + (w1 - w0) * u;
                const float a = a0 * std::pow(1.0f - u, 1.4f);
                const float cx = p[0] + dirx * d, cy = p[1] + diry * d;
                glColor4f(r, g, b, a);
                glVertex3f(cx, cy, roadZ(cx, cy));
                glColor4f(r, g, b, 0.0f);
                glVertex3f(cx - nx * w, cy - ny * w, roadZ(cx - nx * w, cy - ny * w));
            }
            glEnd();
        };
        for (int side = 0; side < 2; ++side) {
            if (car.haveHead[side] && car.lampHead > 0.02f)
                pool(car.headPos[side], fx, fy, 26.0f, 0.5f, 4.2f, 1.0f, 0.93f, 0.72f, 0.34f * car.lampHead);
            if (car.haveTail[side] && car.lampBrake > 0.02f)
                pool(car.tailPos[side], -fx, -fy, 4.5f, 0.4f, 1.8f, 1.0f, 0.08f, 0.04f, 0.30f * car.lampBrake);
        }
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    glPopAttrib();
}

// Tyre smoke (soft grey puffs that grow, rise and fade) and skid marks (dark ribbons on the road that fade over ~30 s), ISSUES #294.
void TrackWorld::DrawTyreEffects(const Camera &cam) const
{
    if ((m_smoke.empty() && m_skids.empty()) || getenv("NFSU2_NO_TYRE_FX"))
        return;
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_CURRENT_BIT | GL_POLYGON_BIT);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_CULL_FACE);
    glDisable(GL_FOG);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    const unsigned now = SDL_GetTicks();
    if (!m_skids.empty()) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-3.0f, -3.0f);
        glBegin(GL_QUADS);
        for (const Skid &k : m_skids) {
            const float age = float(now - k.born) * 0.001f;
            if (age > 30.0f)
                continue;
            const float dx = k.x1 - k.x0, dy = k.y1 - k.y0, len = std::hypot(dx, dy);
            if (len < 1e-4f || len > 6.0f)
                continue; // a jump (a respawn), not a mark
            const float nx = -dy / len * k.width * 0.5f, ny = dx / len * k.width * 0.5f;
            glColor4f(0.02f, 0.02f, 0.025f, k.strength * (1.0f - age / 30.0f) * 0.7f);
            glVertex3f(k.x0 + nx, k.y0 + ny, k.z0), glVertex3f(k.x0 - nx, k.y0 - ny, k.z0);
            glVertex3f(k.x1 - nx, k.y1 - ny, k.z1), glVertex3f(k.x1 + nx, k.y1 + ny, k.z1);
        }
        glEnd();
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    for (const Smoke &s : m_smoke) {
        float dx = s.x - cam.x, dy = s.y - cam.y, dz = s.z - cam.z;
        const float dl = std::max(1e-3f, std::sqrt(dx * dx + dy * dy + dz * dz));
        dx /= dl, dy /= dl, dz /= dl;
        float rx = dy, ry = -dx;
        const float rl = std::max(1e-4f, std::sqrt(rx * rx + ry * ry));
        rx /= rl, ry /= rl;
        const float ux = ry * dz, uy = -rx * dz, uz = rx * dy - ry * dx;
        const float u = s.age / s.life;
        const float R = s.size * (0.6f + 2.4f * u);
        const float alpha = s.a * (1.0f - u) * std::min(1.0f, u * 8.0f);
        constexpr float kRing[3][2] = {{0.0f, 1.0f}, {0.55f, 0.45f}, {1.0f, 0.0f}};
        for (int ring = 0; ring < 2; ++ring) {
            glBegin(GL_TRIANGLE_STRIP);
            for (int i = 0; i <= 12; ++i) {
                const float a = float(i) * 6.2831853f / 12.0f, ca = std::cos(a), sa = std::sin(a);
                for (int e = 0; e < 2; ++e) {
                    const float rr = kRing[ring + e][0] * R;
                    glColor4f(0.86f, 0.86f, 0.88f, kRing[ring + e][1] * alpha);
                    glVertex3f(s.x + (rx * ca + ux * sa) * rr, s.y + (ry * ca + uy * sa) * rr, s.z + uz * sa * rr);
                }
            }
            glEnd();
        }
    }
    glPopAttrib();
}
