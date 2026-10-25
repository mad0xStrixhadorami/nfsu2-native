#pragma once
#include <SDL.h>

// The police siren (ISSUES #318): synthesized (the original has no Israeli siren): a wail that sweeps between about 650 and 1450 Hz and,
// at high heat, a fast yelp, with the harmonics of a horn driver.  SetState is called every frame with the nearest siren's distance.
class SirenSound {
public:
    ~SirenSound();
    bool Open();
    // distance: metres to the nearest siren (< 0: none); yelp: the fast pattern; volume: 0..1 master.
    void SetState(float distance, bool yelp, float volume);

private:
    static void Callback(void *self, Uint8 *stream, int len);
    SDL_AudioDeviceID m_device = 0;
    int m_rate = 32000;
    volatile float m_gain = 0.0f, m_target = 0.0f;
    volatile bool m_yelp = false;
    double m_phase = 0.0, m_sweep = 0.0, m_lp = 0.0;
};
