#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// UG2's performance part packages: GLOBALB container 0x80034a10, subchunk 0x34a15 (loader
// 0x00264db8 -> 0x00264198, ISSUES #197).  The chunk is an array of 0x17c-byte package records;
// a package is one (category, level) pair of the Performance Shop and carries up to 4 parts of
// 0x5c bytes:
//   package +0x00 category, +0x04 level (1..3), +0x08 part count, +0x0c parts
//   part    +0x00 part id (the global index 0x00264310 searches), +0x04 name hash (language string),
//           +0x08 price, +0x0c a small tag count/kind (0..8, meaning not decoded yet), +0x10.. up to
//           eight tag hashes (-1 = unused; meaning not decoded yet), +0x30 four stat floats {acceleration, top speed, handling, spare}, -1 = unused
// The four floats are the stat-bar deltas the shop's accel/top_speed/handling preview bars show
// (UI_BuyPerformanceParts' "accel_preview" etc., 0x002b7ba0).
struct PerfPart {
    uint32_t id = 0;
    uint32_t nameHash = 0;
    uint32_t price = 0;
    uint32_t kind = 0;                                    // +0x0c
    uint32_t tags[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u}; // +0x10..+0x2c
    float stat[4] = {-1, -1, -1, -1};
};

struct PerfPackage {
    uint32_t category = 0;
    uint32_t level = 0;
    std::vector<PerfPart> parts;

    uint32_t TotalPrice() const; // 0x00264500: the sum of the parts' prices
};

class PerfPackages {
public:
    // `container` is the payload of the 0x80034a10 GLOBALB chunk.
    bool Parse(const uint8_t *container, size_t size);
    bool Empty() const { return m_packages.empty(); }
    size_t Count() const { return m_packages.size(); }
    const PerfPackage &At(size_t i) const { return m_packages[i]; }
    // 0x00264388: the package for a category and level, or null.
    const PerfPackage *Find(uint32_t category, uint32_t level) const;
    // 0x00264310: the package holding a part id, or null.
    const PerfPackage *FindByPart(uint32_t partId) const;

private:
    std::vector<PerfPackage> m_packages;
};
