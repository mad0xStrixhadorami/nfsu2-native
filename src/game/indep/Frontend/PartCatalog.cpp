#include "game/indep/Frontend/PartCatalog.h"

#include <cstring>
#include <tuple>
#include <utility>

#include "Speed/Indep/bWare/Inc/Strings.hpp"

namespace {

constexpr uint32_t kCarPartPack = 0x80034602; // GLOBALB: the repacked part database
constexpr uint32_t kPackHeader = 0x34603;     // version + record counts
constexpr uint32_t kPackParts = 0x34604;      // 14-byte part records
constexpr uint32_t kPackAttrs = 0x34605;      // 8-byte {name hash, value} attribute records
constexpr uint32_t kPackStrings = 0x34606;    // names, each at 4 * u16 offset
constexpr uint32_t kPackTypes = 0x3460b;      // car type hashes
constexpr uint32_t kPackTables = 0x3460c;     // per-part attribute tables (i16 count + i16 offsets)
constexpr uint32_t kPriceRoot = 0x80034a10;   // GLOBALB: car-shop price bundle
constexpr uint32_t kPriceTable = 0x34a1c;     // 40-byte price entries (0x0026fdb0)
constexpr size_t kPriceEntry = 40;
constexpr size_t kPartRecord = 14;
constexpr uint32_t kCarbonAttr = 0x721aff7c; // bStringHash("CARBONFIBRE")

// 0x00270888's group -> category switch for the part ids 0x3f..0x43 (FUN_00270228's tiers index
// the same price table).
constexpr uint8_t kGroupCategory[0x1c] = {0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32,
                                          0x33, 0x34, 0x35, 0x36, 0x38, 0x39, 0x3a, 0x3b, 0x41, 0x42,
                                          0x43, 0x3c, 0x3d, 0x40, 0x3e, 0x37, 0x3f, 0x44};

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

// Walk one pack level (id, size, payload) until `fn` returns false.
template <typename Fn> void ForEachChunk(const uint8_t *pack, size_t size, Fn fn)
{
    size_t off = 0;
    while (off + 8 <= size) {
        Chunk c;
        c.id = ReadU32(pack + off);
        c.size = ReadU32(pack + off + 4);
        c.data = pack + off + 8;
        if (c.size > size - off - 8)
            return;
        if (!fn(c))
            return;
        off += 8 + c.size;
    }
}

// 0x002704d8's initializer for DAT_00516840, 0xaa entries of one byte each.  Entry 0 is the
// "no price category" id 0; the assigned values are the table ids 0x01..0x56 above them.
constexpr uint8_t kPriceCategory[0xaa] = {
    /* 0x00 */ 0x00, 0x12, 0x11, 0x13, 0x13, 0x00, 0x16, 0x14, 0x00, 0x0b, 0x00, 0x10, 0x0c, 0x00, 0x0f, 0x0e,
    /* 0x10 */ 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d, 0x0d, 0x21,
    /* 0x20 */ 0x13, 0x00, 0x1e, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x00,
    /* 0x30 */ 0x00, 0x00, 0x00, 0x00, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x40 */ 0x00, 0x00, 0x00, 0x00, 0x28, 0x50, 0x4f, 0x4e, 0x52, 0x4d, 0x51, 0x53, 0x54, 0x00, 0x00, 0x00,
    /* 0x50 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0x00, 0x00,
    /* 0x60 */ 0x56, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c,
    /* 0x70 */ 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c,
    /* 0x80 */ 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c,
    /* 0x90 */ 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x20, 0x19, 0x1a, 0x1b, 0x1c, 0x1b, 0x18,
    /* 0xa0 */ 0x23, 0x24, 0x1d, 0x17, 0x17, 0x17, 0x17, 0x00, 0x00, 0x00,
};

} // namespace

