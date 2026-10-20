#include "game/indep/Career/Sponsors.h"

#include <cstdio>
#include <cstring>

#include "game/bWare/bChunk.h"

namespace Career {
namespace {

constexpr uint32_t kCareerChunks = 0x80034a10;
constexpr uint32_t kAreaChunk = 0x00034a18;
constexpr uint32_t kSponsorChunk = 0x00034a19;
constexpr uint32_t kStringChunk = 0x00034a1d;

uint16_t U16(const uint8_t *p, size_t off)
{
    uint16_t v;
    memcpy(&v, p + off, 2);
    return v;
}

uint32_t U32(const uint8_t *p, size_t off)
{
    uint32_t v;
    memcpy(&v, p + off, 4);
    return v;
}

} // namespace

bool SponsorTable::Load(const uint8_t *bundle, size_t size)
{
    m_sponsors.clear();
    m_areas.clear();
    if (!bundle || size < 8)
        return false;
    const bChunk *container = bFindChunk(bundle, bundle + size, kCareerChunks);
    if (!container)
        return false;
    const uint8_t *begin = container->Data(), *end = container->Data() + container->size;
    const bChunk *recs = bFindChunk(begin, end, kSponsorChunk);
    const bChunk *strs = bFindChunk(begin, end, kStringChunk);
    const bChunk *areas = bFindChunk(begin, end, kAreaChunk);
    if (!recs || !strs || !areas || recs->size % 0x10 != 0 || areas->size % 0x50 != 0)
        return false;

    const uint8_t *pool = strs->Data();
    for (size_t i = 0; i < recs->size / 0x10; ++i) {
        const uint8_t *r = recs->Data() + i * 0x10;
        Sponsor s;
        const uint16_t nameOff = U16(r, 0);
        if (nameOff < strs->size)
            s.name.assign(reinterpret_cast<const char *>(pool + nameOff),
                          strnlen(reinterpret_cast<const char *>(pool + nameOff), strs->size - nameOff));
        s.racePurse = U16(r, 2);
        memcpy(s.flags, r + 4, 3);
        s.hash = U32(r, 8);
        s.signingBonus = U16(r, 0xc);
        s.extra = U16(r, 0xe);
        m_sponsors.push_back(std::move(s));
    }
    for (size_t i = 0; i < areas->size / 0x50; ++i) {
        const uint8_t *r = areas->Data() + i * 0x50;
        SponsorArea a;
        a.count = r[1] > 5 ? 5 : r[1];
        for (int k = 0; k < a.count; ++k) {
            a.hashes.push_back(U32(r, 8 + k * 4));
            a.thresholds.push_back(U16(r, 0x1c + k * 2));
        }
        m_areas.push_back(std::move(a));
    }
    printf("[Career] sponsors: %zu records, %zu areas\n", m_sponsors.size(), m_areas.size());
    return true;
}

const Sponsor *SponsorTable::Find(uint32_t hash) const
{
    for (const Sponsor &s : m_sponsors)
        if (s.hash == hash)
            return &s;
    return nullptr;
}

std::vector<const Sponsor *> SponsorTable::Offers(int area, int completed) const
{
    std::vector<const Sponsor *> out;
    const SponsorArea *a = Area(area);
    if (!a)
        return out;
    for (int i = 0; i < a->count; ++i)
        if (a->thresholds[size_t(i)] <= completed)
            if (const Sponsor *s = Find(a->hashes[size_t(i)]))
                out.push_back(s);
    return out;
}

SponsorTable &SponsorTable::Get()
{
    static SponsorTable table;
    return table;
}

} // namespace Career
