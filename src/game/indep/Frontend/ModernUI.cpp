#include "game/indep/Frontend/ModernUI.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace {
constexpr float kPi = 3.14159265f;
const SDL_Color kAccent{70, 190, 255, 255};   // the blue-and-white of the flag, brightened for a screen
const SDL_Color kWarm{255, 150, 40, 255};
const SDL_Color kRed{255, 60, 55, 255};
const SDL_Color kWhite{245, 248, 252, 255};

std::string FontPath()
{
    if (const char *d = getenv("NFSU2_ASSETS_UI"))
        return std::string(d) + "/Rajdhani-Bold.ttf";
    for (const char *c : {"assets/ui", "../assets/ui", "../../assets/ui"}) {
        std::string p = std::string(c) + "/Rajdhani-Bold.ttf";
        if (FILE *f = fopen(p.c_str(), "rb")) {
            fclose(f);
            return p;
        }
    }
    return "assets/ui/Rajdhani-Bold.ttf";
}

SDL_Color Mix(SDL_Color a, SDL_Color b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return {Uint8(a.r + (b.r - a.r) * t), Uint8(a.g + (b.g - a.g) * t), Uint8(a.b + (b.b - a.b) * t), Uint8(a.a + (b.a - a.a) * t)};
}
SDL_Color Alpha(SDL_Color c, float a) { return {c.r, c.g, c.b, Uint8(std::clamp(a, 0.0f, 1.0f) * 255.0f)}; }
} // namespace

ModernUI::~ModernUI()
{
    if (m_font)
        SDL_DestroyTexture(m_font);
    if (m_glow)
        SDL_DestroyTexture(m_glow);
}

bool ModernUI::Init(SDL_Renderer *r)
{
    m_r = r;
    FILE *f = fopen(FontPath().c_str(), "rb");
    if (!f) {
        printf("[ModernUI] font not found (%s): classic UI only\n", FontPath().c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> ttf((size_t(n)));
    if (fread(ttf.data(), 1, size_t(n), f) != size_t(n)) {
        fclose(f);
        return false;
    }
    fclose(f);
    m_atlasW = 1024, m_atlasH = 1024;
    std::vector<unsigned char> alpha(size_t(m_atlasW) * m_atlasH);
    stbtt_bakedchar baked[96];
    if (stbtt_BakeFontBitmap(ttf.data(), 0, kBakePx, alpha.data(), m_atlasW, m_atlasH, 32, 96, baked) <= 0) {
        printf("[ModernUI] font bake failed\n");
        return false;
    }
    for (int i = 0; i < 96; ++i)
        m_glyphs[i] = {float(baked[i].x0), float(baked[i].y0), float(baked[i].x1), float(baked[i].y1), baked[i].xoff, baked[i].yoff, baked[i].xadvance};
    std::vector<Uint32> px(alpha.size());
    for (size_t i = 0; i < alpha.size(); ++i)
        px[i] = 0x00ffffffu | (Uint32(alpha[i]) << 24);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    m_font = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, m_atlasW, m_atlasH);
    SDL_UpdateTexture(m_font, nullptr, px.data(), m_atlasW * 4);
    SDL_SetTextureBlendMode(m_font, SDL_BLENDMODE_BLEND);
    // a soft round glow (radial alpha falloff)
    const int G = 128;
    std::vector<Uint32> g(G * G);
    for (int y = 0; y < G; ++y)
        for (int x = 0; x < G; ++x) {
            const float dx = (x + 0.5f) / G * 2 - 1, dy = (y + 0.5f) / G * 2 - 1;
            const float d = std::sqrt(dx * dx + dy * dy);
            const float a = std::pow(std::max(0.0f, 1.0f - d), 2.2f);
            g[y * G + x] = 0x00ffffffu | (Uint32(a * 255.0f) << 24);
        }
    m_glow = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, G, G);
    SDL_UpdateTexture(m_glow, nullptr, g.data(), G * 4);
    SDL_SetTextureBlendMode(m_glow, SDL_BLENDMODE_ADD);
    printf("[ModernUI] ready (%s)\n", FontPath().c_str());
    return true;
}

float ModernUI::TextWidth(const std::string &s, float px) const
{
    float w = 0;
    for (unsigned char c : s)
        if (c >= 32 && c < 128)
            w += m_glyphs[c - 32].xadv;
    return w * px / kBakePx;
}

