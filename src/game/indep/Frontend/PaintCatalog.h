#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// UG2's paint/color catalog: GLOBALB's repacked car part database (chunk 0x80034602, the same
// version-6 pack tools/carparts.py reads - ISSUES.md #134).  A color is a car-part record with part
// id 48 (id 49 is the vinyl set) and the 0xebb03e66 attribute identifying its color set; the RGB
// comes from the RED/GREEN/BLUE attributes and the finish from GLOSS.
//
// The body's "Base Paint" option (0x002bcff8, part id 0x3f) is FUN_002bedd8 -> 0x2daab07, the
// 60-record GLOSS set (GLOSS_L1_COLOR01..10, GLOSS_L2_COLOR01..20, GLOSS_L3_TEST01..30).  The
// 60-swatch grid of UI_Paint.fng (group 0xeefb2050, 3 rows x 20) uses that order.
struct PaintColor {
    std::string name; // e.g. GLOSS_L1_COLOR01
    uint8_t r = 0, g = 0, b = 0;
    uint8_t gloss = 128;
};

class PaintCatalog {
public:
    // Parse the GLOSS (base paint) set out of a loaded GLOBALB bundle.  Returns false when the pack
    // or the set is absent; the catalog then stays empty and the port keeps its stock paint.
    bool Parse(const uint8_t *bundle, size_t size);

    bool Empty() const { return m_colors.empty(); }
    size_t Count() const { return m_colors.size(); }
    const PaintColor &At(size_t index) const { return m_colors[index]; }
    const std::vector<PaintColor> &Colors() const { return m_colors; }

private:
    std::vector<PaintColor> m_colors; // catalog order == swatch order
};
