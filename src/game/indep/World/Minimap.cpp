#include "game/indep/World/Minimap.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "game/indep/World/RaceRoute.h"

void Minimap::Build(const std::vector<float> &t)
{
    m_road.clear();
    m_w = m_h = 0;
    if (t.size() < 6)
        return;
    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
    for (size_t i = 0; i + 1 < t.size(); i += 2) {
        lo[0] = std::min(lo[0], t[i]), hi[0] = std::max(hi[0], t[i]);
        lo[1] = std::min(lo[1], t[i + 1]), hi[1] = std::max(hi[1], t[i + 1]);
    }
    m_x0 = lo[0] - 8.0f, m_y0 = lo[1] - 8.0f;
    m_w = int((hi[0] - lo[0] + 16.0f) / kRes) + 1, m_h = int((hi[1] - lo[1] + 16.0f) / kRes) + 1;
    if (m_w > 6000 || m_h > 6000) { // a runaway bound (a stray vertex): clamp the bitmap, not the map
        m_w = std::min(m_w, 6000), m_h = std::min(m_h, 6000);
    }
    m_road.assign(size_t(m_w) * size_t(m_h), 0);
    for (size_t i = 0; i + 5 < t.size(); i += 6) {
        float x[3], y[3];
        for (int k = 0; k < 3; ++k)
            x[k] = (t[i + k * 2] - m_x0) / kRes, y[k] = (t[i + k * 2 + 1] - m_y0) / kRes;
        const int minx = std::max(0, int(std::floor(std::min({x[0], x[1], x[2]})))), maxx = std::min(m_w - 1, int(std::ceil(std::max({x[0], x[1], x[2]}))));
        const int miny = std::max(0, int(std::floor(std::min({y[0], y[1], y[2]})))), maxy = std::min(m_h - 1, int(std::ceil(std::max({y[0], y[1], y[2]}))));
        const float d = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2]);
        if (std::fabs(d) < 1e-6f) { // a sliver: draw its longest edge
            continue;
        }
        for (int py = miny; py <= maxy; ++py)
            for (int px = minx; px <= maxx; ++px) {
                const float fx = px + 0.5f, fy = py + 0.5f;
                const float a = ((y[1] - y[2]) * (fx - x[2]) + (x[2] - x[1]) * (fy - y[2])) / d;
                const float b = ((y[2] - y[0]) * (fx - x[2]) + (x[0] - x[2]) * (fy - y[2])) / d;
                const float c = 1.0f - a - b;
                const float e = -0.5f; // a little slack so thin strips stay connected
                if (a >= e * 0.2f && b >= e * 0.2f && c >= e * 0.2f)
                    m_road[size_t(py) * size_t(m_w) + size_t(px)] = 255;
            }
    }
    printf("[Minimap] road bitmap %d x %d (%.1f m / px), %zu road triangles\n", m_w, m_h, kRes, t.size() / 6);
}

void Minimap::BuildFromLinks(const std::vector<float> &seg, float width)
{
    m_road.clear();
    m_w = m_h = 0;
    if (seg.size() < 4)
        return;
    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
    for (size_t i = 0; i + 1 < seg.size(); i += 2) {
        lo[0] = std::min(lo[0], seg[i]), hi[0] = std::max(hi[0], seg[i]);
        lo[1] = std::min(lo[1], seg[i + 1]), hi[1] = std::max(hi[1], seg[i + 1]);
    }
    m_x0 = lo[0] - 40.0f, m_y0 = lo[1] - 40.0f;
    m_w = std::min(6000, int((hi[0] - lo[0] + 80.0f) / kRes) + 1), m_h = std::min(6000, int((hi[1] - lo[1] + 80.0f) / kRes) + 1);
    m_road.assign(size_t(m_w) * size_t(m_h), 0);
    const float rad = std::max(1.0f, width * 0.5f / kRes);
    const int ri = int(std::ceil(rad));
    for (size_t i = 0; i + 3 < seg.size(); i += 4) {
        const float ax = (seg[i] - m_x0) / kRes, ay = (seg[i + 1] - m_y0) / kRes, bx = (seg[i + 2] - m_x0) / kRes, by = (seg[i + 3] - m_y0) / kRes;
        const float len = std::hypot(bx - ax, by - ay);
        const int steps = std::max(1, int(len / (rad * 0.5f)));
        for (int k = 0; k <= steps; ++k) {
            const float cx = ax + (bx - ax) * float(k) / float(steps), cy = ay + (by - ay) * float(k) / float(steps);
            for (int dy = -ri; dy <= ri; ++dy)
                for (int dx = -ri; dx <= ri; ++dx) {
                    const int x = int(cx) + dx, y = int(cy) + dy;
                    if (x < 0 || y < 0 || x >= m_w || y >= m_h || float(dx * dx + dy * dy) > rad * rad)
                        continue;
                    m_road[size_t(y) * size_t(m_w) + size_t(x)] = 255;
                }
        }
    }
    printf("[Minimap] city map from %zu lane links: bitmap %d x %d, roads %.0f m wide\n", seg.size() / 4, m_w, m_h, width);
}

