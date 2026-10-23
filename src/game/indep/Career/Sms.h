#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "game/indep/Career/Events.h"
#include "game/indep/Career/Profile.h"

namespace Career {

// The career SMS messages (ISSUES #249). GLOBALB's career container 0x80034a10 holds:
//   0x34a17  167 message definitions of 0x14 bytes (parser 0x0026d3d8, lookup by name hash 0x0026d480):
//            {u16 nameOffset into the 0x34a1d string pool, u8 type (+2), u8 category (+3), u8 popup (+4),
//             u8 icon (+5), u16 +6, u32 param (+8), u32 param2 (+0xc), u32 +0x10}
//   0x34a1b  28 triggers of 0x18 bytes (parser 0x002647b8): {u16 nameOffset, u16 locatorOffset, s16 area,
//            u8 kind (0 = car show, 2 = unique part event), u8 value, u32 message-name hash, u32 media id,
//            u8 length, u8 sub-kind / part category ...}
// The type (+2) is the rule that delivers the message (0x0026d510 / 0x0026d620):
//   1  the completed event's id is one of the two ids in +8 / +0xc
//   9  the area's (param >> 16) sponsor events done >= param & 0xffff
//   10 the area's URL events done >= param & 0xffff
//   12 the area's ordinary events done >= param & 0xffff
//   14 the DVD cover whose star threshold equals param was won (0x0026ac88)
//   others (2 / 3 / 4 / 5 / 6 / 13 / 15) are sent by name from the tutorial state machines, the Outrun
//   code and the free-roam triggers, which the port does not have.
// The category (+3) picks the mailbox tab: 1 Inbox, 2 Game Tips, 3 Special Events, 4 Rachel, 5 Unlocks.
struct SmsDef {
    std::string name;
    uint32_t hash = 0; // bStringHash(name)
    uint8_t type = 0;
    uint8_t category = 0;
    uint8_t popup = 0;
    uint8_t icon = 0;
    uint32_t param = 0;
    uint32_t param2 = 0;
    uint32_t sender = 0; // +0x10 language hash of the "From:" name (0x0026d708)
};

struct SmsTrigger {
    std::string name;      // STAGE_3_SHOW_1
    int area = 0;
    uint8_t kind = 0;      // 0 car show, 2 unique part event
    uint8_t value = 0;     // the area's completed-event count (kind 0) the message is sent at
    uint32_t message = 0;  // hash of the definition name
    uint32_t media = 0;    // 0x34a16 record id (+0x20)
    uint8_t subKind = 0;   // +0x14 (1 visual, 2 performance for kind 2)
    uint8_t part = 0;      // +0x15
};

class SmsTable {
public:
    bool Load(const uint8_t *bundle, size_t size); // decompressed GLOBALB
    bool Loaded() const { return !m_defs.empty(); }
    const std::vector<SmsDef> &Defs() const { return m_defs; }
    const std::vector<SmsTrigger> &Triggers() const { return m_triggers; }
    const SmsDef *Find(uint32_t hash) const; // 0x0026d480
    static SmsTable &Get();

private:
    std::vector<SmsDef> m_defs;
    std::vector<SmsTrigger> m_triggers;
};

// The mailbox tabs (0x001f1928): label language hashes of tab 1..5 (index 0 is "Misc").
uint32_t SmsTabLabel(int tab);

// 0x0026dda8 / 0x0026de58: append a message once (false when it is already there or the box is full).
bool SmsDeliver(Profile &p, const SmsDef &def, uint32_t bodyKey = 0, uint32_t subjKey = 0, uint32_t arg = 0);
// 0x0026da78: deliver the definition named `hash` if it exists.
bool SmsDeliverByHash(Profile &p, uint32_t hash);

// An event was completed in `ev.region` (0x00268a30 -> 0x0026e530 for the unlock rules, -> 0x0026ee90 for
// the car-show triggers). Returns the number of messages delivered.
int SmsOnEventComplete(Profile &p, const Event &ev);
// A DVD cover with star threshold `threshold` was won (0x0026ac88 -> 0x0026e5c8).
int SmsOnDvdCover(Profile &p, int threshold);

// The language keys of an entry's subject and body (0x0026d718 / 0x0026d820). `fmtArg` is the language hash of
// the string substituted into the key's %s (0 = none).
struct SmsText {
    uint32_t subjectKey = 0, bodyKey = 0, arg = 0;
};
SmsText SmsTextFor(const Profile::InboxEntry &e);

// Mailbox counters (0x0026e158): unread entries (not deleted) and the first of them (0x0026e0d8).
int SmsUnreadCount(const Profile &p);
Profile::InboxEntry *SmsFirstUnread(Profile &p);
// 0x0026e3c0 / 0x0026e450: opened / deleted.
void SmsMarkRead(Profile::InboxEntry &e);
void SmsDelete(Profile::InboxEntry &e);
// 0x0026e1e8: opening the mailbox files the engage messages under their category tab.
void SmsFileAll(Profile &p);
// 0x001f21d8: does entry `e` show under tab `tab` (1..5)?
bool SmsVisibleInTab(const Profile::InboxEntry &e, int tab);

} // namespace Career
