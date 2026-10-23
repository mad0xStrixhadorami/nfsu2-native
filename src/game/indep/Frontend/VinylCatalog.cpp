#include "game/indep/Frontend/VinylCatalog.h"

#include <algorithm>
#include <cctype>
#include <map>


namespace {

// VINYL_GROUP_* texture hashes (FRONTB, the Graphics Shop's category cards).
const std::vector<VinylGroup> kGroups = {
    {"FLAMES", 0xf8148554, 0xb3ab0d65, false},         {"FLAMES_SET", 0x4ef4919f, 0x4aa403de, true},
    {"LIGHTNING", 0xf2173020, 0x588a0a91, false},      {"LIGHTNING_S", 0x8ca7c26b, 0x0a16b73d, true},
    {"TRIBAL", 0x192d84da, 0xd4c40ceb, false},         {"TRIBAL_SET", 0x4bede425, 0xd55ad357, true},
    {"STRIPES", 0xf7352706, 0xd2982464, false},        {"STRIPES_SET", 0x4c12bc51, 0xff8de430, true},
    {"WILD", 0x69580c2c, 0xb6db5bfd, false},           {"WILD_SET", 0x31cfc477, 0x3f6da129, true},
    {"TEAR", 0x695654a8, 0xb6d9a479, false},           {"TEAR_SET", 0x2075eaf3, 0x4e6ae825, true},
    {"MODERN", 0x08a07001, 0xc436f812, false},         {"MODERN_SET", 0x43a00acc, 0x687f411e, true},
    {"FLAG", 0x694ec516, 0xb6d214e7, false},           {"FLAG_SET", 0x4eece261, 0x2909a9d3, true},
    {"SPLASH", 0x16b5ce67, 0xd24c5678, false},         {"SPLASH_SET", 0x006a2c32, 0x6e37cdc4, true},
    {"RACING", 0x134c6830, 0xcee2f041, false},         {"RACING_SET", 0x36da827b, 0x7f61feed, true},
    {"ART_FACTORY", 0x0dd7673a, 0x8e076bec, false},    {"TOP_LAYER", 0x0960dcab, 0x6fd3b71c, false},
    {"UNIQUE", 0x1b3a8dd3, 0xd6d115e4, false},         {"CONTEST", 0x1ba508fc, 0x4a0c932d, false},
    {"AFTER_MARKET", 0xe22eb851, 0x64cf99a3, false},   {"HOOD", 0x694fec66, 0x913ac36a, false},
    {"BODY", 0x694ca0ca, 0xb6cff09b, false},
};

int GroupIndex(const std::string &name)
{
    for (size_t i = 0; i < kGroups.size(); ++i)
        if (name == kGroups[i].debug)
            return int(i);
    return -1;
}

// Texture-name category -> shop group (without the "_SET" suffix).
const char *GroupFor(const std::string &category)
{
    static const std::map<std::string, const char *> kMap = {
        {"FLAME", "FLAMES"},   {"LIGHTNG", "LIGHTNING"}, {"TRIBAL", "TRIBAL"}, {"STRIPE", "STRIPES"},
        {"STRIPES", "STRIPES"}, {"WILD", "WILD"},         {"TEAR", "TEAR"},     {"TEARS", "TEAR"},
        {"MODERN", "MODERN"},  {"MODERNS", "MODERN"},   {"FLAG", "FLAG"},     {"FLAGS", "FLAG"},
        {"LIQUID", "SPLASH"},  {"LIQUIDS", "SPLASH"},    {"ART", "ART_FACTORY"}, {"TOP", "TOP_LAYER"},
        {"UNIQUE", "UNIQUE"},  {"CONTEST", "CONTEST"},   {"AEM", "AFTER_MARKET"}, {"HOOD", "HOOD"},
        {"BODY", "BODY"},
    };
    auto it = kMap.find(category);
    return it == kMap.end() ? nullptr : it->second;
}

std::string Title(const std::string &s)
{
    std::string out;
    bool up = true;
    for (char ch : s) {
        if (ch == '_') {
            out.push_back(' ');
            up = true;
        } else {
            out.push_back(up ? char(std::toupper(static_cast<unsigned char>(ch))) : char(std::tolower(static_cast<unsigned char>(ch))));
            up = false;
        }
    }
    return out;
}

} // namespace

