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
constexpr uint32_t kPackTables = 0x3460c;     // per-part attribute tables (i16 count + i16 offsets)
constexpr uint32_t kPriceRoot = 0x80034a10;   // GLOBALB: car-shop price bundle
constexpr uint32_t kPriceTable = 0x34a1c;     // 40-byte price entries (0x0026fdb0)
constexpr size_t kPriceEntry = 40;

constexpr uint32_t kColorSetAttr = 0xebb03e66; // the color-set key FUN_002bedd8 returns
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

const PaintColor kNoColor{};

} // namespace

// 0x002bedd8: the color set each paint-shop part id edits.
uint32_t PaintCatalog::SetForPart(uint32_t partId)
{
    switch (partId) {
    case 0x3f: // Base Paint
    case 0x45: // Spoiler
    case 0x4b: // Roof Scoop
    case 0x4c: // Mirrors
        return kGloss;
    case 0x44: // Engine Accents
        return kHoses;
    case 0x46: // Brakes
        return kBrakes;
    case 0x47: // Muffler Tip
        return kMufflers;
    case 0x49: // Rims
    case 0x4a: // Spinners
        return kRims;
    default:
        return 0;
    }
}

bool PaintCatalog::IsPaintSet(uint32_t set)
{
    switch (set) {
    case kGloss:
    case kMetal:
    case kPearl:
    case kHoses:
    case kMufflers:
    case kVinyl:
    case kRims:
    case kBrakes:
        return true;
    default:
        return false;
    }
}

const char *PaintCatalog::SetName(uint32_t set)
{
    // 0x002bcd00 labels the paint screen with bStringHash("_PAINT", set) (0x00156db8), and
    // ENGLISH.BIN resolves those hashes to the finish names below:
    //   0x632d35e2 Gloss, 0xf605116d Metallic, 0x913df10e Pearlescent,
    //   0xcc3401e9 Powder Coat (hoses and rims), 0x459ee68b High Heat (mufflers and brakes).
    switch (set) {
    case kGloss: return "Gloss";
    case kMetal: return "Metallic";
    case kPearl: return "Pearlescent";
    case kHoses: return "Powder Coat";
    case kMufflers: return "High Heat";
    case kVinyl: return "Vinyl";
    case kRims: return "Powder Coat";
    case kBrakes: return "High Heat";
    default: return "Color";
    }
}

bool PaintCatalog::Parse(const uint8_t *bundle, size_t size)
{
    m_sets.clear();
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
    const uint8_t *strs = nullptr, *tables = nullptr;
    size_t headerSize = 0, partsSize = 0, attrsSize = 0, strsSize = 0, tablesSize = 0;
    ForEachChunk(pack, packSize, [&](const Chunk &c) {
        switch (c.id) {
        case kPackHeader: header = c.data; headerSize = c.size; break;
        case kPackParts: parts = c.data; partsSize = c.size; break;
        case kPackAttrs: attrs = c.data; attrsSize = c.size; break;
        case kPackStrings: strs = c.data; strsSize = c.size; break;
        case kPackTables: tables = c.data; tablesSize = c.size; break;
        default: break;
        }
        return true;
    });
    (void)header;
    (void)headerSize;
    if (!parts || !attrs || !strs || !tables)
        return false;

    const uint32_t attrRed = bStringHash("RED");
    const uint32_t attrGreen = bStringHash("GREEN");
    const uint32_t attrBlue = bStringHash("BLUE");
    const uint32_t attrGloss = bStringHash("GLOSS");

    // Color records live in several car-part types (PAINT for body/hoses/mufflers, PAINT_RIM,
    // PAINT_BRAKE, ...); the 0xebb03e66 value, not the type, identifies the set.  The records are
    // already in UI order (L1, L2, L3), so keep that rather than re-sorting: the catalog order
    // matches the swatch grid index-for-index.
    for (size_t off = 0; off + kPartRecord <= partsSize; off += kPartRecord) {
        const uint8_t *r = parts + off;
        const uint8_t gl = r[5]; // group (low 5 bits) | level (high 3 bits)
        const uint16_t nameOff = ReadU16(r + 8);
        const uint16_t attrTable = ReadU16(r + 10);

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
        if (!IsPaintSet(set))
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
        // The record's group(5)|level(3) byte selects a price tier.  Real data uses 1 for GLOSS,
        // 2 for METAL, 3 for PEARL, and 1..3 for the other paint sets.
        color.level = uint8_t((gl & 0xff) >> 5);
        m_sets[set].push_back(std::move(color));
    }

    return !m_sets.empty();
}

