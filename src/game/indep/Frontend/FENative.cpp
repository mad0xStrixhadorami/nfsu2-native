#include "game/indep/Frontend/FENative.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <unordered_set>

#include "Speed/Indep/Src/FEng/FEGroup.h"
#include "Speed/Indep/Src/FEng/FEImage.h"
#include "Speed/Indep/Src/FEng/FEObject.h"
#include "Speed/Indep/Src/FEng/FEObjectCallback.h"
#include "Speed/Indep/Src/FEng/FEPackage.h"
#include "Speed/Indep/Src/FEng/FEScript.h"
#include "Speed/Indep/Src/FEng/FESlotPool.h"
#include "Speed/Indep/Src/FEng/FEString.h"
#include "Speed/Indep/bWare/Inc/Strings.hpp"
#include "game/bWare/bChunk.h"

extern FEMultiPool ObjDataPool; // FEObject.cpp: pool for FEObject transform data
#include "game/bWare/bCompression.h"
#include "game/indep/Frontend/FEStartup.h"
#include "game/indep/Texture/TexturePack.h"
#include "game/indep/World/ModelViewGL.h"
#include "game/indep/World/PlayerCar.h"
#include "game/indep/World/SolidMesh.h"

void SetActiveFEngineForWrapper(FEngine *engine);

namespace {
constexpr uint32_t kChunkFEngPackage = 0x00030203;   // BCHUNK_FENG_PACKAGE
constexpr uint32_t kChunkFEngPackageLZ = 0x00030210; // BCHUNK_FENG_COMPRESSED_PACKAGE: u32 hash + JDLZ
constexpr uint32_t kResourceImage = 1;               // FEResourceRequest::Type for textures
constexpr uint32_t kResourceFont = 2;                // FEResourceRequest::Type for fonts
constexpr uint32_t kResourceMovie = 4;               // FEResourceRequest::Type for movies
constexpr uint32_t kChunkFEngFont = 0x00030201;      // BCHUNK_FENG_FONT
constexpr uint32_t kChunkLanguage = 0x00039000;      // BCHUNK_LANGUAGE

constexpr u32 kMsgDisableInputs = 0x05922615;
constexpr u32 kMsgEnableInputs = 0x7e4d1288;

std::string LowerName(const char *s)
{
    std::string r(s);
    for (char &c : r)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return r;
}

// FEng package short name lives in the PkHd chunk: u32[6] = version, ?, resources, objects,
// name length, file name length, then the short name.
const char *PackageShortName(const uint8_t *feng)
{
    return reinterpret_cast<const char *>(feng + 8 + 8 + 24);
}

// 0x00290d48 (FEngHidePCObjects) + propagate group visibility to children.
struct HidePCObjectsAndStartMovies : FEObjectCallback {
    FEngine *engine;
    FEPackage *pkg;
    FENativeInterface *fe;

    HidePCObjectsAndStartMovies(FEngine *e, FEPackage *p, FENativeInterface *f) : engine(e), pkg(p), fe(f) {}

    bool Callback(FEObject *obj) override
    {
        // Hide PC-only objects (FENG_OBJECT_PC_ONLY = 0x8) as 0x00290d48 does on PS2.
        if (obj->Flags & 0x8) {
            FENativeInterface::SetObjectVisible(obj, false);
            obj->Flags = (obj->Flags & ~0x10000000u) | 0x400000u;
        }
        // If a group is marked invisible in the package, propagate to its children.
        if (obj->Type == FE_Group && (obj->Flags & 1))
            FENativeInterface::SetObjectVisible(obj, false);
        // 0x00290cb8 (FEngMovieStarter)
        if (obj->Type == FE_Movie && engine && !pkg->IsLibrary())
            fe->StartMovie(obj, pkg);
        return true;
    }
};
} // namespace

FENativeInterface::Affine FENativeInterface::Affine::operator*(const Affine &o) const
{
    Affine r;
    r.a = a * o.a + c * o.b;
    r.b = b * o.a + d * o.b;
    r.c = a * o.c + c * o.d;
    r.d = b * o.c + d * o.ty * 0 + d * o.d;
    r.tx = a * o.tx + c * o.ty + tx;
    r.ty = b * o.tx + d * o.ty + ty;
    return r;
}

void FENativeInterface::Affine::Apply(float x, float y, float &ox, float &oy) const
{
    ox = a * x + c * y + tx;
    oy = b * x + d * y + ty;
}

FENativeInterface::FENativeInterface(SDL_Renderer *renderer) : m_renderer(renderer)
{
    if (renderer) {
        m_white = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, 1, 1);
        uint32_t px = 0xffffffff;
        SDL_UpdateTexture(m_white, nullptr, &px, 4);
        SDL_SetTextureBlendMode(m_white, SDL_BLENDMODE_BLEND);
    }
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER)) {
        for (int i = 0; i < SDL_NumJoysticks(); ++i) {
            if (SDL_IsGameController(i)) {
                m_controller = SDL_GameControllerOpen(i);
                if (m_controller)
                    break;
            }
        }
    }
}

// Stand-in: load the verified geometry subset until UG2's full car roster loader is ported.
bool FENativeInterface::LoadCarPreview(const char *model)
{
    if (!model || !m_readFile)
        return false;
    std::string path = "CARS\\" + std::string(model) + "\\GEOMETRY.BIN";
    std::vector<uint8_t> geometry = m_readFile(path.c_str());
    if (geometry.empty()) {
        // The record also names a JDLZ-compressed GEOMETRY.LZC (no car on the PAL disc uses it;
        // CIVIC and RSX have no files at all).
        path.replace(path.size() - 3, 3, "LZC");
        geometry = m_readFile(path.c_str());
    }
    std::vector<SolidMesh::Triangle> decoded = SolidMesh::DecodeGeometry(geometry.data(), geometry.size());
    if (decoded.empty()) {
        fprintf(stderr, "[CarPreview] no geometry decoded from %s\n", path.c_str());
        return false;
    }
    // The stock body as assembled for racing (PlayerCar), drawn with GL when available.
    PlayerCar assembled;
    m_carPreviewModel.clear();
    PlayerCar::Wheel wheels[4];
    if (assembled.LoadModel(geometry, CarWheels(model, wheels) ? wheels : nullptr)) {
        m_carPreviewModel = assembled.Model();
        m_carPreviewSlots = assembled.TriangleTextures();
        m_carPreviewWheels = assembled.WheelTrianglesStart();
    }
    m_carPreviewTris.clear();
    m_carPreviewSplatsBuilt = false;
    m_carPreviewTris.reserve(decoded.size());
    for (const SolidMesh::Triangle &t : decoded) {
        PreviewTriangle tri;
        for (int k = 0; k < 3; ++k)
            tri.v[k] = {t.v[k].x, t.v[k].y, t.v[k].z};
        m_carPreviewTris.push_back(tri);
    }
    m_previewCarName = model;
    fprintf(stderr, "[CarPreview] decoded %zu %s triangles from %s\n", m_carPreviewTris.size(),
            model, path.c_str());
    return true;
}