const std::vector<VinylGroup> &VinylCatalog::Groups() { return kGroups; }

int VinylCatalog::GroupIndexOf(const char *debug) { return GroupIndex(debug); }

bool VinylCatalog::Build(const TexturePack &pack, const std::string &carName)
{
    m_designs.clear();
    const std::string prefix = carName + "_";
    std::map<std::string, std::vector<char>> pieces; // base name -> piece letters seen
    std::vector<std::string> bases;
    for (size_t i = 0; i < pack.Count(); ++i) {
        const std::string &full = pack.At(i).name;
        if (full.compare(0, prefix.size(), prefix) != 0)
            continue;
        std::string n = full.substr(prefix.size());
        if (n.size() > 5 && n.compare(n.size() - 5, 5, "_MASK") == 0)
            continue; // the silhouette images belong to their colour image
        // The pack's name field holds 24 characters: names are cut ("..._MAS"), skip those masks.
        if (n.size() > 4 && (n.compare(n.size() - 4, 4, "_MAS") == 0 || n.compare(n.size() - 3, 3, "_MA") == 0 ||
                             n.compare(n.size() - 2, 2, "_M") == 0))
            continue;
        // Category = text up to the first '_', the rest a number with an optional piece letter.
        const size_t us = n.find('_');
        if (us == std::string::npos)
            continue;
        const std::string category = n.substr(0, us);
        const char *group = GroupFor(category);
        if (!group)
            continue;
        std::string rest = n.substr(us + 1);
        char piece = 0;
        if (!rest.empty() && rest.back() >= 'A' && rest.back() <= 'C' && rest.size() > 1 && std::isdigit(static_cast<unsigned char>(rest[rest.size() - 2]))) {
            piece = rest.back();
            rest.pop_back();
        }
        const std::string base = category + "_" + rest;
        if (piece)
            pieces[base].push_back(piece);
        else if (std::find(bases.begin(), bases.end(), base) == bases.end()) {
            bases.push_back(base);
        }
    }
    std::vector<std::string> sets;
    for (const auto &kv : pieces)
        if (kv.second.size() >= 2)
            sets.push_back(kv.first);
    auto add = [&](const std::string &base, bool set, int count) {
        const size_t us = base.find('_');
        const char *group = GroupFor(base.substr(0, us));
        std::string gname = group;
        if (set)
            gname += gname == "LIGHTNING" ? "_S" : "_SET";
        int gi = GroupIndex(gname);
        if (gi < 0)
            gi = GroupIndex(group); // a category without a set card
        if (gi < 0)
            return;
        VinylDesign d;
        d.name = base;
        d.display = Title(base);
        d.group = gi;
        d.set = set;
        d.pieces = count;
        d.level = 1 + int(m_designs.size() % 3);
        m_designs.push_back(d);
    };
    std::sort(bases.begin(), bases.end());
    std::sort(sets.begin(), sets.end());
    for (const std::string &b : bases)
        add(b, false, 1);
    for (const std::string &b : sets)
        add(b, true, int(std::min<size_t>(3, pieces[b].size())));
    return !m_designs.empty();
}

std::vector<const VinylDesign *> VinylCatalog::InGroup(int group) const
{
    std::vector<const VinylDesign *> out;
    for (const VinylDesign &d : m_designs)
        if (d.group == group)
            out.push_back(&d);
    return out;
}

std::vector<int> VinylCatalog::UsedGroups() const
{
    std::vector<int> out;
    for (size_t g = 0; g < kGroups.size(); ++g)
        for (const VinylDesign &d : m_designs)
            if (d.group == int(g)) {
                out.push_back(int(g));
                break;
            }
    return out;
}

uint32_t VinylCatalog::Price(const VinylDesign &d)
{
    static const uint32_t kTier[3] = {100, 200, 300}; // the 0x4d..0x54 price entries' L1 / L2 / L3 prices
    return kTier[std::max(1, std::min(3, d.level)) - 1] * (d.set ? uint32_t(d.pieces) : 1u);
}
