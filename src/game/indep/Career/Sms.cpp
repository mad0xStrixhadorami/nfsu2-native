#include "game/indep/Career/Sms.h"

#include <cstdio>
#include <cstring>

#include "game/bWare/bChunk.h"
#include "game/indep/World/CarMaterial.h"

namespace Career {
namespace {

constexpr uint32_t kCareerChunks = 0x80034a10;
constexpr uint32_t kDefChunk = 0x00034a17;
constexpr uint32_t kTriggerChunk = 0x00034a1b;
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

std::string PoolString(const bChunk *pool, size_t off)
{
    if (off >= pool->size)
        return std::string();
    const char *s = reinterpret_cast<const char *>(pool->Data() + off);
    return std::string(s, strnlen(s, pool->size - off));
}

uint32_t Hash(const std::string &s)
{
    return CarMaterial::NameHash(s.c_str());
}

std::string Format(const char *fmt, int n)
{
    char buf[96];
    snprintf(buf, sizeof(buf), fmt, n);
    return buf;
}

// 0x002660a0 / 0x002660d0 / 0x00266108: the area object's three completion lists. 0x002661f0 files an
// event by its +0x34 kind: 3 (not special) -> the 35-slot ordinary list, 1 -> 4 sponsor slots, 2 -> 10 URL slots.
int DoneCount(const Profile &p, int area, int kind)
{
    int n = 0;
    for (const Event &e : EventTable::Get().All()) {
        if (e.region != area || e.kind != kind)
            continue;
        if (kind == 3 && e.special)
            continue;
        if (p.Completed(e.id))
            ++n;
    }
    return n;
}

bool HasPrefix(const std::string &name, const char *prefix)
{
    return name.compare(0, strlen(prefix), prefix) == 0;
}

} // namespace

bool SmsTable::Load(const uint8_t *bundle, size_t size)
{
    m_defs.clear();
    m_triggers.clear();
    if (!bundle || size < 8)
        return false;
    const bChunk *container = bFindChunk(bundle, bundle + size, kCareerChunks);
    if (!container)
        return false;
    const uint8_t *begin = container->Data(), *end = container->Data() + container->size;
    const bChunk *defs = bFindChunk(begin, end, kDefChunk);
    const bChunk *trig = bFindChunk(begin, end, kTriggerChunk);
    const bChunk *pool = bFindChunk(begin, end, kStringChunk);
    if (!defs || !pool || defs->size % 0x14 != 0)
        return false;
    for (size_t i = 0; i < defs->size / 0x14; ++i) {
        const uint8_t *r = defs->Data() + i * 0x14;
        SmsDef d;
        d.name = PoolString(pool, U16(r, 0));
        d.hash = Hash(d.name);
        d.type = r[2];
        d.category = r[3];
        d.popup = r[4];
        d.icon = r[5];
        d.param = U32(r, 8);
        d.param2 = U32(r, 0xc);
        d.sender = U32(r, 0x10);
        m_defs.push_back(std::move(d));
    }
    if (trig && trig->size % 0x18 == 0)
        for (size_t i = 0; i < trig->size / 0x18; ++i) {
            const uint8_t *r = trig->Data() + i * 0x18;
            SmsTrigger t;
            t.name = PoolString(pool, U16(r, 0));
            t.area = int16_t(U16(r, 4));
            t.kind = r[6];
            t.value = r[7];
            t.message = U32(r, 8);
            t.media = U32(r, 0xc);
            t.subKind = r[0x14];
            t.part = r[0x15];
            m_triggers.push_back(std::move(t));
        }
    printf("[Career] sms: %zu messages, %zu triggers\n", m_defs.size(), m_triggers.size());
    return true;
}

const SmsDef *SmsTable::Find(uint32_t hash) const
{
    for (const SmsDef &d : m_defs)
        if (d.hash == hash)
            return &d;
    return nullptr;
}

SmsTable &SmsTable::Get()
{
    static SmsTable table;
    return table;
}

uint32_t SmsTabLabel(int tab)
{
    switch (tab) {
    case 0: return 0xb0819356; // Misc
    case 1: return 0xc06e138a; // Inbox
    case 2: return 0xf8cb19a3; // Game Tips
    case 3: return 0x06f8e5bf; // Special Events
    case 4: return 0xe2442059; // Rachel
    case 5: return 0xea33e5b6; // Unlocks
    default: return 0;
    }
}

bool SmsDeliver(Profile &p, const SmsDef &def, uint32_t bodyKey, uint32_t subjKey, uint32_t arg)
{
    if (p.HasMessage(def.hash) || p.inbox.size() >= Profile::kInboxMax)
        return false;
    Profile::InboxEntry e;
    e.def = def.hash;
    e.bodyKey = bodyKey;
    e.subjKey = subjKey;
    e.arg = arg;
    p.inbox.push_back(e);
    printf("[Sms] delivered %s (%zu in the box)\n", def.name.c_str(), p.inbox.size());
    return true;
}

bool SmsDeliverByHash(Profile &p, uint32_t hash)
{
    const SmsDef *d = SmsTable::Get().Find(hash);
    return d && SmsDeliver(p, *d);
}

int SmsOnEventComplete(Profile &p, const Event &ev)
{
    const SmsTable &table = SmsTable::Get();
    int sent = 0;
    // 0x0026e530: every definition whose rule the finished event satisfies (0x0026d510).
    for (const SmsDef &d : table.Defs()) {
        bool met = false;
        const int area = int(d.param >> 16), count = int(d.param & 0xffff);
        switch (d.type) {
        case 1:
            met = d.param == ev.id || d.param2 == ev.id;
            break;
        case 9:
            met = DoneCount(p, area, 1) >= count;
            break;
        case 10:
            met = DoneCount(p, area, 2) >= count;
            break;
        case 12:
            met = DoneCount(p, area, 3) >= count;
            break;
        default:
            break;
        }
        if (met && SmsDeliver(p, d))
            ++sent;
    }
    // 0x0026ee90: car-show triggers of the area fire when its ordinary-event count reaches their value.
    const int done = DoneCount(p, ev.region, 3);
    for (const SmsTrigger &t : table.Triggers()) {
        if (t.kind != 0 || t.area != ev.region || t.value != done)
            continue;
        const SmsDef *d = table.Find(t.message);
        if (!d)
            continue;
        // 0x0026dae0: the generic magazine text alternates between two variants and names the magazine
        // of the media record (+0x28).
        const int variant = int(p.inbox.size() % 2) + 1;
        uint32_t arg = 0;
        for (const MediaRecord &m : EventTable::Get().Media())
            if (m.id == t.media)
                arg = m.nameHash;
        if (SmsDeliver(p, *d, Hash(Format("SMS_MAG_GENERIC_BODY_%d", variant)),
                       Hash(Format("SMS_MAG_GENERIC_SUBJ_%d", variant)), arg))
            ++sent;
    }
    return sent;
}

int SmsOrdinaryDone(const Profile &p, int area)
{
    return DoneCount(p, area, 3);
}

int SmsOnDvdCover(Profile &p, int threshold)
{
    int sent = 0;
    for (const SmsDef &d : SmsTable::Get().Defs())
        if (d.type == 14 && int(d.param) == threshold && SmsDeliver(p, d))
            ++sent;
    return sent;
}

SmsText SmsTextFor(const Profile::InboxEntry &e)
{
    SmsText t;
    const SmsDef *d = SmsTable::Get().Find(e.def);
    if (!d)
        return t;
    if (e.subjKey != 0 || e.bodyKey != 0) { // 0x0026d718 / 0x0026d820: generic text with a %s argument
        t.subjectKey = e.subjKey;
        t.bodyKey = e.bodyKey;
        t.arg = e.arg;
        return t;
    }
    // The car-unlock messages share one text; ENGAGE_TIPS_AUTO and the rest use <name>_SUBJECT / _BODY.
    if (HasPrefix(d->name, "SMS_CAR_UNLOCK_")) {
        t.subjectKey = Hash("SMS_CAR_UNLOCK_1_SUBJECT");
        t.bodyKey = Hash("SMS_CAR_UNLOCK_1_BODY");
    } else {
        t.subjectKey = Hash(d->name + "_SUBJECT");
        t.bodyKey = Hash(d->name + "_BODY");
    }
    return t;
}

int SmsUnreadCount(const Profile &p)
{
    int n = 0;
    for (const Profile::InboxEntry &e : p.inbox)
        if (!e.read && !e.deleted)
            ++n;
    return n;
}

Profile::InboxEntry *SmsFirstUnread(Profile &p)
{
    for (Profile::InboxEntry &e : p.inbox)
        if (!e.read && !e.deleted)
            return &e;
    return nullptr;
}

void SmsMarkRead(Profile::InboxEntry &e)
{
    e.read = true;
}

void SmsDelete(Profile::InboxEntry &e)
{
    e.deleted = true;
    e.read = true;
}

void SmsFileAll(Profile &p)
{
    for (Profile::InboxEntry &e : p.inbox)
        if (const SmsDef *d = SmsTable::Get().Find(e.def))
            if (d->popup == 1)
                e.filed = true;
}

bool SmsVisibleInTab(const Profile::InboxEntry &e, int tab)
{
    if (e.deleted)
        return false;
    if (!e.filed)
        return tab == 1;
    const SmsDef *d = SmsTable::Get().Find(e.def);
    return d && d->category == tab;
}

} // namespace Career
