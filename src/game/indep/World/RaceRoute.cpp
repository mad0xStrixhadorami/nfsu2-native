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

uint16_t Read16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

float Dist(float ax, float ay, float bx, float by) { return std::hypot(bx - ax, by - ay); }

struct RoutePoint {
    float x, y;
};

// A 0x34149 road segment. `front`/`back` are the segment indices linked at its two endpoints
// (+0x24/+0x44); the Paths file also contains the other lane of each dual carriageway and the
// roads of neighbouring events, so the links -- not the array order -- define the route.
struct RouteSeg {
    std::vector<RoutePoint> pts;
    std::vector<uint16_t> front;
    std::vector<uint16_t> back;
    float length = 0;
};

float PolylineLength(const std::vector<RoutePoint> &pts)
{
    float len = 0;
    for (size_t i = 1; i < pts.size(); ++i)
        len += Dist(pts[i - 1].x, pts[i - 1].y, pts[i].x, pts[i].y);
    return len;
}

// Depth-first search over the segment links. The race record's length picks one connected
// cycle/path out of the branching lane graph (0x00344fb8 loads the chunks, 0x00345418 links
// the segment tables; the 0x3414a graph itself is the AI route, not the race line).
struct RouteSearch {
    const std::vector<RouteSeg> &segs;
    float target = 0;
    float tolerance = 0;
    int expansions = 0;
    static constexpr int kExpansionLimit = 500000;

    bool have = false;
    float bestScore = 1e30f;
    float bestLength = 0;
    bool bestClosed = false;
    int bestSegments = 0;
    int bestForward = -1;
    std::vector<RoutePoint> bestPts;

    std::vector<char> visited;
    std::vector<RoutePoint> cur;
    int segCount = 0;
    int forwardCount = 0;
    int startSeg = -1;
    int startDir = 1;
    RoutePoint startEntry{0, 0};

    explicit RouteSearch(const std::vector<RouteSeg> &s, float t) : segs(s), target(t)
    {
        tolerance = std::max(40.0f, t * 0.05f);
        cur.reserve(2048);
        bestPts.reserve(2048);
    }

    void Append(int seg, int dir)
    {
        const auto &pts = segs[seg].pts;
        auto add = [&](const RoutePoint &p) {
            if (cur.empty() || Dist(cur.back().x, cur.back().y, p.x, p.y) > 1e-4f)
                cur.push_back(p);
        };
        if (dir == 1) {
            for (const RoutePoint &p : pts)
                add(p);
        } else {
            for (auto it = pts.rbegin(); it != pts.rend(); ++it)
                add(*it);
        }
    }

    void Consider(float length, bool closed, float closingGap)
    {
        const float total = length + (closed ? closingGap : 0.0f);
        if (target > 0) {
            const float diff = std::fabs(total - target);
            if (diff > tolerance && have)
                return;
            if (diff > tolerance * 2.0f)
                return;
            // The segment point order is the forward race direction; a candidate that reverses
            // several segments is usually hopping the central reservation to the other lane.
            // Prefer the all-forward lane unless it misses the race length by a lot.
            const int backward = segCount - forwardCount;
            const float score = diff + backward * 3.0f;
            bool better = !have || score < bestScore - 0.05f;
            if (!better && have && std::fabs(score - bestScore) <= 2.0f) {
                if (forwardCount > bestForward || (forwardCount == bestForward && segCount < bestSegments))
                    better = true;
            }
            if (!better)
                return;
            bestScore = score;
        } else if (!have || total > bestLength) {
            // No race record length: use the longest connected route.
        } else {
            return;
        }
        have = true;
        bestLength = total;
        bestClosed = closed;
        bestSegments = segCount;
        bestForward = forwardCount;
        bestPts = cur;
        if (closed)
            bestPts.push_back(startEntry);
    }

    void Dfs(int seg, int dir, float length)
    {
        if (++expansions > kExpansionLimit)
            return;
        const auto &pts = segs[seg].pts;
        const RoutePoint &exit = dir == 1 ? pts.back() : pts.front();
        const auto &links = dir == 1 ? segs[seg].back : segs[seg].front;
        bool advanced = false;
        for (uint16_t j : links) {
            if (j >= segs.size() || segs[j].pts.empty())
                continue;
            const float dFront = Dist(exit.x, exit.y, segs[j].pts.front().x, segs[j].pts.front().y);
            const float dBack = Dist(exit.x, exit.y, segs[j].pts.back().x, segs[j].pts.back().y);
            const float gap = std::min(dFront, dBack);
            if (gap > 25.0f)
                continue;
            const int nextDir = dFront <= dBack ? 1 : -1;
            if (j == startSeg) {
                if (segCount >= 2 && Dist(exit.x, exit.y, startEntry.x, startEntry.y) <= 25.0f)
                    Consider(length, true, gap);
                advanced = true;
                continue;
            }
            if (visited[j])
                continue;
            const float nextLength = length + gap + segs[j].length;
            if (target > 0 && nextLength > target * 1.25f + 300.0f)
                continue;
            advanced = true;
            visited[j] = 1;
            ++segCount;
            if (nextDir == 1)
                ++forwardCount;
            const size_t oldSize = cur.size();
            Append(j, nextDir);
            Dfs(j, nextDir, nextLength);
            cur.resize(oldSize);
            if (nextDir == 1)
                --forwardCount;
            --segCount;
            visited[j] = 0;
            if (expansions > kExpansionLimit)
                return;
        }
        // A sprint ends at a dead end (or where its remaining links are already used).
        if (!advanced)
            Consider(length, false, 0.0f);
    }