// Stand-in: the UG2 car-select renderer is not ported yet; draw the decoded model as a point cloud.
void FENativeInterface::RenderCarPreviewOverlay()
{
    FEPackage *pkg = nullptr;
    if (m_engine) {
        for (FEPackage *candidate = m_engine->GetPackageList()->GetFirstPackage(); candidate;
             candidate = candidate->GetNext()) {
            if (candidate->GetName() && strcasecmp(candidate->GetName(), "UI_QRCarSelect.fng") == 0) {
                pkg = candidate;
                break;
            }
        }
    }
    if (!pkg || !m_renderer)
        return;

    if (!m_carPreviewLoadAttempted) {
        m_carPreviewLoadAttempted = true;
        LoadCarPreview("SKYLINE");
    }
    if (m_carPreviewTris.empty())
        return;

    // GL path: the assembled car, slowly turning, in the screen's preview area (FEng 160..480 x
    // 190..390, mapped to window pixels).
    if (!m_carPreviewModel.empty()) {
        int x0, y0, x1, y1;
        SDL_RenderLogicalToWindow(m_renderer, 150.0f, 180.0f, &x0, &y0);
        SDL_RenderLogicalToWindow(m_renderer, 490.0f, 395.0f, &x1, &y1);
        int winW, winH, outW, outH;
        SDL_GetWindowSize(SDL_RenderGetWindow(m_renderer), &winW, &winH);
        SDL_GetRendererOutputSize(m_renderer, &outW, &outH);
        float sx = winW ? float(outW) / winW : 1.0f, sy = winH ? float(outH) / winH : 1.0f;
        SDL_Rect area{int(x0 * sx), int(y0 * sy), int((x1 - x0) * sx), int((y1 - y0) * sy)};
        float yaw = 0.6f + SDL_GetTicks() * 0.0004f;
        UpdateStatBars(pkg);
        if (DrawModelGL(m_renderer, m_carPreviewModel, area, yaw, SDL_Color{60, 110, 210, 255}, &m_carPreviewSlots,
                        m_carPreviewWheels)) {
            DrawCarPreviewLabel(pkg);
            return;
        }
    }

    // Draw the decoded vertices as depth-sorted splats. Strip triangle topology is not
    // recoverable without executing the mesh VU microprogram, and the best-effort triangle
    // approximation leaves bridge slivers streaking the silhouette, so points are used here.
    // Oblique "hero" projection for the car-select preview (matches the offline splat tool).
    constexpr float yawCos = 0.8660254f, yawSin = 0.5f, elevSin = 0.22f;
    if (!m_carPreviewSplatsBuilt) {
        m_carPreviewSplatsBuilt = true;
        std::unordered_set<uint64_t> seen;
        std::vector<PreviewVertex> pts;
        pts.reserve(m_carPreviewTris.size());
        auto quantize = [](float f) { return static_cast<uint32_t>(static_cast<int32_t>(std::lround(f * 2048.0f))); };
        for (const PreviewTriangle &t : m_carPreviewTris) {
            for (const PreviewVertex &v : t.v) {
                uint64_t key = (static_cast<uint64_t>(quantize(v.x)) << 42) ^
                               (static_cast<uint64_t>(quantize(v.y)) << 21) ^
                               static_cast<uint64_t>(quantize(v.z));
                if (seen.insert(key).second)
                    pts.push_back(v);
            }
        }
        if (!pts.empty()) {
            float midX = 0.0f, midY = 0.0f, midZ = 0.0f;
            for (const PreviewVertex &v : pts) { midX += v.x; midY += v.y; midZ += v.z; }
            float inv = 1.0f / static_cast<float>(pts.size());
            midX *= inv; midY *= inv; midZ *= inv;
            struct Projected { float sx, sy, depth; };
            std::vector<Projected> proj(pts.size());
            float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f, minD = 1e30f, maxD = -1e30f;
            for (size_t i = 0; i < pts.size(); ++i) {
                const PreviewVertex &v = pts[i];
                float x = (v.x - midX) * yawCos + (v.z - midZ) * yawSin;
                float d = -(v.x - midX) * yawSin + (v.z - midZ) * yawCos;
                float y = (v.y - midY) - d * elevSin;
                proj[i] = {x, y, d};
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minY = std::min(minY, y); maxY = std::max(maxY, y);
                minD = std::min(minD, d); maxD = std::max(maxD, d);
            }
            float scale = std::min(320.0f / std::max(maxX - minX, 0.01f),
                                   145.0f / std::max(maxY - minY, 0.01f));
            float invDepth = 1.0f / std::max(maxD - minD, 1e-6f);
            std::sort(proj.begin(), proj.end(),
                      [](const Projected &a, const Projected &b) { return a.depth < b.depth; });
            constexpr float hs = 2.6f;
            m_carPreviewSplats.clear();
            m_carPreviewSplats.reserve(proj.size() * 6);
            auto emit = [&](float x, float y, int shade) {
                m_carPreviewSplats.push_back(
                    {{x, y},
                     {static_cast<Uint8>(shade), static_cast<Uint8>(std::min(shade + 40, 255)),
                      static_cast<Uint8>(std::min(shade + 70, 255)), 255},
                     {0.0f, 0.0f}});
            };
            for (const Projected &p : proj) {
                float sx = 320.0f + p.sx * scale, sy = 300.0f - p.sy * scale;
                int shade = 60 + int(150 * std::clamp((p.depth - minD) * invDepth, 0.0f, 1.0f));
                emit(sx - hs, sy - hs, shade); emit(sx + hs, sy - hs, shade); emit(sx + hs, sy + hs, shade);
                emit(sx - hs, sy - hs, shade); emit(sx + hs, sy + hs, shade); emit(sx - hs, sy + hs, shade);
            }
        }
    }
    if (m_carPreviewSplats.empty())
        return;
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_NONE);
    SDL_RenderGeometry(m_renderer, nullptr, m_carPreviewSplats.data(),
                       static_cast<int>(m_carPreviewSplats.size()), nullptr, 0);

    DrawCarPreviewLabel(pkg);
}

// Car-select stat bars (UI_QRCarSelect: "accel_*", "top_speed_*", "handling_*" slider widgets set
// up by 0x001f4130, drawn by 0x002614e0). UG2 feeds them 0..1 values from its car physics model
// (0x0031ce90 / 0x0031cdd8 / 0x0031d060, not ported); here they come from the database drivetrain:
// 0-100 km/h time (12 s -> 4.5 s), simulated top speed (170 -> 270 km/h) and tyre grip per tonne.
// Layout as 0x002614e0: the track starts 46 units into the 256-wide backing and is 164 long; the
// "cur" fill starts 4 units further and grows with the value, the handle sits at its end.
void FENativeInterface::UpdateStatBars(FEPackage *pkg)
{
    if (m_statBarsCar == m_previewCarName)
        return;
    m_statBarsCar = m_previewCarName;
    PlayerCar::Performance p;
    PlayerCar::Wheel w[4];
    float values[3] = {0.5f, 0.5f, 0.5f};
    if (CarPerformance(m_previewCarName, p)) {
        float t100, vmax;
        PlayerCar::Simulate(p, CarWheels(m_previewCarName, w) ? w[0].radius : 0.32f, t100, vmax);
        values[0] = (12.0f - t100) / (12.0f - 4.5f);
        values[1] = (vmax * 3.6f - 170.0f) / (270.0f - 170.0f);
        values[2] = (p.grip - 0.15f * (p.mass - 1.0f) - 0.9f) / (1.25f - 0.9f);
    }
    static const char *const kBars[3] = {"ACCEL", "TOP_SPEED", "HANDLING"};
    constexpr float kInset = 46.0f, kTrack = 164.0f;
    for (int i = 0; i < 3; ++i) {
        const float v = std::clamp(values[i], 0.03f, 1.0f);
        std::string base = kBars[i];
        FEObject *backing = pkg->FindObjectByHash(bStringHash((base + "_BACKING").c_str()));
        FEObject *cur = pkg->FindObjectByHash(bStringHash((base + "_CUR").c_str()));
        FEObject *handle = pkg->FindObjectByHash(bStringHash((base + "_HANDLE").c_str()));
        if (!backing || !cur)
            continue;
        float bx, by, bw, bh, cx, cy, cw, ch;
        GetCenter(backing, bx, by);
        GetSize(backing, bw, bh);
        GetCenter(cur, cx, cy);
        GetSize(cur, cw, ch);
        const float left = bx - bw * 0.5f + kInset;
        SetSize(cur, kTrack * v, ch);
        SetCenter(cur, left + 4.0f + kTrack * v * 0.5f, cy);
        if (handle) {
            float hx, hy;
            GetCenter(handle, hx, hy);
            SetCenter(handle, left + kTrack * v, hy);
        }
    }
    printf("[CarSelect] %s stats: accel %.2f top speed %.2f handling %.2f\n", m_previewCarName.c_str(), values[0],
           values[1], values[2]);
}

