#include "game/indep/World/CarPartAnim.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstring>

float CarPartAnims::Clip::At(float t) const
{
    if (angle.empty())
        return 0.0f;
    const float k = std::clamp(t * 30.0f, 0.0f, float(angle.size() - 1));
    const size_t i = std::min(size_t(k), angle.size() - 1), j = std::min(i + 1, angle.size() - 1);
    return angle[i] + (angle[j] - angle[i]) * (k - float(i));
}

// Container: chunks {u32 id, u32 size, payload}; id 0x00e34010 carries a relocatable ELF whose .data is the bank.  The bank holds its
// name (an upper-case string a few bytes in) and a run of 16-byte unit quaternions, one per frame; the scalar part is the component
// that is 1.0 in the first key, the axis the largest of the others.
bool CarPartAnims::Load(const std::vector<uint8_t> &f)
{
    m_clips.clear();
    memset(m_haveHinge, 0, sizeof(m_haveHinge));
    size_t o = 0;
    while (o + 8 <= f.size()) {
        uint32_t id, sz;
        memcpy(&id, &f[o], 4), memcpy(&sz, &f[o + 4], 4);
        if (o + 8 + sz > f.size() || sz == 0)
            break;
        const uint8_t *p = &f[o + 8];
        if (id == 0xe34010) {
            Clip c;
            for (size_t i = 0; i + 8 < sz; ++i) // the bank name: "<CAR>_ZAM_<PART>_q"
                if (memcmp(p + i, "ZAM_", 4) == 0) {
                    size_t s = i;
                    while (s > 0 && ((p[s - 1] >= 'A' && p[s - 1] <= 'Z') || (p[s - 1] >= '0' && p[s - 1] <= '9') || p[s - 1] == '_'))
                        --s;
                    size_t e = i;
                    while (e < sz && p[e] != 0)
                        ++e;
                    c.name.assign(reinterpret_cast<const char *>(p + s), e - s);
                    break;
                }
            std::vector<std::array<float, 4>> keys;
            for (size_t off = 0; off + 16 <= sz && keys.empty(); off += 4) {
                std::vector<std::array<float, 4>> run;
                for (size_t k = off; k + 16 <= sz; k += 16) {
                    std::array<float, 4> q;
                    memcpy(q.data(), p + k, 16);
                    const float n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
                    if (std::fabs(n2 - 1.0f) > 2e-3f || !(std::fabs(q[0]) + std::fabs(q[1]) + std::fabs(q[2]) + std::fabs(q[3]) > 0.0f))
                        break;
                    run.push_back(q);
                }
                if (run.size() >= 8)
                    keys = std::move(run);
            }
            if (!keys.empty() && !c.name.empty()) {
                int wi = 0;
                for (int i = 1; i < 4; ++i)
                    if (std::fabs(keys[0][i]) > std::fabs(keys[0][wi]))
                        wi = int(i);
                int axis = -1;
                float best = 0.0f;
                for (const auto &q : keys)
                    for (int i = 0; i < 4; ++i)
                        if (i != wi && std::fabs(q[i]) > best)
                            best = std::fabs(q[i]), axis = i;
                for (const auto &q : keys) {
                    float v = 0.0f;
                    for (int i = 0; i < 4; ++i)
                        if (i != wi)
                            v += q[i] * q[i];
                    float a = 2.0f * std::atan2(std::sqrt(v), std::fabs(q[wi]));
                    c.angle.push_back(a * (axis >= 0 && q[axis] < 0 ? -1.0f : 1.0f));
                }
                m_clips.push_back(std::move(c));
            }
        }
        if (id == 0x37100 && sz >= 0x18 + 13 * 80 + 64) {
            // group k (k = 0, 4, 8, 12) starts at +0x18 + k * 80; its translation row is the last row of the 4x4 matrix
            static const struct { int entry, part; } kMap[] = {{0, 1}, {12, 3}, {8, 4}};
            for (const auto &m : kMap) {
                memcpy(m_hinge[m.part], p + 0x18 + size_t(m.entry) * 80 + 48, 12);
                m_haveHinge[m.part] = true;
            }
        }
        o += 8 + sz;
    }
    return !m_clips.empty();
}

const CarPartAnims::Clip *CarPartAnims::Find(const char *part) const
{
    std::string want = std::string("ZAM_") + part + "_q";
    for (const Clip &c : m_clips)
        if (c.name.size() >= want.size() && c.name.compare(c.name.size() - want.size(), want.size(), want) == 0)
            return &c;
    return nullptr;
}

bool CarPartAnims::Hinge(int part, float out[3]) const
{
    if (part < 1 || part > 4 || !m_haveHinge[part])
        return false;
    memcpy(out, m_hinge[part], 12);
    return true;
}
