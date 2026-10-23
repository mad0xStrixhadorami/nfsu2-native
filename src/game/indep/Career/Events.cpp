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
        e.prereq = U32(p, 0x10);
        e.purse = U32(p, 0x30);
        e.respect = U32(p, 0x14);
        e.stage = p[0x0f];
        e.region = p[0x37];
        e.kind = p[0x34];
        e.special = p[0x0d] == 1;
        e.listed = p[0x7f] != 0;
        const uint8_t races = p[0x7e];
        for (uint8_t r = 0; r < races && r < 3; ++r) {
            e.races.push_back(S16(p, 0x18 + r * 4));
            e.raceFlags.push_back(p[0x1a + r * 4]); // +2 of the 4-byte slot
        }
        m_events.push_back(std::move(e));
    }
    // The media (DVD cover / magazine) records of the same container.
    m_media.clear();
    if (const bChunk *media = bFindChunk(container->Data(), container->Data() + container->size, 0x00034a16))
        for (size_t o = 0; o + 0x40 <= media->size; o += 0x40) {
            const uint8_t *r = media->Data() + o;
            MediaRecord m;
            memcpy(m.name, r, 0x1f);
            m.id = U32(r, 0x20);
            m.type = r[0x24];
            m.stage = r[0x25];
            m.dvd = r[0x35] != 0;
            memcpy(&m.threshold, r + 0x3c, 4);
            m_media.push_back(m);
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

namespace Career {

namespace {
constexpr uint32_t kStartEventA = 0xdd60e402, kStartEventB = 0xdd60e403;
// 0x34a18 +0x28 of stage records 2..5.
constexpr uint32_t kStageGate[6] = {0, 0, 0x1185ef08, 0x5e802bce, 0xab7a688e, 0x04cce3e1};

bool Done(uint32_t id, const std::vector<uint32_t> &done)
{
    if (id == kStartEventA || id == kStartEventB)
        return true;
    for (uint32_t d : done)
        if (d == id)
            return true;
    return false;
}
} // namespace

bool EventTable::StageCleared(int stage, const std::vector<uint32_t> &done) const
{
    if (stage <= 0)
        return true;
    if (stage == 1) {
        for (const Event &e : m_events)
            if (e.region == 1 && !Done(e.id, done))
                return false;
        return true;
    }
    if (stage > 5)
        return false;
    // 0x002666c0: the stage's gate event must be done and every DVD cover of the stage won (0x00266618).
    if (!Done(kStageGate[stage], done))
        return false;
    for (const MediaRecord &m : m_media)
        if (m.dvd && m.stage == stage && !Done(m.id, done))
            return false;
    return true;
}

bool EventTable::StageGateDone(int stage, const std::vector<uint32_t> &done) const
{
    return stage >= 2 && stage <= 5 && Done(kStageGate[stage], done);
}

bool EventTable::EventAvailable(const Event &e, const std::vector<uint32_t> &done) const
{
    if (e.region >= 2 && e.prereq == uint32_t(e.region - 1))
        return StageCleared(e.region - 1, done);
    return true; // region 0, region 1 (start event, implicit) and the other layouts
}

bool EventTable::AreaAvailable(int area, const std::vector<uint32_t> &done) const
{
    return area <= 1 || StageCleared(area - 1, done);
}

// 0x0026b438: walk every 0x34a11 event that runs this race (0x002635d8); the race is unlocked if
// any of them passes 0x0026b3a8 -- the slot's flag bit 1 (0x00263858, "starter") or the event is
// complete (0x00268bd8). The original returns false only when it runs out of events without a pass.
bool EventTable::TrackUnlocked(int race, const std::vector<uint32_t> &done) const
{
    for (const Event &e : m_events)
        for (size_t i = 0; i < e.races.size(); ++i)
            if (e.races[i] == race) {
                const uint8_t flags = i < e.raceFlags.size() ? e.raceFlags[i] : 0;
                if ((flags & 2) != 0 || Done(e.id, done))
                    return true;
            }
    return false;
}

} // namespace Career
