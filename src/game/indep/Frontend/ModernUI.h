#pragma once
// A modern native UI layer drawn over / under the original FEng screens (ISSUES #276): an animated full-window menu
// backdrop and a redesigned race HUD (arc rev meter, big speed readout, glass info pills) in the Rajdhani typeface
// (SIL OFL, assets/ui).  Everything draws with the SDL renderer in window pixels (logical size 0).
#include <map>
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
    // The main menu (ISSUES #357): drawn over the 3D showroom, replacing the FEng icon carousel.  Flat and high contrast: a column of big
    // caps with a hard-edged yellow highlight on the selected row, the player's standing bottom left, the car's name bottom right.
    struct MainMenu {
        const char *const *items = nullptr;
        const char *const *notes = nullptr; // one line under the list for the selected item
        int count = 0, selected = 0;
        bool enabledOnline = false;         // the row index 3 (Online) is shown dimmed when false
        std::string kicker = "NEED FOR SPEED", title = "UNDERGROUND 2"; // the wordmark (the garage uses its own)
        const bool *disabled = nullptr;      // per row: dimmed (nullptr: only Online on the main menu)
        std::string player, car, carSub;
        int money = -1, reputation = -1;    // < 0: not shown
    };
    void DrawMainMenu(int w, int h, const MainMenu &m, float seconds);
    // The post-race results and reward screens (ISSUES #362).
    struct ResultRow {
        std::string name, car, time;
        bool player = false;
    };
    struct Results {
        std::vector<ResultRow> rows;
        int place = 0;               // the player's finishing place (0: unknown)
        std::string heading = "RACE COMPLETE", detail;   // detail: best lap / score line
        bool reward = false;         // the reward card instead of the table
        int money = 0, reputation = 0;
        std::string player;
    };
    void DrawResults(int w, int h, const Results &r, float seconds);
    // The race-start countdown (ISSUES #363): `left` seconds to go (3 .. 0), or `goAge` seconds since the green light (< 0: none).
    void DrawCountdown(int w, int h, float left, float goAge);
    // The Quick Race track select (ISSUES #367): a scrolling list of tracks with lock marks and a detail panel for the highlighted one.
    struct TrackSelect {
        std::vector<std::string> names;
        std::vector<char> locked;
        int selected = 0;
        std::string title, type, distance, bestLabel, best, region;
    };
    void DrawTrackSelect(int w, int h, const TrackSelect &t, float seconds);
    // A settings list (ISSUES #368): rows of "label ... < value >", the highlighted row on the yellow blade.
    struct Settings {
        std::string kicker, title;
        std::vector<std::string> labels, values;
        int selected = 0;
        std::string player;
        int money = -1, reputation = -1;
    };
    void DrawSettings(int w, int h, const Settings &st, float seconds);
    // The car select panel (ISSUES #369): the car over the 3D stage, its make and model large, three stat bars, the roster position.
    struct CarSelect {
        std::string kicker, title, make, model, group;
        float stats[3] = {0.5f, 0.5f, 0.5f};
        int index = 0, count = 0;
        std::string player;
        bool customize = true; // show the Customize key
    };
    void DrawCarSelect(int w, int h, const CarSelect &c, float seconds);
    // The parts shop (ISSUES #371): the category column on the left, a bottom bar with the highlighted part, its price, the total and the cash.
    struct Shop {
        std::string kicker, title, name, price, total;
        std::vector<std::string> categories;
        int category = 0, index = 0, count = 0, money = 0;
        bool fitted = false;
    };
    void DrawShop(int w, int h, const Shop &sh, float seconds);
    // The paint shop (ISSUES #372): the part list on the left, the colour grid along the bottom with the selected swatch ringed.
    struct PaintShop {
        std::vector<std::string> parts;
        std::vector<uint32_t> colours;
        int part = 0, selected = 0, money = 0;
        std::string finish, price;
        bool grid = false;
    };
    void DrawPaintShop(int w, int h, const PaintShop &p, float seconds);
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
    // The free-roam event prompt (ISSUES #303): a glass panel over the dimmed street with the event's type, track, status and
    // purse and two buttons (selected 0 = Accept, 1 = Decline).
    void DrawEngage(int w, int h, std::string_view kicker, std::string_view title, const char *const *lines, int lineCount, int selected,
                    float seconds);
    // The police pursuit (ISSUES #318): heat, units, bounty and clock at the top, the BUSTED and COOLDOWN bars at the bottom, a flashing red / blue
    // glow at the screen edges while the cops are close, and a banner (PURSUIT / HEAT LEVEL n / ROADBLOCK).  state: 1 chase, 2 cooldown,
    // 3 busted, 4 escaped.
    struct PursuitHud {
        int state = 0, heat = 0, units = 0;
        float bounty = 0, chaseTime = 0, bust = 0, cooldown = 0, nearest = 1e9f, banner = 0, resultTimer = 0, amount = 0;
        const char *bannerText = "", *bannerSub = "";
    };
    void DrawPursuit(int w, int h, const PursuitHud &p, float seconds);
    // A message toast (ISSUES #308): a glass card sliding in from the top with a kicker, a title and a wrapped body; `age` is seconds since it
    // appeared, `life` how long it stays.
    void DrawToast(int w, int h, std::string_view kicker, std::string_view title, std::string_view body, float age, float life);
    // A magazine cover on show (ISSUES #309): dims the scene, lights and frames the cover `page` (window pixels) and writes the captions around it.
    void DrawCoverFrame(int w, int h, const SDL_Rect &page, std::string_view kicker, std::string_view title, std::string_view detail, std::string_view hint,
                        float seconds);
    // Screen transitions (ISSUES #311).  CaptureFrame keeps what the window shows now (call with the logical size reset, after drawing the frame
    // to hide); DrawWipe then draws that frame over whatever has been drawn since, cut away by a slanted, glowing light edge that sweeps left to
    // right as t goes 0 -> 1; DrawFade lays black over the frame (alpha 0..1).
    void CaptureFrame();
    // The garage's shot title (ISSUES #359): a slanted yellow blade sweeps across the frame, the title cuts in behind it and holds, then fades.
    void DrawShotTitle(int w, int h, const char *title, const char *sub, float age, float dir, bool entry);
    void DrawWipe(int w, int h, float t);
    void DrawFade(int w, int h, float alpha);
    // The profiler's overlay (a corner panel of monospaced-looking lines).
    // A debug panel of text lines, top right by default (the profiler) or top left (the memory telemetry, #331).
    void DrawOverlay(int w, int h, const char lines[][96], int count, bool left = false);
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
    // the animated loading backdrop (ISSUES #310): JPEG frames assets/ui/loading/f001.jpg ... played back and forth at 16 fps with a cross-fade
    std::string m_frameDir;
    int m_frameCount = 0;
    std::map<int, SDL_Texture *> m_frames; // the most recent few
    SDL_Texture *LoadingFrame(int index);
    SDL_Texture *m_capture = nullptr;
    int m_capW = 0, m_capH = 0;
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
