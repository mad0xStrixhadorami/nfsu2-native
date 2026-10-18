#include "game/indep/World/ModelViewGL.h"

#include <algorithm>
#include <cmath>

#include "game/indep/World/GLBridge.h"

bool DrawModelGL(SDL_Renderer *renderer, const std::vector<SolidMesh::Triangle> &model, const SDL_Rect &pixels,
                 float yaw, SDL_Color paint)
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

    const float aspect = float(pixels.w) / float(std::max(pixels.h, 1));
    const float tanHalf = std::tan(16.0f * 3.14159265f / 180.0f);
    // Fit the length (horizontal) rather than the bounding sphere: cars are long and low.
    const float dist = radius / (tanHalf * aspect) * 1.02f, nearZ = std::max(0.05f, dist - radius * 2),
                farZ = dist + radius * 2;
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
    for (const SolidMesh::Triangle &t : model) {
        const SolidMesh::Point &a = t.v[0], &b = t.v[1], &c = t.v[2];
        float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
        float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len < 1e-9f)
            continue;
        float lit = 0.3f + 0.7f * std::fabs((nx * lx + ny * ly + nz * lz) / len);
        glColor3ub(GLubyte(paint.r * lit), GLubyte(paint.g * lit), GLubyte(paint.b * lit));
        glVertex3f(a.x, a.y, a.z);
        glVertex3f(b.x, b.y, b.z);
        glVertex3f(c.x, c.y, c.z);
    }
    glEnd();
    return true;
}
