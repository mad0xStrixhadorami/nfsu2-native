#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Career {

// The free-roam shop hot spots (ISSUES #269).  GLOBALB's career container 0x80034a10 holds 0x34a12: 24 records of 0xa0
// bytes {char name[0x20] (e.g. CC_PERF_SHOP); u32 hashA, hashB at +0x38/+0x3c (bStringHash of the name and of the
// locator); char scene[] at +0x40 (the shop scene file: PERFSHOP01.BIN, PARTSSHOP01.BIN, PAINTSHOP01.BIN,
// AUDIOSHOP01.BIN, CARLOT01.BIN, MEGALOSHOP01.BIN, CRIB01.BIN); u8 +0x9c, u8 area +0x9d (1 Coastal Commons, 2 City
// Core?/Uniques..., see Area())}.  Each region's Tracks\Routes<region>\PathsFreeRoam.bin keeps the positions as type 14
// records inside chunk 0x3414a: the u32 hashB sits 0x30 bytes after the type, preceded by float x, y, dir x, dir y,
// height z.
struct Hotspot {
    std::string name;  // e.g. "CC_PERF_SHOP"
    std::string scene; // e.g. "PERFSHOP01.BIN"
    uint32_t hash = 0;
    uint8_t flag = 0, area = 0;
    float x = 0, y = 0, dx = 0, dy = 1, z = 0; // z = road height at the spot
    // Trigger polygon: 4 corner pairs at record +0x14 after hashB (an oriented rectangle about the spot, ~15 m a side);
    // hasZone is false for a record too short to hold it.
    float zone[4][2] = {};
    bool hasZone = false;
    bool Contains(float px, float py) const; // inside the trigger polygon (falls back to a 9 m circle)
    enum Kind { kCrib, kCarLot, kBodyShop, kGraphics, kPerformance, kSpecialty, kOther };
    Kind kind() const;
};

class HotspotTable {
public:
    bool LoadShops(const uint8_t *bundle, size_t size);  // decompressed GLOBALB
    // Takes a region's PathsFreeRoam.bin; fills the list with the hot spots found in it.
    size_t LoadPositions(const std::vector<uint8_t> &paths);
    const std::vector<Hotspot> &All() const { return m_placed; }
    static HotspotTable &Get();

private:
    std::vector<Hotspot> m_shops, m_placed;
};

} // namespace Career
