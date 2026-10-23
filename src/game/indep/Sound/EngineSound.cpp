#include "game/indep/Sound/EngineSound.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
const int kVagCoef[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

uint32_t LE32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// PS2 SPU ADPCM: 16-byte frames (shift/predictor byte, flags byte, 28 4-bit samples low nibble first).
void DecodeVag(const uint8_t *data, size_t size, uint32_t count, std::vector<float> &out)
{
    int h1 = 0, h2 = 0;
    for (size_t f = 0; f + 16 <= size && out.size() < count; f += 16) {
        const int shift = data[f] & 0x0f, pred = std::min(data[f] >> 4, 4);
        for (int i = 0; i < 28; ++i) {
            const uint8_t b = data[f + 2 + i / 2];
            const int nibble = (i & 1) ? (b >> 4) : (b & 0x0f);
            int s = (int16_t(uint16_t(nibble << 12)) >> shift) + (h1 * kVagCoef[pred][0] + h2 * kVagCoef[pred][1] + 32) / 64;
            s = std::clamp(s, -32768, 32767);
            out.push_back(s / 32768.0f);
            h2 = h1;
            h1 = s;
        }
    }
    out.resize(count, 0.0f);
}

// "PT" header records: tag, length, big-endian value; 0xFF ends.
void ParsePt(const uint8_t *p, const uint8_t *end, uint32_t &count, uint32_t &rate, uint32_t &start)
{
    count = 0, rate = 16000, start = 0;
    for (p += 4; p + 1 < end && *p != 0xff;) {
        const uint8_t tag = *p++;
        if (tag >= 0xfc)
            continue;
        const uint8_t n = *p++;
        uint32_t v = 0;
        for (uint8_t k = 0; k < n && p + k < end; ++k)
            v = v << 8 | p[k];
        p += n;
        if (tag == 0x85)
            count = v;
        else if (tag == 0x84)
            rate = v;
        else if (tag == 0x88)
            start = v;
    }
}
} // namespace

EngineSound::~EngineSound()
{
    if (m_device)
        SDL_CloseAudioDevice(m_device);
}

bool EngineSound::DecodeBank(const std::vector<uint8_t> &bank, std::vector<Loop> &loops)
{
    // BNKl: 'BNKl', u16 version, u16 count, u32 header size, u32 total size, u32, then one u32 per
    // sound -- the offset of its "PT" header from that slot; sample data offsets are from the BNKl.
    const uint8_t *d = bank.data();
    size_t b = 0;
    while (b + 4 <= bank.size() && memcmp(d + b, "BNKl", 4) != 0)
        ++b;
    if (b + 0x18 > bank.size())
        return false;
    const unsigned count = d[b + 6] | d[b + 7] << 8;
    for (unsigned i = 0; i < count; ++i) {
        const size_t slot = b + 0x18 + 4 * i;
        if (slot + 4 > bank.size())
            break;
        const uint32_t rel = LE32(d + slot);
        if (!rel || slot + rel + 2 > bank.size() || memcmp(d + slot + rel, "PT", 2) != 0)
            continue;
        uint32_t samples, rate, start;
        ParsePt(d + slot + rel, d + bank.size(), samples, rate, start);
        const size_t bytes = (samples + 27) / 28 * 16;
        if (!samples || b + start + bytes > bank.size())
            continue;
        Loop l;
        l.rate = float(rate);
        DecodeVag(d + b + start, bytes, samples, l.pcm);
        loops.push_back(std::move(l));
    }
    return !loops.empty();
}

bool EngineSound::OpenDevice()
{
    if (m_device)
        return true;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        return false;
    SDL_AudioSpec want{}, have{};
    want.freq = m_rate;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = &EngineSound::Callback;
    want.userdata = this;
    m_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!m_device)
        return false;
    m_rate = have.freq;
    SDL_PauseAudioDevice(m_device, 0);
    return true;
}

bool EngineSound::Load(const std::vector<uint8_t> &bank)
{
    std::vector<Loop> loops;
    if (!DecodeBank(bank, loops) || loops.size() < 7 || !OpenDevice())
        return false;
    SDL_LockAudioDevice(m_device);
    m_loops = std::move(loops);
    SDL_UnlockAudioDevice(m_device);
    printf("[Engine] %zu loops\n", m_loops.size());
    return true;
}

