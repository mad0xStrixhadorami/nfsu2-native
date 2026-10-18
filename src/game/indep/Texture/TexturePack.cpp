#include "game/indep/Texture/TexturePack.h"

#include <algorithm>
#include <cstring>

#include "game/bWare/bChunk.h"
#include "game/bWare/bCompression.h"

namespace {

constexpr size_t kInfoStride = 124;

uint32_t U32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

uint16_t U16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

// Pixel data is stored in GS memory order. For 8-bit textures this maps a linear (x, y) to
// its byte in the uploaded block.
int Swizzled8Index(int x, int y, int width)
{
    int block = (y & ~0xf) * width + (x & ~0xf) * 2;
    int swap = (((y + 2) >> 2) & 1) * 4;
    int posY = (((y & ~3) >> 1) + (y & 1)) & 7;
    int column = posY * width * 2 + ((x + swap) & 7) * 4;
    int byteNum = ((y >> 1) & 1) + ((x >> 2) & 2);
    return block + column + byteNum;
}

// 256-colour CLUTs are stored with entries 8-15 and 16-23 of every 32 swapped.
int ClutIndex(int i)
{
    int k = i & 0x18;
    if (k == 8 || k == 16)
        return i ^ 0x18;
    return i;
}

uint32_t Ps2ColorToRgba(const uint8_t *c)
{
    // PS2 alpha 0x80 means fully opaque.
    uint32_t a = std::min(255, c[3] * 2);
    return c[0] | (c[1] << 8) | (c[2] << 16) | (a << 24);
}

} // namespace

static TextureInfo ReadInfoRecord(const uint8_t *e)
{
    TextureInfo t;
    t.name.assign(reinterpret_cast<const char *>(e + 12), strnlen(reinterpret_cast<const char *>(e + 12), 24));
    t.nameHash = U32(e + 36);
    t.dataOffset = U32(e + 48);
    t.paletteOffset = U32(e + 52);
    t.dataSize = U32(e + 56);
    t.paletteSize = U32(e + 60);
    t.width = U16(e + 68);
    t.height = U16(e + 70);
    t.bitsPerPixel = e[74];
    t.alphaUsage = e[0x55];
    t.alphaBlend = e[0x56];
    return t;
}

bool TexturePack::Parse(const uint8_t *data, size_t size)
{
    const uint8_t *end = data + size;
    const bChunk *tpk = bFindChunk(data, end, 0xb3300000);
    const bChunk *info = bFindChunk(data, end, 0x33310004);
    const bChunk *compressed = bFindChunk(data, end, 0x33310003);
    const bChunk *blob = bFindChunk(data, end, 0x33320002);
    if (!tpk || !blob || (!info && !compressed))
        return false;

    const uint8_t *b = blob->Data();
    const uint8_t *bEnd = b + blob->size;
    while (b < bEnd && *b == 0x11)
        ++b;
    m_textures.clear();
    m_owned.clear();

    if (info) {
        for (size_t off = 0; off + kInfoStride <= info->size; off += kInfoStride) {
            TextureInfo t = ReadInfoRecord(info->Data() + off);
            if (t.dataOffset + t.dataSize > size_t(bEnd - b) || t.paletteOffset + t.paletteSize > size_t(bEnd - b))
                continue;
            t.pixels = b + t.dataOffset;
            t.palette = b + t.paletteOffset;
            m_textures.push_back(std::move(t));
        }
        return true;
    }

    // Compressed pack (e.g. LANGUAGES\LANGUAGETEXTURES.BIN): 24-byte entries
    //   u32 name hash, u32 offset from the TPK chunk, u32 compressed size, u32 decompressed size, ...
    // Each entry is a HUFF/JDLZ block that expands to: pixels, palette, a 124-byte info record and
    // 84 bytes of GS settings. The record's offsets refer to the uncompressed pack, so pixels start
    // at 0 and the palette at (paletteOffset - dataOffset).
    const uint8_t *tpkBase = reinterpret_cast<const uint8_t *>(tpk);
    for (size_t off = 0; off + 24 <= compressed->size; off += 24) {
        const uint8_t *e = compressed->Data() + off;
        uint32_t offset = U32(e + 4), csize = U32(e + 8), dsize = U32(e + 12);
        if (offset + csize > size_t(end - tpkBase) || dsize < kInfoStride + 84 || !bIsCompressed(tpkBase + offset))
            continue;
        auto buf = std::make_unique<std::vector<uint8_t>>(dsize);
        if (bDecompress(tpkBase + offset, buf->data()) != dsize)
            continue;
        TextureInfo t = ReadInfoRecord(buf->data() + dsize - 84 - kInfoStride);
        uint32_t paletteAt = t.paletteOffset - t.dataOffset;
        if (t.dataSize > dsize || paletteAt + t.paletteSize > dsize)
            continue;
        t.pixels = buf->data();
        t.palette = buf->data() + paletteAt;
        m_owned.push_back(std::move(buf));
        m_textures.push_back(std::move(t));
    }
    return true;
}

const TextureInfo *TexturePack::Find(const char *name) const
{
    for (const TextureInfo &t : m_textures)
        if (t.name == name)
            return &t;
    return nullptr;
}

bool TexturePack::Decode(const TextureInfo &t, DecodedTexture &out) const
{
    if (t.bitsPerPixel != 8 && t.bitsPerPixel != 4)
        return false;
    const uint8_t *pixels = t.pixels;
    // 4-bit textures: expand each byte to two 4-bit indices (low nibble first), then they use the
    // same block swizzle as 8-bit data. Found by scoring layouts against the font glyph table.
    std::vector<uint8_t> expanded;
    if (t.bitsPerPixel == 4) {
        expanded.resize(size_t(t.width) * t.height);
        for (size_t i = 0; i < expanded.size() / 2 && i < t.dataSize; ++i) {
            expanded[2 * i] = pixels[i] & 0x0f;
            expanded[2 * i + 1] = pixels[i] >> 4;
        }
        pixels = expanded.data();
    }
    const uint8_t *palette = t.palette;
    const int colors = t.paletteSize / 4;

    out.width = t.width;
    out.height = t.height;
    out.rgba.assign(size_t(t.width) * t.height, 0);
    for (int y = 0; y < t.height; ++y) {
        for (int x = 0; x < t.width; ++x) {
            int index = pixels[Swizzled8Index(x, y, t.width)];
            int c = colors == 256 ? ClutIndex(index) : index;
            out.rgba[size_t(y) * t.width + x] = Ps2ColorToRgba(palette + 4 * c);
        }
    }
    return true;
}
