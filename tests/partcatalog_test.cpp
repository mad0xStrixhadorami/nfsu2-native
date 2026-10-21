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

// Spoiler records: two painted styles, the first style's carbon variant and one level-4
// (unpriced) style.  ROOF adds the three bodies (base / OFFSET / DUAL) plus a carbon base, and
// EXHAUST two styles at different price tiers (ISSUES #186).
std::vector<uint8_t> MakePack()
{
    std::vector<uint8_t> header(0x40, 0);
    Set32(header, 0x28, 3);  // numTypes
    Set32(header, 0x38, 12); // numParts

    std::vector<uint8_t> types;
    Put32(types, bStringHash("SPOILER"));
    Put32(types, bStringHash("ROOF"));
    Put32(types, bStringHash("EXHAUST"));

    std::vector<uint8_t> strings;
    auto name = [&](const char *text) {
        const uint16_t at = uint16_t(strings.size() / 4);
        PutName(strings, text);
        return at;
    };
    const uint16_t nameJorgen = name("JORGEN");
    const uint16_t nameMystic = name("MYSTIC");
    const uint16_t nameUnique = name("UNIQUE");
    const uint16_t nameCarbon = name("JORGEN_CF");
    const uint16_t nameJetstream = name("JETSTREAM");
    const uint16_t nameNone = name("NO ROOF SCOOP");
    const uint16_t nameHeadwind = name("HEADWIND");
    const uint16_t nameOffset = name("JETSTREAM OFFSET");
    const uint16_t nameDual = name("JETSTREAM DUAL");
    const uint16_t nameRoofCarbon = name("JETSTREAM_CF");
    const uint16_t nameBacklash = name("BACKLASH");
    const uint16_t nameVortex = name("VORTEX");

    std::vector<uint8_t> attrs;
    Put32(attrs, bStringHash("CARBONFIBRE"));
    Put32(attrs, 1);

    auto part = [&](uint8_t id, uint8_t typeIndex, uint8_t level, uint16_t nameOff,
                    uint16_t attrTable) {
        std::vector<uint8_t> p(14, 0);
        p[4] = id;
        p[5] = uint8_t((level & 7) << 5); // group 0, level
        p[7] = typeIndex;
        Set16(p, 8, nameOff);
        Set16(p, 10, attrTable);
        return p;
    };
    std::vector<uint8_t> parts;
    // SPOILER (type 0)
    for (const std::vector<uint8_t> &p : {part(12, 0, 1, nameJorgen, 0xffff),
                                          part(12, 0, 2, nameMystic, 0xffff),
                                          part(12, 0, 1, nameCarbon, 1),
                                          part(12, 0, 4, nameUnique, 0xffff)})
        parts.insert(parts.end(), p.begin(), p.end());
    // ROOF (type 1): the stock record does not count, and styles restart for each body and finish.
    for (const std::vector<uint8_t> &p : {part(7, 1, 0, nameNone, 0xffff),
                                          part(7, 1, 1, nameJetstream, 0xffff),
                                          part(7, 1, 1, nameHeadwind, 0xffff),
                                          part(7, 1, 2, nameOffset, 0xffff),
                                          part(7, 1, 3, nameDual, 0xffff),
                                          part(7, 1, 1, nameRoofCarbon, 1)})
        parts.insert(parts.end(), p.begin(), p.end());
    // EXHAUST (type 2): the level is only the price tier, so the style keeps counting.
    for (const std::vector<uint8_t> &p : {part(16, 2, 1, nameBacklash, 0xffff),
                                          part(16, 2, 2, nameVortex, 0xffff)})
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

// Per-car body pack (#188): three hood records whose model table names the geometry style.  The
// shop order is BEAST/DOUBLE-V/TRI-SLOT but the table says styles 9, (none) and 7, proving the
// table wins and a table-less record still falls back to the pack-order style.
std::vector<uint8_t> MakeModelPack()
{
    std::vector<uint8_t> header(0x40, 0);
    Set32(header, 0x28, 1); // numTypes
    Set32(header, 0x38, 5); // numParts

    std::vector<uint8_t> types;
    Put32(types, bStringHash("SKYLINE"));

    std::vector<uint8_t> strings;
    auto name = [&](const char *text) {
        const uint16_t at = uint16_t(strings.size() / 4);
        PutName(strings, text);
        return at;
    };
    const uint16_t nameFirst = name("BEAST");
    const uint16_t nameSecond = name("DOUBLE-V");
    const uint16_t nameThird = name("TRI-SLOT");
    const uint16_t styleNine = name("_STYLE09");
    const uint16_t styleSeven = name("_STYLE07");
    const uint16_t kitSeventeen = name("_KIT17"); // bumper / skirt: the body panels use the kit number
    const uint16_t kitWideThree = name("_KITW03"); // wide-body shell (#193)

    std::vector<uint8_t> models(5 * 36, 0);
    models[0] = 1;                // TemplatedNameHashes: the middle string is a _STYLE name
    Set16(models, 2, styleNine);
    models[36] = 0;               // plain model hash: no style number
    models[72] = 1;
    Set16(models, 72 + 2, styleSeven);
    models[108] = 1;              // front bumper: the middle string is the kit number (#190)
    Set16(models, 108 + 2, kitSeventeen);
    models[144] = 1;              // wide-body kit: the middle string is _KITW<NN> (#193)
    Set16(models, 144 + 2, kitWideThree);

    std::vector<uint8_t> parts;
    auto part = [&](uint16_t nameOff, uint16_t model, uint8_t partId) {
        std::vector<uint8_t> p(14, 0);
        p[4] = partId;            // 9 = hood, 1 = front bumper
        p[5] = uint8_t(1 << 5);   // group 0, level 1
        p[7] = 0;                 // type 0
        Set16(p, 8, nameOff);
        Set16(p, 10, 0xffff);     // no attributes
        Set16(p, 12, model);
        return p;
    };
    for (uint16_t model : {uint16_t(0), uint16_t(1), uint16_t(2)}) {
        const uint16_t n = model == 0 ? nameFirst : model == 1 ? nameSecond : nameThird;
        std::vector<uint8_t> p = part(n, model, 9);
        parts.insert(parts.end(), p.begin(), p.end());
    }
    {
        const uint16_t n = name("KIT BUMPER");
        std::vector<uint8_t> p = part(n, 3, 1);
        parts.insert(parts.end(), p.begin(), p.end());
    }
    {
        const uint16_t n = name("WIDE KIT");
        std::vector<uint8_t> p = part(n, 4, 6);
        parts.insert(parts.end(), p.begin(), p.end());
    }

    std::vector<uint8_t> tables(2, 0);
    std::vector<uint8_t> attrs;
    Put32(attrs, 0x12345678);
    Put32(attrs, 1);

    std::vector<uint8_t> payload;
    Chunk(payload, 0x34603, header);
    Chunk(payload, 0x34604, parts);
    Chunk(payload, 0x34605, attrs);
    Chunk(payload, 0x34606, strings);
    Chunk(payload, 0x3460a, models);
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
    // ROOF (slot 7), its carbon category, and EXHAUST (slot 0x10).
    block(0x14, 4, 50, 150);
    block(0x14, 0x10, 100, 2500);
    block(0x14, 0x1c, 150, 5000);
    block(0x46, 4, 75, 7500);
    block(0x15, 4, 40, 1100);
    block(0x15, 0x10, 80, 1200);

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
    const uint32_t roof = bStringHash("ROOF");
    const uint32_t exhaust = bStringHash("EXHAUST");
    if (cat.Count(roof) != 6 || cat.Count(exhaust) != 2)
        return Fail("roof/exhaust record counts");

    // ROOF: the stock record is style 0 and the style number restarts for each body (base /
    // OFFSET / DUAL) and finish (#186).
    const std::vector<CarPart> &roofs = cat.Parts(roof);
    if (roofs[0].style != 0 || roofs[0].level != 0)
        return Fail("stock roof record has no style");
    if (roofs[1].style != 1 || roofs[1].variant != 0 || roofs[1].carbon)
        return Fail("first roof base style");
    if (roofs[2].style != 2 || roofs[2].variant != 0)
        return Fail("second roof base style");
    if (roofs[3].style != 1 || roofs[3].variant != 1 || roofs[3].level != 2)
        return Fail("roof OFFSET style restarts");
    if (roofs[4].style != 1 || roofs[4].variant != 2 || roofs[4].level != 3)
        return Fail("roof DUAL style restarts");
    if (roofs[5].style != 1 || !roofs[5].carbon || roofs[5].variant != 0)
        return Fail("carbon roof keeps the base style");
    // EXHAUST: level is the price tier, so the style keeps counting across tiers.
    const std::vector<CarPart> &exhausts = cat.Parts(exhaust);
    if (exhausts[0].style != 1 || exhausts[1].style != 2 || exhausts[1].level != 2)
        return Fail("exhaust style counts across price tiers");

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
    if (PartCatalog::PriceCategory(roofs[1]) != 0x14 || PartCatalog::PriceCategory(roofs[3]) != 0x14)
        return Fail("roof scoop price category");
    if (PartCatalog::PriceCategory(roofs[5]) != 0x46)
        return Fail("carbon roof scoop price category");
    if (PartCatalog::PriceCategory(exhausts[0]) != 0x15)
        return Fail("exhaust tip price category");

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
    if (priced.Price(roofs[1]) != 150 || priced.Price(roofs[3]) != 2500 || priced.Price(roofs[4]) != 5000)
        return Fail("roof scoop prices per body/tier");
    if (priced.Price(roofs[5]) != 7500)
        return Fail("carbon roof scoop price");
    if (priced.Price(exhausts[0]) != 1100 || priced.Price(exhausts[1]) != 1200)
        return Fail("exhaust tip prices per tier");

    // Per-car body parts (#188): the model table's _STYLE name is the geometry style, not the pack
    // position; a record whose model entry has no template falls back to the ordinal.
    const std::vector<uint8_t> modelPack = MakeModelPack();
    PartCatalog cars;
    if (!cars.Parse(modelPack.data(), modelPack.size()))
        return Fail("Parse rejected the per-car pack");
    const std::vector<CarPart> &hoods = cars.Parts(bStringHash("SKYLINE"));
    if (hoods.size() != 5)
        return Fail("per-car part count");
    if (hoods[0].style != 9 || hoods[1].style != 2 || hoods[2].style != 7)
        return Fail("model-table style must override the pack order");
    // _KIT<NN> is the bumper/skirt geometry number (#190); it lands in the same style field.
    if (hoods[3].partId != 1 || hoods[3].style != 17)
        return Fail("model-table kit number must be the bumper style");
    // _KITW<NN> is the wide-body shell number (#193); without this it would fall back to the
    // type-wide ordinal and the shop would hide every kit.
    if (hoods[4].partId != 6 || hoods[4].style != 3)
        return Fail("model-table wide-body kit number must be the shell style");
    if (PartCatalog::PriceCategory(hoods[0]) != 0xb)
        return Fail("hood price category");

    std::printf("partcatalog_test: ok (%zu spoilers, styles 1..%u; %zu roofs, %zu exhausts; "
                "hood styles 9/2/7, bumper kit 17, wide-body kit 3)\n",
                parts.size(), unsigned(parts.back().style), roofs.size(), exhausts.size());
    return 0;
}
