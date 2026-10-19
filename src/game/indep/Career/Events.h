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
    std::vector<int16_t> races; // +0x18 race-database numbers (one event runs 1..3 races)
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

    static EventTable &Get();

private:
    std::vector<Event> m_events;
};

} // namespace Career
