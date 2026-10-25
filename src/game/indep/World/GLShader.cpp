#include "game/indep/World/GLShader.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
typedef GLuint(APIENTRY *PFNCREATESHADER)(GLenum);
typedef void(APIENTRY *PFNSHADERSOURCE)(GLuint, GLsizei, const char *const *, const GLint *);
typedef void(APIENTRY *PFNCOMPILESHADER)(GLuint);
typedef void(APIENTRY *PFNGETSHADERIV)(GLuint, GLenum, GLint *);
typedef void(APIENTRY *PFNGETINFOLOG)(GLuint, GLsizei, GLsizei *, char *);
typedef GLuint(APIENTRY *PFNCREATEPROGRAM)();
typedef void(APIENTRY *PFNATTACHSHADER)(GLuint, GLuint);
typedef void(APIENTRY *PFNLINKPROGRAM)(GLuint);
typedef void(APIENTRY *PFNGETPROGRAMIV)(GLuint, GLenum, GLint *);
typedef void(APIENTRY *PFNUSEPROGRAM)(GLuint);
typedef void(APIENTRY *PFNBINDATTRIB)(GLuint, GLuint, const char *);
typedef GLint(APIENTRY *PFNGETUNIFORMLOC)(GLuint, const char *);
typedef void(APIENTRY *PFNUNIFORM1F)(GLint, float);
typedef void(APIENTRY *PFNUNIFORM2F)(GLint, float, float);
typedef void(APIENTRY *PFNUNIFORM3F)(GLint, float, float, float);
typedef void(APIENTRY *PFNUNIFORM1I)(GLint, int);
typedef void(APIENTRY *PFNENABLEVA)(GLuint);
typedef void(APIENTRY *PFNVAPOINTER)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void(APIENTRY *PFNGENFB)(GLsizei, GLuint *);
typedef void(APIENTRY *PFNDELFB)(GLsizei, const GLuint *);
typedef void(APIENTRY *PFNBINDFB)(GLenum, GLuint);
typedef void(APIENTRY *PFNFBTEX)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum(APIENTRY *PFNCHECKFB)(GLenum);
typedef void(APIENTRY *PFNBLITFB)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef void(APIENTRY *PFNACTIVETEX)(GLenum);

PFNCREATESHADER pCreateShader;
PFNSHADERSOURCE pShaderSource;
PFNCOMPILESHADER pCompileShader;
PFNGETSHADERIV pGetShaderiv;
PFNGETINFOLOG pGetShaderInfoLog, pGetProgramInfoLog;
PFNCREATEPROGRAM pCreateProgram;
PFNATTACHSHADER pAttachShader;
PFNLINKPROGRAM pLinkProgram;
PFNGETPROGRAMIV pGetProgramiv;
PFNUSEPROGRAM pUseProgram;
PFNBINDATTRIB pBindAttribLocation;
PFNGETUNIFORMLOC pGetUniformLocation;
PFNUNIFORM1F pUniform1f;
PFNUNIFORM2F pUniform2f;
PFNUNIFORM3F pUniform3f;
PFNUNIFORM1I pUniform1i;
PFNENABLEVA pEnableVA, pDisableVA;
PFNVAPOINTER pVAPointer;
PFNGENFB pGenFramebuffers;
PFNDELFB pDeleteFramebuffers;
PFNBINDFB pBindFramebuffer;
PFNFBTEX pFramebufferTexture2D;
PFNCHECKFB pCheckFramebufferStatus;
PFNBLITFB pBlitFramebuffer;
PFNACTIVETEX pActiveTexture;
int g_state = 0; // 0 unknown, 1 loaded, -1 failed

constexpr GLenum kVertexShader = 0x8B31, kFragmentShader = 0x8B30, kCompileStatus = 0x8B81, kLinkStatus = 0x8B82;
constexpr GLenum kFramebuffer = 0x8D40, kColorAttachment0 = 0x8CE0, kFramebufferComplete = 0x8CD5;

template <typename T> bool Get(T &fn, const char *name)
{
    fn = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
    if (!fn)
        fprintf(stderr, "[GLSL] missing %s\n", name);
    return fn != nullptr;
}

GLuint Compile(GLenum type, const char *name, const char *source)
{
    GLuint sh = pCreateShader(type);
    pShaderSource(sh, 1, &source, nullptr);
    pCompileShader(sh);
    GLint ok = 0;
    pGetShaderiv(sh, kCompileStatus, &ok);
    if (!ok) {
        char log[2048] = {};
        pGetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
        fprintf(stderr, "[GLSL] %s %s shader failed:\n%s\n", name, type == kVertexShader ? "vertex" : "fragment", log);
        return 0;
    }
    return sh;
}
} // namespace

