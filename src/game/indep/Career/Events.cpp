#include "game/indep/Career/Events.h"

#include <cstdio>
#include <cstring>

#include "game/bWare/bChunk.h"

namespace Career {
namespace {

constexpr uint32_t kCareerChunks = 0x80034a10; // container registered by 0x00264db8
constexpr uint32_t kEventChunk = 0x00034a11;   // the 224-record event table
constexpr size_t kEventRecord = 0x88;

uint32_t U32(const uint8_t *p, size_t off)
{
    uint32_t v;
    memcpy(&v, p + off, 4);
    return v;
}

int16_t S16(const uint8_t *p, size_t off)
{
    int16_t v;
    memcpy(&v, p + off, 2);
    return v;
}

} // namespace

bool EventTable::Load(const uint8_t *bundle, size_t size)
{
    m_events.clear();
    if (!bundle || size < 8)
        return false;
    const bChunk *container = bFindChunk(bundle, bundle + size, kCareerChunks);
    if (!container) {
        fprintf(stderr, "[Career] GLOBALB: no 0x%08x career chunk\n", kCareerChunks);
        return false;
    }
    const bChunk *table =
        bFindChunk(container->Data(), container->Data() + container->size, kEventChunk);
    if (!table || table->size == 0 || table->size % kEventRecord != 0) {
        fprintf(stderr, "[Career] GLOBALB: bad 0x%08x event table\n", kEventChunk);
        return false;
    }

    const uint8_t *p = table->Data();
    const size_t count = table->size / kEventRecord; // 0x00263720: param_2 / 0x88
    m_events.reserve(count);
    for (size_t i = 0; i < count; ++i, p += kEventRecord) {
        Event e;
        memcpy(e.raw, p, 0x88);
        e.id = U32(p, 0x08);
        e.purse = U32(p, 0x30);
        e.respect = U32(p, 0x14);
        e.stage = p[0x0f];
        e.region = p[0x37];
        e.kind = p[0x34];
        e.special = p[0x0d] == 1;
        e.listed = p[0x7f] != 0;
        const uint8_t races = p[0x7e];
        for (uint8_t r = 0; r < races && r < 3; ++r)
            e.races.push_back(S16(p, 0x18 + r * 4));
        m_events.push_back(std::move(e));
    }
    size_t series = 0, withRaces = 0;
    for (const Event &e : m_events) {
        if (e.kind == 3)
            ++series;
        if (!e.races.empty())
            ++withRaces;
    }
    printf("[Career] 0x34a11: %zu events (%zu race series, %zu with a race list)\n", m_events.size(),
           series, withRaces);
    return true;
}

const Event *EventTable::FindByRace(int race) const
{
    for (const Event &e : m_events)
        for (int16_t r : e.races)
            if (r == race)
                return &e;
    return nullptr;
}

const Event *EventTable::FindById(uint32_t id) const
{
    for (const Event &e : m_events)
        if (e.id == id)
            return &e;
    return nullptr;
}

EventTable &EventTable::Get()
{
    static EventTable table;
    return table;
}

} // namespace Career
