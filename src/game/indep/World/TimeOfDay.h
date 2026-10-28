#pragma once
// Time of day (ISSUES #333).
//
// One hour-of-day value drives the sky, the fog, the sun's direction and colour, the strength of the daylight
// grade in the world and car shaders, and the binary switch of the night effects (street lamps, window glass,
// headlight pools, neon).  The presets are plain hours, so NFSU2_TIME=18.0 reproduces a sunset exactly and the
// clock can run on its own.
namespace Sky {

struct State {
    float hours = 13.0f;
    float day = 1.0f;     // 0 at night .. 1 at noon: the continuous shading factor of the shaders
    float sunset = 0.0f;  // 0 .. 1 while the sun is within ~12 degrees of the horizon (the warm low-sun mood)
    float sun[3] = {0.62f, 0.45f, 0.64f};   // unit vector towards the sun (below the horizon at night)
    float moon[3] = {0.35f, 0.45f, 0.82f};  // unit vector towards the moon (fixed, so the night sky has one)
    float sunCol[3] = {1.0f, 0.95f, 0.85f}; // the direct light / the disc colour: white at noon, orange at sunset
    float fog[3] = {0.82f, 0.87f, 0.93f};   // horizon haze and distance fog
    bool night = false;   // the binary switch: lamps, beams, lit windows, headlight pools, additive glows
};

void Init();            // reads NFSU2_TIME (hours) and NFSU2_TIME_SPEED (game hours per real second)
void Update(double dt); // advances the clock when NFSU2_TIME_SPEED is not 0
const State &Now();
State At(float hours);  // the pure function behind everything: the state at `hours`
void SetHours(float hours);
bool Running();         // the clock advances (NFSU2_TIME_SPEED, or F6)
void ToggleRunning();   // F6: freeze / resume the clock (20 minutes for a full day)
const char *Phase();    // "night", "dawn", "morning", "day", "afternoon", "sunset" (for the dev messages)

// F5 cycles the presets; NFSU2_TIME_PRESET=<name> starts on one.
int PresetCount();
const char *PresetName(int i);
float PresetHours(int i);
void NextPreset();
} // namespace Sky
