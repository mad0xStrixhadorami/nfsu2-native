#include "game/indep/Frontend/ModernUI.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "stb_image.h"

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
    if (m_map)
        SDL_DestroyTexture(m_map);
    if (m_loadArt)
        SDL_DestroyTexture(m_loadArt);
    for (auto &f : m_frames)
        if (f.second)
            SDL_DestroyTexture(f.second);
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
    // the loading-screen art (ISSUES #301), next to the font
    {
        std::string art = FontPath();
        art = art.substr(0, art.rfind('/') + 1) + "loading.jpg";
        int iw = 0, ih = 0, comp = 0;
        if (unsigned char *img = stbi_load(art.c_str(), &iw, &ih, &comp, 4)) {
            m_loadArt = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, iw, ih);
            SDL_UpdateTexture(m_loadArt, nullptr, img, iw * 4);
            SDL_SetTextureBlendMode(m_loadArt, SDL_BLENDMODE_NONE);
            SDL_SetTextureScaleMode(m_loadArt, SDL_ScaleModeLinear);
            m_loadW = iw, m_loadH = ih;
            stbi_image_free(img);
        } else {
            printf("[ModernUI] loading art not found (%s)\n", art.c_str());
        }
    }
    {   // the animated backdrop: count the consecutive frames
        std::string dir = FontPath();
        dir = dir.substr(0, dir.rfind('/') + 1) + "loading/";
        m_frameDir = dir;
        char name[600];
        for (int i = 1; i < 2000; ++i) {
            snprintf(name, sizeof(name), "%sf%03d.jpg", dir.c_str(), i);
            FILE *f = fopen(name, "rb");
            if (!f)
                break;
            fclose(f);
            ++m_frameCount;
        }
        if (m_frameCount >= 2 && !getenv("NFSU2_STILL_LOADING")) {
            if (SDL_Texture *first = LoadingFrame(0)) {
                Uint32 fmt;
                int acc;
                SDL_QueryTexture(first, &fmt, &acc, &m_loadW, &m_loadH);
            }
            printf("[ModernUI] animated loading backdrop: %d frames\n", m_frameCount);
        } else {
            m_frameCount = 0;
        }
    }
    printf("[ModernUI] ready (%s)\n", FontPath().c_str());
    return true;
}

float ModernUI::TextWidth(std::string_view s, float px) const
{
    float w = 0;
    for (unsigned char c : s)
        if (c >= 32 && c < 128)
            w += m_glyphs[c - 32].xadv;
    return w * px / kBakePx;
}

void ModernUI::Text(float x, float y, float px, std::string_view s, SDL_Color col, int align)
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

