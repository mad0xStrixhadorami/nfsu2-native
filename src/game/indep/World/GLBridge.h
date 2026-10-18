#pragma once
#include <cstddef>

#include <SDL.h>
#include <SDL_opengl.h>

// Drawing with OpenGL inside SDL's "opengl" render backend (ISSUES.md #108): SDL owns the context
// and caches GL state, so GL code runs inside a Scope that flushes SDL's queue, saves that state
// and restores it afterwards. Buffer/program entry points come from SDL_GL_GetProcAddress.
namespace GLBridge {
// True when the renderer is SDL's OpenGL backend with a depth buffer and GL 1.5 entry points.
bool Available(SDL_Renderer *renderer);

struct Scope {
    explicit Scope(SDL_Renderer *renderer);
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

private:
    GLint m_program = 0, m_arrayBuffer = 0;
};

void GenBuffers(GLsizei n, GLuint *buffers);
void DeleteBuffers(GLsizei n, const GLuint *buffers);
void BindArrayBuffer(GLuint buffer);
void ArrayBufferData(ptrdiff_t size, const void *data); // GL_STATIC_DRAW
} // namespace GLBridge
