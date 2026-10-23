#pragma once
// The Rims shop (ISSUES #226): brands, styles and the files that hold their wheel models.  Wheel
// models live in CARS\WHEELS\GEOMETRY_<KEY>.BIN, one file per brand with solids named
// <BRAND>_STYLE<NN>_<rim inches>_<outer-diameter inches>_<A|B|C> (A / B / C are three levels of
// detail: 503 / 204 / 93 triangles); the artwork is in CARS\WHEELS\TEXTURES.BIN as
// <BRAND>_STYLE<NN>_WHEEL (+ _WHEEL_INNER_MASK).
#include <cstdint>
#include <string>
#include <vector>

#include "game/indep/Texture/TexturePack.h"

struct RimBrand {
    const char *key;     // geometry file suffix and texture / solid prefix, "" = stock
    const char *geometry; // GEOMETRY_<geometry>.BIN
    const char *display;
    uint32_t icon;       // VISUAL_RIMS_BRAND_<...> front-end texture
};

class RimCatalog {
public:
    static const std::vector<RimBrand> &Brands();
    // Reads the style numbers of every brand from the wheel texture pack.
    bool Built() const { return m_built; }
    bool Build(const TexturePack &wheelTextures);
    // Styles (1-based numbers) of a brand, ascending.
    const std::vector<int> &Styles(size_t brand) const { return m_styles[brand]; }
    static std::string GeometryPath(const RimBrand &b);
    // Shop price of a style (stand-in tiers, see ISSUES #226).
    static uint32_t Price(size_t brand, int style);

private:
    bool m_built = false;
    std::vector<std::vector<int>> m_styles = std::vector<std::vector<int>>(16);
};
