#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

#include <SDL.h>

// Player engine sound from UG2's per-car banks (ISSUES.md #124): SOUND\ENGINE\CAR_nn_ENG_MB_SPU.abk
// is an EA "ABKC" bank whose BNKl sub-bank holds 7 looped PS2 SPU-ADPCM sounds -- 0 idle (~10 kHz),
// 1-3 on throttle and 4-6 off throttle at rising engine speed (~16 kHz). The PS2 mixes them with an
// EE code module in the matching _EE.abk (MIPS, not ported); here the loops are crossfaded by rpm
// band and pitched by the car's engine speed in an SDL audio callback.
class EngineSound {
public:
    ~EngineSound();
    bool Load(const std::vector<uint8_t> &spuBank);
    // Engine state for the mixer (called from the game loop).
    void Set(float rpm, float idleRpm, float redline, float throttle, bool running);

private:
    struct Loop {
        std::vector<float> pcm;
        float rate = 16000;
        float pos = 0;
        float gain = 0; // smoothed
    };
    static void Callback(void *self, Uint8 *stream, int len);
    void Mix(float *out, int frames);

    SDL_AudioDeviceID m_device = 0;
    int m_rate = 32000;
    std::vector<Loop> m_loops;
    std::atomic<float> m_rpm{0}, m_idle{900}, m_redline{7000}, m_throttle{0};
    std::atomic<bool> m_running{false};
    float m_load = 0; // smoothed on/off throttle blend
};
