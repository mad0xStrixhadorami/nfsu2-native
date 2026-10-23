#include "game/indep/World/CarSkin.h"

#include <algorithm>
#include <cstdlib>

#include "game/indep/World/CarMaterial.h"

namespace CarSkin {

bool Vinyls::Load(std::vector<uint8_t> bytes, const std::string &carName)
{
    m_pack.reset();
    m_bytes = std::move(bytes);
    m_car = carName;
    auto pack = std::make_unique<TexturePack>();
    if (!pack->Parse(m_bytes.data(), m_bytes.size()))
        return false;
    m_pack = std::move(pack);
    return true;
}

bool Vinyls::Compose(uint8_t paintR, uint8_t paintG, uint8_t paintB, const std::vector<Layer> &layers,
                     std::vector<uint32_t> &rgba) const
{
    rgba.assign(size_t(kSize) * kSize, uint32_t(paintR) | (uint32_t(paintG) << 8) | (uint32_t(paintB) << 16) | 0xff000000u);
    if (!m_pack)
        return layers.empty();
    bool ok = true;
    for (const Layer &layer : layers) {
        const std::string name = m_car + "_" + layer.design;
        const uint32_t hash = CarMaterial::NameHash(name.c_str());
        const TextureInfo *info = nullptr;
        for (size_t i = 0; i < m_pack->Count(); ++i)
            if (m_pack->At(i).nameHash == hash)
                info = &m_pack->At(i);
        DecodedTexture img;
        if (!info || !m_pack->Decode(*info, img) || img.width != kSize || img.height != kSize) {
            ok = false;
            continue;
        }
        for (size_t i = 0; i < rgba.size(); ++i) {
            const uint32_t px = img.rgba[i];
            const int r = int(px & 0xff), g = int((px >> 8) & 0xff), b = int((px >> 16) & 0xff), a = int(px >> 24);
            const int weight = r + g + b;
            if (weight == 0 || a == 0)
                continue;
            // The pixel's colour is the blend of the three region colours by its channel weights;
            // its coverage is how bright the code is (clamped) times the pixel's alpha.
            float c[3] = {0, 0, 0};
            for (int k = 0; k < 3; ++k) {
                const float w = float(k == 0 ? r : k == 1 ? g : b) / float(weight);
                for (int ch = 0; ch < 3; ++ch)
                    c[ch] += w * float(layer.color[k][ch]);
            }
            const float cover = std::min(1.0f, float(weight) / 255.0f) * float(a) / 255.0f;
            // Source-over of the layer (colour c, coverage cover) onto what is there so far.
            const uint32_t dst = rgba[i];
            const float da = float(dst >> 24) / 255.0f;
            const float oa = cover + da * (1.0f - cover);
            float out[3];
            for (int ch = 0; ch < 3; ++ch) {
                const float d = float((dst >> (8 * ch)) & 0xff);
                out[ch] = oa > 0.0f ? (c[ch] * cover + d * da * (1.0f - cover)) / oa : c[ch];
            }
            rgba[i] = uint32_t(out[0] + 0.5f) | (uint32_t(out[1] + 0.5f) << 8) | (uint32_t(out[2] + 0.5f) << 16) |
                      (uint32_t(oa * 255.0f + 0.5f) << 24);
        }
    }
    return ok;
}

} // namespace CarSkin