// 0x0026fdb0: GLOBALB's 0x34a1c subchunk is an array of 40-byte price entries; entry 0 is the
// id-1 entry, and the L1/L2/L3 blocks sit at +4, +0x10 and +0x1c (each {i16 requirement, u16 price}).
bool PaintCatalog::ParsePrices(const uint8_t *bundle, size_t size)
{
    m_priceTable.clear();
    if (!bundle || size < 8)
        return false;

    const uint8_t *pack = bundle;
    size_t packSize = size;
    if (ReadU32(bundle) == kPriceRoot) {
        pack = bundle + 8;
        packSize = ReadU32(bundle + 4);
        if (packSize > size - 8)
            return false;
    }

    const uint8_t *table = nullptr;
    size_t tableSize = 0;
    ForEachChunk(pack, packSize, [&](const Chunk &c) {
        if (c.id == kPriceTable) {
            table = c.data;
            tableSize = c.size;
            return false;
        }
        return true;
    });
    if (!table || tableSize < kPriceEntry)
        return false;
    m_priceTable.assign(table, table + tableSize);
    return true;
}

// 0x00270888: shop part id -> price category.  The 0x3f (Base Paint) case depends on the colour
// set; the other paint-shop cards use the fixed 0x002704d8 table (DAT_00516840).
uint32_t PaintCatalog::PriceCategory(uint32_t shopPartId, uint32_t set)
{
    switch (shopPartId) {
    case 0x3f:
        switch (set) {
        case kGloss: return 0x25;
        case kMetal: return 0x26;
        case kPearl: return 0x27;
        case kHoses: return 0x28;
        case kMufflers: return 0x4e;
        case kRims: return 0x4d;
        default: return 0;
        }
    case 0x44: return 0x28; // Engine Accents (hoses)
    case 0x45: return 0x50; // Spoiler
    case 0x46: return 0x4f; // Brakes
    case 0x47: return 0x4e; // Muffler Tip
    case 0x49: return 0x4d; // Rims
    case 0x4a: return 0x51; // Spinners
    case 0x4b: return 0x53; // Roof Scoop
    case 0x4c: return 0x54; // Mirrors
    default: return 0;
    }
}

// 0x0026b0c8: FUN_00270888 picks the category, then FUN_00270228 picks the L1/L2/L3 block.
uint16_t PaintCatalog::Price(uint32_t shopPartId, uint32_t set, const PaintColor &color) const
{
    const uint32_t category = PriceCategory(shopPartId, set);
    uint32_t level = color.level;
    if (level == 0) {
        if (category != 0x4c) // 0x00270228's one level-0 exception
            return 0;
        level = 1;
    }
    if (category == 0 || level > 3 || m_priceTable.size() < size_t(category) * kPriceEntry)
        return 0;
    const size_t block = (level == 1) ? 4 : (level == 2) ? 0x10 : 0x1c;
    return ReadU16(m_priceTable.data() + (category - 1) * kPriceEntry + block + 2);
}

bool PaintCatalog::Has(uint32_t set) const
{
    auto it = m_sets.find(set);
    return it != m_sets.end() && !it->second.empty();
}

size_t PaintCatalog::Count(uint32_t set) const
{
    auto it = m_sets.find(set);
    return it == m_sets.end() ? 0 : it->second.size();
}

const PaintColor &PaintCatalog::At(uint32_t set, size_t index) const
{
    auto it = m_sets.find(set);
    if (it == m_sets.end() || index >= it->second.size())
        return kNoColor;
    return it->second[index];
}

const std::vector<PaintColor> &PaintCatalog::Colors(uint32_t set) const
{
    static const std::vector<PaintColor> kEmpty;
    auto it = m_sets.find(set);
    return it == m_sets.end() ? kEmpty : it->second;
}
