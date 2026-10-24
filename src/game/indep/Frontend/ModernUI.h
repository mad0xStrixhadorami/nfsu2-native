#pragma once
// A modern native UI layer drawn over / under the original FEng screens (ISSUES #276): an animated full-window menu
// backdrop and a redesigned race HUD (arc rev meter, big speed readout, glass info pills) in the Rajdhani typeface
// (SIL OFL, assets/ui).  Everything draws with the SDL renderer in window pixels (logical size 0).
#include <string>
#include <vector>

#include <SDL.h>

class ModernUI {
public:
    struct Hud {
        float speedKmh = 0, rpm = 0, rpmScale = 8500; // rpmScale = the dial's full scale
        int gear = 1;                                 // 1.. forward, 0 neutral, -1 reverse
        bool hasNitrous = false;
        float nitrous = 0;
        bool nitrousActive = false;
        bool race = false;                            // show position / lap / time
        int position = 1, racers = 1, lap = 1, laps = 1;
        float lapTime = 0, totalTime = 0, bestLap = 0;
        bool mph = false;
    };
    bool Init(SDL_Renderer *r);
    bool Ready() const { return m_font != nullptr; }
    // Menus: the animated backdrop under the 4:3 FEng box (call with the logical size reset, before engine->Render()).
    void DrawBackdrop(int w, int h, float seconds);
    // Race: the HUD cluster over the world.
    void DrawHud(int w, int h, const Hud &hud, float seconds);
    // The pause dialog: a dimmed scene, a glass panel with a title and a list (selected = highlighted).  When `confirm` is set a
    // Yes / No question replaces the list (confirmYes = the highlighted answer).
    void DrawDialog(int w, int h, const std::string &title, const std::vector<std::string> &items, int selected, bool confirm,
                    const std::string &question, bool confirmYes, float seconds);
    ~ModernUI();

private:
    struct Glyph {
        float x0, y0, x1, y1, xoff, yoff, xadv; // atlas rect and layout (pixels at kBakePx)
    };
    SDL_Renderer *m_r = nullptr;
    SDL_Texture *m_font = nullptr, *m_glow = nullptr;
    Glyph m_glyphs[96] = {};
    int m_atlasW = 0, m_atlasH = 0;
    static constexpr float kBakePx = 96.0f;
    void Text(float x, float y, float px, const std::string &s, SDL_Color c, int align = 0); // align 0 left, 1 centre, 2 right; y = top
    float TextWidth(const std::string &s, float px) const;
    void Quad(SDL_FPoint a, SDL_FPoint b, SDL_FPoint c, SDL_FPoint d, SDL_Color ca, SDL_Color cb, SDL_Color cc, SDL_Color cd);
    void Rect(float x, float y, float w, float h, SDL_Color top, SDL_Color bottom);
    void Glass(float x, float y, float w, float h, float cut, SDL_Color tint, SDL_Color edge); // slanted-corner panel
    void ArcSegment(float cx, float cy, float r0, float r1, float a0, float a1, SDL_Color c);
    void Glow(float cx, float cy, float radius, SDL_Color c);
};
