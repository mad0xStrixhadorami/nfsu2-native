#pragma once
#include <cstdint>
#include <vector>

// The free-roam road network, from Tracks\Routes<region>\PathsFreeRoam.bin (chunk 0x80034147,
// loader 0x00344fb8, ISSUES.md #130). Chunk 0x3414d is an array of 36-byte directed lane links:
//   float x0, y0, x1, y1; u8 flags[4] (flags[1] = lane class 1..4); u32 id; u32 pad[3]
// Each link's end point is the next link's start point (442 of 473 in L4RG; the other ends are
// dead ends), so a car follows a lane by hopping to the link starting where the last one ended.
class TrafficNet {
public:
    struct Link {
        float x0, y0, x1, y1;
        float length;
        int next; // link starting at (x1, y1), or -1
        uint8_t laneClass;
    };
    bool Load(const std::vector<uint8_t> &paths);
    bool Valid() const { return !m_links.empty(); }
    const std::vector<Link> &Links() const { return m_links; }
    // Position and unit direction at distance t along link i.
    void Sample(int i, float t, float &x, float &y, float &dx, float &dy) const;

private:
    std::vector<Link> m_links;
};
