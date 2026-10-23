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
                 SDL_Color caliperPaint = {128, 128, 128, 255},
                 const std::vector<uint8_t> *mirrorMask = nullptr,
                 SDL_Color mirrorPaint = {60, 110, 210, 255},
                 const std::vector<uint8_t> *spoilerMask = nullptr,
                 SDL_Color spoilerPaint = {60, 110, 210, 255},
                 const std::vector<uint8_t> *roofMask = nullptr,
                 SDL_Color roofPaint = {60, 110, 210, 255},
                 const std::vector<uint8_t> *engineMask = nullptr,
                 SDL_Color enginePaint = {128, 64, 48, 255},
                 const std::vector<uint8_t> *exhaustMask = nullptr,
                 SDL_Color exhaustPaint = {150, 150, 150, 255}); // triangles from wheelsFrom on: dark tyres,
                                                                // rimMask in rimPaint, caliperMask in caliperPaint;
                                                                // mirrorMask / spoilerMask / aftermarket part
                                                                // masks anywhere in their paint

// The paint skin of the previewed car (ISSUES #223): triangles whose material slot is `slotHash`
// are drawn textured with the 512 x 512 composite (R in the low byte of each pixel) instead of the
// flat paint colour.  rgba == nullptr clears it; `version` changes when the pixels do.
void SetModelViewSkin(uint32_t slotHash, const std::vector<uint32_t> *rgba, int size, uint32_t version);
