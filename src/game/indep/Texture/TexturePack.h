#pragma once
#include <cstdint>
#include <string>
#include <memory>
#include <vector>

// PS2 texture pack (TPK), chunk 0xb3300000:
//   0xb3310000 header container
//     0x33310001 pack name / path
//     0x33310002 name hashes
//     0x33310004 texture info, 124 bytes per texture (or 0x33310003: compressed entries, see .cpp)
//     0x33310005 GS register settings, 84 bytes per texture (not needed natively)
//   0xb3320000 data container
//     0x33320002 palettes followed by pixel data, after 0x11 padding to 0x80 alignment
// The loader code in the game has not been located yet; this layout comes from the data.

struct TextureInfo {
    std::string name;
    uint32_t nameHash;
    uint32_t dataOffset, paletteOffset, dataSize, paletteSize;
    uint16_t width, height;
    uint8_t bitsPerPixel;
    // As MW's TextureInfo +0x55/+0x56: usage 0 none, 1 punch-through, 2 modulated; blend 0 copy,
    // 1 blend, 2 additive, ... (roads use "modulated + copy": their alpha is not opacity).
    uint8_t alphaUsage = 0, alphaBlend = 0;
    const uint8_t *pixels = nullptr;  // resolved at parse time (inside the bundle or a decompressed copy)
    const uint8_t *palette = nullptr;
};

struct DecodedTexture {
    int width = 0, height = 0;
    std::vector<uint32_t> rgba; // 0xAABBGGRR, ready for SDL_PIXELFORMAT_ABGR8888
};

// Dev aid: while set, Decode reads pixels row by row instead of through the GS block swizzle.
void SetForceLinearDecode(bool on);

class TexturePack {
public:
    bool Parse(const uint8_t *data, size_t size);
    const TextureInfo *Find(const char *name) const;
    bool Decode(const TextureInfo &tex, DecodedTexture &out) const;
    size_t Count() const { return m_textures.size(); }
    const TextureInfo &At(size_t i) const { return m_textures[i]; }

private:
    std::vector<TextureInfo> m_textures;
    std::vector<std::unique_ptr<std::vector<uint8_t>>> m_owned; // decompressed texture blocks
};
