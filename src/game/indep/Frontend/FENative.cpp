#include "game/indep/Frontend/FENative.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "Speed/Indep/Src/FEng/FEGroup.h"
#include "Speed/Indep/Src/FEng/FEImage.h"
#include "Speed/Indep/Src/FEng/FEObject.h"
#include "Speed/Indep/Src/FEng/FEObjectCallback.h"
#include "Speed/Indep/Src/FEng/FEPackage.h"
#include "Speed/Indep/Src/FEng/FEScript.h"
#include "Speed/Indep/Src/FEng/FEString.h"
#include "Speed/Indep/bWare/Inc/Strings.hpp"
#include "game/bWare/bChunk.h"
#include "game/bWare/bCompression.h"
#include "game/indep/Frontend/FEStartup.h"
#include "game/indep/Texture/TexturePack.h"

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
                m_unpacked.push_back(std::move(buf));
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

    HidePCObjectsAndStartMovies cb{m_engine, pkg, this};
    pkg->ForAllObjects(cb);

    if (m_startup && !pkg->IsLibrary())
        m_startup->OnPackageLoaded(pkg);
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
}

// 0x00290fe8 (cFEngGameInterface::GetJoyPadMask) + FEJoyInput.cpp:
u32 FENativeInterface::GetJoyPadMask(u8 padIndex)
{
    if (padIndex != 0)
        return 0;
    u32 mask = m_simulatedPadMask;
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

void FENativeInterface::StartMovie(FEObject *obj, FEPackage *pkg)
{
    auto name = m_resourceNames.find(obj->Handle);
    std::vector<uint8_t> data;
    std::string file;
    if (name != m_resourceNames.end() && m_readFile) {
        file = MovieFileName(name->second);
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
    m_movies.push_back({obj, pkg, std::move(player), false});
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
        return;
    }
}

bool FENativeInterface::PackageWillUnload(FEPackage *pkg)
{
    m_movies.erase(std::remove_if(m_movies.begin(), m_movies.end(), [pkg](const ActiveMovie &m) { return m.pkg == pkg; }),
                   m_movies.end());
    return true;
}
