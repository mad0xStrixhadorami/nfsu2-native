#include "game/indep/World/RaceRoute.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace {
uint32_t Read32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

float Dist(float ax, float ay, float bx, float by) { return std::hypot(bx - ax, by - ay); }
} // namespace

bool RaceRoute::Load(const std::vector<uint8_t> &paths)
{
    m_points.clear();
    m_dist.clear();
    m_length = 0;
    m_closed = false;
    if (paths.size() < 8 || Read32(paths.data()) != 0x80034147)
        return false;
    const uint8_t *nodes = nullptr, *segments = nullptr;
    size_t nodeCount = 0, segmentCount = 0;
    const size_t end = std::min<size_t>(paths.size(), 8 + Read32(paths.data() + 4));
    for (size_t p = 8; p + 8 <= end;) {
        uint32_t id = Read32(paths.data() + p), len = Read32(paths.data() + p + 4);
        if (len > end - (p + 8))
            break;
        if (id == 0x34148) {
            nodes = paths.data() + p + 8;
            nodeCount = len / 24;
        } else if (id == 0x34149) {
            segments = paths.data() + p + 8;
            segmentCount = len / 220;
        }
        p += 8 + len;
    }
    if (!nodes || !segments)
        return false;

    std::map<uint32_t, std::vector<Point>> byOrder;
    for (size_t k = 0; k < segmentCount; ++k) {
        const uint8_t *s = segments + k * 220;
        uint16_t first, last;
        memcpy(&first, s + 0x1c, 2);
        memcpy(&last, s + 0x1e, 2);
        std::vector<Point> pts;
        for (uint32_t n = first; n < last && n < nodeCount; ++n) {
            Point pt;
            memcpy(&pt, nodes + n * 24, 8);
            pts.push_back(pt);
        }
        if (!pts.empty())
            byOrder[Read32(s + 0x14)] = std::move(pts);
    }
    // Chain in race order, orienting each segment to continue from the previous one; the first
    // segment is oriented towards the second.
    for (auto it = byOrder.begin(); it != byOrder.end(); ++it) {
        std::vector<Point> pts = it->second;
        if (m_points.empty()) {
            auto next = std::next(it);
            if (next != byOrder.end() && pts.size() > 0) {
                const Point &n0 = next->second.front(), &n1 = next->second.back();
                float toFront = std::min(Dist(pts.front().x, pts.front().y, n0.x, n0.y),
                                         Dist(pts.front().x, pts.front().y, n1.x, n1.y));
                float toBack = std::min(Dist(pts.back().x, pts.back().y, n0.x, n0.y),
                                        Dist(pts.back().x, pts.back().y, n1.x, n1.y));
                if (toFront < toBack)
                    std::reverse(pts.begin(), pts.end());
            }
        } else {
            const Point &last = m_points.back();
            if (Dist(last.x, last.y, pts.back().x, pts.back().y) < Dist(last.x, last.y, pts.front().x, pts.front().y))
                std::reverse(pts.begin(), pts.end());
        }
        m_points.insert(m_points.end(), pts.begin(), pts.end());
    }
    if (m_points.size() < 2)
        return false;
    m_dist.resize(m_points.size());
    for (size_t i = 1; i < m_points.size(); ++i)
        m_dist[i] = m_dist[i - 1] + Dist(m_points[i - 1].x, m_points[i - 1].y, m_points[i].x, m_points[i].y);
    const Point &a = m_points.back(), &b = m_points.front();
    float closing = Dist(a.x, a.y, b.x, b.y);
    m_closed = closing < 30.0f;
    m_length = m_dist.back() + (m_closed ? closing : 0.0f);
    printf("[Route] %zu segments, %zu points, %.0f m, %s\n", segmentCount, m_points.size(), m_length,
           m_closed ? "closed" : "open");
    return true;
}

void RaceRoute::Reverse()
{
    if (m_points.size() < 2)
        return;
    std::reverse(m_points.begin(), m_points.end());
    for (size_t i = 1; i < m_points.size(); ++i)
        m_dist[i] = m_dist[i - 1] + Dist(m_points[i - 1].x, m_points[i - 1].y, m_points[i].x, m_points[i].y);
    // m_length is direction independent (the closing edge on a loop is the same either way).
}

float RaceRoute::Delta(float a, float b) const
{
    float d = b - a;
    if (m_closed) {
        d = std::fmod(d, m_length);
        if (d > m_length * 0.5f)
            d -= m_length;
        if (d <= -m_length * 0.5f)
            d += m_length;
    }
    return d;
}

float RaceRoute::Project(float x, float y, float hint, float window) const
{
    const size_t n = m_points.size();
    const size_t edges = m_closed ? n : n - 1;
    float best = 1e30f, bestS = 0;
    for (size_t i = 0; i < edges; ++i) {
        const Point &a = m_points[i], &b = m_points[(i + 1) % n];
        float s0 = m_dist[i];
        float ex = b.x - a.x, ey = b.y - a.y, len2 = ex * ex + ey * ey;
        float elen = std::sqrt(len2);
        // Keep only edges whose distance *range* [s0, s0+elen] is within the window. Comparing the
        // hint against the edge start alone drops long edges that contain the hint (the drag strip's
        // ~240 m edges made progress jump a whole edge, ISSUES #115).
        if (hint >= 0) {
            float gap;
            if (m_closed) {
                auto rel = [&](float s) {
                    float d = std::fmod(s - hint, m_length);
                    return d < 0 ? d + m_length : d;
                };
                float ra = rel(s0), rb = rel(s0 + elen);
                gap = ra <= rb ? std::min(ra, m_length - rb) : 0.0f;
            } else {
                float s1 = s0 + elen;
                gap = hint < s0 ? s0 - hint : hint > s1 ? hint - s1 : 0.0f;
            }
            if (gap > window + 40.0f)
                continue;
        }
        float t = len2 > 0 ? std::clamp(((x - a.x) * ex + (y - a.y) * ey) / len2, 0.0f, 1.0f) : 0.0f;
        float px = a.x + ex * t, py = a.y + ey * t;
        float d = Dist(x, y, px, py);
        if (d < best) {
            best = d;
            bestS = s0 + t * elen;
        }
    }
    if (hint >= 0 && best > 1e29f)
        return Project(x, y, -1.0f);
    return bestS;
}

void RaceRoute::Sample(float s, float &x, float &y, float &dx, float &dy) const
{
    const size_t n = m_points.size();
    if (m_closed) {
        s = std::fmod(s, m_length);
        if (s < 0)
            s += m_length;
    } else {
        s = std::clamp(s, 0.0f, m_dist.back());
    }
    size_t i = std::upper_bound(m_dist.begin(), m_dist.end(), s) - m_dist.begin();
    i = i == 0 ? 0 : i - 1;
    const Point &a = m_points[i];
    const Point &b = m_points[(i + 1 < n) ? i + 1 : (m_closed ? 0 : i)];
    float len = Dist(a.x, a.y, b.x, b.y);
    float t = len > 0 ? (s - m_dist[i]) / len : 0.0f;
    x = a.x + (b.x - a.x) * t;
    y = a.y + (b.y - a.y) * t;
    dx = len > 0 ? (b.x - a.x) / len : 1.0f;
    dy = len > 0 ? (b.y - a.y) / len : 0.0f;
}
