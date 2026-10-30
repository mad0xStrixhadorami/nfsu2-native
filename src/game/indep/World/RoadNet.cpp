#include "game/indep/World/RoadNet.h"

#include <cmath>
#include <cstring>

namespace {
uint32_t R32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
} // namespace

bool RoadNet::Load(const std::vector<uint8_t> &routes)
{
    m_sections.clear();
    if (routes.size() < 16 || R32(routes.data()) != 0x34121)
        return false;
    const size_t end = std::min<size_t>(routes.size(), 8 + R32(routes.data() + 4));
    for (size_t p = 8; p + 0x8c <= end;) {
        int16_t count;
        memcpy(&count, routes.data() + p + 0x34, 2);
        if (count < 0 || p + 0x8c + size_t(count) * 0x38 > end)
            break;
        Section s;
        uint16_t id;
        memcpy(&id, routes.data() + p + 10, 2);
        s.id = id;
        memcpy(&s.length, routes.data() + p + 0x40, 4);
        for (int k = 0; k < count; ++k) {
            float xy[2];
            memcpy(xy, routes.data() + p + 0x8c + size_t(k) * 0x38, 8);
            s.x.push_back(xy[0]);
            s.y.push_back(xy[1]);
        }
        p += 0x8c + size_t(count) * 0x38;
        if (s.x.size() >= 2)
            m_sections.push_back(std::move(s));
    }
    // a section's end meets the start of the sections that continue it
    for (size_t i = 0; i < m_sections.size(); ++i)
        for (size_t j = 0; j < m_sections.size(); ++j)
            if (i != j && std::hypot(m_sections[i].x.back() - m_sections[j].x.front(), m_sections[i].y.back() - m_sections[j].y.front()) < 60.0f)
                m_sections[i].next.push_back(int(j));
    return !m_sections.empty();
}
