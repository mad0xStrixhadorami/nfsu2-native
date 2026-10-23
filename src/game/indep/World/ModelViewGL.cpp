#include <map>
#include <array>
#include "game/indep/World/ModelViewGL.h"

#include <algorithm>
#include <cmath>

#include "game/indep/World/CarMaterial.h"
#include "game/indep/World/CarSkin.h"
#include "game/indep/World/GLBridge.h"

namespace {
struct SkinState {
    uint32_t slot = 0, version = 0, uploaded = 0xffffffffu;
    int size = 0;
    std::vector<uint32_t> pixels;
    GLuint tex = 0;
    bool active = false;
} g_skin;
} // namespace

void SetModelViewSkin(uint32_t slotHash, const std::vector<uint32_t> *rgba, int size, uint32_t version)
{
    if (!rgba) {
        g_skin.active = false;
        return;
    }
    if (g_skin.active && g_skin.slot == slotHash && g_skin.version == version && g_skin.size == size)
        return;
    g_skin.slot = slotHash;
    g_skin.size = size;
    g_skin.version = version;
    g_skin.pixels = *rgba;
    g_skin.active = true;
}

bool DrawModelGL(SDL_Renderer *renderer, const std::vector<SolidMesh::Triangle> &model, const SDL_Rect &pixels,
                 float yaw, SDL_Color paint, const std::vector<uint32_t> *textures, size_t wheelsFrom,
                 const std::vector<uint8_t> *rimMask, SDL_Color rimPaint,
                 const std::vector<uint8_t> *caliperMask, SDL_Color caliperPaint,
                 const std::vector<uint8_t> *mirrorMask, SDL_Color mirrorPaint,
                 const std::vector<uint8_t> *spoilerMask, SDL_Color spoilerPaint,
                 const std::vector<uint8_t> *roofMask, SDL_Color roofPaint,
                 const std::vector<uint8_t> *engineMask, SDL_Color enginePaint,
                 const std::vector<uint8_t> *exhaustMask, SDL_Color exhaustPaint)
{
    if (model.empty() || !GLBridge::Available(renderer))
        return false;
    int outW = 0, outH = 0;
    SDL_GetRendererOutputSize(renderer, &outW, &outH);

    // Bounds of the model, to frame it.
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const SolidMesh::Triangle &t : model)
        for (const SolidMesh::Point &p : t.v) {
            const float c[3] = {p.x, p.y, p.z};
            for (int a = 0; a < 3; ++a)
                lo[a] = std::min(lo[a], c[a]), hi[a] = std::max(hi[a], c[a]);
        }
    const float centre[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    const float radius = 0.5f * std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) +
                                          (hi[2] - lo[2]) * (hi[2] - lo[2]));

    GLBridge::Scope scope(renderer);
    const int y = outH - (pixels.y + pixels.h); // GL viewports count from the bottom
    glViewport(pixels.x, y, pixels.w, pixels.h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(pixels.x, y, pixels.w, pixels.h);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_TEXTURE_2D);
    glShadeModel(GL_FLAT);
    if (g_skin.active && g_skin.uploaded != g_skin.version) {
        if (!g_skin.tex)
            glGenTextures(1, &g_skin.tex);
        glBindTexture(GL_TEXTURE_2D, g_skin.tex);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0); // SDL's renderer may leave its own pitch set
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_skin.size, g_skin.size, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_skin.pixels.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        g_skin.uploaded = g_skin.version;
    }
    std::vector<size_t> skinTris;

    const float aspect = float(pixels.w) / float(std::max(pixels.h, 1));
    const float tanHalf = std::tan(16.0f * 3.14159265f / 180.0f);
    // Fit while turning: the footprint radius horizontally, the projected height under the 14 degree
    // elevation vertically, plus half the footprint for the near side of the car.
    const float hx = (hi[0] - lo[0]) * 0.5f, hy = (hi[1] - lo[1]) * 0.5f, hz = (hi[2] - lo[2]) * 0.5f;
    const float footprint = std::sqrt(hx * hx + hy * hy);
    const float elev = 14.0f * 3.14159265f / 180.0f;
    const float projH = hz * std::cos(elev) + footprint * std::sin(elev);
    const float dist = std::max(footprint / (tanHalf * aspect), projH / tanHalf) + footprint * 0.5f;
    const float nearZ = std::max(0.05f, dist - radius * 2), farZ = dist + radius * 2;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-nearZ * tanHalf * aspect, nearZ * tanHalf * aspect, -nearZ * tanHalf, nearZ * tanHalf, nearZ, farZ);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    // Camera on the -X side looking at the centre, raised 14 degrees; the model turns by yaw.
    glTranslatef(0, 0, -dist);
    glRotatef(14.0f, 1, 0, 0);
    glRotatef(-90.0f, 1, 0, 0); // car +Z up -> GL +Y up
    glRotatef(90.0f, 0, 0, 1);  // view the car from its side
    glRotatef(yaw * 57.29578f, 0, 0, 1);
    glTranslatef(-centre[0], -centre[1], -centre[2]);

    // Flat shading by the triangle normal against a light from above-front.
    const float lx = 0.45f, ly = 0.35f, lz = 0.82f;
    glBegin(GL_TRIANGLES);
    const bool slots = textures && textures->size() == model.size();
    for (size_t i = 0; i < model.size(); ++i) {
        const SolidMesh::Triangle &t = model[i];
        const SolidMesh::Point &a = t.v[0], &b = t.v[1], &c = t.v[2];
        float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
        float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len < 1e-9f)
            continue;
        float lit = 0.3f + 0.7f * std::fabs((nx * lx + ny * ly + nz * lz) / len);
        float rgb[3] = {paint.r / 255.0f, paint.g / 255.0f, paint.b / 255.0f};
        const bool spoiler = spoilerMask && spoilerMask->size() == model.size() && (*spoilerMask)[i];
        const bool mirror = mirrorMask && mirrorMask->size() == model.size() && (*mirrorMask)[i];
        const bool roof = roofMask && roofMask->size() == model.size() && (*roofMask)[i];
        const bool engine = engineMask && engineMask->size() == model.size() && (*engineMask)[i];
        const bool exhaust = exhaustMask && exhaustMask->size() == model.size() && (*exhaustMask)[i];
        if (spoiler)
            rgb[0] = spoilerPaint.r / 255.0f, rgb[1] = spoilerPaint.g / 255.0f, rgb[2] = spoilerPaint.b / 255.0f;
        else if (mirror)
            rgb[0] = mirrorPaint.r / 255.0f, rgb[1] = mirrorPaint.g / 255.0f, rgb[2] = mirrorPaint.b / 255.0f;
        else if (roof)
            rgb[0] = roofPaint.r / 255.0f, rgb[1] = roofPaint.g / 255.0f, rgb[2] = roofPaint.b / 255.0f;
        else if (engine)
            rgb[0] = enginePaint.r / 255.0f, rgb[1] = enginePaint.g / 255.0f, rgb[2] = enginePaint.b / 255.0f;
        else if (exhaust)
            rgb[0] = exhaustPaint.r / 255.0f, rgb[1] = exhaustPaint.g / 255.0f, rgb[2] = exhaustPaint.b / 255.0f;
        else if (i >= wheelsFrom) {
            const bool rim = rimMask && rimMask->size() == model.size() && (*rimMask)[i];
            const bool caliper = caliperMask && caliperMask->size() == model.size() && (*caliperMask)[i];
            if (rim)
                rgb[0] = rimPaint.r / 255.0f, rgb[1] = rimPaint.g / 255.0f, rgb[2] = rimPaint.b / 255.0f;
            else if (caliper)
                rgb[0] = caliperPaint.r / 255.0f, rgb[1] = caliperPaint.g / 255.0f, rgb[2] = caliperPaint.b / 255.0f;
            else
                rgb[0] = rgb[1] = rgb[2] = 0.09f;
        } else if (slots) {
            if (g_skin.active && (*textures)[i] == g_skin.slot) {
                skinTris.push_back(i); // drawn textured below
                continue;
            }
            CarMaterial::SlotColour((*textures)[i], rgb);
        }
        glColor3ub(GLubyte(255 * rgb[0] * lit), GLubyte(255 * rgb[1] * lit), GLubyte(255 * rgb[2] * lit));
        glVertex3f(a.x, a.y, a.z);
        glVertex3f(b.x, b.y, b.z);
        glVertex3f(c.x, c.y, c.z);
    }
    glEnd();
    if (!skinTris.empty()) {
        glMatrixMode(GL_TEXTURE); // SDL's renderer may leave a texture matrix behind
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-2.0f, -4.0f); // the trim shell lies on the body: the skin wins the tie
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, g_skin.tex);
        // The vinyl atlas is a planar side projection of the body (CarSkin::Projection, ISSUES #223).
        const CarSkin::Projection proj = CarSkin::Projection::ForBody(model, *textures, g_skin.slot);
        // Strip-decode bridge slivers (very long edges) would smear the texture: those stay flat paint.
        std::vector<size_t> flatTris;
        auto longEdge = [](const SolidMesh::Triangle &t) {
            auto d2 = [](const SolidMesh::Point &p, const SolidMesh::Point &q) {
                return (p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) + (p.z - q.z) * (p.z - q.z);
            };
            return std::max(d2(t.v[0], t.v[1]), std::max(d2(t.v[1], t.v[2]), d2(t.v[2], t.v[0]))) > 1e9f; // disabled: low-poly panels have long edges
        };
        glBegin(GL_TRIANGLES);
        for (size_t i : skinTris) {
            if (longEdge(model[i])) {
                flatTris.push_back(i);
                continue;
            }
            const SolidMesh::Triangle &t = model[i];
            const SolidMesh::Point &a = t.v[0], &b = t.v[1], &c = t.v[2];
            float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
            float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
            float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len < 1e-9f)
                continue;
            const float lit = 0.3f + 0.7f * std::fabs((nx * lx + ny * ly + nz * lz) / len);
            glColor3ub(GLubyte(255 * lit), GLubyte(255 * lit), GLubyte(255 * lit));
            const bool left = (a.y + b.y + c.y) > 0.0f;
            for (const SolidMesh::Point *p : {&a, &b, &c}) {
                float s, tc;
                proj.Map(p->x, p->z, left, s, tc);
                glTexCoord2f(s, tc);
                glVertex3f(p->x, p->y, p->z);
            }
        }
        glEnd();
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glBegin(GL_TRIANGLES);
        for (size_t i : flatTris) {
            const SolidMesh::Triangle &t = model[i];
            float ux = t.v[1].x - t.v[0].x, uy = t.v[1].y - t.v[0].y, uz = t.v[1].z - t.v[0].z;
            float vx = t.v[2].x - t.v[0].x, vy = t.v[2].y - t.v[0].y, vz = t.v[2].z - t.v[0].z;
            float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
            float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len < 1e-9f)
                continue;
            const float lit = 0.3f + 0.7f * std::fabs((nx * lx + ny * ly + nz * lz) / len);
            glColor3ub(GLubyte(paint.r * lit), GLubyte(paint.g * lit), GLubyte(paint.b * lit));
            for (const SolidMesh::Point &p : t.v)
                glVertex3f(p.x, p.y, p.z);
        }
        glEnd();
    }
    return true;
}