// Real car-name label from the GLOBALB car table (chunk 0x34600): "<manufacturer> <model>". The
// UG2 car-select class builds this label object at runtime (no string object ships in the .fng),
// so synthesize an FEString here and reuse whatever font the screen itself uses.
void FENativeInterface::DrawCarPreviewLabel(FEPackage *pkg)
{
    if (!m_renderer || !pkg || m_carTable.empty())
        return;
    std::string key = m_previewCarName;
    for (char &ch : key)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    auto it = m_carTable.find(key);
    if (it == m_carTable.end())
        return;

    std::function<u32(FEObject *)> findFont = [&](FEObject *o) -> u32 {
        if (o->Type == FE_String && m_fonts.count(o->Handle))
            return o->Handle;
        if (o->Type == FE_Group)
            for (FEObject *c = static_cast<FEGroup *>(o)->GetFirstChild(); c; c = c->GetNext()) {
                u32 f = findFont(c);
                if (f)
                    return f;
            }
        return 0;
    };
    u32 font = 0;
    for (FEObject *o = pkg->GetFirstObject(); o && !font; o = o->GetNext())
        font = findFont(o);
    if (!font)
        return;

    // FEObject::~FEObject frees pData back to the global ObjDataPool, so the synthetic object's
    // transform data must be pool-allocated too (a stack pointer would crash on destruction).
    FEObjData *data = reinterpret_cast<FEObjData *>(ObjDataPool.Alloc(sizeof(FEObjData)));
    if (!data)
        return;
    data->Col.b = data->Col.g = data->Col.r = data->Col.a = 255;
    data->Pivot = FEVector3(0.0f, 0.0f, 0.0f);
    data->Pos = FEVector3(0.0f, -168.0f, 0.0f);
    data->Rot.x = data->Rot.y = data->Rot.z = 0.0f;
    data->Rot.w = 1.0f;
    data->Size = FEVector3(1.0f, 1.0f, 1.0f);

    std::string name = it->second.manufacturer;
    if (!name.empty() && !it->second.folder.empty())
        name += " ";
    name += it->second.folder;

    FEString label;
    label.Handle = font;
    label.RenderContext = 0;
    label.Format = FESTRING_JUSTIFY_HCENTER;
    label.pData = reinterpret_cast<u8 *>(data);
    label.SetString(name.c_str());
    DrawString(&label);
}

// Cycle the car-select carousel over the GLOBALB car database (chunk 0x34600, record order). Cars
// whose GEOMETRY.BIN fails to decode are skipped so a preview always renders; the small fixed list
// is kept only as a fallback for when no car database is mounted.
void FENativeInterface::CycleCarPreview(int direction)
{
    static const char *const fallback[] = {"350Z", "COROLLA", "CORSA", "COUPE", "FOCUS",
                                           "MIATA", "SKYLINE", "TIBURON", "TT"};
    const bool haveDb = !m_carRoster.empty();
    const size_t count = haveDb ? m_carRoster.size() : (sizeof(fallback) / sizeof(fallback[0]));
    if (direction == 0 || count == 0)
        return;
    auto modelAt = [&](size_t i) -> const char * {
        return haveDb ? m_carRoster[i].c_str() : fallback[i];
    };
    size_t cur = 0;
    for (size_t i = 0; i < count; ++i)
        if (strcasecmp(modelAt(i), m_previewCarName.c_str()) == 0) {
            cur = i;
            break;
        }
    for (size_t attempt = 0; attempt < count; ++attempt) {
        int next = (static_cast<int>(cur) + (direction < 0 ? -1 : 1) + int(count)) % int(count);
        cur = static_cast<size_t>(next);
        const char *model = modelAt(cur);
        // Service/traffic vehicles (taxi, ambulance, vans) are never selectable in Quick Race.
        if (haveDb) {
            auto it = m_carTable.find(model);
            if (it != m_carTable.end() && it->second.traffic)
                continue;
        }
        if (LoadCarPreview(model)) {
            m_previewCarIndex = cur;
            return;
        }
    }
}

FENativeInterface::~FENativeInterface()
{
    if (m_controller)
        SDL_GameControllerClose(m_controller);
    for (auto &t : m_textures)
        if (t.second && t.second != m_white)
            SDL_DestroyTexture(t.second);
    if (m_white)
        SDL_DestroyTexture(m_white);
    for (TexturePack *p : m_packs)
        delete p;
}

void FENativeInterface::AttachEngine(FEngine *engine)
{
    m_engine = engine;
    SetActiveFEngineForWrapper(engine);
    if (engine) {
        engine->SetInterface(this);
        engine->SetNumJoyPads(1);
        engine->SetInitialState();
        engine->SetExecution(true);
    }
}

// 0x00293010 (FEngSetVisible) and 0x00292f80 (FEngSetInvisible): toggle bit 0 (FF_Invisible),
// mark FF_DirtyCode (0x400000), and recurse into FE_Group children.
void FENativeInterface::SetObjectVisible(FEObject *obj, bool visible)
{
    if (!obj)
        return;
    if (visible)
        obj->Flags = (obj->Flags & ~1u) | 0x400000u;
    else
        obj->Flags = obj->Flags | 0x400001u;
    if (obj->Type == FE_Group) {
        for (FEObject *c = static_cast<FEGroup *>(obj)->GetFirstChild(); c; c = c->GetNext())
            SetObjectVisible(c, visible);
    }
}

void FENativeInterface::SetVisible(FEPackage *pkg, uint32_t objHash, bool visible)
{
    if (!pkg)
        return;
    SetObjectVisible(pkg->FindObjectByHash(objHash), visible);
}

// 0x00292980 (FEngFindObject): search the active packages, then the engine's libraries.
FEObject *FENativeInterface::FindObject(uint32_t objHash) const
{
    if (!m_engine)
        return nullptr;
    for (FEPackage *p = m_engine->GetPackageList()->GetFirstPackage(); p; p = p->GetNext())
        if (FEObject *o = p->FindObjectByHash(objHash))
            return o;
    for (FEPackage *p = m_engine->GetFirstLibrary(); p; p = p->GetNext())
        if (FEObject *o = p->FindObjectByHash(objHash))
            return o;
    return nullptr;
}

void FENativeInterface::SetVisible(uint32_t objHash, bool visible)
{
    SetObjectVisible(FindObject(objHash), visible);
}

void FENativeInterface::SetAlpha(uint32_t objHash, uint8_t alpha)
{
    FEObject *obj = FindObject(objHash);
    if (!obj || !obj->GetObjData())
        return;
    obj->GetObjData()->Col.a = alpha;
    obj->Flags |= 0x400000u;
}

// 0x00292a68 (FEngSetLanguageHash): set label hash and clear FENG_OBJECT_IGNORE_LABELHASH (0x2).
void FENativeInterface::SetLanguageHash(FEPackage *pkg, uint32_t objHash, uint32_t labelHash)
{
    if (!pkg)
        return;
    FEObject *obj = pkg->FindObjectByHash(objHash);
    if (!obj)
        return;
    auto apply = [&](FEObject *o) {
        if (o->Type == FE_String) {
            FEString *s = static_cast<FEString *>(o);
            s->SetLabelHash(labelHash);
            s->Flags = (s->Flags & ~2u) | 0x400000u;
        }
    };
    if (obj->Type == FE_Group) {
        for (FEObject *c = static_cast<FEGroup *>(obj)->GetFirstChild(); c; c = c->GetNext())
            apply(c);
    } else {
        apply(obj);
    }
}

void FENativeInterface::SetString(FEPackage *pkg, uint32_t objHash, const char *text)
{
    FEObject *obj = pkg ? pkg->FindObjectByHash(objHash) : nullptr;
    if (!obj || obj->Type != FE_String)
        return;
    FEString *s = static_cast<FEString *>(obj);
    s->SetString(text);
    s->Flags |= 0x2u | 0x400000u; // use own text, dirty
}

// FEObjData::Col.a (FEDebug prints it as a=). UI_Options templates ship at alpha 18.
void FENativeInterface::SetAlpha(FEPackage *pkg, uint32_t objHash, uint8_t alpha)
{
    FEObject *obj = pkg ? pkg->FindObjectByHash(objHash) : nullptr;
    if (!obj || !obj->GetObjData())
        return;
    obj->GetObjData()->Col.a = alpha;
    obj->Flags |= 0x400000u; // FF_DirtyCode
}

// 0x00292ee8 (FEngSetTextureHash): set image texture handle and mark FF_DirtyCode.
void FENativeInterface::SetTextureHash(FEObject *obj, uint32_t texHash)
{
    if (!obj)
        return;
    obj->Handle = texHash;
    obj->Flags |= 0x400000u;
}

// 0x00293240 (FEngSetScript): switch object to scriptHash and optionally reset CurTime = 0.
void FENativeInterface::SetScript(FEPackage *pkg, uint32_t objHash, uint32_t scriptHash, bool resetTime)
{
    if (!pkg)
        return;
    FEObject *obj = pkg->FindObjectByHash(objHash);
    if (!obj)
        return;
    if (FEScript *sc = obj->FindScript(scriptHash)) {
        obj->SetCurrentScript(sc);
        if (resetTime)
            sc->CurTime = 0;
    }
}

// 0x00293b90 (FEngGetCenter) / 0x00293a40 (FEngSetCenter):
void FENativeInterface::GetCenter(const FEObject *obj, float &x, float &y)
{
    if (!obj || !obj->GetObjData()) {
        x = 0.0f;
        y = 0.0f;
        return;
    }
    x = obj->GetObjData()->Pos.x;
    y = obj->GetObjData()->Pos.y;
}

