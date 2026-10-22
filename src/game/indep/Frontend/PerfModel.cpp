#include "game/indep/Frontend/PerfModel.h"

#include <algorithm>
#include <cstring>

namespace PerfModel {

namespace {

// The code's two views of the record: `base` is the untouched car record and `w` the tuned copy,
// both addressed by the byte offsets the original uses (frame = record + 0x118).
struct Frame {
    const uint8_t *base;
    std::vector<float> w; // kFrameSize / 4 floats

    float B(size_t off) const
    {
        float v;
        memcpy(&v, base + kFrameOffset + off, 4);
        return v;
    }
    float &W(size_t off) { return w[off / 4]; }
};

bool Has(const std::vector<uint16_t> &installed, uint16_t id)
{
    return std::find(installed.begin(), installed.end(), id) != installed.end();
}

float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

} // namespace

float Strength(const PerfPackages &packages, const std::vector<uint16_t> &installed, uint32_t category,
               int n, float *out)
{
    for (int k = 0; k < n; ++k)
        out[k] = 0.0f;
    for (uint32_t level = 1; level < 4; ++level) {
        const PerfPackage *pkg = packages.Find(category, level);
        if (!pkg)
            continue;
        for (int k = 0; k < n; ++k)
            for (const PerfPart &part : pkg->parts)
                if (Has(installed, uint16_t(part.id)) && part.stat[k] != -1.0f)
                    out[k] += part.stat[k];
    }
    float sum = 0.0f;
    for (int k = 0; k < n; ++k)
        sum += out[k];
    float mean = sum / float(n);
    if (mean < 0.0f || mean > 1.0f) {
        sum = 0.0f;
        for (int k = 0; k < n; ++k) {
            out[k] = Clamp01(out[k]);
            sum += out[k];
        }
        mean = sum / float(n);
    }
    return mean;
}

void Apply(uint8_t *record, size_t size, const PerfPackages &packages, const std::vector<uint16_t> &installed)
{
    if (!record || size < kRecordSize)
        return;
    // The modifiers write counters into the base block too (0x0031bb50), so work on a copy.
    std::vector<uint8_t> baseCopy(record, record + size);
    Frame f;
    f.base = baseCopy.data();
    f.w.resize(kFrameSize / 4);
    memcpy(f.w.data(), record + kFrameOffset, kFrameSize);
    float out[3];

    // 0x0031baa0 - weight reduction.
    if (float s = Strength(packages, installed, 0, 1, out); s != 0.0f) {
        f.W(0) += f.B(0x344) * s;
        f.W(0x160) += (f.B(0x348) - f.W(0x160)) * s;
        f.W(0x110) *= (f.B(0x340) - 1.0f) * s + 1.0f;
    }
    // 0x0031bb50 - transmission.
    if (float s = Strength(packages, installed, 1, 3, out); s != 0.0f) {
        const int level = int(out[2] * 3.0f + 0.5f);
        if (level >= 1 && level <= 3) {
            const size_t p = 0x310 + size_t(level) * 0x40;
            f.W(0x1b8) = f.B(p + 8);
            f.W(0x1bc) = f.B(p + 12);
            f.W(0x1c0) = f.B(p + 16);
            f.W(0x1b0) = f.B(p);
            f.W(0x1b4) = f.B(p + 4);
            for (int i = 0; i < 8; ++i)
                f.W(0x1d0 + size_t(i) * 4) = f.B(p + 0x20 + size_t(i) * 4);
            // The gear count the original leaves in the block's counter (+0x18): 0 when the third
            // ratio is absent, else one more than the number of present ratios from the fourth on.
            int count = 0;
            if (f.B(p + 0x28) != 0.0f) {
                int index = 2;
                count = 0;
                while (true) {
                    ++index;
                    ++count;
                    if (index > 7)
                        break;
                    if (f.B(p + 0x20 + size_t(index) * 4) == 0.0f)
                        break;
                }
            }
            int bits = count;
            memcpy(&f.W(0x1c8), &bits, 4);
        }
        f.W(0x1cc) += (f.B(0x3ec) - f.W(0x1cc)) * out[0];
        f.W(0x1c4) += (f.B(0x3e4) - f.W(0x1c4)) * out[0];
        f.W(0x1fc) += f.B(0x41c) * out[1];
    }
    // 0x0031bd28 - nitrous.
    if (float s = Strength(packages, installed, 2, 1, out); s != 0.0f) {
        f.W(0x2a0) += (f.B(0x580) - f.W(0x2a0)) * out[0];
        f.W(0x2ac) += f.B(0x58c) - f.W(0x2ac);
    }
    // 0x0031bdb8 - torque curve (category 3).
    if (float s = Strength(packages, installed, 3, 1, out); s != 0.0f) {
        for (int i = 0; i < 9; ++i)
            f.W(0x200 + size_t(i) * 4) += f.B(0x420 + size_t(i) * 4) * s;
        for (int i = 0; i < 3; ++i)
            f.W(0x248 + size_t(i) * 4) += f.B(0x444 + size_t(i) * 4) * s;
    }
    // 0x0031be80 - torque curve (category 4); the original runs it whether or not a part is fitted.
    {
        const float s = Strength(packages, installed, 4, 1, out);
        for (int i = 0; i < 9; ++i) {
            float v = f.W(0x224 + size_t(i) * 4);
            v += (f.B(0x510 + size_t(i) * 4) - v) * s;
            f.W(0x224 + size_t(i) * 4) = v;
            f.W(0x200 + size_t(i) * 4) += v;
        }
    }
    // 0x0031bf18 - chassis.
    if (float s = Strength(packages, installed, 5, 3, out); s != 0.0f) {
        for (int k = 0; k < 2; ++k) {
            const size_t p = k == 0 ? 0x5c0 : 0x5e0;
            const size_t i8 = size_t(k) * 0x20;
            auto blend = [&](size_t off, float target, float by) {
                f.W(off) += (target - f.W(off)) * by;
            };
            blend(0x18c + i8, f.B(p + 28), out[0]);
            blend(0x170 + i8, f.B(p), out[0]);
            blend(0x174 + i8, f.B(p + 4), out[0]);
            for (int j = 0; j < 2; ++j) {
                blend(0x184 + i8 + size_t(j) * 4, f.B(p + 20 + size_t(j) * 4), out[0]);
                blend(0x178 + i8 + size_t(j) * 4, f.B(p + 8 + size_t(j) * 4), out[0]);
            }
            blend(0x180 + i8, f.B(p + 16), out[1]);
        }
        for (size_t o = 0; o < 0x60; o += 0x30)
            f.W(o + 0x18) += f.B(0x604) * out[0];
        for (size_t o = 0x60; o < 0xc0; o += 0x30)
            f.W(o + 0x18) += f.B(0x608) * out[0];
        f.W(8) -= f.B(0x600) * out[0];
        f.W(0x270) += (f.B(0x5bc) - f.W(0x270)) * out[2];
    }
    // 0x0031c138 - tyre values.
    if (float s = Strength(packages, installed, 6, 1, out); s != 0.0f) {
        f.W(0x27c) += (f.B(0x5b8) - f.W(0x27c)) * s;
        f.W(0x278) += (f.B(0x5b4) - f.W(0x278)) * s;
        f.W(0x274) += (f.B(0x5b0) - f.W(0x274)) * s;
    }
    // 0x0031c1e0 - engine.
    {
        const float s = Strength(packages, installed, 7, 1, out);
        if (Has(installed, 0x2d) || Has(installed, 0x2e) || Has(installed, 0x33)) {
            const int zero = 0;
            memcpy(&f.W(0x290), &zero, 4);
        }
        if (s != 0.0f) {
            // FUN_0031a4b8 rounds the stored level (the mean x 3, clamped).
            const int level = int(Clamp01(s) * 3.0f + 0.5f);
            if (level >= 1 && level <= 3) {
                const size_t p = 0x410 + size_t(level) * 0x40;
                f.W(0x1f0) += f.B(p);
                f.W(0x1f4) += f.B(p + 4);
                f.W(0x1f8) += f.B(p + 8);
            }
            for (int i = 0; i < 9; ++i)
                f.W(0x200 + size_t(i) * 4) += f.B(0x4e0 + size_t(i) * 4) * s;
            for (int i = 0; i < 3; ++i)
                f.W(0x248 + size_t(i) * 4) += f.B(0x504 + size_t(i) * 4) * s;
        }
    }
    // 0x0031c358 - per-wheel values.
    if (float s = Strength(packages, installed, 8, 1, out); s != 0.0f) {
        for (int i = 0; i < 2; ++i) {
            const size_t p = i == 0 ? 0x540 : 0x560;
            const size_t o = size_t(i) * 0x20;
            f.W(0xd8 + o) += (f.B(p + 8) - f.W(0xd8 + o)) * s;
            f.W(0xd4 + o) += (f.B(p + 4) - f.W(0xd4 + o)) * s;
            f.W(0xd0 + o) += (f.B(p) - f.W(0xd0 + o)) * s;
            f.W(0xe0 + o) += (f.B(p + 16) - f.W(0xe0 + o)) * s;
            f.W(0xdc + o) += (f.B(p + 12) - f.W(0xdc + o)) * s;
        }
    }
    // 0x0031c478 - handling packs.
    if (float s = Strength(packages, installed, 9, 2, out); s != 0.0f) {
        f.W(0x264) += (f.B(0x5a0) + f.B(0x590)) * out[0];
        f.W(0x268) += (f.B(0x5a4) + f.B(0x594)) * out[1];
        f.W(0x26c) += (f.B(0x5a8) + f.B(0x598)) * s;
    }
    memcpy(record + kFrameOffset, f.w.data(), kFrameSize);
}

} // namespace PerfModel
