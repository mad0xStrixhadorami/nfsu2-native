#include "game/indep/Sound/SirenSound.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

SirenSound::~SirenSound()
{
    if (m_device)
        SDL_CloseAudioDevice(m_device);
}

bool SirenSound::Open()
{
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        return false;
    SDL_AudioSpec want{}, have{};
    want.freq = m_rate;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = &SirenSound::Callback;
    want.userdata = this;
    m_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!m_device)
        return false;
    m_rate = have.freq;
    SDL_PauseAudioDevice(m_device, 0);
    return true;
}

void SirenSound::SetState(float distance, bool yelp, float volume)
{
    if (distance < 0.0f) {
        m_target = 0.0f;
        return;
    }
    const float k = std::clamp(1.0f - (distance - 15.0f) / 300.0f, 0.0f, 1.0f);
    m_target = k * k * 0.30f * volume;
    m_yelp = yelp;
}

void SirenSound::Callback(void *self, Uint8 *stream, int len)
{
    SirenSound &s = *static_cast<SirenSound *>(self);
    float *out = reinterpret_cast<float *>(stream);
    const int frames = len / int(2 * sizeof(float));
    const double dt = 1.0 / s.m_rate;
    for (int f = 0; f < frames; ++f) {
        s.m_gain += (s.m_target - s.m_gain) * 0.0008f;
        s.m_sweep += dt / (s.m_yelp ? 0.32 : 3.6);
        if (s.m_sweep >= 1.0)
            s.m_sweep -= 1.0;
        // wail: a smooth sweep up and down; yelp: a faster rise with a sharp fall
        const double shape = s.m_yelp ? s.m_sweep : 0.5 - 0.5 * std::cos(6.2831853 * s.m_sweep);
        const double freq = 650.0 + 800.0 * shape;
        s.m_phase += freq * dt;
        s.m_phase -= std::floor(s.m_phase);
        const double ph = 6.2831853 * s.m_phase;
        double v = std::sin(ph) + 0.45 * std::sin(2 * ph) + 0.28 * std::sin(3 * ph) + 0.12 * std::sin(5 * ph);
        v *= 0.55;
        s.m_lp += (v - s.m_lp) * 0.55; // take the edge off
        const float o = float(s.m_lp) * s.m_gain;
        out[f * 2] = o;
        out[f * 2 + 1] = o;
    }
}