void FENativeInterface::SetCenter(FEObject *obj, float x, float y)
{
    if (!obj || !obj->GetObjData())
        return;
    obj->GetObjData()->Pos.x = x;
    obj->GetObjData()->Pos.y = y;
    obj->Flags |= 0x400000u;
}

// 0x00293df0 (FEngGetSize) / 0x00293ee0 (FEngSetSize):
void FENativeInterface::GetSize(const FEObject *obj, float &w, float &h)
{
    if (!obj || !obj->GetObjData()) {
        w = 0.0f;
        h = 0.0f;
        return;
    }
    w = obj->GetObjData()->Size.x;
    h = obj->GetObjData()->Size.y;
}

void FENativeInterface::SetSize(FEObject *obj, float w, float h)
{
    if (!obj || !obj->GetObjData())
        return;
    obj->GetObjData()->Size.x = w;
    obj->GetObjData()->Size.y = h;
    obj->Flags |= 0x400000u;
}

void FENativeInterface::ShowLoadingProgress(const char *stage, float fraction)
{
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    if (stage) {
        constexpr int kBarWidth = 24;
        int filled = static_cast<int>(fraction * kBarWidth + 0.5f);
        char bar[kBarWidth + 1];
        for (int i = 0; i < kBarWidth; ++i)
            bar[i] = (i < filled) ? '#' : '.';
        bar[kBarWidth] = '\0';
        printf("\r  %-28s [%s] %3d%%%s", stage, bar, static_cast<int>(fraction * 100.0f + 0.5f),
               fraction >= 1.0f ? "\n" : "");
        fflush(stdout);
    }
    if (!m_renderer)
        return;
    SDL_PumpEvents();
    SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
    SDL_RenderClear(m_renderer);
    if (m_engine)
        m_engine->Render();
    // Draw a subtle progress bar at the bottom of the 640x480 logical screen.
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    SDL_Rect bg = {120, 444, 400, 10};
    SDL_SetRenderDrawColor(m_renderer, 30, 35, 45, 210);
    SDL_RenderFillRect(m_renderer, &bg);
    SDL_Rect fg = {122, 446, static_cast<int>(396.0f * fraction), 6};
    SDL_SetRenderDrawColor(m_renderer, 90, 185, 255, 240);
    SDL_RenderFillRect(m_renderer, &fg);
    SDL_SetRenderDrawColor(m_renderer, 140, 210, 255, 255);
    SDL_RenderDrawRect(m_renderer, &bg);
    SDL_RenderPresent(m_renderer);
}

void FENativeInterface::AddBundle(const uint8_t *data, size_t size, const char *label)
{
    m_bundles.emplace_back(data, size);
    Uint32 lastReport = SDL_GetTicks();
    if (label && size > (512 << 10))
        ShowLoadingProgress(label, 0.0f);

    for (const uint8_t *p = data, *end = data + size; p + 8 <= end;) {
        const bChunk *c = reinterpret_cast<const bChunk *>(p);
        if (c->Data() + c->size > end)
            break;
        if (c->id == kChunkFEngPackage) {
            m_packages[LowerName(PackageShortName(c->Data()))] = c->Data();
        } else if (c->id == kChunkFEngPackageLZ && c->size > 4 + 18 && bIsCompressed(c->Data() + 4)) {
            auto buf = std::make_unique<std::vector<uint8_t>>(bGetDecompressedSize(c->Data() + 4));
            if (bDecompress(c->Data() + 4, buf->data()) == buf->size()) {
                m_packages[LowerName(PackageShortName(buf->data()))] = buf->data();
                // NFSU2_DUMP_PACKAGE=name.fng: write that package's decompressed data (dev aid).
                if (const char *dump = getenv("NFSU2_DUMP_PACKAGE"))
                    if (LowerName(dump) == LowerName(PackageShortName(buf->data())))
                        if (FILE *f = fopen(dump, "wb")) {
                            fwrite(buf->data(), 1, buf->size(), f);
                            fclose(f);
                        }
                m_unpacked.push_back(std::move(buf));
            }
        } else if (c->id == 0x00034600) { // car database: 46 records of 0x890 bytes (GLOBALB)
            constexpr uint32_t kCarRecord = 0x890;
            for (uint32_t off = 0; off + kCarRecord <= c->size; off += kCarRecord) {
                const uint8_t *r = c->Data() + off;
                const char *folder = reinterpret_cast<const char *>(r + 0x8);
                const char *manuf = reinterpret_cast<const char *>(r + 0xC8);
                if (!folder[0])
                    continue;
                CarRecord rec;
                rec.folder = folder;
                rec.manufacturer = manuf;
                rec.traffic = *reinterpret_cast<const uint32_t *>(r + 0x84c) == 2;
                { // engine/chassis, stock tuning level (see PlayerCar::Performance)
                    PlayerCar::Performance &pf = rec.perf;
                    auto f = [r](size_t off) { float v; memcpy(&v, r + off, 4); return v; };
                    pf.mass = f(0x228), pf.efficiency = f(0x2c8), pf.finalDrive = f(0x2d0), pf.reverse = f(0x2e8);
                    for (int gi = 0; gi < 6; ++gi)
                        if ((pf.gears[gi] = f(0x2f0 + gi * 4)) > 0)
                            pf.numGears = gi + 1;
                    pf.idleRpm = f(0x308), pf.redline = f(0x30c), pf.maxRpm = f(0x310);
                    for (int ti = 0; ti < 9; ++ti)
                        pf.torque[ti] = f(0x318 + ti * 4);
                    pf.aero = f(0x378), pf.grip = f(0x388), pf.rolling = f(0x3c8), pf.limiter = f(0x3a8);
                }
                for (int w = 0; w < 4; ++w) { // wheels: 0x30-byte records from +0x128
                    const uint8_t *wr = r + 0x128 + w * 0x30;
                    memcpy(&rec.wheels[w].x, wr, 12);
                    memcpy(&rec.wheels[w].radius, wr + 0x10, 4);
                    memcpy(&rec.wheels[w].width, wr + 0x14, 4);
                }
                std::string key = rec.folder;
                for (char &ch : key)
                    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                m_carTable[key] = rec;
                m_carRoster.push_back(key);
            }
        } else if (c->id == 0x00034201) { // race database
            for (uint32_t off = 0; off + 0x128 <= c->size; off += 0x128) {
                const uint8_t *r = c->Data() + off;
                RaceInfo info;
                memcpy(&info.number, r + 0x8a, 2);
                memcpy(&info.category, r + 0x7c, 4);
                memcpy(&info.typeFlags, r + 0x94, 4);
                info.special = r[0x88];
                memcpy(&info.length, r + 0x98, 4);
                memcpy(info.region, r + 0x40, sizeof(info.region));
                info.region[sizeof(info.region) - 1] = 0;
                m_races.push_back(info);
            }
        } else if (c->id == kChunkFEngFont) {
            FEFont font;
            if (font.Parse(c->Data(), c->size))
                m_fonts[FEResourceHash(font.Name().c_str())] = font;
        } else if (c->id == kChunkLanguage) {
            FELanguage lang;
            if (lang.Parse(c->Data(), c->size))
                m_languages.push_back(lang);
        } else if (c->id == 0xb3300000) {
            auto *pack = new TexturePack();
            if (pack->Parse(p, 8 + c->size))
                m_packs.push_back(pack);
            else
                delete pack;
        }
        p = c->Data() + c->size;
        if (label && size > (512 << 10)) {
            Uint32 now = SDL_GetTicks();
            if (now - lastReport >= 40) {
                ShowLoadingProgress(label, float(p - data) / float(size));
                lastReport = now;
            }
        }
    }
    if (label && size > (512 << 10))
        ShowLoadingProgress(label, 1.0f);
}

u8 *FENativeInterface::GetPackageData(const char *name, u8 **pBlockStart, bool &bDeleteBlock)
{
    const char *base = name;
    for (const char *p = name; *p; ++p)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    auto it = m_packages.find(LowerName(base));
    if (it == m_packages.end()) {
        fprintf(stderr, "FEng: package %s not found in loaded bundles\n", name);
        return nullptr;
    }
    *pBlockStart = const_cast<u8 *>(it->second);
    bDeleteBlock = false;
    return const_cast<u8 *>(it->second);
}

std::vector<std::string> FENativeInterface::PackageNames() const
{
    std::vector<std::string> names;
    for (auto &p : m_packages)
        names.push_back(p.first);
    return names;
}

