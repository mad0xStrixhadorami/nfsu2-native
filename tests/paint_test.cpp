// PaintCatalog parser test (ISSUES #167): a synthetic GLOBALB part pack, so the GLOSS parsing can
// be checked without the disc.  The real pack is chunk 0x80034602:
//   0x34603 header (numTypes at +0x28)
//   0x34604 14-byte part records: +4 part id, +7 type index, +8 name offset (u16 units),
//           +10 attribute table offset (u16 units)
//   0x34605 8-byte {attribute-name hash, value} records
//   0x34606 strings, each at 4 * u16 offset
//   0x3460b car-type hashes (PAINT)
//   0x3460c per-part attribute tables: i16 count + i16 attribute offsets
#include <cstdio>
#include <cstring>
#include <vector>

#include "Speed/Indep/bWare/Inc/Strings.hpp"
#include "game/indep/Frontend/PaintCatalog.h"

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

// One PAINT part (id 48, the paint/hoses/muffler group) with the GLOSS color set.
std::vector<uint8_t> MakePack()
{
    std::vector<uint8_t> header(0x2c, 0);
    Set32(header, 0x28, 1); // numTypes

    std::vector<uint8_t> types;
    Put32(types, bStringHash("PAINT"));

    std::vector<uint8_t> part(14, 0);
    part[4] = 48; // part id
    part[7] = 0;  // PAINT type index
    Set16(part, 8, 0);  // name offset (u16 units)
    Set16(part, 10, 1); // attribute table offset (u16 units)

    std::vector<uint8_t> attrs;
    const uint32_t finishAttr = 0xebb03e66;
    const uint32_t glossSet = bStringHash("GLOSS");
    Put32(attrs, finishAttr);
    Put32(attrs, glossSet);
    Put32(attrs, bStringHash("GLOSS"));
    Put32(attrs, 200);
    Put32(attrs, bStringHash("RED"));
    Put32(attrs, 10);
    Put32(attrs, bStringHash("GREEN"));
    Put32(attrs, 20);
    Put32(attrs, bStringHash("BLUE"));
    Put32(attrs, 30);

    std::vector<uint8_t> strings;
    const char *name = "GLOSS_TEST_COLOR";
    strings.insert(strings.end(), name, name + strlen(name) + 1);

    // Attribute table 1: u16 count=5 followed by the five attribute indices (8-byte records).
    std::vector<uint8_t> tables(2, 0); // table 0 is unused
    Put16(tables, 5);
    for (int i = 0; i < 5; ++i)
        Put16(tables, uint16_t(i));

    std::vector<uint8_t> payload;
    Chunk(payload, 0x34603, header);
    Chunk(payload, 0x34604, part);
    Chunk(payload, 0x34605, attrs);
    Chunk(payload, 0x34606, strings);
    Chunk(payload, 0x3460b, types);
    Chunk(payload, 0x3460c, tables);

    std::vector<uint8_t> pack;
    Chunk(pack, 0x80034602, payload);
    return pack;
}

int Fail(const char *what)
{
    std::fprintf(stderr, "paint_test: %s\n", what);
    return 1;
}

} // namespace

int main()
{
    const std::vector<uint8_t> pack = MakePack();

    PaintCatalog cat;
    if (!cat.Parse(pack.data(), pack.size()))
        return Fail("whole 0x80034602 bundle did not parse");
    if (cat.Count(PaintCatalog::kGloss) != 1)
        return Fail("expected one GLOSS colour");
    const PaintColor &c = cat.At(PaintCatalog::kGloss, 0);
    if (c.name != "GLOSS_TEST_COLOR" || c.r != 10 || c.g != 20 || c.b != 30 || c.gloss != 200)
        return Fail("colour fields mismatch");

    // AddBundle may hand Parse the 0x80034602 payload directly (no wrapper chunk).
    PaintCatalog payloadCat;
    if (!payloadCat.Parse(pack.data() + 8, pack.size() - 8) ||
        payloadCat.Count(PaintCatalog::kGloss) != 1 ||
        payloadCat.At(PaintCatalog::kGloss, 0).name != "GLOSS_TEST_COLOR")
        return Fail("payload form did not parse");

    // A non-GLOSS colour set (e.g. METAL) is its own set, not part of GLOSS (#168).
    std::vector<uint8_t> metal = pack;
    // The finish value is the second u32 of the first attribute record; find the attributes chunk
    // and patch it.  The chunk layout above is fixed, so walk it to stay honest about offsets.
    size_t off = 8; // skip the 0x80034602 wrapper
    while (off + 8 <= metal.size()) {
        const uint32_t id = uint32_t(metal[off]) | (uint32_t(metal[off + 1]) << 8) |
                            (uint32_t(metal[off + 2]) << 16) | (uint32_t(metal[off + 3]) << 24);
        const uint32_t size = uint32_t(metal[off + 4]) | (uint32_t(metal[off + 5]) << 8) |
                              (uint32_t(metal[off + 6]) << 16) | (uint32_t(metal[off + 7]) << 24);
        if (id == 0x34605 && size >= 8) {
            Set32(metal, off + 8 + 4, bStringHash("METAL"));
            break;
        }
        off += 8 + size;
    }
    PaintCatalog metalCat;
    if (!metalCat.Parse(metal.data(), metal.size()))
        return Fail("METAL pack did not parse");
    if (metalCat.Count(PaintCatalog::kMetal) != 1 || metalCat.Count(PaintCatalog::kGloss) != 0)
        return Fail("METAL must be its own set, not GLOSS");

    // 0x002bedd8: the nine paint-shop part ids and their sets.
    if (PaintCatalog::SetForPart(0x3f) != PaintCatalog::kGloss ||
        PaintCatalog::SetForPart(0x45) != PaintCatalog::kGloss ||
        PaintCatalog::SetForPart(0x4b) != PaintCatalog::kGloss ||
        PaintCatalog::SetForPart(0x4c) != PaintCatalog::kGloss ||
        PaintCatalog::SetForPart(0x44) != PaintCatalog::kHoses ||
        PaintCatalog::SetForPart(0x46) != PaintCatalog::kBrakes ||
        PaintCatalog::SetForPart(0x47) != PaintCatalog::kMufflers ||
        PaintCatalog::SetForPart(0x49) != PaintCatalog::kRims ||
        PaintCatalog::SetForPart(0x4a) != PaintCatalog::kRims ||
        PaintCatalog::SetForPart(0x99) != 0)
        return Fail("SetForPart mapping mismatch");

    // 0x002bcd00 labels the finish with bStringHash("_PAINT", set); these are the ENGLISH.BIN
    // strings for the five finish hashes (ISSUES #171).
    if (std::strcmp(PaintCatalog::SetName(PaintCatalog::kGloss), "Gloss") != 0 ||
        std::strcmp(PaintCatalog::SetName(PaintCatalog::kMetal), "Metallic") != 0 ||
        std::strcmp(PaintCatalog::SetName(PaintCatalog::kPearl), "Pearlescent") != 0 ||
        std::strcmp(PaintCatalog::SetName(PaintCatalog::kHoses), "Powder Coat") != 0 ||
        std::strcmp(PaintCatalog::SetName(PaintCatalog::kRims), "Powder Coat") != 0 ||
        std::strcmp(PaintCatalog::SetName(PaintCatalog::kMufflers), "High Heat") != 0 ||
        std::strcmp(PaintCatalog::SetName(PaintCatalog::kBrakes), "High Heat") != 0)
        return Fail("finish label mismatch");

    std::printf("paint_test: passed (%zu GLOSS colours)\n", cat.Count(PaintCatalog::kGloss));
    return 0;
}
