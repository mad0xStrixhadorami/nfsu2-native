#pragma once
// Imported Israeli street / building meshes (ISSUES #336): the .ilm files written by tools/import_il_assets.py
// from the supplied 3dILassets (shikun, tower, bus stop, intersections).  A model is a set of material sub-meshes in
// the game's frame (x east, y north, z up, origin at the ground).  Emit() turns an instance of one into the world's
// render triangles (alpha 254 = prop: the world shader keeps the authored colours).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "game/indep/World/TrackWorld.h"

namespace IlAssets {

struct Material {
    std::string name, tex; // tex = png stem under <dir>/tex, "" = flat colour
    float rgb[3] = {1, 1, 1}, emis[3] = {0, 0, 0}, emisStrength = 1, alpha = 1, metal = 0, rough = 1;
    uint32_t flags = 0; // 1 emissive, 2 blend, 4 two-sided
};
struct Vertex {
    float p[3], n[3], uv[2];
};
// A connected piece of a sub-mesh (one lamp post, one kerb block, one stripe): triangles that share vertices.
struct Component {
    uint32_t sub = 0;
    float c[3] = {0, 0, 0};   // centre of its box (model space)
    float ext[3] = {0, 0, 0}; // box size
    float zLo = 0;
};
struct Sub {
    uint32_t mat = 0;
    std::string part; // the source mesh's last name part: Furniture / Islands / Markings / Signals / Signs, "" for a building
    std::vector<Vertex> v;
    std::vector<uint32_t> idx;
    std::vector<uint32_t> comp; // per vertex: index into Model::comps
};
struct Model {
    std::string name;
    std::vector<Material> mats;
    std::vector<Sub> subs;
    std::vector<Component> comps;
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    size_t Triangles() const;
};

// The asset directory (assets/israel/il, or NFSU2_ASSETS/il); empty when the files are not there.
std::string Dir();
// Loads <name>.ilm once; nullptr when missing or unreadable.  NFSU2_NO_IL_ASSETS=1 turns every model off.
const Model *Get(const std::string &name);
// Registers the textures of `m` on the world (once per world).
void RegisterTextures(TrackWorld &world, const Model &m);

struct Instance {
    float x = 0, y = 0, z = 0; // the model's origin in the world
    float yaw = 0;             // radians about +z (the model's +x turns towards (cos, sin))
    float sx = 1, sy = 1, sz = 1;
};
// Ground draping: z of the surface under (x, y) at or below `zMax`; false = none there.
using GroundFn = std::function<bool(float x, float y, float zMax, float &z)>;
struct EmitOptions;
// Decides per component whether it is emitted (world centre of the component after fitting).
using KeepFn = std::function<bool(const Model &, const Sub &, const Component &, float wx, float wy)>;
struct EmitOptions {
    bool dropBlend = true;      // leave out translucent glass (the world has no per-vertex alpha blending for props)
    const GroundFn *ground = nullptr;
    float drapeBelow = -1.0f;   // model-space z up to which a vertex follows the ground by itself (markings, kerbs, paving)
    float lift = 0.0f;          // added to every vertex (a draped surface sits above the road to avoid z-fighting)
    float anchorSlack = 1.0f;   // a draped vertex searches the ground up to instance z + this
    float lightScale = 1.0f;
    // Rubber-sheet fit (intersections): Instance.sx / sy are the sheet's scales along the model's x / y.  Components smaller than
    // `smallLimit` metres (poles, signal heads, signs, paint dashes) keep their own shape and only their position follows the sheet;
    // larger ones (kerb blocks, medians, stop lines, mast arms) are stretched with it.
    // Model-space height band [cutLo, cutHi] removed from a repeating stack (a tower loses whole floors): what lies above moves down.
    float cutLo = 0, cutHi = 0;
    bool rubber = false;
    float smallLimit = 6.0f;
    const KeepFn *keep = nullptr;
};
// Appends the instance's triangles to `out` and returns how many were added.
size_t Emit(TrackWorld &world, const Model &m, const Instance &inst, const EmitOptions &opt,
            std::vector<TrackWorld::RTriPublic> &out);
// The world-space position of a model-space point under `inst` (no draping).
void ToWorld(const Instance &inst, float mx, float my, float mz, float &wx, float &wy, float &wz);

} // namespace IlAssets