bool FENativeInterface::LoadResources(FEPackage *, i32 count, FEResourceRequest *list)
{
    for (i32 i = 0; i < count; ++i)
        if ((list[i].Type == kResourceImage || list[i].Type == kResourceFont || list[i].Type == kResourceMovie) &&
            list[i].pFilename) {
            list[i].Handle = FEResourceHash(list[i].pFilename);
            if (list[i].Type == kResourceMovie) {
                std::string name = list[i].pFilename;
                size_t slash = name.find_last_of("\\/");
                if (slash != std::string::npos)
                    name = name.substr(slash + 1);
                name = name.substr(0, name.find('.'));
                m_resourceNames[list[i].Handle] = name;
            }
        }
    return true;
}

// 0x00290968 + 0x00290db8 (cFEngGameInterface::PackageWasLoaded):
void FENativeInterface::PackageWasLoaded(FEPackage *pkg)
{
    if (!pkg)
        return;
    pkg->InitializePackage();
    pkg->SetExecute(true);
    pkg->SetInputEnabled(true); // UG2 FEngine enables input by default (0x00288070)
    if (!pkg->IsLibrary() && m_engine)
        pkg->Update(m_engine, 0);

    // 0x00294558 (MenuScreen::MenuScreen): hide online/messenger icon groups by default.
    SetVisible(pkg, FEHashUpper("OL_ICON_GROUP"), false);
    SetVisible(pkg, FEHashUpper("EA_MESSENGER_GROUP"), false);

    // Screen setup first (the PS2 creates the MenuScreen, whose constructor may set the movie
    // name, before the package's movies are started).
    if (m_startup && !pkg->IsLibrary())
        m_startup->OnPackageLoaded(pkg);

    HidePCObjectsAndStartMovies cb{m_engine, pkg, this};
    pkg->ForAllObjects(cb);
}

// 0x00290788 (cFEngGameInterface::NotificationMessage):
void FENativeInterface::NotificationMessage(u32 message, FEObject *obj, u32 param1, u32 param2)
{
    if (message == kMsgDisableInputs || message == kMsgEnableInputs)
        return;
    FEPackage *pkg = reinterpret_cast<FEPackage *>(param2);
    if (getenv("NFSU2_FEDEBUG"))
        fprintf(stderr, "FENative::NotificationMessage msg=%08x pkg=%s\n", message, pkg ? pkg->GetName() : "?");
    if (m_startup && pkg)
        m_startup->NotificationMessage(message, obj, param1, pkg);
}

// 0x00290868 (cFEngGameInterface::NotifySoundMessage):
void FENativeInterface::NotifySoundMessage(u32 message, FEObject *, u32, u32 pkgPtr)
{
    FEPackage *pkg = reinterpret_cast<FEPackage *>(pkgPtr);
    if (getenv("NFSU2_FEDEBUG"))
        fprintf(stderr, "FENative::NotifySoundMessage msg=%08x pkg=%s\n", message, pkg ? pkg->GetName() : "?");
    if (m_soundHook)
        m_soundHook(message);
}

