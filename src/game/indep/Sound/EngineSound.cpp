#include "game/indep/Sound/EngineSound.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

bool EngineSound::Load(const std::vector<uint8_t> &bank)
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
    std::vector<Loop> loops;
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
    if (loops.size() < 7)
        return false;
    if (!m_device) {
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
    }
    SDL_LockAudioDevice(m_device);
    m_loops = std::move(loops);
    SDL_UnlockAudioDevice(m_device);
    SDL_PauseAudioDevice(m_device, 0);
    printf("[Engine] %zu loops\n", m_loops.size());
    return true;
}

void EngineSound::Set(float rpm, float idleRpm, float redline, float throttle, bool running)
{
    m_rpm = rpm, m_idle = idleRpm, m_redline = redline, m_throttle = throttle, m_running = running;
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
        const float step = l.rate / m_rate * pitch;
        const float n = float(l.pcm.size());
        for (int f = 0; f < frames; ++f) {
            l.gain += (target[i] * volume - l.gain) * 0.002f; // smooth gain changes
            if (l.gain < 1e-4f && target[i] == 0.0f) {
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
}
