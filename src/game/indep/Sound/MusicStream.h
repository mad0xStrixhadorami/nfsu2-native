#pragma once
#include <cstdint>
#include <vector>

#include <SDL.h>

#include "game/indep/File/ZipFileSystem.h"

// Native player for UG2's music: SOUND\PFDATA\MUSICSFX.MUS (266 MB on the PAL disc) is 43
// concatenated EA SCHl streams -- 32 kHz stereo EA-XA v2 (ISSUES.md #123). The PS2 plays it through
// EA's interactive music system (MUSICSFX.MPF, the song map, is not decoded); here the streams play
// one after another, read from the archive a block at a time and decoded into an SDL audio queue.
class MusicStream {
public:
    ~MusicStream();
    bool Open(const ZipFileSystem &fs, const char *path);
    // Keep about 0.75 s queued; call once per frame.
    void Update();
    void SetVolume(float v) { m_volume = v; }
    void SetPaused(bool paused);

private:
    bool ReadAt(int64_t pos, void *dst, size_t size);
    void DecodeBlock(const uint8_t *body, size_t size);

    ZipFile m_file{};
    int64_t m_pos = 0;
    int m_channels = 2;
    int m_rate = 32000;
    int m_stream = 0;
    int16_t m_hist[2][2] = {};
    float m_volume = 1.0f;
    SDL_AudioDeviceID m_device = 0;
    std::vector<uint8_t> m_block;
    std::vector<int16_t> m_pcm[2];
    std::vector<int16_t> m_out;
};
