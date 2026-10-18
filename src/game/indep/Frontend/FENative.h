#pragma once
#include "game/indep/World/PlayerCar.h"
#include "game/indep/World/SolidMesh.h"
#include <SDL.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Speed/Indep/Src/FEng/FEGameInterface.h"
#include "Speed/Indep/Src/FEng/FEngine.h"
#include "game/indep/Frontend/FEText.h"
#include "platform/linux/PlatformMovie.h"

class TexturePack;

// One record of UG2's race database (chunk 0x34201 in GLOBAL\GLOBALB, 0x128 bytes each;
// accessors 0x003389d8 / 0x00338990).
struct RaceInfo {
    int16_t number;     // +0x8a race / track number (TRACK_NAME_%d)
    int32_t category;   // +0x7c track category (icon)
    uint32_t typeFlags; // +0x94 race types this track supports (bit per type, see TrackSelect)
    uint8_t special;    // +0x88 non-zero: not offered in quick race
    int32_t length;     // +0x98 length in metres
    char region[8];     // +0x40 world region bundle, e.g. "L4RA" (TRACKS\STREAM%s.BUN)
};
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
    // Reads a file from the game data (used for movies, which are streamed separately).
    void SetFileReader(std::function<std::vector<uint8_t>(const char *)> reader) { m_readFile = std::move(reader); }
    // 0x00290cb8 (FEngMovieStarter): start the movie for a FE_Movie object, or report it done.
    void StartMovie(FEObject *obj, FEPackage *pkg);
    // 0x00259e48 / 0x00292f20 (FEngSetMovieName): set the movie a FE_Movie object plays.
    void SetMovieName(FEPackage *pkg, uint32_t objHash, const char *name);
    void SetSimulatedPadMask(u32 mask) { m_simulatedPadMask = mask; }
    void CycleCarPreview(int direction); // Stand-in for the car-select vehicle carousel.
    const std::string &PreviewCarName() const { return m_previewCarName; } // car folder, e.g. SKYLINE
    // Wheel placement for a car folder from the database (false if unknown).
    bool CarWheels(const std::string &folder, PlayerCar::Wheel out[4]) const
    {
        auto it = m_carTable.find(folder);
        if (it == m_carTable.end() || it->second.wheels[0].radius <= 0)
            return false;
        for (int i = 0; i < 4; ++i)
            out[i] = it->second.wheels[i];
        return true;
    }
    bool CarPerformance(const std::string &folder, PlayerCar::Performance &out) const
    {
        auto it = m_carTable.find(folder);
        if (it == m_carTable.end() || !it->second.perf.Valid())
            return false;
        out = it->second.perf;
        return true;
    }
    // "<manufacturer> <model>" for a car folder (the car-select label), or the folder itself.
    std::string CarDisplayName(const std::string &folder) const
    {
        auto it = m_carTable.find(folder);
        if (it == m_carTable.end())
            return folder;
        return it->second.manufacturer.empty() ? folder : it->second.manufacturer + " " + folder;
    }
    // Quick Race cars in database order (GLOBALB 0x34600, service/traffic vehicles skipped).
    std::vector<std::string> RaceableCars() const
    {
        std::vector<std::string> out;
        for (const std::string &name : m_carRoster) {
            auto it = m_carTable.find(name);
            if (it != m_carTable.end() && !it->second.traffic)
                out.push_back(name);
        }
        return out;
    }

    // Packages and textures come from loaded bundles (decompressed .BUN/.LZC data).
    void AddBundle(const uint8_t *data, size_t size, const char *label = nullptr);
    void ShowLoadingProgress(const char *stage, float fraction);
    std::vector<std::string> PackageNames() const;

    // Object helpers decompiled from UG2:
    //   0x00293010 FEngSetVisible / 0x00292f80 FEngSetInvisible
    //   0x00292a68 FEngSetLanguageHash
    //   0x00292ee8 FEngSetTextureHash
    //   0x00293240 FEngSetScript
    //   0x00293a40 FEngSetCenter / 0x00293b90 FEngGetCenter
    //   0x00293df0 FEngGetSize   / 0x00293ee0 FEngSetSize
    static void SetObjectVisible(FEObject *obj, bool visible);
    static void SetVisible(FEPackage *pkg, uint32_t objHash, bool visible);
    // 0x00292980 (FEngFindObject): searches the active packages, then the engine's libraries, so
    // name-based lookups reach objects the screen package doesn't own.
    FEObject *FindObject(uint32_t objHash) const;
    void SetVisible(uint32_t objHash, bool visible);
    void SetAlpha(uint32_t objHash, uint8_t alpha);
    static void SetLanguageHash(FEPackage *pkg, uint32_t objHash, uint32_t labelHash);
    // Set the object's colour alpha (FEObjData::Col.a). UI_Options ships its OPTION_NAME_n /
    // OPTION_DATA_n templates at alpha 18; the screen's row widgets clone them at full opacity.
    static void SetAlpha(FEPackage *pkg, uint32_t objHash, uint8_t alpha);
    // FEPrintf-style: give a string object its own text instead of a language label.
    static void SetString(FEPackage *pkg, uint32_t objHash, const char *text);
    const std::vector<RaceInfo> &Races() const { return m_races; }
    static void SetTextureHash(FEObject *obj, uint32_t texHash);
    static void SetScript(FEPackage *pkg, uint32_t objHash, uint32_t scriptHash, bool resetTime = true);
    static void GetCenter(const FEObject *obj, float &x, float &y);
    static void SetCenter(FEObject *obj, float x, float y);
    static void GetSize(const FEObject *obj, float &w, float &h);
    static void SetSize(FEObject *obj, float w, float h);

    u8 *GetPackageData(const char *pPackageName, u8 **pBlockStart, bool &bDeleteBlock) override;
    bool LoadResources(FEPackage *pPackage, i32 Count, FEResourceRequest *pList) override;
    bool UnloadResources(FEPackage *, i32, FEResourceRequest *) override { return true; }
    void PackageWasLoaded(FEPackage *pPackage) override;
    bool PackageWillUnload(FEPackage *pkg) override;
    void NotificationMessage(u32 Message, FEObject *pObject, u32 Param1, u32 Param2) override;
    void NotifySoundMessage(u32 Message, FEObject *pObject, u32 ControlMask, u32 pPackagePtr) override;
    void BeginPackageRendering(FEPackage *) override {}
    void EndPackageRendering(FEPackage *) override {}
    void RenderCarPreviewOverlay(); // Stand-in overlay for the unported UG2 car renderer.
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
    struct PreviewVertex {
        float x, y, z;
    };
    struct PreviewTriangle {
        PreviewVertex v[3];
    };
    struct CarRecord {
        std::string folder;
        std::string manufacturer;
        bool traffic = false; // chunk field +0x84c == 2: service/traffic vehicle, not selectable
        PlayerCar::Wheel wheels[4] = {};
        PlayerCar::Performance perf;
    };

    Affine LocalTransform(const FEObject *obj) const;
    void ObjectColor(const FEObject *obj, int &r, int &g, int &b, int &a) const;
    SDL_Texture *TextureFor(uint32_t hash);
    bool LoadCarPreview(const char *model);
    void DrawCarPreviewLabel(FEPackage *pkg);
    void UpdateStatBars(FEPackage *pkg);
    std::string m_statBarsCar; // car the stat bars were last set for
    void DrawImage(FEObject *obj);
    void DrawString(FEObject *obj);
    void DrawQuad(const Affine &xform, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                  SDL_Color col, SDL_Texture *tex);

    SDL_Renderer *m_renderer;
    FEngine *m_engine = nullptr;
    FEStartupSequence *m_startup = nullptr;

    void DrawMovie(FEObject *obj);
    // One entry of a "SUBTITLES\<movie>" file (not the 0xFFFF terminator): a decisecond/second
    // start time and the localized-string label hash to show from then on (0x1A20BA clears).
    struct SubtitleEntry {
        uint16_t startTime;
        uint32_t stringHash;
    };
    struct ActiveMovie {
        FEObject *obj;
        FEPackage *pkg;
        std::unique_ptr<MoviePlayer> player;
        bool notified;
        std::string movieName;
        std::vector<SubtitleEntry> subtitles;
        size_t subtitleNext = 0;
        uint32_t subtitleLabel = 0x1A20BAu; // current label (0x1A20BA = hidden)
    };
    void LoadSubtitles(ActiveMovie &m);
    void UpdateSubtitles(ActiveMovie &m);
    void DrawSubtitles(ActiveMovie &m);
    std::vector<ActiveMovie> m_movies;
    std::map<uint32_t, std::string> m_resourceNames; // resource handle -> name (movies)
    std::map<FEObject *, std::string> m_movieNames;
    std::vector<RaceInfo> m_races;  // names set by screens (PS2: string at obj+0x24)
    std::map<std::string, CarRecord> m_carTable; // GLOBALB chunk 0x34600, keyed by upper-case folder
    std::vector<std::string> m_carRoster;         // same chunk, in record order (car-select carousel)
    std::vector<PreviewTriangle> m_carPreviewTris;
    std::vector<SolidMesh::Triangle> m_carPreviewModel; // assembled stock body (GL preview)
    std::vector<uint32_t> m_carPreviewSlots;            // its material slot hashes
    size_t m_carPreviewWheels = SIZE_MAX;               // first wheel triangle
    std::vector<SDL_Vertex> m_carPreviewSplats;
    bool m_carPreviewSplatsBuilt = false;
    bool m_carPreviewLoadAttempted = false;
    size_t m_previewCarIndex = 6; // Current carousel slot (fallback roster default: SKYLINE).
    std::string m_previewCarName = "SKYLINE";
    std::function<std::vector<uint8_t>(const char *)> m_readFile;
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
