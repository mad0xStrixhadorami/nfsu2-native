#include "game/indep/Sound/UiSound.h"

#include <algorithm>
#include <cstdio>

UiSound::~UiSound()
{
    if (m_device)
        SDL_CloseAudioDevice(m_device);
}

bool UiSound::Load(const std::vector<uint8_t> &bank)
{
    if (!EngineSound::DecodeBank(bank, m_sounds))
        return false;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        return false;
    SDL_AudioSpec want{}, have{};
    want.freq = m_rate;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = &UiSound::Callback;
    want.userdata = this;
    m_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!m_device)
        return false;
    m_rate = have.freq;
    SDL_PauseAudioDevice(m_device, 0);
    printf("[UiSound] %zu sounds\n", m_sounds.size());
    return true;
}

void UiSound::OnMessage(uint32_t msg)
{
    int trigger = -1;
    switch (msg) {
    case 0x72619778: trigger = 0; break; // PAD_UP
    case 0x911c0a4b: trigger = 1; break; // PAD_DOWN
    case 0x9120409e: trigger = 2; break; // PAD_LEFT
    case 0xb5971bf1: trigger = 3; break; // PAD_RIGHT
    case 0x0c407210:                     // BUTTON_PRESSED
    case 0x406415e3: trigger = 4; break; // PAD_ACCEPT
    case 0x911ab364: trigger = 5; break; // PAD_BACK
    case 0xb5af2461: trigger = 6; break; // PAD_START
    default: return;
    }
    // One press can arrive as both BUTTON_PRESSED and PAD_ACCEPT: play it once.
    const uint32_t now = SDL_GetTicks();
    if (trigger == 4 && (m_lastMsg == 0x0c407210 || m_lastMsg == 0x406415e3) && m_lastMsg != msg && now - m_lastTicks < 80)
        return;
    m_lastMsg = msg;
    m_lastTicks = now;
    Play(trigger);
}

void UiSound::Play(int trigger)
{
    if (!m_device || trigger < 0 || size_t(trigger) >= m_sounds.size())
        return;
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_voices.size() < 8)
        m_voices.push_back({&m_sounds[size_t(trigger)], 0.0f});
}

void UiSound::Callback(void *self, Uint8 *stream, int len)
{
    UiSound &ui = *static_cast<UiSound *>(self);
    float *out = reinterpret_cast<float *>(stream);
    const int frames = len / int(2 * sizeof(float));
    std::fill(out, out + frames * 2, 0.0f);
    std::lock_guard<std::mutex> lock(ui.m_lock);
    for (Voice &v : ui.m_voices) {
        const float step = v.sound->rate / ui.m_rate;
        for (int f = 0; f < frames && v.pos < v.sound->pcm.size(); ++f, v.pos += step) {
            const float s = v.sound->pcm[size_t(v.pos)] * ui.m_volume;
            out[f * 2] += s;
            out[f * 2 + 1] += s;
        }
    }
    ui.m_voices.erase(std::remove_if(ui.m_voices.begin(), ui.m_voices.end(),
                                     [](const Voice &v) { return v.pos >= v.sound->pcm.size(); }),
                      ui.m_voices.end());
}
