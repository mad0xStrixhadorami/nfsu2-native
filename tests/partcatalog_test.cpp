// PartCatalog parser test (ISSUES #184): a synthetic GLOBALB part pack, so the shop catalogue can
// be checked without the disc.  The real pack is chunk 0x80034602:
//   0x34604 14-byte part records: +4 part id, +5 group(5)|level(3), +7 type index,
//           +8 name offset (u16 units, x4 bytes), +10 attribute table offset (u16 units)
//   0x34605 8-byte {attribute-name hash, value} records
//   0x34606 strings, each at 4 * u16 offset
//   0x3460b car-type hashes (SPOILER)
//   0x3460c per-part attribute tables: i16 count + i16 attribute offsets
#include <cstdio>
#include <cstring>
#include <vector>

#include "Speed/Indep/bWare/Inc/Strings.hpp"
#include "game/indep/Frontend/PartCatalog.h"

namespace {

void Put16(std::vector<uint8_t> &b, uint16_t v)
{
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
}
void Put32(std::vector<uint8_t> &b, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back(uint8_t(v >> (i * 8)));
}
void Set16(std::vector<uint8_t> &b, size_t off, uint16_t v)
{
    b[off] = uint8_t(v);
    b[off + 1] = uint8_t(v >> 8);
}
void Set32(std::vector<uint8_t> &b, size_t off, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b[off + i] = uint8_t(v >> (i * 8));
}
void Chunk(std::vector<uint8_t> &out, uint32_t id, const std::vector<uint8_t> &payload)
{
    Put32(out, id);
    Put32(out, uint32_t(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
}
// Append a NUL-terminated name and pad to a 4-byte boundary (name offsets are u16 x4).
void PutName(std::vector<uint8_t> &strings, const char *name)
{
    strings.insert(strings.end(), name, name + strlen(name) + 1);
    while (strings.size() % 4)
        strings.push_back(0);
}

// Four SPOILER records: two painted styles, the first style's carbon variant, and one level-4
// (unpriced) style.
std::vector<uint8_t> MakePack()
{
    std::vector<uint8_t> header(0x40, 0);
    Set32(header, 0x28, 1); // numTypes
    Set32(header, 0x38, 4); // numParts

    std::vector<uint8_t> types;
    Put32(types, bStringHash("SPOILER"));

    std::vector<uint8_t> strings;
    const uint16_t nameJorgen = 0;
    PutName(strings, "JORGEN");
    const uint16_t nameMystic = uint16_t(strings.size() / 4);
    PutName(strings, "MYSTIC");
    const uint16_t nameUnique = uint16_t(strings.size() / 4);
    PutName(strings, "UNIQUE");
    const uint16_t nameCarbon = uint16_t(strings.size() / 4);
    PutName(strings, "JORGEN_CF");

    std::vector<uint8_t> attrs;
    Put32(attrs, bStringHash("CARBONFIBRE"));
    Put32(attrs, 1);

    auto part = [&](uint8_t level, uint16_t nameOff, uint16_t attrTable) {
        std::vector<uint8_t> p(14, 0);
        p[4] = 12;                        // spoiler shop id
        p[5] = uint8_t((level & 7) << 5); // group 0, level
        p[7] = 0;                         // SPOILER type index
        Set16(p, 8, nameOff);
        Set16(p, 10, attrTable);
        return p;
    };
    std::vector<uint8_t> parts;
    for (const std::vector<uint8_t> &p : {part(1, nameJorgen, 0xffff), part(2, nameMystic, 0xffff),
                                          part(1, nameCarbon, 1), part(4, nameUnique, 0xffff)})
        parts.insert(parts.end(), p.begin(), p.end());

    // Table 0 unused; table 1 = one attribute record (CARBONFIBRE).
    std::vector<uint8_t> tables(2, 0);
    Put16(tables, 1);
    Put16(tables, 0);

    std::vector<uint8_t> payload;
    Chunk(payload, 0x34603, header);
    Chunk(payload, 0x34604, parts);
    Chunk(payload, 0x34605, attrs);
    Chunk(payload, 0x34606, strings);
    Chunk(payload, 0x3460b, types);
    Chunk(payload, 0x3460c, tables);

    std::vector<uint8_t> pack;
    Chunk(pack, 0x80034602, payload);
    return pack;
}

// Price bundle with category 0x0c (painted spoiler) at 400/800/1000 and 0x4a (carbon spoiler) at
// 1000/1250/1500 - the two categories 0x00270888 picks for part id 12.
std::vector<uint8_t> MakePrices()
{
    std::vector<uint8_t> table(0x52 * 40, 0);
    auto block = [&](uint32_t category, size_t at, uint16_t req, uint16_t price) {
        const size_t base = size_t(category - 1) * 40;
        Set16(table, base + at, req);
        Set16(table, base + at + 2, price);
    };
    block(0xc, 4, 75, 400);
    block(0xc, 0x10, 150, 800);
    block(0xc, 0x1c, 200, 1000);
    block(0x4a, 4, 100, 1000);
    block(0x4a, 0x10, 200, 1250);
    block(0x4a, 0x1c, 300, 1500);

    std::vector<uint8_t> payload;
    Chunk(payload, 0x34a1c, table);
    std::vector<uint8_t> bundle;
    Chunk(bundle, 0x80034a10, payload);
    return bundle;
}

int Fail(const char *what)
{
    std::fprintf(stderr, "partcatalog_test: %s\n", what);
    return 1;
}

} // namespace

int main()
{
    const std::vector<uint8_t> pack = MakePack();
    const uint32_t spoiler = bStringHash("SPOILER");

    PartCatalog cat;
    if (!cat.Parse(pack.data(), pack.size()))
        return Fail("Parse rejected the synthetic pack");
    if (cat.Count(spoiler) != 4)
        return Fail("spoiler record count");
    if (cat.Count(bStringHash("ROOF")) != 0)
        return Fail("absent type should be empty");

    const std::vector<CarPart> &parts = cat.Parts(spoiler);
    if (parts[0].name != "JORGEN" || parts[0].partId != 12 || parts[0].level != 1 ||
        parts[0].carbon || parts[0].style != 1)
        return Fail("first painted style");
    if (parts[1].name != "MYSTIC" || parts[1].level != 2 || parts[1].style != 2)
        return Fail("second painted style");
    if (parts[2].name != "JORGEN_CF" || !parts[2].carbon || parts[2].style != 1)
        return Fail("carbon variant keeps its style number");
    if (parts[3].name != "UNIQUE" || parts[3].level != 4 || parts[3].style != 3)
        return Fail("level-4 style");

    // The payload form (no 0x80034602 wrapper) must parse too.
    PartCatalog payloadCat;
    if (!payloadCat.Parse(pack.data() + 8, pack.size() - 8) || payloadCat.Count(spoiler) != 4)
        return Fail("payload form");

    if (PartCatalog::PriceCategory(0xc) != 0xc)
        return Fail("spoiler price category");
    if (PartCatalog::PriceCategory(0x45) != 0x50)
        return Fail("paint-shop spoiler price category");
    if (PartCatalog::PriceCategory(0xaa) != 0)
        return Fail("out-of-table part id");
    if (PartCatalog::PriceCategory(parts[0]) != 0xc || PartCatalog::PriceCategory(parts[2]) != 0x4a)
        return Fail("carbon attribute switches the spoiler price category");

    PartCatalog priced;
    const std::vector<uint8_t> prices = MakePrices();
    if (!priced.ParsePrices(prices.data(), prices.size()) || priced.PriceEntryCount() != 0x52)
        return Fail("price table");
    if (priced.Price(parts[0]) != 400 || priced.Price(parts[1]) != 800)
        return Fail("painted spoiler prices per tier");
    if (priced.Price(parts[2]) != 1000)
        return Fail("carbon spoiler price");
    if (priced.Price(parts[3]) != 0)
        return Fail("no price for tier 4");

    std::printf("partcatalog_test: ok (%zu spoilers, styles 1..%u)\n", parts.size(),
                unsigned(parts.back().style));
    return 0;
}
