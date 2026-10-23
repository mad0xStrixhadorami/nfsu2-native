#pragma once
// The Graphics Shop's vinyl catalogue for one car (ISSUES #223): designs come from the texture
// names in CARS\<NAME>\VINYLS.BIN (<CAR>_<CATEGORY>_<NUMBER>), grouped as the shop's VINYL_GROUP_*
// icon cards.  Single designs are the base textures (no A/B/C suffix); a "set" is the A/B/C pieces
// of one design.
#include <cstdint>
#include <string>
#include <vector>

#include "game/indep/Texture/TexturePack.h"

struct VinylDesign {
    std::string name;    // texture name without the car prefix: "WILD_172" (a set: "WILD_172A" etc.)
    std::string display; // "Wild 172"
    int group = 0;       // index into VinylCatalog::Groups()
    bool set = false;    // true: the A / B / C pieces together
    int pieces = 1;      // 1 for a single design, 2..3 for a set
    int level = 1;       // price tier 1..3
};

struct VinylGroup {
    const char *debug;   // "FLAMES"
    uint32_t icon;       // the front-end texture VINYL_GROUP_<debug>
    uint32_t label;      // language label of the group (0 = none known)
    bool set;            // the "_SET" variant of the category
};

class VinylCatalog {
public:
    // Builds the lists from a car's vinyl pack (the pack's texture names).
    bool Build(const TexturePack &pack, const std::string &carName);
    static const std::vector<VinylGroup> &Groups();
    const std::vector<VinylDesign> &Designs() const { return m_designs; }
    // Designs of one group, in catalogue order.
    std::vector<const VinylDesign *> InGroup(int group) const;
    // Groups that have at least one design.
    std::vector<int> UsedGroups() const;
    // Shop price of a design (stand-in tiers, see ISSUES #223).
    static uint32_t Price(const VinylDesign &d);

private:
    std::vector<VinylDesign> m_designs;
};
