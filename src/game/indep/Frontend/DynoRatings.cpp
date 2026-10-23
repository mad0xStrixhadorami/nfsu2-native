#include "game/indep/Frontend/DynoRatings.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace DynoRatings {

namespace {

float F(const uint8_t *frame, size_t off)
{
    float v;
    memcpy(&v, frame + off, 4);
    return v;
}

constexpr float kStep = 1.3410818f;     // DAT_00563748 / 00563784: 3 mph in m/s
constexpr float kMaxSpeed = 44.702728f; // DAT_00563744: 100 mph
constexpr float kRadSToRpm = 9.549304f; // DAT_00563788
constexpr size_t kMaxSamples = 0x5c;

// 0x0031e0d0 + 0x0031ef20: forward acceleration (m/s^2) in the best-shifted gear every 3 mph.
// Shift points: the rpm where the next gear's torque at the matched rpm beats this gear's
// (searched from mid-range in 50 rpm steps), else maxRpm - 100.
struct AccelCurve {
    float a[kMaxSamples] = {};
    size_t count = 0;
};

AccelCurve BuildCurve(const PlayerCar::Performance &p, float radius)
{
    AccelCurve c;
    const float mass = p.mass;
    float shift[8] = {};
    for (int g = 0; g + 1 < p.numGears; ++g) {
        const float cur = p.gears[g], next = p.gears[g + 1];
        shift[g] = p.maxRpm - 100.0f;
        for (float rpm = (p.idleRpm + p.maxRpm) * 0.5f; rpm < p.maxRpm; rpm += 50.0f) {
            const float here = PlayerCar::Torque(p, rpm);
            const float there = PlayerCar::Torque(p, rpm * next / cur) * next / cur;
            if (here < there) {
                shift[g] = rpm;
                break;
            }
        }
    }
    shift[p.numGears - 1] = p.maxRpm + 500.0f;
    int gear = 0;
    float v = 0.0f;
    while (c.count < kMaxSamples && gear < p.numGears) {
        const float rpm = v / radius * p.finalDrive * p.gears[gear] * kRadSToRpm;
        if (rpm >= shift[gear]) {
            ++gear;
            continue;
        }
        c.a[c.count++] = PlayerCar::Torque(p, rpm) * p.finalDrive * p.gears[gear] / radius / mass;
        v += kStep;
    }
    return c;
}

float Lookup(const AccelCurve &c, float v)
{
    if (c.count == 0)
        return 0.0f;
    const float t = std::clamp(v / kStep, 0.0f, float(c.count - 1));
    const size_t i = size_t(t);
    const size_t j = std::min(i + 1, c.count - 1);
    return c.a[i] + (c.a[j] - c.a[i]) * (t - float(i));
}

float Clamp01(float v, float lo) { return std::min(1.0f, std::max(lo, v)); }

} // namespace

bool Limits::Parse(const uint8_t *container, size_t size)
{
    valid = false;
    for (size_t off = 0; off + 8 <= size;) {
        uint32_t id, len;
        memcpy(&id, container + off, 4);
        memcpy(&len, container + off + 4, 4);
        if (off + 8 + len > size)
            break;
        if (id == kLimitsChunk && len >= kLimitCount * 4) {
            memcpy(v, container + off + 8, kLimitCount * 4);
            valid = true;
        }
        off += 8 + len;
    }
    return valid;
}

Ratings Compute(const Limits &L, const uint8_t *record, const PlayerCar::Performance &p, float wheelRadius)
{
    Ratings r;
    if (!L.valid || !p.Valid() || wheelRadius <= 0.0f)
        return r;
    const uint8_t *f = record + 0x118; // the frame
    const float mass = F(f, 0x110), aero = F(f, 0x260);
    const auto norm = [&L](float x, int lo, int hi) { return (x - L.v[lo]) / (L.v[hi] - L.v[lo]); };

    // The curve: drive acceleration against 3 mph steps; drag deceleration is v^2 * aero / mass.
    const AccelCurve curve = BuildCurve(p, wheelRadius);
    float integral = 0.0f; // 0x0031cf90: area between the two up to 100 mph
    for (float v = 0.0f; v < kMaxSpeed; v += kStep) {
        const float drag = v * v * aero / mass;
        const float a = Lookup(curve, v);
        if (a < drag)
            break;
        integral += (a - drag) * kStep;
    }
    float top = 0.0f; // 0x0031dca8: where the drive no longer beats the drag
    const float last = curve.count ? float(curve.count - 1) * kStep : 0.0f;
    for (top = 0.0f; top <= last; top += kStep)
        if (Lookup(curve, top) < top * top * aero / mass)
            break;
    if (p.limiter > 0.0f)
        top = std::min(top, p.limiter * 0.44702727f);

    const float power = F(f, 0x2a0) * F(f, 0x2ac); // the shared power term
    // 0x0031ce90: nitrous count (obj + 0x2e4) is not part of the frame; taken as none.
    r.acceleration = Clamp01((1.0f - 0.18f) * norm(integral, 2, 3) + 0.1f * norm(power, 8, 9), 0.03f);
    // 0x0031cdd8
    r.topSpeed = Clamp01((1.0f - 0.085f) * norm(top, 0, 1) + 0.085f * norm(power, 8, 9), 0.03f);

    // 0x0031d060: nine frame ratios weighted by the table at 0x004807a8.
    static const float kWeights[9] = {0.14f, 0.10f, 0.15f, 0.15f, 0.10f, 0.15f, 0.15f, 0.08f, 0.08f};
    float w[9];
    w[0] = norm(((F(f, 0xdc) + F(f, 0xe0)) * 0.5f + (F(f, 0xfc) + F(f, 0x100)) * 0.5f) * 0.5f / mass, 14, 15);
    w[1] = norm((F(f, 0x274) * 0.65f + F(f, 0x278) * 0.35f) / mass, 16, 17);
    w[2] = norm((((F(f, 0x174) * F(f, 0x170) + F(f, 0x180)) * 0.5f + (F(f, 0x194) * F(f, 0x190) + F(f, 0x1a0)) * 0.5f) * 0.5f) / mass,
                12, 13);
    w[3] = 1.0f - norm(mass, 18, 19);
    const float split = F(f, 0x1c0);
    w[4] = split > 0.6f ? (1.0f - split) * 2.5f : (split + 0.2f) * 1.25f;
    w[5] = 1.0f - norm(F(f, 0x08), 20, 21);
    w[6] = 1.0f - norm(F(f, 0x160), 22, 23);
    w[7] = norm(F(f, 0x264), 24, 25);
    w[8] = norm(F(f, 0x268), 26, 27);
    float sum = 0.0f;
    for (int i = 0; i < 9; ++i)
        sum += w[i] * kWeights[i];
    r.handling = Clamp01(sum, 0.03f);
    return r;
}

} // namespace DynoRatings
