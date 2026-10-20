#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// UG2's paint/color catalog: GLOBALB's repacked car part database (chunk 0x80034602, the same
// version-6 pack tools/carparts.py reads - ISSUES.md #134).  A color is a car-part record whose
// 0xebb03e66 attribute identifies its color set; the RGB comes from the RED/GREEN/BLUE attributes
// and the finish from GLOSS.
//
// The paint shop's parts map to sets through 0x002bedd8 (PaintCatalog::SetForPart):
//   0x3f Base Paint, 0x45 Spoiler, 0x4b Roof Scoop, 0x4c Mirrors -> GLOSS  (PAINT)
//   0x44 Engine Accents                                      -> HOSES  (PAINT)
//   0x47 Muffler Tip                                         -> MUFFLERS (PAINT)
//   0x49 Rims, 0x4a Spinners                                 -> RIMS   (PAINT_RIM)
//   0x46 Brakes                                              -> CALIPERS (PAINT_BRAKE)
// The Base Paint card can also switch between three finishes with the shoulder buttons
// (0x002be5f0): GLOSS -> METAL -> PEARL, each a separate set in the same pack.
struct PaintColor {
    std::string name; // e.g. GLOSS_L1_COLOR01
    uint8_t r = 0, g = 0, b = 0;
    uint8_t gloss = 128;
};

class PaintCatalog {
public:
    // 0xebb03e66 values (the finish's bStringHash, e.g. bStringHash("GLOSS")).
    static constexpr uint32_t kGloss = 0x2daab07;
    static constexpr uint32_t kMetal = 0x3437a52;
    static constexpr uint32_t kPearl = 0x3797533;
    static constexpr uint32_t kHoses = 0x16bdce;
    static constexpr uint32_t kMufflers = 0x66350f0;
    static constexpr uint32_t kVinyl = 0x3e871f1;
    static constexpr uint32_t kRims = 0xda27;
    static constexpr uint32_t kBrakes = 0xd6640dff;

    // 0x002bedd8: color set for a paint-shop part id (0 = the part is not colorable).
    static uint32_t SetForPart(uint32_t partId);
    static bool IsPaintSet(uint32_t set);
    static const char *SetName(uint32_t set); // finish label, e.g. "Gloss", "Metallic", "Powder Coat"

    // Parse every known paint set out of a loaded GLOBALB bundle.  Returns false when the pack or
    // all sets are absent; the catalog then stays empty and the port keeps its stock paint.
    bool Parse(const uint8_t *bundle, size_t size);

    bool Empty() const { return m_sets.empty(); }
    bool Has(uint32_t set) const;
    size_t Count(uint32_t set) const;
    const PaintColor &At(uint32_t set, size_t index) const;
    const std::vector<PaintColor> &Colors(uint32_t set) const;

private:
    std::map<uint32_t, std::vector<PaintColor>> m_sets; // catalog order == swatch order
};
