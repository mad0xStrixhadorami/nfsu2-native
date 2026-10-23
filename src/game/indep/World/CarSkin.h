#pragma once
// The car's paint skin (<CAR>_SKIN1), composed at runtime from the body paint and the vinyl layers
// (ISSUES #223).  UG2 keeps each car's vinyl artwork in CARS\<NAME>\VINYLS.BIN, a texture pack of
// 512 x 512 body-atlas images: a design's colour image is RGB-coded (red, green and blue pixels
// select up to three recolourable regions) and the matching *_MASK image is its silhouette.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "game/indep/Texture/TexturePack.h"
#include "game/indep/World/SolidMesh.h"

namespace CarSkin {

constexpr int kSize = 512;

struct Layer {
    std::string design;       // texture name without the car prefix, e.g. "WILD_172A"
    uint8_t color[3][3] = {}; // RGB of the red / green / blue regions
};

class Vinyls {
public:
    // Takes the bytes of CARS\<NAME>\VINYLS.BIN.
    bool Load(std::vector<uint8_t> bytes, const std::string &carName);
    bool Valid() const { return m_pack != nullptr; }
    size_t Count() const { return m_pack ? m_pack->Count() : 0; }
    const TexturePack *Pack() const { return m_pack.get(); }
    // Fills rgba (kSize * kSize, R in the low byte) with the paint colour and the layers over it.
    // Returns false if a design is missing.
    bool Compose(uint8_t paintR, uint8_t paintG, uint8_t paintB, const std::vector<Layer> &layers,
                 std::vector<uint32_t> &rgba) const;

private:
    std::vector<uint8_t> m_bytes;
    std::unique_ptr<TexturePack> m_pack;
    std::string m_car;
};

// The material slot of the painted body shell: the slot with the most triangles that spans the car's
// length, without the tyre and "<CAR>_SKIN1" (trim shell) slots.  0 if none.
uint32_t DetectBodySlot(const std::vector<SolidMesh::Triangle> &model, const std::vector<uint32_t> &slots,
                        const std::string &carName);

// The planar side projection of the vinyl atlas (ISSUES #223): the left side's profile is the top
// strip, the right side's the bottom strip; car length maps to u 0.043 .. 0.925, height uses the
// same scale from the lowest body point.
struct Projection {
    float x0 = 0, z0 = 0, scale = 1;
    static Projection ForBody(const std::vector<SolidMesh::Triangle> &model, const std::vector<uint32_t> &slots,
                              uint32_t bodySlot);
    void Map(float x, float z, bool leftSide, float &s, float &t) const
    {
        s = 0.043f + (x - x0) * scale;
        const float up = (z - z0) * scale;
        t = leftSide ? up : 0.752f - up;
    }
};

} // namespace CarSkin
