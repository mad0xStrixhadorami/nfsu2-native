#include "game/indep/World/SignGen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "stb_truetype.h"

namespace SignGen {
namespace {
stbtt_fontinfo g_font;
std::vector<unsigned char> g_ttf;
bool g_ready = false, g_tried = false;

struct Canvas {
    int w, h;
    std::vector<uint32_t> px;
    Canvas(int W, int H, uint32_t c = 0) : w(W), h(H), px(size_t(W) * H, c) {}
};
uint32_t Rgba(int r, int g, int b, int a = 255) { return uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | uint32_t(a) << 24; }

std::vector<int> Utf8(const std::string &s)
{
    std::vector<int> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        int cp, n;
        if (c < 0x80) cp = c, n = 1;
        else if ((c >> 5) == 6) cp = c & 31, n = 2;
        else if ((c >> 4) == 14) cp = c & 15, n = 3;
        else cp = c & 7, n = 4;
        for (int k = 1; k < n && i + k < s.size(); ++k)
            cp = cp << 6 | (s[i + k] & 63);
        out.push_back(cp);
        i += size_t(n);
    }
    return out;
}
bool IsHeb(int cp) { return cp >= 0x5d0 && cp <= 0x5ea; }

// Logical (typed) order -> visual left-to-right order for a line that is mainly Hebrew: words reversed, Hebrew words
// reversed letter by letter, digits / Latin words kept as typed.
std::vector<int> Visual(const std::string &s)
{
    std::vector<std::vector<int>> words(1);
    for (int cp : Utf8(s)) {
        if (cp == ' ')
            words.emplace_back();
        else
            words.back().push_back(cp);
    }
    bool heb = false;
    for (auto &w : words)
        for (int cp : w)
            heb |= IsHeb(cp);
    std::vector<int> out;
    if (!heb) {
        for (size_t i = 0; i < words.size(); ++i) {
            if (i) out.push_back(' ');
            out.insert(out.end(), words[i].begin(), words[i].end());
        }
        return out;
    }
    for (size_t i = words.size(); i-- > 0;) {
        std::vector<int> w = words[i];
        if (!w.empty() && IsHeb(w[0]))
            std::reverse(w.begin(), w.end());
        out.insert(out.end(), w.begin(), w.end());
        if (i) out.push_back(' ');
    }
    return out;
}

float Advance(const std::vector<int> &cps, float scale)
{
    float w = 0;
    for (int cp : cps) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
        w += float(adv) * scale;
    }
    return w;
}

// Alpha mask of the text, fitted into the box (x, y, bw, bh) of the canvas and centred; returns it in `mask` (canvas sized).
void TextMask(const Canvas &cv, const std::string &text, float bx, float by, float bw, float bh, std::vector<float> &mask)
{
    mask.assign(size_t(cv.w) * cv.h, 0.0f);
    const std::vector<int> cps = Visual(text);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&g_font, &asc, &desc, &gap);
    const float unit = stbtt_ScaleForPixelHeight(&g_font, 100.0f);
    const float w100 = Advance(cps, unit), h100 = float(asc - desc) * unit;
    const float k = std::min(bw / std::max(w100, 1.0f), bh / std::max(h100, 1.0f));
    const float scale = unit * k;
    const float tw = Advance(cps, scale);
    float x = bx + (bw - tw) * 0.5f;
    const float base = by + bh * 0.5f + (float(asc + desc) * 0.5f) * scale; // vertically centred on the line box
    for (int cp : cps) {
        int adv, lsb, gw, gh, xo, yo;
        unsigned char *bm = stbtt_GetCodepointBitmap(&g_font, scale, scale, cp, &gw, &gh, &xo, &yo);
        if (bm) {
            for (int yy = 0; yy < gh; ++yy)
                for (int xx = 0; xx < gw; ++xx) {
                    const int px = int(x) + xo + xx, py = int(base) + yo + yy;
                    if (px >= 0 && px < cv.w && py >= 0 && py < cv.h)
                        mask[size_t(py) * cv.w + px] = std::max(mask[size_t(py) * cv.w + px], bm[yy * gw + xx] / 255.0f);
                }
            stbtt_FreeBitmap(bm, nullptr);
        }
        stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
        x += float(adv) * scale;
    }
}