    bool Run()
    {
        visited.assign(segs.size(), 0);
        for (int s = 0; s < int(segs.size()); ++s) {
            if (segs[s].pts.empty())
                continue;
            for (int d : {1, -1}) {
                visited.assign(segs.size(), 0);
                cur.clear();
                segCount = 1;
                forwardCount = d == 1 ? 1 : 0;
                startSeg = s;
                startDir = d;
                startEntry = d == 1 ? segs[s].pts.front() : segs[s].pts.back();
                visited[s] = 1;
                Append(s, d);
                Dfs(s, d, segs[s].length);
                if (expansions > kExpansionLimit)
                    return have;
            }
        }
        return have;
    }
};
} // namespace

bool RaceRoute::Load(const std::vector<uint8_t> &paths, float targetLength)
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

    std::vector<RouteSeg> segs(segmentCount);
    for (size_t k = 0; k < segmentCount; ++k) {
        const uint8_t *s = segments + k * 220;
        const uint16_t first = Read16(s + 0x1c), last = Read16(s + 0x1e);
        auto &pts = segs[k].pts;
        for (uint32_t n = first; n < last && n < nodeCount; ++n) {
            RoutePoint pt;
            memcpy(&pt, nodes + n * 24, 8);
            pts.push_back(pt);
        }
        if (pts.size() < 2)
            continue;
        segs[k].length = PolylineLength(pts);
        // Link counts at +0x22/+0x23; each list holds at most four 8-byte entries.
        const int count[2] = {std::min<int>(s[0x22], 4), std::min<int>(s[0x23], 4)};
        for (int side = 0; side < 2; ++side) {
            auto &dst = side == 0 ? segs[k].front : segs[k].back;
            for (int i = 0; i < count[side]; ++i) {
                const uint16_t next = Read16(s + (side == 0 ? 0x24 : 0x44) + i * 8);
                if (next < segmentCount && next != k)
                    dst.push_back(next);
            }
        }
    }

    RouteSearch search(segs, targetLength);
    const bool found = search.Run();
    if (found) {
        m_closed = search.bestClosed;
        m_points.reserve(search.bestPts.size());
        for (const RoutePoint &p : search.bestPts) {
            Point pt;
            pt.x = p.x;
            pt.y = p.y;
            m_points.push_back(pt);
        }
        m_length = search.bestLength;
    } else {
        // Fallback for a Paths file without usable links: chain the segments by their stored
        // order field, as the port did before the lane graph was decoded.
        std::map<uint32_t, std::vector<Point>> byOrder;
        for (size_t k = 0; k < segmentCount; ++k) {
            const uint8_t *s = segments + k * 220;
            const uint16_t first = Read16(s + 0x1c), last = Read16(s + 0x1e);
            std::vector<Point> pts;
            for (uint32_t n = first; n < last && n < nodeCount; ++n) {
                Point pt;
                memcpy(&pt, nodes + n * 24, 8);
                pts.push_back(pt);
            }
            if (!pts.empty())
                byOrder[Read32(s + 0x14)] = std::move(pts);
        }
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
                if (Dist(last.x, last.y, pts.back().x, pts.back().y) <
                    Dist(last.x, last.y, pts.front().x, pts.front().y))
                    std::reverse(pts.begin(), pts.end());
            }
            m_points.insert(m_points.end(), pts.begin(), pts.end());
        }
    }
    if (m_points.size() < 2)
        return false;

    m_dist.resize(m_points.size());
    for (size_t i = 1; i < m_points.size(); ++i)
        m_dist[i] = m_dist[i - 1] + Dist(m_points[i - 1].x, m_points[i - 1].y, m_points[i].x, m_points[i].y);
    if (found) {
        if (m_closed)
            m_length = m_dist.back();
        else
            m_length = m_dist.back();
    } else {
        const Point &a = m_points.back(), &b = m_points.front();
        const float closing = Dist(a.x, a.y, b.x, b.y);
        m_closed = closing < 30.0f;
        m_length = m_dist.back() + (m_closed ? closing : 0.0f);
    }
    printf("[Route] %zu segments, %zu points, %.0f m, %s%s\n", segmentCount, m_points.size(), m_length,
           m_closed ? "closed" : "open", found ? "" : " (order fallback)");
    if (getenv("NFSU2_ROUTE_DEBUG")) {
        for (size_t i = 0; i < m_points.size(); ++i)
            printf("  p%zu %.1f %.1f\n", i, m_points[i].x, m_points[i].y);
    }
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
