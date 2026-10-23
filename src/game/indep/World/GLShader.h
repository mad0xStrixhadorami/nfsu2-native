#pragma once
#include <cstddef>
#include <initializer_list>
#include <utility>

#include <SDL.h>
#include <SDL_opengl.h>

// GLSL support for the world renderer (ISSUES #263). SDL's OpenGL render backend hands out a compatibility context, so
// the shaders are GLSL 1.20 programs that keep using the fixed-function vertex arrays (gl_Vertex, gl_Color, gl_MultiTexCoord0)
// and the matrices GL already holds; entry points beyond GL 1.5 come from SDL_GL_GetProcAddress.
namespace GLShader {
// Loads the GL 2.0 / 3.0 entry points (shaders, uniforms, attributes, framebuffers). False when one is missing.
bool Load();
bool Available();

// Compiles and links a program; `attribs` binds generic attribute slots before linking. Returns 0 (and prints the log) on
// failure. `name` only labels the log.
GLuint Build(const char *name, const char *vertexSource, const char *fragmentSource,
             std::initializer_list<std::pair<GLuint, const char *>> attribs = {});
void Use(GLuint program);
GLint Uniform(GLuint program, const char *name);
void Set1f(GLint loc, float v);
void Set2f(GLint loc, float a, float b);
void Set3f(GLint loc, float a, float b, float c);
void Set1i(GLint loc, int v);

// Generic vertex attributes (the car shader's normals and gloss).
void EnableAttrib(GLuint slot);
void DisableAttrib(GLuint slot);
void AttribPointer(GLuint slot, int components, GLenum type, bool normalized, GLsizei stride, const void *pointer);

// Framebuffers for the post-processing passes.
GLuint GenFramebuffer();
void DeleteFramebuffer(GLuint fbo);
void BindFramebuffer(GLuint fbo); // 0 = the window's framebuffer
bool AttachColor(GLuint texture);  // attaches to the currently bound framebuffer; true when complete
// Resolves the window's (possibly multisampled) framebuffer into `dst` (size w x h); leaves the window framebuffer bound.
void BlitWindowTo(GLuint dst, int w, int h);
void ActiveTexture(int unit);
} // namespace GLShader
