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
    case NameHash("BRAKELIGHT_LEFT"): case NameHash("BRAKELIGHT_RIGHT"): case NameHash("BRAKELIGHT_CENTRE"):
        return set(0.75f, 0.08f, 0.06f);
    // The clear cover over each lamp cluster (no texture ships for it; the shader draws it translucent, ISSUES #286).
    case NameHash("HEADLIGHT_GLASS_LEFT"): case NameHash("HEADLIGHT_GLASS_RIGHT"):
        return set(0.92f, 0.95f, 0.98f);
    case NameHash("BRAKELIGHT_GLASS_LEFT"): case NameHash("BRAKELIGHT_GLASS_RIGHT"):
        return set(0.80f, 0.07f, 0.05f);
    case NameHash("WINDOW_TINT1"): return set(0.06f, 0.07f, 0.09f);
    case NameHash("WINDOW_TINT2"): return set(0.03f, 0.035f, 0.045f);
    case NameHash("WINDOW_TINT3"): return set(0.005f, 0.005f, 0.01f);
    // Underglow (ISSUES #227): NEON_GLOW_<1..10>, the shop's ten colours (stand-in palette).
    case NameHash("NEON_GLOW_1"): return set(1.00f, 0.10f, 0.10f);
    case NameHash("NEON_GLOW_2"): return set(1.00f, 0.50f, 0.05f);
    case NameHash("NEON_GLOW_3"): return set(1.00f, 0.95f, 0.10f);
    case NameHash("NEON_GLOW_4"): return set(0.30f, 1.00f, 0.15f);
    case NameHash("NEON_GLOW_5"): return set(0.05f, 1.00f, 0.60f);
    case NameHash("NEON_GLOW_6"): return set(0.10f, 0.90f, 1.00f);
    case NameHash("NEON_GLOW_7"): return set(0.15f, 0.30f, 1.00f);
    case NameHash("NEON_GLOW_8"): return set(0.60f, 0.20f, 1.00f);
    case NameHash("NEON_GLOW_9"): return set(1.00f, 0.20f, 0.80f);
    case NameHash("NEON_GLOW_10"): return set(1.00f, 1.00f, 1.00f);
    case NameHash("CARBONFIBRE"):
        return set(0.12f, 0.12f, 0.13f);
    default:
        return false;
    }
}
} // namespace CarMaterial