void Blur(std::vector<float> &m, int w, int h, int r)
{
    std::vector<float> t(m.size());
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < h; ++y) { // horizontal box
            float acc = 0;
            for (int x = -r; x <= r; ++x) acc += m[size_t(y) * w + std::clamp(x, 0, w - 1)];
            for (int x = 0; x < w; ++x) {
                t[size_t(y) * w + x] = acc / float(2 * r + 1);
                acc += m[size_t(y) * w + std::min(x + r + 1, w - 1)] - m[size_t(y) * w + std::max(x - r, 0)];
            }
        }
        for (int x = 0; x < w; ++x) { // vertical
            float acc = 0;
            for (int y = -r; y <= r; ++y) acc += t[size_t(std::clamp(y, 0, h - 1)) * w + x];
            for (int y = 0; y < h; ++y) {
                m[size_t(y) * w + x] = acc / float(2 * r + 1);
                acc += t[size_t(std::min(y + r + 1, h - 1)) * w + x] - t[size_t(std::max(y - r, 0)) * w + x];
            }
        }
    }
}

void Over(Canvas &cv, const std::vector<float> &mask, uint32_t colour, float gain = 1.0f)
{
    const int r = colour & 255, g = (colour >> 8) & 255, b = (colour >> 16) & 255;
    for (size_t i = 0; i < mask.size(); ++i) {
        const float a = std::clamp(mask[i] * gain, 0.0f, 1.0f);
        if (a <= 0.0f)
            continue;
        const uint32_t d = cv.px[i];
        const int dr = d & 255, dg = (d >> 8) & 255, db = (d >> 16) & 255, da = d >> 24;
        cv.px[i] = Rgba(int(dr + (r - dr) * a), int(dg + (g - dg) * a), int(db + (b - db) * a), std::max(da, int(a * 255)));
    }
}
void Fill(Canvas &cv, int x0, int y0, int x1, int y1, uint32_t c)
{
    for (int y = std::max(0, y0); y < std::min(cv.h, y1); ++y)
        for (int x = std::max(0, x0); x < std::min(cv.w, x1); ++x)
            cv.px[size_t(y) * cv.w + x] = c;
}

// Neon: black field, soft coloured halo, bright core.
void Neon(Canvas &cv, const std::string &text, int hue)
{
    static const int cols[6][3] = {{255, 60, 160}, {60, 220, 255}, {90, 255, 120}, {255, 190, 50}, {255, 90, 60}, {170, 120, 255}};
    const int *c = cols[hue % 6];
    Fill(cv, 0, 0, cv.w, cv.h, Rgba(0, 0, 0, 255));
    std::vector<float> mask, halo;
    TextMask(cv, text, cv.w * 0.07f, cv.h * 0.12f, cv.w * 0.86f, cv.h * 0.76f, mask);
    halo = mask;
    Blur(halo, cv.w, cv.h, std::max(2, cv.h / 22));
    Over(cv, halo, Rgba(c[0], c[1], c[2]), 2.2f);
    Over(cv, mask, Rgba(std::min(255, c[0] + 110), std::min(255, c[1] + 110), std::min(255, c[2] + 110)), 1.0f);
}

void Panel(Canvas &cv, const std::string &text, uint32_t bg, uint32_t fg, const std::string &sub = "", uint32_t border = 0)
{
    Fill(cv, 0, 0, cv.w, cv.h, bg);
    if (border) {
        const int b = std::max(2, cv.h / 14);
        Fill(cv, 0, 0, cv.w, b, border), Fill(cv, 0, cv.h - b, cv.w, cv.h, border);
        Fill(cv, 0, 0, b, cv.h, border), Fill(cv, cv.w - b, 0, cv.w, cv.h, border);
    }
    std::vector<float> mask;
    if (sub.empty()) {
        TextMask(cv, text, cv.w * 0.06f, cv.h * 0.14f, cv.w * 0.88f, cv.h * 0.72f, mask);
        Over(cv, mask, fg);
    } else {
        TextMask(cv, text, cv.w * 0.06f, cv.h * 0.08f, cv.w * 0.88f, cv.h * 0.54f, mask);
        Over(cv, mask, fg);
        TextMask(cv, sub, cv.w * 0.10f, cv.h * 0.62f, cv.w * 0.80f, cv.h * 0.28f, mask);
        Over(cv, mask, fg, 0.9f);
    }
}

