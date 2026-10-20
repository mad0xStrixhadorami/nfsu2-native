#pragma once
#include <cstdint>
#include <vector>

// A race's route through the road network, from Tracks\Routes<region>\Paths<race>.bin
// (chunk 0x80034147, loaded by 0x00344fb8):
//   0x34148 road nodes, 24 bytes: float x, y; u16 ?; u16 ?; s32 ?; u32 ?; float ?
//   0x34149 road segments, 220 bytes: char name[16]; u32 ? (+0x10); u32 race order (+0x14);
//           u16 first node, end node (+0x1c, +0x1e: nodes [first, end) are its points ~25-30 m
//           apart); float length (+0x64); float distance range (+0x74/+0x78); float direction
//           (+0x7c); float bbox (+0x88)
// Chaining the segments by race order (each oriented to continue from the previous one) gives the
// route polyline: track 4012 closes into a 1,870 m loop matching its race record length.
class RaceRoute {
public:
    // `targetLength` is the race record's length (metres); the Paths file also carries the
    // region's other lanes/branches, so it selects the connected cycle/path of that length.
    bool Load(const std::vector<uint8_t> &paths, float targetLength = 0.0f);
    bool Valid() const { return m_points.size() >= 2; }
    bool Closed() const { return m_closed; }
    float Length() const { return m_length; }
    // Traverse the same geometry the other way (race settings +4 Reverse, 0x0025c7b0): the route
    // points and cumulative distances are reversed, so progress/AI/lap logic need no direction flag.
    void Reverse();

    // Distance along the route of the nearest route point to (x, y). With hint >= 0 only the
    // stretch within `window` metres of the hint is searched (keeps progress continuous where
    // the route passes itself); otherwise the whole route.
    float Project(float x, float y, float hint = -1.0f, float window = 80.0f) const;
    // Position and unit direction at distance s (wrapped on a closed route).
    void Sample(float s, float &x, float &y, float &dx, float &dy) const;
    // Signed difference b - a, wrapped into (-L/2, L/2] on a closed route.
    float Delta(float a, float b) const;

private:
    struct Point {
        float x, y;
    };
    std::vector<Point> m_points;
    std::vector<float> m_dist; // cumulative distance at each point
    float m_length = 0;
    bool m_closed = false;
};
