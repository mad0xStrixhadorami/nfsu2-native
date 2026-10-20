#pragma once
#include <cstdint>
#include <vector>

#include <SDL.h>

#include "game/indep/World/SolidMesh.h"

// Draws a car-space model (+X forward, +Y left, +Z up) with OpenGL into a window-pixel rectangle,
// turned by `yaw` and lit by a fixed light: the car-select preview (ISSUES.md #108). Returns false
// when the GL bridge is unavailable (callers fall back to their software drawing).
// textures (optional): per-triangle material slot hashes; windows and lights get their colours.
bool DrawModelGL(SDL_Renderer *renderer, const std::vector<SolidMesh::Triangle> &model, const SDL_Rect &pixels,
                 float yaw, SDL_Color paint, const std::vector<uint32_t> *textures = nullptr,
                 size_t wheelsFrom = SIZE_MAX, const std::vector<uint8_t> *rimMask = nullptr,
                 SDL_Color rimPaint = {38, 38, 38, 255},
                 const std::vector<uint8_t> *caliperMask = nullptr,
                 SDL_Color caliperPaint = {128, 128, 128, 255}); // triangles from wheelsFrom on: dark tyres,
                                                               // rimMask in rimPaint, caliperMask in caliperPaint