namespace GLShader {
bool Load()
{
    if (g_state != 0)
        return g_state == 1;
    g_state = -1;
    if (getenv("NFSU2_NO_SHADERS"))
        return false;
    bool ok = true;
    ok &= Get(pCreateShader, "glCreateShader") && Get(pShaderSource, "glShaderSource") && Get(pCompileShader, "glCompileShader");
    ok &= Get(pGetShaderiv, "glGetShaderiv") && Get(pGetShaderInfoLog, "glGetShaderInfoLog") && Get(pGetProgramInfoLog, "glGetProgramInfoLog");
    ok &= Get(pCreateProgram, "glCreateProgram") && Get(pAttachShader, "glAttachShader") && Get(pLinkProgram, "glLinkProgram");
    ok &= Get(pGetProgramiv, "glGetProgramiv") && Get(pUseProgram, "glUseProgram") && Get(pBindAttribLocation, "glBindAttribLocation");
    ok &= Get(pGetUniformLocation, "glGetUniformLocation") && Get(pUniform1f, "glUniform1f") && Get(pUniform2f, "glUniform2f");
    ok &= Get(pUniform3f, "glUniform3f") && Get(pUniform1i, "glUniform1i");
    ok &= Get(pEnableVA, "glEnableVertexAttribArray") && Get(pDisableVA, "glDisableVertexAttribArray") && Get(pVAPointer, "glVertexAttribPointer");
    ok &= Get(pGenFramebuffers, "glGenFramebuffers") && Get(pDeleteFramebuffers, "glDeleteFramebuffers") && Get(pBindFramebuffer, "glBindFramebuffer");
    ok &= Get(pFramebufferTexture2D, "glFramebufferTexture2D") && Get(pCheckFramebufferStatus, "glCheckFramebufferStatus");
    ok &= Get(pBlitFramebuffer, "glBlitFramebuffer") && Get(pActiveTexture, "glActiveTexture");
    g_state = ok ? 1 : -1;
    return ok;
}

bool Available() { return g_state == 1; }

GLuint Build(const char *name, const char *vs, const char *fs, std::initializer_list<std::pair<GLuint, const char *>> attribs)
{
    if (g_state != 1)
        return 0;
    const GLuint v = Compile(kVertexShader, name, vs), f = Compile(kFragmentShader, name, fs);
    if (!v || !f)
        return 0;
    const GLuint prog = pCreateProgram();
    pAttachShader(prog, v);
    pAttachShader(prog, f);
    for (const auto &a : attribs)
        pBindAttribLocation(prog, a.first, a.second);
    pLinkProgram(prog);
    GLint ok = 0;
    pGetProgramiv(prog, kLinkStatus, &ok);
    if (!ok) {
        char log[2048] = {};
        pGetProgramInfoLog(prog, sizeof(log) - 1, nullptr, log);
        fprintf(stderr, "[GLSL] %s link failed:\n%s\n", name, log);
        return 0;
    }
    return prog;
}

void Use(GLuint program)
{
    if (pUseProgram)
        pUseProgram(program);
}
GLint Uniform(GLuint program, const char *name) { return pGetUniformLocation(program, name); }
void Set1f(GLint loc, float v) { if (loc >= 0) pUniform1f(loc, v); }
void Set2f(GLint loc, float a, float b) { if (loc >= 0) pUniform2f(loc, a, b); }
void Set3f(GLint loc, float a, float b, float c) { if (loc >= 0) pUniform3f(loc, a, b, c); }
void Set1i(GLint loc, int v) { if (loc >= 0) pUniform1i(loc, v); }
void EnableAttrib(GLuint slot) { pEnableVA(slot); }
void DisableAttrib(GLuint slot) { pDisableVA(slot); }
void AttribPointer(GLuint slot, int components, GLenum type, bool normalized, GLsizei stride, const void *pointer)
{
    pVAPointer(slot, components, type, normalized ? GL_TRUE : GL_FALSE, stride, pointer);
}
GLuint GenFramebuffer()
{
    GLuint fbo = 0;
    pGenFramebuffers(1, &fbo);
    return fbo;
}
void DeleteFramebuffer(GLuint fbo) { pDeleteFramebuffers(1, &fbo); }
void BindFramebuffer(GLuint fbo) { pBindFramebuffer(kFramebuffer, fbo); }
bool AttachColor(GLuint texture)
{
    pFramebufferTexture2D(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, texture, 0);
    return pCheckFramebufferStatus(kFramebuffer) == kFramebufferComplete;
}
} // namespace GLShader

namespace GLShader {
void BlitWindowTo(GLuint dst, int w, int h, bool depth)
{
    constexpr GLenum kRead = 0x8CA8, kDraw = 0x8CA9;
    pBindFramebuffer(kRead, 0);
    pBindFramebuffer(kDraw, dst);
    pBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT | (depth ? GL_DEPTH_BUFFER_BIT : 0), GL_NEAREST);
    pBindFramebuffer(kFramebuffer, 0);
}

bool AttachDepth(GLuint texture)
{
    constexpr GLenum kDepthAttachment = 0x8D00;
    pFramebufferTexture2D(kFramebuffer, kDepthAttachment, GL_TEXTURE_2D, texture, 0);
    return pCheckFramebufferStatus(kFramebuffer) == kFramebufferComplete;
}
void ActiveTexture(int unit) { pActiveTexture(0x84C0 + GLenum(unit)); }
} // namespace GLShader
