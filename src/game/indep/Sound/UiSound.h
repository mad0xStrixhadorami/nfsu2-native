#pragma once
#include <cstdint>
#include <mutex>
#include <vector>

#include <SDL.h>

#include "game/indep/Sound/EngineSound.h"

// Front-end sounds (ISSUES.md #126). FEng reports pad/button messages through NotifySoundMessage;
// UG2's menu screens turn them into UI sound triggers (MenuScreen 0x00295230 -> 0x00295c38 ->
// 0x001754e8: triggers below 0x17 go to the common bank, the rest to the front-end bank). The
// trigger -> sample table is built at runtime by EA's CSI scripts (not decoded), so this assumes
// SOUND\GLOBAL\FE_COMMON_MB.abk holds the common triggers in MW's eMenuSoundTriggers order (up,
// down, left, right, select, back, start/pause, wrong ...) -- its first sounds come in matching
// pairs (0/1 0.18/0.19 s, 2/3 0.27/0.26 s), which fits.
class UiSound {
public:
    ~UiSound();
    bool Load(const std::vector<uint8_t> &commonBank);
    void SetVolume(float v) { m_volume = v; }
    // An FEng message (PAD_UP, BUTTON_PRESSED, ...): plays its trigger if it has one.
    void OnMessage(uint32_t msg);

private:
    struct Voice {
        const EngineSound::Loop *sound;
        float pos;
    };
    static void Callback(void *self, Uint8 *stream, int len);
    void Play(int trigger);

    SDL_AudioDeviceID m_device = 0;
    int m_rate = 32000;
    float m_volume = 0.5f;
    std::vector<EngineSound::Loop> m_sounds;
    std::vector<Voice> m_voices;
    std::mutex m_lock;
    uint32_t m_lastMsg = 0, m_lastTicks = 0;
};
