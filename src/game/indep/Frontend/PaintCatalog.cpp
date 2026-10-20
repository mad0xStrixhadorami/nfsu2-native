#include "game/indep/Frontend/PaintCatalog.h"

#include <algorithm>
#include <cstring>

#include "Speed/Indep/bWare/Inc/Strings.hpp"

namespace {

constexpr uint32_t kCarPartPack = 0x80034602; // GLOBALB: the repacked part database
constexpr uint32_t kPackHeader = 0x34603;     // version + record counts
constexpr uint32_t kPackParts = 0x34604;      // 14-byte part records
constexpr uint32_t kPackAttrs = 0x34605;      // 8-byte {name hash, value} attribute records
constexpr uint32_t kPackStrings = 0x34606;    // names, each at 4 * u16 offset
constexpr uint32_t kPackTypes = 0x3460b;      // car type hashes
constexpr uint32_t kPackTables = 0x3460c;     // per-part attribute tables (i16 count + i16 offsets)

constexpr uint32_t kColorSetAttr = 0xebb03e66; // the color-set key FUN_002bedd8 returns
constexpr uint32_t kGlossSet = 0x2daab07;      // Base Paint (part id 0x3f) -> the 60 GLOSS colors
constexpr int kPaintPartId = 48;               // PAINT type: 48 = paint/hoses/mufflers, 49 = vinyls
constexpr size_t kPartRecord = 14;

uint32_t ReadU32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
uint16_t ReadU16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

struct Chunk {
    uint32_t id = 0;
    const uint8_t *data = nullptr;
    size_t size = 0;
};

// Iterate the chunked format shared by bundles and the part pack (u32 id, u32 size, payload).
template <typename Fn> void ForEachChunk(const uint8_t *data, size_t size, Fn fn)
{
    for (size_t off = 0; off + 8 <= size;) {
        Chunk c;
        c.id = ReadU32(data + off);
        c.size = ReadU32(data + off + 4);
        c.data = data + off + 8;
        if (off + 8 + c.size > size)
            break;
        if (!fn(c))
            return;
        off += 8 + c.size;
    }
}

} // namespace

bool PaintCatalog::Parse(const uint8_t *bundle, size_t size)
{
    m_colors.clear();
    if (!bundle || size < 8)
        return false;

    const uint8_t *pack = bundle;
    size_t packSize = size;
    // AddBundle hands us either the whole bundle or the 0x80034602 payload directly.
    if (size >= 8 && ReadU32(bundle) == kCarPartPack) {
        pack = bundle + 8;
        packSize = ReadU32(bundle + 4);
        if (packSize > size - 8)
            return false;
    }

    const uint8_t *header = nullptr, *parts = nullptr, *attrs = nullptr;
    const uint8_t *strs = nullptr, *types = nullptr, *tables = nullptr;
    size_t headerSize = 0, partsSize = 0, attrsSize = 0, strsSize = 0, typesSize = 0, tablesSize = 0;
    ForEachChunk(pack, packSize, [&](const Chunk &c) {
        switch (c.id) {
        case kPackHeader: header = c.data; headerSize = c.size; break;
        case kPackParts: parts = c.data; partsSize = c.size; break;
        case kPackAttrs: attrs = c.data; attrsSize = c.size; break;
        case kPackStrings: strs = c.data; strsSize = c.size; break;
        case kPackTypes: types = c.data; typesSize = c.size; break;
        case kPackTables: tables = c.data; tablesSize = c.size; break;
        default: break;
        }
        return true;
    });
    if (!parts || !attrs || !strs || !types || !tables)
        return false;

    // Car type index of "PAINT" (the folder-style type hash, like the car database records).
    uint32_t numTypes = 0;
    if (header && headerSize >= 0x2c)
        numTypes = ReadU32(header + 0x28);
    if (!numTypes)
        numTypes = uint32_t(typesSize / 4);
    const uint32_t paintType = bStringHash("PAINT");
    int paintTypeIndex = -1;
    for (uint32_t i = 0; i < numTypes && (i + 1) * 4 <= typesSize; ++i) {
        if (ReadU32(types + i * 4) == paintType) {
            paintTypeIndex = int(i);
            break;
        }
    }
    if (paintTypeIndex < 0)
        return false;

    const uint32_t attrRed = bStringHash("RED");
    const uint32_t attrGreen = bStringHash("GREEN");
    const uint32_t attrBlue = bStringHash("BLUE");
    const uint32_t attrGloss = bStringHash("GLOSS");

    for (size_t off = 0; off + kPartRecord <= partsSize; off += kPartRecord) {
        const uint8_t *r = parts + off;
        const int partId = int(int8_t(r[4]));
        const uint8_t typeIndex = r[7];
        const uint16_t nameOff = ReadU16(r + 8);
        const uint16_t attrTable = ReadU16(r + 10);
        if (partId != kPaintPartId || typeIndex != uint8_t(paintTypeIndex))
            continue;

        uint32_t set = 0;
        int red = -1, green = -1, blue = -1, gloss = -1;
        if (attrTable != 0xffff && size_t(attrTable) * 2 + 2 <= tablesSize) {
            const int16_t n = int16_t(ReadU16(tables + attrTable * 2));
            if (n > 0 && n < 64 && size_t(attrTable) * 2 + 2 + size_t(n) * 2 <= tablesSize) {
                for (int k = 0; k < n; ++k) {
                    const int16_t ao = int16_t(ReadU16(tables + attrTable * 2 + 2 + k * 2));
                    if (ao < 0 || size_t(ao) * 8 + 8 > attrsSize)
                        continue;
                    const uint32_t nameHash = ReadU32(attrs + size_t(ao) * 8);
                    const uint32_t value = ReadU32(attrs + size_t(ao) * 8 + 4);
                    if (nameHash == kColorSetAttr)
                        set = value;
                    else if (nameHash == attrRed)
                        red = int(value);
                    else if (nameHash == attrGreen)
                        green = int(value);
                    else if (nameHash == attrBlue)
                        blue = int(value);
                    else if (nameHash == attrGloss)
                        gloss = int(value);
                }
            }
        }
        if (set != kGlossSet)
            continue;

        PaintColor color;
        if (size_t(nameOff) * 4 < strsSize) {
            const char *s = reinterpret_cast<const char *>(strs + size_t(nameOff) * 4);
            color.name.assign(s, strnlen(s, strsSize - size_t(nameOff) * 4));
        }
        auto clamp8 = [](int v) { return uint8_t(std::clamp(v, 0, 255)); };
        color.r = clamp8(red);
        color.g = clamp8(green);
        color.b = clamp8(blue);
        color.gloss = clamp8(gloss);
        m_colors.push_back(std::move(color));
    }

    // The records already come in UI order (L1, L2, L3); keep that rather than re-sorting, so the
    // catalog order matches the swatch grid index-for-index.
    return !m_colors.empty();
}