#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// UG2 front-end fonts (chunk 0x30201, BCHUNK_FENG_FONT) and language strings (chunk 0x39000).
// Font chunk: +0x000 font name, +0x100 texture name, +0x200 EA RealFont data ("FNTS").
// Layout of RealFont matches nfsmw's RealFontOld::Font; the glyph lookup and drawing that nfsmw
// leaves unsolved are implemented here.

struct FontGlyph {     // RealFontOld::Glyph, 16 bytes when font flags have 0x40000
    uint16_t unicode;
    uint8_t width, height;
    uint16_t u, v;
    int8_t advanceY, offsetX, offsetY;
    uint8_t numKern;
    uint16_t kernIndex;
    int16_t advanceX;
};

class FEFont {
public:
    bool Parse(const uint8_t *chunkData, size_t size);
    const FontGlyph *Glyph(uint16_t unicode) const;
    int Kern(const FontGlyph *glyph, uint16_t prev) const;
    static uint16_t ConvertCharacter(uint16_t c); // 0x0028f720 / FEngFont::ConvertCharacter
    float Height() const { return m_height; }
    uint32_t TextureHash() const { return m_textureHash; }
    const std::string &Name() const { return m_name; }

private:
    std::string m_name;
    uint32_t m_textureHash = 0;
    float m_height = 0;
    std::vector<FontGlyph> m_glyphs;                  // sorted by unicode in the file
    const uint8_t *m_kern = nullptr;                  // kerning table (see Kern)
};

class FELanguage {
public:
    bool Parse(const uint8_t *chunkData, size_t size); // chunk 0x39000 payload (0x00263130)
    const char *Find(uint32_t labelHash) const;        // labelHash = bStringHash(upper(label))
    // 0x00296ec8: decode an 8-bit/multibyte language string into UTF-16 codepoints using the
    // conversion table at chunkData + Read<u32>(0) + 4.
    void DecodeString(const char *s8, std::vector<uint16_t> &out) const;
    size_t Count() const { return m_entries.size(); }

private:
    std::vector<std::pair<uint32_t, uint32_t>> m_entries; // (hash, offset) sorted by hash
    const char *m_strings = nullptr;
    const uint16_t *m_convTable = nullptr;                // u16 entries at convOffset + 4
    size_t m_convCount = 0;
};

// Hash key used for both texture and font resources: bStringHash of the upper-case file name
// without directory and extension ("ConduitMdITC_TT14i.ffn" -> "CONDUITMDITC_TT14I").
uint32_t FEResourceHash(const char *filename);
