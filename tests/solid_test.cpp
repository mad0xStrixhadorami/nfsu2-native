// 0x134011 solid-header transform (#175): the detached stock spoiler is authored around its own
// origin and mounted by the column-major matrix at header +0x40 (translation in [12..14]).
// 0x13401a position markers (#178): a 16-byte-aligned array of {hash, i32, f32, f32, matrix} records.
#include "game/indep/World/SolidMesh.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

bool Near(float a, float b)
{
    return std::fabs(a - b) < 1e-5f;
}

void PutU32(std::vector<uint8_t> &d, size_t off, uint32_t v) { std::memcpy(&d[off], &v, 4); }
void PutF32(std::vector<uint8_t> &d, size_t off, float v) { std::memcpy(&d[off], &v, 4); }

// One 0x80134010 solid with a 0xc0-byte header and one 0x13401a marker record, laid out like the
// car files: the marker body starts unaligned and is filled with 0x11 up to the next 16-byte bound.
std::vector<uint8_t> MarkerSolid()
{
    std::vector<uint8_t> d(0x130, 0);
    PutU32(d, 0, 0x80134010);
    PutU32(d, 4, 0x128);      // root body 8..0x130
    PutU32(d, 8, 0x134011);
    PutU32(d, 12, 0xc0);      // header body 0x10..0xd0
    std::memcpy(&d[0x10 + 0xa4], "TEST_SOLID", 10);
    PutU32(d, 0xd0, 0x13401a);
    PutU32(d, 0xd4, 0x58);    // 8 fill bytes + one 0x50 record, body 0xd8..0x130
    std::memset(&d[0xd8], 0x11, 8);
    const size_t rec = 0xe0;
    PutU32(d, rec, 0x90c81258); // ROOF_SCOOP
    PutF32(d, rec + 0x10 + 48, 1.5f);
    PutF32(d, rec + 0x10 + 52, -2.5f);
    PutF32(d, rec + 0x10 + 56, 3.5f);
    return d;
}

} // namespace

int main()
{
    SolidMesh::NamedSolid solid;
    const SolidMesh::Point p{1.0f, 2.0f, 3.0f};
    SolidMesh::Point q = SolidMesh::TransformPoint(solid, p);
    assert(Near(q.x, 1.0f) && Near(q.y, 2.0f) && Near(q.z, 3.0f));

    // SKYLINE_KIT00_SPOILER_A's header translation: (-2.1749325, 0, 0.7869703).
    solid.transform[12] = -2.1749325f;
    solid.transform[14] = 0.7869703f;
    q = SolidMesh::TransformPoint(solid, p);
    assert(Near(q.x, -1.1749325f) && Near(q.y, 2.0f) && Near(q.z, 3.7869703f));

    // A Y mirror (right-hand mirror LOD) still follows the same row-vector convention.
    solid.transform[12] = solid.transform[14] = 0.0f;
    solid.transform[5] = -1.0f;
    q = SolidMesh::TransformPoint(solid, p);
    assert(Near(q.x, 1.0f) && Near(q.y, -2.0f) && Near(q.z, 3.0f));

    const std::vector<uint8_t> data = MarkerSolid();
    const std::vector<SolidMesh::NamedSolid> solids = SolidMesh::DecodeNamedSolids(data.data(), data.size());
    assert(solids.size() == 1);
    assert(solids[0].name == "TEST_SOLID");
    const SolidMesh::PositionMarker *marker = solids[0].FindMarker(0x90c81258);
    assert(marker != nullptr);
    assert(Near(marker->matrix[12], 1.5f) && Near(marker->matrix[13], -2.5f) &&
           Near(marker->matrix[14], 3.5f));
    assert(solids[0].FindMarker(0xbd7cf15e) == nullptr);

    // Body-panel availability (#192): the catalogue lists designs a car may not ship.  The
    // SKYLINE geometry has KIT01..18,20..22,24..26,29 but no KIT19/23/27/28/30; the COROLLA has
    // no STYLE01..04 headlights.  Only names actually present may be offered or mounted.
    std::vector<SolidMesh::NamedSolid> panels;
    for (const char *name : {"TEST_KIT01_FRONT_BUMPER_", "TEST_KIT01_REAR_BUMPER_A",
                             "TEST_KIT01_SKIRT_A", "TEST_KIT02_FRONT_BUMPER_",
                             "TEST_KITW01_BODY_A", "TEST_KIT19_HOOD_UNDER_LE",
                             "TEST_KIT23_HOOD_UNDER_RI", "TEST_STYLE01_HOOD_LEFT_A",
                             "TEST_STYLE01_HOOD_RIGHT_", "TEST_STYLE01_HEADLIGHT_LEFT_A",
                             "TEST_STYLE01_BRAKELIGHT_"}) {
        SolidMesh::NamedSolid s;
        s.name = name;
        panels.push_back(std::move(s));
    }
    const SolidMesh::BodyStyleSet styles = SolidMesh::FindBodyStyles(panels);
    assert(styles.Has(0x1, 1) && styles.Has(0x2, 1) && styles.Has(0xb, 1));
    assert(styles.Has(0x1, 2) && !styles.Has(0x2, 2));
    assert(!styles.Has(0x1, 19) && !styles.Has(0x1, 23) && !styles.Has(0x1, 0));
    assert(styles.Has(9, 1) && styles.Has(0xe, 1) && styles.Has(0xf, 1));
    assert(!styles.Has(9, 2) && !styles.Has(0xe, 2) && !styles.Has(0xf, 2));
    assert(!styles.Has(9, 19) && !styles.Has(9, 23)); // KIT hood unders are not STYLE hoods
    assert(styles.Has(6, 1) && !styles.Has(6, 2));    // KITW wide-body shell (#193)
    return 0;
}
