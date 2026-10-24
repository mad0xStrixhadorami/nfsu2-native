#include "game/indep/Career/Hotspots.h"

#include <cstring>

#include "game/bWare/bChunk.h"

namespace Career {
namespace {

constexpr uint32_t kCareerChunks = 0x80034a10;
constexpr uint32_t kShopChunk = 0x00034a12;
constexpr size_t kShopRecord = 0xa0;

uint32_t U32(const uint8_t *p, size_t off)
{
    uint32_t v;
    memcpy(&v, p + off, 4);
    return v;
}

float F32(const uint8_t *p, size_t off)
{
    float v;
    memcpy(&v, p + off, 4);
    return v;
}

std::string CStr(const uint8_t *p, size_t max)
{
    return std::string(reinterpret_cast<const char *>(p), strnlen(reinterpret_cast<const char *>(p), max));
}

} // namespace

Hotspot::Kind Hotspot::kind() const
{
    if (scene.rfind("CRIB", 0) == 0)
        return kCrib;
    if (scene.rfind("CARLOT", 0) == 0)
        return kCarLot;
    if (scene.rfind("PARTSSHOP", 0) == 0)
        return kBodyShop;
    if (scene.rfind("PAINTSHOP", 0) == 0)
        return kGraphics;
    if (scene.rfind("PERFSHOP", 0) == 0)
        return kPerformance;
    if (scene.rfind("AUDIOSHOP", 0) == 0)
        return kSpecialty;
    return kOther;
}

bool Hotspot::Contains(float px, float py) const
{
    if (!hasZone)
        return (px - x) * (px - x) + (py - y) * (py - y) < 81.0f;
    // Convex polygon: the point lies on one side of every edge (either winding).
    bool pos = false, neg = false;
    for (int i = 0; i < 4; ++i) {
        const float *a = zone[i], *b = zone[(i + 1) % 4];
        const float c = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
        pos |= c > 0, neg |= c < 0;
    }
    return !(pos && neg);
}

HotspotTable &HotspotTable::Get()
{
    static HotspotTable table;
    return table;
}

bool HotspotTable::LoadShops(const uint8_t *bundle, size_t size)
{
    m_shops.clear();
    if (!bundle || size < 8)
        return false;
    const bChunk *container = bFindChunk(bundle, bundle + size, kCareerChunks);
    if (!container)
        return false;
    const bChunk *recs = bFindChunk(container->Data(), container->Data() + container->size, kShopChunk);
    if (!recs || recs->size % kShopRecord != 0)
        return false;
    for (size_t i = 0; i < recs->size / kShopRecord; ++i) {
        const uint8_t *r = recs->Data() + i * kShopRecord;
        Hotspot h;
        h.name = CStr(r, 0x20);
        h.hash = U32(r, 0x3c);
        h.scene = CStr(r + 0x40, 0x20);
        h.flag = r[0x9c];
        h.area = r[0x9d];
        m_shops.push_back(h);
    }
    return !m_shops.empty();
}

size_t HotspotTable::LoadPositions(const std::vector<uint8_t> &paths)
{
    m_placed.clear();
    if (paths.size() < 16 || U32(paths.data(), 0) != 0x80034147u)
        return 0;
    const bChunk *roads = bFindChunk(paths.data(), paths.data() + paths.size(), 0x3414a);
    if (!roads)
        return 0;
    const uint8_t *d = roads->Data();
    for (size_t j = 0x30; j + 4 <= roads->size; j += 4) {
        const uint32_t key = U32(d, j);
        if (key == 0 || U32(d, j - 0x30) != 14 || j < 0x2c + 20)
            continue;
        for (const Hotspot &s : m_shops) {
            if (s.hash != key)
                continue;
            Hotspot h = s;
            h.x = F32(d, j - 0x2c);
            h.y = F32(d, j - 0x28);
            h.dx = F32(d, j - 0x24);
            h.dy = F32(d, j - 0x20);
            h.z = F32(d, j - 0x1c);
            if (j + 0x14 + 32 <= roads->size) {
                for (int c = 0; c < 4; ++c)
                    h.zone[c][0] = F32(d, j + 0x14 + c * 8), h.zone[c][1] = F32(d, j + 0x18 + c * 8);
                h.hasZone = true;
            }
            m_placed.push_back(h);
            break;
        }
    }
    return m_placed.size();
}

} // namespace Career