// 0x00290fe8 (cFEngGameInterface::GetJoyPadMask) + FEJoyInput.cpp:
u32 FENativeInterface::GetJoyPadMask(u8 padIndex)
{
    if (padIndex != 0)
        return 0;
    u32 mask = m_simulatedPadMask;
    // NFSU2_NOINPUT=1: scripted input only (automated runs ignore keys pressed in the window).
    static const bool scriptedOnly = getenv("NFSU2_NOINPUT") != nullptr;
    if (scriptedOnly)
        return mask;
    int numKeys = 0;
    if (const Uint8 *keys = SDL_GetKeyboardState(&numKeys)) {
        if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_KP_8])
            mask |= kPadUp;
        if (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_KP_2])
            mask |= kPadDown;
        if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_KP_4])
            mask |= kPadLeft;
        if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_KP_6])
            mask |= kPadRight;
        if (keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER] || keys[SDL_SCANCODE_SPACE])
            mask |= kPadAccept;
        if (keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER])
            mask |= kPadStart;
        if (keys[SDL_SCANCODE_BACKSPACE])
            mask |= kPadBack;
        if (keys[SDL_SCANCODE_Q] || keys[SDL_SCANCODE_PAGEUP])
            mask |= kPadLTrigger;
        if (keys[SDL_SCANCODE_E] || keys[SDL_SCANCODE_PAGEDOWN])
            mask |= kPadRTrigger;
    }
    if (m_controller && SDL_GameControllerGetAttached(m_controller)) {
        constexpr Sint16 kDeadzone = 12000;
        Sint16 lx = SDL_GameControllerGetAxis(m_controller, SDL_CONTROLLER_AXIS_LEFTX);
        Sint16 ly = SDL_GameControllerGetAxis(m_controller, SDL_CONTROLLER_AXIS_LEFTY);
        if (SDL_GameControllerGetButton(m_controller, SDL_CONTROLLER_BUTTON_DPAD_UP) || ly < -kDeadzone)
            mask |= kPadUp;
        if (SDL_GameControllerGetButton(m_controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ly > kDeadzone)
            mask |= kPadDown;
        if (SDL_GameControllerGetButton(m_controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || lx < -kDeadzone)
            mask |= kPadLeft;
        if (SDL_GameControllerGetButton(m_controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || lx > kDeadzone)
            mask |= kPadRight;
        if (SDL_GameControllerGetButton(m_controller, SDL_CONTROLLER_BUTTON_A))
            mask |= kPadAccept;
        if (SDL_GameControllerGetButton(m_controller, SDL_CONTROLLER_BUTTON_B) ||
            SDL_GameControllerGetButton(m_controller, SDL_CONTROLLER_BUTTON_BACK))
            mask |= kPadBack;
        if (SDL_GameControllerGetButton(m_controller, SDL_CONTROLLER_BUTTON_START))
            mask |= kPadStart;
        if (SDL_GameControllerGetButton(m_controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ||
            SDL_GameControllerGetAxis(m_controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > kDeadzone)
            mask |= kPadLTrigger;
        if (SDL_GameControllerGetButton(m_controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ||
            SDL_GameControllerGetAxis(m_controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > kDeadzone)
            mask |= kPadRTrigger;
        if (SDL_GameControllerGetButton(m_controller, SDL_CONTROLLER_BUTTON_X))
            mask |= kPadButton0;
        if (SDL_GameControllerGetButton(m_controller, SDL_CONTROLLER_BUTTON_Y))
            mask |= kPadButton1;
    }
    return mask;
}

SDL_Texture *FENativeInterface::TextureFor(uint32_t hash)
{
    if (!m_renderer)
        return nullptr;
    auto it = m_textures.find(hash);
    if (it != m_textures.end())
        return it->second;
    SDL_Texture *tex = nullptr;
    for (TexturePack *pack : m_packs) {
        for (size_t i = 0; i < pack->Count() && !tex; ++i) {
            const TextureInfo &info = pack->At(i);
            if (info.nameHash != hash)
                continue;
            DecodedTexture img;
            if (pack->Decode(info, img)) {
                tex = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, img.width,
                                        img.height);
                SDL_UpdateTexture(tex, nullptr, img.rgba.data(), img.width * 4);
                SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
            } else {
                fprintf(stderr, "FEng: texture %s (%d-bit) not decodable yet, drawing flat colour\n",
                        info.name.c_str(), info.bitsPerPixel);
                tex = m_white;
            }
        }
    }
    if (!tex)
        fprintf(stderr, "FEng: texture %08x not in any loaded pack\n", hash);
    m_textures[hash] = tex;
    return tex;
}

// Object transform as in MW cFEngRender::MakeRenderMatrix: scale by Size, rotate about Pivot,
// translate by Pos. Only rotation about the screen axis is used by 2D front-end screens.
FENativeInterface::Affine FENativeInterface::LocalTransform(const FEObject *obj) const
{
    const FEObjData *d = obj->GetObjData();
    Affine scale;
    scale.a = d->Size.x;
    scale.d = d->Size.y;
    float angle = 2.0f * std::atan2(d->Rot.z, d->Rot.w);
    Affine rot;
    rot.a = std::cos(angle);
    rot.b = std::sin(angle);
    rot.c = -rot.b;
    rot.d = rot.a;
    Affine toPivot, fromPivot, pos;
    toPivot.tx = -d->Pivot.x;
    toPivot.ty = -d->Pivot.y;
    fromPivot.tx = d->Pivot.x;
    fromPivot.ty = d->Pivot.y;
    pos.tx = d->Pos.x;
    pos.ty = d->Pos.y;
    return pos * fromPivot * rot * toPivot * scale;
}

void FENativeInterface::ObjectColor(const FEObject *obj, int &r, int &g, int &b, int &a) const
{
    const FEObjData *d = obj->GetObjData();
    r = std::clamp(int(d->Col.r), 0, 255);
    g = std::clamp(int(d->Col.g), 0, 255);
    b = std::clamp(int(d->Col.b), 0, 255);
    a = std::clamp(int(d->Col.a), 0, 255);
    if (obj->RenderContext != 0) {
        const Context &con = m_contexts[obj->RenderContext];
        r = (con.r * r + 128) >> 8;
        g = (con.g * g + 128) >> 8;
        b = (con.b * b + 128) >> 8;
        a = (con.a * a + 128) >> 8;
    }
}

void FENativeInterface::GenerateRenderContext(u16 uContext, FEObject *pObject)
{
    Context &con = m_contexts[uContext & 0xff];
    Affine local = LocalTransform(pObject);
    con.xform = pObject->RenderContext != 0 ? m_contexts[pObject->RenderContext].xform * local : local;
    ObjectColor(pObject, con.r, con.g, con.b, con.a);
}

bool FENativeInterface::GetContextTransform(u16 uContext, FEMatrix4 &m)
{
    m.Identify();
    if (uContext == 0)
        return true;
    const Affine &x = m_contexts[uContext & 0xff].xform;
    m.m11 = x.a;
    m.m12 = x.b;
    m.m21 = x.c;
    m.m22 = x.d;
    m.m41 = x.tx;
    m.m42 = x.ty;
    return true;
}

void FENativeInterface::GetViewTransformation(FEMatrix4 *pView)
{
    pView->Identify();
}

void FENativeInterface::DrawImage(FEObject *obj)
{
    if (!m_renderer)
        return;
    const FEImageData *d = reinterpret_cast<const FEImageData *>(obj->pData);
    if (getenv("NFSU2_FEDEBUG"))
        fprintf(stderr, "FEImage %08x pos (%.1f,%.1f,%.1f) size (%.1f,%.1f) ctx %u flags %08x\n", obj->NameHash, d->Pos.x,
                d->Pos.y, d->Pos.z, d->Size.x, d->Size.y, obj->RenderContext, obj->Flags);
    Affine xform = LocalTransform(obj);
    if (obj->RenderContext != 0)
        xform = m_contexts[obj->RenderContext].xform * xform;
    Affine screen;
    screen.tx = kScreenWidth / 2.0f;
    screen.ty = kScreenHeight / 2.0f;
    xform = screen * xform;

    int r, g, b, a;
    ObjectColor(obj, r, g, b, a);
    if (a == 0)
        return;
    SDL_Color col = {Uint8(r), Uint8(g), Uint8(b), Uint8(a)};

    SDL_Texture *tex = obj->Handle ? TextureFor(obj->Handle) : m_white;
    if (!tex)
        return; // texture not in any loaded pack: draw nothing rather than a placeholder
    DrawQuad(xform, -0.5f, -0.5f, 0.5f, 0.5f, d->UpperLeft.x, d->UpperLeft.y, d->LowerRight.x, d->LowerRight.y, col, tex);
}

void FENativeInterface::DrawQuad(const Affine &xform, float x0, float y0, float x1, float y1, float u0, float v0,
                                 float u1, float v1, SDL_Color col, SDL_Texture *tex)
{
    const float corners[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    const float uv[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
    SDL_Vertex v[4];
    for (int i = 0; i < 4; ++i) {
        xform.Apply(corners[i][0], corners[i][1], v[i].position.x, v[i].position.y);
        v[i].color = col;
        v[i].tex_coord = {uv[i][0], uv[i][1]};
    }
    const int indices[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(m_renderer, tex, v, 4, indices, 6);
}

// Text, following nfsmw FEngFont::RenderString: lines break on '\n' or '^', justification from
// FEString::Format, glyph quads at x + kern + offsetX, advancing by the glyph's advance.
void FENativeInterface::DrawString(FEObject *obj)
{
    if (!m_renderer)
        return;
    FEString *str = static_cast<FEString *>(obj);
    auto font = m_fonts.find(obj->Handle);
    if (font == m_fonts.end()) {
        static uint32_t warned = 0;
        if (warned != obj->Handle)
            fprintf(stderr, "FEng: font %08x not loaded\n", obj->Handle);
        warned = obj->Handle;
        return;
    }
    const FEFont &f = font->second;

    // Text: unless FENG_OBJECT_IGNORE_LABELHASH (0x2) is set, look up the label in the language
    // tables first (FELocalizer / 0x00292a68), falling back to the object's literal string.
    std::vector<uint16_t> text;
    if (!(obj->Flags & 0x2) && str->GetLabelHash() != 0 && str->GetLabelHash() != 0xFFFFFFFF) {
        for (const FELanguage &lang : m_languages)
            if (const char *s8 = lang.Find(str->GetLabelHash())) {
                lang.DecodeString(s8, text);
                break;
            }
    }
    if (text.empty()) {
        if (const i16 *w = str->GetString(); w && *w) {
            for (; *w; ++w)
                text.push_back(static_cast<uint16_t>(*w));
        }
    }
    if (getenv("NFSU2_FEDEBUG")) {
        const FEObjData *dd = obj->GetObjData();
        fprintf(stderr, "FEString %08x label %08x len %zu pos (%.1f,%.1f,%.1f) size (%.2f,%.2f) pivot (%.0f,%.0f) flags %08x ctx %u fmt %x a=%d text:",
                obj->NameHash, str->GetLabelHash(), text.size(), dd->Pos.x, dd->Pos.y, dd->Pos.z, dd->Size.x, dd->Size.y,
                dd->Pivot.x, dd->Pivot.y, obj->Flags, obj->RenderContext, str->Format, int(dd->Col.a));
        for (size_t i = 0; i < text.size() && i < 60; ++i)
            fputc(text[i] < 128 ? int(text[i]) : '?', stderr);
        fputc('\n', stderr);
    }
    if (text.empty())
        return;

    const bool allowIcons = (obj->Flags & 0x820) == 0;
    // Decompiled from 0x0028ef08 (GetJoyEventTextureInfo) and 0x002e53e0 / 0x00518600 / 0x0051e8c0:
    // map $ICON_SPACER$ and $JOY_EVENT_...$ tokens to PS2 button icon texture hashes.
    auto joyEventTexHash = [](const std::string &token) -> uint32_t {
        if (strcasecmp(token.c_str(), "ICON_SPACER") == 0)
            return 0xbca80379;
        static const struct {
            const char *name;
            uint32_t texHash;
        } kTable[] = {
            {"JOY_EVENT_FENG_HELP", 0xd9a22505},
            {"JOY_EVENT_MENU_HELP", 0xd9a22505},
            {"JOY_EVENT_CLEAR_TEXT", 0xd9a22505},
            {"JOY_EVENT_REPLAY_HELP", 0xd9a22505},
            {"JOY_EVENT_FENG_SELECT", 0xe795717d},
            {"JOY_EVENT_MENU_SELECT", 0xe795717d},
            {"JOY_EVENT_EXIT_EDIT_TEXT", 0xe795717d},
            {"JOY_EVENT_SKIP", 0xe795717d},
            {"JOY_EVENT_FENG_CANCEL", 0x1ac94ca9},
            {"JOY_EVENT_MENU_BACK", 0x1ac94ca9},
            {"JOY_EVENT_CANCEL_EDIT_TEXT", 0x1ac94ca9},
            {"JOY_EVENT_FENG_START", 0xe8b7d6a1},
            {"JOY_EVENT_START", 0xe8b7d6a1},
            {"JOY_EVENT_PAUSE", 0xe8b7d6a1},
            {"JOY_EVENT_FENG_BUTTON0", 0xfea90a33},
            {"JOY_EVENT_SELECT", 0xfea90a33},
            {"JOY_EVENT_RESET_CAR", 0xfea90a33},
            {"JOY_EVENT_FENG_BUTTON1", 0xff871084},
            {"JOY_EVENT_MENU_CANCEL", 0xff871084},
            {"JOY_EVENT_MENU_SQUARE", 0xff871084},
            {"JOY_EVENT_DELETE_CHAR", 0xff871084},
            {"JOY_EVENT_FENG_BUTTON2", 0x739d7071},
            {"JOY_EVENT_MENU_L2", 0x739d7071},
            {"JOY_EVENT_FENG_BUTTON3", 0x739d7137},
            {"JOY_EVENT_MENU_R2", 0x739d7137},
            {"JOY_EVENT_FENG_LTRIGGER", 0x739d7070},
            {"JOY_EVENT_MENU_L1", 0x739d7070},
            {"JOY_EVENT_MAIN_MENU_SELECT_TRANSMISSION", 0x739d7070},
            {"JOY_EVENT_CARSEL_ORBIT_OUT", 0x739d7070},
            {"JOY_EVENT_FENG_RTRIGGER", 0x739d7136},
            {"JOY_EVENT_MENU_R1", 0x739d7136},
            {"JOY_EVENT_CARSEL_ORBIT_IN", 0x739d7136},
            {"JOY_EVENT_FENG_PAD_UP", 0xe7a633c1},
            {"JOY_EVENT_SCROLL_UP", 0xe7a633c1},
            {"JOY_EVENT_FENG_PAD_DOWN", 0xe7a633b0},
            {"JOY_EVENT_SCROLL_DOWN", 0xe7a633b0},
            {"JOY_EVENT_FENG_PAD_LEFT", 0xe7a633b8},
            {"JOY_EVENT_SCROLL_LEFT", 0xe7a633b8},
            {"JOY_EVENT_FENG_PAD_RIGHT", 0xe7a633be},
            {"JOY_EVENT_SCROLL_RIGHT", 0xe7a633be},
        };
        for (const auto &e : kTable) {
            if (strcasecmp(token.c_str(), e.name) == 0)
                return e.texHash;
        }
        return FEResourceHash(token.c_str());
    };

    // 0x0028ee10 (IsJoyEventTexture) & 0x0028ee68 (SkipJoyEventTexture):
    auto parseIconToken = [&](size_t pos, size_t &endPos, SDL_Texture *&iconTex, int &iw, int &ih) -> bool {
        if (!allowIcons || pos + 2 >= text.size() || text[pos] != '$' || text[pos + 1] == '$')
            return false;
        size_t close = pos + 1;
        while (close < text.size() && text[close] != '$')
            ++close;
        if (close >= text.size() || close == pos + 1)
            return false;
        std::string token;
        token.reserve(close - (pos + 1));
        for (size_t k = pos + 1; k < close; ++k)
            token.push_back(static_cast<char>(text[k] & 0xff));
        endPos = close;
        iconTex = TextureFor(joyEventTexHash(token));
        iw = 0;
        ih = 0;
        if (iconTex)
            SDL_QueryTexture(iconTex, nullptr, nullptr, &iw, &ih);
        return true;
    };

    auto isNewline = [](uint16_t c) { return c == '\n' || c == '^'; };
    auto lineWidth = [&](size_t from) {
        float w = 0;
        uint16_t prevChar = 0;
        for (size_t i = from; i < text.size() && !isNewline(text[i]); ++i) {
            size_t endPos = i;
            SDL_Texture *iconTex = nullptr;
            int iw = 0, ih = 0;
            if (parseIconToken(i, endPos, iconTex, iw, ih)) {
                w += static_cast<float>(iw);
                i = endPos;
                prevChar = 0;
                continue;
            }
            if (const FontGlyph *g = f.Glyph(text[i])) {
                if (prevChar)
                    w += static_cast<float>(f.Kern(g, prevChar));
                w += g->advanceX;
                prevChar = text[i];
            }
        }
        return w;
    };
    int lines = 1;
    for (uint16_t c : text)
        lines += isNewline(c);
    const float leading = static_cast<float>(str->Leading);
    const float textHeight = lines * f.Height() + (lines - 1) * leading;
    const u32 format = str->Format;
    auto xOffset = [&](float w) {
        return (format & FESTRING_JUSTIFY_HCENTER) ? -0.5f * w : (format & FESTRING_JUSTIFY_HRIGHT) ? -w : 0.0f;
    };
    float y = (format & FESTRING_JUSTIFY_VCENTER) ? -0.5f * textHeight : (format & FESTRING_JUSTIFY_VBOTTOM) ? -textHeight : 0.0f;
    float x = xOffset(lineWidth(0));

    Affine xform = LocalTransform(obj);
    // Glyphs are laid out in pixels; drop the unit-quad scale and keep Size as a multiplier.
    if (obj->RenderContext != 0)
        xform = m_contexts[obj->RenderContext].xform * xform;
    Affine screen;
    screen.tx = kScreenWidth / 2.0f;
    screen.ty = kScreenHeight / 2.0f;
    xform = screen * xform;

    int r, g, b, a;
    ObjectColor(obj, r, g, b, a);
    if (a == 0)
        return;
    SDL_Color col = {Uint8(r), Uint8(g), Uint8(b), Uint8(a)};
    SDL_Texture *tex = TextureFor(f.TextureHash());
    if (!tex)
        return;
    int tw = 1, th = 1;
    SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th);

    uint16_t prev = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        uint16_t c = text[i];
        if (isNewline(c)) {
            y += f.Height() + leading;
            x = xOffset(lineWidth(i + 1));
            prev = 0;
            continue;
        }
        size_t endPos = i;
        SDL_Texture *iconTex = nullptr;
        int iw = 0, ih = 0;
        if (parseIconToken(i, endPos, iconTex, iw, ih)) {
            // 0x0028f0b0 (FEngFont::HandleJoyEventTexture): draw inline icon quad and advance by width.
            if (iconTex && iw > 0 && ih > 0) {
                DrawQuad(xform, x, y, x + float(iw), y + float(ih), 0.0f, 0.0f, 1.0f, 1.0f, col, iconTex);
                x += static_cast<float>(iw);
            }
            i = endPos;
            prev = 0;
            continue;
        }
        const FontGlyph *gl = f.Glyph(c);
        if (!gl)
            continue;
        float kern = prev ? static_cast<float>(f.Kern(gl, prev)) : 0.0f;
        float gw = std::max<float>(gl->width, 4.0f);
        float x0 = x + kern + gl->offsetX, y0 = y + gl->offsetY;
        DrawQuad(xform, x0, y0, x0 + gw, y0 + gl->height, float(gl->u) / tw, float(gl->v) / th,
                 float(gl->u + gl->width + 1) / tw, float(gl->v + gl->height) / th, col, tex);
        x += gl->advanceX + kern;
        prev = c;
    }
}

void FENativeInterface::RenderObject(FEObject *pObject)
{
    // 0x00290788 / cFEngGameInterface::RenderObject: skip objects with FF_Invisible (0x1).
    if (pObject->Flags & 1)
        return;
    switch (pObject->Type) {
    case FE_Image:
        DrawImage(pObject);
        break;
    case FE_String:
        DrawString(pObject);
        break;
    case FE_Movie:
        DrawMovie(pObject);
        break;
    default:
        // Models, multi-images etc. are not drawn yet (ISSUES.md).
        break;
    }
}

// 0x002909e8: movie file name. Plain ".mpc" for FMVOpening, the district movies and THX; "ealogo"
// and others take a language suffix (English: "_en.mpc"; other names only in this PAL region).
static std::string MovieFileName(const std::string &name)
{
    static const char *const kPlain[] = {"FMVOpening", "IndustEast", "IndustWest", "Suburban",
                                         "UpperClass", "THX_logo",   "THX_logo_fr"};
    for (const char *p : kPlain)
        if (strcasecmp(name.c_str(), p) == 0)
            return "MOVIES\\" + name + ".mpc";
    // Language 0 (English): ealogo always "_en"; others "_en" when the region check 0x002dbbf0
    // passes, which it does for this PAL build (region 1).
    return "MOVIES\\" + name + "_en.mpc";
}

void FENativeInterface::SetMovieName(FEPackage *pkg, uint32_t objHash, const char *name)
{
    FEObject *obj = pkg ? pkg->FindObjectByHash(objHash) : nullptr;
    if (!obj)
        return;
    m_movieNames[obj] = name;
    obj->Flags |= 0x400000u;
}

void FENativeInterface::StartMovie(FEObject *obj, FEPackage *pkg)
{
    std::string movie;
    if (auto set = m_movieNames.find(obj); set != m_movieNames.end())
        movie = set->second;
    else if (auto res = m_resourceNames.find(obj->Handle); res != m_resourceNames.end())
        movie = res->second;
    std::vector<uint8_t> data;
    std::string file;
    if (!movie.empty() && m_readFile) {
        file = MovieFileName(movie);
        data = m_readFile(file.c_str());
    }
    auto player = std::make_unique<MoviePlayer>();
    if (data.empty() || !player->Open(std::move(data), m_renderer)) {
        // As on PS2: a missing movie is reported done straight away.
        printf("Movie %s: not available, posting movie-done\n", file.empty() ? "(unknown)" : file.c_str());
        m_engine->QueueMessage(FEStartupSequence::kMsgMovieDone, obj, pkg, reinterpret_cast<FEObject *>(0xFFFFFFFFu), 0);
        return;
    }
    printf("Movie %s: playing\n", file.c_str());
    m_movies.push_back({obj, pkg, std::move(player), false, movie, {}, 0, 0x1A20BAu});
    LoadSubtitles(m_movies.back());
}

void FENativeInterface::DrawMovie(FEObject *obj)
{
    for (ActiveMovie &m : m_movies) {
        if (m.obj != obj)
            continue;
        if (!m.player->Update() && !m.notified) {
            m.notified = true;
            m_engine->QueueMessage(FEStartupSequence::kMsgMovieDone, obj, m.pkg, reinterpret_cast<FEObject *>(0xFFFFFFFFu), 0);
        }
        UpdateSubtitles(m);
        if (!m.player->Texture())
            return;
        Affine xform = LocalTransform(obj);
        if (obj->RenderContext != 0)
            xform = m_contexts[obj->RenderContext].xform * xform;
        Affine screen;
        screen.tx = kScreenWidth / 2.0f;
        screen.ty = kScreenHeight / 2.0f;
        xform = screen * xform;
        int r, g, b, a;
        ObjectColor(obj, r, g, b, a);
        DrawQuad(xform, -0.5f, -0.5f, 0.5f, 0.5f, 0, 0, 1, 1, SDL_Color{Uint8(r), Uint8(g), Uint8(b), Uint8(a)},
                 m.player->Texture());
        DrawSubtitles(m);
        return;
    }
}

// 0x002edc70 (FEngCreateSubtitles): load "SUBTITLES\<movie>" and read its entry table. Each entry
// is {u16 startTime, u16 pad, u32 labelHash}; the table ends at a 0xFFFF startTime. startTime is in
// the same clock as 0x002ede10 (seconds since the movie started for this build).
void FENativeInterface::LoadSubtitles(ActiveMovie &m)
{
    m.subtitles.clear();
    m.subtitleNext = 0;
    m.subtitleLabel = 0x1A20BAu;
    if (m.movieName.empty() || !m_readFile)
        return;
    std::string path = "SUBTITLES\\" + m.movieName;
    std::vector<uint8_t> data = m_readFile(path.c_str());
    if (data.size() < 8)
        return;
    for (size_t i = 0; i + 8 <= data.size(); i += 8) {
        uint16_t startTime;
        uint32_t stringHash;
        memcpy(&startTime, data.data() + i, 2);
        memcpy(&stringHash, data.data() + i + 4, 4);
        if (startTime == 0xFFFF)
            break;
        m.subtitles.push_back({startTime, stringHash});
    }
    if (getenv("NFSU2_MSGDEBUG"))
        printf("Subtitles %s: %zu entries\n", path.c_str(), m.subtitles.size());
}

// 0x002ede70 (FEngUpdateSubtitles): show the most recent entry whose startTime has elapsed;
// 0x1A20BA is the explicit clear/hide marker.
void FENativeInterface::UpdateSubtitles(ActiveMovie &m)
{
    if (m.subtitles.empty())
        return;
    const double elapsed = m.player->CurrentTime();
    while (m.subtitleNext < m.subtitles.size() && m.subtitles[m.subtitleNext].startTime <= elapsed)
        ++m.subtitleNext;
    m.subtitleLabel = m.subtitleNext == 0 ? 0x1A20BAu : m.subtitles[m.subtitleNext - 1].stringHash;
}

// Draw the current subtitle as a centered string near the bottom of the movie.
void FENativeInterface::DrawSubtitles(ActiveMovie &m)
{
    if (!m_renderer || !m.pkg || m.subtitleLabel == 0x1A20BAu || m.subtitleLabel == 0)
        return;
    std::function<u32(FEObject *)> findFont = [&](FEObject *o) -> u32 {
        if (o->Type == FE_String && m_fonts.count(o->Handle))
            return o->Handle;
        if (o->Type == FE_Group)
            for (FEObject *c = static_cast<FEGroup *>(o)->GetFirstChild(); c; c = c->GetNext()) {
                u32 f = findFont(c);
                if (f)
                    return f;
            }
        return 0;
    };
    u32 font = 0;
    for (FEObject *o = m.pkg->GetFirstObject(); o && !font; o = o->GetNext())
        font = findFont(o);
    if (!font)
        return;
    auto fit = m_fonts.find(font);
    if (fit == m_fonts.end())
        return;
    const FEFont &face = fit->second;

    // Resolve the label to text, then soft-wrap it to the on-screen subtitle width. The PS2 text
    // renderer wraps the subtitle box at its MaxWidth; DrawString only breaks on explicit markers.
    std::vector<uint16_t> cps;
    for (const FELanguage &lang : m_languages)
        if (const char *s8 = lang.Find(m.subtitleLabel)) {
            lang.DecodeString(s8, cps);
            break;
        }
    if (cps.empty())
        return;

    auto advance = [&](uint16_t c, uint16_t prev) -> float {
        const FontGlyph *g = face.Glyph(c);
        return g ? float(prev ? face.Kern(g, prev) : 0) + g->advanceX : 0.0f;
    };
    std::vector<std::vector<uint16_t>> words(1);
    for (uint16_t c : cps) {
        if (c == ' ')
            words.emplace_back();
        else if (c == '\n' || c == '^') {
            words.back().push_back(0x01); // forced-break marker
            words.emplace_back();
        } else
            words.back().push_back(c);
    }
    const float maxWidth = 580.0f;
    const float spaceWidth = advance(' ', 0);
    std::vector<std::vector<uint16_t>> lines;
    std::vector<uint16_t> line;
    float lineWidth = 0;
    for (auto &word : words) {
        if (word.empty())
            continue;
        if (word.size() == 1 && word[0] == 0x01) {
            lines.push_back(line);
            line.clear();
            lineWidth = 0;
            continue;
        }
        float w = 0;
        uint16_t prev = 0;
        for (uint16_t c : word) {
            w += advance(c, prev);
            prev = c;
        }
        if (!line.empty() && lineWidth + spaceWidth + w > maxWidth) {
            lines.push_back(line);
            line.clear();
            lineWidth = 0;
        }
        if (!line.empty()) {
            line.push_back(' ');
            lineWidth += spaceWidth;
        }
        for (uint16_t c : word)
            line.push_back(c);
        lineWidth += w;
    }
    if (!line.empty())
        lines.push_back(line);
    if (lines.empty())
        return;

    std::vector<i16> buffer;
    for (size_t li = 0; li < lines.size(); ++li) {
        if (li)
            buffer.push_back('\n');
        for (uint16_t c : lines[li])
            buffer.push_back(static_cast<i16>(c));
    }
    buffer.push_back(0);

    FEObjData *data = reinterpret_cast<FEObjData *>(ObjDataPool.Alloc(sizeof(FEObjData)));
    if (!data)
        return;
    data->Col.b = data->Col.g = data->Col.r = data->Col.a = 255;
    data->Pivot = FEVector3(0.0f, 0.0f, 0.0f);
    data->Pos = FEVector3(0.0f, 150.0f, 0.0f);
    data->Rot.x = data->Rot.y = data->Rot.z = 0.0f;
    data->Rot.w = 1.0f;
    data->Size = FEVector3(1.0f, 1.0f, 1.0f);

    FEString label;
    label.Handle = font;
    label.RenderContext = 0;
    label.Format = FESTRING_JUSTIFY_HCENTER;
    label.pData = reinterpret_cast<u8 *>(data);
    label.Flags &= ~0x2u;
    label.SetString(buffer.data());
    DrawString(&label);
}

bool FENativeInterface::PackageWillUnload(FEPackage *pkg)
{
    m_movies.erase(std::remove_if(m_movies.begin(), m_movies.end(), [pkg](const ActiveMovie &m) { return m.pkg == pkg; }),
                   m_movies.end());
    for (auto it = m_movieNames.begin(); it != m_movieNames.end();)
        it = pkg->FindObjectByGUID(it->first->GUID) == it->first ? m_movieNames.erase(it) : std::next(it);
    return true;
}
