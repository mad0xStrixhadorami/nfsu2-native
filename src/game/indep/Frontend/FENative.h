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
#include "game/indep/Frontend/PaintCatalog.h"
#include "game/indep/Frontend/PartCatalog.h"
#include "game/indep/Frontend/PerfPackages.h"
#include "game/indep/Frontend/PerfModel.h"
#include "game/indep/Frontend/TuneSystem.h"
#include "platform/linux/PlatformMovie.h"

class TexturePack;

// One record of UG2's race database (chunk 0x34201 in GLOBAL\GLOBALB, 0x128 bytes each;
// accessors 0x003389d8 / 0x00338990).
struct RaceInfo {
    int16_t number;     // +0x8a race / track number (TRACK_NAME_%d)
    int32_t category;   // +0x7c track category (icon)
    uint32_t typeFlags; // +0x94 race types this track supports (bit per type, see TrackSelect)
    uint8_t special;    // +0x88 non-zero: not offered in quick race
    uint8_t directionOption; // +0x86: the race supports the Track Direction option (0x001f7a80)
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
    // Called for every FEng sound notification (UI sounds, ISSUES.md #126).
    void SetSoundHook(std::function<void(uint32_t msg)> hook) { m_soundHook = std::move(hook); }
    void SetFileReader(std::function<std::vector<uint8_t>(const char *)> reader) { m_readFile = std::move(reader); }
    std::vector<uint8_t> ReadGameFile(const char *name) const { return m_readFile ? m_readFile(name) : std::vector<uint8_t>(); }
    // 0x00290cb8 (FEngMovieStarter): start the movie for a FE_Movie object, or report it done.
    void StartMovie(FEObject *obj, FEPackage *pkg);
    // 0x00259e48 / 0x00292f20 (FEngSetMovieName): set the movie a FE_Movie object plays.
    void SetMovieName(FEPackage *pkg, uint32_t objHash, const char *name);
    void SetSimulatedPadMask(u32 mask) { m_simulatedPadMask = mask; }
    // While a text-entry screen is up the letter keys type instead of driving the pad (WASD, Q, E, Space).
    void SetTextInputMode(bool on) { m_textInputMode = on; }
    bool TextInputMode() const { return m_textInputMode; }
    void CycleCarPreview(int direction); // Stand-in for the car-select vehicle carousel.
    // Load a specific car as the car-select/race preview (career mode restores the saved car).
    bool SetPreviewCar(const std::string &model);
    const std::string &PreviewCarName() const { return m_previewCarName; } // car folder, e.g. SKYLINE
    // Body paint: the set selects a PaintCatalog color set (GLOSS/METAL/PEARL, ISSUES #168) and the
    // 1-based index selects At(set, index - 1); index 0 is the car's stock paint.  Both values are
    // what OwnedCar::paintSet/paint store, so they survive through the memory-card save.
    void SetPreviewPaint(uint32_t index) { m_previewPaint = index; } // keep the current finish
    void SetPreviewPaint(uint32_t set, uint32_t index)
    {
        m_previewPaintSet = set ? set : PaintCatalog::kGloss;
        m_previewPaint = index;
    }
    uint32_t PreviewPaint() const { return m_previewPaint; }
    uint32_t PreviewPaintSet() const { return m_previewPaintSet; }
    // The paint on the preview car's rim faces (Rims/Spinners cards, ISSUES #172).  index 0 keeps
    // the stock dark wheels; the set is normally PaintCatalog::kRims.
    void SetPreviewRimPaint(uint32_t set, uint32_t index)
    {
        m_previewRimSet = set ? set : PaintCatalog::kRims;
        m_previewRim = index;
    }
    uint32_t PreviewRimPaint() const { return m_previewRim; }
    uint32_t PreviewRimSet() const { return m_previewRimSet; }
    // The paint on the preview car's brake calipers (Brakes card, ISSUES #173).  index 0 keeps
    // the stock grey calipers; the set is PaintCatalog::kBrakes.
    void SetPreviewCaliperPaint(uint32_t set, uint32_t index)
    {
        m_previewCaliperSet = set ? set : PaintCatalog::kBrakes;
        m_previewCaliper = index;
    }
    uint32_t PreviewCaliperPaint() const { return m_previewCaliper; }
    uint32_t PreviewCaliperSet() const { return m_previewCaliperSet; }
    // The paint on the preview car's mirror housings (Mirrors card, ISSUES #174).  index 0 keeps
    // the stock body colour; the set is normally PaintCatalog::kGloss.
    void SetPreviewMirrorPaint(uint32_t set, uint32_t index)
    {
        m_previewMirrorSet = set ? set : PaintCatalog::kGloss;
        m_previewMirror = index;
    }
    uint32_t PreviewMirrorPaint() const { return m_previewMirror; }
    uint32_t PreviewMirrorSet() const { return m_previewMirrorSet; }
    // The paint on the preview car's stock spoiler (Spoiler card, ISSUES #175).  index 0 keeps the
    // body colour; the set is normally PaintCatalog::kGloss.
    void SetPreviewSpoilerPaint(uint32_t set, uint32_t index)
    {
        m_previewSpoilerSet = set ? set : PaintCatalog::kGloss;
        m_previewSpoiler = index;
    }
    uint32_t PreviewSpoilerPaint() const { return m_previewSpoiler; }
    uint32_t PreviewSpoilerSet() const { return m_previewSpoilerSet; }
    // The spoiler *part* fitted to the preview car (Body Shop, ISSUES #185): style 1..40 of
    // CARS\SPOILER\GEOMETRY.BIN (SPOILER_STYLE<NN>[_CF]_A), 0 = the stock wing.  Unlike the paint
    // above this changes the geometry, so setting it rebuilds the preview when one is loaded.
    void SetPreviewSpoilerPart(int style, bool carbon);
    int PreviewSpoilerPart() const { return m_previewPartSpoiler; }
    bool PreviewSpoilerPartCarbon() const { return m_previewPartSpoilerCarbon; }
    // Roof scoop / exhaust tip fitted by the Body Shop (#186).  style 0 keeps the stock part.
    void SetPreviewRoofPart(int style, bool dual, bool offset, bool carbon);
    int PreviewRoofPart() const { return m_previewPartRoof; }
    void SetPreviewExhaustPart(int style);
    int PreviewExhaustPart() const { return m_previewPartExhaust; }
    // Hood / headlights / taillights fitted by the Body Shop (#188).  These are solids in the car's
    // own GEOMETRY.BIN; style 0 keeps the stock panel.  The carbon hood reuses the painted solid
    // (the files ship no _CF hood), so only the purchase/price flag differs.
    void SetPreviewHoodPart(int style, bool carbon);
    int PreviewHoodPart() const { return m_previewPartHood; }
    bool PreviewHoodPartCarbon() const { return m_previewPartHoodCarbon; }
    void SetPreviewHeadlightPart(int style);
    int PreviewHeadlightPart() const { return m_previewPartHeadlight; }
    void SetPreviewTaillightPart(int style);
    int PreviewTaillightPart() const { return m_previewPartTaillight; }
    // Bumpers and side skirts fitted by the Body Shop (#190).  These are the car file's KIT<NN>
    // panels; the value is the catalogue's kit number, 0 keeps the stock panel.
    // True if the previewed car's GEOMETRY.BIN contains a panel for this catalogue part id/style
    // (#192).  Body cards hide designs the car does not ship; before the first preview load the
    // answer is optimistic so an unloaded preview never empties the shop.
    bool PreviewHasBodyStyle(int partId, int style) const;
    void SetPreviewFrontBumperPart(int kit);
    int PreviewFrontBumperPart() const { return m_previewPartFrontBumper; }
    void SetPreviewRearBumperPart(int kit);
    int PreviewRearBumperPart() const { return m_previewPartRearBumper; }
    void SetPreviewSkirtPart(int kit);
    int PreviewSkirtPart() const { return m_previewPartSkirt; }
    // Wide-body kit fitted by the Body Shop (#193): 1..4 selects <CAR>_KITW<NN>_BODY_A and the
    // matching door/panel/sill solids, 0 keeps the stock KIT00 shell.
    void SetPreviewWideBodyPart(int kit);
    int PreviewWideBodyPart() const { return m_previewPartWideBody; }
    // The dev environment's NFSU2_PART_* selection overridden by the preview setters, i.e. the
    // parts the currently previewed car should be assembled with.  Both the frontend preview and
    // the race car loader use this so a bought panel shows in the race (#188).
    PlayerCar::InstalledParts InstalledPartsForPreview() const;
    // The paint on the installed aftermarket parts (Roof Scoop / Engine Accents / Muffler Tip
    // cards, ISSUES.md #178).  index 0 keeps each part's stock colour; the sets are normally
    // PaintCatalog::kGloss / kHoses / kMufflers.
    void SetPreviewRoofPaint(uint32_t set, uint32_t index)
    {
        m_previewRoofSet = set ? set : PaintCatalog::kGloss;
        m_previewRoof = index;
    }
    uint32_t PreviewRoofPaint() const { return m_previewRoof; }
    uint32_t PreviewRoofSet() const { return m_previewRoofSet; }
    void SetPreviewEnginePaint(uint32_t set, uint32_t index)
    {
        m_previewEngineSet = set ? set : PaintCatalog::kHoses;
        m_previewEngine = index;
    }
    uint32_t PreviewEnginePaint() const { return m_previewEngine; }
    uint32_t PreviewEngineSet() const { return m_previewEngineSet; }
    void SetPreviewExhaustPaint(uint32_t set, uint32_t index)
    {
        m_previewExhaustSet = set ? set : PaintCatalog::kMufflers;
        m_previewExhaust = index;
    }
    uint32_t PreviewExhaustPaint() const { return m_previewExhaust; }
    uint32_t PreviewExhaustSet() const { return m_previewExhaustSet; }
    const PaintCatalog &Paint() const { return m_paint; }
    // GLOBALB's shop part records (spoilers, roof scoops, ...) and their price tiers (#184).
    const PartCatalog &Parts() const { return m_parts; }
    const PerfPackages &PerfParts() const { return m_perf; }
    static void ReadCarPerformance(const uint8_t *record, PlayerCar::Performance &out);
    std::vector<uint8_t> TunedCarRecord(const std::string &folder, const std::vector<uint16_t> &parts,
                                        const std::vector<uint8_t> &tune = {}) const;
    const Tune::Table &TuneTable() const { return m_tuneTable; }
    // The stock record and the part-tuned 0x2b0-byte frame a tuning session starts from (empty if unknown).
    bool TuneInputs(const std::string &folder, const std::vector<uint16_t> &parts, const uint8_t *&record,
                    std::vector<uint8_t> &frame) const;
    // The performance parts fitted to the car on show (the career car's OwnedCar::perfParts).
    void SetPreviewPerfParts(const std::vector<uint16_t> &parts) { m_previewPerfParts = parts; }
    void SetPreviewTune(const std::vector<uint8_t> &tune) { m_previewTune = tune; }
    const std::vector<uint8_t> &PreviewTune() const { return m_previewTune; }
    const std::vector<uint16_t> &PreviewPerfParts() const { return m_previewPerfParts; }
    // RGB for a paint index; `stock` is returned for 0 or an out-of-range index.
    SDL_Color ColorForPaint(uint32_t set, uint32_t index, SDL_Color stock) const
    {
        if (index >= 1 && index <= m_paint.Count(set)) {
            const PaintColor &c = m_paint.At(set, index - 1);
            return SDL_Color{c.r, c.g, c.b, 255};
        }
        return stock;
    }
    SDL_Color ColorForPaint(uint32_t index, SDL_Color stock) const
    {
        return ColorForPaint(m_previewPaintSet, index, stock);
    }
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
        std::string key = folder;
        for (char &ch : key)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        auto it = m_carTable.find(key);
        if (it == m_carTable.end())
            return folder;
        return it->second.manufacturer.empty() ? folder : it->second.manufacturer + " " + folder;
    }
    std::string CarManufacturer(const std::string &folder) const
    {
        std::string key = folder;
        for (char &ch : key)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        auto it = m_carTable.find(key);
        return it != m_carTable.end() ? it->second.manufacturer : "";
    }
    // Raw database record of a car folder (nullptr if unknown).
    const uint8_t *CarRecordData(const std::string &folder) const
    {
        auto it = m_carTable.find(folder);
        return it != m_carTable.end() && it->second.stock.size() == 0x890 ? it->second.stock.data() : nullptr;
    }
    // Database record index of a car folder = UG2's car type (-1 if unknown).
    int CarTypeIndex(const std::string &folder) const
    {
        for (size_t i = 0; i < m_carRoster.size(); ++i)
            if (m_carRoster[i] == folder)
                return int(i);
        return -1;
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

    // Service/traffic vehicles in database order (the ones RaceableCars skips).
    std::vector<std::string> TrafficCars() const
    {
        std::vector<std::string> out;
        for (const std::string &name : m_carRoster) {
            auto it = m_carTable.find(name);
            if (it != m_carTable.end() && it->second.traffic)
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
    // The language string for a label hash as ASCII ('?' outside it); empty when no table has it.
    std::string LanguageText(uint32_t labelHash) const;
    const std::vector<RaceInfo> &Races() const { return m_races; }
    static void SetTextureHash(FEObject *obj, uint32_t texHash);
    static void SetTextureHash(FEPackage *pkg, uint32_t objHash, uint32_t texHash);
    // previewParts (package screens): also fill the "<name>_PREVIEW" bars for that fitted set.
    void UpdateStatBars(FEPackage *pkg, bool force = false, const std::vector<uint16_t> *previewParts = nullptr);
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
    // Credits.fng (ISSUES #158): the package ships one template string ("DEFAULT STRING", 0xe6093486)
    // that the game's credits screen repeats per line while scrolling it up the panel. The host
    // draws the template once per visible line, moving it up at `speed` px/s inside [top, bottom].
    void StartTextScroll(FEPackage *pkg, uint32_t objHash, std::vector<std::string> lines, float startY,
                         float lineHeight, float speed, float top, float bottom);
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
        std::vector<uint8_t> raw;   // the 0x890-byte record as stored
        std::vector<uint8_t> stock; // raw with the stock performance model applied (PerfModel)
    };

    Affine LocalTransform(const FEObject *obj) const;
    void ObjectColor(const FEObject *obj, int &r, int &g, int &b, int &a) const;
    SDL_Texture *TextureFor(uint32_t hash);
    bool LoadCarPreview(const char *model);
    void DrawCarPreviewLabel(FEPackage *pkg);
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
    PaintCatalog m_paint;                          // GLOBALB chunk 0x80034602 GLOSS set (#167)
    std::vector<uint16_t> m_previewPerfParts;
    std::vector<uint8_t> m_previewTune;
    bool m_textInputMode = false;
    Tune::Table m_tuneTable;                       // chunk 0x34a1a slider definitions (#207)
    PerfPackages m_perf;                           // performance packages, chunk 0x34a15 (#197)
    PartCatalog m_parts;                           // the same pack's shop records (#184)
    std::vector<PreviewTriangle> m_carPreviewTris;
    std::vector<SolidMesh::Triangle> m_carPreviewModel; // assembled stock body (GL preview)
    std::vector<uint32_t> m_carPreviewSlots;            // its material slot hashes
    std::vector<uint8_t> m_carPreviewRims;              // 1 = painted rim face (#172)
    std::vector<uint8_t> m_carPreviewCalipers;          // 1 = brake caliper (#173)
    std::vector<uint8_t> m_carPreviewMirrors;           // 1 = painted mirror housing (#174)
    std::vector<uint8_t> m_carPreviewSpoilers;          // 1 = stock spoiler mesh (#175)
    std::vector<uint8_t> m_carPreviewRoofs;             // 1 = installed roof scoop (#178)
    std::vector<uint8_t> m_carPreviewEngines;           // 1 = installed engine accent (#178)
    std::vector<uint8_t> m_carPreviewExhausts;          // 1 = installed muffler tip (#178)
    size_t m_carPreviewWheels = SIZE_MAX;               // first wheel triangle
    std::vector<SDL_Vertex> m_carPreviewSplats;
    bool m_carPreviewSplatsBuilt = false;
    bool m_carPreviewLoadAttempted = false;
    size_t m_previewCarIndex = 6; // Current carousel slot (fallback roster default: SKYLINE).
    std::string m_previewCarName = "SKYLINE";
    uint32_t m_previewPaint = 0; // 0 = stock; else 1..PaintCatalog::Count(set)
    uint32_t m_previewPaintSet = PaintCatalog::kGloss; // the finish the index belongs to (#168)
    uint32_t m_previewRim = 0; // 0 = stock dark wheels; else 1..Count(kRims)
    uint32_t m_previewRimSet = PaintCatalog::kRims;
    uint32_t m_previewCaliper = 0; // 0 = stock grey calipers; else 1..Count(kBrakes)
    uint32_t m_previewCaliperSet = PaintCatalog::kBrakes;
    uint32_t m_previewMirror = 0; // 0 = stock body colour; else 1..Count(mirror set)
    uint32_t m_previewMirrorSet = PaintCatalog::kGloss;
    uint32_t m_previewSpoiler = 0; // 0 = body colour; else 1..Count(spoiler set)
    uint32_t m_previewSpoilerSet = PaintCatalog::kGloss;
    int m_previewPartSpoiler = 0;       // fitted spoiler style 1..40 (0 = stock, #185)
    bool m_previewPartSpoilerCarbon = false;
    int m_previewPartRoof = 0;          // fitted roof scoop 1..17 (#186)
    bool m_previewPartRoofDual = false, m_previewPartRoofOffset = false, m_previewPartRoofCarbon = false;
    int m_previewPartExhaust = 0;       // fitted exhaust tip 1..10 (#186)
    int m_previewPartHood = 0;          // fitted hood 1..28 (#188)
    bool m_previewPartHoodCarbon = false;
    int m_previewPartHeadlight = 0;     // fitted headlights 1..14 (#188)
    int m_previewPartTaillight = 0;     // fitted taillights 1..14 (#188)
    int m_previewPartFrontBumper = 0;   // fitted front bumper kit 1..30 (#190)
    int m_previewPartRearBumper = 0;    // fitted rear bumper kit 1..30 (#190)
    int m_previewPartSkirt = 0;         // fitted side skirt kit 1..30 (#190)
    int m_previewPartWideBody = 0;      // fitted wide-body kit 1..4 (#193)
    SolidMesh::BodyStyleSet m_previewBodyStyles; // body panels present in the previewed car (#192)
    bool m_previewBodyStylesKnown = false;
    uint32_t m_previewRoof = 0; // 0 = body colour; else 1..Count(gloss set)
    uint32_t m_previewRoofSet = PaintCatalog::kGloss;
    uint32_t m_previewEngine = 0; // 0 = stock engine accent colour; else 1..Count(hoses set)
    uint32_t m_previewEngineSet = PaintCatalog::kHoses;
    uint32_t m_previewExhaust = 0; // 0 = stock steel; else 1..Count(mufflers set)
    uint32_t m_previewExhaustSet = PaintCatalog::kMufflers;
    std::function<std::vector<uint8_t>(const char *)> m_readFile;
    struct TextScroll {
        FEObject *obj = nullptr;
        std::vector<std::string> lines;
        float startY = 0, lineHeight = 24, speed = 30, top = -180, bottom = 180;
        uint32_t startMs = 0;
    } m_textScroll;
    std::function<void(uint32_t)> m_soundHook;
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