void ModernUI::Text(float x, float y, float px, const std::string &s, SDL_Color col, int align)
{
    if (!m_font)
        return;
    const float k = px / kBakePx;
    const float w = TextWidth(s, px);
    float cx = align == 1 ? x - w * 0.5f : align == 2 ? x - w : x;
    const float base = y + px * 0.78f; // baseline from the top of the line
    SDL_SetTextureColorMod(m_font, col.r, col.g, col.b);
    SDL_SetTextureAlphaMod(m_font, col.a);
    for (unsigned char c : s) {
        if (c < 32 || c >= 128)
            continue;
        const Glyph &g = m_glyphs[c - 32];
        const SDL_Rect src{int(g.x0), int(g.y0), int(g.x1 - g.x0), int(g.y1 - g.y0)};
        const SDL_FRect dst{cx + g.xoff * k, base + g.yoff * k, (g.x1 - g.x0) * k, (g.y1 - g.y0) * k};
        SDL_RenderCopyF(m_r, m_font, &src, &dst);
        cx += g.xadv * k;
    }
}

void ModernUI::Quad(SDL_FPoint a, SDL_FPoint b, SDL_FPoint c, SDL_FPoint d, SDL_Color ca, SDL_Color cb, SDL_Color cc, SDL_Color cd)
{
    const SDL_Vertex v[4] = {{a, ca, {0, 0}}, {b, cb, {0, 0}}, {c, cc, {0, 0}}, {d, cd, {0, 0}}};
    const int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(m_r, nullptr, v, 4, idx, 6);
}

void ModernUI::Rect(float x, float y, float w, float h, SDL_Color top, SDL_Color bottom)
{
    Quad({x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}, top, top, bottom, bottom);
}

// A panel whose top-right and bottom-left corners are cut at 45 degrees (the "sport" silhouette), a vertical tint gradient
// and a bright 2 px rim on the cut edges.
void ModernUI::Glass(float x, float y, float w, float h, float cut, SDL_Color tint, SDL_Color edge)
{
    const SDL_Color top = tint, bot = Mix(tint, {0, 0, 0, tint.a}, 0.5f);
    const SDL_Vertex v[6] = {{{x, y}, top, {0, 0}},          {{x + w - cut, y}, top, {0, 0}}, {{x + w, y + cut}, Mix(top, bot, cut / h), {0, 0}},
                             {{x + w, y + h}, bot, {0, 0}},  {{x + cut, y + h}, bot, {0, 0}}, {{x, y + h - cut}, Mix(top, bot, 1 - cut / h), {0, 0}}};
    const int idx[12] = {0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5};
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(m_r, nullptr, v, 6, idx, 12);
    SDL_SetRenderDrawColor(m_r, edge.r, edge.g, edge.b, edge.a);
    SDL_RenderDrawLineF(m_r, x, y, x + w - cut, y);
    SDL_RenderDrawLineF(m_r, x + w - cut, y, x + w, y + cut);
    SDL_RenderDrawLineF(m_r, x + w, y + cut, x + w, y + h);
    SDL_RenderDrawLineF(m_r, x + w, y + h, x + cut, y + h);
    SDL_RenderDrawLineF(m_r, x + cut, y + h, x, y + h - cut);
    SDL_RenderDrawLineF(m_r, x, y + h - cut, x, y);
}

void ModernUI::ArcSegment(float cx, float cy, float r0, float r1, float a0, float a1, SDL_Color c)
{
    const float c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1), s1 = std::sin(a1);
    Quad({cx + c0 * r0, cy + s0 * r0}, {cx + c0 * r1, cy + s0 * r1}, {cx + c1 * r1, cy + s1 * r1}, {cx + c1 * r0, cy + s1 * r0}, c, c, c, c);
}

