#pragma once
#include <SDL.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Speed/Indep/Src/FEng/FEGameInterface.h"
#include "Speed/Indep/Src/FEng/FEngine.h"
#include "game/indep/Frontend/FEText.h"

class TexturePack;
class FEStartupSequence;

// Native front-end host: the game side of EA's FEng engine, replacing MW's cFEng/cFEngRender
// (which drive the console renderer). Draws FEng objects with SDL's geometry API.
class FENativeInterface : public FEGameInterface {
public:
    static constexpr int kScreenWidth = 640;  // FEng screen space (SCREEN_WIDTH in MW)
    static constexpr int kScreenHeight = 480;

    // FEng pad mask bits (from FEJoyInput.cpp MapJoyEventToFEPad / UG2 0x00288fc8):
    static constexpr u32 kPadUp = 0x00000001;
    static constexpr u32 kPadDown = 0x00000002;
    static constexpr u32 kPadLeft = 0x00000004;
    static constexpr u32 kPadRight = 0x00000008;
    static constexpr u32 kPadAccept = 0x00000010;
    static constexpr u32 kPadBack = 0x00000020;
    static constexpr u32 kPadStart = 0x00000040;
    static constexpr u32 kPadLTrigger = 0x00000080;
    static constexpr u32 kPadRTrigger = 0x00000100;
    static constexpr u32 kPadButton0 = 0x00000200;
    static constexpr u32 kPadButton1 = 0x00000400;
    static constexpr u32 kPadButton2 = 0x00000800;
    static constexpr u32 kPadButton3 = 0x00001000;
    static constexpr u32 kPadButton4 = 0x00002000;
    static constexpr u32 kPadButton5 = 0x00004000;

    explicit FENativeInterface(SDL_Renderer *renderer);
    virtual ~FENativeInterface();

    // Attach to an FEngine instance and configure joypads (0x002914a0 / cFEng::cFEng).
    void AttachEngine(FEngine *engine);
    void SetStartupSequence(FEStartupSequence *startup) { m_startup = startup; }
    void SetSimulatedPadMask(u32 mask) { m_simulatedPadMask = mask; }

    // Packages and textures come from loaded bundles (decompressed .BUN/.LZC data).
    void AddBundle(const uint8_t *data, size_t size);
    std::vector<std::string> PackageNames() const;

    // Object helpers decompiled from UG2:
    //   0x00293010 FEngSetVisible / 0x00292f80 FEngSetInvisible
    //   0x00292a68 FEngSetLanguageHash
    //   0x00293240 FEngSetScript
    static void SetObjectVisible(FEObject *obj, bool visible);
    static void SetVisible(FEPackage *pkg, uint32_t objHash, bool visible);
    static void SetLanguageHash(FEPackage *pkg, uint32_t objHash, uint32_t labelHash);
    static void SetScript(FEPackage *pkg, uint32_t objHash, uint32_t scriptHash, bool resetTime = true);

    u8 *GetPackageData(const char *pPackageName, u8 **pBlockStart, bool &bDeleteBlock) override;
    bool LoadResources(FEPackage *pPackage, i32 Count, FEResourceRequest *pList) override;
    bool UnloadResources(FEPackage *, i32, FEResourceRequest *) override { return true; }
    void PackageWasLoaded(FEPackage *pPackage) override;
    bool PackageWillUnload(FEPackage *) override { return true; }
    void NotificationMessage(u32 Message, FEObject *pObject, u32 Param1, u32 Param2) override;
    void NotifySoundMessage(u32 Message, FEObject *pObject, u32 ControlMask, u32 pPackagePtr) override;
    void BeginPackageRendering(FEPackage *) override {}
    void EndPackageRendering(FEPackage *) override {}
    void GenerateRenderContext(u16 uContext, FEObject *pObject) override;
    bool GetContextTransform(u16 uContext, FEMatrix4 &Matrix) override;
    void RenderObject(FEObject *pObject) override;
    void GetViewTransformation(FEMatrix4 *pView) override;
    u32 GetJoyPadMask(u8 padIndex) override;
    void GetMouseInfo(FEMouseInfo &) override {}
    bool DoesPointTouchObject(float, float, FEObject *) override { return false; }

private:
    // 2D affine transform: x' = a*x + c*y + tx, y' = b*x + d*y + ty
    struct Affine {
        float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
        Affine operator*(const Affine &o) const; // apply o first, then this
        void Apply(float x, float y, float &ox, float &oy) const;
    };
    struct Context {
        Affine xform;
        int r = 255, g = 255, b = 255, a = 255;
    };

    Affine LocalTransform(const FEObject *obj) const;
    void ObjectColor(const FEObject *obj, int &r, int &g, int &b, int &a) const;
    SDL_Texture *TextureFor(uint32_t hash);
    void DrawImage(FEObject *obj);
    void DrawString(FEObject *obj);
    void DrawQuad(const Affine &xform, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                  SDL_Color col, SDL_Texture *tex);

    SDL_Renderer *m_renderer;
    FEngine *m_engine = nullptr;
    FEStartupSequence *m_startup = nullptr;
    SDL_GameController *m_controller = nullptr;
    u32 m_simulatedPadMask = 0;
    std::vector<std::pair<const uint8_t *, size_t>> m_bundles;
    std::map<std::string, const uint8_t *> m_packages;              // FEng package data by lower-case short name
    std::vector<std::unique_ptr<std::vector<uint8_t>>> m_unpacked;  // decompressed packages (0x30210)
    std::vector<TexturePack *> m_packs;
    std::map<uint32_t, SDL_Texture *> m_textures; // by texture name hash
    SDL_Texture *m_white = nullptr;               // stand-in for textures we cannot decode yet
    std::map<uint32_t, FEFont> m_fonts;           // by font name hash
    std::vector<FELanguage> m_languages;          // string tables, searched in load order
    Context m_contexts[256];
};
