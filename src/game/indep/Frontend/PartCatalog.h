#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// UG2's visual car-part catalogue (GLOBALB 0x80034602, the same version-6 pack tools/carparts.py
// dumps - ISSUES #134).  A "part" here is a shop item: the car type says which slot it fills
// (SPOILER, ROOF, EXHAUST, PAINT, ...), the part id is the slot's shop id (12 = spoiler), and the
// record's group/level byte carries the price tier (level) that 0x0026b0c8 feeds to the price table.
//
// Order matters: the shop lists a car type's records in pack order, and the universal geometry
// files name their solids SPOILER_STYLE01..40 in that same order (#181/#182), with the carbon
// variant of every style collected after the 40 painted ones (CARBONFIBRE attribute = 1).
struct CarPart {
    std::string name;    // the shop label, e.g. "JORGEN", "MYSTIC", "ROCKER 5"
    uint8_t partId = 0;  // shop slot id (12 = spoiler, 7 = brakes, 9 = roof scoop, ...)
    uint8_t group = 0;   // record byte +5, low 5 bits
    uint8_t level = 0;   // record byte +5, high 3 bits: the price tier (0..4)
    bool carbon = false; // has the CARBONFIBRE attribute (0xef9931bb) set
    uint16_t style = 0;  // 1-based position among the same type and finish: the model's _STYLE%02d
};

class PartCatalog {
public:
    // Parse the part pack out of a loaded GLOBALB bundle (or the 0x80034602 payload directly).
    bool Parse(const uint8_t *bundle, size_t size);

    // GLOBALB chunk 0x80034a10 holds the car-shop price table (subchunk 0x34a1c, 40-byte entries).
    // Same table PaintCatalog reads; kept separate because the two index it differently.
    bool ParsePrices(const uint8_t *bundle, size_t size);

    bool Empty() const { return m_parts.empty(); }
    size_t Count(uint32_t typeHash) const;
    // Records for a car type, in pack (= shop) order.  typeHash is bStringHash of the type's folder
    // name, e.g. bStringHash("SPOILER").
    const std::vector<CarPart> &Parts(uint32_t typeHash) const;

    // 0x002704d8: the static part-id -> price-category table (DAT_00516840), transcribed whole.
    static uint32_t PriceCategory(uint32_t partId);
    // 0x00270888: the category for a record.  The CARBONFIBRE attribute - the very 0x721aff7c
    // FUN_00270888 tests for - moves body parts to their carbon category (spoiler 0xc -> 0x4a,
    // roof 9 -> 0x4b, body kit 0x20 -> 0x45); the paint ids are PaintCatalog's business.
    static uint32_t PriceCategory(const CarPart &part);
    // 0x0026b0c8 / 0x00270228: the price of a part at its tier.  Level 0 has no price (the one
    // exception, mirrors, is handled by PaintCatalog for the paint cards), and level 4 - the top
    // tier of a few body parts - also returns 0 as the original does.
    uint16_t Price(const CarPart &part) const;
    size_t PriceEntryCount() const { return m_priceTable.size() / 40; }

private:
    std::map<uint32_t, std::vector<CarPart>> m_parts; // car type hash -> records in pack order
    std::vector<uint8_t> m_priceTable;                // 0x34a1c: 40-byte entries, entry-1 index
};
