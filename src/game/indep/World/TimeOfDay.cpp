#include "game/indep/World/TimeOfDay.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr float kPi = 3.14159265f;

// The presets are what F5 cycles and what NFSU2_TIME_PRESET names.  Morning and noon are near the old fixed
// daylight; "afternoon" has the long shadows, "sunset" the orange sky with the lamps already on, "night" the
// dusk-blue original (#277).
struct Preset {
    const char *name;
    float hours;
};
const Preset kPresets[] = {
    {"day", 12.5f},
    {"afternoon", 16.0f},
    {"sunset", 17.8f},
    {"night", 21.0f},
};
constexpr int kPresetCount = int(sizeof(kPresets) / sizeof(kPresets[0]));

float saturate(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
float smoothstep(float a, float b, float x) { const float t = saturate((x - a) / (b - a)); return t * t * (3.0f - 2.0f * t); }
float lerp(float a, float b, float t) { return a + (b - a) * t; }
void mix3(const float a[3], const float b[3], float t, float out[3])
{
    for (int i = 0; i < 3; ++i)
        out[i] = lerp(a[i], b[i], t);
}

Sky::State g_state;
float g_speed = 0.0f; // game hours per real second

} // namespace

namespace Sky {

State At(float hours)
{
    State s;
    s.hours = std::fmod(std::fmod(hours, 24.0f) + 24.0f, 24.0f);

    // The sun's arc: rises in the east (+x) at 06:00, leans south (+y), sets in the west at 18:00 and keeps
    // travelling below the horizon through the night, so nothing jumps at dusk.  The noon sun stands ~53
    // degrees up, the way the old fixed daylight direction (elevation ~40 degrees) read in the city.
    const float t = (s.hours - 6.0f) / 12.0f; // 0 at sunrise, 0.5 at noon, 1 at sunset
    const float a = t * kPi;
    float x = std::cos(a), y = 0.55f * std::sin(a) + 0.15f, z = 0.95f * std::sin(a);
    const float len = std::sqrt(x * x + y * y + z * z);
    s.sun[0] = x / len, s.sun[1] = y / len, s.sun[2] = z / len;

    const float h = s.sun[2]; // sine of the sun's elevation
    // 0 = night, 1 = full daylight.  Civil twilight (the sun at the horizon) keeps a little of the day grade.
    s.day = smoothstep(-0.06f, 0.40f, h);
    // The warm band around sunrise and sunset: strong at the horizon, a faint golden cast through the late afternoon.
    s.sunset = std::exp(-(h * h) / (0.30f * 0.30f));
    if (h < -0.30f)
        s.sunset = 0.0f;
    // The night effects switch when the sun is around 9 degrees up: at a sunset the lamps are already on.
    s.night = h < 0.16f;

    // Direct light / disc colour: white at noon, deep orange at the horizon, dim blue moonlight at night.
    const float low = 1.0f - smoothstep(0.04f, 0.55f, h);
    const float warm[3] = {1.0f, 0.52f, 0.26f}, white[3] = {1.0f, 0.96f, 0.88f};
    mix3(white, warm, low, s.sunCol);
    if (s.night && h < 0.02f) { // the sun is down: the moon is a cold, faint fill
        const float moon[3] = {0.42f, 0.50f, 0.68f};
        mix3(s.sunCol, moon, smoothstep(0.02f, -0.15f, h), s.sunCol);
    }

    // Haze: the old bright azure by day, the old dusk blue at night, and a golden horizon through the sunset.
    const float dayFog[3] = {0.82f, 0.87f, 0.93f}, nightFog[3] = {0.55f, 0.62f, 0.72f};
    mix3(nightFog, dayFog, s.day, s.fog);
    const float golden[3] = {1.0f, 0.62f, 0.35f};
    mix3(s.fog, golden, s.sunset * 0.70f, s.fog);
    return s;
}

void SetHours(float hours)
{
    g_state = At(hours);
}

void Init()
{
    float hours = 13.0f;
    bool preset = false;
    if (const char *p = getenv("NFSU2_TIME_PRESET"))
        for (int i = 0; i < kPresetCount; ++i)
            if (!strcmp(p, kPresets[i].name))
                hours = kPresets[i].hours, preset = true;
    if (const char *t = getenv("NFSU2_TIME"))
        hours = float(atof(t)), preset = false;
    if (const char *sp = getenv("NFSU2_TIME_SPEED"))
        g_speed = float(atof(sp));
    if (getenv("NFSU2_NIGHT") && !getenv("NFSU2_TIME") && !getenv("NFSU2_TIME_PRESET"))
        hours = 21.0f, preset = true; // the old switch still means night (ISSUES #277)
    SetHours(hours);
    if (preset || getenv("NFSU2_TIME"))
        printf("[Sky] time of day %.2f h (%s, sun %+.0f deg), cycle %.3f h/s\n", g_state.hours, Phase(),
               std::asin(g_state.sun[2]) * 180.0f / kPi, g_speed);
}

void Update(double dt)
{
    if (g_speed == 0.0f)
        return;
    SetHours(g_state.hours + float(dt) * g_speed);
}

const State &Now() { return g_state; }

const char *Phase()
{
    const float h = g_state.hours;
    if (g_state.sun[2] < -0.02f)
        return "night";
    if (g_state.sunset > 0.5f)
        return h < 12.0f ? "dawn" : "sunset";
    if (h < 11.0f)
        return "morning";
    if (h < 15.0f)
        return "day";
    return "afternoon";
}

bool Running() { return g_speed != 0.0f; }

void ToggleRunning()
{
    g_speed = g_speed != 0.0f ? 0.0f : 0.02f; // 0.02 h/s = 20 minutes for a day
    printf("[Sky] time of day %s at %.2f h%s\n", g_speed != 0.0f ? "running" : "frozen", g_state.hours,
           g_speed != 0.0f ? " (20 min per day)" : "");
}

int PresetCount() { return kPresetCount; }
const char *PresetName(int i) { return i >= 0 && i < kPresetCount ? kPresets[i].name : "?"; }
float PresetHours(int i) { return i >= 0 && i < kPresetCount ? kPresets[i].hours : 13.0f; }

void NextPreset()
{
    // The first preset strictly after the current hour, wrapping to the first one at night.
    float best = 1e9f, hours = kPresets[0].hours;
    const char *name = kPresets[0].name;
    for (int i = 0; i < kPresetCount; ++i) {
        const float d = kPresets[i].hours > g_state.hours + 0.05f ? kPresets[i].hours - g_state.hours : kPresets[i].hours + 24.0f - g_state.hours;
        if (d < best)
            best = d, hours = kPresets[i].hours, name = kPresets[i].name;
    }
    SetHours(hours);
    printf("[Sky] time of day %.2f h (%s, sun %+.0f deg)\n", g_state.hours, name, std::asin(g_state.sun[2]) * 180.0f / kPi);
}

} // namespace Sky
