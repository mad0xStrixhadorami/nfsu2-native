#pragma once
// Israeli signage generated at texture-upload time (ISSUES #277): the American neon, brand and speed-limit textures of the
// world are replaced by Hebrew neon words, generic Israeli shop signs and Israeli speed-limit signs.  The replacement keeps
// the original's aspect ratio but is rendered at a higher resolution.
#include <cstdint>
#include <string>
#include <vector>

namespace SignGen {
bool Init();
// If `name` (the original texture's name, upper case) has a replacement, fills `rgba` (R in the low byte, alpha in the high byte)
// for a w x h picture and returns true; w and h are in/out (they may grow).
bool Replacement(const std::string &name, int &w, int &h, std::vector<uint32_t> &rgba);
// A flat sign picture for the props: kind 0 shop sign, 1 street name sign, 2 flag; variant picks the wording / colours.
bool Make(int kind, int variant, int w, int h, std::vector<uint32_t> &rgba);
int ShopVariants();
int StreetVariants();
} // namespace SignGen