void ModernUI::Glow(float cx, float cy, float radius, SDL_Color c)
{
    if (!m_glow)
        return;
    SDL_SetTextureColorMod(m_glow, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(m_glow, c.a);
    const SDL_FRect dst{cx - radius, cy - radius, radius * 2, radius * 2};
    SDL_RenderCopyF(m_r, m_glow, nullptr, &dst);
}

void ModernUI::DrawBackdrop(int w, int h, float t)
{
    const float W = float(w), H = float(h);
    // deep midnight gradient
    Rect(0, 0, W, H, {6, 12, 28, 255}, {2, 4, 10, 255});
    // slow drifting colour pools: Mediterranean blue, desert amber, and a violet dusk
    Glow(W * (0.22f + 0.05f * std::sin(t * 0.13f)), H * (0.30f + 0.04f * std::cos(t * 0.11f)), W * 0.55f, {20, 90, 200, 120});
    Glow(W * (0.82f + 0.04f * std::cos(t * 0.09f)), H * (0.78f + 0.05f * std::sin(t * 0.15f)), W * 0.50f, {230, 120, 30, 90});
    Glow(W * (0.55f + 0.06f * std::sin(t * 0.07f)), H * 0.10f, W * 0.40f, {110, 40, 170, 70});
    // light streaks sweeping across (speed lines), a few per second
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < 9; ++i) {
        const float ph = std::fmod(t * (0.10f + 0.03f * (i % 4)) + i * 0.173f, 1.0f);
        const float y = H * (0.12f + 0.80f * std::fmod(i * 0.381f, 1.0f));
        const float len = W * (0.18f + 0.14f * (i % 3));
        const float x = -len + (W + len * 2) * ph;
        const float a = std::sin(ph * kPi);
        const SDL_Color c = i % 3 == 0 ? kWarm : kAccent;
        Quad({x, y}, {x + len, y}, {x + len, y + 2}, {x, y + 2}, Alpha(c, 0.0f), Alpha(c, 0.45f * a), Alpha(c, 0.45f * a), Alpha(c, 0.0f));
    }
    // fine horizon grid for depth
    const float hy = H * 0.74f;
    for (int i = -12; i <= 12; ++i) {
        const float x0 = W * 0.5f + i * W * 0.012f, x1 = W * 0.5f + i * W * 0.11f;
        SDL_SetRenderDrawColor(m_r, 70, 150, 255, 22);
        SDL_RenderDrawLineF(m_r, x0, hy, x1, H);
    }
    for (int i = 1; i < 9; ++i) {
        const float k = std::pow(float(i) / 9.0f, 2.0f);
        SDL_SetRenderDrawColor(m_r, 70, 150, 255, Uint8(14 + 22 * k));
        SDL_RenderDrawLineF(m_r, 0, hy + (H - hy) * k, W, hy + (H - hy) * k);
    }
    // vignette
    Rect(0, 0, W, H * 0.22f, {0, 0, 0, 150}, {0, 0, 0, 0});
    Rect(0, H * 0.78f, W, H * 0.22f, {0, 0, 0, 0}, {0, 0, 0, 170});
    Quad({0, 0}, {W * 0.14f, 0}, {W * 0.14f, H}, {0, H}, {0, 0, 0, 150}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 150});
    Quad({W * 0.86f, 0}, {W, 0}, {W, H}, {W * 0.86f, H}, {0, 0, 0, 0}, {0, 0, 0, 150}, {0, 0, 0, 150}, {0, 0, 0, 0});
    // accent line at the very top
    Quad({0, 0}, {W, 0}, {W, 3}, {0, 3}, kAccent, kWarm, kWarm, kAccent);
}

