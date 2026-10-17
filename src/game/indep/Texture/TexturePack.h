#pragma once
#include <cstdint>
#include <string>
#include <vector>

// PS2 texture pack (TPK), chunk 0xb3300000:
//   0xb3310000 header container
//     0x33310001 pack name / path
//     0x33310002 name hashes
//     0x33310004 texture info, 124 bytes per texture
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
};

struct DecodedTexture {
    int width = 0, height = 0;
    std::vector<uint32_t> rgba; // 0xAABBGGRR, ready for SDL_PIXELFORMAT_ABGR8888
};

class TexturePack {
public:
    bool Parse(const uint8_t *data, size_t size);
    const TextureInfo *Find(const char *name) const;
    bool Decode(const TextureInfo &tex, DecodedTexture &out) const;
    size_t Count() const { return m_textures.size(); }

private:
    std::vector<TextureInfo> m_textures;
    const uint8_t *m_blob = nullptr;
    size_t m_blobSize = 0;
};
