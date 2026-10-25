#pragma once
// A modern native UI layer drawn over / under the original FEng screens (ISSUES #276): an animated full-window menu
// backdrop and a redesigned race HUD (arc rev meter, big speed readout, glass info pills) in the Rajdhani typeface
// (SIL OFL, assets/ui).  Everything draws with the SDL renderer in window pixels (logical size 0).
#include <string>
#include <string_view>
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
        // Drifting (ISSUES #291)
        bool drifting = false;
        float driftAngle = 0, driftChain = 0, driftBanner = 0, driftBank = 0, driftTotal = 0;
        int driftMult = 1;
        // The map (ISSUES #296): the RGBA window from Minimap::RenderModern, its heading and how far to the finish.
        const uint32_t *mapPixels = nullptr;
        int mapSize = 0;
        bool mapNew = true;    // the pixels changed since the last draw (the map image refreshes at 30 Hz)
        bool mapRotate = true; // heading up; otherwise north up
        float mapYaw = 0;      // the car's yaw (rad)
        float mapMetres = 0;   // the window's diameter in metres (the scale label)
        float finishKm = -1;   // < 0: no finish (free roam)
        const char *eventName = ""; // the label under the map (the event type)
    };
    bool Init(SDL_Renderer *r);
    bool Ready() const { return m_font != nullptr; }
    // Menus: the animated backdrop under the 4:3 FEng box (call with the logical size reset, before engine->Render()).
    void DrawBackdrop(int w, int h, float seconds);
    // Race: the HUD cluster over the world.
    void DrawHud(int w, int h, const Hud &hud, float seconds);
    // Speed wind (ISSUES #292): streaks racing outward from the screen centre at the edges, strength 0..1 (speed), nitrous adds a blue
    // tunnel of lines. Drawn under the HUD.
    void DrawWind(int w, int h, float strength, float nitro, float seconds);
    // The pause dialog: a dimmed scene, a glass panel with a title and a list (selected = highlighted).  When `confirm` is set a
    // Yes / No question replaces the list (confirmYes = the highlighted answer).
    void DrawDialog(int w, int h, std::string_view title, const char *const *items, int itemCount, int selected, bool confirm,
                    std::string_view question, bool confirmYes, float seconds);
    // The profiler's overlay (a corner panel of monospaced-looking lines).
    void DrawOverlay(int w, int h, const char lines[][96], int count);
    // The loading screen (ISSUES #301): assets/ui/loading.jpg full-window with a slow drift, a stage label, a smooth progress bar with
    // a percentage and a rotating hint.  `fraction` is the overall progress 0..1.  Returns false when the art / font is missing.
    bool DrawLoading(int w, int h, const char *stage, float fraction, float seconds);
    ~ModernUI();
    void DrawMinimap(int w, int h, const Hud &hud, float seconds);

private:
    struct Glyph {
        float x0, y0, x1, y1, xoff, yoff, xadv; // atlas rect and layout (pixels at kBakePx)
    };
    SDL_Renderer *m_r = nullptr;
    SDL_Texture *m_font = nullptr, *m_glow = nullptr, *m_map = nullptr, *m_loadArt = nullptr;
    int m_loadW = 0, m_loadH = 0;
    int m_mapTexSize = 0;
    Glyph m_glyphs[96] = {};
    int m_atlasW = 0, m_atlasH = 0;
    static constexpr float kBakePx = 96.0f;
    void Text(float x, float y, float px, std::string_view s, SDL_Color c, int align = 0); // align 0 left, 1 centre, 2 right; y = top
    float TextWidth(std::string_view s, float px) const;
    void Quad(SDL_FPoint a, SDL_FPoint b, SDL_FPoint c, SDL_FPoint d, SDL_Color ca, SDL_Color cb, SDL_Color cc, SDL_Color cd);
    void Rect(float x, float y, float w, float h, SDL_Color top, SDL_Color bottom);
    void Glass(float x, float y, float w, float h, float cut, SDL_Color tint, SDL_Color edge); // slanted-corner panel
    void ArcSegment(float cx, float cy, float r0, float r1, float a0, float a1, SDL_Color c);
    // A smooth arc band (many segments) with a colour gradient from c0 at a0 to c1 at a1; angles clockwise from +x on the screen.
    void Arc(float cx, float cy, float r0, float r1, float a0, float a1, SDL_Color c0, SDL_Color c1);
    void Ring(float cx, float cy, float r0, float r1, SDL_Color c) { Arc(cx, cy, r0, r1, 0.0f, 6.2831853f, c, c); }
    void DrawSpeedo(int w, int h, const Hud &hud, float seconds);
    void Glow(float cx, float cy, float radius, SDL_Color c);
};