bool EngineSound::LoadLayer(const std::vector<uint8_t> &bank, Loop &layer)
{
    std::vector<Loop> loops;
    if (!DecodeBank(bank, loops) || !OpenDevice())
        return false;
    SDL_LockAudioDevice(m_device);
    layer = std::move(loops[0]);
    SDL_UnlockAudioDevice(m_device);
    return true;
}

bool EngineSound::LoadShift(const std::vector<uint8_t> &bank)
{
    std::vector<Loop> sounds;
    if (!DecodeBank(bank, sounds) || sounds.size() < 2 || !OpenDevice())
        return false;
    SDL_LockAudioDevice(m_device);
    m_voices.clear();
    m_shift = std::move(sounds);
    SDL_UnlockAudioDevice(m_device);
    return true;
}

bool EngineSound::LoadNitrous(const std::vector<uint8_t> &bank)
{
    std::vector<Loop> sounds;
    if (!DecodeBank(bank, sounds) || sounds.empty() || !OpenDevice())
        return false;
    if (getenv("NFSU2_SOUND_DEBUG"))
        for (size_t i = 0; i < sounds.size(); ++i)
            printf("[Engine] nitrous sound %zu: %.2f s at %.0f Hz\n", i, float(sounds[i].pcm.size()) / sounds[i].rate,
                   sounds[i].rate);
    SDL_LockAudioDevice(m_device);
    m_nitrous = std::move(sounds);
    SDL_UnlockAudioDevice(m_device);
    return true;
}

bool EngineSound::LoadCollision(const std::vector<uint8_t> &bank)
{
    std::vector<Loop> sounds;
    if (!DecodeBank(bank, sounds) || sounds.empty() || !OpenDevice())
        return false;
    if (getenv("NFSU2_SOUND_DEBUG"))
        for (size_t i = 0; i < sounds.size(); ++i)
            printf("[Engine] collision sound %zu: %.2f s at %.0f Hz\n", i, float(sounds[i].pcm.size()) / sounds[i].rate,
                   sounds[i].rate);
    SDL_LockAudioDevice(m_device);
    m_collision = std::move(sounds);
    SDL_UnlockAudioDevice(m_device);
    return true;
}

// The bank's sound-to-event mapping (EA's "stitch" collision sets) is not decoded: pick by length,
// short sounds for light hits, long ones for hard hits (stand-in, ISSUES #221).
void EngineSound::Crash(float closingSpeed)
{
    if (!m_device || m_collision.empty() || !m_running)
        return;
    const float lo = closingSpeed < 4.0f ? 0.05f : closingSpeed < 9.0f ? 0.3f : 0.55f;
    const float hi = closingSpeed < 4.0f ? 0.3f : closingSpeed < 9.0f ? 0.55f : 1.3f;
    std::vector<size_t> pick;
    for (size_t i = 0; i < m_collision.size(); ++i) {
        const float len = float(m_collision[i].pcm.size()) / m_collision[i].rate;
        if (len >= lo && len < hi)
            pick.push_back(i);
    }
    if (pick.empty())
        return;
    static unsigned seed = 12345;
    seed = seed * 1664525u + 1013904223u;
    const size_t which = pick[(seed >> 8) % pick.size()];
    SDL_LockAudioDevice(m_device);
    if (m_voices.size() < 4)
        m_voices.push_back({&m_collision[which], 0.0f});
    SDL_UnlockAudioDevice(m_device);
}

void EngineSound::NitrousBurst(bool start)
{
    if (!m_device || m_nitrous.empty() || !m_running || !start)
        return;
    SDL_LockAudioDevice(m_device);
    if (m_voices.size() < 4)
        m_voices.push_back({&m_nitrous[0], 0.0f});
    SDL_UnlockAudioDevice(m_device);
}

void EngineSound::Shift(int direction)
{
    if (!m_device || m_shift.size() < 2 || !m_running)
        return;
    if (getenv("NFSU2_SOUND_DEBUG"))
        printf("[Engine] shift %s\n", direction > 0 ? "up" : "down");
    SDL_LockAudioDevice(m_device);
    if (m_voices.size() < 4)
        m_voices.push_back({&m_shift[direction > 0 ? 0 : 1], 0.0f});
    SDL_UnlockAudioDevice(m_device);
}