bool PartCatalog::Parse(const uint8_t *bundle, size_t size)
{
    m_parts.clear();
    if (!bundle || size < 8)
        return false;

    const uint8_t *pack = bundle;
    size_t packSize = size;
    // AddBundle hands us either the whole bundle or the 0x80034602 payload directly.
    if (ReadU32(bundle) == kCarPartPack) {
        pack = bundle + 8;
        packSize = ReadU32(bundle + 4);
        if (packSize > size - 8)
            return false;
    }

    const uint8_t *parts = nullptr, *attrs = nullptr, *strs = nullptr;
    const uint8_t *types = nullptr, *tables = nullptr;
    size_t partsSize = 0, attrsSize = 0, strsSize = 0, typesSize = 0, tablesSize = 0;
    ForEachChunk(pack, packSize, [&](const Chunk &c) {
        switch (c.id) {
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
    if (partsSize < kPartRecord || typesSize < 4 || attrsSize < 8 || tablesSize < 2)
        return false;

    const size_t numParts = partsSize / kPartRecord;
    const size_t numTypes = typesSize / 4;
    std::map<std::tuple<uint32_t, bool, uint8_t>, uint16_t> styles;
    for (size_t i = 0; i < numParts; ++i) {
        const uint8_t *r = parts + i * kPartRecord;
        const uint8_t gl = r[5];
        const uint8_t typeIndex = r[7];
        if (typeIndex >= numTypes)
            continue;
        CarPart part;
        part.partId = uint8_t(r[4]);
        part.group = uint8_t(gl & 0x1f);
        part.level = uint8_t(gl >> 5);
        part.carbon = false;

        const uint16_t nameOff = ReadU16(r + 8);
        if (!strsSize || size_t(nameOff) * 4 >= strsSize)
            continue;
        const char *name = reinterpret_cast<const char *>(strs + size_t(nameOff) * 4);
        part.name.assign(name, strnlen(name, strsSize - size_t(nameOff) * 4));
        // ROOF lists each scoop in three bodies: base, "NAME OFFSET" and "NAME DUAL", and the
        // universal file names them ROOF_STYLE<NN>[_DUAL][_OFFSET]_A.  The style number restarts
        // for each body, so strip the suffix before counting (ISSUES #186).
        if (part.name.size() > 7 && part.name.compare(part.name.size() - 7, 7, " OFFSET") == 0)
            part.variant = 1;
        else if (part.name.size() > 5 && part.name.compare(part.name.size() - 5, 5, " DUAL") == 0)
            part.variant = 2;

        const uint16_t attrTable = ReadU16(r + 10);
        if (attrTable != 0xffff && size_t(attrTable) * 2 + 2 <= tablesSize) {
            const int16_t count = int16_t(ReadU16(tables + size_t(attrTable) * 2));
            for (int a = 0; a < count; ++a) {
                const size_t at = size_t(attrTable) * 2 + 2 + size_t(a) * 2;
                if (at + 2 > tablesSize)
                    break;
                const uint16_t attrOffset = ReadU16(tables + at);
                if (size_t(attrOffset) * 8 + 8 > attrsSize)
                    continue;
                if (ReadU32(attrs + size_t(attrOffset) * 8) == kCarbonAttr &&
                    ReadU32(attrs + size_t(attrOffset) * 8 + 4) != 0)
                    part.carbon = true;
            }
        }

        const uint32_t typeHash = ReadU32(types + size_t(typeIndex) * 4);
        // Level 0 is the stock/no-part record ("NO ROOF SCOOP"); it is not a purchasable style and
        // must not shift the geometry numbering that follows it.
        part.style = part.level == 0 ? 0 : ++styles[{typeHash, part.carbon, part.variant}];
        m_parts[typeHash].push_back(std::move(part));
    }
    return !m_parts.empty();
}

bool PartCatalog::ParsePrices(const uint8_t *bundle, size_t size)
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

size_t PartCatalog::Count(uint32_t typeHash) const
{
    auto it = m_parts.find(typeHash);
    return it == m_parts.end() ? 0 : it->second.size();
}

const std::vector<CarPart> &PartCatalog::Parts(uint32_t typeHash) const
{
    static const std::vector<CarPart> kEmpty;
    auto it = m_parts.find(typeHash);
    return it == m_parts.end() ? kEmpty : it->second;
}

uint32_t PartCatalog::PriceCategory(uint32_t partId)
{
    return partId < sizeof(kPriceCategory) ? kPriceCategory[partId] : 0;
}

uint32_t PartCatalog::PriceCategory(const CarPart &part)
{
    if (part.partId >= 0x3f && part.partId <= 0x43)
        return part.group < sizeof(kGroupCategory) ? kGroupCategory[part.group] : 0;
    if (part.carbon) {
        switch (part.partId) {
        case 9: return 0x4b;  // hood
        case 7: return 0x46;  // roof scoop
        case 0xc: return 0x4a; // spoiler
        case 0x20: return 0x45; // body kit
        default: return 0;
        }
    }
    // 0x1d is the paint colour record (its 0xebb03e66 set picks the finish); the paint screen
    // prices those through PaintCatalog.  9 with group 5 is the stock hood.
    if (part.partId == 0x1d)
        return 0x21;
    if (part.partId == 9 && part.group == 5)
        return 0x22;
    return PriceCategory(part.partId);
}

uint16_t PartCatalog::Price(const CarPart &part) const
{
    const uint32_t category = PriceCategory(part);
    if (category == 0)
        return 0;
    // 0x00270228 returns 0 for level 4 (and for 0, apart from the mirror card's exception).
    uint8_t tier = part.level;
    if (tier == 0 || tier > 3)
        return 0;
    if (m_priceTable.size() < size_t(category) * kPriceEntry)
        return 0;
    const size_t block = (tier == 1) ? 4 : (tier == 2) ? 0x10 : 0x1c;
    return ReadU16(m_priceTable.data() + size_t(category - 1) * kPriceEntry + block + 2);
}