void Speed(Canvas &cv, int kmh)
{
    Fill(cv, 0, 0, cv.w, cv.h, Rgba(255, 255, 255, 255));
    const float cx = cv.w * 0.5f, cy = cv.h * 0.5f, R = std::min(cv.w, cv.h) * 0.48f;
    for (int y = 0; y < cv.h; ++y)
        for (int x = 0; x < cv.w; ++x) {
            const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
            if (d > R)
                cv.px[size_t(y) * cv.w + x] = Rgba(255, 255, 255, 255);
            else if (d > R * 0.74f)
                cv.px[size_t(y) * cv.w + x] = Rgba(205, 25, 30, 255);
        }
    std::vector<float> mask;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", kmh);
    TextMask(cv, buf, cx - R * 0.55f, cy - R * 0.42f, R * 1.1f, R * 0.84f, mask);
    Over(cv, mask, Rgba(20, 20, 20));
}

// The flag of Israel: white field, two blue stripes, the Star of David between them.
void Flag(Canvas &cv)
{
    Fill(cv, 0, 0, cv.w, cv.h, Rgba(255, 255, 255, 255));
    const uint32_t blue = Rgba(0, 56, 184, 255);
    Fill(cv, 0, int(cv.h * 0.085f), cv.w, int(cv.h * 0.085f + cv.h * 0.115f), blue);
    Fill(cv, 0, int(cv.h * 0.80f), cv.w, int(cv.h * 0.915f), blue);
    const float cx = cv.w * 0.5f, cy = cv.h * 0.5f, R = cv.h * 0.26f, t = cv.h * 0.03f;
    auto tri = [&](float rot) {
        float p[3][2];
        for (int i = 0; i < 3; ++i)
            p[i][0] = cx + R * std::cos(rot + i * 2.0944f), p[i][1] = cy + R * std::sin(rot + i * 2.0944f);
        for (int y = 0; y < cv.h; ++y)
            for (int x = 0; x < cv.w; ++x) {
                float inside = 1e9f;
                bool in = true;
                for (int i = 0; i < 3; ++i) {
                    const float *a = p[i], *b = p[(i + 1) % 3];
                    const float e = (b[0] - a[0]) * (y + 0.5f - a[1]) - (b[1] - a[1]) * (x + 0.5f - a[0]);
                    const float len = std::hypot(b[0] - a[0], b[1] - a[1]);
                    inside = std::min(inside, std::fabs(e) / len);
                    in = in && (e >= 0);
                }
                // outline only: near an edge and inside-or-outside of that triangle edge segment
                bool nearEdge = false;
                for (int i = 0; i < 3; ++i) {
                    const float *a = p[i], *b = p[(i + 1) % 3];
                    const float dx = b[0] - a[0], dy = b[1] - a[1];
                    const float u = std::clamp(((x + 0.5f - a[0]) * dx + (y + 0.5f - a[1]) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
                    nearEdge |= std::hypot(x + 0.5f - (a[0] + dx * u), y + 0.5f - (a[1] + dy * u)) < t;
                }
                if (nearEdge)
                    cv.px[size_t(y) * cv.w + x] = blue;
            }
    };
    tri(-1.5708f);
    tri(1.5708f);
}

struct Neonword {
    const char *key, *he;
};
const Neonword kNeon[] = {{"COFFEE", "קפה"},    {"CAFE", "קפה"},      {"SUSHI", "סושי"},     {"TACO", "שווארמה"},    {"PUB", "פאב"},
                          {"MARTINI", "בר"},    {"CASINO", "מועדון"},  {"CLUB", "מועדון"},    {"LOUNGE", "לאונג'"},   {"PRESCRIPTION", "בית מרקחת"},
                          {"OPEN_24", "24 שעות"}, {"DELI", "מעדנייה"}, {"THEATRE", "קולנוע"}, {"CINEMA", "קולנוע"}, {"MOVIE", "קולנוע"},
                          {"HOTEL", "מלון"},    {"MOTEL", "מלון"},    {"ROOMS", "חדרים"},    {"PAWN", "שוק"},       {"SEAHORSE", "סוסון ים"},
                          {"TRAVEL", "טיולים"}, {"EMPIRE", "אימפריה"}, {"DIPLOMAT", "דיפלומט"}, {"IMPERIAL", "אימפריאל"}, {"AUDIOSHOP", "אודיו"},
                          {"LITS", "פתוח"},     {"OUTLINE", "פתוח"},   {"STRIPS", "פתוח"}};
const struct { const char *he, *en; uint32_t bg, fg; } kShops[] = {
    {"מכולת", "GROCERY", Rgba(220, 40, 40), Rgba(255, 255, 255)},        {"פלאפל", "FALAFEL", Rgba(250, 200, 30), Rgba(40, 30, 10)},
    {"בית קפה", "CAFE", Rgba(30, 110, 70), Rgba(255, 255, 255)},          {"מאפיית לחם", "BAKERY", Rgba(235, 220, 190), Rgba(120, 50, 20)},
    {"שווארמה", "SHAWARMA", Rgba(200, 60, 20), Rgba(255, 240, 200)},      {"חומוס", "HUMMUS", Rgba(245, 235, 200), Rgba(30, 80, 40)},
    {"פיצה", "PIZZA", Rgba(190, 30, 30), Rgba(255, 255, 255)},            {"מספרה", "BARBER", Rgba(20, 20, 30), Rgba(240, 240, 240)},
    {"בית מרקחת", "PHARMACY", Rgba(0, 130, 90), Rgba(255, 255, 255)},     {"סופרמרקט", "SUPERMARKET", Rgba(0, 70, 160), Rgba(255, 255, 255)},
    {"פרחים", "FLOWERS", Rgba(190, 50, 120), Rgba(255, 255, 255)},        {"אופטיקה", "OPTICS", Rgba(30, 30, 90), Rgba(255, 255, 255)},
    {"ירקות ופירות", "FRUIT", Rgba(80, 160, 40), Rgba(255, 255, 255)},    {"קצביה", "BUTCHER", Rgba(120, 20, 20), Rgba(255, 255, 255)},
    {"צילום", "PHOTO", Rgba(240, 240, 240), Rgba(20, 20, 20)},            {"גלידה", "ICE CREAM", Rgba(250, 190, 210), Rgba(120, 30, 60)}};
const struct { const char *he, *en; } kStreets[] = {{"רחוב הרצל", "HERZL ST."},         {"שדרות בן גוריון", "BEN GURION BLVD."}, {"רחוב רוטשילד", "ROTHSCHILD ST."},
                                                   {"רחוב דיזנגוף", "DIZENGOFF ST."}, {"רחוב ז'בוטינסקי", "JABOTINSKY ST."},   {"רחוב אלנבי", "ALLENBY ST."},
                                                   {"רחוב ויצמן", "WEIZMANN ST."},    {"שדרות בגין", "BEGIN BLVD."},           {"רחוב יפו", "JAFFA ST."},
                                                   {"רחוב העצמאות", "HA'ATZMAUT ST."}};
} // namespace

bool Init()
{
    if (g_tried)
        return g_ready;
    g_tried = true;
    std::string path = "assets/ui/Alef-Bold.ttf";
    if (const char *d = getenv("NFSU2_ASSETS_UI"))
        path = std::string(d) + "/Alef-Bold.ttf";
    for (const char *c : {"assets/ui", "../assets/ui", "../../assets/ui"}) {
        std::string p = std::string(c) + "/Alef-Bold.ttf";
        if (FILE *f = fopen(p.c_str(), "rb")) {
            fclose(f);
            path = p;
            break;
        }
    }
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_ttf.resize(size_t(n));
    if (fread(g_ttf.data(), 1, size_t(n), f) != size_t(n)) {
        fclose(f);
        return false;
    }
    fclose(f);
    g_ready = stbtt_InitFont(&g_font, g_ttf.data(), stbtt_GetFontOffsetForIndex(g_ttf.data(), 0)) != 0;
    printf("[SignGen] %s (%s)\n", g_ready ? "Hebrew sign font ready" : "font failed", path.c_str());
    return g_ready;
}

int ShopVariants() { return int(sizeof(kShops) / sizeof(kShops[0])); }
int StreetVariants() { return int(sizeof(kStreets) / sizeof(kStreets[0])); }

bool Make(int kind, int variant, int w, int h, std::vector<uint32_t> &rgba)
{
    if (!Init())
        return false;
    Canvas cv(w, h);
    if (kind == 0) {
        const auto &s = kShops[variant % ShopVariants()];
        Panel(cv, s.he, s.bg, s.fg, s.en, Rgba(255, 255, 255, 255));
    } else if (kind == 1) {
        const auto &s = kStreets[variant % StreetVariants()];
        Panel(cv, s.he, Rgba(0, 76, 150), Rgba(255, 255, 255), s.en, Rgba(255, 255, 255));
    } else {
        // the flag is 11:8: draw it at that ratio, then squeeze it into the requested picture (the prop quad un-squeezes it)
        Canvas wide(h * 11 / 8, h);
        Flag(wide);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                cv.px[size_t(y) * w + x] = wide.px[size_t(y) * wide.w + std::min(wide.w - 1, x * wide.w / w)];
    }
    rgba = std::move(cv.px);
    return true;
}

bool Replacement(const std::string &name, int &w, int &h, std::vector<uint32_t> &rgba)
{
    if (!Init())
        return false;
    auto has = [&](const char *k) { return name.find(k) != std::string::npos; };
    // grow tiny originals: keep the aspect, aim for a 256..512 px wide picture
    auto grow = [&](int minW) {
        int k = 1;
        while (w * k < minW && k < 16)
            k *= 2;
        w *= k, h *= k;
    };
    if (has("SPEEDLIMIT")) {
        const int kmh = has("LIMIT35") ? 30 : has("LIMIT45") ? 50 : has("LIMIT55") ? 70 : 90;
        w = h = 256;
        Canvas cv(w, h);
        Speed(cv, kmh);
        rgba = std::move(cv.px);
        return true;
    }
    if (has("BURGERKING")) {
        grow(256);
        Canvas cv(w, h);
        Panel(cv, "המבורגר", Rgba(215, 35, 30), Rgba(255, 230, 120), "BURGER", Rgba(255, 190, 40));
        rgba = std::move(cv.px);
        return true;
    }
    if (has("AUTOZONE")) {
        grow(256);
        Canvas cv(w, h);
        Panel(cv, "חלפים לרכב", Rgba(240, 120, 20), Rgba(20, 20, 20), "AUTO PARTS", Rgba(20, 20, 20));
        rgba = std::move(cv.px);
        return true;
    }
    static const char *const kDest[][2] = {{"תל אביב", "TEL AVIV"}, {"ירושלים", "JERUSALEM"}, {"חיפה", "HAIFA"}, {"באר שבע", "BE'ER SHEVA"},
                                           {"אשדוד", "ASHDOD"},     {"אילת", "EILAT"},       {"נתניה", "NETANYA"}, {"רמת גן", "RAMAT GAN"}};
    const size_t nh = std::hash<std::string>{}(name);
    if (has("SGN_FWY")) { // intercity direction signs: green, white Hebrew over Latin
        grow(256);
        Canvas cv(w, h);
        Panel(cv, kDest[nh % 8][0], Rgba(0, 105, 62), Rgba(255, 255, 255), kDest[nh % 8][1], Rgba(255, 255, 255));
        rgba = std::move(cv.px);
        return true;
    }
    if (has("SLUMSIGN") || has("RBLDING_SIGN") || has("OBSERVATORYSIGN")) { // small shop signs
        grow(256);
        Canvas cv(w, h);
        const auto &sh = kShops[nh % size_t(ShopVariants())];
        Panel(cv, sh.he, sh.bg, sh.fg, sh.en, Rgba(255, 255, 255, 255));
        rgba = std::move(cv.px);
        return true;
    }
    if (has("PRKG_SIGN")) {
        grow(256);
        Canvas cv(w, h);
        Panel(cv, "חניה", Rgba(0, 76, 160), Rgba(255, 255, 255), "PARKING", Rgba(255, 255, 255));
        rgba = std::move(cv.px);
        return true;
    }
    if (((name.rfind("SGN_", 0) == 0) && !has("SIGNAL") && !has("BYSIGN")) || (has("NEON") && (name.rfind("ARC_", 0) == 0 || name.rfind("SFX_", 0) == 0))) {
        for (const Neonword &n : kNeon)
            if (has(n.key)) {
                grow(512);
                Canvas cv(w, h);
                Neon(cv, n.he, int(std::hash<std::string>{}(name) % 6));
                rgba = std::move(cv.px);
                return true;
            }
    }
    return false;
}
} // namespace SignGen