void ModernUI::DrawHud(int w, int h, const Hud &hud, float t)
{
    if (!m_font)
        return;
    const float s = float(h) / 720.0f;
    const SDL_Color glass{8, 14, 28, 150}, edge{120, 190, 255, 120};
    char buf[64];

    // ---- speed cluster, bottom right ----
    const float R = 98 * s, cx = float(w) - 150 * s, cy = float(h) - 140 * s;
    Glow(cx, cy, R * 1.9f, {20, 80, 170, 60});
    Glass(cx - R * 1.35f, cy - R * 1.25f, R * 2.7f, R * 2.5f, 26 * s, glass, edge);
    const float a0 = 0.75f * kPi, sweep = 1.5f * kPi; // 135 deg .. 405 deg: open at the bottom
    const int N = 44;
    const float frac = std::clamp(hud.rpm / std::max(hud.rpmScale, 1000.0f), 0.0f, 1.0f);
    for (int i = 0; i < N; ++i) {
        const float u0 = float(i) / N, u1 = float(i + 0.82f) / N;
        const bool lit = u0 < frac;
        const bool red = u0 > 0.82f;
        SDL_Color c = lit ? (red ? Mix(kWarm, kRed, (u0 - 0.82f) / 0.18f) : Mix(kAccent, {255, 255, 255, 255}, u0 * 0.5f)) : SDL_Color{120, 150, 190, 38};
        const float shorten = (i % 4 == 0) ? 0.0f : 6 * s; // every fourth tick is longer
        ArcSegment(cx, cy, R - 16 * s + shorten, R, a0 + sweep * u0, a0 + sweep * u1, c);
    }
    // numerals at the major ticks (thousands of rpm)
    const int majors = int(hud.rpmScale / 1000.0f);
    for (int m = 0; m <= majors; ++m) {
        const float u = float(m * 1000) / hud.rpmScale;
        if (u > 1.0f)
            break;
        const float a = a0 + sweep * u;
        snprintf(buf, sizeof(buf), "%d", m);
        Text(cx + std::cos(a) * (R + 14 * s), cy + std::sin(a) * (R + 14 * s) - 8 * s, 16 * s, buf, {200, 220, 245, 170}, 1);
    }
    const float spd = hud.mph ? hud.speedKmh * 0.621371f : hud.speedKmh;
    snprintf(buf, sizeof(buf), "%d", int(std::fabs(spd) + 0.5f));
    Text(cx, cy - 56 * s, 96 * s, buf, kWhite, 1);
    Text(cx, cy + 40 * s, 22 * s, hud.mph ? "MPH" : "KM/H", {160, 205, 255, 230}, 1);
    // gear
    const char *g = hud.gear < 0 ? "R" : hud.gear == 0 ? "N" : nullptr;
    if (g)
        snprintf(buf, sizeof(buf), "%s", g);
    else
        snprintf(buf, sizeof(buf), "%d", hud.gear);
    Glass(cx - 24 * s, cy + 66 * s, 48 * s, 40 * s, 10 * s, {20, 60, 110, 190}, {120, 200, 255, 200});
    Text(cx, cy + 66 * s, 40 * s, buf, frac > 0.9f ? kWarm : kWhite, 1);
    // nitrous bar under the cluster
    if (hud.hasNitrous) {
        const float bw = R * 2.3f, bx = cx - bw * 0.5f, by = cy + R * 1.0f + 12 * s;
        Rect(bx, by, bw, 8 * s, {255, 255, 255, 30}, {255, 255, 255, 30});
        const float f = std::clamp(hud.nitrous, 0.0f, 1.0f);
        const SDL_Color nc = hud.nitrousActive ? SDL_Color{120, 255, 255, 255} : SDL_Color{60, 160, 255, 255};
        Quad({bx, by}, {bx + bw * f, by}, {bx + bw * f, by + 8 * s}, {bx, by + 8 * s}, nc, Mix(nc, kWhite, 0.5f), Mix(nc, kWhite, 0.5f), nc);
        Text(bx, by - 18 * s, 14 * s, "NOS", {160, 215, 255, 220}, 0);
    }

    if (!hud.race)
        return;
    // ---- race info, top left: position and lap ----
    const float px = 28 * s, py = 24 * s;
    Glass(px, py, 190 * s, 74 * s, 16 * s, glass, edge);
    snprintf(buf, sizeof(buf), "%d", hud.position);
    Text(px + 18 * s, py + 2 * s, 66 * s, buf, hud.position == 1 ? SDL_Color{255, 214, 80, 255} : kWhite, 0);
    const char *ord = hud.position == 1 ? "ST" : hud.position == 2 ? "ND" : hud.position == 3 ? "RD" : "TH";
    Text(px + 18 * s + TextWidth(buf, 66 * s) + 2 * s, py + 8 * s, 24 * s, ord, {200, 225, 255, 230}, 0);
    snprintf(buf, sizeof(buf), "/ %d", hud.racers);
    Text(px + 170 * s, py + 40 * s, 22 * s, buf, {160, 205, 255, 220}, 2);
    Glass(px, py + 82 * s, 190 * s, 40 * s, 12 * s, glass, edge);
    snprintf(buf, sizeof(buf), "LAP %d / %d", std::min(hud.lap, hud.laps), hud.laps);
    Text(px + 16 * s, py + 86 * s, 28 * s, buf, kWhite, 0);

    // ---- timer, top centre ----
    const int ms = int(hud.lapTime * 1000.0f);
    snprintf(buf, sizeof(buf), "%02d:%02d.%02d", ms / 60000, (ms / 1000) % 60, (ms / 10) % 100);
    const float tw = 210 * s;
    Glass(float(w) * 0.5f - tw * 0.5f, py, tw, 54 * s, 14 * s, glass, edge);
    Text(float(w) * 0.5f, py + 4 * s, 44 * s, buf, kWhite, 1);
    if (hud.bestLap > 0.0f) {
        const int bm = int(hud.bestLap * 1000.0f);
        snprintf(buf, sizeof(buf), "BEST %02d:%02d.%02d", bm / 60000, (bm / 1000) % 60, (bm / 10) % 100);
        Text(float(w) * 0.5f, py + 58 * s, 18 * s, buf, {255, 214, 80, 235}, 1);
    }
    (void)t;
}

