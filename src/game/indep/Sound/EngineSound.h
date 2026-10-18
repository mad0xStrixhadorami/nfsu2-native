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
    // Continuous effect layers (first loop of each bank): tyre squeal (SOUND\SKIDS\SKID_PAV_MB.abk)
    // and road noise (SOUND\IG_GLOBAL\ROADNOISE_00_MB.abk).
    bool LoadSkid(const std::vector<uint8_t> &bank) { return LoadLayer(bank, m_skid); }
    bool LoadRoad(const std::vector<uint8_t> &bank) { return LoadLayer(bank, m_road); }
    // Engine state for the mixer (called from the game loop).
    void Set(float rpm, float idleRpm, float redline, float throttle, bool running);
    void SetMotion(float speed, float slip) { m_speed = speed, m_slip = slip; }

    struct Loop {
        std::vector<float> pcm;
        float rate = 16000;
        float pos = 0;
        float gain = 0; // smoothed
    };
    // Decodes every sound of an ABKC/BNKl bank (PS2 SPU ADPCM) -- shared with UiSound.
    static bool DecodeBank(const std::vector<uint8_t> &bank, std::vector<Loop> &loops);

private:
    bool LoadLayer(const std::vector<uint8_t> &bank, Loop &layer);
    bool OpenDevice();
    static void MixLoop(Loop &l, float target, float step, float *out, int frames);
    static void Callback(void *self, Uint8 *stream, int len);
    void Mix(float *out, int frames);

    SDL_AudioDeviceID m_device = 0;
    int m_rate = 32000;
    std::vector<Loop> m_loops;
    Loop m_skid, m_road;
    std::atomic<float> m_speed{0}, m_slip{0};
    std::atomic<float> m_rpm{0}, m_idle{900}, m_redline{7000}, m_throttle{0};
    std::atomic<bool> m_running{false};
    float m_load = 0; // smoothed on/off throttle blend
};
