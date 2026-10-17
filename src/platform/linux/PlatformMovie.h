#pragma once
#include <SDL.h>

#include <cstdint>
#include <memory>
#include <vector>

// Native replacement for UG2's PS2 movie player (0x0025ac60). UG2 movies (MOVIES\*.mpc) are EA
// containers: MPEG-2 video in MPCh chunks and EA ADPCM audio in SCHl/SCCl/SCDl/SCEl chunks.
// FFmpeg's "ea" demuxer reads both; video goes to an SDL YUV texture, audio to an SDL queue.
class MoviePlayer {
public:
    MoviePlayer();
    ~MoviePlayer();
    MoviePlayer(const MoviePlayer &) = delete;
    MoviePlayer &operator=(const MoviePlayer &) = delete;

    // Takes ownership of the whole movie file. Returns false if it cannot be decoded.
    bool Open(std::vector<uint8_t> data, SDL_Renderer *renderer);
    // Decode up to the current play time. Returns false once the movie has ended.
    bool Update();
    SDL_Texture *Texture() const { return m_texture; }
    bool Finished() const { return m_finished; }
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
    SDL_Texture *m_texture = nullptr;
    bool m_finished = false;
};
