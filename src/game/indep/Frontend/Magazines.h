#pragma once
// The career's magazine covers (ISSUES #309).  FRONTEND\MAGAZINES_FRONTEND.BIN (chunk 0x30230) lists, per magazine, the 512 x 512 layer
// textures that make a cover - three backgrounds, two mastheads, three model cut-outs and up to three headline layers (`XT`) - which live in
// MAGAZINES_STREAMING_HIGH.BIN (full covers) and MAGAZINES_STREAMING_LOW.BIN (the extra mastheads); MAGAZINES_SHOWCASE.BIN (chunk 0x30231)
// holds 14 masthead-only "showcase" magazines keyed by the language label of the career media record (0x34a16 +0x28).  The page occupies the
// top-left 0.655 x 0.84 of each square texture.  The player's car is drawn between the background and the model layer by the caller.
#include <SDL.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "game/indep/Texture/TexturePack.h"

class Magazines {
public:
    struct Cover {
        std::string code;                    // CAE, EGT, IT ...
        uint32_t bg[3] = {}, masthead[2] = {}, model[3] = {}, xt[3] = {};
        bool full = true;                    // false: a masthead-only showcase magazine
        uint32_t label = 0;                  // showcase: the media record's language label (+0x28)
    };
    // `frontend` / `showcase` / `high` / `low` are the four files' bytes.
    bool Load(const std::vector<uint8_t> &frontend, const std::vector<uint8_t> &showcase, std::vector<uint8_t> high, std::vector<uint8_t> low);
    bool Ready() const { return !m_covers.empty(); }
    const std::vector<Cover> &Covers() const { return m_covers; }
    // The cover for a magazine code (the text of the media record's label, "MAGAZINE_SETUP_<code>"): a full one, else a showcase one, else null.
    const Cover *ForCode(const std::string &code) const;
    const Cover *ForLabel(uint32_t label) const;

    // Draws the cover into `area` (a square of which the left 0.65 is the page): `variant` picks the background / masthead / model / headline;
    // `underModel` runs between the background and the model layer (the player's car).
    void Draw(SDL_Renderer *r, const Cover &c, const SDL_Rect &area, unsigned variant, const std::function<void(const SDL_Rect &page)> &underModel);
    // The page rectangle inside a square cover area.
    static SDL_Rect PageOf(const SDL_Rect &area) { return {area.x, area.y, int(area.w * 0.655f), int(area.h * 0.84f)}; }

private:
    SDL_Texture *Texture(SDL_Renderer *r, uint32_t hash);
    std::vector<Cover> m_covers;
    std::vector<uint8_t> m_high, m_low;
    TexturePack m_highPack, m_lowPack;
    std::map<uint32_t, SDL_Texture *> m_cache;
};
