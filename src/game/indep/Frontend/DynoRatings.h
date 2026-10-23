#pragma once
// The Acceleration / Top Speed / Handling bars of the Dyno Results screen (ISSUES #215):
// 0x0031ce90 / 0x0031cdd8 / 0x0031d060 normalise the car's frame values with the limits of GLOBALB
// chunk 0x34a32 (30 floats, copied to 0x0051da68).
#include <cstddef>
#include <cstdint>

#include "game/indep/World/PlayerCar.h"

namespace DynoRatings {

constexpr uint32_t kLimitsChunk = 0x34a32;
constexpr size_t kLimitCount = 30;

struct Limits {
    float v[kLimitCount] = {};
    bool valid = false;
    // Finds chunk 0x34a32 in the 0x80034a10 container.
    bool Parse(const uint8_t *container, size_t size);
};

struct Ratings {
    float acceleration = 0, topSpeed = 0, handling = 0; // 0.03 .. 1
};

// `record` is a car record (0x2b0-byte frame at +0x118), `p` its drivetrain figures, `wheelRadius` in m.
Ratings Compute(const Limits &limits, const uint8_t *record, const PlayerCar::Performance &p, float wheelRadius);

} // namespace DynoRatings