void ModernUI::Arc(float cx, float cy, float r0, float r1, float a0, float a1, SDL_Color c0, SDL_Color c1)
{
    const int steps = std::max(2, int(std::ceil(std::fabs(a1 - a0) / 0.09f)));
    auto mix = [&](float t) {
        return SDL_Color{Uint8(c0.r + (c1.r - c0.r) * t), Uint8(c0.g + (c1.g - c0.g) * t), Uint8(c0.b + (c1.b - c0.b) * t), Uint8(c0.a + (c1.a - c0.a) * t)};
    };
    for (int i = 0; i < steps; ++i) {
        const float ta = float(i) / steps, tb = float(i + 1) / steps;
        const float aa = a0 + (a1 - a0) * ta, ab = a0 + (a1 - a0) * tb;
        const SDL_Color ca = mix(ta), cb = mix(tb);
        Quad({cx + std::cos(aa) * r0, cy + std::sin(aa) * r0}, {cx + std::cos(aa) * r1, cy + std::sin(aa) * r1},
             {cx + std::cos(ab) * r1, cy + std::sin(ab) * r1}, {cx + std::cos(ab) * r0, cy + std::sin(ab) * r0}, ca, ca, cb, cb);
    }
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

void ModernUI::DrawWind(int w, int h, float strength, float nitro, float t)
{
    if (strength <= 0.02f && nitro <= 0.02f)
        return;
    const float cx = float(w) * 0.5f, cy = float(h) * 0.5f, diag = std::hypot(cx, cy);
    const int lines = 34 + int(strength * 34.0f + nitro * 30.0f);
    for (int i = 0; i < lines; ++i) {
        // a deterministic hash per streak: angle, phase, speed, thickness
        uint32_t k = uint32_t(i) * 2654435761u + 12345u;
        auto rnd = [&k]() { k ^= k << 13; k ^= k >> 17; k ^= k << 5; return float(k & 0xffff) / 65535.0f; };
        const float ang = rnd() * 6.2831853f, ph = rnd(), sp = 0.55f + rnd() * 0.9f, th = 1.2f + rnd() * 2.2f;
        const float u = std::fmod(ph + t * sp * (0.8f + 1.4f * strength), 1.0f); // 0 (inner) .. 1 (off the edge)
        const float r0 = diag * (0.52f + 0.52f * u * u), len = diag * (0.05f + 0.16f * strength + 0.10f * nitro) * (0.4f + u);
        const float r1 = r0 + len;
        const float edge = std::clamp((u - 0.05f) * 3.0f, 0.0f, 1.0f) * std::clamp((1.0f - u) * 4.0f, 0.0f, 1.0f);
        const float alpha = (0.10f + 0.22f * strength + 0.16f * nitro) * edge;
        const SDL_Color c = nitro > 0.3f ? Mix({210, 235, 255, 255}, {110, 190, 255, 255}, nitro) : SDL_Color{225, 238, 250, 255};
        const float ca = std::cos(ang), sa = std::sin(ang), nx = -sa * th * 0.5f, ny = ca * th * 0.5f;
        Quad({cx + ca * r0 + nx, cy + sa * r0 + ny}, {cx + ca * r0 - nx, cy + sa * r0 - ny},
             {cx + ca * r1 - nx * 0.2f, cy + sa * r1 - ny * 0.2f}, {cx + ca * r1 + nx * 0.2f, cy + sa * r1 + ny * 0.2f},
             Alpha(c, 0.0f), Alpha(c, 0.0f), Alpha(c, alpha), Alpha(c, alpha));
    }
}

void ModernUI::DrawMinimap(int w, int h, const Hud &hud, float t)
{
    if (!m_font || !hud.mapPixels || hud.mapSize <= 0)
        return;
    if (!m_map || m_mapTexSize != hud.mapSize) {
        if (m_map)
            SDL_DestroyTexture(m_map);
        m_map = SDL_CreateTexture(m_r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, hud.mapSize, hud.mapSize);
        m_mapTexSize = hud.mapSize;
        if (m_map)
            SDL_SetTextureBlendMode(m_map, SDL_BLENDMODE_BLEND);
    }
    if (!m_map)
        return;
    if (hud.mapNew)
        SDL_UpdateTexture(m_map, nullptr, hud.mapPixels, hud.mapSize * 4);
    // NFS Unbound's map: a frosted glass disc (the scene shows through), a thin white double ring with a colour fringe, a yellow arrow with
    // green rings, and the event name in a wide face underneath.
    const float s = float(h) / 720.0f;
    const float R = 92.0f * s, cx = 30.0f * s + R + 18.0f * s, cy = float(h) - 78.0f * s - R;
    const SDL_FRect dst{cx - R, cy - R, R * 2.0f, R * 2.0f};
    SDL_RenderCopyF(m_r, m_map, nullptr, &dst);
    // a soft inner shade towards the rim
    for (int i = 0; i < 4; ++i)
        Ring(cx, cy, R - float(i + 1) * 4.2f * s, R - float(i) * 4.2f * s, {0, 0, 0, Uint8(7 + (3 - i) * 2)});
    Ring(cx + 1.3f * s, cy, R - 0.6f * s, R + 0.9f * s, {255, 70, 90, 60});  // chromatic fringe
    Ring(cx - 1.3f * s, cy, R - 0.6f * s, R + 0.9f * s, {70, 220, 255, 60});
    Ring(cx, cy, R - 0.9f * s, R + 1.2f * s, {250, 252, 255, 238});
    Ring(cx, cy, R + 4.2f * s, R + 5.0f * s, {235, 240, 250, 70});
    // The player: a yellow arrow with a green outline and two green rings behind it (up when the map turns with the car).
    const float psi = hud.mapRotate ? 0.0f : 1.5707963f - hud.mapYaw; // clockwise from up
    auto rot = [&](float x, float y, float k) { return SDL_FPoint{cx + (x * std::cos(psi) - y * std::sin(psi)) * s * k, cy + (x * std::sin(psi) + y * std::cos(psi)) * s * k}; };
    Ring(cx, cy - 5.0f * s, 12.0f * s, 13.6f * s, {90, 255, 80, 215});
    Ring(cx, cy - 15.0f * s, 10.0f * s, 11.4f * s, {90, 255, 80, 170});
    auto arrow = [&](float k, SDL_Color c) {
        const SDL_FPoint tip = rot(0, -13, k), l = rot(-9.5f, 10, k), n = rot(0, 4.5f, k), r = rot(9.5f, 10, k);
        Quad(tip, l, n, tip, c, c, c, c);
        Quad(tip, n, r, tip, c, c, c, c);
    };
    arrow(1.28f, {70, 235, 60, 255});
    arrow(1.0f, {255, 226, 58, 255});
    // The label: the event type with the same colour fringe as the ring, and the distance (or the map scale) under it.
    const float ly = cy + R + 14.0f * s;
    const char *name = hud.eventName && hud.eventName[0] ? hud.eventName : "FREE ROAM";
    Text(cx - 1.5f * s, ly, 24.0f * s, name, {70, 220, 255, 120}, 1);
    Text(cx + 1.5f * s, ly, 24.0f * s, name, {255, 70, 110, 120}, 1);
    Text(cx, ly, 24.0f * s, name, {250, 252, 255, 250}, 1);
    char buf[48];
    if (hud.finishKm >= 0.0f)
        snprintf(buf, sizeof buf, "%.1f KM TO GO", hud.finishKm);
    else
        snprintf(buf, sizeof buf, "%d M", int(hud.mapMetres + 0.5f));
    Text(cx, ly + 26.0f * s, 15.0f * s, buf, hud.finishKm >= 0 ? SDL_Color{255, 226, 90, 235} : SDL_Color{205, 225, 245, 210}, 1);
    (void)t;
}

// NFS Unbound's dial: a dark glass disc with the speed in padded digits, "mph" and a gear tile; around it an rpm scale in thousands (a
// red zone at the end), a bright white arc outside showing the revs, and to the left a quarter-circle gauge with a needle for the nitrous.
void ModernUI::DrawSpeedo(int w, int h, const Hud &hud, float t)
{
    const float s = float(h) / 720.0f;
    const float Rout = 100.0f * s, Rdisc = 77.0f * s, cx = float(w) - 152.0f * s, cy = float(h) - 138.0f * s;
    constexpr float kPi = 3.14159265f;
    const float a0 = 145.0f * kPi / 180.0f, sweep = 240.0f * kPi / 180.0f; // 0 at the lower left, clockwise over the top to the lower right
    const float frac = std::clamp(hud.rpm / std::max(hud.rpmScale, 1000.0f), 0.0f, 1.0f);
    const float redStart = 0.82f;
    char buf[64];
    // while the nitrous burns the dial turns lime, as in NFS Unbound's boost
    const bool boost = hud.nitrousActive;
    const SDL_Color ink = boost ? SDL_Color{196, 255, 72, 255} : SDL_Color{250, 252, 255, 255};
    Glow(cx, cy, Rout * 1.7f, boost ? SDL_Color{110, 190, 30, 70} : SDL_Color{30, 50, 90, 46});
    // the disc and the scale band (a darker red band at the end)
    Arc(cx, cy, 0.0f, Rdisc, 0.0f, 6.2831853f, {14, 16, 22, 222}, {14, 16, 22, 222});
    Arc(cx, cy, Rdisc, Rout, a0, a0 + sweep * redStart, {44, 60, 88, 150}, {44, 60, 88, 150});
    Arc(cx, cy, Rdisc, Rout, a0 + sweep * redStart, a0 + sweep, {150, 26, 32, 215}, {150, 26, 32, 215});
    if (frac > 0.0f) // the part of the band the revs have reached is a little brighter
        Arc(cx, cy, Rdisc, Rout, a0, a0 + sweep * std::min(frac, redStart), {70, 96, 138, 120}, {88, 118, 170, 150});
    // rings with the colour fringe of the original's glass
    Ring(cx + 1.2f * s, cy, Rout - 0.6f * s, Rout + 0.8f * s, {255, 70, 90, 55});
    Ring(cx - 1.2f * s, cy, Rout - 0.6f * s, Rout + 0.8f * s, {70, 220, 255, 55});
    Ring(cx, cy, Rout - 0.8f * s, Rout + 1.0f * s, {ink.r, ink.g, ink.b, 225});
    Ring(cx, cy, Rdisc - 0.8f * s, Rdisc + 0.8f * s, {ink.r, ink.g, ink.b, 210});
    // the rpm numerals (thousands)
    const int majors = int(hud.rpmScale / 1000.0f);
    for (int m = 0; m <= majors; ++m) {
        const float u = float(m * 1000) / hud.rpmScale;
        if (u > 1.0f)
            break;
        const float a = a0 + sweep * u, r = (Rdisc + Rout) * 0.5f;
        snprintf(buf, sizeof buf, "%d", m);
        Text(cx + std::cos(a) * r, cy + std::sin(a) * r - 8.0f * s, 15.0f * s, buf, u > redStart ? SDL_Color{255, 215, 215, 235} : SDL_Color{225, 235, 248, 215}, 1);
    }
    // the bright white rev arc outside the scale, growing with the revs
    if (frac > 0.003f)
        Arc(cx, cy, Rout + 3.5f * s, Rout + 9.5f * s, a0, a0 + sweep * frac, {ink.r, ink.g, ink.b, 110}, ink);
    // the two small indicator rings under the digits: nitrous burst (cyan) and a drift in progress (amber)
    const bool nos = hud.nitrousActive, drift = hud.drifting;
    Ring(cx - 20.0f * s, cy - 47.0f * s, 9.6f * s, 11.2f * s, {235, 240, 250, 95});
    Ring(cx + 15.0f * s, cy - 54.0f * s, 10.6f * s, 12.2f * s, {235, 240, 250, 95});
    if (nos)
        Arc(cx - 20.0f * s, cy - 47.0f * s, 0.0f, 9.0f * s, 0.0f, 6.2831853f, {110, 235, 255, 150}, {110, 235, 255, 150});
    if (drift)
        Arc(cx + 15.0f * s, cy - 54.0f * s, 0.0f, 10.0f * s, 0.0f, 6.2831853f, {255, 180, 60, 150}, {255, 180, 60, 150});
    // speed: three padded digits in a heavy face (the same text drawn twice, a hair apart, for weight), the unit and the gear tile
    const float spd = hud.mph ? hud.speedKmh * 0.621371f : hud.speedKmh;
    snprintf(buf, sizeof buf, "%03d", std::min(999, int(std::fabs(spd) + 0.5f)));
    Text(cx - 0.8f * s, cy - 40.0f * s, 60.0f * s, buf, ink, 1);
    Text(cx + 0.8f * s, cy - 40.0f * s, 60.0f * s, buf, ink, 1);
    Text(cx, cy + 14.0f * s, 15.0f * s, hud.mph ? "mph" : "km/h", {215, 225, 240, 230}, 1);
    const char *g = hud.gear < 0 ? "R" : hud.gear == 0 ? "N" : nullptr;
    if (g)
        snprintf(buf, sizeof buf, "%s", g);
    else
        snprintf(buf, sizeof buf, "%d", hud.gear);
    const float tile = 31.0f * s;
    Glass(cx - tile * 0.5f, cy + 31.0f * s, tile, tile, 8.0f * s, {205, 208, 214, 235}, {240, 243, 248, 235});
    Text(cx, cy + 31.0f * s + 1.0f * s, 27.0f * s, buf, frac > 0.92f ? SDL_Color{190, 40, 40, 255} : SDL_Color{34, 38, 48, 255}, 1);

    // ---- the nitrous gauge: a quarter circle left of the dial, a needle on its pivot ----
    const float px = cx - 113.0f * s, py = cy + 66.0f * s, ro = 90.0f * s, ri = 70.0f * s;
    const float na0 = kPi, naSweep = 0.5f * kPi; // from the left, clockwise to straight up
    const float fill = hud.hasNitrous ? std::clamp(hud.nitrous, 0.0f, 1.0f) : 0.0f;
    const SDL_Color blue = hud.nitrousActive ? SDL_Color{130, 255, 255, 255} : SDL_Color{58, 150, 255, 245};
    Arc(px, py, ro, ro + 2.4f * s, na0, na0 + naSweep, blue, blue);
    const float gap = 0.05f, seg = (naSweep - 2 * gap) / 3.0f;
    for (int i = 0; i < 3; ++i) {
        const float sa = na0 + gap + float(i) * (seg + gap), sb = sa + seg;
        const float lit = std::clamp(fill * 3.0f - float(i), 0.0f, 1.0f); // how much of this segment is filled
        Arc(px, py, ri, ro - 1.0f * s, sa, sb, {92, 98, 110, 150}, {92, 98, 110, 150});
        if (lit > 0.0f)
            Arc(px, py, ri, ro - 1.0f * s, sa, sa + (sb - sa) * lit, {blue.r, blue.g, blue.b, 230}, {blue.r, blue.g, blue.b, 230});
    }
    const float needle = na0 + naSweep * fill;
    {
        const float dx = std::cos(needle), dy = std::sin(needle), nx = -dy * 3.6f * s, ny = dx * 3.6f * s, len = 62.0f * s;
        Quad({px + dx * 9.0f * s + nx, py + dy * 9.0f * s + ny}, {px + dx * 9.0f * s - nx, py + dy * 9.0f * s - ny},
             {px + dx * len, py + dy * len}, {px + dx * len, py + dy * len}, {255, 255, 255, 235}, {255, 255, 255, 235}, {255, 255, 255, 0}, {255, 255, 255, 0});
    }
    Ring(px + 1.0f * s, py, 7.4f * s, 9.4f * s, {255, 70, 90, 60});
    Ring(px - 1.0f * s, py, 7.4f * s, 9.4f * s, {70, 220, 255, 60});
    Ring(px, py, 7.6f * s, 9.2f * s, {250, 252, 255, 240});
    // the battery with a bolt
    {
        const float bx = px - 36.0f * s, by = py - 52.0f * s, bw = 15.0f * s, bh = 23.0f * s;
        const SDL_Color bc = fill > 0.0f || hud.nitrousActive ? SDL_Color{60, 150, 255, 255} : SDL_Color{110, 125, 150, 200};
        Rect(bx + bw * 0.3f, by - 3.0f * s, bw * 0.4f, 3.0f * s, bc, bc);
        Rect(bx, by, bw, bh, bc, {bc.r, bc.g, Uint8(std::min(255, bc.b + 0)), bc.a});
        const SDL_Color wc{235, 245, 255, 255};
        Quad({bx + bw * 0.62f, by + 3.0f * s}, {bx + bw * 0.28f, by + bh * 0.55f}, {bx + bw * 0.52f, by + bh * 0.55f}, {bx + bw * 0.72f, by + 3.0f * s}, wc, wc, wc, wc);
        Quad({bx + bw * 0.38f, by + bh - 3.0f * s}, {bx + bw * 0.72f, by + bh * 0.45f}, {bx + bw * 0.48f, by + bh * 0.45f}, {bx + bw * 0.28f, by + bh - 3.0f * s}, wc, wc, wc, wc);
    }
    (void)t;
}

void ModernUI::DrawHud(int w, int h, const Hud &hud, float t)
{
    if (!m_font)
        return;
    DrawMinimap(w, h, hud, t);
    const float s = float(h) / 720.0f;
    const SDL_Color glass{8, 14, 28, 150}, edge{120, 190, 255, 120};
    char buf[64];

    DrawSpeedo(w, h, hud, t);

    // ---- drift, lower centre (ISSUES #291): the running chain with its multiplier and the slide angle; the banked points pop up ----
    if (hud.drifting || hud.driftChain > 0.5f || hud.driftBanner > 0.0f) {
        const float dx = float(w) * 0.5f, dy = float(h) * 0.70f;
        if (hud.driftChain > 0.5f) {
            const float pw = 250 * s;
            Glow(dx, dy + 30 * s, pw * 0.7f, {255, 140, 40, 40});
            Glass(dx - pw * 0.5f, dy, pw, 66 * s, 16 * s, {26, 16, 8, 170}, {255, 170, 80, 170});
            Text(dx - pw * 0.5f + 16 * s, dy + 4 * s, 20 * s, "DRIFT", {255, 190, 110, 240}, 0);
            snprintf(buf, sizeof(buf), "%d", int(hud.driftChain * float(hud.driftMult)));
            Text(dx - 4 * s, dy + 12 * s, 48 * s, buf, kWhite, 1);
            snprintf(buf, sizeof(buf), "x%d", hud.driftMult);
            Text(dx + pw * 0.5f - 14 * s, dy + 12 * s, 40 * s, buf, hud.driftMult >= 4 ? SDL_Color{255, 214, 80, 255} : kWarm, 2);
            const float f = std::clamp(std::fabs(hud.driftAngle) / 60.0f, 0.0f, 1.0f);
            Rect(dx - pw * 0.5f + 14 * s, dy + 58 * s, (pw - 28 * s) * f, 4 * s, kWarm, kRed);
        }
        if (hud.driftBanner > 0.0f) {
            const float a = std::min(1.0f, hud.driftBanner / 0.8f), rise = (2.5f - hud.driftBanner) * 18 * s;
            snprintf(buf, sizeof(buf), "+%d", int(hud.driftBank));
            Text(dx, dy - 150 * s - rise, 52 * s, buf, {255, 214, 80, Uint8(255 * a)}, 1);
        }
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

void ModernUI::DrawDialog(int w, int h, std::string_view title, const char *const *items, int itemCount, int selected, bool confirm,
                          std::string_view question, bool confirmYes, float t)
{
    if (!m_font)
        return;
    const float s = float(h) / 720.0f, W = float(w), H = float(h);
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    Rect(0, 0, W, H, {2, 6, 16, 150}, {2, 6, 16, 190}); // dim the scene
    const float pw = 460 * s, ph = (confirm ? 250.0f : 130.0f + 62.0f * float(itemCount)) * s;
    const float px = W * 0.5f - pw * 0.5f, py = H * 0.5f - ph * 0.5f;
    Glow(W * 0.5f, H * 0.5f, pw * 1.1f, {20, 80, 170, 60});
    Glass(px, py, pw, ph, 28 * s, {8, 14, 28, 225}, {120, 190, 255, 170});
    Quad({px, py}, {px + pw - 28 * s, py}, {px + pw - 28 * s, py + 3 * s}, {px, py + 3 * s}, kAccent, kWarm, kWarm, kAccent);
    Text(px + 30 * s, py + 18 * s, 40 * s, title, kWhite, 0);
    char buf[16];
    (void)buf;
    if (!confirm) {
        for (int i = 0; i < itemCount; ++i) {
            const float ry = py + 84 * s + 62 * s * float(i);
            const bool on = i == selected;
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

void ModernUI::DrawEngage(int w, int h, std::string_view kicker, std::string_view title, const char *const *lines, int lineCount, int selected,
                          float t)
{
    if (!m_font)
        return;
    const float s = float(h) / 720.0f, W = float(w), H = float(h);
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    Rect(0, 0, W, H, {2, 6, 16, 110}, {2, 6, 16, 170});
    const float pw = 520 * s, ph = (170.0f + 30.0f * float(lineCount) + 70.0f) * s;
    const float px = W * 0.5f - pw * 0.5f, py = H * 0.5f - ph * 0.5f;
    Glow(W * 0.5f, H * 0.5f, pw * 1.1f, {20, 80, 170, 60});
    Glass(px, py, pw, ph, 28 * s, {8, 14, 28, 230}, {120, 190, 255, 170});
    Quad({px, py}, {px + pw - 28 * s, py}, {px + pw - 28 * s, py + 3 * s}, {px, py + 3 * s}, kAccent, kWarm, kWarm, kAccent);
    Text(px + 30 * s, py + 18 * s, 17 * s, kicker, Alpha(kWarm, 0.95f), 0);
    Text(px + 30 * s, py + 38 * s, 44 * s, title, kWhite, 0);
    for (int i = 0; i < lineCount; ++i) { // "label|value": two columns
        const char *bar = std::strchr(lines[i], '|');
        const std::string label = bar ? std::string(lines[i], size_t(bar - lines[i])) : std::string(lines[i]);
        Text(px + 32 * s, py + 104 * s + 30 * s * float(i), 22 * s, label, {140, 175, 215, 235}, 0);
        if (bar)
            Text(px + 190 * s, py + 104 * s + 30 * s * float(i), 22 * s, bar + 1, kWhite, 0);
    }
    for (int i = 0; i < 2; ++i) {
        const bool on = i == selected;
        const float bw = pw * 0.5f - 45 * s, bx = px + 30 * s + float(i) * (bw + 30 * s), by = py + ph - 78 * s;
        const float pulse = 0.75f + 0.25f * std::sin(t * 6.0f);
        Glass(bx, by, bw, 54 * s, 14 * s, on ? SDL_Color{30, 100, 190, Uint8(170 + 50 * pulse)} : SDL_Color{12, 20, 40, 200},
              on ? SDL_Color{150, 215, 255, 230} : SDL_Color{80, 120, 170, 120});
        Text(bx + bw * 0.5f, by + 8 * s, 34 * s, i == 0 ? "ACCEPT" : "DECLINE", on ? kWhite : SDL_Color{175, 200, 230, 235}, 1);
    }
}

void ModernUI::DrawToast(int w, int h, std::string_view kicker, std::string_view title, std::string_view body, float age, float life)
{
    if (!m_font || age < 0.0f || age > life)
        return;
    const float s = float(h) / 720.0f, W = float(w);
    const float slide = std::clamp(age / 0.35f, 0.0f, 1.0f), out = std::clamp((life - age) / 0.35f, 0.0f, 1.0f);
    const float k = std::min(slide, out), ease = 1.0f - (1.0f - k) * (1.0f - k);
    const float pw = 560 * s, px = W * 0.5f - pw * 0.5f;
    // wrap the body to the card width
    std::vector<std::string> lines;
    {
        std::string cur;
        size_t i = 0;
        const float maxw = pw - 60 * s;
        while (i < body.size()) {
            size_t j = body.find(' ', i);
            if (j == std::string_view::npos)
                j = body.size();
            const std::string word(body.substr(i, j - i));
            const std::string tryLine = cur.empty() ? word : cur + " " + word;
            if (TextWidth(tryLine, 19 * s) > maxw && !cur.empty()) {
                lines.push_back(cur);
                cur = word;
            } else {
                cur = tryLine;
            }
            i = j + 1;
        }
        if (!cur.empty())
            lines.push_back(cur);
    }
    if (lines.size() > 5)
        lines.resize(5);
    const float ph = (82.0f + 25.0f * float(lines.size()) + 18.0f) * s;
    const float py = -ph + (ph + 22.0f * s) * ease;
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    const Uint8 a = Uint8(235.0f * ease);
    Glass(px, py, pw, ph, 22 * s, {8, 14, 28, a}, {120, 190, 255, Uint8(170.0f * ease)});
    Quad({px, py}, {px + pw - 22 * s, py}, {px + pw - 22 * s, py + 3 * s}, {px, py + 3 * s}, kAccent, kWarm, kWarm, kAccent);
    Text(px + 28 * s, py + 14 * s, 15 * s, kicker, Alpha(kWarm, ease), 0);
    Text(px + 28 * s, py + 32 * s, 30 * s, title, Alpha(kWhite, ease), 0);
    for (size_t i = 0; i < lines.size(); ++i)
        Text(px + 28 * s, py + (76.0f + 25.0f * float(i)) * s, 19 * s, lines[i], {190, 212, 238, Uint8(235.0f * ease)}, 0);
    // a thin timer line at the bottom
    const float f = std::clamp(1.0f - age / life, 0.0f, 1.0f);
    Quad({px + 28 * s, py + ph - 8 * s}, {px + 28 * s + (pw - 56 * s) * f, py + ph - 8 * s}, {px + 28 * s + (pw - 56 * s) * f, py + ph - 6 * s}, {px + 28 * s, py + ph - 6 * s},
         Alpha(kAccent, 0.8f * ease), Alpha(kAccent, 0.8f * ease), Alpha(kAccent, 0.8f * ease), Alpha(kAccent, 0.8f * ease));
}

void ModernUI::DrawCoverFrame(int w, int h, const SDL_Rect &page, std::string_view kicker, std::string_view title, std::string_view detail,
                              std::string_view hint, float t)
{
    if (!m_font)
        return;
    const float s = float(h) / 720.0f, W = float(w), H = float(h);
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    Rect(0, 0, W, H, {2, 6, 16, 175}, {2, 6, 16, 215});
    Glow(page.x + page.w * 0.5f, page.y + page.h * 0.5f, page.h * 0.95f, {30, 100, 210, Uint8(70 + 15 * std::sin(t * 1.5f))});
    // a soft shadow and a thin lit edge around the page
    for (int i = 5; i >= 1; --i) {
        const float g = float(i) * 5.0f * s;
        Rect(page.x - g + 8 * s, page.y - g + 10 * s, page.w + 2 * g, page.h + 2 * g, {0, 0, 0, Uint8(14)}, {0, 0, 0, Uint8(14)});
    }
    // captions on the left of the page
    const float lx = std::max(24.0f * s, float(page.x) - 360.0f * s);
    Text(lx, page.y + 40 * s, 17 * s, kicker, Alpha(kWarm, 0.95f), 0);
    Text(lx, page.y + 62 * s, 46 * s, title, kWhite, 0);
    Quad({lx, page.y + 122 * s}, {lx + 70 * s, page.y + 122 * s}, {lx + 70 * s, page.y + 125 * s}, {lx, page.y + 125 * s}, kWarm, kWarm, kWarm, kWarm);
    Text(lx, page.y + 142 * s, 22 * s, detail, {190, 212, 238, 240}, 0);
    Text(W * 0.5f, H - 40 * s, 18 * s, hint, {140, 175, 215, 220}, 1);
}

void ModernUI::DrawOverlay(int w, int h, const char lines[][96], int count)
{
    if (!m_font || count <= 0)
        return;
    const float s = float(h) / 720.0f, panel = 400 * s, px = float(w) - panel - 8 * s, py = 96 * s, lh = 21 * s; // top right, under the timer row
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    Rect(px - 8 * s, py - 6 * s, panel + 8 * s, lh * float(count) + 12 * s, {0, 0, 0, 175}, {0, 0, 0, 175});
    for (int i = 0; i < count; ++i) {
        SDL_Color c = i == 0 ? SDL_Color{120, 255, 160, 255} : SDL_Color{225, 235, 245, 255};
        if (std::strstr(lines[i], "GPU-bound") || std::strstr(lines[i], "CPU-bound"))
            c = {255, 190, 70, 255};
        Text(px, py + lh * float(i), 17 * s, lines[i], c, 0);
    }
}

SDL_Texture *ModernUI::LoadingFrame(int index)
{
    auto it = m_frames.find(index);
    if (it != m_frames.end())
        return it->second;
    if (m_frames.size() >= 6) { // keep only a few: drop the one furthest from the request
        auto far = m_frames.begin();
        for (auto k = m_frames.begin(); k != m_frames.end(); ++k)
            if (std::abs(k->first - index) > std::abs(far->first - index))
                far = k;
        if (far->second)
            SDL_DestroyTexture(far->second);
        m_frames.erase(far);
    }
    char name[600];
    snprintf(name, sizeof(name), "%sf%03d.jpg", m_frameDir.c_str(), index + 1);
    SDL_Texture *tex = nullptr;
    int iw = 0, ih = 0, comp = 0;
    if (unsigned char *img = stbi_load(name, &iw, &ih, &comp, 4)) {
        tex = SDL_CreateTexture(m_r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, iw, ih);
        SDL_UpdateTexture(tex, nullptr, img, iw * 4);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
        stbi_image_free(img);
    }
    m_frames[index] = tex;
    return tex;
}

bool ModernUI::DrawLoading(int w, int h, const char *stage, float fraction, float t)
{
    if (!m_font || (!m_loadArt && m_frameCount < 2))
        return false;
    const float W = float(w), H = float(h);
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    SDL_SetRenderDrawColor(m_r, 0, 0, 0, 255);
    SDL_RenderClear(m_r);
    // the art, "cover" fitted with a slow push-in and a small sideways drift (Ken Burns), so the screen is never static
    const float zoom = 1.04f + 0.035f * (0.5f - 0.5f * std::cos(t * 0.22f));
    const float cover = std::max(W / m_loadW, H / m_loadH) * zoom;
    const float dw = m_loadW * cover, dh = m_loadH * cover;
    const float ox = (W - dw) * (0.5f + 0.35f * std::sin(t * 0.17f)), oy = (H - dh) * 0.5f;
    const SDL_FRect dst{ox, oy, dw, dh};
    if (m_frameCount >= 2) {
        // ping-pong playback of the frames (16 fps) with a cross-fade between neighbours, so the loop never jumps
        const float pos = t * 16.0f, period = float(2 * (m_frameCount - 1));
        float p = std::fmod(pos, period);
        if (p > float(m_frameCount - 1))
            p = period - p;
        const int i0 = std::clamp(int(p), 0, m_frameCount - 1), i1 = std::min(i0 + 1, m_frameCount - 1);
        const float k = p - float(i0);
        SDL_Texture *a = LoadingFrame(i0), *b = LoadingFrame(i1);
        if (a) {
            SDL_SetTextureBlendMode(a, SDL_BLENDMODE_NONE);
            SDL_RenderCopyF(m_r, a, nullptr, &dst);
        }
        if (b && i1 != i0 && k > 0.02f) {
            SDL_SetTextureBlendMode(b, SDL_BLENDMODE_BLEND);
            SDL_SetTextureAlphaMod(b, Uint8(std::clamp(k, 0.0f, 1.0f) * 255.0f));
            SDL_RenderCopyF(m_r, b, nullptr, &dst);
        }
    } else {
        SDL_RenderCopyF(m_r, m_loadArt, nullptr, &dst);
    }
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    // grading: darker top and a deep floor for the text, soft side vignette
    Rect(0, 0, W, H * 0.20f, {0, 4, 14, 150}, {0, 4, 14, 0});
    Rect(0, H * 0.58f, W, H * 0.42f, {0, 4, 14, 0}, {0, 4, 14, 235});
    Quad({0, 0}, {W * 0.10f, 0}, {W * 0.10f, H}, {0, H}, {0, 0, 0, 120}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 120});
    Quad({W * 0.90f, 0}, {W, 0}, {W, H}, {W * 0.90f, H}, {0, 0, 0, 0}, {0, 0, 0, 120}, {0, 0, 0, 120}, {0, 0, 0, 0});
    Quad({0, 0}, {W, 0}, {W, 3}, {0, 3}, kAccent, kWarm, kWarm, kAccent);

    const float u = H / 720.0f; // layout unit: everything scales with the window height
    const float mx = W * 0.055f;
    // title block, lower left
    Text(mx, H - 236 * u, 15 * u, "NEED FOR SPEED", Alpha(kAccent, 0.95f));
    Text(mx - 2 * u, H - 218 * u, 58 * u, "UNDERGROUND 2", kWhite);
    Quad({mx, H - 150 * u}, {mx + 70 * u, H - 150 * u}, {mx + 70 * u, H - 147 * u}, {mx, H - 147 * u}, kWarm, kWarm, kWarm, kWarm);
    // hint (rotates every 6 s, cross-fades)
    static const char *const kHints[] = {
        "Pull the handbrake at speed to start a drift - chain them for a bigger multiplier.",
        "Nitrous is best saved for the straights after a long corner.",
        "Tel Aviv, Jerusalem, Haifa, Be'er Sheva: every district has its own character.",
        "Visit a shop to tune your car: Performance, Body, Graphics and Specialties each tune your car differently.",
        "Fit better tyres before you chase more power - grip wins races.",
        "Press Esc at any time to pause, restart or change options.",
    };
    const int nh = int(sizeof(kHints) / sizeof(kHints[0]));
    const float hs = t / 6.0f;
    const int hi = int(hs) % nh;
    const float ph = hs - std::floor(hs);
    const float fade = std::min(std::min(ph * 8.0f, (1.0f - ph) * 8.0f), 1.0f);
    Text(mx, H - 128 * u, 12 * u, "HINT", Alpha(kWarm, 0.9f * fade));
    Text(mx + 46 * u, H - 132 * u, 18 * u, kHints[hi], Alpha({210, 222, 238, 255}, 0.92f * fade));

    // progress bar along the bottom: a glass track, a gradient fill with a bright head and a sweeping sheen
    const float bx = mx, bw = W - 2 * mx, by = H - 52 * u, bh = 6 * u;
    Rect(bx, by, bw, bh, {255, 255, 255, 38}, {255, 255, 255, 22});
    const float fw = std::max(bh, bw * fraction);
    Quad({bx, by}, {bx + fw, by}, {bx + fw, by + bh}, {bx, by + bh}, kAccent, Mix(kAccent, kWarm, 0.55f), Mix(kAccent, kWarm, 0.55f), kAccent);
    const float sheen = std::fmod(t * 0.55f, 1.6f) - 0.3f;
    {
        const float sx = bx + fw * sheen, sw = 90 * u;
        const float x0 = std::clamp(sx, bx, bx + fw), x1 = std::clamp(sx + sw, bx, bx + fw);
        if (x1 > x0 + 1)
            Quad({x0, by}, {x1, by}, {x1, by + bh}, {x0, by + bh}, {255, 255, 255, 0}, {255, 255, 255, 120}, {255, 255, 255, 120}, {255, 255, 255, 0});
    }
    Glow(bx + fw, by + bh * 0.5f, 34 * u, Alpha(kAccent, 0.85f));
    // stage label (left) and percentage (right)
    char pct[16];
    snprintf(pct, sizeof(pct), "%d%%", int(fraction * 100.0f + 0.5f));
    Text(bx, by - 34 * u, 20 * u, stage && *stage ? stage : "Loading", Alpha(kWhite, 0.95f));
    Text(bx + bw, by - 40 * u, 30 * u, pct, kWhite, 2);
    // spinner next to the stage label's baseline: a short comet orbiting a ring
    {
        const float cx = bx + bw - 118 * u, cy = by - 24 * u, r = 9 * u;
        Ring(cx, cy, r - 1.4f * u, r, Alpha(kWhite, 0.18f));
        const float a = std::fmod(t * 4.2f, 2 * kPi);
        Arc(cx, cy, r - 1.8f * u, r + 0.4f * u, a - 1.5f, a, Alpha(kAccent, 0.0f), kAccent);
    }
    return true;
}
