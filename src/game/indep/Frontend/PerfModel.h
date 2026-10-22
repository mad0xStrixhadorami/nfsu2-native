#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "game/indep/Frontend/PerfPackages.h"

// UG2's performance-part model: how the parts of the Performance Shop change a car's engine and
// chassis block (ISSUES #200).  The original builds a tuned copy of the car's database record:
// 0x0031d570 -> 0x0031b520 copies 0x2b0 bytes from `record + 0x118` (in game memory the record is
// seen 8 bytes in, so the code's base is `+0x110`) and lets nine modifiers blend that copy toward
// the upgrade tables stored further down the same record:
//   0x0031baa0 category 0   weight reduction (mass, centre, one scale)
//   0x0031bb50 category 1   transmission    (gear set blocks at +0x310 + 0x40 * level, drive ratios)
//   0x0031bd28 category 2   nitrous         (+0x2a0 / +0x2ac)
//   0x0031bdb8 category 3   torque curve    (+0x200 x9 and +0x248 x3 from +0x420 / +0x444)
//   0x0031be80 category 4   torque curve    (+0x224 x9 blended to +0x510, then summed into +0x200)
//   0x0031bf18 category 5   chassis         (suspension blocks +0x5c0 / +0x5e0, four wheel entries)
//   0x0031c138 category 6   tyre values     (+0x274 / +0x278 / +0x27c from +0x5b0..)
//   0x0031c1e0 category 7   engine          (rpm limits from +0x410 + 0x40 * level, torque from +0x4e0)
//   0x0031c358 category 8   per-wheel       (+0xd0.. from +0x540 / +0x560)
//   0x0031c478 category 9   handling packs  (+0x264 / +0x268 / +0x26c from +0x590 / +0x5a0)
// Each category's strength is the sum of the stat floats of its installed parts (0x0031a9e0).
// Not ported: the single-part flags 0x0031b720 applies and the inertia refresh 0x0031e510.
namespace PerfModel {

constexpr size_t kRecordSize = 0x890;
constexpr size_t kFrameOffset = 0x118; // record offset of the code's frame origin
constexpr size_t kFrameSize = 0x2b0;   // bytes the tuned copy carries
constexpr int kCategories = 10;

// 0x0031a9e0: the strength of one category given the installed part ids.  `out` receives the
// per-stat sums (n values, clamped to 0..1 when their mean leaves that range) and the mean is
// returned.
float Strength(const PerfPackages &packages, const std::vector<uint16_t> &installed, uint32_t category,
               int n, float *out);

// Applies the installed parts to a copy of a car record (0x890 bytes), in place.  Call it for every
// car, parts or not: 0x0031be80 runs unconditionally, so even a stock car's torque curve gains the
// record's +0x33c extra curve (the Skyline's peak torque rises from 243 to 299 Nm).
void Apply(uint8_t *record, size_t size, const PerfPackages &packages, const std::vector<uint16_t> &installed);

} // namespace PerfModel