void Minimap::Render(float px, float py, float yaw, float metres, bool rotate, const RaceRoute *route, const std::vector<Dot> &dots,
                     int n, std::vector<uint32_t> &out, bool roundMask) const
{
    out.assign(size_t(n) * size_t(n), 0);
    const float R = metres * 0.5f;
    const float cy = std::cos(rotate ? yaw : 0.0f), sy = std::sin(rotate ? yaw : 0.0f);
    // screen (u right, v up, in metres from the centre) -> world offset
    auto toWorld = [&](float u, float v, float &wx, float &wy) {
        // rotate mode: up = forward (cos yaw, sin yaw), right = (sin yaw, -cos yaw); north-up: up = +Y, right = +X
        if (rotate) {
            wx = px + v * cy + u * sy;
            wy = py + v * sy - u * cy;
        } else {
            wx = px + u;
            wy = py + v;
        }
    };
    // world -> screen pixel
    auto toPixel = [&](float wx, float wy, float &sx, float &sv) {
        const float dx = wx - px, dy = wy - py;
        float u, v;
        if (rotate) {
            v = dx * cy + dy * sy;
            u = dx * sy - dy * cy;
        } else {
            u = dx, v = dy;
        }
        sx = (u / R * 0.5f + 0.5f) * n;
        sv = (0.5f - v / R * 0.5f) * n;
    };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const float u = ((i + 0.5f) / n * 2.0f - 1.0f) * R, v = (1.0f - (j + 0.5f) / n * 2.0f) * R;
            const float rr = std::sqrt(u * u + v * v) / R;
            if (roundMask && rr > 1.0f)
                continue;
            float wx, wy;
            toWorld(u, v, wx, wy);
            const float bx = (wx - m_x0) / kRes - 0.5f, by = (wy - m_y0) / kRes - 0.5f;
            float road = 0;
            if (bx >= 0 && by >= 0 && bx < m_w - 1 && by < m_h - 1) { // bilinear
                const int x0 = int(bx), y0 = int(by);
                const float fx = bx - x0, fy = by - y0;
                auto at = [&](int x, int y) { return float(m_road[size_t(y) * size_t(m_w) + size_t(x)]); };
                road = (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
                road /= 255.0f;
            }
            const float edge = roundMask ? std::clamp((1.0f - rr) * 18.0f, 0.0f, 1.0f) : 1.0f; // soft rim
            const uint8_t rch = uint8_t(14 + road * (205 - 14)), gch = uint8_t(20 + road * (210 - 20)), bch = uint8_t(34 + road * (220 - 34));
            out[size_t(j) * n + i] = uint32_t(edge * 235.0f) << 24 | uint32_t(bch) << 16 | uint32_t(gch) << 8 | rch;
        }
    auto plot = [&](float sx, float sv, int rad, uint8_t r, uint8_t g, uint8_t b) {
        for (int dy = -rad; dy <= rad; ++dy)
            for (int dx = -rad; dx <= rad; ++dx) {
                const int x = int(sx) + dx, y = int(sv) + dy;
                if (dx * dx + dy * dy > rad * rad + 1 || x < 0 || y < 0 || x >= n || y >= n)
                    continue;
                uint32_t &px2 = out[size_t(y) * n + x];
                if ((px2 >> 24) == 0)
                    continue; // outside the round window
                px2 = 0xff000000u | uint32_t(b) << 16 | uint32_t(g) << 8 | r;
            }
    };
    if (route && route->Valid()) { // the race line, every 3 m
        const float len = route->Length();
        for (float s = 0; s < len; s += 3.0f) {
            float x, y, dx, dy, sx, sv;
            route->Sample(s, x, y, dx, dy);
            if (std::fabs(x - px) > R * 1.2f || std::fabs(y - py) > R * 1.2f)
                continue;
            toPixel(x, y, sx, sv);
            plot(sx, sv, std::max(1, n / 64), 255, 190, 30);
        }
    }
    for (const Dot &d : dots) {
        float sx, sv;
        toPixel(d.x, d.y, sx, sv);
        plot(sx, sv, std::max(2, n / 48), d.r, d.g, d.b);
    }
    if (!roundMask)
        return; // the full map marks the player separately (RenderFull)
    // The player's arrow at the centre, pointing along the heading (up in rotate mode).
    const float ang = rotate ? 0.0f : std::atan2(cy * 0 + std::sin(yaw), std::cos(yaw)); // north-up: arrow follows the yaw
    const float a0 = rotate ? 0.0f : (1.5707963f - yaw); // clockwise angle from up
    (void)ang;
    const float sz = n / 22.0f;
    const float cx0 = n * 0.5f, cy0 = n * 0.5f;
    const float tip[2] = {0.0f, -sz * 1.5f}, l[2] = {-sz * 0.9f, sz}, r2[2] = {sz * 0.9f, sz};
    auto rot = [&](const float p[2], float &ox, float &oy) {
        ox = cx0 + p[0] * std::cos(a0) - p[1] * std::sin(a0);
        oy = cy0 + p[0] * std::sin(a0) + p[1] * std::cos(a0);
    };
    float ax, ay, bx, by, cx2, cy2;
    rot(tip, ax, ay), rot(l, bx, by), rot(r2, cx2, cy2);
    const float minx = std::min({ax, bx, cx2}), maxx = std::max({ax, bx, cx2}), miny = std::min({ay, by, cy2}), maxy = std::max({ay, by, cy2});
    for (int y = int(miny); y <= int(maxy) + 1; ++y)
        for (int x = int(minx); x <= int(maxx) + 1; ++x) {
            const float fx = x + 0.5f, fy = y + 0.5f;
            const float d = (by - cy2) * (ax - cx2) + (cx2 - bx) * (ay - cy2);
            if (std::fabs(d) < 1e-6f)
                continue;
            const float a = ((by - cy2) * (fx - cx2) + (cx2 - bx) * (fy - cy2)) / d;
            const float b = ((cy2 - ay) * (fx - cx2) + (ax - cx2) * (fy - cy2)) / d;
            if (a >= 0 && b >= 0 && a + b <= 1 && x >= 0 && y >= 0 && x < n && y < n)
                out[size_t(y) * n + x] = 0xffffffffu;
        }
}

