#pragma once
#include <cstdint>
#include <vector>

class RaceRoute;

// The HUD minimap (ISSUES #259). The original's per-track map image (TRACKS\TRACKMAP<n>.bin) is not on the disc in a
// form the port can read, so the map is drawn from the world itself: the region's road triangles are rasterised once
// into a top-down bitmap, and every frame a window around the player (heading up, or north up) is sampled into a
// round RGBA image with the race route, rivals and the player's arrow on top.
class Minimap {
public:
    // roadTris: x0 y0 x1 y1 x2 y2 per triangle (world metres).
    void Build(const std::vector<float> &roadTris);
    bool Valid() const { return m_w > 0; }
    struct Dot {
        float x, y;
        uint8_t r, g, b;
    };
    // Renders a size x size RGBA image (0xAABBGGRR) centred on (px, py). metres = the window's diameter. rotate: the
    // player's yaw is up, otherwise north (+Y) is up. route (optional) is drawn as a line.
    void Render(float px, float py, float yaw, float metres, bool rotate, const RaceRoute *route, const std::vector<Dot> &dots,
                int size, std::vector<uint32_t> &out) const;

private:
    std::vector<uint8_t> m_road; // 255 on a road
    int m_w = 0, m_h = 0;
    float m_x0 = 0, m_y0 = 0;
    static constexpr float kRes = 1.5f; // metres per bitmap pixel
};
