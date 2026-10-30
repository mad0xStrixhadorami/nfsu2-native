#pragma once
#include <cstdint>
#include <vector>

// The free-roam road network, from Tracks\Routes<region>\RoutesFreeRoam.bin (chunk 0x34121 "TrackRoutes", handler 0x00342fa8 ->
// 0x003416a0 / 0x003415f0, ISSUES #388).  The chunk is a run of sections; a section is a 0x8c-byte header followed by `count`
// 0x38-byte waypoints (count: s16 at +0x34; the next section starts right after, so the stride is 0x8c + count * 0x38):
//   header +0x0a u16 section id (101..1219), +0x34 s16 count, +0x40 float length (m), +0x44..+0x50 four u32 (-1 here),
//          +0x5c four floats (the section's bounding corners);
//   waypoint +0x00 float x, +0x04 float y, +0x0c.. packed lane / link bytes (not decoded).
// Sections are whole road pieces (a few hundred metres to ~1.4 km); a section's end meets the start of the next within ~25 m.
class RoadNet {
public:
    struct Section {
        int id = 0;
        float length = 0;
        std::vector<float> x, y;
        std::vector<int> next; // sections starting where this one ends (indices)
    };
    bool Load(const std::vector<uint8_t> &routes);
    bool Valid() const { return !m_sections.empty(); }
    const std::vector<Section> &Sections() const { return m_sections; }

private:
    std::vector<Section> m_sections;
};