void EngineSound::Set(float rpm, float idleRpm, float redline, float throttle, bool running)
{
    m_rpm = rpm, m_idle = idleRpm, m_redline = redline, m_throttle = throttle, m_running = running;
}

void EngineSound::MixLoop(Loop &l, float target, float step, float *out, int frames)
{
    const float n = float(l.pcm.size());
    for (int f = 0; f < frames; ++f) {
        l.gain += (target - l.gain) * 0.002f; // smooth gain changes
        if (l.gain < 1e-4f && target == 0.0f) {
            l.pos = std::fmod(l.pos + step, n);
            continue;
        }
        const int i0 = int(l.pos);
        const float t = l.pos - float(i0);
        const float s = l.pcm[size_t(i0)] * (1 - t) + l.pcm[size_t((i0 + 1) % l.pcm.size())] * t;
        out[f * 2] += s * l.gain;
        out[f * 2 + 1] += s * l.gain;
        l.pos += step;
        if (l.pos >= n)
            l.pos -= n;
    }
}

void EngineSound::Callback(void *self, Uint8 *stream, int len)
{
    static_cast<EngineSound *>(self)->Mix(reinterpret_cast<float *>(stream), len / int(2 * sizeof(float)));
}

void EngineSound::Mix(float *out, int frames)
{
    std::fill(out, out + frames * 2, 0.0f);
    if (m_loops.size() < 7)
        return;
    const float rpm = m_rpm, idle = m_idle, redline = std::max(m_redline.load(), idle + 1000.0f);
    const bool running = m_running;
    // Blend on/off throttle sets over ~0.15 s.
    m_load += ((m_throttle > 0.1f ? 1.0f : 0.0f) - m_load) * std::min(1.0f, frames / (0.15f * m_rate));
    const float r = std::clamp((rpm - idle) / (redline - idle), 0.0f, 1.2f);
    // Band centres (fractions of idle..redline) for loops 1-3 / 4-6, and the rpm each represents.
    const float centres[3] = {0.15f, 0.5f, 0.85f};
    float target[7] = {};
    target[0] = std::max(0.0f, 1.0f - r * 5.0f);
    for (int k = 0; k < 3; ++k) {
        float w = std::max(0.0f, 1.0f - std::fabs(r - centres[k]) / 0.35f);
        if (k == 2 && r > centres[2])
            w = 1.0f;
        w *= 1.0f - target[0];
        target[1 + k] = w * m_load;
        target[4 + k] = w * (1.0f - m_load);
    }
    const float volume = running ? 0.30f : 0.0f;
    for (int i = 0; i < 7; ++i) {
        Loop &l = m_loops[size_t(i)];
        const float ref = i == 0 ? idle : idle + centres[(i - 1) % 3] * (redline - idle);
        const float pitch = std::clamp(std::max(rpm, idle) / ref, 0.5f, 2.0f);
        MixLoop(l, target[i] * volume, l.rate / m_rate * pitch, out, frames);
    }
    // Tyre squeal with slip; road noise rising with speed.
    const float speed = std::fabs(m_speed.load());
    if (!m_skid.pcm.empty())
        MixLoop(m_skid, running ? 0.22f * m_slip : 0.0f, m_skid.rate / m_rate, out, frames);
    if (!m_road.pcm.empty())
        MixLoop(m_road, running ? 0.12f * std::min(1.0f, speed / 50.0f) : 0.0f,
                m_road.rate / m_rate * (0.8f + std::min(speed, 70.0f) / 120.0f), out, frames);
    // One-shots (gear changes).
    for (Voice &v : m_voices) {
        const float step = v.sound->rate / m_rate;
        for (int f = 0; f < frames && v.pos < v.sound->pcm.size(); ++f, v.pos += step) {
            const float smp = v.sound->pcm[size_t(v.pos)] * 0.35f;
            out[f * 2] += smp;
            out[f * 2 + 1] += smp;
        }
    }
    m_voices.erase(std::remove_if(m_voices.begin(), m_voices.end(),
                                  [](const Voice &v) { return v.pos >= v.sound->pcm.size(); }),
                   m_voices.end());
}