void Minimap::RenderModern(float px, float py, float yaw, float metres, bool rotate, const RaceRoute *route, const std::vector<Dot> &dots, int n,
                           std::vector<uint32_t> &out) const
{
    out.assign(size_t(n) * size_t(n), 0);
    const float R = metres * 0.5f;
    const float cy = std::cos(rotate ? yaw : 0.0f), sy = std::sin(rotate ? yaw : 0.0f);
    auto toWorld = [&](float u, float v, float &wx, float &wy) {
        if (rotate)
            wx = px + v * cy + u * sy, wy = py + v * sy - u * cy;
        else
            wx = px + u, wy = py + v;
    };
    auto toPixel = [&](float wx, float wy, float &sx, float &sv) {
        const float dx = wx - px, dy = wy - py;
        const float u = rotate ? dx * sy - dy * cy : dx, v = rotate ? dx * cy + dy * sy : dy;
        sx = (u / R * 0.5f + 0.5f) * n;
        sv = (0.5f - v / R * 0.5f) * n;
    };
    auto roadAt = [&](float bx, float by) { // bilinear road coverage 0..1 (smooth edges)
        bx -= 0.5f, by -= 0.5f;
        const int x = int(std::floor(bx)), y = int(std::floor(by));
        const float fx = bx - float(x), fy = by - float(y);
        auto at = [&](int xx, int yy) { return (xx < 0 || yy < 0 || xx >= m_w || yy >= m_h) ? 0.0f : float(m_road[size_t(yy) * size_t(m_w) + size_t(xx)]); };
        return ((at(x, y) * (1 - fx) + at(x + 1, y) * fx) * (1 - fy) + (at(x, y + 1) * (1 - fx) + at(x + 1, y + 1) * fx) * fy) * (1.0f / 255.0f);
    };
    auto blendPx = [&](int x, int y, float r, float g, float b, float a) {
        if (x < 0 || y < 0 || x >= n || y >= n)
            return;
        uint32_t &px2 = out[size_t(y) * n + x];
        if ((px2 >> 24) == 0)
            return;
        const float dr = float(px2 & 255), dg = float((px2 >> 8) & 255), db = float((px2 >> 16) & 255);
        const uint32_t nr = uint32_t(std::clamp(dr + (r - dr) * a, 0.0f, 255.0f)), ng = uint32_t(std::clamp(dg + (g - dg) * a, 0.0f, 255.0f)),
                       nb = uint32_t(std::clamp(db + (b - db) * a, 0.0f, 255.0f));
        const uint32_t oa = px2 >> 24, na = uint32_t(std::min(255.0f, float(oa) + 255.0f * a)); // drawn marks raise the plate's opacity
        px2 = na << 24 | nb << 16 | ng << 8 | nr;
    };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const float u = ((i + 0.5f) / n * 2.0f - 1.0f) * R, v = (1.0f - (j + 0.5f) / n * 2.0f) * R;
            const float rr = std::sqrt(u * u + v * v) / R;
            if (rr > 1.0f)
                continue;
            float wx, wy;
            toWorld(u, v, wx, wy);
            const float bx = (wx - m_x0) / kRes, by = (wy - m_y0) / kRes;
            const float c0 = roadAt(bx, by);
            // roads: the fill, its edge (inside, against the nearest non-road) and a soft glow around it
            const float e = 2.2f;
            const float nb = std::min({roadAt(bx + e, by), roadAt(bx - e, by), roadAt(bx, by + e), roadAt(bx, by - e)});
            const float edge = std::clamp(c0 * (1.0f - nb) * 1.25f, 0.0f, 1.0f);
            float glow = 0.0f;
            for (int k = 0; k < 8; ++k) {
                const float a = float(k) * 0.785398f;
                glow += roadAt(bx + std::cos(a) * 4.5f, by + std::sin(a) * 4.5f);
            }
            glow = (glow * 0.125f) * (1.0f - c0);
            // Frosted glass (NFS Unbound): a pale desaturated ground that lets the scene show through, darker towards the rim, with the roads
            // as thin pale lines over a faint fill.
            const float vig = 1.0f - 0.55f * rr * rr * rr;
            float r = 74.0f * vig + 6.0f, g = 82.0f * vig + 8.0f, b = 94.0f * vig + 10.0f;
            const float fill = c0 * 0.30f;
            r += fill * 70.0f, g += fill * 78.0f, b += fill * 90.0f;
            r += glow * 36.0f, g += glow * 40.0f, b += glow * 48.0f;
            r += edge * 150.0f, g += edge * 152.0f, b += edge * 158.0f;
            const float aa = std::clamp((1.0f - rr) * float(n) * 0.5f, 0.0f, 1.0f); // a 1-2 pixel anti-aliased rim
            const float alpha = (150.0f + 70.0f * rr * rr) * aa + (c0 > 0.1f ? 18.0f * aa : 0.0f);
            out[size_t(j) * n + i] = uint32_t(std::clamp(alpha, 0.0f, 255.0f)) << 24 | uint32_t(std::clamp(b, 0.0f, 255.0f)) << 16 |
                                     uint32_t(std::clamp(g, 0.0f, 255.0f)) << 8 | uint32_t(std::clamp(r, 0.0f, 255.0f));
        }
    auto disc = [&](float sx, float sv, float rad, float r, float g, float b, float a) {
        const int ir = int(std::ceil(rad)) + 1;
        for (int dy = -ir; dy <= ir; ++dy)
            for (int dx = -ir; dx <= ir; ++dx) {
                const float d = std::sqrt(float(dx * dx + dy * dy));
                if (d <= rad + 0.5f)
                    blendPx(int(sx) + dx, int(sv) + dy, r, g, b, a * std::clamp(rad + 0.5f - d, 0.0f, 1.0f));
            }
    };
    if (route && route->Valid()) {
        // The route: a thin line from yellow near the car to green far ahead (the gradient follows the distance from the centre).
        const float len = route->Length();
        const float rad = std::max(1.8f, n / 90.0f);
        for (int pass = 0; pass < 2; ++pass)
            for (float s = 0; s < len; s += 2.0f) {
                float x, y, dx, dy, sx, sv;
                route->Sample(s, x, y, dx, dy);
                if (std::fabs(x - px) > R * 1.25f || std::fabs(y - py) > R * 1.25f)
                    continue;
                toPixel(x, y, sx, sv);
                const float t = std::clamp(std::hypot(sx - n * 0.5f, sv - n * 0.5f) / (n * 0.5f), 0.0f, 1.0f);
                const float cr = 255.0f + (110.0f - 255.0f) * t, cg = 226.0f + (255.0f - 226.0f) * t, cb = 60.0f + (90.0f - 60.0f) * t;
                if (pass == 0)
                    disc(sx, sv, rad + 1.8f, cr, cg, cb, 0.22f); // the glow
                else
                    disc(sx, sv, rad, cr, cg, cb, 1.0f);
            }
        float fx, fy, fdx, fdy, sx, sv; // the finish: a ringed flag dot at the end of the route
        route->Sample(len, fx, fy, fdx, fdy);
        toPixel(fx, fy, sx, sv);
        const float rrf = std::hypot(sx - n * 0.5f, sv - n * 0.5f) / (n * 0.5f);
        if (rrf < 0.92f) {
            disc(sx, sv, rad + 3.4f, 255, 255, 255, 1.0f);
            disc(sx, sv, rad + 1.6f, 14, 18, 30, 1.0f);
            disc(sx, sv, rad - 0.2f, 255, 255, 255, 1.0f);
        }
    }
    for (const Dot &d : dots) {
        float sx, sv;
        toPixel(d.x, d.y, sx, sv);
        disc(sx, sv, std::max(4.2f, n / 54.0f), 255, 255, 255, 0.95f);
        disc(sx, sv, std::max(2.8f, n / 80.0f), d.r, d.g, d.b, 1.0f);
    }
}

