#include "game/indep/Frontend/PerfPackages.h"

#include <cstring>

namespace {

constexpr uint32_t kPackageChunk = 0x34a15;
constexpr size_t kPackageSize = 0x17c;
constexpr size_t kPartSize = 0x5c;
constexpr size_t kMaxParts = 4; // (0x17c - 0xc) / 0x5c

uint32_t U32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

float F32(const uint8_t *p)
{
    float v;
    memcpy(&v, p, 4);
    return v;
}

} // namespace

uint32_t PerfPackage::TotalPrice() const
{
    uint32_t sum = 0;
    for (const PerfPart &p : parts)
        sum += p.price;
    return sum;
}

bool PerfPackages::Parse(const uint8_t *container, size_t size)
{
    m_packages.clear();
    for (size_t off = 0; off + 8 <= size;) {
        const uint32_t id = U32(container + off);
        const size_t len = U32(container + off + 4);
        if (off + 8 + len > size)
            break;
        if (id == kPackageChunk) {
            const uint8_t *data = container + off + 8;
            for (size_t r = 0; r + kPackageSize <= len; r += kPackageSize) {
                const uint8_t *rec = data + r;
                PerfPackage pkg;
                pkg.category = U32(rec);
                pkg.level = U32(rec + 4);
                const uint32_t count = U32(rec + 8);
                for (uint32_t i = 0; i < count && i < kMaxParts; ++i) {
                    const uint8_t *p = rec + 0xc + i * kPartSize;
                    PerfPart part;
                    part.id = U32(p);
                    part.nameHash = U32(p + 4);
                    part.price = U32(p + 8);
                    part.kind = U32(p + 12);
                    for (int k = 0; k < 8; ++k)
                        part.tags[k] = U32(p + 0x10 + k * 4);
                    for (int k = 0; k < 4; ++k)
                        part.stat[k] = F32(p + 0x30 + k * 4);
                    pkg.parts.push_back(part);
                }
                m_packages.push_back(pkg);
            }
        }
        off += 8 + len;
    }
    return !m_packages.empty();
}

const PerfPackage *PerfPackages::Find(uint32_t category, uint32_t level) const
{
    for (const PerfPackage &p : m_packages)
        if (p.category == category && p.level == level)
            return &p;
    return nullptr;
}

const PerfPackage *PerfPackages::FindByPart(uint32_t partId) const
{
    for (const PerfPackage &p : m_packages)
        for (const PerfPart &part : p.parts)
            if (part.id == partId)
                return &p;
    return nullptr;
}
