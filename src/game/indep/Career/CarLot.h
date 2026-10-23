#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Career {

// The Car Lot's unlock table (ISSUES #250). GLOBALB's career container 0x80034a10 holds 0x34a1f: 24 records of
// 12 bytes (parser 0x00264cd0, lookup 0x00264d38) {u32 bStringHash(car folder), u32 event id (variant A), u32
// event id (variant B)}. 0x00268dd0 starts with every car available and drops each listed car whose event
// (variant B when the region flag 0x002dbae0 is non-zero, as on the PAL disc) is not complete (0x00268bd8):
// the cars come from winning Underground Racing League events (kind +0x34 == 2), not from money.
struct CarLotEntry {
    uint32_t carHash = 0;
    uint32_t eventA = 0, eventB = 0;
};

class CarLotTable {
public:
    bool Load(const uint8_t *bundle, size_t size); // decompressed GLOBALB
    bool Loaded() const { return m_loaded; }
    const std::vector<CarLotEntry> &Entries() const { return m_entries; }
    // 0x00268dd0 for one car: unlisted cars are always on the lot; a listed car needs its event done.
    bool Available(const std::string &folder, const std::vector<uint32_t> &completed) const;
    // The event that unlocks `folder` (0 = none, always available).
    uint32_t UnlockEvent(const std::string &folder) const;
    static CarLotTable &Get();

private:
    std::vector<CarLotEntry> m_entries;
    bool m_loaded = false;
};

} // namespace Career
