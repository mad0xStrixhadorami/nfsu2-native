#include "game/indep/World/TrafficNet.h"

#include <cmath>
#include <cstring>
#include <map>
#include <utility>

bool TrafficNet::Load(const std::vector<uint8_t> &paths)
{
    m_links.clear();
    size_t o = 8;
    if (paths.size() < 16)
        return false;
    uint32_t outer;
    memcpy(&outer, &paths[0], 4);
    if (outer != 0x80034147u)
        return false;
    while (o + 8 <= paths.size()) {
        uint32_t id, size;
        memcpy(&id, &paths[o], 4);
        memcpy(&size, &paths[o + 4], 4);
        if (o + 8 + size > paths.size())
            break;
        if (id == 0x3414d) {
            for (size_t r = 0; r + 36 <= size; r += 36) {
                const uint8_t *p = &paths[o + 8 + r];
                Link l{};
                memcpy(&l.x0, p, 16);
                l.laneClass = p[17];
                l.length = std::hypot(l.x1 - l.x0, l.y1 - l.y0);
                l.next = -1;
                m_links.push_back(l);
            }
        }
        o += 8 + size;
    }
    std::map<std::pair<float, float>, int> starts;
    for (size_t i = 0; i < m_links.size(); ++i)
        starts.emplace(std::make_pair(m_links[i].x0, m_links[i].y0), int(i));
    for (Link &l : m_links) {
        auto it = starts.find({l.x1, l.y1});
        if (it != starts.end())
            l.next = it->second;
    }
    return !m_links.empty();
}

void TrafficNet::Sample(int i, float t, float &x, float &y, float &dx, float &dy) const
{
    const Link &l = m_links[size_t(i)];
    float f = l.length > 1e-3f ? t / l.length : 0.0f;
    x = l.x0 + (l.x1 - l.x0) * f;
    y = l.y0 + (l.y1 - l.y0) * f;
    dx = l.length > 1e-3f ? (l.x1 - l.x0) / l.length : 1.0f;
    dy = l.length > 1e-3f ? (l.y1 - l.y0) / l.length : 0.0f;
}
