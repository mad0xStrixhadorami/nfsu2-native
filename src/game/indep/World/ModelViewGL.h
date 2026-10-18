#pragma once
#include <vector>

#include <SDL.h>

#include "game/indep/World/SolidMesh.h"

// Draws a car-space model (+X forward, +Y left, +Z up) with OpenGL into a window-pixel rectangle,
// turned by `yaw` and lit by a fixed light: the car-select preview (ISSUES.md #108). Returns false
// when the GL bridge is unavailable (callers fall back to their software drawing).
bool DrawModelGL(SDL_Renderer *renderer, const std::vector<SolidMesh::Triangle> &model, const SDL_Rect &pixels,
                 float yaw, SDL_Color paint);
