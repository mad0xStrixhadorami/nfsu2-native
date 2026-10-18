#include "game/indep/Sound/MusicStream.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "platform/linux/PlatformFile.h"

namespace {
const int kEaXaCoef[8] = {0, 240, 460, 392, 0, 0, -208, -220};

uint32_t BE32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t LE32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
int16_t BE16(const uint8_t *p) { return int16_t(uint16_t(p[0]) << 8 | p[1]); }

// EA-XA v2, one channel: 0xEE + 2 history samples + 28 BE PCM samples (61 bytes), or a
// coefficient/shift byte and 28 4-bit samples, high nibble first (15 bytes).
void DecodeEaXa(const uint8_t *data, size_t size, uint32_t count, int16_t hist[2], std::vector<int16_t> &out)
{
    out.clear();
    int h1 = hist[0], h2 = hist[1];
    for (size_t p = 0; out.size() < count && p < size;) {
        const uint8_t info = data[p];
        if (info == 0xEE) {
            if (p + 61 > size)
                break;
            for (int i = 0; i < 28; ++i)
                out.push_back(BE16(data + p + 5 + i * 2));
            h1 = out[out.size() - 1];
            h2 = out[out.size() - 2];
            p += 61;
            continue;
        }
        if (p + 15 > size)
            break;
        const int c1 = kEaXaCoef[info >> 4 & 3], c2 = kEaXaCoef[(info >> 4 & 3) + 4];
        const int shift = (info & 0x0f) + 8;
        for (int i = 0; i < 28; ++i) {
            const uint8_t b = data[p + 1 + i / 2];
            const int nibble = (i & 1) ? (b & 0x0f) : (b >> 4);
            int s = int32_t(uint32_t(nibble) << 28) >> shift;
            s = (s + h1 * c1 + h2 * c2 + 128) >> 8;
            s = std::clamp(s, -32768, 32767);
            out.push_back(int16_t(s));
            h2 = h1;
            h1 = s;
        }
        p += 15;
    }
    out.resize(count, 0);
    hist[0] = int16_t(h1);
    hist[1] = int16_t(h2);
}
} // namespace

MusicStream::~MusicStream()
{
    if (m_device)
        SDL_CloseAudioDevice(m_device);
}

bool MusicStream::Open(const ZipFileSystem &fs, const char *path)
{
    if (!fs.Open(path, m_file))
        return false;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        return false;
    SDL_AudioSpec want{}, have{};
    want.freq = m_rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 2048;
    m_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!m_device)
        return false;
    SDL_PauseAudioDevice(m_device, 0);
    printf("[Music] %s: %lld bytes\n", path, (long long)m_file.size);
    return true;
}

void MusicStream::SetPaused(bool paused)
{
    if (m_device)
        SDL_PauseAudioDevice(m_device, paused ? 1 : 0);
}

bool MusicStream::ReadAt(int64_t pos, void *dst, size_t size)
{
    if (pos < 0 || pos + int64_t(size) > m_file.size)
        return false;
    return platform::FileRead(m_file.archiveHandle, m_file.offset + pos, dst, int64_t(size)) == int64_t(size);
}

void MusicStream::DecodeBlock(const uint8_t *body, size_t size)
{
    // SCDl body: u32 BE sample count, u32 BE per-channel data offsets, then the channel data.
    const size_t base = 4 + 4 * size_t(m_channels);
    if (size < base)
        return;
    const uint32_t count = BE32(body);
    for (int c = 0; c < m_channels && c < 2; ++c) {
        const size_t begin = base + BE32(body + 4 + 4 * c);
        const size_t end = c + 1 < m_channels ? base + BE32(body + 8 + 4 * c) : size;
        if (begin > end || end > size)
            return;
        DecodeEaXa(body + begin, end - begin, count, m_hist[c], m_pcm[c]);
    }
    m_out.resize(size_t(count) * 2);
    for (uint32_t i = 0; i < count; ++i)
        for (int c = 0; c < 2; ++c) {
            int s = m_pcm[m_channels > 1 ? c : 0][i];
            m_out[i * 2 + c] = int16_t(std::clamp(int(s * m_volume), -32768, 32767));
        }
    SDL_QueueAudio(m_device, m_out.data(), Uint32(m_out.size() * sizeof(int16_t)));
}

void MusicStream::Update()
{
    if (!m_device)
        return;
    const Uint32 target = Uint32(m_rate * 4 * 3 / 4); // 0.75 s of 16-bit stereo
    for (int guard = 0; guard < 64 && SDL_GetQueuedAudioSize(m_device) < target; ++guard) {
        uint8_t head[8];
        if (!ReadAt(m_pos, head, 8)) {
            m_pos = 0; // end of the file: start the playlist over
            continue;
        }
        const uint32_t size = LE32(head + 4);
        if (size < 8) {
            m_pos = 0;
            continue;
        }
        if (!memcmp(head, "SCHl", 4)) {
            // Header (GSTR variant): tag/length/value records; 0x82 channels, 0x84 rate, 0xFF end.
            m_block.resize(size);
            if (!ReadAt(m_pos, m_block.data(), size))
                return;
            size_t p = 8 + (memcmp(m_block.data() + 8, "GSTR", 4) == 0 ? 8 : 0);
            while (p + 1 < size && m_block[p] != 0xff) {
                const uint8_t tag = m_block[p++];
                if (tag >= 0xfc)
                    continue;
                const uint8_t n = m_block[p++];
                uint32_t v = 0;
                for (uint8_t k = 0; k < n && p + k < size; ++k)
                    v = v << 8 | m_block[p + k];
                if (tag == 0x82)
                    m_channels = int(v);
                p += n;
            }
            memset(m_hist, 0, sizeof(m_hist));
            printf("[Music] stream %d\n", m_stream++);
        } else if (!memcmp(head, "SCDl", 4)) {
            m_block.resize(size);
            if (!ReadAt(m_pos, m_block.data(), size))
                return;
            DecodeBlock(m_block.data() + 8, size - 8);
        }
        m_pos += size;
    }
}
