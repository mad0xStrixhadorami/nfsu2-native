#include "game/indep/Frontend/RimCatalog.h"

#include <algorithm>
#include <cstdlib>

namespace {
// The brands that have a VISUAL_RIMS_BRAND_* card in FRONTB (the shop's brand list).  OZ's models are
// in GEOMETRY_P9.BIN.
const std::vector<RimBrand> kBrands = {
    {"", "", "Stock", 0x0f3990b6},
    {"BBS", "BBS", "BBS", 0xf224ab29},
    {"ENKEI", "ENKEI", "Enkei", 0x0e38de9e},
    {"GIANELLE", "GIANELLE", "Gianelle", 0xbe399df3},
    {"KONIG", "KONIG", "Konig", 0x0ea60b4a},
    {"LEXANI", "LEXANI", "Lexani", 0xe50d0313},
    {"MOMO", "MOMO", "Momo", 0x36c04f4a},
    {"NFSU", "NFSU", "NFSU", 0x36c0b62e},
    {"OASIS", "OASIS", "Oasis", 0x0ee6d551},
    {"OZ", "P9", "OZ Racing", 0x6c2fa9db},
    {"RACINGHART", "RACINGHART", "Racing Hart", 0x27ebd095},
    {"SPINNER", "SPINNER", "Spinner", 0x0dc9c33a}, // wheel + a spinning hub piece (_SPIN_)
};
} // namespace

const std::vector<RimBrand> &RimCatalog::Brands() { return kBrands; }

std::string RimCatalog::GeometryPath(const RimBrand &b)
{
    return std::string("CARS\\WHEELS\\GEOMETRY_") + b.geometry + ".BIN";
}

bool RimCatalog::Build(const TexturePack &pack)
{
    for (auto &v : m_styles)
        v.clear();
    for (size_t i = 0; i < pack.Count(); ++i) {
        const std::string &n = pack.At(i).name; // names are cut at 24 characters
        const size_t at = n.find("_STYLE");
        if (at == std::string::npos || at + 8 > n.size())
            continue;
        const std::string brand = n.substr(0, at);
        const int style = atoi(n.c_str() + at + 6);
        if (style <= 0)
            continue;
        for (size_t b = 1; b < kBrands.size(); ++b)
            if (brand == kBrands[b].key && std::find(m_styles[b].begin(), m_styles[b].end(), style) == m_styles[b].end())
                m_styles[b].push_back(style);
    }
    bool any = false;
    for (size_t b = 1; b < kBrands.size(); ++b) {
        std::sort(m_styles[b].begin(), m_styles[b].end());
        any = any || !m_styles[b].empty();
    }
    m_built = true;
    return any;
}

uint32_t RimCatalog::Price(size_t brand, int style)
{
    static const uint32_t kTier[3] = {400, 700, 1000}; // stand-in: rim sets cost $400 / $700 / $1000
    return kTier[(brand + size_t(style)) % 3];
}
