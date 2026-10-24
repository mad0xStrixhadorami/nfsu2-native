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
    // The city-wide map of the career World Map (ISSUES #293): lane-link segments (x0 y0 x1 y1 per link, world metres) drawn as roads
    // `width` metres wide. The loaded region's own bitmap only covers its 1 km, while events sit all over the 4 km city.
    void BuildFromLinks(const std::vector<float> &segments, float width);
    bool Valid() const { return m_w > 0; }
    struct Dot {
        float x, y;
        uint8_t r, g, b;
    };
    // Renders a size x size RGBA image (0xAABBGGRR) centred on (px, py). metres = the window's diameter. rotate: the
    // player's yaw is up, otherwise north (+Y) is up. route (optional) is drawn as a line.
    void Render(float px, float py, float yaw, float metres, bool rotate, const RaceRoute *route, const std::vector<Dot> &dots,
                int size, std::vector<uint32_t> &out, bool roundMask = true) const;
    // The Unbound-style map (ISSUES #296): the same window as Render but styled for ModernUI to draw natively: a dark gradient ground, roads as
    // a blue-grey fill with bright edges and a soft glow, the route as a thick glowing amber line with direction dots, the finish as a
    // flag marker at the end of the route, rivals as red dots with a white ring. No player arrow (ModernUI draws it crisp on top).
    void RenderModern(float px, float py, float yaw, float metres, bool rotate, const RaceRoute *route, const std::vector<Dot> &dots, int size,
                      std::vector<uint32_t> &out) const;
    // The whole region, north up, as a square image (the full-screen maps). `player` (optional) is drawn as an arrow.
    void RenderFull(const RaceRoute *route, const std::vector<Dot> &dots, int size, std::vector<uint32_t> &out, const Dot *player = nullptr,
                    float playerYaw = 0.0f) const;

private:
    std::vector<uint8_t> m_road; // 255 on a road
    std::vector<uint8_t> m_edge, m_glow; // derived once at build: the road's inner edge, and the road blurred (for the glow), see BuildDerived
    void BuildDerived();
    int m_w = 0, m_h = 0;
    float m_x0 = 0, m_y0 = 0;
    static constexpr float kRes = 1.5f; // metres per bitmap pixel
};
