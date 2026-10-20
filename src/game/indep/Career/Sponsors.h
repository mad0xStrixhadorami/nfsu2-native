#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Career {

// Career sponsors (ISSUES #165). GLOBALB's career container 0x80034a10 holds:
//   0x34a19  20 sponsor records of 0x10 bytes (parser 0x002645f8): {u16 nameOffset into the 0x34a1d
//            string pool (0x00264728), u16 race purse (+2, "Sponsor Race Purse"), u8 x3 (+4),
//            u32 bStringHash(name) at +8, u16 signing bonus (+0xc, "Signing Bonus"), u16 +0xe}
//   0x34a18  6 area records of 0x50 (parser 0x00264918): +1 sponsor count, +8 five sponsor name
//            hashes (resolved to records by 0x00264548), +0x1c five u16 thresholds
// An area offers its sponsors once the player has completed enough ordinary races in it; the game
// then signs one at random (0x00266338 / 0x002663d8 / 0x0026bc60) and pays the signing bonus.
struct Sponsor {
    uint32_t hash = 0;      // bStringHash of the name
    std::string name;       // e.g. "STREETGLOW"
    int racePurse = 0;      // +2
    int signingBonus = 0;   // +0xc
    int extra = 0;          // +0xe, not decoded
    uint8_t flags[3] = {};  // +4..+6, not decoded
};

struct SponsorArea {
    int count = 0;                      // +1
    std::vector<uint32_t> hashes;       // +8 (count entries)
    std::vector<int> thresholds;        // +0x1c
};

class SponsorTable {
public:
    bool Load(const uint8_t *bundle, size_t size); // decompressed GLOBALB
    bool Loaded() const { return !m_sponsors.empty(); }
    const Sponsor *Find(uint32_t hash) const;
    const SponsorArea *Area(int area) const { return area >= 0 && area < int(m_areas.size()) ? &m_areas[size_t(area)] : nullptr; }

    // Sponsors area `area` offers after `completed` ordinary races there (0x002663d8's count).
    std::vector<const Sponsor *> Offers(int area, int completed) const;

    static SponsorTable &Get();

private:
    std::vector<Sponsor> m_sponsors;
    std::vector<SponsorArea> m_areas;
};

} // namespace Career