void Minimap::RenderFull(const RaceRoute *route, const std::vector<Dot> &dots, int size, std::vector<uint32_t> &out, const Dot *player,
                         float playerYaw) const
{
    if (!Valid()) {
        out.assign(size_t(size) * size_t(size), 0);
        return;
    }
    const float metres = float(std::max(m_w, m_h)) * kRes;
    const float cx = m_x0 + float(m_w) * kRes * 0.5f, cy = m_y0 + float(m_h) * kRes * 0.5f;
    std::vector<Dot> all = dots;
    Render(cx, cy, 0.0f, metres, false, route, all, size, out, false);
    if (player) { // a larger marker with a heading tick
        const float R = metres * 0.5f;
        const float sx = ((player->x - cx) / R * 0.5f + 0.5f) * size, sv = (0.5f - (player->y - cy) / R * 0.5f) * size;
        const int rad = std::max(5, size / 45);
        for (int dy = -rad; dy <= rad; ++dy)
            for (int dx = -rad; dx <= rad; ++dx) {
                const int x = int(sx) + dx, y = int(sv) + dy;
                if (dx * dx + dy * dy <= rad * rad && x >= 0 && y >= 0 && x < size && y < size)
                    out[size_t(y) * size + x] = 0xffffffffu;
            }
        for (int k = rad; k < rad * 3; ++k) {
            const int x = int(sx + std::cos(playerYaw) * k), y = int(sv - std::sin(playerYaw) * k);
            if (x >= 0 && y >= 0 && x < size && y < size)
                out[size_t(y) * size + x] = 0xffffffffu;
        }
    }
}
