#include "game/indep/World/GLBridge.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
typedef void(APIENTRY *PFNGENBUFFERS)(GLsizei, GLuint *);
typedef void(APIENTRY *PFNDELETEBUFFERS)(GLsizei, const GLuint *);
typedef void(APIENTRY *PFNBINDBUFFER)(GLenum, GLuint);
typedef void(APIENTRY *PFNBUFFERDATA)(GLenum, ptrdiff_t, const void *, GLenum);
typedef void(APIENTRY *PFNUSEPROGRAM)(GLuint);
PFNGENBUFFERS pglGenBuffers;
PFNDELETEBUFFERS pglDeleteBuffers;
PFNBINDBUFFER pglBindBuffer;
PFNBUFFERDATA pglBufferData;
PFNUSEPROGRAM pglUseProgram;
int g_state = 0; // 0 unknown, 1 available, -1 not

constexpr GLenum kArrayBuffer = 0x8892;        // GL_ARRAY_BUFFER
constexpr GLenum kStaticDraw = 0x88E4;         // GL_STATIC_DRAW
constexpr GLenum kCurrentProgram = 0x8B8D;     // GL_CURRENT_PROGRAM
constexpr GLenum kArrayBufferBinding = 0x8894; // GL_ARRAY_BUFFER_BINDING
} // namespace

namespace GLBridge {
bool Available(SDL_Renderer *renderer)
{
    if (g_state == 0) {
        g_state = -1;
        SDL_RendererInfo info;
        if (!getenv("NFSU2_SOFTWARE_WORLD") && renderer && SDL_GetRendererInfo(renderer, &info) == 0 &&
            strcmp(info.name, "opengl") == 0) {
            pglGenBuffers = reinterpret_cast<PFNGENBUFFERS>(SDL_GL_GetProcAddress("glGenBuffers"));
            pglDeleteBuffers = reinterpret_cast<PFNDELETEBUFFERS>(SDL_GL_GetProcAddress("glDeleteBuffers"));
            pglBindBuffer = reinterpret_cast<PFNBINDBUFFER>(SDL_GL_GetProcAddress("glBindBuffer"));
            pglBufferData = reinterpret_cast<PFNBUFFERDATA>(SDL_GL_GetProcAddress("glBufferData"));
            pglUseProgram = reinterpret_cast<PFNUSEPROGRAM>(SDL_GL_GetProcAddress("glUseProgram"));
            int depth = 0;
            SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &depth);
            if (pglGenBuffers && pglDeleteBuffers && pglBindBuffer && pglBufferData && pglUseProgram && depth >= 16)
                g_state = 1;
            const GLubyte *name = glGetString(GL_RENDERER);
            int samples = 0;
            SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES, &samples);
            printf("[GL] renderer %s (%s, depth %d, %d samples)\n", g_state == 1 ? "on" : "unavailable",
                   name ? reinterpret_cast<const char *>(name) : "?", depth, samples);
            const GLubyte *ver = glGetString(GL_VERSION), *glsl = glGetString(0x8B8C /* GL_SHADING_LANGUAGE_VERSION */);
            printf("[GL] version %s, GLSL %s\n", ver ? reinterpret_cast<const char *>(ver) : "?", glsl ? reinterpret_cast<const char *>(glsl) : "?");
        }
    }
    return g_state == 1;
}

Scope::Scope(SDL_Renderer *renderer)
{
    SDL_RenderFlush(renderer); // SDL's queued draws go first
    glGetIntegerv(kCurrentProgram, &m_program);
    glGetIntegerv(kArrayBufferBinding, &m_arrayBuffer);
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
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
}

Scope::~Scope()
{
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopClientAttrib();
    glPopAttrib();
    pglBindBuffer(kArrayBuffer, GLuint(m_arrayBuffer));
    pglUseProgram(GLuint(m_program));
}

void GenBuffers(GLsizei n, GLuint *buffers) { pglGenBuffers(n, buffers); }
void DeleteBuffers(GLsizei n, const GLuint *buffers) { pglDeleteBuffers(n, buffers); }
void BindArrayBuffer(GLuint buffer) { pglBindBuffer(kArrayBuffer, buffer); }
void ArrayBufferData(ptrdiff_t size, const void *data) { pglBufferData(kArrayBuffer, size, data, kStaticDraw); }
} // namespace GLBridge