void ModernUI::DrawDialog(int w, int h, const std::string &title, const std::vector<std::string> &items, int selected, bool confirm,
                          const std::string &question, bool confirmYes, float t)
{
    if (!m_font)
        return;
    const float s = float(h) / 720.0f, W = float(w), H = float(h);
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    Rect(0, 0, W, H, {2, 6, 16, 150}, {2, 6, 16, 190}); // dim the scene
    const float pw = 460 * s, ph = (confirm ? 250.0f : 130.0f + 62.0f * float(items.size())) * s;
    const float px = W * 0.5f - pw * 0.5f, py = H * 0.5f - ph * 0.5f;
    Glow(W * 0.5f, H * 0.5f, pw * 1.1f, {20, 80, 170, 60});
    Glass(px, py, pw, ph, 28 * s, {8, 14, 28, 225}, {120, 190, 255, 170});
    Quad({px, py}, {px + pw - 28 * s, py}, {px + pw - 28 * s, py + 3 * s}, {px, py + 3 * s}, kAccent, kWarm, kWarm, kAccent);
    Text(px + 30 * s, py + 18 * s, 40 * s, title, kWhite, 0);
    char buf[16];
    (void)buf;
    if (!confirm) {
        for (size_t i = 0; i < items.size(); ++i) {
            const float ry = py + 84 * s + 62 * s * float(i);
            const bool on = int(i) == selected;
            if (on) {
                const float pulse = 0.75f + 0.25f * std::sin(t * 6.0f);
                Glass(px + 20 * s, ry, pw - 40 * s, 52 * s, 14 * s, {30, 100, 190, Uint8(150 + 50 * pulse)}, {150, 215, 255, 230});
                Quad({px + 20 * s, ry + 6 * s}, {px + 26 * s, ry + 6 * s}, {px + 26 * s, ry + 46 * s}, {px + 20 * s, ry + 46 * s}, kWarm, kWarm, kWarm, kWarm);
            }
            Text(px + 50 * s, ry + 6 * s, 34 * s, items[i], on ? kWhite : SDL_Color{175, 200, 230, 235}, 0);
        }
        Text(px + pw * 0.5f, py + ph - 34 * s, 17 * s, "UP / DOWN select    ENTER confirm    ESC resume", {140, 175, 215, 220}, 1);
    } else {
        Text(px + pw * 0.5f, py + 84 * s, 30 * s, question, kWhite, 1);
        Text(px + pw * 0.5f, py + 122 * s, 20 * s, "Your race progress will be lost.", {170, 200, 235, 230}, 1);
        for (int i = 0; i < 2; ++i) {
            const bool on = (i == 0) == confirmYes;
            const float bx = px + 40 * s + i * (pw * 0.5f - 20 * s), bw = pw * 0.5f - 60 * s, by = py + 160 * s;
            Glass(bx, by, bw, 54 * s, 14 * s, on ? SDL_Color{30, 100, 190, 220} : SDL_Color{12, 20, 40, 200}, on ? SDL_Color{150, 215, 255, 230} : SDL_Color{80, 120, 170, 120});
            Text(bx + bw * 0.5f, by + 8 * s, 34 * s, i == 0 ? "YES" : "NO", on ? kWhite : SDL_Color{175, 200, 230, 235}, 1);
        }
    }
}
