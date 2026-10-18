#pragma once
#include <cstdint>

// Car parts reference material slots by name hash (0x134012), not always textures. These names
// come from executable strings: the paint skin (<CAR>_SKIN1, GLOBAL_SKIN1 -- composed at runtime
// from paint and vinyls, not ported), windows, lights (swapped per style kit) and carbon fibre.
namespace CarMaterial {
constexpr uint32_t NameHash(const char *s)
{
    uint32_t h = 0xffffffffu; // bStringHash
    for (; *s; ++s)
        h = h * 33 + uint8_t(*s);
    return h;
}

// Stand-in colour for a fixed slot; false when the slot is paint or a texture.
inline bool SlotColour(uint32_t hash, float rgb[3])
{
    auto set = [rgb](float r, float g, float b) { rgb[0] = r, rgb[1] = g, rgb[2] = b; return true; };
    switch (hash) {
    case NameHash("WINDOW_FRONT"): case NameHash("WINDOW_REAR"): case NameHash("WINDOW_LEFT_FRONT"):
    case NameHash("WINDOW_RIGHT_FRONT"): case NameHash("WINDOW_LEFT_REAR"): case NameHash("WINDOW_RIGHT_REAR"):
        return set(0.10f, 0.12f, 0.16f);
    case NameHash("HEADLIGHT_LEFT"): case NameHash("HEADLIGHT_RIGHT"):
        return set(0.92f, 0.92f, 0.86f);
    case NameHash("BRAKELIGHT_LEFT"): case NameHash("BRAKELIGHT_RIGHT"):
        return set(0.75f, 0.08f, 0.06f);
    case NameHash("CARBONFIBRE"):
        return set(0.12f, 0.12f, 0.13f);
    default:
        return false;
    }
}
} // namespace CarMaterial
