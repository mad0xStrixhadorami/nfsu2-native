#include "game/indep/Frontend/FEText.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "Speed/Indep/bWare/Inc/Strings.hpp"

namespace {
template <typename T> T Read(const uint8_t *p)
{
    T v;
    memcpy(&v, p, sizeof(v));
    return v;
}
} // namespace

uint32_t FEResourceHash(const char *filename)
{
    const char *base = filename;
    for (const char *p = filename; *p; ++p)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    char upper[128];
    size_t n = 0;
    for (; base[n] && base[n] != '.' && n < sizeof(upper) - 1; ++n)
        upper[n] = static_cast<char>(toupper(static_cast<unsigned char>(base[n])));
    upper[n] = '\0';
    return bStringHash(upper);
}

bool FEFont::Parse(const uint8_t *chunk, size_t size)
{
    if (size < 0x200 + 0x80 || memcmp(chunk + 0x200, "FNTS", 4) != 0)
        return false;
    m_name.assign(reinterpret_cast<const char *>(chunk), strnlen(reinterpret_cast<const char *>(chunk), 0x100));
    m_textureHash = FEResourceHash(reinterpret_cast<const char *>(chunk + 0x100));

    const uint8_t *font = chunk + 0x200;
    const uint16_t count = Read<uint16_t>(font + 0xa);
    const uint32_t flags = Read<uint32_t>(font + 0xc);
    const int32_t glyphTable = Read<int32_t>(font + 0x14);
    const int32_t kernTable = Read<int32_t>(font + 0x18);
    const int32_t heightState = Read<int32_t>(font + 0x20 + 4 * 1); // mStates[FONT_HEIGHT]
    m_height = static_cast<float>(heightState);
    const int stride = (flags & 0x40000) ? 0x10 : 0x0c;
    if (0x200 + glyphTable + size_t(count) * stride > size)
        return false;

    m_glyphs.resize(count);
    for (int i = 0; i < count; ++i) {
        const uint8_t *g = font + glyphTable + i * stride;
        FontGlyph &o = m_glyphs[i];
        o.unicode = Read<uint16_t>(g);
        o.width = g[2];
        o.height = g[3];
        o.u = Read<uint16_t>(g + 4);
        o.v = Read<uint16_t>(g + 6);
        o.advanceY = static_cast<int8_t>(g[8]);
        o.offsetX = static_cast<int8_t>(g[9]);
        o.offsetY = static_cast<int8_t>(g[10]);
        o.numKern = g[11];
        o.kernIndex = stride == 0x10 ? Read<uint16_t>(g + 12) : 0;
        o.advanceX = stride == 0x10 ? Read<int16_t>(g + 14) : o.width;
    }
    m_kern = kernTable > 0 ? font + kernTable : nullptr;
    return true;
}

uint16_t FEFont::ConvertCharacter(uint16_t c)
{
    // 0x0028f720 (and nfsmw FEngFont::ConvertCharacter):
    if (c >= 0xff80)
        c &= 0xff;
    if (c == 0x99)
        return 0x2122; // ™
    if (c == 0x9c)
        return 0x0153; // œ
    if (c == 0xa0)
        return 0x0020; // NBSP -> space
    return c;
}

const FontGlyph *FEFont::Glyph(uint16_t unicode) const
{
    unicode = ConvertCharacter(unicode);
    auto it = std::lower_bound(m_glyphs.begin(), m_glyphs.end(), unicode,
                               [](const FontGlyph &g, uint16_t u) { return g.unicode < u; });
    return it != m_glyphs.end() && it->unicode == unicode ? &*it : nullptr;
}

int FEFont::Kern(const FontGlyph *glyph, uint16_t prev) const
{
    if (!m_kern || !glyph->numKern)
        return 0;
    prev = ConvertCharacter(prev);
    // Kerning table: u32 count, then 4-byte entries {u16 previous char, s8 amount, u8 glyph char}.
    for (int i = 0; i < glyph->numKern; ++i) {
        const uint8_t *k = m_kern + 4 + (glyph->kernIndex + i) * 4;
        if (Read<uint16_t>(k) == prev)
            return static_cast<int8_t>(k[2]);
    }
    return 0;
}

// Header (0x00263130): u32 convTableOffset (0x10), u32 count, u32 table offset, u32 string data offset.
bool FELanguage::Parse(const uint8_t *chunk, size_t size)
{
    if (size < 16)
        return false;
    const uint32_t convOffset = Read<uint32_t>(chunk + 0);
    const uint32_t count = Read<uint32_t>(chunk + 4);
    const uint32_t table = Read<uint32_t>(chunk + 8);
    const uint32_t strings = Read<uint32_t>(chunk + 12);
    if (table + count * 8ull > size || strings > size)
        return false;
    if (convOffset + 4 <= size && convOffset + 4 <= table) {
        m_convTable = reinterpret_cast<const uint16_t *>(chunk + convOffset + 4);
        m_convCount = (table - (convOffset + 4)) / sizeof(uint16_t);
    }
    m_entries.resize(count);
    for (uint32_t i = 0; i < count; ++i)
        m_entries[i] = {Read<uint32_t>(chunk + table + i * 8), Read<uint32_t>(chunk + table + i * 8 + 4)};
    m_strings = reinterpret_cast<const char *>(chunk + strings);
    return true;
}

const char *FELanguage::Find(uint32_t hash) const
{
    auto it = std::lower_bound(m_entries.begin(), m_entries.end(), hash,
                               [](const std::pair<uint32_t, uint32_t> &e, uint32_t h) { return e.first < h; });
    return it != m_entries.end() && it->first == hash ? m_strings + it->second : nullptr;
}

void FELanguage::DecodeString(const char *s8, std::vector<uint16_t> &out) const
{
    // Decompiled from 0x00296ec8 (called by 0x00262eb8):
    if (!s8)
        return;
    const auto *bytes = reinterpret_cast<const uint8_t *>(s8);
    for (size_t i = 0; bytes[i] != 0;) {
        uint8_t b0 = bytes[i++];
        uint16_t ch = b0;
        if (b0 >= 0x80 && m_convTable && b0 < m_convCount) {
            uint16_t mapped = m_convTable[b0];
            ch = mapped;
            if (mapped != 0 && mapped < 0x80) {
                uint8_t b1 = bytes[i];
                if (b1 != 0)
                    ++i;
                size_t idx = size_t(mapped) * 0x80 + b1 - 0x80;
                ch = (b1 >= 0x80 && idx < m_convCount) ? m_convTable[idx] : 0;
            }
            if (ch == 0)
                ch = 0x5f; // '_'
        }
        out.push_back(ch);
    }
}
