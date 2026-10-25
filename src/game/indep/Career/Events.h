#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Career {

// The career event table: GLOBALB chunk 0x34a11 (inside the container 0x80034a10, loader 0x00264db8,
// parser 0x00263720 / 0x002637c0). 224 records of 0x88 bytes. The field offsets come from the code
// that reads the table:
//   0x00263568  looks a record up by +0x08 (the completion key)
//   0x002635d8  finds the record whose race list (+0x18, count at +0x7e) contains a race number
//   0x001ee590  the world map's event list: stage +0x0f, race ids +0x18, region +0x37, completion
//               +0x08, purse/prize are read elsewhere (0x00267958 / 0x002673d0 use +0x30, +0x34)
//   0x0025a168  +0x0d == 1 (the special entries)
//   0x00267128  +0x7f is a filter byte
// The stage is 1..5 for the career events; the special entries (24/29/81/82) are not stage-bound.
struct Event {
    uint32_t id = 0;      // +0x08 completion key (0x00268bd8 tests it against the profile)
    uint32_t purse = 0;   // +0x30 prize money ("Purse")
    uint32_t respect = 0; // +0x14 "EarnableRespect" (100 on almost every record)
    uint8_t stage = 0;    // +0x0f EVENT TYPE (0 circuit, 1 sprint, 2 street X, 3 outrun, 4 drag, 5 drift; 0x0025e850), not a career stage
    uint8_t region = 0;   // +0x37 city/URL group (0..5)
    uint8_t kind = 0;     // +0x34 3 = race series (154 records), 1/2 = the other event types
    bool special = false; // +0x0d
    bool listed = false;  // +0x7f
    uint32_t locator = 0;       // +0x28 bStringHash of the free-roam locator that starts it (EV_nn, EVENT_LOCATOR_nn; ISSUES #303)
    uint32_t prereq = 0;        // +0x10 (a stage number r-1 for the region-r events, see EventAvailable)
    uint8_t raw[0x88] = {};     // the record as stored (fields not decoded yet)
    std::vector<int16_t> races; // +0x18 race-database numbers (one event runs 1..3 races)
    // The flag byte of each +0x18 slot ({s16 race, u8 flags, u8}): bit 0 is the reverse direction
    // (0x00263848) and bit 1 marks the race as unlocked from the start (0x00263858). Parallel to
    // races; used by the Quick Race track list (ISSUES #157).
    std::vector<uint8_t> raceFlags;

    // The direction the slot that runs `race` is authored with (0x00263848: flag bit 0).
    uint8_t DirectionFor(int race) const
    {
        for (size_t i = 0; i < races.size() && i < raceFlags.size(); ++i)
            if (races[i] == race)
                return raceFlags[i] & 1;
        return 0;
    }
};

// One 0x34a16 media record (0x40 bytes, ISSUES #246): 30 per game.  `dvd` records (flag +0x35) are the DVD
// covers that gate a stage (0x00266618: the stage is entered only when all of them are won, 0x002666c0); the
// others are magazine pictures.  `threshold` (+0x3c) is the Visual Rating a car needs to be featured on the
// cover (0x0026ac88 unlocks a record of the current stage when 0x00267e48's rating reaches it).
struct MediaRecord {
    char name[0x20] = {};
    uint32_t id = 0;       // +0x20 completion id (the profile's completed-event list)
    uint32_t nameHash = 0; // +0x28 language hash of the cover's magazine (the SMS_MAG_* arguments, #249)
    uint8_t type = 0;      // +0x24
    uint8_t stage = 0;     // +0x25 (2..5)
    bool dvd = false;      // +0x35
    float threshold = 0.0f; // +0x3c star rating
};

// The parsed 0x34a11 table. It is static game data: load it once from the GLOBALB bundle.
class EventTable {
public:
    bool Load(const uint8_t *bundle, size_t size); // the decompressed GLOBALB BUN
    bool Loaded() const { return !m_events.empty(); }
    size_t Count() const { return m_events.size(); }
    const Event &At(size_t i) const { return m_events[i]; }
    const std::vector<Event> &All() const { return m_events; }
    const Event *FindByRace(int race) const; // 0x002635d8
    const Event *FindById(uint32_t id) const; // 0x00263568

    const std::vector<MediaRecord> &Media() const { return m_media; }
    static EventTable &Get();

    // Career progression (ISSUES #152). An event in region r >= 2 whose +0x10 byte is r-1 (the other
    // bytes zero) needs stage r-1 cleared; region 1's events need the career start event 0xdd60e403
    // (+0x10); region 0 is free. Stage 1 is cleared when all of region 1's events are done, stage k
    // >= 2 when the gate event of the stage record (0x34a18 +0x28) is done (the DVD-cover requirements
    // of 0x34a16 are not tracked yet). The start events 0xdd60e402/3 count as done: the port has no
    // career intro.
    bool EventAvailable(const Event &e, const std::vector<uint32_t> &done) const;
    bool StageCleared(int stage, const std::vector<uint32_t> &done) const;
    bool StageGateDone(int stage, const std::vector<uint32_t> &done) const; // the gate event alone (no DVD covers)
    bool AreaAvailable(int area, const std::vector<uint32_t> &done) const; // world map tab

    // Quick Race track lock (0x0026b438, called through 0x00271a90 flags bit 0). A race is playable
    // when any 0x34a11 event that runs it either marks the slot as unlocked from the start (bit 1 of
    // the slot's flag byte, 0x00263858) or is complete in the profile (0x00268bd8).
    bool TrackUnlocked(int race, const std::vector<uint32_t> &done) const;

private:
    std::vector<Event> m_events;
    std::vector<MediaRecord> m_media;
};

} // namespace Career
