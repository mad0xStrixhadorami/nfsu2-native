#include "game/indep/Frontend/FEStartup.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "Speed/Indep/bWare/Inc/Strings.hpp"
#include "game/indep/Career/Memcard.h"
#include "game/indep/Career/Profile.h"
#include "game/indep/World/CarMaterial.h"
#include "game/indep/Frontend/FENative.h"
#include "Speed/Indep/Src/FEng/FEGroup.h"
#include "Speed/Indep/Src/FEng/FEImage.h"
#include "Speed/Indep/Src/FEng/FEString.h"

namespace {
void ApplyYesNoFocus(FEPackage *pkg, int selected); // the two-button prompt, defined with the dialog code

// 0x004e6ba0 (NTSC) and 0x004e6bd0 (PAL): 12-entry startup tables read by 0x0023ae10.
// Empty strings (0x00566608) are skipped when building the linked list.
constexpr const char *kStartupTableNTSC[12] = {
    "MC_Bootup.fng",
    "",
    "",
    "LS_EAlogo.fng",
    "LS_THXMovie.fng",
    "",
    "",
    "LS_PSAMovie.fng",
    "UG_LS_IntroFMV.fng",
    "UG_LS_Splash.fng",
    "MC_Background.fng",
    "UI_Main.fng",
};

constexpr const char *kStartupTablePAL[12] = {
    "",
    "LS_LangSelect.fng",
    "MC_Bootup.fng",
    "LS_EAlogo.fng",
    "LS_THXMovie.fng",
    "",
    "",
    "LS_PSAMovie.fng",
    "UG_LS_IntroFMV.fng",
    "UG_LS_Splash.fng",
    "MC_Background.fng",
    "UI_Main.fng",
};

// Memory-card screens. MC_List.fng (create 0x002286b8, ctor 0x00228348) is the file browser: 8 rows,
// each a group (backing + two icons) with a name string on the left and a value string on the right.
// MC_Main.fng (create 0x00225f50, ctor 0x00226720, dispatch 0x00228700) is the card status screen the
// career options open; its CALL button (0x47ff4e7c, labelled "CALL" by 0x00293378 in the tick) pushes
// MC_List.fng over it, and 0x00228990/0x00226908 build the rows from the memcard library. The
// packages carry no object names (Ghidra reads raw hashes), but each row's three objects are numbered
// 1..8, so their hashes are consecutive and the rows run top to bottom (y -120..161).
constexpr uint32_t kMcListRow[8] = {0x66e29936, 0x66e29937, 0x66e29938, 0x66e29939,
                                    0x66e2993a, 0x66e2993b, 0x66e2993c, 0x66e2993d};
constexpr uint32_t kMcListName[8] = {0xb2980ba8, 0xb2980ba9, 0xb2980baa, 0xb2980bab,
                                     0xb2980bac, 0xb2980bad, 0xb2980bae, 0xb2980baf};
// The row's right-hand column is 9b483b01..08 (template 0x465ac8, x 415); it sits outside the 640x480
// frame in this layout, so the port leaves it alone.
constexpr int kMcListRows = 8;
// MC_List.fng ships its dialog (frame, side buttons, scroll bar, title) at alpha 0; EA's class fades
// it in when the rows are filled (0x00228348 / 0x00228990). These are the frame objects to reveal.
constexpr uint32_t kMcListFrame[] = {0x8d7e0c53, 0x44496a02, 0x44496a03, 0x688a1551, 0x3fbb7a4e};
// MC_Main.fng's free-standing status string ("Checking for memory card (PS2) in MEMORY CARD slot 1")
constexpr uint32_t kMcStatusString = 0x1e2640fa;

// Show/hide a FEng object the way the card classes do: alpha in FEObjData::Col.a, and bit 0 of the
// object flags (FF_Invisible per cFEngGameInterface::RenderObject 0x00290788). Row groups ship with
// a=0, which makes FEngine::RenderGroup 0x00290120 skip the whole subtree.
void RevealObject(FEPackage *pkg, uint32_t hash, bool show)
{
    FEObject *o = pkg ? pkg->FindObjectByHash(hash) : nullptr;
    if (!o || !o->GetObjData())
        return;
    FEObjData *d = o->GetObjData();
    d->Col.a = show ? 255 : 0;
    if (show)
        d->Col.r = d->Col.g = d->Col.b = 255; // the screen ships its frames tinted; draw them plain
    if (show)
        o->Flags &= ~(1u | 0x04000000u); // visible: clear FF_Invisible and the list's hidden bit
    else
        o->Flags |= 0x04000000u;
    o->Flags |= 0x400000u; // FF_DirtyCode
    // The rows' children carry FF_Invisible too; a group with a!=0 draws them, so clear it there.
    if (show && o->Type == FE_Group)
        for (FEObject *c = static_cast<FEGroup *>(o)->GetFirstChild(); c; c = c->GetNext())
            if (c->GetObjData()) {
                if (c->GetObjData()->Col.a == 0)
                    c->GetObjData()->Col.a = 255;
                c->Flags &= ~1u;
                c->Flags |= 0x400000u;
            }
}

// MC_List.fng's groups ship with the authoring tool's placeholder tint (which EA's class overwrites
// before the dialog is shown); the port would otherwise draw the whole dialog through a red cast.
// Plain white on the groups keeps the package's own leaf colours (the title stays its language colour).
void WhitenGroups(FEObject *obj)
{
    for (; obj; obj = obj->GetNext()) {
        if (obj->GetObjData() && obj->Type == FE_Group) {
            obj->GetObjData()->Col.r = obj->GetObjData()->Col.g = obj->GetObjData()->Col.b = 255;
            obj->Flags |= 0x400000u;
        }
        if (obj->Type == FE_Group)
            WhitenGroups(static_cast<FEGroup *>(obj)->GetFirstChild());
    }
}
// The screen the player is looking at is the last package in the list: FEPackageList keeps its
// packages ordered by priority (AddPackage) and a pushed screen gets priority 1 over the screen it
// covers - 0x00225fb8 opens MC_List.fng that way (FUN_002925b0 = the engine's push). The port's
// GetFirstPackage() is the bottom of that stack, so the card flow asks for the top instead.
FEPackage *TopScreen(FEngine *engine)
{
    FEPackage *top = nullptr;
    for (FEPackage *pkg = engine->GetPackageList()->GetFirstPackage(); pkg; pkg = pkg->GetNext())
        top = pkg;
    return top;
}

// Object/script hashes used by SplashScreen (0x0023c2d8, 0x0023c4a0):
constexpr uint32_t kHashESRBIcon = 0x43d41f73;       // bStringHash("ESRBicon")
constexpr uint32_t kHashStartClick = 0xc4df3ff2;     // bStringHash("start_click")
constexpr uint32_t kHashStartClickAlt = 0x13cf446d;  // bStringHash("startclick")
constexpr uint32_t kHashMouseClick = 0x8c0bd743;     // bStringHash("mouseclick")
constexpr uint32_t kHashLicenseBlurb = 0x4b98c4b9;   // bStringHash("LicenseBlurb")
constexpr uint32_t kHashPressStartStr = 0xeeeb248e;  // "Press START button" in ENGLISH.BIN
constexpr uint32_t kHashSplashGroup = 0x02ddf58c;    // bStringHash("Group")
constexpr uint32_t kHashPromptGroup = 0x4c46f86a;    // prompt container group in UG_LS_Splash.fng
constexpr uint32_t kHashFadeIn = 0x5b0d9106;         // bStringHash("FadeIn")

} // namespace

FEStartupSequence::FEStartupSequence(FEngine *engine, FENativeInterface *fe, bool isPal)
    : m_engine(engine), m_fe(fe), m_isPal(isPal)
{
    // 0x0023ae10: iterate the 12-entry region table and append non-empty package names.
    const char *const *table = m_isPal ? kStartupTablePAL : kStartupTableNTSC;
    for (size_t i = 0; i < 12; ++i) {
        if (table[i][0] != '\0')
            m_screens.emplace_back(table[i]);
    }
    if (m_fe)
        m_fe->SetStartupSequence(this);
}

const char *FEStartupSequence::CurrentPackage() const
{
    if (m_current < m_screens.size())
        return m_screens[m_current].c_str();
    return nullptr;
}

void FEStartupSequence::Start()
{
    // 0x00292280: push the first package in the startup list (0x0023b0b8 / 0x0023b0e0).
    if (m_screens.empty() || !m_engine)
        return;
    m_current = 0;
    m_finished = false;
    printf("[Startup] Starting sequence with %s\n", m_screens[0].c_str());
    m_engine->PushPackage(m_screens[0].c_str(), 0, 0xff);
}

void FEStartupSequence::LoadBootProfile()
{
    if (!m_profile.valid)
        LoadCareerProfile();
    // Dev aid: NFSU2_PAINT=<1..60> selects a GLOBALB GLOSS body colour without the paint screen
    // (0 = the port's stock blue).  NFSU2_PAINT_SET=metal|pearl selects that finish instead (#168).
    uint32_t paintSet = PaintCatalog::kGloss;
    if (const char *set = getenv("NFSU2_PAINT_SET")) {
        if (strcasecmp(set, "metal") == 0)
            paintSet = PaintCatalog::kMetal;
        else if (strcasecmp(set, "pearl") == 0)
            paintSet = PaintCatalog::kPearl;
    }
    if (const char *paint = getenv("NFSU2_PAINT")) {
        const int index = atoi(paint);
        if (index >= 0)
            m_fe->SetPreviewPaint(paintSet, uint32_t(index));
    } else if (getenv("NFSU2_PAINT_SET")) {
        m_fe->SetPreviewPaint(paintSet, m_fe->PreviewPaint());
    }
    // Dev aid: NFSU2_RIM_PAINT=<1..60> previews a RIMS colour on the car and race wheels.
    if (const char *rim = getenv("NFSU2_RIM_PAINT")) {
        const int index = atoi(rim);
        if (index >= 0)
            m_fe->SetPreviewRimPaint(PaintCatalog::kRims, uint32_t(index));
    }
    // Dev aid: NFSU2_BRAKE_PAINT=<1..60> previews a CALIPERS colour on the brake calipers.
    if (const char *brake = getenv("NFSU2_BRAKE_PAINT")) {
        const int index = atoi(brake);
        if (index >= 0)
            m_fe->SetPreviewCaliperPaint(PaintCatalog::kBrakes, uint32_t(index));
    }
    // Dev aid: NFSU2_MIRROR_PAINT=<1..60> previews a GLOSS colour on the mirror housings.
    if (const char *mirror = getenv("NFSU2_MIRROR_PAINT")) {
        const int index = atoi(mirror);
        if (index >= 0)
            m_fe->SetPreviewMirrorPaint(PaintCatalog::kGloss, uint32_t(index));
    }
    // Dev aid: NFSU2_SPOILER_PAINT=<1..60> previews a GLOSS colour on the stock spoiler.
    if (const char *spoiler = getenv("NFSU2_SPOILER_PAINT")) {
        const int index = atoi(spoiler);
        if (index >= 0)
            m_fe->SetPreviewSpoilerPaint(PaintCatalog::kGloss, uint32_t(index));
    }
    // Dev aid: NFSU2_ROOF_PAINT=<1..60> previews a GLOSS colour on the installed roof scoop.
    if (const char *roof = getenv("NFSU2_ROOF_PAINT")) {
        const int index = atoi(roof);
        if (index >= 0)
            m_fe->SetPreviewRoofPaint(PaintCatalog::kGloss, uint32_t(index));
    }
    // Dev aid: NFSU2_ENGINE_PAINT=<1..N> previews a HOSES colour on the installed engine accents.
    if (const char *engine = getenv("NFSU2_ENGINE_PAINT")) {
        const int index = atoi(engine);
        if (index >= 0)
            m_fe->SetPreviewEnginePaint(PaintCatalog::kHoses, uint32_t(index));
    }
    // Dev aid: NFSU2_EXHAUST_PAINT=<1..N> previews a MUFFLERS colour on the installed muffler tip.
    if (const char *exhaust = getenv("NFSU2_EXHAUST_PAINT")) {
        const int index = atoi(exhaust);
        if (index >= 0)
            m_fe->SetPreviewExhaustPaint(PaintCatalog::kMufflers, uint32_t(index));
    }
    // Dev aid: NFSU2_AWARD_TEST=<race> awards a career win of that race at boot (use NFSU2_MEMCARD on a copy).
    if (const char *award = getenv("NFSU2_AWARD_TEST")) {
        m_raceMode = 1;
        AwardCareerResult(atoi(award), 1);
    }
    // Dev aid: NFSU2_SIGNED="2:STREETGLOW,3:BRIDE" signs sponsors without playing the events (not saved).
    if (const char *signedList = getenv("NFSU2_SIGNED")) {
        for (const char *c = signedList; *c;) {
            const int area = atoi(c);
            const char *colon = strchr(c, ':');
            if (!colon)
                break;
            const char *end = strchr(colon, ',');
            const std::string name(colon + 1, end ? size_t(end - colon - 1) : strlen(colon + 1));
            if (area >= 0 && area < Career::Profile::kAreas)
                m_profile.signedSponsors[area] = CarMaterial::NameHash(name.c_str());
            if (!end)
                break;
            c = end + 1;
        }
        m_profile.valid = true;
    }
}

void FEStartupSequence::Advance(const char *fromPkgName)
{
    // 0x0023b118: find fromPkgName in the startup list and switch to the next screen.
    if (!fromPkgName || !m_engine || m_screens.empty())
        return;

    size_t idx = m_current;
    for (size_t i = 0; i < m_screens.size(); ++i) {
        if (strcasecmp(m_screens[i].c_str(), fromPkgName) == 0) {
            idx = i;
            break;
        }
    }

    size_t nextIdx = idx + 1;
    if (nextIdx >= m_screens.size()) {
        m_finished = true;
        return;
    }

    // 0x0023b118 lines 37-43: if the next screen is MC_Background.fng and 0x0023d098() == 0,
    // skip MC_Background.fng and go straight to UI_Main.fng.
    if (strcasecmp(m_screens[nextIdx].c_str(), "MC_Background.fng") == 0) {
        printf("[Startup] Skipping %s (stand-in: memory-card check not needed)\n",
               m_screens[nextIdx].c_str());
        ++nextIdx;
    }

    if (nextIdx >= m_screens.size()) {
        m_finished = true;
        return;
    }

    m_current = nextIdx;
    const char *nextPkg = m_screens[m_current].c_str();
    printf("[Startup] %s -> %s\n", fromPkgName, nextPkg);
    m_engine->QueuePackageSwitch(nextPkg, 0xff);
    if (m_current + 1 >= m_screens.size())
        m_finished = true;
}

bool FEStartupSequence::JumpToScreen(const char *fromPkgName, const char *targetPkgName)
{
    // 0x0023b258: rewind the current node pointer to targetPkgName and SwitchPackage.
    if (!fromPkgName || !targetPkgName || !m_engine)
        return false;
    for (size_t i = 0; i < m_screens.size(); ++i) {
        if (strcasecmp(m_screens[i].c_str(), targetPkgName) == 0) {
            m_current = i;
            m_finished = false;
            printf("[Startup] Jump %s -> %s\n", fromPkgName, m_screens[i].c_str());
            m_engine->QueuePackageSwitch(m_screens[i].c_str(), 0xff);
            return true;
        }
    }
    return false;
}

void FEStartupSequence::OnPackageLoaded(FEPackage *pkg)
{
    if (!pkg || !pkg->GetName())
        return;
    const char *name = pkg->GetName();
    m_navPending = false;

    if (strcasecmp(name, "UG_LS_Splash.fng") == 0) {
        HandleSplashLoaded(pkg);
        return;
    }
    // 0x0023b770 (LS_PSAMovie screen): FEngSetMovieName(pkg, 0xf2754103, "PSA"). The subtitle
    // player (0x00296760 / 0x002edc70) is started by FENativeInterface when the movie opens.
    if (strcasecmp(name, "LS_PSAMovie.fng") == 0) {
        m_fe->SetMovieName(pkg, 0xf2754103, "PSA");
        return;
    }
    // 0x0023b598 (UG_LS_IntroFMV screen): FEngSetMovieName(pkg, 0x72cf9f38, "FMVOpening").
    if (strcasecmp(name, "UG_LS_IntroFMV.fng") == 0) {
        m_fe->SetMovieName(pkg, 0x72cf9f38, "FMVOpening");
        return;
    }
    if (strcasecmp(name, "UI_Main.fng") == 0) {
        printf("[Startup] Reached %s (main menu)\n", name);
        m_finished = true;
        m_backStack.clear();
        HandleMainLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_QRModeSelect.fng") == 0) {
        HandleQRModeSelectLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_QRTrackSelect.fng") == 0) {
        HandleQRTrackSelectLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_QRModeOptions.fng") == 0) {
        HandleQRModeOptionsLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_QRCarSelect.fng") == 0) {
        HandleQRCarSelectLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_ChooseCustomizeCategory.fng") == 0) {
        HandleCustomizeCategoryLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_ChoosePerformanceCategory.fng") == 0) {
        HandlePerformanceCategoryLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_ChoosePerformancePackage.fng") == 0) {
        HandlePerformancePackageLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_BuyPerformanceParts.fng") == 0) {
        HandleBuyPartsLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_VirtualKeyboard.fng") == 0) {
        HandleKeyboardLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_PerformanceDyno_MAIN.fng") == 0 ||
        strcasecmp(name, "UI_PerformanceTuning_Master.fng") == 0) {
        HandleCardMenuLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_PerformanceTuning_Sliders.fng") == 0) {
        HandleSlidersLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_PerformanceTuning_NOS.fng") == 0) {
        HandleNosLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_ChoosePaintCategory.fng") == 0) {
        HandlePaintCategoryLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_Paint.fng") == 0) {
        HandlePaintLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_GenericParts_Browser.fng") == 0) {
        HandlePartsBrowserLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_StartCareer.fng") == 0) {
        HandleStartCareerLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_CareerCrib.fng") == 0) {
        HandleCareerCribLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_CareerWorldMap.fng") == 0) {
        HandleCareerWorldMapLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_CareerCarSelect.fng") == 0) {
        HandleCareerCarSelectLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_RewardsSponsor.fng") == 0) {
        HandleSponsorViewLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "Credits.fng") == 0) {
        HandleCreditsLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "MC_Main.fng") == 0 || strcasecmp(name, "MC_List.fng") == 0) {
        HandleMemcardLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_ProfileManager.fng") == 0) {
        HandleProfileManagerLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_OptionsMain.fng") == 0) {
        HandleOptionsMainLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_Options.fng") == 0) {
        HandleOptionsLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_Pause.fng") == 0) {
        HandlePauseLoaded(pkg);
        return;
    }
}

void FEStartupSequence::NotificationMessage(uint32_t msg, FEObject * /*obj*/, uint32_t /*param1*/,
                                            FEPackage *pkg)
{
    if (!pkg || !pkg->GetName() || pkg->GetControlMask() == 0)
        return;
    const char *name = pkg->GetName();
    if (getenv("NFSU2_MSGDEBUG") && msg != kMsgScreenTick)
        fprintf(stderr, "[msg] %s %08x\n", name, msg);

    // A message prompt sits over the screen that opened it: the covered screen must not see the
    // pad while the prompt is up (Tick already consumed the press).  Without this, FEng forwards
    // the same Accept as kMsgButtonPressed and the shop sells twice (#189).
    if (m_dialogPackage && pkg != m_dialogPackage) {
        switch (msg) {
        case kMsgPadUp:
        case kMsgPadDown:
        case kMsgPadLeft:
        case kMsgPadRight:
        case kMsgPadAccept:
        case kMsgPadBack:
        case kMsgPadStart:
        case kMsgButtonPressed:
            return;
        default:
            break;
        }
    }

    // Stand-ins for screens whose subsystems (language select / memory card) are skipped for now:
    if (strcasecmp(name, "LS_LangSelect.fng") == 0) {
        if (msg == kMsgScreenTick) {
            printf("[Startup] %s: stand-in (language select skipped)\n", name);
            Advance(name);
        }
        return;
    }
    if (strcasecmp(name, "MC_Bootup.fng") == 0) {
        // 0x0023b410: starts memory-card check on FEMSG_SCREEN_TICK (0xc98356ba); stand-in advances.
        if (msg == kMsgScreenTick) {
            printf("[Startup] %s: stand-in (memory-card check skipped)\n", name);
            Advance(name);
        }
        return;
    }

    // 0x0023b710 (LS_EAlogo.fng) and 0x0023b908 (LS_THXMovie.fng):
    // advance on movie-done message 0xc3960eb9 (sent by FEngMovieStarter 0x00290cb8).
    if (strcasecmp(name, "LS_EAlogo.fng") == 0 || strcasecmp(name, "LS_THXMovie.fng") == 0) {
        if (msg == kMsgMovieDone) {
            printf("[Startup] %s: movie finished (0x%08x)\n", name, msg);
            Advance(name);
        }
        return;
    }

    // 0x0023b7f8 (LS_PSAMovie.fng) and 0x0023b610 (UG_LS_IntroFMV.fng):
    // advance on movie-done (0xc3960eb9), PAD_ACCEPT (0x406415e3), or PAD_START (0xb5af2461).
    if (strcasecmp(name, "LS_PSAMovie.fng") == 0 || strcasecmp(name, "UG_LS_IntroFMV.fng") == 0) {
        if (msg == kMsgMovieDone || msg == kMsgPadAccept || msg == kMsgPadStart) {
            printf("[Startup] %s: movie finished/skipped (0x%08x)\n", name, msg);
            Advance(name);
        }
        return;
    }

    if (strcasecmp(name, "UG_LS_Splash.fng") == 0) {
        HandleSplashMessage(msg, pkg);
        return;
    }

    if (strcasecmp(name, "UI_Main.fng") == 0) {
        HandleMainMessage(msg, pkg);
        return;
    }
    if (strcasecmp(name, "UI_Pause.fng") == 0) {
        HandlePauseMessage(msg, pkg);
        return;
    }

    if (strcasecmp(name, "UI_QRModeSelect.fng") == 0) {
        HandleQRModeSelectMessage(msg, pkg);
        return;
    }
    if (strcasecmp(name, "UI_QRTrackSelect.fng") == 0) {
        HandleQRTrackSelectMessage(msg, pkg);
        return;
    }
    if (strcasecmp(name, "UI_QRModeOptions.fng") == 0) {
        // Pad messages for this list are taken from the pad in Tick(); ignore FEng's copies.
        if (msg != kMsgPadUp && msg != kMsgPadDown && msg != kMsgPadLeft && msg != kMsgPadRight &&
            msg != kMsgPadAccept && msg != kMsgPadBack)
            HandleQRModeOptionsMessage(msg, pkg);
        return;
    }
    if (strcasecmp(name, "UI_OptionsMain.fng") == 0 && msg != kMsgPadBack) {
        HandleOptionsMainMessage(msg, pkg);
        return;
    }
    if (strcasecmp(name, "UI_ChooseCustomizeCategory.fng") == 0) {
        // Pad messages are taken from the pad in Tick(); keep FEng's copies out of the scroller.
        if (msg != kMsgPadLeft && msg != kMsgPadRight && msg != kMsgPadAccept && msg != kMsgPadBack)
            HandleCustomizeCategoryMessage(msg, pkg);
        return;
    }
    if (strcasecmp(name, "UI_BuyPerformanceParts.fng") == 0 || strcasecmp(name, "UI_VirtualKeyboard.fng") == 0 ||
        strcasecmp(name, "UI_PerformanceTuning_Sliders.fng") == 0 ||
        strcasecmp(name, "UI_PerformanceTuning_NOS.fng") == 0)
        return; // the pad is read in Tick()
    if (strcasecmp(name, "UI_PerformanceDyno_MAIN.fng") == 0 ||
        strcasecmp(name, "UI_PerformanceTuning_Master.fng") == 0) {
        if (msg != kMsgPadLeft && msg != kMsgPadRight && msg != kMsgPadAccept && msg != kMsgPadBack)
            HandleCardMenuMessage(msg, pkg);
        return;
    }
    if (strcasecmp(name, "UI_ChoosePerformancePackage.fng") == 0) {
        if (msg != kMsgPadLeft && msg != kMsgPadRight && msg != kMsgPadAccept && msg != kMsgPadBack)
            HandlePerformancePackageMessage(msg, pkg);
        return;
    }
    if (strcasecmp(name, "UI_ChoosePerformanceCategory.fng") == 0) {
        if (msg != kMsgPadLeft && msg != kMsgPadRight && msg != kMsgPadAccept && msg != kMsgPadBack)
            HandlePerformanceCategoryMessage(msg, pkg);
        return;
    }
    if (strcasecmp(name, "UI_ChoosePaintCategory.fng") == 0) {
        // Pad messages are taken from the pad in Tick(); keep FEng's copies out of the scroller.
        if (msg != kMsgPadLeft && msg != kMsgPadRight && msg != kMsgPadAccept && msg != kMsgPadBack)
            HandlePaintCategoryMessage(msg, pkg);
        return;
    }
    if (strcasecmp(name, "UI_Paint.fng") == 0) {
        // The swatch grid reads the pad in Tick(); Back/Accept arrive there too.
        if (msg != kMsgPadUp && msg != kMsgPadDown && msg != kMsgPadLeft && msg != kMsgPadRight &&
            msg != kMsgPadAccept && msg != kMsgPadBack)
            HandlePaintMessage(msg, pkg);
        return;
    }
    if (strcasecmp(name, "UI_GenericParts_Browser.fng") == 0) {
        // Pad messages are taken from the pad in Tick(); keep FEng's copies out (they would sell
        // the highlighted part a second time, #189).
        if (msg != kMsgPadUp && msg != kMsgPadDown && msg != kMsgPadLeft && msg != kMsgPadRight &&
            msg != kMsgPadAccept && msg != kMsgPadBack && msg != kMsgButtonPressed)
            HandlePartsBrowserMessage(msg, pkg);
        return;
    }
    if (strcasecmp(name, "genericdialog_small.fng") == 0) {
        // The prompt reads the pad in Tick(); only the load notification is needed here.
        if (msg == kMsgInitComplete)
            HandleDialogLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_StartCareer.fng") == 0 && msg != kMsgPadBack) {
        HandleStartCareerMessage(msg, pkg);
        return;
    }
    if (strcasecmp(name, "UI_CareerCrib.fng") == 0) {
        HandleCareerCribMessage(msg, pkg);
        return;
    }
    if (strcasecmp(name, "UI_CareerWorldMap.fng") == 0) {
        HandleCareerWorldMapMessage(msg, pkg);
        return;
    }
    if (strcasecmp(name, "UI_CareerCarSelect.fng") == 0) {
        HandleCareerCarSelectMessage(msg, pkg);
        return;
    }
    if (strcasecmp(name, "UI_RewardsSponsor.fng") == 0) {
        HandleSponsorViewMessage(msg, pkg);
        return;
    }
    if (strcasecmp(name, "MC_Main.fng") == 0 || strcasecmp(name, "MC_List.fng") == 0) {
        // Pad messages for the card screens are taken from the pad in Tick(); ignore FEng's copies.
        if (msg != kMsgPadUp && msg != kMsgPadDown && msg != kMsgPadLeft && msg != kMsgPadRight &&
            msg != kMsgPadAccept && msg != kMsgPadBack)
            HandleMemcardMessage(msg, pkg);
        return;
    }
    if (strcasecmp(name, "UI_ProfileManager.fng") == 0) {
        // The cards read the pad in Tick(); only the header refresh and the scroll animation need
        // the FEng messages.
        if (msg != kMsgPadUp && msg != kMsgPadDown && msg != kMsgPadLeft && msg != kMsgPadRight &&
            msg != kMsgPadAccept && msg != kMsgPadBack && msg != kMsgButtonPressed)
            HandleProfileManagerMessage(msg, pkg);
        return;
    }
    if (strcasecmp(name, "UI_Options.fng") == 0) {
        // Pad messages for this list come from the pad in Tick(); ignore FEng's copies.
        if (msg != kMsgPadUp && msg != kMsgPadDown && msg != kMsgPadLeft && msg != kMsgPadRight &&
            msg != kMsgPadAccept && msg != kMsgPadBack)
            HandleOptionsMessage(msg, pkg);
        return;
    }
    // Other quick race flow screens (contents not ported yet): Accept goes on, Back goes back.
    if (QuickRaceBack(name) && (msg == kMsgPadBack) && !m_navPending) {
        SwitchFlow(name, QuickRaceBack(name));
        return;
    }
    if (QuickRaceNext(name) && (msg == kMsgPadAccept) && !m_navPending) {
        SwitchFlow(name, QuickRaceNext(name));
        return;
    }

    // Screens entered from the main menu: Back returns to the previous screen (the PS2 keeps a
    // screen stack in FEManager, 0x00292158 push / 0x00292560 switch).
    if (msg == kMsgPadBack && !m_backStack.empty() && !m_navPending) {
        std::string prev = m_backStack.back();
        m_backStack.pop_back();
        printf("[Menu] %s -> back to %s\n", name, prev.c_str());
        m_navPending = true;
        m_engine->QueuePackageSwitch(prev.c_str(), 0xff);
        return;
    }
}

void FEStartupSequence::Tick(uint32_t nowMs)
{
    m_lastTickMs = m_nowMs;
    m_nowMs = nowMs;
    if (!m_engine)
        return;
    // 0x0028ea98 (FEPackageManager::Tick): broadcast FEMSG_SCREEN_TICK (0xc98356ba) to active screens.
    for (FEPackage *pkg = m_engine->GetPackageList()->GetFirstPackage(); pkg != nullptr;
         pkg = pkg->GetNext()) {
        NotificationMessage(kMsgScreenTick, nullptr, 0, pkg);
    }

    // Option lists read the pad directly: in our packages the focused row has no FEng responses for
    // Up/Down/Left/Right, so FEng would not forward them. Only new presses, only while the package
    // accepts input (FEng disables input during screen intros).
    const uint32_t pad = m_fe->GetJoyPadMask(0);
    const uint32_t pressed = pad & ~m_lastPad;
    m_lastPad = pad;
    FEPackage *active = m_engine->GetPackageList()->GetFirstPackage();
    FEPackage *top = TopScreen(m_engine);
    // A message prompt sits over the screen that opened it: while it is up, the pad only closes
    // it (#189).  The covered screen stays loaded underneath and keeps its selection.
    if (m_dialogPackage && pressed) {
        if (m_dialogYesNo) {
            if (pressed & (FENativeInterface::kPadLeft | FENativeInterface::kPadRight)) {
                m_dialogYesSel = (pressed & FENativeInterface::kPadLeft) ? 0 : 1;
                ApplyYesNoFocus(m_dialogPackage, m_dialogYesSel);
            } else if (pressed & (FENativeInterface::kPadAccept | FENativeInterface::kPadStart)) {
                auto action = m_dialogYesSel == 0 ? m_dialogYesAction : nullptr;
                CloseMessageDialog();
                if (action)
                    action();
            } else if (pressed & FENativeInterface::kPadBack) {
                CloseMessageDialog();
            }
            return;
        }
        if (pressed & (FENativeInterface::kPadAccept | FENativeInterface::kPadBack |
                       FENativeInterface::kPadStart))
            CloseMessageDialog();
        return;
    }
    if (getenv("NFSU2_MSGDEBUG") && pressed && top && top->GetName() &&
        (strcasecmp(top->GetName(), "MC_Main.fng") == 0 || strcasecmp(top->GetName(), "MC_List.fng") == 0)) {
        fprintf(stderr, "[mc-pad] top=%s input=%d pressed=%02x\n", top->GetName(),
                int(top->IsInputEnabled()), pressed);
    }
    if (pressed && active && active->GetName() && active->IsInputEnabled() &&
        strcasecmp(active->GetName(), "UI_QRCarSelect.fng") == 0) {
        // Stand-in: the real carousel uses UG2 car-select messages (0x36db742/0x36db743).
        if (pressed & FENativeInterface::kPadLeft) {
            m_fe->CycleCarPreview(-1);
            UpdateQRCarSelect(active);
        } else if (pressed & FENativeInterface::kPadRight) {
            m_fe->CycleCarPreview(1);
            UpdateQRCarSelect(active);
        }
        // L1/R1 cycle the filter type (DAT_00566764) -- messages 0x5073ef13 / 0x260113a7 (0x001f50f0).
        else if (pressed & FENativeInterface::kPadLTrigger)
            CycleCarFilter(-1, active);
        else if (pressed & FENativeInterface::kPadRTrigger)
            CycleCarFilter(1, active);
        // Accept (0x001f32f8, quick race mode 2: PAD_ACCEPT -> StartRace 0x0025d4d8). Mode 0 is a
        // car select pushed directly with NFSU2_PACKAGE. Mode 8 is career car select (buy the
        // starter car into the profile before the race starts).
        else if ((pressed & FENativeInterface::kPadAccept) && m_raceMode == 0x20) {
            // Main-menu Customize: 0x002593c8 sets DAT_00506b50 = 0x20 and pushes the car select;
            // its Accept calls FUN_002a1ac0(3,...) to open the car shop's category picker
            // (UI_ChooseCustomizeCategory.fng, 0x002a48f8).  ISSUES #167/#185.  The picker keeps
            // the last chosen card; the first one (Change Body Parts) is the original's default.
            // The original's mode-0x20 car select stands on the profile's car (0x001f44f0 loads
            // 0x4f1744); our roster browsing is only a picker stand-in, so snap the preview back
            // before customizing.
            if (const Career::OwnedCar *car = m_profile.CurrentCar())
                ApplyOwnedCarPreview(*car);
            NavigateTo(active->GetName(), "UI_ChooseCustomizeCategory.fng");
        }
        else if ((pressed & FENativeInterface::kPadAccept) && m_raceMode == 4) {
            // Split-screen: the original's car select Accept starts a two-player race (0x001f32f8
            // dispatches on 0x506b50).  The port renders one viewport and has one player, so the
            // press would be a silent dead end; say so with the #189 prompt (ISSUES #194).  Back
            // still returns to the mode select.
            printf("[Menu] Split-screen is not available in the native build\n");
            ShowMessageDialogText("Split-screen is not available in the native build");
        }
        else if ((pressed & FENativeInterface::kPadAccept) &&
                 (m_raceMode == 2 || m_raceMode == 0 || m_raceMode == 8 || m_raceMode == 1)) {
            if (m_raceMode == 8) {
                RecordCareerCar();
                // Career opens in the city: free roam, no track/route (as the Resume path).
                m_raceType = 9;
                m_raceNumber = 0;
            }
            if (m_raceMode == 1 || m_raceMode == 8)
                m_cribRequested = true; // 0x00265970: the career opens in the crib
            RequestRace();
        }
        else if (pressed & FENativeInterface::kPadBack) {
            if (m_raceMode == 1 || m_raceMode == 8)
                SwitchFlow(active->GetName(), "UI_StartCareer.fng");
            else
                SwitchFlow(active->GetName(), QuickRaceBack(active->GetName()));
        }
        return;
    }
    if (pressed && active && active->GetName() && active->IsInputEnabled() &&
        strcasecmp(active->GetName(), "UI_ChooseCustomizeCategory.fng") == 0) {
        if (pressed & FENativeInterface::kPadLeft)
            HandleCustomizeCategoryMessage(kMsgPadLeft, active);
        else if (pressed & FENativeInterface::kPadRight)
            HandleCustomizeCategoryMessage(kMsgPadRight, active);
        else if (pressed & FENativeInterface::kPadAccept)
            HandleCustomizeCategoryMessage(kMsgPadAccept, active);
        else if (pressed & FENativeInterface::kPadBack)
            HandleCustomizeCategoryMessage(kMsgPadBack, active);
        return;
    }
    if (pressed && top && top->GetName() && top->IsInputEnabled() &&
        (strcasecmp(top->GetName(), "UI_PerformanceDyno_MAIN.fng") == 0 ||
         strcasecmp(top->GetName(), "UI_PerformanceTuning_Master.fng") == 0)) {
        if (pressed & FENativeInterface::kPadLeft)
            HandleCardMenuMessage(kMsgPadLeft, top);
        else if (pressed & FENativeInterface::kPadRight)
            HandleCardMenuMessage(kMsgPadRight, top);
        else if (pressed & FENativeInterface::kPadAccept)
            HandleCardMenuMessage(kMsgPadAccept, top);
        else if (pressed & FENativeInterface::kPadBack)
            HandleCardMenuMessage(kMsgPadBack, top);
        return;
    }
    if (pressed && top && top->GetName() && top->IsInputEnabled() &&
        strcasecmp(top->GetName(), "UI_PerformanceTuning_NOS.fng") == 0) {
        if (pressed & FENativeInterface::kPadUp)
            HandleNosMessage(kMsgPadUp, top);
        else if (pressed & FENativeInterface::kPadDown)
            HandleNosMessage(kMsgPadDown, top);
        else if (pressed & FENativeInterface::kPadLeft)
            HandleNosMessage(kMsgPadLeft, top);
        else if (pressed & FENativeInterface::kPadRight)
            HandleNosMessage(kMsgPadRight, top);
        else if (pressed & FENativeInterface::kPadLTrigger)
            HandleNosMessage(kMsgPadLTrigger, top);
        else if (pressed & FENativeInterface::kPadBack)
            HandleNosMessage(kMsgPadBack, top);
        return;
    }
    if (pressed && top && top->GetName() && top->IsInputEnabled() &&
        strcasecmp(top->GetName(), "UI_PerformanceTuning_Sliders.fng") == 0) {
        if (pressed & FENativeInterface::kPadUp)
            HandleSlidersMessage(kMsgPadUp, top);
        else if (pressed & FENativeInterface::kPadDown)
            HandleSlidersMessage(kMsgPadDown, top);
        else if (pressed & FENativeInterface::kPadLeft)
            HandleSlidersMessage(kMsgPadLeft, top);
        else if (pressed & FENativeInterface::kPadRight)
            HandleSlidersMessage(kMsgPadRight, top);
        else if (pressed & FENativeInterface::kPadLTrigger)
            HandleSlidersMessage(kMsgPadLTrigger, top);
        else if (pressed & FENativeInterface::kPadBack)
            HandleSlidersMessage(kMsgPadBack, top);
        return;
    }
    if (pressed && top && top->GetName() && top->IsInputEnabled() &&
        strcasecmp(top->GetName(), "UI_VirtualKeyboard.fng") == 0) {
        if (pressed & FENativeInterface::kPadUp)
            HandleKeyboardMessage(kMsgPadUp, top);
        else if (pressed & FENativeInterface::kPadDown)
            HandleKeyboardMessage(kMsgPadDown, top);
        else if (pressed & FENativeInterface::kPadLeft)
            HandleKeyboardMessage(kMsgPadLeft, top);
        else if (pressed & FENativeInterface::kPadRight)
            HandleKeyboardMessage(kMsgPadRight, top);
        else if (pressed & FENativeInterface::kPadStart)
            KeyboardFinish(top, true);
        else if (pressed & FENativeInterface::kPadAccept)
            HandleKeyboardMessage(kMsgPadAccept, top);
        else if (pressed & FENativeInterface::kPadBack)
            HandleKeyboardMessage(kMsgPadBack, top);
        return;
    }
    if (pressed && active && active->GetName() && active->IsInputEnabled() &&
        strcasecmp(active->GetName(), "UI_BuyPerformanceParts.fng") == 0) {
        if (pressed & FENativeInterface::kPadUp)
            HandleBuyPartsMessage(kMsgPadUp, active);
        else if (pressed & FENativeInterface::kPadDown)
            HandleBuyPartsMessage(kMsgPadDown, active);
        else if (pressed & FENativeInterface::kPadAccept)
            HandleBuyPartsMessage(kMsgPadAccept, active);
        else if (pressed & FENativeInterface::kPadBack)
            HandleBuyPartsMessage(kMsgPadBack, active);
        return;
    }
    if (pressed && active && active->GetName() && active->IsInputEnabled() &&
        strcasecmp(active->GetName(), "UI_ChoosePerformancePackage.fng") == 0) {
        if (pressed & FENativeInterface::kPadLeft)
            HandlePerformancePackageMessage(kMsgPadLeft, active);
        else if (pressed & FENativeInterface::kPadRight)
            HandlePerformancePackageMessage(kMsgPadRight, active);
        else if (pressed & FENativeInterface::kPadAccept)
            HandlePerformancePackageMessage(kMsgPadAccept, active);
        else if (pressed & FENativeInterface::kPadBack)
            HandlePerformancePackageMessage(kMsgPadBack, active);
        return;
    }
    if (pressed && active && active->GetName() && active->IsInputEnabled() &&
        strcasecmp(active->GetName(), "UI_ChoosePerformanceCategory.fng") == 0) {
        if (pressed & FENativeInterface::kPadLeft)
            HandlePerformanceCategoryMessage(kMsgPadLeft, active);
        else if (pressed & FENativeInterface::kPadRight)
            HandlePerformanceCategoryMessage(kMsgPadRight, active);
        else if (pressed & FENativeInterface::kPadAccept)
            HandlePerformanceCategoryMessage(kMsgPadAccept, active);
        else if (pressed & FENativeInterface::kPadBack)
            HandlePerformanceCategoryMessage(kMsgPadBack, active);
        return;
    }
    if (pressed && active && active->GetName() && active->IsInputEnabled() &&
        strcasecmp(active->GetName(), "UI_ChoosePaintCategory.fng") == 0) {
        if (pressed & FENativeInterface::kPadLeft)
            HandlePaintCategoryMessage(kMsgPadLeft, active);
        else if (pressed & FENativeInterface::kPadRight)
            HandlePaintCategoryMessage(kMsgPadRight, active);
        else if (pressed & FENativeInterface::kPadAccept)
            HandlePaintCategoryMessage(kMsgPadAccept, active);
        else if (pressed & FENativeInterface::kPadBack)
            HandlePaintCategoryMessage(kMsgPadBack, active);
        return;
    }
    if (pressed && active && active->GetName() && active->IsInputEnabled() &&
        strcasecmp(active->GetName(), "UI_GenericParts_Browser.fng") == 0) {
        if (pressed & FENativeInterface::kPadLeft)
            HandlePartsBrowserMessage(kMsgPadLeft, active);
        else if (pressed & FENativeInterface::kPadRight)
            HandlePartsBrowserMessage(kMsgPadRight, active);
        else if (pressed & FENativeInterface::kPadUp)
            HandlePartsBrowserMessage(kMsgPadUp, active);
        else if (pressed & FENativeInterface::kPadDown)
            HandlePartsBrowserMessage(kMsgPadDown, active);
        else if (pressed & FENativeInterface::kPadAccept)
            HandlePartsBrowserMessage(kMsgPadAccept, active);
        else if (pressed & FENativeInterface::kPadBack)
            HandlePartsBrowserMessage(kMsgPadBack, active);
        return;
    }
    if (pressed && active && active->GetName() && active->IsInputEnabled() &&
        strcasecmp(active->GetName(), "UI_Paint.fng") == 0) {
        static const struct { uint32_t bit, msg; } kPaintMap[] = {
            {FENativeInterface::kPadUp, kMsgPadUp},
            {FENativeInterface::kPadDown, kMsgPadDown},
            {FENativeInterface::kPadLeft, kMsgPadLeft},
            {FENativeInterface::kPadRight, kMsgPadRight},
            {FENativeInterface::kPadLTrigger, kMsgPadLTrigger},
            {FENativeInterface::kPadRTrigger, kMsgPadRTrigger},
            {FENativeInterface::kPadAccept, kMsgPadAccept},
            {FENativeInterface::kPadBack, kMsgPadBack}};
        for (auto &m : kPaintMap)
            if (pressed & m.bit) {
                HandlePaintMessage(m.msg, active);
                break;
            }
        return;
    }
    if (pressed && active && active->GetName() && active->IsInputEnabled() &&
        strcasecmp(active->GetName(), "UI_StartCareer.fng") == 0) {
        if (pressed & FENativeInterface::kPadLeft)
            HandleStartCareerMessage(kMsgPadLeft, active);
        else if (pressed & FENativeInterface::kPadRight)
            HandleStartCareerMessage(kMsgPadRight, active);
        else if (pressed & FENativeInterface::kPadAccept)
            HandleStartCareerMessage(kMsgPadAccept, active);
        else if (pressed & FENativeInterface::kPadBack)
            HandleStartCareerMessage(kMsgPadBack, active);
        return;
    }
    // UI_ProfileManager.fng (#195): the three cards are plain objects, so the pad is read here.
    if (pressed && top && top->GetName() && top->IsInputEnabled() &&
        strcasecmp(top->GetName(), "UI_ProfileManager.fng") == 0) {
        if (pressed & FENativeInterface::kPadLeft)
            HandleProfileManagerMessage(kMsgPadLeft, top);
        else if (pressed & FENativeInterface::kPadRight)
            HandleProfileManagerMessage(kMsgPadRight, top);
        else if (pressed & FENativeInterface::kPadAccept)
            ProfileManagerAccept();
        else if ((pressed & FENativeInterface::kPadBack) && !m_navPending) {
            m_profileSub = false; // leaving the Create / Delete screen returns to the manager
            PopScreen();
        }
        return;
    }
    // Memory-card screens (MC_Main.fng / MC_List.fng): their rows are plain objects (no FEng
    // responses), so the pad is read directly here, as for UI_StartCareer above. MC_List.fng is
    // pushed over MC_Main.fng, so the screen that has the pad is the top of the package stack.
    if (pressed && top && top->GetName() && top->IsInputEnabled() &&
        (strcasecmp(top->GetName(), "MC_Main.fng") == 0 ||
         strcasecmp(top->GetName(), "MC_List.fng") == 0)) {
        static const struct { uint32_t bit, msg; } kMcMap[] = {
            {FENativeInterface::kPadUp, kMsgPadUp},     {FENativeInterface::kPadDown, kMsgPadDown},
            {FENativeInterface::kPadAccept, kMsgPadAccept}, {FENativeInterface::kPadBack, kMsgPadBack}};
        for (auto &m : kMcMap)
            if (pressed & m.bit) {
                HandleMemcardMessage(m.msg, top);
                break;
            }
        return;
    }
    FEPackage *list = m_widgets.Package();
    if (pressed && list && list->IsInputEnabled() && m_engine->GetPackageList()->GetFirstPackage() == list) {
        // A list belongs to exactly one options screen.  The engine releases and reuses package
        // storage, so a stale m_pkg could otherwise alias the next screen and eat its Left/Right
        // input (the Quick Race mode selector has no OPTION_DATA_3/5 row objects, ISSUES #179).
        const char *name = list->GetName();
        if (!name || (strcasecmp(name, "UI_Options.fng") != 0 && strcasecmp(name, "UI_QRModeOptions.fng") != 0)) {
            m_widgets.Clear();
            return;
        }
        static const struct { uint32_t bit, msg; } kMap[] = {{0x01, kMsgPadUp},    {0x02, kMsgPadDown},
                                                             {0x04, kMsgPadLeft},  {0x08, kMsgPadRight},
                                                             {0x10, kMsgPadAccept}, {0x20, kMsgPadBack}};
        for (auto &m : kMap)
            if (pressed & m.bit) {
                if (strcasecmp(name, "UI_Options.fng") == 0)
                    HandleOptionsMessage(m.msg, list);
                else
                    HandleQRModeOptionsMessage(m.msg, list);
            }
    }
}

void FEStartupSequence::HandleSplashLoaded(FEPackage *pkg)
{
    // Decompiled from SplashScreen::SplashScreen at 0x0023c2d8:
    const char *name = pkg->GetName();
    m_splashStartMs = m_nowMs;
    m_copyrightStartMs = m_nowMs;
    m_splashFadedIn = false;
    m_splashAdvanced = false;

    // If GetVideoMode() == 0 (PAL), hide ESRBicon (0x43d41f73).
    if (m_isPal)
        FENativeInterface::SetVisible(pkg, kHashESRBIcon, false);

    // Show "start_click" (0xc4df3ff2), hide "startclick" (0x13cf446d) and "mouseclick" (0x8c0bd743).
    FENativeInterface::SetVisible(pkg, kHashStartClick, true);
    FENativeInterface::SetVisible(pkg, kHashStartClickAlt, false);
    if (FEObject *mouseClick = pkg->FindObjectByHash(kHashMouseClick)) {
        mouseClick->Flags &= ~0x10000000u; // clear FF_IsButton
        FENativeInterface::SetObjectVisible(mouseClick, false);
    }

    // Hide LicenseBlurb (0x4b98c4b9: placeholder "Licensed by Some Company").
    FENativeInterface::SetVisible(pkg, kHashLicenseBlurb, false);

    // Set "start_click" (0xc4df3ff2) string label hash to 0xeeeb248e ("Press START button").
    FENativeInterface::SetLanguageHash(pkg, kHashStartClick, kHashPressStartStr);

    // If bAllowContinue (0x00566e28) is already set from a previous visit, trigger FadeIn immediately.
    if (m_allowContinue) {
        FENativeInterface::SetScript(pkg, kHashSplashGroup, kHashFadeIn, false);
        FENativeInterface::SetScript(pkg, kHashPromptGroup, kHashFadeIn, false);
        m_splashFadedIn = true;
    }
    printf("[Startup] %s initialized (allowContinue=%d)\n", name, m_allowContinue ? 1 : 0);
}

void FEStartupSequence::HandleSplashMessage(uint32_t msg, FEPackage *pkg)
{
    // Decompiled from SplashScreen::NotificationMessage at 0x0023c4a0:
    const char *name = pkg->GetName();
    if (msg == kMsgInitComplete) {
        // 0x35f8620b (FEHASH_INITCOMPLETE): start the 4-second copyright timer.
        m_copyrightStartMs = m_nowMs;
        return;
    }

    if (msg == kMsgScreenTick) {
        // 0xc98356ba (FEMSG_SCREEN_TICK):
        // After 4.0s, set bAllowContinue = true and run "FadeIn" (0x5b0d9106) on 0x02ddf58c & 0x4c46f86a.
        if (!m_splashFadedIn && (m_nowMs - m_copyrightStartMs >= 4000)) {
            m_allowContinue = true;
            m_splashFadedIn = true;
            FENativeInterface::SetScript(pkg, kHashSplashGroup, kHashFadeIn, false);
            FENativeInterface::SetScript(pkg, kHashPromptGroup, kHashFadeIn, false);
            printf("[Startup] %s: FadeIn triggered after 4.0s\n", name);
        }
        // After 30.0s idle on the title screen, replay UG_LS_IntroFMV.fng (0x0023b258).
        if (!m_splashAdvanced && (m_nowMs - m_splashStartMs > 30000)) {
            m_splashAdvanced = true;
            JumpToScreen(name, "UG_LS_IntroFMV.fng");
        }
        return;
    }

    if (msg == kMsgPadAccept || msg == kMsgPadStart) {
        // 0x406415e3 (PAD_ACCEPT) or 0xb5af2461 (PAD_START): advance to UI_Main.fng (0x0023b118).
        if (!m_splashAdvanced) {
            m_splashAdvanced = true;
            printf("[Startup] %s: button pressed (0x%08x), advancing to main menu\n", name, msg);
            Advance(name);
        }
    }
}

void FEStartupSequence::HandleMainLoaded(FEPackage *pkg)
{
    // UIMain::UIMain (0x001e6b78) / UIMain::AddOptions (0x001e6ea0): 7 options on PAL
    // (Online only when 0x002dbcb8() != 0). Default selection: last button, else Quick Race.
    static const std::vector<FEIconScroller::Def> kMainDefs = {
        {0x03704f3d, 0x5e7e1192}, // Career
        {0x4e6fbb02, 0x24909fd7}, // Quick Race
        {0x883e25d6, 0x6a2b93eb}, // 2 Player Split-Screen
        {0x771849a5, 0x7b63ff85}, // Online
        {0xb0c46023, 0xa53039a3}, // Customize
        {0x6b303856, 0xbcb18f38}, // Profile Manager
        {0x3058fe37, 0xed1f0b2c}, // Options
    };
    m_scroller.Setup(pkg, kMainDefs, m_mainLastSelection);
}

void FEStartupSequence::HandleMainMessage(uint32_t msg, FEPackage *pkg)
{
    if (m_scroller.Empty() || m_scroller.Package() != pkg)
        return;
    // 0x001e6c88: the intro event sends 0x34dc1bcf, which queues 0x251bf83e; then edge fades start.
    if (msg == 0x34dc1bcf || msg == 0x251bf83e) {
        FENativeInterface::SetScript(pkg, 0x47ff4e7c, 0x03d8eabc, true);
        m_scroller.AllowFade();
        return;
    }
    if ((msg == kMsgButtonPressed || msg == kMsgPadAccept) && !m_navPending) {
        // Option actions (slot 5 of each option class, on BUTTON_PRESSED 0x0c407210):
        //   Career 0x00259398 -> push UI_StartCareer.fng; Quick Race 0x002595d0 / 2P 0x00259560 ->
        //   race mode 2 / 4, quick race flow (0x001f5f48) at UI_QRModeSelect.fng; Online 0x00259498;
        //   Customize 0x002593c8 -> mode 0x20, push UI_QRCarSelect.fng; Profile 0x00259368 ->
        //   push UI_ProfileManager.fng; Options 0x00259458 -> mode 0x10, switch to UI_OptionsMain.fng.
        static const struct {
            const char *target;
            int raceMode;
        } kActions[] = {{"UI_StartCareer.fng", 1}, {"UI_QRModeSelect.fng", 2}, {"UI_QRModeSelect.fng", 4},
                        {nullptr, 8},              {"UI_QRCarSelect.fng", 0x20}, {"UI_ProfileManager.fng", 0},
                        {"UI_OptionsMain.fng", 0x10}};
        size_t sel = m_scroller.Selected();
        m_mainLastSelection = sel;
        if (sel >= 7 || !kActions[sel].target) {
            printf("[Menu] Online is not available in the native build\n");
            return;
        }
        if (kActions[sel].raceMode)
            m_raceMode = kActions[sel].raceMode;
        NavigateTo(pkg->GetName(), kActions[sel].target);
        return;
    }
    m_scroller.HandleMessage(msg, TickSeconds());
}

// ---------------------------------------------------------------------------------------------
// Quick race flow (0x001f5f48 / 0x001f6048): nodes with next/back functions
//   UI_QRModeSelect   next 0x001f6540: TrackSelect (Free Run type 9: CarSelect)   back: UI_Main
//   UI_QRTrackSelect  next 0x001f6680: ModeOptions                                back: ModeSelect
//   UI_QRModeOptions  next 0x001f6648: CarSelect                                  back: ModeSelect
// (The online flow uses UI_OLMain / UI_OLGameRoom instead; not ported.)
const char *FEStartupSequence::QuickRaceNext(const char *screen) const
{
    if (strcasecmp(screen, "UI_QRModeSelect.fng") == 0)
        return m_raceType == 9 ? "UI_QRCarSelect.fng" : "UI_QRTrackSelect.fng";
    if (strcasecmp(screen, "UI_QRTrackSelect.fng") == 0)
        return "UI_QRModeOptions.fng";
    if (strcasecmp(screen, "UI_QRModeOptions.fng") == 0)
        return "UI_QRCarSelect.fng";
    return nullptr;
}

const char *FEStartupSequence::QuickRaceBack(const char *screen) const
{
    if (strcasecmp(screen, "UI_QRModeSelect.fng") == 0)
        return "UI_Main.fng";
    if (strcasecmp(screen, "UI_QRTrackSelect.fng") == 0 || strcasecmp(screen, "UI_QRModeOptions.fng") == 0)
        return "UI_QRModeSelect.fng";
    if (strcasecmp(screen, "UI_QRCarSelect.fng") == 0) {
        if (m_raceMode == 1 || m_raceMode == 8)
            return "UI_StartCareer.fng";
        if (m_raceMode == 0x20)
            return "UI_Main.fng";
        return m_raceType == 9 ? "UI_QRModeSelect.fng" : "UI_QRModeOptions.fng";
    }
    return nullptr;
}

// UIQRModeSelect::UIQRModeSelect (0x001f6838) + AddOptions (0x001f6b50).
void FEStartupSequence::HandleQRModeSelectLoaded(FEPackage *pkg)
{
    struct ModeDef {
        FEIconScroller::Def def;
        int raceType;
    };
    std::vector<ModeDef> modes = {
        {{0xe9638d3e, 0x93fdee74}, 0},  // Circuit
        {{0x2521e5eb, 0xdcb9f861}, 1},  // Sprint
        {{0xaaab31e9, 0x35102edf}, 2},  // Drag
        {{0x00119144, 0xd7162cfa}, 3},  // Drift
        {{0x91703aef, 0x30da61e1}, 10}, // Street X
        {{0x3f6495c5, 0xc786c4e7}, 4},  // Underground Racing League
    };
    if (m_raceMode == 4)
        modes.push_back({{0x086c74cb, 0x613d3106}, 8}); // Outrun (split screen)
    else
        modes.push_back({{0x713d99c1, 0x6f2421b7}, 9}); // Free Run (offline quick race)
    std::vector<FEIconScroller::Def> defs;
    m_modeTypes.clear();
    for (const ModeDef &m : modes) {
        defs.push_back(m.def);
        m_modeTypes.push_back(m.raceType);
    }
    m_scroller.Setup(pkg, defs, m_modeLastSelection);
    // Screen title (0x42adb44c): "Quick Race Mode Select" / "Split-Screen Mode Select".
    FENativeInterface::SetLanguageHash(pkg, 0x42adb44c, m_raceMode == 4 ? 0xef9f780d : 0x05416105);
}

void FEStartupSequence::HandleQRModeSelectMessage(uint32_t msg, FEPackage *pkg)
{
    if (m_scroller.Package() != pkg)
        return;
    if ((msg == kMsgButtonPressed || msg == kMsgPadAccept) && !m_navPending) {
        // Mode option actions (0x002585xx-0x002586e8) set the race type (DAT_004f13b0); the screen
        // then advances through the flow (0x001f69d0 -> 0x001f6350) once its exit animation ends.
        size_t sel = m_scroller.Selected();
        m_modeLastSelection = sel;
        m_raceType = sel < m_modeTypes.size() ? m_modeTypes[sel] : 0;
        printf("[Menu] race type %d\n", m_raceType);
        SwitchFlow(pkg->GetName(), QuickRaceNext(pkg->GetName()));
        return;
    }
    if (msg == kMsgPadBack && !m_navPending) {
        SwitchFlow(pkg->GetName(), QuickRaceBack(pkg->GetName())); // 0x001f6428
        return;
    }
    m_scroller.HandleMessage(msg, TickSeconds());
}

// UIQRTrackSelect (ctor 0x001fa428): track list from the race database (0x001faa38), filtered for
// the race type by 0x001fadc0, icon per track category (0x001f9d28), name TRACK_NAME_%d
// (0x00275640), length in the info panel (0x001faf90).
static bool TrackOffered(const RaceInfo &r, int raceType)
{
    if (r.special)
        return false;
    const uint32_t f = r.typeFlags;
    switch (raceType) {
    case 0: return f & 1;           // circuit
    case 1: return (f >> 2) & 1;    // sprint
    case 2: return f & 2;           // drag
    case 3: return f & 8;           // drift
    case 4: return (f >> 13) & 1;   // underground racing league
    case 5: return (f >> 12) & 1;
    case 8: case 9: return (f >> 4) & 1;
    case 10: return (f >> 11) & 1;  // street x
    case 12: return (f & 5) != 0;
    default: return false;
    }
}

static uint32_t TrackIcon(const RaceInfo &r, int raceType)
{
    switch (r.category) {
    case 0: return 0xb5a633a0;
    case 1: return 0x79cead40;
    case 2: return 0x4b938e3c;
    case 3: return 0x4fde6feb;
    case 4: return 0x6ba31820;
    case 5:
        if (raceType == 2) return bStringHash("TRACK_ICON_DRAG");
        if (raceType == 3) return bStringHash("TRACK_ICON_DRIFT");
        if (raceType == 4) return bStringHash("TRACK_ICON_U_RACING_LEAGUE");
        return 0x74ce8c0b;
    default: return 0x74ce8c0b;
    }
}

void FEStartupSequence::HandleQRTrackSelectLoaded(FEPackage *pkg)
{
    // 0x00271a90(flags = 1, race, 0, 0) -> 0x00271018 -> 0x0026b438: a track is playable when a
    // 0x34a11 event that runs it marks the slot as a starter (flag bit 1) or is complete in the
    // profile; iGpffffdf68 (NFSU2_UNLOCK_ALL) unlocks everything. Locked tracks stay in the list and
    // show the padlock icon (0x001f9d28 returns 0xf05704ac when the item's +8 is set, 0x001facc0).
    constexpr uint32_t kLockIcon = 0xf05704ac;
    const bool unlockAll = getenv("NFSU2_UNLOCK_ALL") != nullptr;
    const Career::EventTable &events = Career::EventTable::Get();
    std::vector<FEIconScroller::Def> defs;
    m_tracks.clear();
    m_trackLocked.clear();
    size_t lockedCount = 0;
    for (const RaceInfo &r : m_fe->Races()) {
        if (!TrackOffered(r, m_raceType))
            continue;
        const bool locked = !unlockAll && !events.TrackUnlocked(r.number, m_profile.completedEvents);
        char label[32];
        snprintf(label, sizeof(label), "TRACK_NAME_%d", r.number);
        defs.push_back({locked ? kLockIcon : TrackIcon(r, m_raceType), bStringHash(label)});
        m_tracks.push_back(r);
        m_trackLocked.push_back(locked);
        lockedCount += locked;
    }
    size_t slots = 0;
    for (FEObject *o = pkg->GetFirstObject(); o; o = o->GetNext())
        slots += o->NameHash == bStringHash("TRACK_ICON");
    printf("[Menu] track select: %zu tracks for race type %d (%zu TRACK_ICON objects, %zu locked)\n",
           m_tracks.size(), m_raceType, slots, lockedCount);
    m_scroller.Setup(pkg, defs, 0, "ICON_MASTER", "TRACK_ICON"); // 0x0029af60 args
    // Title per mode (0x42adb44c): quick race / split screen track select.
    FENativeInterface::SetLanguageHash(pkg, 0x42adb44c, m_raceMode == 4 ? 0x1bf733bd : 0xe5d63bb5);
    RefreshTrackInfo(pkg);
}

// 0x002ef778 / 0x002ef708 format a 0x4fb4ac value: seconds = value / 4000, hundredths =
// (value % 4000) / 4 (rounded). The English (language 0) templates selected by 0x002ef2b0 are
// "00h:00:00" above an hour, "00:00.00" above a minute, "00.00" above a second and "0.00" below
// (0x47cd70 / 0x47cdf0 / 0x568420 / 0x568440). The port stores milliseconds, so ms/10 is hundredths.
static void FormatBestTime(int millis, char *out, size_t n)
{
    // 0x00272e18 returns 0x7fffffff when no entry exists; 0x002ef778 passes that (and 0) to
    // 0x002ef2b0 as all-zero fields, which the English template "0.--" overwrites to "0.00".
    if (millis <= 0) {
        snprintf(out, n, "0.00");
        return;
    }
    // 0x002ef230(param_4 = 0) space-pads a leading zero: the hours/minutes/seconds field that starts
    // the string is written with "%2d", while the following fields use param_4 = 1 ("%02d"). The
    // helper emits exactly two digits, so hours are taken modulo 100.
    if (millis >= 3600000)
        snprintf(out, n, "%2dh:%02d:%02d", millis / 3600000 % 100, millis / 60000 % 60, millis / 1000 % 60);
    else if (millis >= 60000)
        snprintf(out, n, "%2d:%02d.%02d", millis / 60000, millis / 1000 % 60, millis / 10 % 100);
    else if (millis >= 1000)
        snprintf(out, n, "%2d.%02d", millis / 1000, millis / 10 % 100);
    else
        snprintf(out, n, "0.%02d", millis / 10 % 100);
}

void FEStartupSequence::RefreshTrackInfo(FEPackage *pkg)
{
    if (m_tracks.empty())
        return;
    const RaceInfo &r = m_tracks[m_scroller.Selected()];
    // QR list items are built with direction 0 (0x001facc0 stores 0 at item +0x10), so the header
    // always uses TRACK_NAME_%d here; REVERSE_TRACK_NAME_%d (0x00275640) belongs to reverse event
    // entries in other lists. Track Direction is chosen on the options screen and applied at race start.
    char label[40];
    snprintf(label, sizeof(label), "TRACK_NAME_%d", r.number);
    FENativeInterface::SetLanguageHash(pkg, 0xf601f2d4, bStringHash(label)); // 0x001faf90 header
    FENativeInterface::SetVisible(pkg, 0x53c956c4, false);                    // debug track number
    // 0x001fb388: race type text in 0x15e1351b.
    uint32_t typeLabel = 0x93fdee74; // Circuit (default)
    switch (m_raceType) {
    case 1: typeLabel = 0xdcb9f861; break;  // Sprint
    case 2: typeLabel = 0x35102edf; break;  // Drag
    case 3: typeLabel = 0xd7162cfa; break;  // Drift
    case 4: typeLabel = 0x75f903f4; break;  // Underground Racing League
    case 9: typeLabel = 0x6f2421b7; break;  // Free Run
    case 10: typeLabel = 0x30da61e1; break; // Street X
    }
    FENativeInterface::SetLanguageHash(pkg, 0x15e1351b, typeLabel);
    // Length: metres * 0.001 (km), * 0.62137 for miles (units setting 0x004f12e8 = 0 -> "mi").
    char text[32];
    snprintf(text, sizeof(text), "%.2f mi", r.length * 0.001f * 0.62137f);
    FENativeInterface::SetString(pkg, 0x080c9daa, text);
    // Best Lap (0x0555eeca): 0x001faf90 line 114 calls 0x00272e18(0x4fb4ac, race, item+0x10).
    // QR list items store 0 at +0x10 (0x001facc0), so the forward record is shown even when the
    // Track Direction setting is Reverse. 0x002ef778 formats the value as mm:ss:cc.
    const int best = m_records.BestTimeFor(r.number, 0);
    // 0x001faf90 also sets the caption at 0x36227ee9: drift reads the score table (0x00272ee8)
    // under "Best Lap" (0x3b7b9567), sprint/drag say "Best Time:", everything else "Best Lap:".
    uint32_t bestLabel = 0x3b4a1ecb; // Best Lap:
    if (m_raceType == 3)
        bestLabel = 0x3b7b9567; // Best Lap (drift score)
    else if (m_raceType == 1 || m_raceType == 2)
        bestLabel = 0xdb0c5efd; // Best Time:
    FENativeInterface::SetLanguageHash(pkg, 0x36227ee9, bestLabel);
    if (m_raceType == 3) {
        const int score = m_records.BestScoreFor(r.number, 0);
        snprintf(text, sizeof(text), "%d", score);
    } else {
        FormatBestTime(best, text, sizeof(text));
    }
    FENativeInterface::SetString(pkg, 0x0555eeca, text);
    printf("[Menu] track %d best %s\n", r.number, text);
}

void FEStartupSequence::HandleQRTrackSelectMessage(uint32_t msg, FEPackage *pkg)
{
    if (m_scroller.Package() != pkg)
        return;
    if ((msg == kMsgButtonPressed || msg == kMsgPadAccept) && !m_navPending) {
        const size_t sel = m_scroller.Selected();
        // 0x001fa6e8: Accept returns without starting when the highlighted item's +8 is set (locked
        // track, #157); the padlock icon is the only feedback in the original too.
        if (sel < m_trackLocked.size() && m_trackLocked[sel]) {
            printf("[Menu] track %d is locked\n", m_tracks[sel].number);
            return;
        }
        if (!m_tracks.empty()) {
            m_raceNumber = m_tracks[sel].number; // 0x001fa998: race settings
            printf("[Menu] track %d\n", m_raceNumber);
        }
        SwitchFlow(pkg->GetName(), QuickRaceNext(pkg->GetName()));
        return;
    }
    if (msg == kMsgPadBack && !m_navPending) {
        SwitchFlow(pkg->GetName(), QuickRaceBack(pkg->GetName()));
        return;
    }
    m_scroller.HandleMessage(msg, TickSeconds());
    if (msg == kMsgPadLeft || msg == kMsgPadRight)
        RefreshTrackInfo(pkg);
}

// UIQRModeOptions (ctor 0x001f6eb8, options 0x001f7930 per race type; circuit 0x001f7a80).
// Widgets (vtable slot 5 Act / slot 9 Draw): Track Direction 0x0046f380, Lap KO 0x0046f410,
// Laps 0x0046f530, Traffic 0x0046f6e0, Opponents 0x0046f890, Difficulty 0x0046f800,
// Catch Up 0x0046f770. Settings are per race type (0x004f13b0 + 0x20 + type * 0x20); defaults
// as nfsmw RaceSettings::Default (3 opponents, medium, traffic 1, catch up on, 2 laps).
void FEStartupSequence::HandleQRModeOptionsLoaded(FEPackage *pkg)
{
    RaceOptions &o = m_options[m_raceType & 15];
    auto onOff = [](bool v) { return v ? 0x0000063cu : 0x0000ccfau; }; // "On" / "Off"
    std::vector<FEWidgetMenu::Widget> w;
    // Track Direction (0x0046f380, tracked at 0x00256b10): only races whose record byte +0x86 is set
    // offer Reverse. 0x001f7a80 adds it before every other row. Values: 0x9e4c4648 "Forward" /
    // 0x22f254af "Reverse"; REA/Ghidra shows 0x00256b10 toggles on __PAD_LEFT__ (0x9120409e) and
    // __PAD_RIGHT__ (0xb5971bf1): any nonzero setting is reset to 0, otherwise +4 becomes 1. It also
    // writes the active direction global 0x0053e404 immediately; 0x0025c7b0 copies settings +4 again
    // at race start. (Modes 8/0x200 use 0x00506b9c instead of the per-type 0x004f13b0 settings.)
    const RaceInfo *selected = nullptr;
    for (const RaceInfo &r : m_fe->Races())
        if (r.number == m_raceNumber) {
            selected = &r;
            break;
        }
    if (selected && selected->directionOption) {
        w.push_back({0xa88ffeb4, // Track Direction
                     [&o](uint32_t &l, std::string &) { l = o.reverse ? 0x22f254af : 0x9e4c4648; },
                     [&o](int) {
                         o.reverse = !o.reverse;
                         printf("[Menu] track direction %s\n", o.reverse ? "Reverse" : "Forward");
                     }});
    }
    printf("[Menu] race %d: track direction option %s\n", m_raceNumber,
           selected && selected->directionOption ? "offered" : "not offered");
    const bool circuitLike = m_raceType == 0 || m_raceType == 4 || m_raceType == 10;
    if (circuitLike) {
        w.push_back({0x7fd73849, [&o, onOff](uint32_t &l, std::string &) { l = onOff(o.lapKO); }, // Lap KO
                     [&o](int) { o.lapKO = !o.lapKO; }});
        w.push_back({0x48494e83, [&o](uint32_t &, std::string &t) { t = std::to_string(o.laps); }, // Laps 1..10
                     [&o](int d) { o.laps = o.laps + d < 1 ? 10 : o.laps + d > 10 ? 1 : o.laps + d; }});
    }
    w.push_back({0xeb9dfc09, // Traffic Level: None, Minimum, Moderate, Maximum
                 [&o](uint32_t &l, std::string &) {
                     static const uint32_t k[] = {0x896bcb4d, 0xb6bd9c95, 0x907c555e, 0xb672f15a};
                     l = k[o.traffic & 3];
                 },
                 [&o](int d) { o.traffic = (o.traffic + d + 4) % 4; }});
    const int maxOpp = m_raceType == 4 ? 5 : 3; // 0x00257860
    w.push_back({0x3384a679, [&o](uint32_t &, std::string &t) { t = std::to_string(o.opponents); }, // Opponents
                 [&o, maxOpp](int d) { o.opponents = o.opponents + d < 0 ? maxOpp : o.opponents + d > maxOpp ? 0 : o.opponents + d; }});
    w.push_back({0x4d156786, // Difficulty: Easy, Medium, Hard
                 [&o](uint32_t &l, std::string &) {
                     static const uint32_t k[] = {0xdbbb4e8f, 0xcab262de, 0xdbbcf37c};
                     l = k[o.difficulty % 3];
                 },
                 [&o](int d) { o.difficulty = (o.difficulty + d + 3) % 3; }});
    w.push_back({0x8b8e913a, [&o, onOff](uint32_t &l, std::string &) { l = onOff(o.catchUp); }, // Catch Up
                 [&o](int) { o.catchUp = !o.catchUp; }});
    m_widgets.Setup(pkg, std::move(w));
    // Title (0x42adb44c): quick race / split screen mode options.
    FENativeInterface::SetLanguageHash(pkg, 0x42adb44c, m_raceMode == 4 ? 0x7d7a6e0e : 0x47597606);
}

void FEStartupSequence::HandleQRModeOptionsMessage(uint32_t msg, FEPackage *pkg)
{
    if (m_widgets.Package() != pkg)
        return;
    if (msg == kMsgButtonPressed)
        return; // Accept comes from the pad path (Tick)
    if (msg == kMsgInitComplete) {
        m_widgets.OnInitComplete();
        return;
    }
    if ((msg == kMsgButtonPressed || msg == kMsgPadAccept) && !m_navPending) {
        const RaceOptions &o = m_options[m_raceType & 15];
        printf("[Menu] options: laps %d, opponents %d, difficulty %d, traffic %d, lap KO %d, catch up %d, reverse %d\n",
               o.laps, o.opponents, o.difficulty, o.traffic, o.lapKO, o.catchUp, o.reverse);
        SwitchFlow(pkg->GetName(), QuickRaceNext(pkg->GetName()));
        return;
    }
    if (msg == kMsgPadBack && !m_navPending) {
        SwitchFlow(pkg->GetName(), QuickRaceBack(pkg->GetName()));
        return;
    }
    m_widgets.HandleMessage(msg);
}

// UI_QRCarSelect (0x001f2c40 / ctor 0x001f2ce0). The screen's only state we can drive without the
// registry is the L1/R1 "filter type" (DAT_00566764): an ownership category, not a tuner/muscle
// class. FUN_001f44f0 sets the hint object 0xd999b6e7 ("Filter type goes here") to a label from
// DAT_00566764; 0x001f50f0 cycles it. The filter selects registry types (STOCK/CAREER_SLOT/SPONSOR),
// so with no profile save only Stock Cars and All Cars have entries; the whole roster is shown for
// every state here (best-effort, like the unlocked-tracks note in ISSUES #60).
void FEStartupSequence::HandleQRCarSelectLoaded(FEPackage *pkg)
{
    // Title (0x001f44f0 -> FUN_00259f88(pkg, 0x42adb44c, label) by game mode DAT_00506b50): 2 quick race
    // "Quick Race Car Select", 4 split screen, 8 (set by the main menu's Online entry, 0x001e6c88)
    // "Online Car Select", and the career's new-game pick (mode 0x20 from 0x002593c8) and the garage
    // (0x100) "Customize Car Select". The port's m_raceMode 8 is its New Career pick. The labels
    // 0xe1385c1e / 0xa7fa00b6 the old code used are the Help popup's button legend (+0x28 of the screen,
    // 0x001f2ce0), which printed that legend over the screen.
    uint32_t title = 0x5ac1fb59; // Quick Race Car Select
    if (m_raceMode == 4)
        title = 0xf221d7ed; // Split-Screen Car Select
    else if (m_raceMode == 8 || m_raceMode == 1 || m_raceMode == 0x20)
        title = 0x61032ce5; // Customize Car Select
    FENativeInterface::SetLanguageHash(pkg, 0x42adb44c, title);
    FENativeInterface::SetVisible(pkg, 0xf5a9d520, true);
    // Quick Race / split-screen / starter-pick cars wear their stock paint and wheels.  The preview
    // colours are shared with the career, whose save loads them at boot (ISSUES #167/#172), so clear
    // them here unless the NFSU2_PAINT / NFSU2_RIM_PAINT dev aids deliberately override every car.
    if (m_raceMode == 0 || m_raceMode == 2 || m_raceMode == 4 || m_raceMode == 8) {
        if (!getenv("NFSU2_PAINT"))
            m_fe->SetPreviewPaint(0);
        if (!getenv("NFSU2_RIM_PAINT"))
            m_fe->SetPreviewRimPaint(PaintCatalog::kRims, 0);
        if (!getenv("NFSU2_BRAKE_PAINT"))
            m_fe->SetPreviewCaliperPaint(PaintCatalog::kBrakes, 0);
        if (!getenv("NFSU2_MIRROR_PAINT"))
            m_fe->SetPreviewMirrorPaint(PaintCatalog::kGloss, 0);
        if (!getenv("NFSU2_SPOILER_PAINT"))
            m_fe->SetPreviewSpoilerPaint(PaintCatalog::kGloss, 0);
        if (!getenv("NFSU2_ROOF_PAINT"))
            m_fe->SetPreviewRoofPaint(PaintCatalog::kGloss, 0);
        if (!getenv("NFSU2_ENGINE_PAINT"))
            m_fe->SetPreviewEnginePaint(PaintCatalog::kHoses, 0);
        if (!getenv("NFSU2_EXHAUST_PAINT"))
            m_fe->SetPreviewExhaustPaint(PaintCatalog::kMufflers, 0);
        // Fitted body parts are career-only too (#185/#186/#188): a quick-race or starter car is
        // stock, and the career path re-applies the saved set when its own car select loads.
        if (!getenv("NFSU2_PART_SPOILER"))
            m_fe->SetPreviewSpoilerPart(0, false);
        if (!getenv("NFSU2_PART_ROOF"))
            m_fe->SetPreviewRoofPart(0, false, false, false);
        if (!getenv("NFSU2_PART_EXHAUST"))
            m_fe->SetPreviewExhaustPart(0);
        if (!getenv("NFSU2_PART_HOOD"))
            m_fe->SetPreviewHoodPart(0, false);
        if (!getenv("NFSU2_PART_HEADLIGHT"))
            m_fe->SetPreviewHeadlightPart(0);
        if (!getenv("NFSU2_PART_TAILLIGHT"))
            m_fe->SetPreviewTaillightPart(0);
        if (!getenv("NFSU2_PART_FRONT_BUMPER"))
            m_fe->SetPreviewFrontBumperPart(0);
        if (!getenv("NFSU2_PART_REAR_BUMPER"))
            m_fe->SetPreviewRearBumperPart(0);
        if (!getenv("NFSU2_PART_SKIRT"))
            m_fe->SetPreviewSkirtPart(0);
        if (!getenv("NFSU2_PART_WIDEBODY"))
            m_fe->SetPreviewWideBodyPart(0);
        m_fe->SetPreviewPerfParts({}); // a Quick Race car is stock (#200)
        m_fe->SetPreviewTune({});
    }
    m_carFilterState = 1; // DAT_00566764 default: Stock Cars
    ApplyCarFilterLabel(pkg);
    UpdateQRCarSelect(pkg);
}

// 0x001f44f0 / 0x001f4038: Update car-select screen objects for the current vehicle:
// - Manufacturer logo (0x3e01ad1d) set to CARSELECT_MANUFACTURER_<MANUF>
// - Car model badge (0xb05dd708) set to SECONDARY_LOGO_<MODEL>
// - Left / right arrows (0x444969fd / 0x444969fe)
// - Performance stat bars (0x001f4228 / 0x002614e0)
void FEStartupSequence::UpdateQRCarSelect(FEPackage *pkg)
{
    if (!pkg)
        return;
    const std::string &car = m_fe->PreviewCarName();
    std::string manuf = m_fe->CarManufacturer(car);
    std::string upperManuf, upperCar;
    for (char c : manuf)
        upperManuf += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (char c : car)
        upperCar += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    uint32_t manufTex = bStringHash(("CARSELECT_MANUFACTURER_" + upperManuf).c_str());
    uint32_t badgeTex = bStringHash(("SECONDARY_LOGO_" + upperCar).c_str());

    FENativeInterface::SetTextureHash(pkg, 0x3e01ad1d, manufTex); // MANUFACTURERLOGO
    FENativeInterface::SetTextureHash(pkg, 0xb05dd708, badgeTex); // CARBADGE
    FENativeInterface::SetVisible(pkg, 0x3e01ad1d, true);
    FENativeInterface::SetVisible(pkg, 0xb05dd708, true);

    FENativeInterface::SetVisible(pkg, 0x444969fd, true); // ScrollBar_Arrow_1
    FENativeInterface::SetVisible(pkg, 0x444969fe, true); // ScrollBar_Arrow_2

    m_fe->UpdateStatBars(pkg, true);
}

// 0x001f44f0: label hash for the current filter state.
static uint32_t CarFilterLabelHash(int state)
{
    if (state == 3)
        return 0x6b68362c; // All Cars
    uint32_t h = 0xe798a2f7;            // Stock Cars (default)
    if (state & 2)
        h = 0xe7ab43d3;                 // My Cars
    if (state & 4)
        h = 0xb4045365;                 // Career
    if (state & 8)
        h = 0x26fecee7;                 // SPONSOR CARS
    if (state & 0x10)
        h = 0x1a23d5e6;                 // Ranked Cars
    return h;
}

void FEStartupSequence::ApplyCarFilterLabel(FEPackage *pkg)
{
    FENativeInterface::SetLanguageHash(pkg, 0xd999b6e7, CarFilterLabelHash(m_carFilterState));
}

void FEStartupSequence::CycleCarFilter(int direction, FEPackage *pkg)
{
    // FUN_001f50f0 cycles through the filter states an empty garage can reach; the on-screen names
    // (from ENGLISH.BIN) are Stock Cars / My Cars / All Cars / Career / SPONSOR CARS / Ranked Cars.
    static const int kCycle[] = {1, 3, 2, 4, 8, 0x10};
    const int n = static_cast<int>(sizeof(kCycle) / sizeof(kCycle[0]));
    int cur = 0;
    for (int i = 0; i < n; ++i)
        if (kCycle[i] == m_carFilterState) {
            cur = i;
            break;
        }
    m_carFilterState = kCycle[(cur + (direction < 0 ? -1 : 1) + n) % n];
    printf("[CarSelect] filter state %d\n", m_carFilterState);
    ApplyCarFilterLabel(pkg);
}

// UI_ChooseCustomizeCategory.fng (factory 0x002a48f8, AddOptions 0x002a4dc0): the car shop's
// category picker.  The five PAL cards are Change Body Parts, Change Performance Parts, Change
// Paint / Vinyls, Change Interiors / Car Specialties and Performance Tuning / Dyno.  The port
// implements Change Body Parts (the Body Shop, ISSUES #185) and Change Paint / Vinyls (#167); the
// other three are greyed out.  Selecting Paint / Vinyls sets DAT_0051456c = 2 in the original and
// opens UI_ChoosePaintCategory.fng; the port goes straight to UI_Paint.fng because the paint/vinyl
// submenu only chooses between Paint, Vinyls and Decals and none of the latter two exist yet.
// Change Body Parts is the Body Shop (DAT_0051456c = 0, title 0x796dc449) and opens the shared
// UI_GenericParts_Browser.fng.
void FEStartupSequence::HandleCustomizeCategoryLoaded(FEPackage *pkg)
{
    static const struct {
        uint32_t tex, label;
    } kOptions[] = {
        {0xb802f10e, 0xa92b4ae8}, // Change Body Parts
        {0xc56d3032, 0xeca6b0cc}, // Change Performance Parts
        {0xdc99aafe, 0xc6089ca7}, // Change Paint / Vinyls
        {0x3f40e75f, 0xc10d7979}, // Change Interiors / Car Specialties
        {0x7a9b397a, 0x8513f2c0}, // Performance Tuning / Dyno
    };
    std::vector<FEIconScroller::Def> defs;
    for (size_t i = 0; i < sizeof(kOptions) / sizeof(kOptions[0]); ++i)
        defs.push_back({kOptions[i].tex, kOptions[i].label, i != 0 && i != 1 && i != 2 && i != 4});
    printf("[Customize] category picker: %zu options (body parts, paint)\n", defs.size());
    m_scroller.Setup(pkg, defs, m_customizeCategorySel);
}

void FEStartupSequence::HandleCustomizeCategoryMessage(uint32_t msg, FEPackage *pkg)
{
    if (m_scroller.Empty() || m_scroller.Package() != pkg)
        return;
    if (msg == kMsgScreenTick) {
        m_scroller.HandleMessage(msg, TickSeconds());
        return;
    }
    if (msg == kMsgInitComplete) {
        m_scroller.HandleMessage(msg, 0.0f);
        m_scroller.AllowFade();
        return;
    }
    if (msg == kMsgPadLeft || msg == kMsgPadRight) {
        m_scroller.HandleMessage(msg, 0.0f);
        return;
    }
    if (msg == kMsgPadBack && !m_navPending) {
        PopScreen();
        return;
    }
    if ((msg == kMsgButtonPressed || msg == kMsgPadAccept) && !m_navPending) {
        m_customizeCategorySel = m_scroller.Selected();
        if (m_customizeCategorySel == 2) {
            NavigateTo(pkg->GetName(), "UI_ChoosePaintCategory.fng");
            return;
        }
        if (m_customizeCategorySel == 0) {
            NavigateTo(pkg->GetName(), "UI_GenericParts_Browser.fng");
            return;
        }
        if (m_customizeCategorySel == 1) {
            NavigateTo(pkg->GetName(), "UI_ChoosePerformanceCategory.fng");
            return;
        }
        if (m_customizeCategorySel == 4) {
            NavigateTo(pkg->GetName(), "UI_PerformanceDyno_MAIN.fng");
            return;
        }
        printf("[Customize] category %zu is not ported yet\n", m_customizeCategorySel);
    }
}

namespace {
// 0x002b64c8's nine cards, in screen order: the performance category (= the 0x34a15 package
// category and the shop's part id), its icon and its label.  0x002b7e80 reuses the labels as the
// package screen's header.
const struct PerfCategoryCard {
    uint32_t partId, tex, label;
} kPerfCategories[] = {
    {3, 0xc15c94e6, 0x026eea5e}, {7, 0x502abeed, 0xa7884765}, {1, 0x01a29ffa, 0x73ad2f72},
    {5, 0x178475e7, 0x9c34c75f}, {2, 0x9701bde4, 0xa7886f38}, {8, 0x06e8e477, 0xabca6aef},
    {6, 0xbaa23a28, 0xaa8959ad}, {0, 0xea57d2c4, 0x758d5e3c}, {4, 0x06ef789c, 0xabd0ff14},
};

} // namespace

// UI_ChoosePerformanceCategory.fng (factory 0x002b5d70, ctor 0x002b5da8, AddOptions 0x002b64c8):
// "Change Performance Parts".  0x002b64c8 calls 0x002b6358(screen, partId, icon, label) nine
// times, in the order below; each adds a card (vtable 0x477c90, part id at +0x58) greyed when no
// level of that part is unlocked (0x002b57a0 -> 0x002719d8, the career unlock query; all
// unlocked here, as with NFSU2_UNLOCK_ALL).  The package's own title is kept.  The follow-on
// screens (UI_ChoosePerformancePackage / UI_BuyPerformanceParts) are not ported (ISSUES #196).
void FEStartupSequence::HandlePerformanceCategoryLoaded(FEPackage *pkg)
{
    std::vector<FEIconScroller::Def> defs;
    for (const auto &c : kPerfCategories)
        defs.push_back({c.tex, c.label, false});
    printf("[Customize] performance parts: %zu categories\n", defs.size());
    m_scroller.Setup(pkg, defs, m_performanceCategorySel);
}

void FEStartupSequence::HandlePerformanceCategoryMessage(uint32_t msg, FEPackage *pkg)
{
    if (m_scroller.Empty() || m_scroller.Package() != pkg)
        return;
    if (msg == kMsgScreenTick) {
        m_scroller.HandleMessage(msg, TickSeconds());
        return;
    }
    if (msg == kMsgInitComplete) {
        m_scroller.HandleMessage(msg, 0.0f);
        m_scroller.AllowFade();
        return;
    }
    if (msg == kMsgPadLeft || msg == kMsgPadRight) {
        m_scroller.HandleMessage(msg, 0.0f);
        m_performanceCategorySel = m_scroller.Selected();
        return;
    }
    if (msg == kMsgPadBack && !m_navPending) {
        PopScreen();
        return;
    }
    if ((msg == kMsgButtonPressed || msg == kMsgPadAccept) && !m_navPending) {
        m_performanceCategorySel = m_scroller.Selected();
        m_perfCategory = kPerfCategories[m_performanceCategorySel].partId;
        NavigateTo(pkg->GetName(), "UI_ChoosePerformancePackage.fng");
    }
}

// UI_GenericParts_Browser.fng (factory 0x002a50c0, ctor 0x002a50f8, refresh 0x002a6be0): the
// shared parts browser behind the car shop's "Change ... Parts" cards.  0x002a8870 dispatches on
// DAT_0051456c: 0 fills the Body Shop's slot carousel (0x002a7e70), 2 the Graphics Shop's vinyl
// categories (0x002a8430).  The Body Shop order in 0x002a7e70 is Front Bumpers (1), Rear Bumpers
// (2), Side Skirts (0xb), Spoilers (0xc), Hoods (9), Roof Scoops (7), Headlights (0xe), Taillights
// (0xf), Side Mirrors (0x20), Exhaust Tips (0x10) and Wide Body Kits (6), plus Rims (0x002a7860)
// and a Carbon Fiber card (0x002a7938).  The mountable cards are wired up here: their geometry
// (#182/#180/#188/#190/#193) and prices (#184) are decoded; each card lists the catalogue's
// records of its type and finish (ISSUES #185/#186).
//
// The two carousels are IconScroller instances built by 0x0029af60 with the templates
// CATEGORY_MASTER/CATEGORY_ICON and MINI_MASTER/PARTS_ICON.  The selected item's name and price
// are the package's own PART_NAME (0xd28b9316, shipped as "Wolf") and PRICE (0x380b632, "$3000")
// strings, which 0x002a6be0 rewrites through 0x00292d00 with the "%$d" format at 0x5678f8.
// 0x002a6fd0 also calls 0x002b2518 (the car manager) on every list step, i.e. previews the part.
//
// GenericDialog_SMALL.fng object hashes (NFSU2_FETREE dump): the centred message string and the
// three button labels the dialog manager picks between (the Yes/No pair and the single centred
// Yes).  Each label's backing 9-slice is a separate group (the constants below the pair).
constexpr uint32_t kDialogMessage = 0x1e2640fa;
constexpr uint32_t kDialogYesLeft = 0xb8a7c6cc;
constexpr uint32_t kDialogYesCentre = 0xb8a7c6ce;
constexpr uint32_t kDialogNo = 0xb8a7c6cd;
// The labels sit on separate 9-slice button groups: b1000cc6 under the left Yes, b1000cc8 under
// the centred Yes and b1000cc7 under the No.  Hiding only the labels leaves their bars behind.
constexpr uint32_t kDialogYesLeftBar = 0xb1000cc6;
constexpr uint32_t kDialogYesCentreBar = 0xb1000cc8;
constexpr uint32_t kDialogNoBar = 0xb1000cc7;
// ENGLISH.BIN 0xd1ac60be: "You do not have enough bank to purchase this item." - the label
// 0x002a7278 passes to the dialog through 0x0025fc70.
constexpr uint32_t kLabelNotEnoughBank = 0xd1ac60be;

// The career item pool helpers (#187).
namespace {
// A key packs the shop slot id, the style and the finish/body flags;
// the original keeps a real item pool in the profile block, this is the flat stand-in.
uint32_t MakePartKey(uint8_t partId, uint8_t style, bool carbon, uint8_t variant)
{
    return uint32_t(partId) | (uint32_t(style) << 8) | (carbon ? 1u << 24 : 0) |
           (variant == 1 ? 1u << 25 : 0) | (variant == 2 ? 1u << 26 : 0);
}

uint32_t PartsShopKey(const CarPart &part)
{
    return MakePartKey(part.partId, uint8_t(part.style), part.carbon, part.variant);
}

bool HasOwnedPart(const Career::Profile &profile, uint32_t key)
{
    return std::find(profile.ownedParts.begin(), profile.ownedParts.end(), key) !=
           profile.ownedParts.end();
}

void AddOwnedPart(Career::Profile &profile, uint32_t key)
{
    if (key && !HasOwnedPart(profile, key))
        profile.ownedParts.push_back(key);
}

// Old saves (and parts fitted before #187) have no pool entry.  Preserve every part that is on the
// car, so replacing it later does not lose the part or charge for it again.
void RememberFittedParts(Career::Profile &profile, const Career::OwnedCar &car)
{
    if (car.spoiler)
        AddOwnedPart(profile, MakePartKey(0xc, car.spoiler, car.spoilerCarbon, 0));
    if (car.roof)
        AddOwnedPart(profile, MakePartKey(7, car.roof, car.roofCarbon,
                                          car.roofDual ? 2 : car.roofOffset ? 1 : 0));
    if (car.exhaust)
        AddOwnedPart(profile, MakePartKey(0x10, car.exhaust, false, 0));
    if (car.hood)
        AddOwnedPart(profile, MakePartKey(9, car.hood, car.hoodCarbon, 0));
    if (car.headlight)
        AddOwnedPart(profile, MakePartKey(0xe, car.headlight, false, 0));
    if (car.taillight)
        AddOwnedPart(profile, MakePartKey(0xf, car.taillight, false, 0));
    // Kit-number body panels (#190): front/rear bumper and side skirt kit numbers.
    if (car.frontBumper)
        AddOwnedPart(profile, MakePartKey(0x1, car.frontBumper, false, 0));
    if (car.rearBumper)
        AddOwnedPart(profile, MakePartKey(0x2, car.rearBumper, false, 0));
    if (car.skirt)
        AddOwnedPart(profile, MakePartKey(0xb, car.skirt, false, 0));
    if (car.wideBody)
        AddOwnedPart(profile, MakePartKey(6, car.wideBody, false, 0));
}

// Show or hide one control of GenericDialog_SMALL.fng.  The shipped labels start fully transparent;
// the dialog manager fades in the button form it picks, so a shown label also gets its alpha back.
// The focused button's label uses the package's highlight green (the colour its two-button No ships
// with); the one-button prompt's centred Yes is the only button, hence focused.
void ShowDialogControl(FEPackage *pkg, uint32_t hash, bool visible, bool focused = false)
{
    FEObject *obj = pkg ? pkg->FindObjectByHash(hash) : nullptr;
    if (!obj)
        return;
    if (visible) {
        if (FEObjData *data = obj->GetObjData()) {
            data->Col.a = 255;
            if (focused) {
                data->Col.r = 204;
                data->Col.g = 244;
                data->Col.b = 138;
            }
        }
    }
    FENativeInterface::SetObjectVisible(obj, visible);
}
} // namespace

// The Body Shop's slot cards, in the original order (the 0x002a7e70 AddOption calls, then the
// 0x002a7938 Carbon Fiber filter).  partId is the catalogue's shop slot (GLOBALB part id);
// universal parts live in their own pack (SPOILER / ROOF / EXHAUST) while hoods, lights and the
// body panels are records of the car model's own type.  The port ships the subset it can mount;
// the Side Mirrors card (0x20) is paint-only in the catalogue and the Rims card is the wheel shop.
struct PartsCardDef {
    uint32_t icon, label;
    uint8_t partId;
    bool universal;
    bool carbon;
    const char *debug;
};
constexpr PartsCardDef kPartsCards[] = {
    {0x739c4a1b, 0x739c4a1b, 0x1, false, false, "front bumpers"}, // 1 in 0x002a7e70
    {0x015c767c, 0x015c767c, 0x2, false, false, "rear bumpers"},  // 2
    {0x4e6c2af9, 0x4e6c2af9, 0xb, false, false, "side skirts"},   // 0xb
    {0xbb034ea6, 0xbb034ea6, 0xc, true, false, "spoilers"},       // 0xc
    {0x28f7092, 0x28f7092, 0x9, false, false, "hoods"},           // 9
    {0x79165861, 0x79165861, 0x7, true, false, "roof scoops"},    // 7
    {0xf69ac384, 0x3dc6a111, 0xe, false, false, "headlights"},    // 0xe
    {0x2cbb2d1c, 0x663437a9, 0xf, false, false, "taillights"},    // 0xf
    {0xa434a006, 0x2aced725, 0x10, true, false, "exhaust tips"},  // 0x10
    {0x945e9bc5, 0x945e9bc5, 0x6, false, false, "wide body kits"}, // 6
    {0x24911e04, 0x24911e04, 0, false, true, "carbon fiber"},     // 0x002a7938
};
constexpr size_t kPartsCardCount = sizeof(kPartsCards) / sizeof(kPartsCards[0]);
constexpr size_t kPartsCarbonCard = 10;

uint32_t UniversalPartsType(uint8_t partId)
{
    switch (partId) {
    case 0xc: return bStringHash("SPOILER");
    case 7: return bStringHash("ROOF");
    case 0x10: return bStringHash("EXHAUST");
    default: return 0;
    }
}

// Load the shared parts browser: build the slot cards and the item list, then restore the car's
// fitted set on the preview (#185/#186/#190).
void FEStartupSequence::HandlePartsBrowserLoaded(FEPackage *pkg)
{
    m_partsCategorySel = std::min(m_partsCategorySel, kPartsCardCount - 1);
    std::vector<FEIconScroller::Def> defs;
    for (const PartsCardDef &card : kPartsCards)
        defs.push_back({card.icon, card.label, false});
    m_partsCategoryScroller.Setup(pkg, defs, m_partsCategorySel, "CATEGORY_MASTER", "CATEGORY_ICON");
    // Screen title (0x42adb44c): 0x002a6be0 writes 0x796dc449 ("Body Shop") for DAT_0051456c == 0,
    // and the Carbon Fiber card's own title (0x24911e04) while that filter is active.
    FENativeInterface::SetLanguageHash(
        pkg, 0x42adb44c,
        m_partsCategorySel == kPartsCarbonCard ? 0x24911e04u : 0x796dc449u);
    printf("[PartsShop] body shop: %s card\n", kPartsCards[m_partsCategorySel].debug);

    // The item-info group (0x4a1316e9, the parent of PART_NAME and PRICE) ships hidden: the
    // screen's own 0x002a8870 shows it once the item list has a selection (the 0x00259c90 /
    // 0x00259c80 pair on the two scrollbar groups).  The port shows it when the list is populated.
    if (FEObject *info = pkg->FindObjectByHash(0x4a1316e9u)) {
        FENativeInterface::SetObjectVisible(info, true);
        if (FEObjData *data = info->GetObjData())
            data->Col.a = 255;
    }
    if (const Career::OwnedCar *car = ShopCar())
        RememberFittedParts(m_profile, *car);
    RefreshPartsBrowserList(pkg);
}

// Rebuild PARTS_ICON for the selected slot card.  The cards are the Body Shop's part slots; the
// Carbon Fiber card is the original's cross-type filter (0x5e), so it collects the carbon records
// of every body part the port can mount.  Geometry numbers come from the part's model entry
// (#188: _STYLE<NN>, #190: _KIT<NN>), which is authoritative for the universal and per-car parts
// alike.
void FEStartupSequence::RefreshPartsBrowserList(FEPackage *pkg)
{
    static const uint32_t kCarbonTypes[] = {bStringHash("SPOILER"), bStringHash("ROOF"),
                                            bStringHash("EXHAUST")};
    // The hood / headlights / taillights are per-car records: their type is the car model's hash
    // (the GLOBALB car database folder name, e.g. "SKYLINE"), unlike the universal spoiler/roof/
    // exhaust packs.  The previewed car is the one whose panels are shown, so prefer its name.
    const std::string carModel =
        !m_fe->PreviewCarName().empty() ? m_fe->PreviewCarName() : (ShopCar() ? ShopCar()->model : "");
    const uint32_t carType = carModel.empty() ? 0 : bStringHash(carModel.c_str());

    m_partsShopItems.clear();
    const PartsCardDef &card = kPartsCards[m_partsCategorySel];
    if (card.carbon) {
        for (uint32_t type : kCarbonTypes) {
            const std::vector<CarPart> &parts = m_fe->Parts().Parts(type);
            for (size_t i = 0; i < parts.size(); ++i)
                if (parts[i].carbon && parts[i].level > 0)
                    m_partsShopItems.push_back({type, uint16_t(i), 0x24911e04});
        }
        // Carbon hoods live under the car's own type (#188); the card collects them too.
        if (carType) {
            const std::vector<CarPart> &parts = m_fe->Parts().Parts(carType);
            for (size_t i = 0; i < parts.size(); ++i)
                if (parts[i].carbon && parts[i].level > 0 && parts[i].partId == 9 &&
                    m_fe->PreviewHasBodyStyle(9, parts[i].style))
                    m_partsShopItems.push_back({carType, uint16_t(i), 0x24911e04});
        }
    } else {
        // Universal packs hold only their own slot (part id 0xc/7/0x10); the body panels are
        // records of the previewed car's type, filtered by part id.  A car ships only some of the
        // catalogue designs (#192: 25 of 30 kits on the SKYLINE, 10 of 14 Corolla lights), so hide
        // the styles its GEOMETRY.BIN has no solid for instead of selling a no-op panel.
        const uint32_t type = card.universal ? UniversalPartsType(card.partId) : carType;
        const std::vector<CarPart> &parts = m_fe->Parts().Parts(type);
        for (size_t i = 0; i < parts.size(); ++i)
            if (!parts[i].carbon && parts[i].level > 0 && parts[i].partId == card.partId &&
                (card.universal || m_fe->PreviewHasBodyStyle(card.partId, parts[i].style)))
                m_partsShopItems.push_back({type, uint16_t(i), card.icon});
    }

    // The original selects the fitted part when the window opens (0x002a8870 -> 0x0029b850);
    // afterwards the player's position is kept.
    m_partsSel = 0;
    if (const Career::OwnedCar *car = ShopCar()) {
        for (size_t i = 0; i < m_partsShopItems.size(); ++i) {
            const CarPart &candidate =
                m_fe->Parts().Parts(m_partsShopItems[i].type)[m_partsShopItems[i].index];
            if (OwnedCarHasPart(*car, candidate)) {
                m_partsSel = i;
                break;
            }
        }
    }

    // The package ships 132 texture-less PARTS_ICON slots: the original fills them with thumbnails
    // it renders offscreen, which the port does not do yet.  Give every slot its slot card's icon
    // so the list stays visible (ISSUES #185).
    std::vector<FEIconScroller::Def> defs;
    for (const PartsShopItem &item : m_partsShopItems)
        defs.push_back({item.icon, 0, false});
    // The item list is the horizontal MINI_MASTER/PARTS_ICON row above the bottom bar, like the
    // other carousels; 0x002a50f8 gives it 20 units of spacing (the slot cards use 40).
    m_partsScroller.Setup(pkg, defs, m_partsSel, "MINI_MASTER", "PARTS_ICON", 20.0f);
    const char *first = "-";
    if (!m_partsShopItems.empty())
        first = m_fe->Parts().Parts(m_partsShopItems.front().type)[m_partsShopItems.front().index].name.c_str();
    printf("[PartsShop] %s: %zu records, first %s, selected %zu\n", card.debug,
           m_partsShopItems.size(), first, m_partsSel);
    PreviewOwnedParts(ShopCar());
    RefreshPartsBrowserInfo(pkg);
}

const CarPart *FEStartupSequence::SelectedPartsShopPart() const
{
    if (m_partsShopItems.empty())
        return nullptr;
    const PartsShopItem &item = m_partsShopItems[std::min(m_partsSel, m_partsShopItems.size() - 1)];
    return &m_fe->Parts().Parts(item.type)[item.index];
}

bool FEStartupSequence::OwnedCarHasPart(const Career::OwnedCar &car, const CarPart &part)
{
    switch (part.partId) {
    case 0xc: // spoiler
        return car.spoiler == part.style && car.spoilerCarbon == part.carbon;
    case 7: // roof scoop
        return car.roof == part.style && car.roofCarbon == part.carbon &&
               car.roofDual == (part.variant == 2) && car.roofOffset == (part.variant == 1);
    case 0x10: // exhaust tip
        return car.exhaust == part.style;
    case 9: // hood
        return car.hood == part.style && car.hoodCarbon == part.carbon;
    case 0xe: // headlights
        return car.headlight == part.style;
    case 0xf: // taillights
        return car.taillight == part.style;
    case 0x1: // front bumper (kit number, #190)
        return car.frontBumper == part.style;
    case 0x2: // rear bumper
        return car.rearBumper == part.style;
    case 0xb: // side skirt
        return car.skirt == part.style;
    case 6: // wide-body kit (#193)
        return car.wideBody == part.style;
    default:
        return false;
    }
}

void FEStartupSequence::PreviewPartsBrowserPart(const CarPart &part)
{
    switch (part.partId) {
    case 0xc:
        m_fe->SetPreviewSpoilerPart(part.style, part.carbon);
        break;
    case 7:
        m_fe->SetPreviewRoofPart(part.style, part.variant == 2, part.variant == 1, part.carbon);
        break;
    case 0x10:
        m_fe->SetPreviewExhaustPart(part.style);
        break;
    case 9:
        m_fe->SetPreviewHoodPart(part.style, part.carbon);
        break;
    case 0xe:
        m_fe->SetPreviewHeadlightPart(part.style);
        break;
    case 0xf:
        m_fe->SetPreviewTaillightPart(part.style);
        break;
    case 0x1:
        m_fe->SetPreviewFrontBumperPart(part.style);
        break;
    case 0x2:
        m_fe->SetPreviewRearBumperPart(part.style);
        break;
    case 0xb:
        m_fe->SetPreviewSkirtPart(part.style);
        break;
    case 6:
        m_fe->SetPreviewWideBodyPart(part.style);
        break;
    default:
        break;
    }
}

void FEStartupSequence::PreviewOwnedParts(const Career::OwnedCar *car)
{
    m_fe->SetPreviewSpoilerPart(car ? car->spoiler : 0, car && car->spoilerCarbon);
    m_fe->SetPreviewRoofPart(car ? car->roof : 0, car && car->roofDual, car && car->roofOffset,
                             car && car->roofCarbon);
    m_fe->SetPreviewExhaustPart(car ? car->exhaust : 0);
    m_fe->SetPreviewHoodPart(car ? car->hood : 0, car && car->hoodCarbon);
    m_fe->SetPreviewHeadlightPart(car ? car->headlight : 0);
    m_fe->SetPreviewTaillightPart(car ? car->taillight : 0);
    m_fe->SetPreviewFrontBumperPart(car ? car->frontBumper : 0);
    m_fe->SetPreviewRearBumperPart(car ? car->rearBumper : 0);
    m_fe->SetPreviewSkirtPart(car ? car->skirt : 0);
    m_fe->SetPreviewWideBodyPart(car ? car->wideBody : 0);
}

void FEStartupSequence::RefreshPartsBrowserInfo(FEPackage *pkg)
{
    FENativeInterface::SetLanguageHash(pkg, 0x6c9e933e, kPartsCards[m_partsCategorySel].label);
    const CarPart *part = SelectedPartsShopPart();
    if (!part) {
        FENativeInterface::SetString(pkg, 0xd28b9316, "-");
        FENativeInterface::SetString(pkg, 0x380b632, "$0");
        FENativeInterface::SetString(pkg, 0x2f5e7da3, "00000");
        return;
    }
    // 0x002a7638 builds the label from the record name and its level ("JORGEN L1"); see the
    // [Parts] boot summary for the same rendering.
    char name[96];
    snprintf(name, sizeof name, "%s L%u", part->name.c_str(), unsigned(part->level));
    const uint32_t price = m_fe->Parts().Price(*part);
    char priceText[32];
    snprintf(priceText, sizeof priceText, "$%u", unsigned(price));
    FENativeInterface::SetString(pkg, 0xd28b9316, name);
    FENativeInterface::SetString(pkg, 0x380b632, priceText);
    // The bottom bar's "Total Cost" value (0x2f5e7da3) is written as five digits by 0x002bb0a0;
    // with no basket the highlighted part's price stands in, as in the paint shop (#176/#185).
    char total[16];
    snprintf(total, sizeof total, "%05u", unsigned(price));
    FENativeInterface::SetString(pkg, 0x2f5e7da3, total);
    PreviewPartsBrowserPart(*part);
}

Career::OwnedCar *FEStartupSequence::ShopCar()
{
    if (!m_profile.valid)
        return nullptr;
    // The shop stands on the previewed car (0x001f44f0 loads the profile's current car); fall back
    // to the profile's current slot when the preview is showing a roster stand-in.
    for (Career::OwnedCar &car : m_profile.cars)
        if (car.model == m_fe->PreviewCarName())
            return &car;
    if (m_profile.currentCar >= 0 && m_profile.currentCar < int(m_profile.cars.size()))
        return &m_profile.cars[m_profile.currentCar];
    return nullptr;
}

void FEStartupSequence::ShowMessageDialog(uint32_t messageLabel)
{
    ShowMessageDialogText(m_fe->LanguageText(messageLabel).c_str());
}

void FEStartupSequence::ShowMessageDialogText(const char *text)
{
    if (m_dialogPackage)
        return;
    FEPackage *pkg = m_engine->PushPackage("genericdialog_small.fng", 0, 0xff);
    if (!pkg) {
        printf("[Dialog] could not push genericdialog_small.fng\n");
        return;
    }
    m_dialogPackage = pkg;
    // The package ships the memory-card prompt; the caller's label replaces it.  0x00292888 sets
    // the same centred text object from the label 0x00260b40 stored.
    FENativeInterface::SetString(pkg, kDialogMessage, text);
    // 0x00260170 selects the one-button form: the manager shows the centred "Yes" and drops the
    // Yes/No pair the two-button form uses.  Hide the pair together with its backing bars.
    ShowDialogControl(pkg, kDialogYesLeft, false);
    ShowDialogControl(pkg, kDialogYesLeftBar, false);
    ShowDialogControl(pkg, kDialogNo, false);
    ShowDialogControl(pkg, kDialogNoBar, false);
    ShowDialogControl(pkg, kDialogYesCentre, true, true);
    ShowDialogControl(pkg, kDialogYesCentreBar, true);
    printf("[Dialog] %s\n", text);
}

namespace {
// Focus the Yes or No button of the two-button prompt: the focused label uses the highlight green,
// the other one plain white.
void ApplyYesNoFocus(FEPackage *pkg, int selected)
{
    auto tint = [pkg](uint32_t hash, bool focused) {
        if (FEObject *o = pkg->FindObjectByHash(hash))
            if (FEObjData *d = o->GetObjData()) {
                d->Col.a = 255;
                d->Col.r = focused ? 204 : 255;
                d->Col.g = focused ? 244 : 255;
                d->Col.b = focused ? 138 : 255;
            }
    };
    tint(kDialogYesLeft, selected == 0);
    tint(kDialogNo, selected == 1);
}
void ShowYesNoControls(FEPackage *pkg, int selected)
{
    ShowDialogControl(pkg, kDialogYesCentre, false);
    ShowDialogControl(pkg, kDialogYesCentreBar, false);
    ShowDialogControl(pkg, kDialogYesLeft, true);
    ShowDialogControl(pkg, kDialogYesLeftBar, true);
    ShowDialogControl(pkg, kDialogNo, true);
    ShowDialogControl(pkg, kDialogNoBar, true);
    ApplyYesNoFocus(pkg, selected);
}
} // namespace

void FEStartupSequence::ShowYesNoDialog(const char *text, std::function<void()> onYes)
{
    if (m_dialogPackage)
        return;
    FEPackage *pkg = m_engine->PushPackage("genericdialog_small.fng", 0, 0xff);
    if (!pkg) {
        printf("[Dialog] could not push genericdialog_small.fng\n");
        return;
    }
    m_dialogPackage = pkg;
    m_dialogYesNo = true;
    m_dialogYesSel = 1; // No is the safe default
    m_dialogYesAction = std::move(onYes);
    FENativeInterface::SetString(pkg, kDialogMessage, text);
    ShowYesNoControls(pkg, m_dialogYesSel);
    printf("[Dialog] %s (Yes / No)\n", text);
}

void FEStartupSequence::HandleDialogLoaded(FEPackage *pkg)
{
    if (m_dialogYesNo) { // the intro fade re-shows the default objects; keep the two-button form
        ShowYesNoControls(pkg, m_dialogYesSel);
        return;
    }
    // The package's intro fade re-shows its default objects; keep the one-button form on top of it.
    ShowDialogControl(pkg, kDialogYesLeft, false);
    ShowDialogControl(pkg, kDialogYesLeftBar, false);
    ShowDialogControl(pkg, kDialogNo, false);
    ShowDialogControl(pkg, kDialogNoBar, false);
    ShowDialogControl(pkg, kDialogYesCentre, true, true);
    ShowDialogControl(pkg, kDialogYesCentreBar, true);
}

bool FEStartupSequence::CloseMessageDialog()
{
    if (!m_dialogPackage)
        return false;
    FEPackage *pkg = m_dialogPackage;
    m_dialogPackage = nullptr;
    m_dialogYesNo = false;
    m_dialogYesAction = nullptr;
    printf("[Dialog] dismissed\n");
    m_engine->UnloadPackage(pkg);
    return true;
}

void FEStartupSequence::BuyPartsBrowserPart(FEPackage *pkg)
{
    (void)pkg;
    const CarPart *selected = SelectedPartsShopPart();
    if (!selected)
        return;
    const CarPart &part = *selected;
    Career::OwnedCar *car = ShopCar();
    // 0x002b5b00 tests the career item pool for the part before selling it: an owned part can be
    // fitted again for free, and the part already on the car is a no-op.
    if (car && OwnedCarHasPart(*car, part)) {
        printf("[PartsShop] %s L%u is already fitted\n", part.name.c_str(), unsigned(part.level));
        return;
    }
    const uint32_t key = PartsShopKey(part);
    const bool owned = HasOwnedPart(m_profile, key);
    const uint32_t price = m_fe->Parts().Price(part);
    // 0x002a7278 compares the career balance (0x0026bbc0) with the price (0x0026b0c8) and, when
    // it is short, opens GenericDialog_SMALL.fng (0x9f00f07) with the "not enough bank" label and
    // refuses the sale (ISSUES #189).
    if (!owned && car) {
        if (m_profile.money < int(price)) {
            // 0x002a7278: the balance (0x0026bbc0) is below the part's price, so the original
            // opens GenericDialog_SMALL.fng instead of selling.
            printf("[PartsShop] %s L%u costs $%u, career has $%d\n", part.name.c_str(),
                   unsigned(part.level), price, m_profile.money);
            ShowMessageDialog(kLabelNotEnoughBank);
            return;
        }
        m_profile.money -= int(price);
    }
    const char *what = "part";
    if (car) {
        // Keep whatever was fitted before this replacement in the pool (#187).
        RememberFittedParts(m_profile, *car);
        switch (part.partId) {
        case 0xc:
            car->spoiler = uint8_t(part.style);
            car->spoilerCarbon = part.carbon;
            what = "spoiler";
            break;
        case 7:
            car->roof = uint8_t(part.style);
            car->roofDual = part.variant == 2;
            car->roofOffset = part.variant == 1;
            car->roofCarbon = part.carbon;
            what = "roof scoop";
            break;
        case 0x10:
            car->exhaust = uint8_t(part.style);
            what = "exhaust tip";
            break;
        case 9:
            car->hood = uint8_t(part.style);
            car->hoodCarbon = part.carbon;
            what = "hood";
            break;
        case 0xe:
            car->headlight = uint8_t(part.style);
            what = "headlights";
            break;
        case 0xf:
            car->taillight = uint8_t(part.style);
            what = "taillights";
            break;
        case 0x1:
            car->frontBumper = uint8_t(part.style);
            what = "front bumper";
            break;
        case 0x2:
            car->rearBumper = uint8_t(part.style);
            what = "rear bumper";
            break;
        case 0xb:
            car->skirt = uint8_t(part.style);
            what = "side skirt";
            break;
        case 6:
            car->wideBody = uint8_t(part.style);
            what = "wide body kit";
            break;
        default:
            break;
        }
        AddOwnedPart(m_profile, key);
        if (m_profile.valid && !Career::SaveCareer(m_profile))
            printf("[PartsShop] fitted %s L%u but the save failed\n", part.name.c_str(),
                   unsigned(part.level));
        else if (owned)
            printf("[PartsShop] fitted %s style %u (%s L%u, owned), career $%d\n", what,
                   unsigned(part.style), part.name.c_str(), unsigned(part.level), m_profile.money);
        else
            printf("[PartsShop] fitted %s style %u (%s L%u, $%u), career $%d\n", what,
                   unsigned(part.style), part.name.c_str(), unsigned(part.level), price,
                   m_profile.money);
    } else {
        printf("[PartsShop] no career car; previewing %s L%u only\n", part.name.c_str(),
               unsigned(part.level));
    }
    PreviewPartsBrowserPart(part);
}

void FEStartupSequence::HandlePartsBrowserMessage(uint32_t msg, FEPackage *pkg)
{
    if (m_partsCategoryScroller.Empty() || m_partsCategoryScroller.Package() != pkg)
        return;
    if (msg == kMsgScreenTick) {
        m_partsCategoryScroller.HandleMessage(msg, TickSeconds());
        m_partsScroller.HandleMessage(msg, TickSeconds());
        return;
    }
    if (msg == kMsgInitComplete) {
        m_partsCategoryScroller.HandleMessage(msg, 0.0f);
        m_partsScroller.HandleMessage(msg, 0.0f);
        m_partsCategoryScroller.AllowFade();
        m_partsScroller.AllowFade();
        return;
    }
    if (msg == kMsgPadLeft || msg == kMsgPadRight) {
        m_partsCategoryScroller.HandleMessage(msg, 0.0f);
        if (m_partsCategoryScroller.Selected() != m_partsCategorySel) {
            m_partsCategorySel = m_partsCategoryScroller.Selected();
            FENativeInterface::SetLanguageHash(
                pkg, 0x42adb44c,
                m_partsCategorySel == kPartsCarbonCard ? 0x24911e04u : 0x796dc449u);
            RefreshPartsBrowserList(pkg);
        }
        return;
    }
    // 0x002a6fd0 moves the item list; the pad's Up/Down drive it (Left/Right are the slot cards).
    // The scroller only knows Left/Right, so translate before handing the message over.
    if (msg == kMsgPadUp || msg == kMsgPadDown) {
        m_partsScroller.HandleMessage(msg == kMsgPadUp ? kMsgPadLeft : kMsgPadRight, 0.0f);
        if (!m_partsShopItems.empty() && m_partsScroller.Selected() != m_partsSel) {
            m_partsSel = m_partsScroller.Selected();
            RefreshPartsBrowserInfo(pkg);
        }
        return;
    }
    if (msg == kMsgPadBack && !m_navPending) {
        // Leaving the shop puts the fitted parts back on the preview; the highlighted one was only
        // a preview until Accept bought it.
        PreviewOwnedParts(ShopCar());
        PopScreen();
        return;
    }
    if ((msg == kMsgButtonPressed || msg == kMsgPadAccept) && !m_navPending) {
        BuyPartsBrowserPart(pkg);
        return;
    }
}

// UI_ChoosePaintCategory.fng (ctor 0x002ba798, AddOptions 0x002bb0f0): the Graphics Shop's four
// cards.  0x002bb0f0 adds Paint (option +0x58 = 2, +0x5c = 0), Vinyls (+0x58 = 0, +0x5c = 1),
// Decals (+0x58 = 1, +0x5c = 2) and the Install/Purchase Setup option (+0x54 = 1).  The package
// ships the cards transparent at 4b4e3888..8b, so bind them with their AddOptions icon/label
// hashes.  Only Paint's destination, UI_Paint.fng, exists in the port; the other three remain
// visible but disabled rather than opening an unimplemented screen.
void FEStartupSequence::HandlePaintCategoryLoaded(FEPackage *pkg)
{
    static const struct {
        uint32_t tex, label;
        bool disabled;
        const char *debug;
    } kOptions[] = {
        {0x550681a4, 0x6a4c46a6, false, "Paint"},
        {0x55778e5a, 0x78deea1c, true, "Vinyls"},
        {0xda1dae54, 0x4e933e83, true, "Decals"},
        // 0x002bb0f0 picks the label by game mode: 0x920dbb02 in career (DAT_00506b50 == 1),
        // otherwise 0xd132a6fe.  The port's car-shop path is the non-career one.
        {0x911a4ede, 0xd132a6fe, true, "Install Setup"},
    };
    m_paintCategorySel = std::min<size_t>(m_paintCategorySel, 3);
    std::vector<FEIconScroller::Def> defs;
    std::vector<FEObject *> slots;
    for (size_t i = 0; i < sizeof(kOptions) / sizeof(kOptions[0]); ++i) {
        defs.push_back({kOptions[i].tex, kOptions[i].label, kOptions[i].disabled});
        slots.push_back(pkg->FindObjectByHash(0x4b4e3888u + uint32_t(i)));
    }
    m_paintCategoryScroller.SetupObjects(pkg, defs, slots, m_paintCategorySel, "OPTION_MASTER");
    printf("[Customize] graphics shop: %zu cards, %s selected\n", defs.size(),
           kOptions[m_paintCategorySel].debug);
}

void FEStartupSequence::HandlePaintCategoryMessage(uint32_t msg, FEPackage *pkg)
{
    if (m_paintCategoryScroller.Empty() || m_paintCategoryScroller.Package() != pkg)
        return;
    if (msg == kMsgScreenTick) {
        m_paintCategoryScroller.HandleMessage(msg, TickSeconds());
        return;
    }
    if (msg == kMsgInitComplete) {
        m_paintCategoryScroller.HandleMessage(msg, 0.0f);
        m_paintCategoryScroller.AllowFade();
        return;
    }
    if (msg == kMsgPadLeft || msg == kMsgPadRight) {
        m_paintCategoryScroller.HandleMessage(msg, 0.0f);
        m_paintCategorySel = m_paintCategoryScroller.Selected();
        return;
    }
    if (msg == kMsgPadBack && !m_navPending) {
        PopScreen();
        return;
    }
    if ((msg == kMsgButtonPressed || msg == kMsgPadAccept) && !m_navPending) {
        m_paintCategorySel = m_paintCategoryScroller.Selected();
        if (m_paintCategorySel == 0) {
            NavigateTo(pkg->GetName(), "UI_Paint.fng");
        } else {
            // 0x002a45e0 maps these to UI_ChooseVinylLayer.fng / UI_DecalMain.fng; neither screen
            // is ported yet, and Install/Purchase Setup has no ported target either.
            printf("[Customize] graphics shop option %zu is not ported yet\n", m_paintCategorySel);
        }
        return;
    }
}

// UI_Paint.fng (ctor 0x002bc1e0, AddOptions 0x002bcff8, fill 0x002bd868): the paint shop's colorable
// parts.  The package's 9 part cards (4b4e3888..4b4e3890) are a scroller (OPTION_MASTER b04669e3 at
// 0,-150) selecting Base Paint, Spoiler, Roof Scoop, Mirrors, Rims, Spinners, Brakes, Engine Accents
// and Muffler Tip.  Accept enters the selected part's color grid (0x002bdf10), Back leaves it
// (0x002be150), and Back on the cards exits the screen.  The grid is 3 rows of 20 (group eefb2050
// under db285767), and row 1 runs right-to-left in the package.  Only Base Paint is visible on the
// port's single-mesh car; the other eight parts still store their own color per car (#169).
namespace {
constexpr uint32_t kPaintGridGroup = 0xdb285767;
constexpr uint32_t kPaintSwatchGroup = 0xeefb2050;
constexpr uint32_t kPaintInfoGroup = 0x9ff84a88;
constexpr uint32_t kPaintHighlight = 0x23a43844;
constexpr uint32_t kPaintColorName = 0xef653aff; // bottom-left selected-colour name (group 031a9c0a)
constexpr uint32_t kPaintCost = 0x2f5e7da3;      // total-cost value, set by 0x002bb0a0 (#176)
constexpr uint32_t kPaintPartPrice = 0x0013c45e; // "CASH" text used as the part price (0x002bdf10)

// 0x002bcff8's part records in AddOptions order.  The icon/label hashes are the ones passed to
// FUN_0029ae78 for each card; the part id is stored at option +0x58.
struct PaintPartDef {
    uint32_t id;
    uint32_t icon;
    uint32_t label;
    const char *debug;
};
constexpr PaintPartDef kPaintParts[] = {
    {0x3f, 0xa3b76154, 0xef5b4a75, "Base Paint"},
    {0x45, 0xa8d7e6ed, 0xdda5b898, "Spoiler"},
    {0x4b, 0xd2241205, 0xef644b90, "Roof Scoop"},
    {0x4c, 0xdf5c2bca, 0x4d90f415, "Mirrors"},
    {0x49, 0xd223f84a, 0x0f030942, "Rims"},
    {0x4a, 0xd223f84a, 0xdd3bebf9, "Spinners"},
    {0x46, 0x7eb55a26, 0xdacd9fff, "Brakes"},
    {0x44, 0x4e875a92, 0x3b3bfaf0, "Engine Accents"},
    {0x47, 0xa6343aec, 0xba1cc49c, "Muffler Tip"},
};
constexpr size_t kPaintPartCount = sizeof(kPaintParts) / sizeof(kPaintParts[0]);

int PaintGridCol(size_t index)
{
    const int col = int(index % 20);
    return int(index / 20) == 1 ? 19 - col : col; // row 1 is laid out right-to-left
}
size_t PaintGridIndex(int row, int col)
{
    return size_t(row * 20 + (row == 1 ? 19 - col : col));
}
int PaintGridRows(size_t count)
{
    return int((count + 19) / 20);
}
// Occupied columns of a row.  A short last row leaves the rest of the 20-slot strip hidden; row 1
// fills from the right because the package lays it out right-to-left (serpentine).
void PaintRowRange(int row, size_t count, int &lo, int &hi)
{
    const size_t start = size_t(row) * 20;
    const size_t n = count > start ? std::min<size_t>(20, count - start) : 0;
    if (!n) {
        lo = hi = 0;
    } else if (row == 1) {
        lo = 20 - int(n);
        hi = 19;
    } else {
        lo = 0;
        hi = int(n) - 1;
    }
}
} // namespace

void FEStartupSequence::HandlePaintLoaded(FEPackage *pkg)
{
    // 0x002bcff8: add the nine part cards to the part scroller.  The package ships the cards
    // invisible and transparent; FEIconScroller::SetupObjects fills in the icon/name and alpha.
    std::vector<FEIconScroller::Def> defs;
    std::vector<FEObject *> slots;
    for (size_t i = 0; i < kPaintPartCount; ++i) {
        defs.push_back({kPaintParts[i].icon, kPaintParts[i].label, false});
        slots.push_back(pkg->FindObjectByHash(0x4b4e3888u + uint32_t(i)));
    }
    m_paintScroller.SetupObjects(pkg, defs, slots, 0, "OPTION_MASTER");
    m_paintPart = 0;
    m_paintGridActive = false;

    // Seed each card from the car the preview shows.  Base Paint comes from the preview (which the
    // career load / car select already set); the other eight parts come from the car's saved part
    // colours.  A stored set of 0 means the part's default finish (PaintCatalog::SetForPart).
    const Career::OwnedCar *car = nullptr;
    if (m_careerExists && m_profile.currentCar >= 0 &&
        m_profile.currentCar < int(m_profile.cars.size())) {
        const Career::OwnedCar &c = m_profile.cars[size_t(m_profile.currentCar)];
        if (c.model == m_fe->PreviewCarName())
            car = &c;
    }
    for (size_t i = 0; i < kPaintPartCount; ++i) {
        m_partSet[i] = PartDefaultSet(i);
        m_partSel[i] = 0;
        m_partPaintIndex[i] = 0;
        if (i == 0) {
            const uint32_t set = m_fe->PreviewPaintSet();
            if (m_fe->Paint().Has(set))
                m_partSet[i] = set;
            const uint32_t paint = m_fe->PreviewPaint();
            if (paint > 0 && paint <= m_fe->Paint().Count(m_partSet[i])) {
                m_partSel[i] = paint - 1;
                m_partPaintIndex[i] = paint;
            }
        } else if (car) {
            const uint32_t set = car->partPaintSet[i - 1];
            if (set && m_fe->Paint().Has(set))
                m_partSet[i] = set;
            const uint32_t paint = car->partPaint[i - 1];
            if (paint > 0 && paint <= m_fe->Paint().Count(m_partSet[i])) {
                m_partSel[i] = paint - 1;
                m_partPaintIndex[i] = paint;
            }
        }
    }
    m_paintSet = m_partSet[0];
    m_paintSel = m_partSel[0];
    RefreshRimPreview();
    RefreshCaliperPreview();
    RefreshMirrorPreview();
    RefreshSpoilerPreview();

    // 0x002bc2f0 keeps the swatch grid closed (0x67) until Accept; the cards and OPTION_MASTER's
    // header are the screen's initial state.  The header stays "Graphics Shop" (42adb44c label
    // 46c41a7c); the scroller writes the selected part's name into ICON_TITLE.
    ShowPaintGrid(pkg, false);
    RefreshPaintCost(pkg);
    FENativeInterface::SetVisible(pkg, 0x444969fd, false); // scrollbar arrows
    FENativeInterface::SetVisible(pkg, 0x444969fe, false);
    FENativeInterface::SetVisible(pkg, 0x688a1551, false); // scrollbar backing/handle
    FENativeInterface::SetVisible(pkg, 0x3fbb7a4e, false);
    if (m_fe->Paint().Empty())
        printf("[Paint] no GLOBALB paint colours; the grid stays empty\n");
    printf("[Paint] %zu part cards, 0x%08x selected\n", kPaintPartCount, kPaintParts[0].id);
}

uint32_t FEStartupSequence::PartDefaultSet(size_t part) const
{
    return part < kPaintPartCount ? PaintCatalog::SetForPart(kPaintParts[part].id) : 0;
}

// Both Rims (card 4) and Spinners (card 5) drive the only wheel mesh the port assembles.  Show the
// Rims colour when the car has one, otherwise the Spinners colour (ISSUES #172).
void FEStartupSequence::RefreshRimPreview()
{
    // Dev aid: NFSU2_RIM_PAINT=<1..60> previews a RIMS/FINISH colour without touching the save.
    if (const char *rim = getenv("NFSU2_RIM_PAINT")) {
        const int index = atoi(rim);
        if (index >= 0) {
            m_fe->SetPreviewRimPaint(PaintCatalog::kRims, uint32_t(index));
            return;
        }
    }
    size_t part = 4;
    if (m_partPaintIndex[4] == 0 && m_partPaintIndex[5] != 0)
        part = 5;
    m_fe->SetPreviewRimPaint(m_partSet[part] ? m_partSet[part] : PaintCatalog::kRims,
                             m_partPaintIndex[part]);
}

void FEStartupSequence::RefreshCaliperPreview()
{
    if (const char *brake = getenv("NFSU2_BRAKE_PAINT")) {
        const int index = atoi(brake);
        if (index >= 0) {
            m_fe->SetPreviewCaliperPaint(PaintCatalog::kBrakes, uint32_t(index));
            return;
        }
    }
    m_fe->SetPreviewCaliperPaint(m_partSet[6] ? m_partSet[6] : PaintCatalog::kBrakes,
                                 m_partPaintIndex[6]);
}

void FEStartupSequence::RefreshMirrorPreview()
{
    if (const char *mirror = getenv("NFSU2_MIRROR_PAINT")) {
        const int index = atoi(mirror);
        if (index >= 0) {
            m_fe->SetPreviewMirrorPaint(PaintCatalog::kGloss, uint32_t(index));
            return;
        }
    }
    m_fe->SetPreviewMirrorPaint(m_partSet[3] ? m_partSet[3] : PaintCatalog::kGloss,
                                m_partPaintIndex[3]);
}

void FEStartupSequence::RefreshSpoilerPreview()
{
    if (const char *spoiler = getenv("NFSU2_SPOILER_PAINT")) {
        const int index = atoi(spoiler);
        if (index >= 0) {
            m_fe->SetPreviewSpoilerPaint(PaintCatalog::kGloss, uint32_t(index));
            return;
        }
    }
    m_fe->SetPreviewSpoilerPaint(m_partSet[1] ? m_partSet[1] : PaintCatalog::kGloss,
                                 m_partPaintIndex[1]);
}

void FEStartupSequence::ShowPaintGrid(FEPackage *pkg, bool show)
{
    // 0x002bd868 reveals db285767 (grid + info strip) when a part is entered; 0x002be150 hides the
    // grid again (0xdf422345) but leaves the cards/header up.
    if (FEObject *grid = pkg->FindObjectByHash(kPaintGridGroup)) {
        FENativeInterface::SetObjectVisible(grid, show);
        if (show)
            FENativeInterface::SetAlpha(pkg, kPaintGridGroup, 255);
    }
    if (FEObject *info = pkg->FindObjectByHash(kPaintInfoGroup)) {
        FENativeInterface::SetObjectVisible(info, show);
        if (show)
            FENativeInterface::SetAlpha(pkg, kPaintInfoGroup, 255);
    }
    if (!show)
        FENativeInterface::SetVisible(pkg, kPaintHighlight, false);
}

void FEStartupSequence::EnterPaintGrid(FEPackage *pkg)
{
    if (m_paintPart >= kPaintPartCount)
        return;
    m_paintSet = m_partSet[m_paintPart] ? m_partSet[m_paintPart] : PartDefaultSet(m_paintPart);
    if (!m_fe->Paint().Has(m_paintSet))
        m_paintSet = PartDefaultSet(m_paintPart);
    const size_t count = m_fe->Paint().Count(m_paintSet);
    if (!count) {
        printf("[Paint] %s has no colours in this pack\n", kPaintParts[m_paintPart].debug);
        return;
    }
    m_paintSel = m_partSel[m_paintPart] < count ? m_partSel[m_paintPart] : 0;
    m_gridOriginalSet = m_fe->PreviewPaintSet();
    m_gridOriginalPaint = m_fe->PreviewPaint();
    m_gridOriginalRimSet = m_fe->PreviewRimSet();
    m_gridOriginalRim = m_fe->PreviewRimPaint();
    m_gridOriginalCaliperSet = m_fe->PreviewCaliperSet();
    m_gridOriginalCaliper = m_fe->PreviewCaliperPaint();
    m_gridOriginalMirrorSet = m_fe->PreviewMirrorSet();
    m_gridOriginalMirror = m_fe->PreviewMirrorPaint();
    m_gridOriginalSpoilerSet = m_fe->PreviewSpoilerSet();
    m_gridOriginalSpoiler = m_fe->PreviewSpoilerPaint();
    m_gridOriginalRoofSet = m_fe->PreviewRoofSet();
    m_gridOriginalRoof = m_fe->PreviewRoofPaint();
    m_gridOriginalEngineSet = m_fe->PreviewEngineSet();
    m_gridOriginalEngine = m_fe->PreviewEnginePaint();
    m_gridOriginalExhaustSet = m_fe->PreviewExhaustSet();
    m_gridOriginalExhaust = m_fe->PreviewExhaustPaint();
    m_paintGridActive = true;
    ShowPaintGrid(pkg, true);
    RefreshPaintSwatches(pkg);
    if (m_paintPart == 0) // Base Paint tints the body
        m_fe->SetPreviewPaint(m_paintSet, m_paintSel + 1);
    else if (m_paintPart == 4 || m_paintPart == 5) // Rims/Spinners tint the wheel faces (#172)
        m_fe->SetPreviewRimPaint(m_paintSet, m_paintSel + 1);
    else if (m_paintPart == 6) // Brakes tint the calipers (#173)
        m_fe->SetPreviewCaliperPaint(m_paintSet, m_paintSel + 1);
    else if (m_paintPart == 3) // Mirrors tint the housings (#174)
        m_fe->SetPreviewMirrorPaint(m_paintSet, m_paintSel + 1);
    else if (m_paintPart == 1) // Spoiler tints the stock wing (#175)
        m_fe->SetPreviewSpoilerPaint(m_paintSet, m_paintSel + 1);
    else if (m_paintPart == 2) // Roof Scoop tints the installed scoop (#180)
        m_fe->SetPreviewRoofPaint(m_paintSet, m_paintSel + 1);
    else if (m_paintPart == 7) // Engine Accents tint the installed accents (#180)
        m_fe->SetPreviewEnginePaint(m_paintSet, m_paintSel + 1);
    else if (m_paintPart == 8) // Muffler Tip tints the installed tip (#180)
        m_fe->SetPreviewExhaustPaint(m_paintSet, m_paintSel + 1);
    printf("[Paint] enter %s (0x%08x), %zu colours\n", kPaintParts[m_paintPart].debug,
           kPaintParts[m_paintPart].id, count);
}

void FEStartupSequence::LeavePaintGrid(FEPackage *pkg, bool restorePreview)
{
    if (m_paintPart < kPaintPartCount)
        m_partSel[m_paintPart] = m_paintSel; // the original keeps the grid's cursor per filter
    if (restorePreview && m_paintPart == 0)
        m_fe->SetPreviewPaint(m_gridOriginalSet, m_gridOriginalPaint);
    if (restorePreview && (m_paintPart == 4 || m_paintPart == 5))
        m_fe->SetPreviewRimPaint(m_gridOriginalRimSet, m_gridOriginalRim);
    if (restorePreview && m_paintPart == 6)
        m_fe->SetPreviewCaliperPaint(m_gridOriginalCaliperSet, m_gridOriginalCaliper);
    if (restorePreview && m_paintPart == 3)
        m_fe->SetPreviewMirrorPaint(m_gridOriginalMirrorSet, m_gridOriginalMirror);
    if (restorePreview && m_paintPart == 1)
        m_fe->SetPreviewSpoilerPaint(m_gridOriginalSpoilerSet, m_gridOriginalSpoiler);
    if (restorePreview && m_paintPart == 2)
        m_fe->SetPreviewRoofPaint(m_gridOriginalRoofSet, m_gridOriginalRoof);
    if (restorePreview && m_paintPart == 7)
        m_fe->SetPreviewEnginePaint(m_gridOriginalEngineSet, m_gridOriginalEngine);
    if (restorePreview && m_paintPart == 8)
        m_fe->SetPreviewExhaustPaint(m_gridOriginalExhaustSet, m_gridOriginalExhaust);
    m_paintGridActive = false;
    ShowPaintGrid(pkg, false);
    RefreshPaintCost(pkg);
}

void FEStartupSequence::RefreshPaintSwatches(FEPackage *pkg)
{
    FEObject *grid = pkg->FindObjectByHash(kPaintSwatchGroup);
    if (!grid || grid->Type != FE_Group)
        return;
    const size_t count = m_fe->Paint().Count(m_paintSet);
    size_t i = 0;
    // Walk every package slot: a 30-color set (PEARL) must hide the grid's last 30 swatches, and
    // switching back to a 60-color set must reveal them again (0x002bd868 hides slots past the
    // filled count with 0x00259ee8).
    for (FEObject *swatch = static_cast<FEGroup *>(grid)->GetFirstChild(); swatch;
         swatch = swatch->GetNext(), ++i) {
        if (i >= count) {
            FENativeInterface::SetObjectVisible(swatch, false);
            continue;
        }
        const PaintColor &c = m_fe->Paint().At(m_paintSet, i);
        FEObjData *d = swatch->GetObjData();
        if (!d)
            continue;
        d->Col.b = c.b;
        d->Col.g = c.g;
        d->Col.r = c.r;
        d->Col.a = 255;
        swatch->Flags |= 0x400000u; // FF_DirtyCode
        FENativeInterface::SetObjectVisible(swatch, true);
    }
    PositionPaintHighlight(pkg);
    RefreshPaintLabel(pkg);
    RefreshPaintCost(pkg);
}

void FEStartupSequence::PositionPaintHighlight(FEPackage *pkg)
{
    FEObject *grid = pkg->FindObjectByHash(kPaintSwatchGroup);
    FEObject *highlight = pkg->FindObjectByHash(kPaintHighlight);
    if (!grid || !highlight || grid->Type != FE_Group)
        return;
    size_t i = 0;
    FEObject *swatch = static_cast<FEGroup *>(grid)->GetFirstChild();
    for (; swatch && i < m_paintSel; swatch = swatch->GetNext(), ++i) {
    }
    if (!swatch)
        return;
    float gx, gy, sx, sy;
    FENativeInterface::GetCenter(grid, gx, gy); // eefb2050's origin inside db285767
    FENativeInterface::GetCenter(swatch, sx, sy);
    FENativeInterface::SetCenter(highlight, gx + sx, gy + sy);
    if (highlight->GetObjData())
        highlight->GetObjData()->Col.a = 255; // the package ships the frame transparent
    FENativeInterface::SetObjectVisible(highlight, true);
}

void FEStartupSequence::RefreshPaintLabel(FEPackage *pkg)
{
    // 031a9c0a's ef653aff is the selected colour's name in the bottom bar.  The original shows the
    // paint's friendly name from the paint manager; the port has only the GLOBALB record names
    // (GLOSS_L1_COLOR01, ...), which overflow the bar.  The set name makes the L1/R1 finish switch
    // visible, which the flat-color preview cannot show by itself (#168).
    char text[48];
    snprintf(text, sizeof(text), "%s %02u", PaintCatalog::SetName(m_paintSet), unsigned(m_paintSel + 1));
    FENativeInterface::SetString(pkg, kPaintColorName, text);
}

void FEStartupSequence::RefreshPaintCost(FEPackage *pkg)
{
    // 0x002b3e10 sums the pending parts and 0x002bb0a0 writes the result to 0x2f5e7da3; 0x002bdf10
    // also writes the highlighted part's own price to the "CASH" text (bStringHash("CASH") =
    // 0x0013c45e, format "%$d") and shows it, while 0x002be150 hides that row again.  The port has
    // no purchase/economy path yet, so the highlighted colour's price (0x0026b0c8) stands in for
    // both fields while the swatch grid is open.
    unsigned price = 0;
    if (m_paintGridActive && m_paintPart < kPaintPartCount) {
        const PaintColor &color = m_fe->Paint().At(m_paintSet, m_paintSel);
        price = m_fe->Paint().Price(kPaintParts[m_paintPart].id, m_paintSet, color);
    }
    char text[16];
    snprintf(text, sizeof(text), "%05u", price);
    FENativeInterface::SetString(pkg, kPaintCost, text);
    FENativeInterface::SetString(pkg, kPaintPartPrice, text);
    FENativeInterface::SetVisible(pkg, kPaintPartPrice, m_paintGridActive);
}

void FEStartupSequence::HandlePaintMessage(uint32_t msg, FEPackage *pkg)
{
    if (msg == kMsgScreenTick) {
        if (!m_paintScroller.Empty())
            m_paintScroller.HandleMessage(msg, TickSeconds());
        return;
    }
    if (msg == kMsgInitComplete) {
        if (!m_paintScroller.Empty()) {
            m_paintScroller.HandleMessage(msg, 0.0f);
            m_paintScroller.AllowFade();
        }
        return;
    }
    if (m_paintScroller.Empty() || m_paintScroller.Package() != pkg)
        return;

    if (!m_paintGridActive) {
        // 0x002bc2f0: while the grid is closed the d-pad drives the part-card scroller; the
        // selected card's color set becomes the screen's current set (0x002bedd8).
        if (msg == kMsgPadLeft || msg == kMsgPadRight) {
            m_paintScroller.HandleMessage(msg, 0.0f);
            if (m_paintScroller.Selected() != m_paintPart) {
                m_paintPart = m_paintScroller.Selected();
                m_paintSet = m_partSet[m_paintPart];
                m_paintSel = m_partSel[m_paintPart];
            }
            return;
        }
        if (msg == kMsgPadBack && !m_navPending) {
            PopScreen();
            return;
        }
        if ((msg == kMsgButtonPressed || msg == kMsgPadAccept) && !m_navPending) {
            // 0x002bd6f0 -> 0x002bdf10.  The original can first open the parts-purchase dialog;
            // the port has no parts economy, so every card opens its grid directly.
            EnterPaintGrid(pkg);
            return;
        }
        return;
    }

    if (msg == kMsgPadLTrigger || msg == kMsgPadRTrigger) {
        // 0x002bc2f0 forwards the shoulder buttons only while the grid is open and the part edits
        // one of the base-paint finishes.  0x002bdf10 forces GLOSS back onto the Spoiler card if
        // it was cycled, so finish switching is effectively Base Paint only.
        if (m_paintPart != 0)
            return;
        if (m_paintSet != PaintCatalog::kGloss && m_paintSet != PaintCatalog::kMetal &&
            m_paintSet != PaintCatalog::kPearl)
            return;
        if (CyclePaintFinish(msg == kMsgPadRTrigger ? 1 : -1)) {
            RefreshPaintSwatches(pkg);
            m_fe->SetPreviewPaint(m_paintSet, m_paintSel + 1);
        }
        return;
    }
    const size_t count = m_fe->Paint().Count(m_paintSet);
    if (!count)
        return;
    if (msg == kMsgPadLeft || msg == kMsgPadRight) {
        const int row = int(m_paintSel / 20);
        int lo, hi;
        PaintRowRange(row, count, lo, hi);
        int col = PaintGridCol(m_paintSel);
        if (msg == kMsgPadLeft)
            col = col > lo ? col - 1 : hi;
        else
            col = col < hi ? col + 1 : lo;
        m_paintSel = uint32_t(PaintGridIndex(row, col));
    } else if (msg == kMsgPadUp || msg == kMsgPadDown) {
        const int rows = PaintGridRows(count);
        const int row = int(m_paintSel / 20);
        int col = PaintGridCol(m_paintSel);
        const int next = (row + (msg == kMsgPadUp ? -1 : 1) + rows) % rows;
        int lo, hi;
        PaintRowRange(next, count, lo, hi);
        col = std::clamp(col, lo, hi);
        m_paintSel = uint32_t(PaintGridIndex(next, col));
    } else if ((msg == kMsgButtonPressed || msg == kMsgPadAccept) && !m_navPending) {
        // 0x002bc2f0 applies the part colour and 0x002be150(0) closes the grid back to the cards.
        ApplyPaint(m_paintPart, m_paintSet, m_paintSel + 1);
        LeavePaintGrid(pkg, false);
        return;
    } else if (msg == kMsgPadBack && !m_navPending) {
        // 0x002be150(1): leave the grid and drop its temporary colour.
        LeavePaintGrid(pkg, true);
        return;
    } else {
        return;
    }
    // The original previews the part colour while the swatch moves (0x002bc2f0 -> 0x002bdf10).
    if (m_paintPart == 0)
        m_fe->SetPreviewPaint(m_paintSet, m_paintSel + 1);
    else if (m_paintPart == 4 || m_paintPart == 5)
        m_fe->SetPreviewRimPaint(m_paintSet, m_paintSel + 1);
    else if (m_paintPart == 6)
        m_fe->SetPreviewCaliperPaint(m_paintSet, m_paintSel + 1);
    else if (m_paintPart == 3)
        m_fe->SetPreviewMirrorPaint(m_paintSet, m_paintSel + 1);
    else if (m_paintPart == 1)
        m_fe->SetPreviewSpoilerPaint(m_paintSet, m_paintSel + 1);
    else if (m_paintPart == 2) // Roof Scoop
        m_fe->SetPreviewRoofPaint(m_paintSet, m_paintSel + 1);
    else if (m_paintPart == 7) // Engine Accents
        m_fe->SetPreviewEnginePaint(m_paintSet, m_paintSel + 1);
    else if (m_paintPart == 8) // Muffler Tip
        m_fe->SetPreviewExhaustPaint(m_paintSet, m_paintSel + 1);
    PositionPaintHighlight(pkg);
    RefreshPaintLabel(pkg);
    RefreshPaintCost(pkg);
}

bool FEStartupSequence::CyclePaintFinish(int direction)
{
    // 0x002be5f0: R1 walks GLOSS -> METAL -> PEARL -> GLOSS, L1 walks the other way.  0x002be4d8
    // (the "is this finish available" test) is a constant 1 in this build, so every present set is
    // reachable; an empty set is still skipped here in case the pack is partial.
    static constexpr uint32_t kFinishes[] = {PaintCatalog::kGloss, PaintCatalog::kMetal,
                                             PaintCatalog::kPearl};
    constexpr int kCount = int(sizeof(kFinishes) / sizeof(kFinishes[0]));
    int current = -1;
    for (int i = 0; i < kCount; ++i)
        if (kFinishes[i] == m_paintSet) {
            current = i;
            break;
        }
    if (current < 0)
        return false;
    for (int step = 1; step <= kCount; ++step) {
        const uint32_t next = kFinishes[(current + direction * step + kCount * 2) % kCount];
        if (!m_fe->Paint().Has(next))
            continue;
        if (next == m_paintSet)
            return false;
        m_paintSet = next;
        // 0x002bd868 re-fills the grid and selects the equipped color when the new set contains it;
        // a different finish does not, so the first swatch of the new set is current.
        m_paintSel = 0;
        m_fe->SetPreviewPaint(m_paintSet, 1);
        printf("[Paint] finish %s\n", PaintCatalog::SetName(m_paintSet));
        return true;
    }
    return false;
}

void FEStartupSequence::ApplyPaint(size_t part, uint32_t set, uint32_t index)
{
    if (part >= kPaintPartCount)
        return;
    m_partSet[part] = set;
    m_partSel[part] = index > 0 ? index - 1 : 0;
    m_partPaintIndex[part] = index;
    if (part == 0)
        m_fe->SetPreviewPaint(set, index); // Base Paint tints the body
    else if (part == 4 || part == 5)
        m_fe->SetPreviewRimPaint(set, index); // Rims/Spinners tint the wheel faces (#172)
    else if (part == 6)
        m_fe->SetPreviewCaliperPaint(set, index); // Brakes tint the calipers (#173)
    else if (part == 3)
        m_fe->SetPreviewMirrorPaint(set, index); // Mirrors tint the housings (#174)
    else if (part == 1)
        m_fe->SetPreviewSpoilerPaint(set, index); // Spoiler tints the stock wing (#175)
    else if (part == 2)
        m_fe->SetPreviewRoofPaint(set, index); // Roof Scoop tints the installed scoop (#178)
    else if (part == 7)
        m_fe->SetPreviewEnginePaint(set, index); // Engine Accents tint the installed accents (#178)
    else if (part == 8)
        m_fe->SetPreviewExhaustPaint(set, index); // Muffler Tip tints the installed tip (#178)

    if (m_careerExists && m_profile.currentCar >= 0 &&
        m_profile.currentCar < int(m_profile.cars.size())) {
        Career::OwnedCar &car = m_profile.cars[size_t(m_profile.currentCar)];
        if (car.model == m_fe->PreviewCarName()) {
            if (part == 0) {
                car.paint = index;
                car.paintSet = set;
            } else {
                car.partPaint[part - 1] = index;
                car.partPaintSet[part - 1] = set;
            }
            if (Career::SaveCareer(m_profile))
                printf("[Paint] %s %s %u saved on %s\n", kPaintParts[part].debug,
                       PaintCatalog::SetName(set), index, car.model.c_str());
            else
                printf("[Paint] %s %s %u applied, save failed\n", kPaintParts[part].debug,
                       PaintCatalog::SetName(set), index);
            return;
        }
    }
    printf("[Paint] %s %s %u applied for this session\n", kPaintParts[part].debug,
           PaintCatalog::SetName(set), index);
}

bool FEStartupSequence::PopScreen()
{
    if (m_backStack.empty())
        return false;
    std::string prev = m_backStack.back();
    m_backStack.pop_back();
    printf("[Menu] back to %s\n", prev.c_str());
    m_navPending = true;
    m_engine->QueuePackageSwitch(prev.c_str(), 0xff);
    return true;
}

// UI_StartCareer (0x001e7168 / ctor 0x001e71f0): Career Mode starting screen.
// An icon carousel with 3 options:
//   Resume Career (tex 0xa230cd88, label 0xe072db21)
//   New Career (tex 0x3cb92e26, label 0x97b1dcda)
//   Career Garage (tex 0x72c87f3c, label 0xaa3f09d8)
void FEStartupSequence::HandleStartCareerLoaded(FEPackage *pkg)
{
    // 0x001e71f0: Screen title "CAREER" (0x5e7e1192)
    FENativeInterface::SetLanguageHash(pkg, 0x42adb44c, 0x5e7e1192);

    static const struct {
        uint32_t tex, label;
        int action; // 0 Resume, 1 New, 2 Garage
    } kOptions[] = {
        {0xa230cd88, 0xe072db21, 0}, // Resume Career
        {0x3cb92e26, 0x97b1dcda, 1}, // New Career
        {0x72c87f3c, 0xaa3f09d8, 2}, // Career Garage
    };
    // Option list per the constructor 0x001e71f0: with a saved career (DAT_004f0e70 != 0) the first
    // option is Resume Career and New Career follows; without one New Career comes first and there is
    // no Resume. Career Garage is always last. DAT_004f0e70 is the career-present flag; the port
    // derives it from the memory card (Career::CareerExists), so a save survives a restart.
    m_careerExists = Career::CareerExists();
    std::vector<FEIconScroller::Def> defs;
    m_careerActions.clear();
    for (const auto &o : kOptions) {
        if (o.action == 0 && !m_careerExists)
            continue;
        defs.push_back({o.tex, o.label});
        m_careerActions.push_back(o.action);
    }
    printf("[Menu] StartCareer: %zu options (career %s)\n", defs.size(), m_careerExists ? "exists" : "none");
    m_scroller.Setup(pkg, defs, m_startCareerLastSelection);
}

void FEStartupSequence::HandleStartCareerMessage(uint32_t msg, FEPackage *pkg)
{
    if (m_scroller.Empty() || m_scroller.Package() != pkg)
        return;
    if (msg == kMsgScreenTick) {
        m_scroller.HandleMessage(msg, TickSeconds());
        return;
    }
    if (msg == kMsgInitComplete) {
        m_scroller.HandleMessage(msg, 0.0f);
        m_scroller.AllowFade();
        return;
    }
    if (msg == kMsgPadLeft || msg == kMsgPadRight) {
        m_scroller.HandleMessage(msg, 0.0f);
        return;
    }
    if (msg == kMsgPadBack && !m_navPending) {
        NavigateTo(pkg->GetName(), "UI_Main.fng");
        return;
    }
    if ((msg == kMsgButtonPressed || msg == kMsgPadAccept) && !m_navPending) {
        size_t sel = m_scroller.Selected();
        m_startCareerLastSelection = sel;
        if (sel >= m_careerActions.size())
            return;
        int action = m_careerActions[sel];
        printf("[Career] action %d selected\n", action);
        if (action == 0) {
            // Resume Career -> 0x00259300 (career state 0xdd60e403 present): load through
            // 0x00265970 / 0x0025e388 -> enter the world. The career opens in the crib (UI_CareerCrib);
            // the port enters the city free roam (L4RA), as before.
            ResumeCareer();
        } else if (action == 1) {
            // New Career -> 0x00259328 (vtable 0x470618): MC_Main.fng with mode 0x21961 - create the
            // profile save on the memory card, then career creation.
            m_memcardNext = "UI_QRCarSelect.fng";
            m_memcardCreate = true;
            NavigateTo(pkg->GetName(), "MC_Main.fng");
        } else if (action == 2) {
            // Career Garage -> 0x002592c0: MC_Main.fng with mode 0xc11 - pick the profile, then
            // GarageMain.fng.
            m_memcardNext = "GarageMain.fng";
            NavigateTo(pkg->GetName(), "MC_Main.fng");
        }
        return;
    }
}

// UI_CareerCrib.fng (factory 0x001e74d0, ctor 0x001e7508, AddOptions 0x001e79d8): the career hub.
// The original opens it as part of the world (0x00265970 / 0x0025e388 load the crib region, then the
// package is the menu over the 3D crib), so the port pushes it over a career free-roam session
// (main.cpp) instead of making it a front-end screen. AddOption order, texture and label hashes:
//   Return to Explore Mode  0xa230cd88 / 0x194efa7a  -> close the crib and drive
//   World Map               0x22f2ceab / 0x9c07ca52  -> only when 1 < DAT_004f022c
//   Car Select              0xa964095b / 0x451879e2
//   Change Parts            0x511f9d75 / 0xabb24bbc
//   Performance Tuning/Dyno 0x7a9b397a / 0x8513f2c0
//   View Signed Sponsors    0x0878ffdf / 0xe0004602  -> +0x24 = (0x00267920(0x4e8710) == 0): disabled
//   Save                    0x8ada6fd4 / 0x03ec7395
//   Back to Main Menu       0xfc39853f / 0x046e7d84
// DAT_004f022c is the career area the world map filters its event list by (0x001ee590 reads it);
// the port does not model it yet, so World Map is always offered. The sponsor check has no port yet
// either, so that option reports that no sponsor is signed instead of doing nothing silently.
void FEStartupSequence::HandleCareerCribLoaded(FEPackage *pkg)
{
    static const struct {
        uint32_t tex, label;
        int action; // 0 explore, 1 world map, 2 car select, 3 parts, 4 tuning, 5 sponsors, 6 save, 7 exit
    } kOptions[] = {
        {0xa230cd88, 0x194efa7a, 0}, {0x22f2ceab, 0x9c07ca52, 1}, {0xa964095b, 0x451879e2, 2},
        {0x511f9d75, 0xabb24bbc, 3}, {0x7a9b397a, 0x8513f2c0, 4}, {0x0878ffdf, 0xe0004602, 5},
        {0x8ada6fd4, 0x03ec7395, 6}, {0xfc39853f, 0x046e7d84, 7},
    };
    std::vector<FEIconScroller::Def> defs;
    m_cribActions.clear();
    for (const auto &o : kOptions) {
        // View Signed Sponsors is greyed while 0x00267920(0x4e8710) == 0 (no sponsor signed).
        defs.push_back({o.tex, o.label, o.action == 5 && m_profile.SignedSponsorCount() == 0});
        m_cribActions.push_back(o.action);
    }
    printf("[Career] crib: %zu options\n", defs.size());
    m_scroller.Setup(pkg, defs, m_cribLastSelection);
}

void FEStartupSequence::HandleCareerCribMessage(uint32_t msg, FEPackage *pkg)
{
    if (m_scroller.Empty() || m_scroller.Package() != pkg)
        return;
    if (msg == kMsgScreenTick) {
        m_scroller.HandleMessage(msg, TickSeconds());
        return;
    }
    if (msg == kMsgInitComplete) {
        m_scroller.HandleMessage(msg, 0.0f);
        m_scroller.AllowFade();
        return;
    }
    if (msg == kMsgPadLeft || msg == kMsgPadRight) {
        m_scroller.HandleMessage(msg, 0.0f);
        return;
    }
    if (msg == kMsgPadBack) {
        printf("[Career] crib: back to main menu\n");
        m_pauseAction = kPauseQuit;
        return;
    }
    if ((msg == kMsgButtonPressed || msg == kMsgPadAccept) && !m_navPending) {
        size_t sel = m_scroller.Selected();
        m_cribLastSelection = sel;
        if (sel >= m_cribActions.size())
            return;
        switch (m_cribActions[sel]) {
        case 0:
            printf("[Career] crib: return to explore mode\n");
            m_pauseAction = kPauseResume; // main.cpp unloads the crib
            break;
        case 1:
            printf("[Career] crib: world map\n");
            m_careerMenuSwitch = "UI_CareerWorldMap.fng";
            break;
        case 2:
            printf("[Career] crib: car select\n");
            m_careerMenuSwitch = "UI_CareerCarSelect.fng";
            break;
        case 3:
            printf("[Career] crib: change parts (not ported)\n");
            break;
        case 4:
            printf("[Career] crib: performance tuning (not ported)\n");
            break;
        case 5:
            printf("[Career] crib: signed sponsors (%d)\n", m_profile.SignedSponsorCount());
            if (m_profile.SignedSponsorCount() > 0)
                m_careerMenuSwitch = "UI_RewardsSponsor.fng";
            break;
        case 6:
            if (m_profile.valid && Career::SaveCareer(m_profile))
                printf("[Career] crib: saved (money %d, respect %d)\n", m_profile.money, m_profile.respect);
            else
                printf("[Career] crib: save failed\n");
            break;
        case 7:
            printf("[Career] crib: back to main menu\n");
            m_pauseAction = kPauseQuit;
            break;
        }
        return;
    }
    m_scroller.HandleMessage(msg, TickSeconds());
}

// UI_CareerWorldMap.fng (create 0x001ee0b8, ctor 0x001ee0f0, event list 0x001ee590, messages
// 0x001eed48): the career event picker. Up/Down (0x001eea80) move through the area's events, Left/
// Right (0x001eeb58) change the area, Accept confirms and starts the event (0x00267d40 selects it,
// 0x0025eba8 starts it), Back returns to the crib (0x00292560 -> UI_CareerCrib.fng).
// Stand-ins: the Accept confirmation dialog (0x00260528) is skipped and the per-track map image
// (TRACKS\TRACKMAP<n>.bin, 0x001fa010) is not available, so the package's default map stays. The
// area/event lists follow the #152 stage gates; the fourth row is the best time (0x00272e18) or,
// for drift events, the best score (0x00272ee8) from the profile's 0x4fb4ac table (#144/#162).
static uint32_t RaceTypeLabel(int type)
{
    switch (type) {
    case 1: return 0xdcb9f861;  // Sprint
    case 2: return 0x35102edf;  // Drag
    case 3: return 0xd7162cfa;  // Drift
    case 4: return 0x75f903f4;  // Underground Racing League
    case 10: return 0x30da61e1; // Street X
    default: return 0x93fdee74; // Circuit
    }
}

// The race type of a career event, from the event record as 0x0025e850 (StartRace's career setup)
// reads it: +0x34 == 2 is an Underground Racing League event; otherwise +0x0f (the Event::stage
// field) is the event type: 0 circuit, 1 sprint, 2 street X, 3 outrun (the tracks flagged bit 4),
// 4 drag, 5 drift (the world map scores 5 by points, 0x00272ee8). The switch at 0x0025e850 maps
// 4 -> drag, 5 -> drift, 2 -> street X; all 224 records agree with their first track's
// supported-type flags (circuit 0x71, sprint 0x34, street X 0x800, outrun 0x10, drag 0x2,
// drift 0x8, URL 0x2010).
static int CareerRaceType(const Career::Event &ev, const RaceInfo &)
{
    if (ev.kind == 2)
        return 4;
    switch (ev.stage) {
    case 1: return 1;
    case 2: return 10;
    case 3: return 8;
    case 4: return 2;
    case 5: return 3;
    default: return 0;
    }
}

void FEStartupSequence::HandleCareerWorldMapLoaded(FEPackage *pkg)
{
    m_wmEvents.clear();
    m_wmSel = 0;
    const Career::EventTable &table = Career::EventTable::Get();
    for (size_t i = 0; i < table.Count() && m_wmEvents.size() < 60; ++i) {
        const Career::Event &ev = table.At(i);
        if (ev.region != m_wmArea && !(m_wmArea == 1 && ev.region == 0))
            continue;
        if (ev.races.empty())
            continue;
        const RaceInfo *r = nullptr;
        for (const RaceInfo &ri : m_fe->Races())
            if (ri.number == ev.races[0])
                r = &ri;
        if (!r)
            continue;
        if (ev.id - 0xdd60e402u < 2u) // 0x001ee590 skips these two ids
            continue;
        if (!getenv("NFSU2_UNLOCK_ALL") && !table.EventAvailable(ev, m_profile.completedEvents))
            continue; // its stage is not cleared yet (ISSUES #152)
        m_wmEvents.push_back(&ev);
    }
    printf("[Career] world map: area %d, %zu events\n", m_wmArea, m_wmEvents.size());
    RefreshCareerWorldMap(pkg);
}

void FEStartupSequence::RefreshCareerWorldMap(FEPackage *pkg)
{
    char text[48];
    snprintf(text, sizeof(text), "Region %d", m_wmArea);
    FENativeInterface::SetString(pkg, 0xc8e97c9c, text);
    FENativeInterface::SetVisible(pkg, 0x444969fd, m_wmArea > 1); // area arrows (0x001eec40)
    FENativeInterface::SetVisible(pkg, 0x444969fe,
                                  m_wmArea < 5 && (getenv("NFSU2_UNLOCK_ALL") ||
                                                   Career::EventTable::Get().AreaAvailable(m_wmArea + 1, m_profile.completedEvents)));
    FENativeInterface::SetVisible(pkg, 0x640f0f24, m_wmSel > 0); // list arrows
    FENativeInterface::SetVisible(pkg, 0x640f0f23, m_wmSel + 1 < m_wmEvents.size());
    // 0x001eea40 / 0x001991c0: the scroll handle follows the selection between the two arrows.
    const float t = m_wmEvents.size() > 1 ? float(m_wmSel) / float(m_wmEvents.size() - 1) : 0.0f;
    for (uint32_t h : {0x38e39ad7u, 0xda3c4771u}) { // handle icon and its selection frame
        if (FEObject *handle = pkg->FindObjectByHash(h)) {
            float hx, hy;
            FENativeInterface::GetCenter(handle, hx, hy);
            FENativeInterface::SetCenter(handle, hx, -98.0f + t * 241.0f);
        }
    }
    if (m_wmEvents.empty()) {
        for (uint32_t h : {0x23eae8b9u, 0x23eae8bau, 0x23eae8bbu, 0x23eae8bcu})
            FENativeInterface::SetString(pkg, h, "");
        return;
    }
    const Career::Event &ev = *m_wmEvents[m_wmSel];
    const RaceInfo *r = nullptr;
    for (const RaceInfo &ri : m_fe->Races())
        if (ri.number == ev.races[0])
            r = &ri;
    const bool done = m_profile.valid && m_profile.Completed(ev.id);
    // 0x001edcb0 sets the four row captions too; the package ships "Race Status:" and
    // "Previous Time:". Drift events (type 5) read the shared 0x4fb4ac table with 0x00272ee8 and
    // caption it "Best Score:"; sprint and drag (types 1/4) call their time "Best Time:".
    FENativeInterface::SetLanguageHash(pkg, 0x4d2f5c61, 0xed8ca191); // Event Status
    FENativeInterface::SetLanguageHash(pkg, 0x4d2f5c62, 0x7f904ae8); // Race Type:
    FENativeInterface::SetLanguageHash(pkg, 0x4d2f5c63, 0x7019add7); // Distance:
    const uint32_t bestLabel = ev.stage == 5 ? 0x4e0a36a0u
                               : (ev.stage == 1 || ev.stage == 4) ? 0xdb0c5efdu
                                                                 : 0x3b4a1ecbu;
    FENativeInterface::SetLanguageHash(pkg, 0x4d2f5c64, bestLabel);
    FENativeInterface::SetLanguageHash(pkg, 0x23eae8b9, done ? 0xb1ee4ca9 : 0xd11ecd79); // Completed / Not Completed
    if (r) {
        FENativeInterface::SetLanguageHash(pkg, 0x23eae8ba, RaceTypeLabel(CareerRaceType(ev, *r)));
        snprintf(text, sizeof(text), "%.1f Miles", r->length * 0.001f * 0.62137f);
        FENativeInterface::SetString(pkg, 0x23eae8bb, text);
    }
    if (ev.stage == 5) {
        // 0x00272ee8: largest score for this race/direction; no entry reads as 0.
        const int score = r ? m_records.BestScoreFor(r->number, ev.DirectionFor(r->number)) : 0;
        snprintf(text, sizeof(text), "%d", score);
    } else {
        const int best = r ? m_records.BestTimeFor(r->number, ev.DirectionFor(r->number)) : 0;
        FormatBestTime(best, text, sizeof(text));
    }
    FENativeInterface::SetString(pkg, 0x23eae8bc, text);
}

void FEStartupSequence::StartCareerEvent(const Career::Event &ev)
{
    const RaceInfo *r = nullptr;
    for (const RaceInfo &ri : m_fe->Races())
        if (ri.number == ev.races[0])
            r = &ri;
    if (!r)
        return;
    m_raceNumber = r->number;
    m_raceType = CareerRaceType(ev, *r);
    m_careerEventRun = true;
    m_careerReverse = ev.DirectionFor(r->number) != 0;
    printf("[Career] start event %08x: race %d type %d purse %u direction %s\n", ev.id, m_raceNumber, m_raceType,
           ev.purse, m_careerReverse ? "Reverse" : "Forward");
    RequestRace();
}

void FEStartupSequence::HandleCareerWorldMapMessage(uint32_t msg, FEPackage *pkg)
{
    if (msg == kMsgPadBack) {
        printf("[Career] world map: back to the crib\n");
        m_careerMenuSwitch = "UI_CareerCrib.fng";
        return;
    }
    if (msg == kMsgPadUp || msg == kMsgPadDown) {
        const size_t old = m_wmSel;
        if (msg == kMsgPadUp && m_wmSel > 0)
            --m_wmSel;
        if (msg == kMsgPadDown && m_wmSel + 1 < m_wmEvents.size())
            ++m_wmSel;
        if (old != m_wmSel)
            RefreshCareerWorldMap(pkg);
        return;
    }
    if (msg == kMsgPadLeft || msg == kMsgPadRight) {
        const int area = m_wmArea + (msg == kMsgPadLeft ? -1 : 1);
        if (area >= 1 && area <= 5 &&
            (getenv("NFSU2_UNLOCK_ALL") || Career::EventTable::Get().AreaAvailable(area, m_profile.completedEvents))) {
            m_wmArea = area;
            HandleCareerWorldMapLoaded(pkg);
        }
        return;
    }
    if ((msg == kMsgButtonPressed || msg == kMsgPadAccept) && !m_wmEvents.empty()) {
        StartCareerEvent(*m_wmEvents[m_wmSel]);
        return;
    }
}


// UI_CareerCarSelect.fng (create 0x001e9ef8, ctor 0x001e9f30, messages 0x001ea0c0, slot list
// 0x001ead20, refresh 0x001ea938): the garage car picker. 0x001ead20 builds one slot per car in the
// player's save block (+ locked/sponsor slots); Left/Right (0x9120409e / 0xb5971bf1 -> 0x001eb008)
// move the slot, the slot caption is "Slot %03d" (0x6d221aa2), the stat bars are the accel /
// top_speed / handling bars of the selected car, and Accept asks to switch (0x00260528 /
// 0x002602c8 dialogs); Back returns to the crib (0xe1fde1d1 -> 0x00292560 UI_CareerCrib.fng).
// Stand-ins: no confirmation dialog, no trade-in value (0x2f5e7da3 stays 00000), no sponsor/locked
// slots (only owned cars), no L1/R1 filter. The switch re-enters the crib session so the world
// builds the player car from the new model (the world is not rebuilt in place).
void FEStartupSequence::HandleCareerCarSelectLoaded(FEPackage *pkg)
{
    m_csSel = size_t(std::max(0, m_profile.currentCar));
    if (m_csSel >= m_profile.cars.size())
        m_csSel = 0;
    RefreshCareerCarSelect(pkg);
}

void FEStartupSequence::RefreshCareerCarSelect(FEPackage *pkg)
{
    char text[32];
    snprintf(text, sizeof(text), "Slot %03zu", m_csSel + 1);
    FENativeInterface::SetString(pkg, 0x6d221aa2, text);
    const bool have = m_csSel < m_profile.cars.size();
    FENativeInterface::SetVisible(pkg, 0xba5709c8, !have); // "Open for sponsor / Locked"
    FENativeInterface::SetVisible(pkg, 0x26f1f64b, m_csSel > 0);
    FENativeInterface::SetVisible(pkg, 0x26f1f64c, m_csSel + 1 < m_profile.cars.size());
    if (have) {
        ApplyOwnedCarPreview(m_profile.cars[m_csSel]);
        m_fe->UpdateStatBars(pkg, true);
    }
}

// The car-select preview shows the same body colour the race will use.  The paint index is part of
// the career save (OwnedCar::paint, #167), so a saved paint comes back with the car.
void FEStartupSequence::ApplyOwnedCarPreview(const Career::OwnedCar &car)
{
    // Set the fitted parts before the car so their geometry is assembled in the same load
    // (#185/#186/#188).
    m_fe->SetPreviewSpoilerPart(car.spoiler, car.spoilerCarbon);
    m_fe->SetPreviewRoofPart(car.roof, car.roofDual, car.roofOffset, car.roofCarbon);
    m_fe->SetPreviewExhaustPart(car.exhaust);
    m_fe->SetPreviewHoodPart(car.hood, car.hoodCarbon);
    m_fe->SetPreviewHeadlightPart(car.headlight);
    m_fe->SetPreviewTaillightPart(car.taillight);
    m_fe->SetPreviewFrontBumperPart(car.frontBumper);
    m_fe->SetPreviewRearBumperPart(car.rearBumper);
    m_fe->SetPreviewSkirtPart(car.skirt);
    m_fe->SetPreviewWideBodyPart(car.wideBody);
    m_fe->SetPreviewPerfParts(car.perfParts);
    m_fe->SetPreviewTune(car.tune);
    m_fe->SetPreviewCar(car.model);
    m_fe->SetPreviewPaint(car.paintSet ? car.paintSet : PaintCatalog::kGloss, car.paint);
    // Rims (partPaint[3]) drive the wheel faces; if only Spinners (partPaint[4]) are painted, use
    // that colour, matching the shared wheel mesh in the port (#172).
    m_fe->SetPreviewRimPaint(car.partPaintSet[3] ? car.partPaintSet[3] : PaintCatalog::kRims,
                             car.partPaint[3] ? car.partPaint[3] : car.partPaint[4]);
    m_fe->SetPreviewCaliperPaint(car.partPaintSet[5] ? car.partPaintSet[5] : PaintCatalog::kBrakes,
                                 car.partPaint[5]);
    m_fe->SetPreviewMirrorPaint(car.partPaintSet[2] ? car.partPaintSet[2] : PaintCatalog::kGloss,
                                car.partPaint[2]);
    m_fe->SetPreviewSpoilerPaint(car.partPaintSet[0] ? car.partPaintSet[0] : PaintCatalog::kGloss,
                                 car.partPaint[0]);
    m_fe->SetPreviewRoofPaint(car.partPaintSet[1] ? car.partPaintSet[1] : PaintCatalog::kGloss,
                              car.partPaint[1]);
    m_fe->SetPreviewEnginePaint(car.partPaintSet[6] ? car.partPaintSet[6] : PaintCatalog::kHoses,
                                car.partPaint[6]);
    m_fe->SetPreviewExhaustPaint(car.partPaintSet[7] ? car.partPaintSet[7] : PaintCatalog::kMufflers,
                                 car.partPaint[7]);
}

void FEStartupSequence::HandleCareerCarSelectMessage(uint32_t msg, FEPackage *pkg)
{
    if (msg == kMsgPadBack) {
        if (const Career::OwnedCar *car = m_profile.CurrentCar())
            ApplyOwnedCarPreview(*car); // leaving without a choice keeps the current car
        printf("[Career] car select: back to the crib\n");
        m_careerMenuSwitch = "UI_CareerCrib.fng";
        return;
    }
    if (msg == kMsgPadLeft || msg == kMsgPadRight) {
        const size_t old = m_csSel;
        if (msg == kMsgPadLeft && m_csSel > 0)
            --m_csSel;
        if (msg == kMsgPadRight && m_csSel + 1 < m_profile.cars.size())
            ++m_csSel;
        if (old != m_csSel)
            RefreshCareerCarSelect(pkg);
        return;
    }
    if ((msg == kMsgButtonPressed || msg == kMsgPadAccept) && m_csSel < m_profile.cars.size()) {
        if (int(m_csSel) == m_profile.currentCar) {
            printf("[Career] car select: %s is already the current car\n", m_profile.cars[m_csSel].model.c_str());
            return;
        }
        m_profile.currentCar = int(m_csSel);
        Career::SaveCareer(m_profile);
        printf("[Career] car select: now driving %s (slot %zu)\n", m_profile.cars[m_csSel].model.c_str(),
               m_csSel + 1);
        // Rebuild the crib session around the new car.
        m_raceNumber = 0;
        m_raceType = 9;
        m_cribRequested = true;
        RequestRace();
    }
}

// UI_RewardsSponsor.fng (create 0x001e8230, ctor 0x001e8268, scroller fill 0x001e8520, per-sponsor fill
// 0x001e8658): the crib's "View Signed Sponsors". 0x001e8520 adds one scroller option per area object
// (areas 0..DAT_004f022c) whose +0xe8 sponsor is set, with the texture DECAL_ICON_<name>. The selected
// sponsor fills: the name (SPONSOR_<name>, f13f76b4), SPONSOR_INFO_<name> (f13cef3f), the signing
// bonus (f25b6bba, record +0xc), the sponsor race purse (195d8d22, record +2), "Each URL Victory"
// (7d8e1791, 0x00267958: the area's first kind-2 event purse) and the TERMS_<n> lines from
// SPONSOR_DETAILS_<name> (0x001e81d0). Stand-ins: the required-race list and bonuses panels keep
// their placeholders (their data is not decoded); Back returns to the crib.
void FEStartupSequence::HandleSponsorViewLoaded(FEPackage *pkg)
{
    m_svAreas.clear();
    std::vector<FEIconScroller::Def> defs;
    for (int a = 0; a < Career::Profile::kAreas; ++a) {
        const Career::Sponsor *sp = Career::SponsorTable::Get().Find(m_profile.signedSponsors[a]);
        if (!sp)
            continue;
        m_svAreas.push_back(a);
        defs.push_back({CarMaterial::NameHash(("DECAL_ICON_" + sp->name).c_str()),
                        CarMaterial::NameHash(("SPONSOR_" + sp->name).c_str())});
    }
    m_scroller.Setup(pkg, defs, 0);
    FENativeInterface::SetString(pkg, 0xeb406fec, m_profile.name.c_str()); // the package's PLAYER NAME
    RefreshSponsorView(pkg);
}

void FEStartupSequence::RefreshSponsorView(FEPackage *pkg)
{
    if (m_svAreas.empty())
        return;
    const size_t sel = std::min(m_scroller.Selected(), m_svAreas.size() - 1);
    const int area = m_svAreas[sel];
    const Career::Sponsor *sp = Career::SponsorTable::Get().Find(m_profile.signedSponsors[area]);
    if (!sp)
        return;
    int urlPurse = 0;
    for (const Career::Event &e : Career::EventTable::Get().All())
        if (e.region == area && e.kind == 2) {
            urlPurse = int(e.purse);
            break;
        }
    char text[32];
    snprintf(text, sizeof(text), "%d", sp->signingBonus);
    FENativeInterface::SetString(pkg, 0xf25b6bba, text);
    snprintf(text, sizeof(text), "%d", sp->racePurse);
    FENativeInterface::SetString(pkg, 0x195d8d22, text);
    snprintf(text, sizeof(text), "%d", urlPurse);
    FENativeInterface::SetString(pkg, 0x7d8e1791, text);
    FENativeInterface::SetLanguageHash(pkg, 0xf13f76b4, CarMaterial::NameHash(("SPONSOR_" + sp->name).c_str()));
    FENativeInterface::SetLanguageHash(pkg, 0xf13cef3f,
                                       CarMaterial::NameHash(("SPONSOR_INFO_" + sp->name).c_str()));
    // 0x001e81d0 / 0x00262eb8: SPONSOR_DETAILS_<name> split into the TERMS_n lines (explicit line
    // breaks, then wrapped to the panel width); Up/Down scroll them.
    m_svLines.clear();
    const std::string details = m_fe->LanguageText(CarMaterial::NameHash(("SPONSOR_DETAILS_" + sp->name).c_str()));
    constexpr size_t kWrap = 36;
    for (size_t pos = 0; pos <= details.size();) {
        size_t nl = details.find('^', pos); // EA's line-break marker
        std::string para = details.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? details.size() + 1 : nl + 1;
        while (para.size() > kWrap) {
            size_t cut = para.rfind(' ', kWrap);
            if (cut == std::string::npos || cut == 0)
                cut = kWrap;
            m_svLines.push_back(para.substr(0, cut));
            para.erase(0, para.find_first_not_of(' ', cut));
        }
        m_svLines.push_back(para);
    }
    m_svScroll = 0;
    if (getenv("NFSU2_FEDEBUG")) {
        printf("[Sponsors] DETAILS: %s\n", details.c_str());
        printf("[Sponsors] INFO: %s\n", m_fe->LanguageText(CarMaterial::NameHash(("SPONSOR_INFO_" + sp->name).c_str())).c_str());
    }
    ShowSponsorTerms(pkg);
    printf("[Career] sponsors: %s (area %d) bonus %d purse %d url %d\n", sp->name.c_str(), area,
           sp->signingBonus, sp->racePurse, urlPurse);
}

void FEStartupSequence::ShowSponsorTerms(FEPackage *pkg)
{
    static const uint32_t kLines[10] = {0xfcc58dda, 0xfcc58ddb, 0xfcc58ddc, 0xfcc58ddd, 0xfcc58dde,
                                        0xfcc58ddf, 0xfcc58de0, 0xfcc58de1, 0xfcc58de2, 0x9577494a};
    for (size_t i = 0; i < 10; ++i) {
        const size_t at = m_svScroll + i;
        FENativeInterface::SetString(pkg, kLines[i], at < m_svLines.size() ? m_svLines[at].c_str() : "");
    }
}

void FEStartupSequence::HandleSponsorViewMessage(uint32_t msg, FEPackage *pkg)
{
    if (m_scroller.Empty() || m_scroller.Package() != pkg)
        return;
    if (msg == kMsgPadBack) {
        printf("[Career] sponsors: back to the crib\n");
        m_careerMenuSwitch = "UI_CareerCrib.fng";
        return;
    }
    if (msg == kMsgPadUp && m_svScroll > 0) {
        --m_svScroll;
        ShowSponsorTerms(pkg);
        return;
    }
    if (msg == kMsgPadDown && m_svScroll + 10 < m_svLines.size()) {
        ++m_svScroll;
        ShowSponsorTerms(pkg);
        return;
    }
    if (msg == kMsgPadLeft || msg == kMsgPadRight) {
        m_scroller.HandleMessage(msg, 0.0f);
        RefreshSponsorView(pkg);
        return;
    }
    if (msg == kMsgScreenTick) {
        m_scroller.HandleMessage(msg, TickSeconds());
        return;
    }
    if (msg == kMsgInitComplete) {
        m_scroller.HandleMessage(msg, 0.0f);
        m_scroller.AllowFade();
        RefreshSponsorView(pkg);
        return;
    }
    m_scroller.HandleMessage(msg, TickSeconds());
}

// Credits.fng (the options carousel's Credits entry; OMCredits / CreditsScreen / CreditsArea strings in the
// executable, text from "CREDITS\%s%s.TXT" = language + region suffix, here CREDITS\UK_ENGLISH.TXT for
// the PAL English build). The package carries one centred template string (0xe6093486 "DEFAULT STRING",
// y 291, below the 360 px panel); the screen repeats it per line and scrolls the lines up the panel.
// The file is UTF-16LE with a BOM, CRLF lines. The port draws the template once per visible line
// (FENativeInterface::StartTextScroll); accented letters outside ASCII are shown as '?' (the game
// maps them through its font conversion table).
void FEStartupSequence::HandleCreditsLoaded(FEPackage *pkg)
{
    std::vector<uint8_t> raw = m_fe->ReadGameFile("CREDITS\\UK_ENGLISH.TXT");
    std::vector<std::string> lines;
    if (raw.size() >= 4 && raw[0] == 0xff && raw[1] == 0xfe) {
        std::string cur;
        for (size_t i = 2; i + 1 < raw.size(); i += 2) {
            const uint16_t c = uint16_t(raw[i] | (raw[i + 1] << 8));
            if (c == '\r')
                continue;
            if (c == '\n') {
                lines.push_back(cur);
                cur.clear();
            } else {
                cur += c < 0x80 ? char(c) : '?';
            }
        }
        if (!cur.empty())
            lines.push_back(cur);
    }
    printf("[Menu] credits: %zu lines\n", lines.size());
    FENativeInterface::SetVisible(pkg, 0xe6093486, true);
    m_fe->StartTextScroll(pkg, 0xe6093486, std::move(lines), 190.0f, 19.0f, 28.0f, -170.0f, 172.0f);
}

// MC_Main.fng (create 0x00225f50, ctor 0x00226720, dispatch 0x00228700): the card status screen that
// the career options open (mode DAT_004e3618: 0x21961 New Career, 0xc11 Career Garage). Its class
// polls the memcard library (0x0023d570 / 0x0023d9b0) and writes the status string; on the screen
// tick it also relabels object 0x47ff4e7c with the "CALL" script (0x00293378 -> 0x00293240) - that is
// the button that opens the file browser (0x00225fb8 pushes MC_List.fng over this screen,
// FUN_002925b0 = push). The port performs the card operations natively and reports the native card.
void FEStartupSequence::HandleMemcardLoaded(FEPackage *pkg)
{
    if (strcasecmp(pkg->GetName(), "MC_List.fng") == 0) {
        FillMemcardList(pkg);
        return;
    }
    // 0x00228700 (the screen-tick branch) relabels object 0x47ff4e7c with the "CALL" text at 0x566640
    // (FUN_00293378 -> 0x00293240) - that is the button that opens the file browser.
    FENativeInterface::SetString(pkg, 0x47ff4e7c, "CALL");
    if (m_memcardCreate) {
        m_memcardCreate = false;
        // 0x00259328 writes the new profile to the card at once; the PS2 asks for its name first
        // (the keyboard, #204).  Cancelling keeps the default name so the flow can continue.
        OpenKeyboard("PLAYER", [this](const std::string &name) { BeginCareer(name); }, [this]() { BeginCareer(); });
    }
    // The real screen polls the card every 0x15 ticks (0x0023d570 / 0x0023d9b0); the port reads it here.
    Career::MemoryCard &card = Career::MemoryCard::Get();
    char status[160];
    snprintf(status, sizeof(status), "Memory card (PS2) in MEMORY CARD slot 1: %d save(s)",
             card.Count());
    FENativeInterface::SetString(pkg, kMcStatusString, status);
    // The package ships six placeholder dialog buttons (Yes / No / Yes ..., hashes 0xb8a7c6cc..d1, in groups that carry
    // their backing bars); the game's state machine shows the ones its prompt needs
    // (0x00228700). The port has no prompt states -- the card work is done natively and Accept continues --
    // so they are hidden instead of overlapping as five stray "Yes"/"No" buttons.
    // Hide a button string and the 215x42 / 42x42 backing images of the button rows (children of groups).
    std::function<void(FEObject *)> hideButtons = [&](FEObject *o) {
        for (; o; o = o->GetNext()) {
            if (o->Type == FE_Group) {
                hideButtons(static_cast<FEGroup *>(o)->GetFirstChild());
                continue;
            }
            const bool button = o->Type == FE_String && o->NameHash >= 0xb8a7c6cc && o->NameHash <= 0xb8a7c6d1;
            const FEObjData *d = o->GetObjData();
            const bool backing = o->Type == FE_Image && d && d->Size.y > 40.0f && d->Size.y < 44.0f &&
                                 d->Size.x > 40.0f && d->Size.x < 220.0f;
            if (button || backing)
                o->Flags |= 1; // FF_Invisible
        }
    };
    hideButtons(pkg->GetFirstObject());
    printf("[Memcard] MC_Main.fng: %s (%s)\n", status, card.SlotDir().c_str());
}

// 0x00228348 / 0x00228990: fill the file rows. UG2 adds a widget per card entry (0x00228990) after
// 0x00226908 filters the list the memcard library built; the port drives the package's own 8 rows.
void FEStartupSequence::FillMemcardList(FEPackage *pkg)
{
    m_memcardFiles = Career::MemoryCard::Get().List();
    m_memcardSel = 0;
    for (size_t i = 0; i < m_memcardFiles.size(); ++i)
        if (!m_memcardPick.empty() && m_memcardFiles[i].name == m_memcardPick)
            m_memcardSel = int(i);
    if (m_memcardSel >= int(m_memcardFiles.size()))
        m_memcardSel = 0;
    ApplyMemcardList(pkg, true);
}

// The dialog's own scripts (FEPackage::UpdateObject -> script tracks) rewrite the rows' colours and
// alphas every frame, so the reveal has to be re-applied after the scripts run; PreRender does that
// (the rows would otherwise reappear with the package's untinted placeholder colours).
void FEStartupSequence::ApplyMemcardList(FEPackage *pkg, bool log)
{
    WhitenGroups(pkg->GetFirstObject());
    for (int i = 0; i < kMcListRows; ++i) {
        const bool used = i < int(m_memcardFiles.size());
        RevealObject(pkg, kMcListRow[i], used);
        RevealObject(pkg, kMcListName[i], used);
        if (!used)
            continue;
        const Career::CardFile &f = m_memcardFiles[size_t(i)];
        FENativeInterface::SetString(pkg, kMcListName[i], f.name.c_str());
        if (FEObject *row = pkg->FindObjectByHash(kMcListRow[i])) {
            if (row->GetObjData())
                row->GetObjData()->Col.a = (i == m_memcardSel) ? 255 : 170; // cursor highlight
            row->Flags |= 0x400000u;
        }
    }
    for (uint32_t h : kMcListFrame)
        RevealObject(pkg, h, true);
    if (log)
        printf("[Memcard] MC_List.fng: %zu save(s), %d selected\n", m_memcardFiles.size(), m_memcardSel);
}

// Called from the front-end loop just before FEngine::Render, i.e. after the script update.
void FEStartupSequence::PreRender()
{
    if (!m_engine || m_memcardFiles.empty())
        return;
    if (FEPackage *pkg = m_engine->GetPackageList()->FindPackage("MC_List.fng"))
        ApplyMemcardList(pkg, false);
}

// 0x00225fb8: push MC_List.fng over MC_Main.fng (FUN_002925b0 is the engine's push).
void FEStartupSequence::OpenMemcardList()
{
    if (m_memcardListOpen)
        return; // already open (the pad message can arrive twice in a frame)
    FEPackage *top = TopScreen(m_engine);
    if (top && top->GetName() && strcasecmp(top->GetName(), "MC_List.fng") == 0)
        return;
    printf("[Memcard] open file list\n");
    m_memcardListOpen = true;
    m_engine->QueuePackagePush("MC_List.fng", 0xff);
}

void FEStartupSequence::ChooseMemcardFile()
{
    if (m_memcardSel >= int(m_memcardFiles.size())) {
        printf("[Memcard] nothing to choose\n");
        return;
    }
    m_memcardPick = m_memcardFiles[size_t(m_memcardSel)].name;
    m_memcardListOpen = false;
    printf("[Memcard] selected %s -> %s\n", m_memcardPick.c_str(), m_memcardNext.c_str());
    // The Profile Manager's Load card (#195): swap the loaded profile and stay on the manager, so
    // its name field and Save card follow the new profile.
    if (m_memcardFromProfileManager) {
        m_memcardFromProfileManager = false;
        if (Career::LoadCareer(m_profile, m_memcardPick)) {
            m_careerExists = true;
            if (const Career::OwnedCar *car = m_profile.CurrentCar())
                ApplyOwnedCarPreview(*car);
            printf("[Profile] loaded %s (money %d, respect %d)\n", m_profile.name.c_str(), m_profile.money,
                   m_profile.respect);
        } else {
            printf("[Profile] %s could not be read\n", m_memcardPick.c_str());
        }
        if (FEPackage *list = m_engine->GetPackageList()->FindPackage("MC_List.fng"))
            m_engine->UnloadPackage(list);
        if (FEPackage *mgr = m_engine->GetPackageList()->FindPackage("UI_ProfileManager.fng"))
            FENativeInterface::SetString(mgr, 0x47ff4e7c, m_profile.valid ? m_profile.name.c_str() : "");
        return;
    }
    // Anything but the New Career flow wants that save loaded (Career Garage opens it).
    if (m_memcardNext != "UI_QRCarSelect.fng" && Career::LoadCareer(m_profile, m_memcardPick)) {
        m_careerExists = true;
        if (const Career::OwnedCar *car = m_profile.CurrentCar())
            ApplyOwnedCarPreview(*car);
    }
    m_navPending = true;
    CloseMemcardScreens();
    NavigateTo("MC_Main.fng", m_memcardNext.c_str());
}

// A switch (QueuePackageSwitch -> FEngine::ProcessPackageCommands 0x002901...; command 3) only unloads
// the package that currently has control, and the card screens are stacked (MC_List.fng was pushed
// over MC_Main.fng, which lost control), so both are removed here before the next screen is queued.
void FEStartupSequence::CloseMemcardScreens()
{
    for (FEPackage *pkg = m_engine->GetPackageList()->GetFirstPackage(); pkg != nullptr;) {
        FEPackage *next = pkg->GetNext();
        if (pkg->GetName() && (strcasecmp(pkg->GetName(), "MC_Main.fng") == 0 ||
                               strcasecmp(pkg->GetName(), "MC_List.fng") == 0))
            m_engine->UnloadPackage(pkg);
        pkg = next;
    }
    m_memcardListOpen = false;
}

void FEStartupSequence::HandleMemcardMessage(uint32_t msg, FEPackage *pkg)
{
    const bool isList = strcasecmp(pkg->GetName(), "MC_List.fng") == 0;
    if (isList) {
        if (msg == kMsgPadUp || msg == kMsgPadDown) {
            const int n = int(m_memcardFiles.size());
            if (n > 0) {
                m_memcardSel = (m_memcardSel + (msg == kMsgPadDown ? 1 : n - 1)) % n;
                FillMemcardList(pkg);
            }
            return;
        }
        if (msg == kMsgPadBack && !m_navPending) {
            printf("[Memcard] file list closed\n");
            m_memcardListOpen = false;
            m_engine->UnloadPackage(pkg);
            return;
        }
        if ((msg == kMsgPadAccept || msg == kMsgButtonPressed) && !m_navPending)
            ChooseMemcardFile();
        return;
    }
    // MC_Main.fng. CloseMemcardScreens frees this package, so keep its name for NavigateTo/back stack.
    if (msg == kMsgPadBack && !m_navPending) {
        const std::string from = pkg->GetName();
        printf("[Memcard] cancelled\n");
        CloseMemcardScreens();
        NavigateTo(from.c_str(), "UI_StartCareer.fng");
        return;
    }
    if ((msg == kMsgPadAccept || msg == kMsgButtonPressed) && !m_navPending)
        OpenMemcardList();
}

// Menus whose option cards are built at runtime (FUN_0029ae78 clones OPTION_MASTER, the 118x59
// card image, once per option) ship no card objects; this makes `count` top-level clones of the
// master, hides the master and returns the clones plus the row centre the scroller should use.
// Empty when the package has no OPTION_MASTER image.
std::vector<FEObject *> CloneOptionCards(FEPackage *pkg, size_t count, const char *prefix, float &centerX,
                                         float &centerY)
{
    std::vector<FEObject *> cards;
    FEObject *art = pkg->FindObjectByHash(bStringHash("OPTION_MASTER"));
    FEImage *artImage = art && art->Type == FE_Image ? static_cast<FEImage *>(art) : nullptr;
    if (!artImage || !artImage->GetImageData())
        return cards;
    float cardW = 118.0f, cardH = 59.0f;
    const FEImageData artData = *artImage->GetImageData();
    FENativeInterface::GetCenter(art, centerX, centerY);
    FENativeInterface::GetSize(art, cardW, cardH);
    FENativeInterface::SetObjectVisible(art, false); // the template; every card is a clone
    for (size_t i = 0; i < count; ++i) {
        // FEObject's copy ctor zeroes the transform data (FEObject.cpp:82), and an FEImage's zero
        // UV rect draws nothing, so the art data is copied over before the transform is set.
        FEImage *card = static_cast<FEImage *>(art->Clone(false));
        FEImageData *data = card ? card->GetImageData() : nullptr;
        if (!data) {
            delete card;
            break;
        }
        *data = artData;
        data->Col.r = data->Col.g = data->Col.b = data->Col.a = 255;
        data->Pos = FEVector3(centerX, centerY, 0.0f);
        data->Size = FEVector3(cardW, cardH, 1.0f);
        char cardName[32];
        snprintf(cardName, sizeof(cardName), "%s%zu", prefix, i + 1);
        card->SetName(cardName);
        pkg->AddObject(card);
        cards.push_back(card);
    }
    return cards;
}

// UI_ChoosePerformancePackage.fng (factory 0x002b6918, ctor 0x002b6950, cards 0x002b78f0, refresh
// 0x002b6fc0): the packages of the chosen category (ISSUES #198).  0x002b78f0 adds, in order, a
// first card (type 0, 0xc7d3f6a7 / 0xdf3fd1d6), one card per level 1..3 (type 1,
// PERFORMANCE_ICON_PACKAGE / PERFORMANCE_PACKAGE_<n>, carrying the package) and a last card
// (type 2, 0x6a93275f / 0xed55c635 = "Performance Individual Parts").  Like the Profile Manager
// the package ships no card objects, so they are clones of OPTION_MASTER.
// A package card lists its parts in PartName_<n> / PartPrice_<n> rows (0x002b7cd8, a part already
// fitted or owned shows PURCHASED_ICON_<n> instead of a price).  0x0026b218 prices it: the sum of
// the parts not yet owned (0x002643f8); with two or more of them 10% comes off (20% for the one
// car type 0x7fc8d0d0) and the "discount" row shows the saving.
void FEStartupSequence::HandlePerformancePackageLoaded(FEPackage *pkg)
{
    if (const char *cat = getenv("NFSU2_PERF_CATEGORY")) // dev aid: open a given category
        m_perfCategory = uint32_t(atoi(cat));
    // Dev aid: NFSU2_PERF_TEST=<money> opens the shop with an in-memory career owning a Skyline
    // (for screens launched with NFSU2_PACKAGE, which have no loaded profile).
    if (const char *money = getenv("NFSU2_PERF_TEST")) {
        if (!m_profile.valid || m_profile.cars.empty()) {
            m_profile.valid = true;
            m_profile.money = atoi(money);
            m_profile.cars.push_back({"SKYLINE", 0});
            m_profile.currentCar = 0;
        }
    }
    struct Card {
        int type;
        uint32_t tex, label, level;
    };
    std::vector<Card> cards;
    cards.push_back({0, 0xc7d3f6a7, 0xdf3fd1d6, 0});
    for (uint32_t level = 1; level < 4; ++level) {
        char name[48];
        snprintf(name, sizeof(name), "PERFORMANCE_PACKAGE_%u", level);
        if (m_fe->PerfParts().Find(m_perfCategory, level))
            cards.push_back({1, bStringHash("PERFORMANCE_ICON_PACKAGE"), bStringHash(name), level});
    }
    cards.push_back({2, 0x6a93275f, 0xed55c635, 0});

    std::vector<FEIconScroller::Def> defs;
    m_perfCardType.clear();
    m_perfCardLevel.clear();
    m_perfCardLabel.clear();
    for (const Card &c : cards) {
        defs.push_back({c.tex, c.label, false});
        m_perfCardType.push_back(c.type);
        m_perfCardLevel.push_back(c.level);
        m_perfCardLabel.push_back(c.label);
    }
    float centerX = 0.0f, centerY = -149.0f;
    m_perfCards = CloneOptionCards(pkg, defs.size(), "PERF_CARD_", centerX, centerY);
    if (m_perfCards.size() != defs.size()) {
        printf("[Customize] performance package: OPTION_MASTER missing\n");
        return;
    }
    m_perfScroller.SetupObjects(pkg, defs, m_perfCards, m_perfCardSel < defs.size() ? m_perfCardSel : 0,
                                "OPTION_MASTER");
    m_perfScroller.SetLayoutCenter(centerX, centerY);
    printf("[Customize] performance package: category %u, %zu cards\n", m_perfCategory, defs.size());
    RefreshPerformancePackage(pkg);
}

// The owned/fitted test of 0x002b5a38: a part is for sale unless it is on the car (0x002b5aa0) or
// in the profile's item pool (0x002b5ac0).  Pool keys carry bit 31 (the Body Shop keys never do).
bool FEStartupSequence::PerfPartAvailable(uint16_t id)
{
    const Career::OwnedCar *car = ShopCar();
    if (car && std::find(car->perfParts.begin(), car->perfParts.end(), id) != car->perfParts.end())
        return false;
    return !HasOwnedPart(m_profile, 0x80000000u | id);
}

// 0x0026b218: {total to pay, saving}.
void FEStartupSequence::PerfPackagePrice(const PerfPackage &pkg, int &total, int &saving)
{
    int sum = 0, count = 0;
    for (const PerfPart &part : pkg.parts)
        if (PerfPartAvailable(uint16_t(part.id))) {
            sum += int(part.price);
            ++count;
        }
    total = sum;
    saving = 0;
    if (count >= 2) {
        total = sum * 90 / 100;
        saving = sum - total;
    }
}

void FEStartupSequence::RefreshPerformancePackage(FEPackage *pkg)
{
    if (m_perfCards.empty())
        return;
    const size_t sel = m_perfScroller.Selected();
    if (sel >= m_perfCardType.size())
        return;
    // Header = the category's label (0x002b7e80), the selected-card caption = the card's label.
    for (const auto &c : kPerfCategories)
        if (c.partId == m_perfCategory)
            FENativeInterface::SetLanguageHash(pkg, 0x42adb44c, c.label);
    FENativeInterface::SetLanguageHash(pkg, 0x0666176b, m_perfCardLabel[sel]);
    const PerfPackage *pp =
        m_perfCardType[sel] == 1 ? m_fe->PerfParts().Find(m_perfCategory, m_perfCardLevel[sel]) : nullptr;
    for (int row = 1; row <= 4; ++row) {
        char name[32];
        snprintf(name, sizeof(name), "PARTNAME_%d", row); // bStringHash is case sensitive
        const uint32_t nameObj = bStringHash(name);
        snprintf(name, sizeof(name), "PARTPRICE_%d", row);
        const uint32_t priceObj = bStringHash(name);
        snprintf(name, sizeof(name), "PURCHASED_ICON_%d", row);
        const uint32_t iconObj = bStringHash(name);
        const PerfPart *part = pp && size_t(row) <= pp->parts.size() ? &pp->parts[size_t(row) - 1] : nullptr;
        FENativeInterface::SetVisible(pkg, nameObj, part != nullptr);
        const bool available = part && PerfPartAvailable(uint16_t(part->id));
        FENativeInterface::SetVisible(pkg, priceObj, available);
        FENativeInterface::SetVisible(pkg, iconObj, part && !available);
        if (part && !available)
            FENativeInterface::SetAlpha(pkg, iconObj, 255); // the checkmark ships at alpha 0
        if (part) {
            FENativeInterface::SetLanguageHash(pkg, nameObj, part->nameHash);
            char price[24];
            snprintf(price, sizeof(price), "$%u", part->price);
            FENativeInterface::SetString(pkg, priceObj, price);
        }
    }
    const bool packageCard = pp != nullptr;
    for (uint32_t obj : {0x06e0c9a8u, 0xbe8693e1u, 0x0380b632u, 0x89ca4fdbu, 0x507e6a40u})
        FENativeInterface::SetVisible(pkg, obj, packageCard);
    if (pp) {
        int total = 0, saving = 0;
        PerfPackagePrice(*pp, total, saving);
        char text[24];
        snprintf(text, sizeof(text), "$%d", saving);
        if (saving > 0)
            FENativeInterface::SetString(pkg, 0xbe8693e1, text);
        else
            FENativeInterface::SetLanguageHash(pkg, 0xbe8693e1, 0xa4c02484);
        snprintf(text, sizeof(text), "$%d", total);
        if (total > 0)
            FENativeInterface::SetString(pkg, 0x0380b632, text);
        else
            FENativeInterface::SetLanguageHash(pkg, 0x0380b632, 0xa4c02484);
        // The label is a printf format ("10%% Discount:"); the original passes it through the
        // formatter, so the doubled percent sign becomes one.
        std::string discount = m_fe->LanguageText(0x6da17a25);
        for (size_t at = discount.find("%%"); at != std::string::npos; at = discount.find("%%", at + 1))
            discount.erase(at, 1);
        FENativeInterface::SetString(pkg, 0x06e0c9a8, discount.c_str());
    }
    // The stat bars: the car as it is, plus the bars for the car with the highlighted package fitted
    // (0x002b7498 fills the "current" and "preview" values from two tuning objects).
    if (pp) {
        std::vector<uint16_t> after;
        if (const Career::OwnedCar *car = ShopCar())
            after = car->perfParts;
        for (uint32_t level = 1; level < 4; ++level)
            if (const PerfPackage *other = m_fe->PerfParts().Find(m_perfCategory, level))
                for (const PerfPart &part : other->parts)
                    after.erase(std::remove(after.begin(), after.end(), uint16_t(part.id)), after.end());
        for (const PerfPart &part : pp->parts)
            after.push_back(uint16_t(part.id));
        m_fe->UpdateStatBars(pkg, true, &after);
    } else {
        m_fe->UpdateStatBars(pkg, true);
    }
    snprintf(m_perfBank, sizeof(m_perfBank), "$%d", m_profile.valid ? m_profile.money : 0);
    FENativeInterface::SetString(pkg, 0xeb3a688a, m_perfBank);
}

void FEStartupSequence::BuyPerformancePackage(FEPackage *pkg)
{
    const size_t sel = m_perfScroller.Selected();
    Career::OwnedCar *car = ShopCar();
    if (sel >= m_perfCardType.size() || m_perfCardType[sel] != 1 || !car)
        return;
    const PerfPackage *pp = m_fe->PerfParts().Find(m_perfCategory, m_perfCardLevel[sel]);
    if (!pp)
        return;
    int total = 0, saving = 0;
    PerfPackagePrice(*pp, total, saving);
    bool anyNew = false;
    for (const PerfPart &part : pp->parts)
        anyNew = anyNew || PerfPartAvailable(uint16_t(part.id));
    if (!anyNew) {
        printf("[PerfShop] package %u/%u is already fitted\n", m_perfCategory, m_perfCardLevel[sel]);
        return;
    }
    if (m_profile.money < total) {
        printf("[PerfShop] package %u/%u costs $%d, career has $%d\n", m_perfCategory, m_perfCardLevel[sel],
               total, m_profile.money);
        ShowMessageDialog(kLabelNotEnoughBank);
        return;
    }
    m_profile.money -= total;
    // A category holds one package at a time: drop the car's parts of this category, then fit
    // this package's (owned parts of other levels stay in the pool for free refitting).
    for (const PerfPart &part : pp->parts)
        AddOwnedPart(m_profile, 0x80000000u | part.id);
    for (uint32_t level = 1; level < 4; ++level)
        if (const PerfPackage *other = m_fe->PerfParts().Find(m_perfCategory, level))
            for (const PerfPart &part : other->parts)
                car->perfParts.erase(std::remove(car->perfParts.begin(), car->perfParts.end(), uint16_t(part.id)),
                                     car->perfParts.end());
    for (const PerfPart &part : pp->parts)
        car->perfParts.push_back(uint16_t(part.id));
    m_fe->SetPreviewPerfParts(car->perfParts);
    printf("[PerfShop] fitted package %u/%u for $%d (saving $%d); bank $%d\n", m_perfCategory,
           m_perfCardLevel[sel], total, saving, m_profile.money);
    if (!Career::SaveCareer(m_profile))
        printf("[PerfShop] the package is fitted but the save failed\n");
    RefreshPerformancePackage(pkg);
}

void FEStartupSequence::HandlePerformancePackageMessage(uint32_t msg, FEPackage *pkg)
{
    if (m_perfCards.empty() || m_perfScroller.Package() != pkg)
        return;
    if (msg == kMsgScreenTick) {
        m_perfScroller.HandleMessage(msg, TickSeconds());
        return;
    }
    if (msg == kMsgInitComplete) {
        m_perfScroller.HandleMessage(msg, 0.0f);
        m_perfScroller.AllowFade();
        return;
    }
    if (msg == kMsgPadLeft || msg == kMsgPadRight) {
        m_perfScroller.HandleMessage(msg, 0.0f);
        m_perfCardSel = m_perfScroller.Selected();
        RefreshPerformancePackage(pkg);
        return;
    }
    if (msg == kMsgPadBack && !m_navPending) {
        PopScreen();
        return;
    }
    if ((msg == kMsgButtonPressed || msg == kMsgPadAccept) && !m_navPending) {
        const size_t sel = m_perfScroller.Selected();
        if (sel < m_perfCardType.size() && m_perfCardType[sel] == 1)
            BuyPerformancePackage(pkg);
        else if (sel < m_perfCardType.size() && m_perfCardType[sel] == 2)
            NavigateTo(pkg->GetName(), "UI_BuyPerformanceParts.fng");
        else
            printf("[PerfShop] card %zu (type %d) is not ported yet\n", sel,
                   sel < m_perfCardType.size() ? m_perfCardType[sel] : -1);
    }
}

// Peak torque (N*m) and peak power (kW) of the car with the given performance parts fitted:
// 0x0031d438 / 0x0031d2e0 scan the tuned torque spline from idle to the limit in 50 rpm steps.
struct PerfPeaks {
    float torqueNm = 0.0f, powerKw = 0.0f;
};
PerfPeaks ComputePerfPeaks(const FENativeInterface &fe, const std::string &car, const std::vector<uint16_t> &parts)
{
    PerfPeaks peaks;
    PlayerCar::Performance p;
    if (parts.empty()) {
        if (!fe.CarPerformance(car, p))
            return peaks;
    } else {
        const std::vector<uint8_t> tuned = fe.TunedCarRecord(car, parts);
        if (tuned.empty())
            return peaks;
        FENativeInterface::ReadCarPerformance(tuned.data(), p);
    }
    if (!p.Valid())
        return peaks;
    for (float rpm = p.idleRpm; rpm < p.maxRpm; rpm += 50.0f) {
        const float torque = PlayerCar::Torque(p, rpm); // kN*m
        peaks.torqueNm = std::max(peaks.torqueNm, torque * 1000.0f);
        peaks.powerKw = std::max(peaks.powerKw, torque * rpm * 0.10471976f); // kN*m * rad/s
    }
    return peaks;
}

// UI_BuyPerformanceParts.fng (factory 0x002b7f18, ctor 0x002b7f50, list 0x002b88f0, refresh
// 0x002b8690): "Performance Individual Parts" (ISSUES #199).  0x002b88f0 adds one row per part of
// the category's three packages that 0x002b5760 offers for sale; the list is the EA array scroller
// (0x0029f608 / 0x002a02b8) over 13 off-screen row templates, four rows visible: a name string
// (b2980ba8..) at x -267 and a price string (9b483b01..) at x 201, with the cursor bars a753c46c
// (names) and c128b184 (price) following the selected row.  The port lays the four visible rows
// out directly.  The Torque / Power stat panel needs the engine model (not ported, #198) and is
// hidden.
namespace {
constexpr uint32_t kPartNameSlot[] = {0xb2980ba8, 0xb2980ba9, 0xb2980baa, 0xb2980bab};
constexpr uint32_t kPartDataSlot[] = {0x9b483b01, 0x9b483b02, 0x9b483b03, 0x9b483b04};
constexpr float kPartRowY[] = {-73.0f, -48.0f, -23.0f, 2.0f}; // text sits 11 units below its cursor bar
constexpr float kPartNameX = -267.0f, kPartDataX = 201.0f;
constexpr size_t kPartRows = 4;
} // namespace

void FEStartupSequence::HandleBuyPartsLoaded(FEPackage *pkg)
{
    if (const char *cat = getenv("NFSU2_PERF_CATEGORY")) // dev aid: open a given category
        m_perfCategory = uint32_t(atoi(cat));
    if (const char *money = getenv("NFSU2_PERF_TEST")) { // dev aid, see HandlePerformancePackageLoaded
        if (!m_profile.valid || m_profile.cars.empty()) {
            m_profile.valid = true;
            m_profile.money = atoi(money);
            m_profile.cars.push_back({"SKYLINE", 0});
            m_profile.currentCar = 0;
        }
    }
    m_buyParts.clear();
    m_buySel = 0;
    m_buyTop = 0;
    for (uint32_t level = 1; level < 4; ++level)
        if (const PerfPackage *pp = m_fe->PerfParts().Find(m_perfCategory, level))
            for (const PerfPart &part : pp->parts)
                if (PerfPartAvailable(uint16_t(part.id)))
                    m_buyParts.push_back(&part);
    printf("[PerfShop] individual parts: category %u, %zu for sale\n", m_perfCategory, m_buyParts.size());
    RefreshBuyParts(pkg);
}

void FEStartupSequence::RefreshBuyParts(FEPackage *pkg)
{
    const size_t count = m_buyParts.size();
    if (m_buySel >= count)
        m_buySel = count ? count - 1 : 0;
    if (m_buySel < m_buyTop)
        m_buyTop = m_buySel;
    if (m_buySel >= m_buyTop + kPartRows)
        m_buyTop = m_buySel + 1 - kPartRows;
    for (size_t row = 0; row < kPartRows; ++row) {
        const size_t index = m_buyTop + row;
        const bool used = index < count;
        FENativeInterface::SetVisible(pkg, kPartNameSlot[row], used);
        FENativeInterface::SetVisible(pkg, kPartDataSlot[row], used);
        if (!used)
            continue;
        const PerfPart &part = *m_buyParts[index];
        FENativeInterface::SetLanguageHash(pkg, kPartNameSlot[row], part.nameHash);
        char price[24];
        snprintf(price, sizeof(price), "$%u", part.price);
        FENativeInterface::SetString(pkg, kPartDataSlot[row], price);
        // Every row slot ships at the same x column as row 0 (the template slots are staggered by
        // 10 units in the layout); all rows use row 0's name / price columns.
        if (FEObject *o = pkg->FindObjectByHash(kPartNameSlot[row]))
            FENativeInterface::SetCenter(o, kPartNameX, kPartRowY[row]);
        if (FEObject *o = pkg->FindObjectByHash(kPartDataSlot[row]))
            FENativeInterface::SetCenter(o, kPartDataX, kPartRowY[row]);
    }
    // The shipped row-0 backdrop group (66e29936) is the cursor: it follows the selected row.
    if (FEObject *cursor = pkg->FindObjectByHash(0x66e29936)) {
        float x, y;
        FENativeInterface::GetCenter(cursor, x, y);
        FENativeInterface::SetCenter(cursor, x, kPartRowY[m_buySel - m_buyTop] - kPartRowY[0] - 63.0f);
    }
    UpdateBuyPartsStats(pkg);
    // The scroll bar: 688a1551 is the 8.5 x 49.6 track between the arrows and 3fbb7a4e the 16 x 16 knob,
    // which slides along it with the window's position in the list.
    if (FEObject *track = pkg->FindObjectByHash(0x688a1551))
        if (FEObject *knob = pkg->FindObjectByHash(0x3fbb7a4e)) {
            float tx, ty, tw, th, kx, ky, kw, kh;
            FENativeInterface::GetCenter(track, tx, ty);
            FENativeInterface::GetSize(track, tw, th);
            FENativeInterface::GetCenter(knob, kx, ky);
            FENativeInterface::GetSize(knob, kw, kh);
            const float span = th - kh;
            const float frac = count > kPartRows ? float(m_buyTop) / float(count - kPartRows) : 0.0f;
            FENativeInterface::SetCenter(knob, kx, ty - th * 0.5f + kh * 0.5f + span * frac);
            FENativeInterface::SetObjectVisible(knob, count > kPartRows);
        }
    // Scroll arrows: up when rows are above the window, down when below.
    FENativeInterface::SetAlpha(pkg, 0x444969fd, m_buyTop > 0 ? 255 : 0);
    FENativeInterface::SetAlpha(pkg, 0x444969fe, m_buyTop + kPartRows < count ? 255 : 0);
}

// The "Performance Stats" panel (0x002b8690): the torque and power the highlighted part adds,
// as a number (ft-lbs / Nm and bhp / kW, by the speedometer units) and as current / preview bars.
void FEStartupSequence::UpdateBuyPartsStats(FEPackage *pkg)
{
    const Career::OwnedCar *car = ShopCar();
    const bool selected = m_buySel < m_buyParts.size() && car;
    for (uint32_t obj : {0xf601f2d4u, 0x38be24ceu, 0x38be24efu, 0x6bb32446u, 0x18531358u})
        FENativeInterface::SetVisible(pkg, obj, selected);
    for (const char *name : {"TORQUE_BACKING", "TORQUE_LEFT_CAP", "TORQUE_CURRENT_BAR", "TORQUE_PREVIEW_BAR",
                             "HORSEPOWER_BACKING", "HORSEPOWER_LEFT_CAP", "HORSEPOWER_CURRENT_BAR",
                             "HORSEPOWER_PREVIEW_BAR"})
        FENativeInterface::SetVisible(pkg, bStringHash(name), selected);
    if (!selected)
        return;
    std::vector<uint16_t> after = car->perfParts;
    after.push_back(uint16_t(m_buyParts[m_buySel]->id));
    const PerfPeaks now = ComputePerfPeaks(*m_fe, car->model, car->perfParts);
    const PerfPeaks next = ComputePerfPeaks(*m_fe, car->model, after);
    const bool metric = m_speedoUnits != 0;
    // 0x002b8690: torque in ft-lbs (x0.7376) or Nm, power in bhp or kW; the same "%2.2f %s" format.
    const float torque = (next.torqueNm - now.torqueNm) * (metric ? 1.0f : 0.7375621f);
    const float power = (next.powerKw - now.powerKw) * (metric ? 1.0f : 1.3410221f);
    char text[48];
    snprintf(text, sizeof(text), "%2.2f %s", torque,
             m_fe->LanguageText(metric ? 0x5b81d613u : 0xacb8dff3u).c_str());
    FENativeInterface::SetString(pkg, 0x18531358, text);
    snprintf(text, sizeof(text), "%2.2f %s", power, m_fe->LanguageText(metric ? 0x5b81d5bau : 0xcbbc6512u).c_str());
    FENativeInterface::SetString(pkg, 0x6bb32446, text);
    // Bars: the stat bars' track (46 units in, 164 long); the full scale is a stand-in (the
    // original's constants are runtime data).
    constexpr float kInset = 46.0f, kTrack = 164.0f;
    auto bar = [&](const char *base, float current, float preview, float full) {
        const std::string name = base;
        FEObject *backing = pkg->FindObjectByHash(bStringHash((name + "_BACKING").c_str()));
        FEObject *cur = pkg->FindObjectByHash(bStringHash((name + "_CURRENT_BAR").c_str()));
        FEObject *prev = pkg->FindObjectByHash(bStringHash((name + "_PREVIEW_BAR").c_str()));
        if (!backing || !cur || !prev)
            return;
        float bx, by, bw, bh, cx, cy, cw, ch, px, py, pw, ph;
        FENativeInterface::GetCenter(backing, bx, by);
        FENativeInterface::GetSize(backing, bw, bh);
        FENativeInterface::GetCenter(cur, cx, cy);
        FENativeInterface::GetSize(cur, cw, ch);
        FENativeInterface::GetCenter(prev, px, py);
        FENativeInterface::GetSize(prev, pw, ph);
        const float left = bx - bw * 0.5f + kInset;
        const float v = std::clamp(current / full, 0.03f, 1.0f);
        const float pv = std::clamp(preview / full, v, 1.0f);
        FENativeInterface::SetSize(cur, kTrack * v, ch);
        FENativeInterface::SetCenter(cur, left + 4.0f + kTrack * v * 0.5f, cy);
        FENativeInterface::SetObjectVisible(prev, pv > v + 0.002f);
        FENativeInterface::SetSize(prev, kTrack * (pv - v), ph);
        FENativeInterface::SetCenter(prev, left + 4.0f + kTrack * (v + pv) * 0.5f, py);
    };
    bar("TORQUE", now.torqueNm, next.torqueNm, 700.0f);
    bar("HORSEPOWER", now.powerKw, next.powerKw, 450.0f);
}

void FEStartupSequence::BuySelectedPerfPart(FEPackage *pkg)
{
    Career::OwnedCar *car = ShopCar();
    if (m_buySel >= m_buyParts.size() || !car)
        return;
    const PerfPart &part = *m_buyParts[m_buySel];
    if (m_profile.money < int(part.price)) {
        printf("[PerfShop] part %u costs $%u, career has $%d\n", part.id, part.price, m_profile.money);
        ShowMessageDialog(kLabelNotEnoughBank);
        return;
    }
    m_profile.money -= int(part.price);
    AddOwnedPart(m_profile, 0x80000000u | part.id);
    car->perfParts.push_back(uint16_t(part.id));
    m_fe->SetPreviewPerfParts(car->perfParts);
    printf("[PerfShop] fitted part %u for $%u; bank $%d\n", part.id, part.price, m_profile.money);
    if (!Career::SaveCareer(m_profile))
        printf("[PerfShop] the part is fitted but the save failed\n");
    // Rebuild the for-sale list without it.
    const size_t keep = m_buySel;
    HandleBuyPartsLoaded(pkg);
    m_buySel = keep;
    RefreshBuyParts(pkg);
}

void FEStartupSequence::HandleBuyPartsMessage(uint32_t msg, FEPackage *pkg)
{
    if (msg == kMsgPadUp && m_buySel > 0) {
        --m_buySel;
        RefreshBuyParts(pkg);
    } else if (msg == kMsgPadDown && m_buySel + 1 < m_buyParts.size()) {
        ++m_buySel;
        RefreshBuyParts(pkg);
    } else if (msg == kMsgPadBack && !m_navPending) {
        PopScreen();
    } else if ((msg == kMsgButtonPressed || msg == kMsgPadAccept) && !m_navPending) {
        BuySelectedPerfPart(pkg);
    }
}

// UI_VirtualKeyboard.fng (factory 0x001fb528, ISSUES #204): the name-entry screen.  The package is
// the PS2 on-screen keyboard: 0x5dfa6642 is the text field, the keys are strings over grey key
// backings (rows 0..3 at y -70 / -29 / 8 / 48, SPACE at y 89, BACK / DONE / CAPS / SHIFT among
// them).  The class highlights keys itself (no FEng responses ship), so the port keeps its own
// cursor: the selected key's label is drawn larger.  The pad moves the cursor and types; the
// window's keyboard types too (SDL_TEXTINPUT) and Enter finishes.  Letters are upper case
// (CAPS flips, SHIFT flips the next key), a name holds up to 15 characters.
void FEStartupSequence::OpenKeyboard(const std::string &initial, std::function<void(const std::string &)> done,
                                     std::function<void()> cancel)
{
    m_kbText = initial;
    m_kbDone = std::move(done);
    m_kbCancel = std::move(cancel);
    m_kbCaps = true;
    m_kbShift = false;
    m_kbSel = 0;
    m_kbActive = true;
    m_fe->SetTextInputMode(true);
    m_engine->QueuePackagePush("UI_VirtualKeyboard.fng", 0xff);
}

namespace {
std::string WideToAscii(const i16 *w)
{
    std::string out;
    for (; w && *w; ++w)
        out.push_back(*w < 128 ? char(*w) : '?');
    return out;
}
} // namespace

void FEStartupSequence::HandleKeyboardLoaded(FEPackage *pkg)
{
    // Dev aid: NFSU2_PACKAGE=UI_VirtualKeyboard.fng opens it standalone.
    if (!m_kbActive) {
        m_kbActive = true;
        m_fe->SetTextInputMode(true);
    }
    m_kbKeys.clear();
    static const uint32_t kNotKeys[] = {0x47ff4e7c, 0x5dfa6642, 0x42adb44c, 0x0880ac4c, 0x64aff8d3};
    std::function<void(FEObject *)> collect = [&](FEObject *o) {
        for (; o; o = o->GetNext()) {
            if (o->Type == FE_Group) {
                collect(static_cast<FEGroup *>(o)->GetFirstChild());
                continue;
            }
            if (o->Type != FE_String)
                continue;
            bool skip = false;
            for (uint32_t h : kNotKeys)
                skip = skip || o->NameHash == h;
            float x, y;
            FENativeInterface::GetCenter(o, x, y);
            if (skip || y < -80.0f || y > 100.0f)
                continue;
            const std::string text = WideToAscii(static_cast<FEString *>(o)->GetString());
            if (text.empty() || text.size() > 5)
                continue;
            KbKey key;
            key.obj = o;
            key.label = text;
            key.x = x;
            key.y = y;
            m_kbKeys.push_back(key);
        }
    };
    collect(pkg->GetFirstObject());
    // Reading order: rows top to bottom, keys left to right.
    std::sort(m_kbKeys.begin(), m_kbKeys.end(), [](const KbKey &a, const KbKey &b) {
        if (std::fabs(a.y - b.y) > 12.0f)
            return a.y < b.y;
        return a.x < b.x;
    });
    // Start on "A" (the home row) rather than the first key.
    for (size_t i = 0; i < m_kbKeys.size(); ++i)
        if (m_kbKeys[i].label == "Q") {
            m_kbSel = i;
            break;
        }
    printf("[Keyboard] %zu keys\n", m_kbKeys.size());
    KeyboardRefresh(pkg);
}

void FEStartupSequence::KeyboardRefresh(FEPackage *pkg)
{
    FENativeInterface::SetString(pkg, 0x5dfa6642, (m_kbText + "_").c_str());
    for (size_t i = 0; i < m_kbKeys.size(); ++i) {
        const bool selected = i == m_kbSel;
        const bool latched = (m_kbKeys[i].label == "CAPS" && m_kbCaps) || (m_kbKeys[i].label == "SHIFT" && m_kbShift);
        FENativeInterface::SetSize(m_kbKeys[i].obj, selected ? 1.15f : (latched ? 1.0f : 0.8f), selected ? 1.15f : (latched ? 1.0f : 0.8f));
    }
}

void FEStartupSequence::OnTextInput(const char *utf8)
{
    if (!m_kbActive || !utf8)
        return;
    for (const char *c = utf8; *c; ++c) {
        const unsigned char ch = static_cast<unsigned char>(*c);
        if (m_kbText.size() >= 15 || ch < 32 || ch > 126)
            continue;
        if (!(isalnum(ch) || ch == ' ' || ch == '-' || ch == '_' || ch == '.'))
            continue;
        m_kbText.push_back(char(toupper(ch)));
    }
    if (FEPackage *pkg = m_engine->GetPackageList()->FindPackage("UI_VirtualKeyboard.fng"))
        KeyboardRefresh(pkg);
}

void FEStartupSequence::KeyboardPress(FEPackage *pkg)
{
    if (m_kbSel >= m_kbKeys.size())
        return;
    const std::string &label = m_kbKeys[m_kbSel].label;
    if (getenv("NFSU2_KB_DEBUG"))
        printf("[Keyboard] press '%s' (text '%s')\n", label.c_str(), m_kbText.c_str());
    if (label == "DONE") {
        KeyboardFinish(pkg, true);
        return;
    }
    if (label == "BACK") {
        if (!m_kbText.empty())
            m_kbText.pop_back();
    } else if (label == "CAPS") {
        m_kbCaps = !m_kbCaps;
    } else if (label == "SHIFT") {
        m_kbShift = !m_kbShift;
    } else if (label == "<" || label == ">") {
        // the cursor keys of the original edit inside the text; the port appends only
    } else if (m_kbText.size() < 15) {
        char ch = label == "SPACE" ? ' ' : label[0];
        if (isalpha(static_cast<unsigned char>(ch))) {
            const bool upper = m_kbCaps != m_kbShift;
            ch = char(upper ? toupper(ch) : tolower(ch));
        }
        m_kbText.push_back(ch);
        m_kbShift = false;
    }
    KeyboardRefresh(pkg);
}

void FEStartupSequence::KeyboardFinish(FEPackage *pkg, bool accepted)
{
    std::string text = m_kbText;
    // Trim trailing spaces; an empty name is a cancel.
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
    m_kbActive = false;
    m_fe->SetTextInputMode(false);
    auto done = std::move(m_kbDone);
    auto cancel = std::move(m_kbCancel);
    m_kbDone = nullptr;
    m_kbCancel = nullptr;
    m_engine->UnloadPackage(pkg);
    printf("[Keyboard] %s '%s'\n", accepted && !text.empty() ? "done" : "cancelled", text.c_str());
    if (accepted && !text.empty() && done)
        done(text);
    else if (cancel)
        cancel();
}

void FEStartupSequence::HandleKeyboardMessage(uint32_t msg, FEPackage *pkg)
{
    if (m_kbKeys.empty())
        return;
    const KbKey &cur = m_kbKeys[m_kbSel];
    auto nearest = [&](float dy) {
        // The key in the adjacent row whose x is closest.
        size_t best = m_kbSel;
        float bestDist = 1e9f;
        for (size_t i = 0; i < m_kbKeys.size(); ++i) {
            const float rowDy = m_kbKeys[i].y - cur.y;
            if (dy * rowDy < 8.0f)
                continue; // wrong direction or the same row
            const float score = std::fabs(rowDy) * 4.0f + std::fabs(m_kbKeys[i].x - cur.x);
            if (score < bestDist) {
                bestDist = score;
                best = i;
            }
        }
        return best;
    };
    if (msg == kMsgPadLeft) {
        if (m_kbSel > 0 && std::fabs(m_kbKeys[m_kbSel - 1].y - cur.y) < 12.0f)
            --m_kbSel;
    } else if (msg == kMsgPadRight) {
        if (m_kbSel + 1 < m_kbKeys.size() && std::fabs(m_kbKeys[m_kbSel + 1].y - cur.y) < 12.0f)
            ++m_kbSel;
    } else if (msg == kMsgPadUp) {
        m_kbSel = nearest(-1.0f);
    } else if (msg == kMsgPadDown) {
        m_kbSel = nearest(1.0f);
    } else if (msg == kMsgPadAccept) {
        KeyboardPress(pkg);
        return;
    } else if (msg == kMsgPadBack) {
        if (m_kbText.empty())
            KeyboardFinish(pkg, false);
        else
            m_kbText.pop_back();
    }
    KeyboardRefresh(pkg);
}

// The tuning shop's two card menus (ISSUES #206), both icon scrollers over cloned OPTION_MASTER
// cards like the Profile Manager.  UI_PerformanceDyno_MAIN.fng (ctor 0x002c25f0, AddOptions
// 0x002c2c08) holds Tune Settings, Test Run, Dyno Run, Mode Select, Save Profile (only once the
// tuning session has changes, 0x002c1ac8) and Quit Tuning; UI_PerformanceTuning_Master.fng
// (0x002c3978, AddOptions 0x002c3d88) holds Suspension, Drivetrain, Aerodynamics, Tires & Brakes,
// ECU, Turbo and Nitrous, each opening a slider / graph screen (0x002c3e90).  Only the navigation
// is ported: Tune Settings opens the master, the sliders, the test and dyno runs are not.
void FEStartupSequence::HandleCardMenuLoaded(FEPackage *pkg)
{
    struct Card {
        uint32_t icon, label;
        std::function<void()> action;
    };
    std::vector<Card> cards;
    const bool dyno = strcasecmp(pkg->GetName(), "UI_PerformanceDyno_MAIN.fng") == 0;
    auto notPorted = [this](const char *what) {
        return [this, what]() {
            printf("[Tuning] %s is not ported yet\n", what);
            ShowMessageDialogText((std::string(what) + " is not available in the native build").c_str());
        };
    };
    if (dyno) {
        cards.push_back({0xd4060fc4, 0xa34e13bd, [this, pkg]() { NavigateTo(pkg->GetName(), "UI_PerformanceTuning_Master.fng"); }});
        cards.push_back({0xf188098c, 0xf62234ef, notPorted("Test Run")});
        cards.push_back({0xfe1e723e, 0x436c91d4, notPorted("Dyno Run")});
        cards.push_back({0xc2b2829c, 0xffe08459, notPorted("Mode Select")});
        cards.push_back({0x4c9e34e6, 0xde5cdfb2, [this]() { PopScreen(); }}); // Quit Tuning
    } else {
        static const struct { uint32_t icon, label; const char *name; } kMaster[] = {
            {0x178475e7, 0xef3772c1, "Suspension tuning"}, {0x01a29ffa, 0xe8da9e62, "Drivetrain tuning"},
            {0x214db7cf, 0x7ed554d1, "Aerodynamics tuning"}, {0xd6dc23ee, 0x6741bf48, "Tires & Brakes tuning"},
            {0x502abeed, 0x135bdd07, "ECU tuning"}, {0x06ef789c, 0x5ae08536, "Turbo tuning"},
            {0x9701bde4, 0x135c04da, "Nitrous tuning"}};
        int cardId = 0;
        for (const auto &m : kMaster) {
            if (cardId == 6) // Nitrous
                cards.push_back({m.icon, m.label, [this, pkg]() { NavigateTo(pkg->GetName(), "UI_PerformanceTuning_NOS.fng"); }});
            else if (cardId < 4) // Suspension / Drivetrain / Aerodynamics / Tires & Brakes (0x002c3e90)
                cards.push_back({m.icon, m.label, [this, pkg, cardId]() {
                                     m_tuneCard = cardId;
                                     NavigateTo(pkg->GetName(), "UI_PerformanceTuning_Sliders.fng");
                                 }});
            else
                cards.push_back({m.icon, m.label, notPorted(m.name)});
            ++cardId;
        }
    }
    std::vector<FEIconScroller::Def> defs;
    m_cardActions.clear();
    for (const Card &c : cards) {
        defs.push_back({c.icon, c.label, false});
        m_cardActions.push_back(c.action);
    }
    float centerX = 0.0f, centerY = -149.0f;
    m_cardMenuCards = CloneOptionCards(pkg, defs.size(), "TUNING_CARD_", centerX, centerY);
    if (m_cardMenuCards.size() != defs.size()) {
        printf("[Tuning] OPTION_MASTER missing\n");
        return;
    }
    m_cardScroller.SetupObjects(pkg, defs, m_cardMenuCards, 0, "OPTION_MASTER");
    m_cardScroller.SetLayoutCenter(centerX, centerY);
    FENativeInterface::SetString(pkg, 0x242217c1, ""); // the dyno screen's "Filter type goes here" placeholder
    printf("[Tuning] %s: %zu cards\n", pkg->GetName(), defs.size());
}

void FEStartupSequence::HandleCardMenuMessage(uint32_t msg, FEPackage *pkg)
{
    if (m_cardMenuCards.empty() || m_cardScroller.Package() != pkg)
        return;
    if (msg == kMsgScreenTick) {
        m_cardScroller.HandleMessage(msg, TickSeconds());
    } else if (msg == kMsgInitComplete) {
        m_cardScroller.HandleMessage(msg, 0.0f);
        m_cardScroller.AllowFade();
    } else if (msg == kMsgPadLeft || msg == kMsgPadRight) {
        m_cardScroller.HandleMessage(msg, 0.0f);
    } else if (msg == kMsgPadBack && !m_navPending) {
        PopScreen();
    } else if ((msg == kMsgButtonPressed || msg == kMsgPadAccept) && !m_navPending) {
        const size_t sel = m_cardScroller.Selected();
        if (sel < m_cardActions.size() && m_cardActions[sel])
            m_cardActions[sel]();
    }
}

// UI_PerformanceTuning_Sliders.fng (ISSUES #214): the tuning sliders of the master menu's first four
// cards (the class `0x002c3f68` builds the rows per card, DAT_005161f8 = the card id):
//   0 Suspension     fields 3, 7 (Low / High), 0, 4, 1, 5, 2, 6 (Soft / Hard), 8 (Less / More)
//   1 Drivetrain     fields 10..15 (the gears, shown as the speed at the redline), 9 (Top Speed / Acceleration)
//   2 Aerodynamics   fields 0x15, 0x16 (Weak / Strong)
//   3 Tires & Brakes fields 0x13, 0x14 (Weak / Strong), 0x12 (Rear / Front)
// A row exists when the field is unlocked by an installed part (Tune::Session::FieldActive).  The
// value is shown with "%2.2f" (0x00567c10) and a press of Left / Right moves it a thirtieth of the
// slider's display span (0x002c1e30); Reset puts the slider back to 100 (neutral).  The row
// pool is the package's own (one group + name / value / two end-label strings per row, the
// widget's slider bars are not drawn).  Leaving writes the 41 slider bytes to the car and saves.
namespace {
struct TuneRowDef {
    int field;
    uint32_t left, right; // the end labels
};
const uint32_t kTuneFieldName[25] = {
    0xfe22b6a2, 0xefdb2567, 0xb6d444b4, 0x0b161ec3, 0x93b7d1a3, 0x87c86448, 0xff0cbfd5, 0x534e99e4, 0x2430e6be,
    0x6a71e798, 0x7d822b64, 0x7d822b65, 0x7d822b66, 0x7d822b67, 0x7d822b68, 0x7d822b69, 0x55c60293, 0xfbbdd223,
    0x2308f06c, 0x3988faef, 0xe4eebd70, 0xd99fc2e4, 0x21d83e05, 0xc8b8fbf1, 0xfe82fde0};
std::vector<TuneRowDef> TuneRowsForCard(int card)
{
    constexpr uint32_t kLow = 0x26033125, kHigh = 0xe66708f3, kSoft = 0xe66d2a8f, kHard = 0xe666e852,
                       kLess = 0xe6692b0a, kMore = 0xe669e1c6, kWeak = 0xe66f30db, kStrong = 0x36c30e70,
                       kRear = 0x001c0cc9, kFront = 0x02cbdc28, kTop = 0xcebfaa36, kAccel = 0x95a7073d;
    switch (card) {
    case 0:
        return {{3, kLow, kHigh}, {7, kLow, kHigh}, {0, kSoft, kHard}, {4, kSoft, kHard}, {1, kSoft, kHard},
                {5, kSoft, kHard}, {2, kSoft, kHard}, {6, kSoft, kHard}, {8, kLess, kMore}};
    case 1:
        return {{10, 0, 0}, {11, 0, 0}, {12, 0, 0}, {13, 0, 0}, {14, 0, 0}, {15, 0, 0}, {9, kTop, kAccel}};
    case 2:
        return {{0x15, kWeak, kStrong}, {0x16, kWeak, kStrong}};
    case 3:
        return {{0x13, kWeak, kStrong}, {0x14, kWeak, kStrong}, {0x12, kRear, kFront}};
    default:
        return {};
    }
}
constexpr size_t kTuneVisibleRows = 6;
constexpr float kTuneRowY0 = -50.0f, kTuneRowStep = 44.0f;
} // namespace

void FEStartupSequence::HandleSlidersLoaded(FEPackage *pkg)
{
    m_tuneRows.clear();
    m_tuneSel = m_tuneTop = 0;
    m_tuneSession.reset();
    // Dev aid: NFSU2_TUNE_TEST=<card> opens the screen standalone with an in-memory Skyline carrying
    // every level-3 performance package (use NFSU2_MEMCARD on a scratch directory).
    if (const char *card = getenv("NFSU2_TUNE_TEST")) {
        m_tuneCard = atoi(card);
        if (!m_profile.valid || m_profile.cars.empty()) {
            m_profile.valid = true;
            m_profile.cars.push_back({"SKYLINE", 0});
            m_profile.currentCar = 0;
        }
        Career::OwnedCar &car = m_profile.cars[size_t(m_profile.currentCar)];
        if (car.perfParts.empty())
            for (size_t i = 0; i < m_fe->PerfParts().Count(); ++i)
                if (m_fe->PerfParts().At(i).level == 3)
                    for (const PerfPart &part : m_fe->PerfParts().At(i).parts)
                        car.perfParts.push_back(uint16_t(part.id));
    }
    Career::OwnedCar *car = ShopCar();
    const uint8_t *record = nullptr;
    std::vector<uint8_t> frame;
    if (car && m_fe->TuneInputs(car->model, car->perfParts, record, frame)) {
        m_tuneSession = std::make_unique<Tune::Session>(m_fe->TuneTable(), record, frame, car->perfParts);
        m_tuneSession->SetMetric(m_speedoUnits != 0);
        if (car->tune.size() == Tune::kSettingSize)
            m_tuneSession->LoadSetting(car->tune);
        for (const TuneRowDef &def : TuneRowsForCard(m_tuneCard))
            if (m_tuneSession->FieldActive(def.field))
                m_tuneRows.push_back({def.field, def.left, def.right});
    }
    printf("[Tuning] sliders card %d: %zu rows\n", m_tuneCard, m_tuneRows.size());
    RefreshSliders(pkg);
}

void FEStartupSequence::RefreshSliders(FEPackage *pkg)
{
    const size_t count = m_tuneRows.size();
    if (m_tuneSel >= count)
        m_tuneSel = count ? count - 1 : 0;
    if (m_tuneSel < m_tuneTop)
        m_tuneTop = m_tuneSel;
    if (m_tuneSel >= m_tuneTop + kTuneVisibleRows)
        m_tuneTop = m_tuneSel + 1 - kTuneVisibleRows;
    // The package ships nine row groups (66e29936..3e) and the name / value / end-label strings.
    for (size_t slot = 0; slot < 9; ++slot) {
        const uint32_t group = 0x66e29936u + uint32_t(slot);
        const uint32_t nameObj = 0xb2980ba8u + uint32_t(slot), dataObj = 0x9b483b01u + uint32_t(slot);
        const uint32_t leftObj = 0x8e435402u + uint32_t(slot), rightObj = 0x8e435843u + uint32_t(slot);
        const bool used = slot < kTuneVisibleRows && m_tuneTop + slot < count;
        for (uint32_t obj : {group, nameObj, dataObj, leftObj, rightObj})
            FENativeInterface::SetVisible(pkg, obj, used);
        if (!used)
            continue;
        const size_t index = m_tuneTop + slot;
        const TuneRow &row = m_tuneRows[index];
        const float y = kTuneRowY0 + kTuneRowStep * float(slot);
        const bool selected = index == m_tuneSel;
        const uint8_t alpha = selected ? 255 : 170;
        if (row.field == 3 || row.field == 7 || row.field == 10) { // 0x002c2480: unit suffix
            const bool length = row.field != 10;
            const std::string title =
                m_fe->LanguageText(kTuneFieldName[row.field]) + " (" +
                m_fe->LanguageText(length ? (m_speedoUnits != 0 ? 0x5e1f61eeu : 0x6382ece0u)
                                          : (m_speedoUnits != 0 ? 0xcbbc8bf8u : 0x8569ab44u)) + ")";
            FENativeInterface::SetString(pkg, nameObj, title.c_str());
        } else {
            FENativeInterface::SetLanguageHash(pkg, nameObj, kTuneFieldName[row.field]);
        }
        if (row.left) {
            FENativeInterface::SetLanguageHash(pkg, leftObj, row.left);
            FENativeInterface::SetLanguageHash(pkg, rightObj, row.right);
        } else {
            FENativeInterface::SetString(pkg, leftObj, "");
            FENativeInterface::SetString(pkg, rightObj, "");
        }
        char value[32];
        // 0x00567c08 / 0x00567c10: one decimal for the gear speeds, two for everything else.
        snprintf(value, sizeof(value), row.field >= 10 && row.field <= 15 ? "%2.1f" : "%2.2f",
                 m_tuneSession ? m_tuneSession->Display(row.field, 0) : 0.0f);
        FENativeInterface::SetString(pkg, dataObj, value);
        const struct { uint32_t obj; float x; } place[] = {{group, -4.0f}, {nameObj, -290.0f}, {leftObj, -95.0f},
                                                              {dataObj, 100.0f}, {rightObj, 215.0f}};
        for (const auto &pl : place)
            if (FEObject *o = pkg->FindObjectByHash(pl.obj)) {
                FENativeInterface::SetCenter(o, pl.x, y);
                if (pl.obj != group)
                    FENativeInterface::SetAlpha(pkg, pl.obj, alpha);
            }
    }
}

void FEStartupSequence::AdjustSlider(FEPackage *pkg, int dir)
{
    if (!m_tuneSession || m_tuneSel >= m_tuneRows.size())
        return;
    const int field = m_tuneRows[m_tuneSel].field;
    const float step = m_tuneSession->Step(field);
    m_tuneSession->SetDisplay(field, 0, m_tuneSession->Display(field, 0) + float(dir) * step);
    RefreshSliders(pkg);
}

void FEStartupSequence::FinishSliders(FEPackage *pkg)
{
    (void)pkg;
    if (Career::OwnedCar *car = ShopCar(); car && m_tuneSession) {
        car->tune = m_tuneSession->Setting();
        m_fe->SetPreviewTune(car->tune);
        if (!Career::SaveCareer(m_profile))
            printf("[Tuning] the tune is kept but the save failed\n");
        printf("[Tuning] tune saved for %s\n", car->model.c_str());
    }
    m_tuneSession.reset();
}

void FEStartupSequence::HandleSlidersMessage(uint32_t msg, FEPackage *pkg)
{
    if (msg == kMsgPadUp && m_tuneSel > 0) {
        --m_tuneSel;
        RefreshSliders(pkg);
    } else if (msg == kMsgPadDown && m_tuneSel + 1 < m_tuneRows.size()) {
        ++m_tuneSel;
        RefreshSliders(pkg);
    } else if (msg == kMsgPadLeft) {
        AdjustSlider(pkg, -1);
    } else if (msg == kMsgPadRight) {
        AdjustSlider(pkg, +1);
    } else if (msg == kMsgPadLTrigger) { // "Reset to default"
        if (m_tuneSession && m_tuneSel < m_tuneRows.size()) {
            const int field = m_tuneRows[m_tuneSel].field;
            m_tuneSession->Reset(field);
            m_tuneSession->SetDisplay(field, 0, m_tuneSession->Display(field, 0));
            RefreshSliders(pkg);
        }
    } else if (msg == kMsgPadBack && !m_navPending) {
        FinishSliders(pkg);
        PopScreen();
    }
}

// UI_PerformanceTuning_NOS.fng (ISSUES #214, card 6 of the master menu): the nitrous graph.  The
// dot is the pair {duration (field 0x10), torque (field 0x11)} of the car's tuning session, the
// crosshair follows it and the two sliders are coupled (0x00321b70: more torque, shorter burn).
// The original class (0x002c5fd8 / 0x002c6078) places the dot in the graph box from the fields'
// slider values; the port maps the duration over the 0..12 s axis and the torque over its
// 0.1..0.9 range.  Left / Right change the duration, Up / Down the torque.
namespace {
constexpr float kNosX0 = -148.6f, kNosW = 319.3f; // the graph box (d35a7923 spans it)
constexpr float kNosYTop = 124.0f, kNosH = 202.0f;  // 01183698 centred at y 23, 202 tall
}

void FEStartupSequence::HandleNosLoaded(FEPackage *pkg)
{
    m_tuneRows.clear();
    m_tuneSession.reset();
    if (const char *test = getenv("NFSU2_TUNE_TEST")) { // see HandleSlidersLoaded
        (void)test;
        if (!m_profile.valid || m_profile.cars.empty()) {
            m_profile.valid = true;
            m_profile.cars.push_back({"SKYLINE", 0});
            m_profile.currentCar = 0;
        }
        Career::OwnedCar &c = m_profile.cars[size_t(m_profile.currentCar)];
        if (c.perfParts.empty())
            for (size_t i = 0; i < m_fe->PerfParts().Count(); ++i)
                if (m_fe->PerfParts().At(i).level == 3)
                    for (const PerfPart &part : m_fe->PerfParts().At(i).parts)
                        c.perfParts.push_back(uint16_t(part.id));
    }
    Career::OwnedCar *car = ShopCar();
    const uint8_t *record = nullptr;
    std::vector<uint8_t> frame;
    if (car && m_fe->TuneInputs(car->model, car->perfParts, record, frame)) {
        m_tuneSession = std::make_unique<Tune::Session>(m_fe->TuneTable(), record, frame, car->perfParts);
        m_tuneSession->SetMetric(m_speedoUnits != 0);
        if (car->tune.size() == Tune::kSettingSize)
            m_tuneSession->LoadSetting(car->tune);
        if (!m_tuneSession->FieldActive(0x10) || !m_tuneSession->FieldActive(0x11))
            m_tuneSession.reset(); // no nitrous parts fitted
    }
    printf("[Tuning] nitrous graph: %s\n", m_tuneSession ? "ready" : "no nitrous parts fitted");
    RefreshNos(pkg);
}

void FEStartupSequence::RefreshNos(FEPackage *pkg)
{
    const bool ready = m_tuneSession != nullptr;
    for (uint32_t obj : {0x9c90d375u, 0x01183698u, 0xd35a7923u, 0x11495cd6u})
        FENativeInterface::SetVisible(pkg, obj, ready);
    if (!ready)
        return;
    float lo, hi;
    m_tuneSession->Bounds(0x11, 0, true, lo, hi);
    const float seconds = m_tuneSession->Display(0x10, 0);
    const float torque = m_tuneSession->Display(0x11, 0);
    const float x = kNosX0 + std::clamp(seconds / 12.0f, 0.0f, 1.0f) * kNosW;
    const float y = kNosYTop - std::clamp((torque - lo) / (hi - lo), 0.0f, 1.0f) * kNosH;
    auto place = [&](uint32_t hash, float cx, float cy) {
        if (FEObject *o = pkg->FindObjectByHash(hash))
            FENativeInterface::SetCenter(o, cx, cy);
    };
    place(0x9c90d375, x, y);                                   // the dot
    place(0x01183698, x, kNosYTop - kNosH * 0.5f);             // the vertical line
    place(0xd35a7923, kNosX0 + kNosW * 0.5f, y);               // the horizontal line
    if (FEObject *seg = pkg->FindObjectByHash(0x11495cd6)) {   // the duration bar under the line
        FENativeInterface::SetSize(seg, x - kNosX0, 3.0f);
        FENativeInterface::SetCenter(seg, kNosX0 + (x - kNosX0) * 0.5f, y + 1.5f);
    }
}

void FEStartupSequence::AdjustNos(FEPackage *pkg, int field, int dir)
{
    if (!m_tuneSession)
        return;
    const float step = m_tuneSession->Step(field);
    m_tuneSession->SetDisplay(field, 0, m_tuneSession->Display(field, 0) + float(dir) * step);
    RefreshNos(pkg);
}

void FEStartupSequence::HandleNosMessage(uint32_t msg, FEPackage *pkg)
{
    if (msg == kMsgPadLeft)
        AdjustNos(pkg, 0x10, -1);
    else if (msg == kMsgPadRight)
        AdjustNos(pkg, 0x10, +1);
    else if (msg == kMsgPadUp)
        AdjustNos(pkg, 0x11, +1);
    else if (msg == kMsgPadDown)
        AdjustNos(pkg, 0x11, -1);
    else if (msg == kMsgPadLTrigger) { // Reset to default
        if (m_tuneSession) {
            m_tuneSession->Reset(0x10);
            m_tuneSession->Reset(0x11);
            m_tuneSession->SetDisplay(0x10, 0, m_tuneSession->Display(0x10, 0));
            RefreshNos(pkg);
        }
    } else if (msg == kMsgPadBack && !m_navPending) {
        FinishSliders(pkg);
        PopScreen();
    }
}

// UI_ProfileManager.fng (factory 0x0023c968, ctor 0x0023c9a0, options 0x0023cbb8): the main menu's
// Profile Manager.  0x0023cbb8 builds three option objects at runtime with
// FUN_0029ae78(obj, icon, label, 0) and hands each one to IconScroller::AddOption (0x0029cd38):
//   (0x228b7e32, 0x1c8ace) = "Save", (0x56026748, 0x4af2e79e) = "Profile",
//   (0x2287e063, 0x18ecff) = "Load".
// The .fng ships no option objects - only the 126x62 card art 687fda31 (with the df422345 quad
// over it) - so the port clones that art once per card and drives the same IconScroller the other
// menus use.  0x0023ca08 greys the Save card when there is no profile to write
// (FUN_00273600(0x4f12a8) == 0) and shows the profile's name.
void FEStartupSequence::HandleProfileManagerLoaded(FEPackage *pkg)
{
    std::vector<FEIconScroller::Def> defs;
    if (m_profileSub) {
        // 0x0023cd50 / 0x0023cdb8: the "Profile" screen, titled 0x4af2e79e, with Create and Delete.
        FENativeInterface::SetLanguageHash(pkg, 0x42adb44c, 0x4af2e79e);
        defs.push_back({0x56026748, 0x55423473});                       // Create
        defs.push_back({0xb9929709, 0x56b00632, !m_profile.valid});     // Delete
    } else {
        FENativeInterface::SetLanguageHash(pkg, 0x42adb44c, 0xbcb18f38); // "Profile Manager"
        static const struct {
            uint32_t icon, label;
        } kCards[] = {{0x228b7e32, 0x1c8ace}, {0x56026748, 0x4af2e79e}, {0x2287e063, 0x18ecff}};
        defs.push_back({kCards[0].icon, kCards[0].label, !m_profile.valid});
        defs.push_back({kCards[1].icon, kCards[1].label});
        defs.push_back({kCards[2].icon, kCards[2].label});
    }

    float centerX = 0.0f, centerY = -149.0f;
    m_profileCards = CloneOptionCards(pkg, defs.size(), "PROFILE_CARD_", centerX, centerY);
    if (m_profileCards.size() == defs.size()) {
        // Dev aid: NFSU2_PROFILE_CARD_TEX=<hex> forces one texture on the three cards, so the
        // layout can be checked when the card icons themselves are missing.
        if (const char *tex = getenv("NFSU2_PROFILE_CARD_TEX")) {
            const uint32_t hash = uint32_t(strtoul(tex, nullptr, 16));
            for (FEObject *card : m_profileCards)
                FENativeInterface::SetTextureHash(card, hash);
        }
        if (const char *pos = getenv("NFSU2_PROFILE_CARD_POS")) {
            sscanf(pos, "%f,%f", &centerX, &centerY);
        }
        m_profileScroller.SetupObjects(pkg, defs, m_profileCards, 0, "OPTION_MASTER");
        m_profileScroller.SetLayoutCenter(centerX, centerY);
    } else {
        printf("[Profile] OPTION_MASTER missing; the manager has no cards\n");
    }
    // The card's own scripts relabel 47ff4e7c with the current profile's name (0x0023ca08).
    FENativeInterface::SetString(pkg, 0x47ff4e7c, m_profile.valid ? m_profile.name.c_str() : "");
    printf("[Profile] %s: %zu cards, profile %s\n", m_profileSub ? "profile screen (Create, Delete)" : "manager",
           m_profileCards.size(), m_profile.valid ? m_profile.name.c_str() : "none");
}

void FEStartupSequence::HandleProfileManagerMessage(uint32_t msg, FEPackage *pkg)
{
    if (!pkg)
        return;
    if (msg == kMsgPadLeft || msg == kMsgPadRight) {
        m_profileScroller.HandleMessage(msg, 0.0f);
        printf("[Profile] card %zu\n", m_profileScroller.Selected());
    } else if (msg == kMsgInitComplete || msg == kMsgScreenTick) {
        m_profileScroller.HandleMessage(msg, TickSeconds());
    }
}

// Profile -> Create: name the profile with the keyboard, refuse a name that is already on the card,
// write a fresh career and make it current.
void FEStartupSequence::CreateProfileWithKeyboard()
{
    OpenKeyboard("", [this](const std::string &name) {
            const std::string file = Career::MemoryCard::SanitizeName(name);
            for (const Career::CardFile &f : Career::MemoryCard::Get().List())
                if (f.name == file) {
                    ShowMessageDialogText("A profile with that name already exists");
                    return;
                }
            m_profile = Career::Profile();
            m_profile.valid = true;
            m_profile.name = name;
            m_profile.cars.clear();
            m_profile.currentCar = 0;
            if (!Career::SaveCareer(m_profile)) {
                ShowMessageDialogText("The memory card could not be written");
                return;
            }
            m_careerExists = true;
            printf("[Profile] created %s (money %d, respect %d)\n", m_profile.name.c_str(), m_profile.money,
                   m_profile.respect);
            if (FEPackage *mgr = m_engine->GetPackageList()->FindPackage("UI_ProfileManager.fng"))
                FENativeInterface::SetString(mgr, 0x47ff4e7c, m_profile.name.c_str());
        });
}

// Profile -> Delete: after a Yes / No prompt remove the loaded profile's save from the card.
void FEStartupSequence::DeleteCurrentProfile()
{
    if (!m_profile.valid) {
        ShowMessageDialogText("There is no profile to delete");
        return;
    }
    const std::string text = "Delete profile " + m_profile.name + "?";
    ShowYesNoDialog(text.c_str(), [this]() {
        const std::string file = Career::MemoryCard::SanitizeName(m_profile.name);
        const bool removed = Career::MemoryCard::Get().Remove(file);
        printf("[Profile] delete %s: %s\n", file.c_str(), removed ? "removed" : "not on the card");
        m_profile = Career::Profile();
        m_careerExists = Career::CareerExists();
        if (FEPackage *mgr = m_engine->GetPackageList()->FindPackage("UI_ProfileManager.fng"))
            FENativeInterface::SetString(mgr, 0x47ff4e7c, "");
    });
}

// Accept on the highlighted card.  0x0023cbb8's options dispatch through the three vtables
// 0x46ad10 / 0x46acb0 / 0x46acf8; the "Profile" one pushes the 0x0023cd50 screen (title
// 0x4af2e79e = "Profile") whose two options are "Create" and "Delete".
void FEStartupSequence::ProfileManagerAccept()
{
    const size_t sel = m_profileScroller.Selected();
    if (m_profileSub) {
        if (sel == 0)
            CreateProfileWithKeyboard();
        else
            DeleteCurrentProfile();
        return;
    }
    if (sel == 0) {
        // "Save": write the profile that is loaded (0x00273600(0x4f12a8) != 0).
        if (!m_profile.valid) {
            ShowMessageDialogText("There is no profile to save");
            return;
        }
        if (Career::SaveCareer(m_profile)) {
            m_careerExists = true;
            printf("[Profile] saved %s (money %d, respect %d)\n", m_profile.name.c_str(), m_profile.money,
                   m_profile.respect);
        } else {
            ShowMessageDialogText("The memory card could not be written");
        }
    } else if (sel == 1) {
        // "Profile" opens the Create / Delete screen (0x0023cd50): the same package, re-entered
        // with the two other cards.
        m_profileSub = true;
        NavigateTo("UI_ProfileManager.fng", "UI_ProfileManager.fng");
    } else {
        // "Load": pick a save off the card through the career flow's own MC_List.fng.
        if (!Career::CareerExists()) {
            ShowMessageDialogText("There are no profiles on the memory card");
            return;
        }
        m_memcardNext = "UI_ProfileManager.fng";
        m_memcardFromProfileManager = true;
        m_memcardPick = m_profile.valid ? m_profile.name : std::string();
        OpenMemcardList();
    }
}

// 0x001e71f0 / 0x00259328: a new career starts with an empty profile and the starting cash/respect;

// the save is written at once (UG2 does the same, then asks for a name and a starter car).
void FEStartupSequence::BeginCareer(const std::string &name)
{
    m_profile = Career::Profile();
    m_profile.valid = true;
    m_profile.name = name;
    m_profile.cars.clear();
    m_profile.currentCar = 0;
    printf("[Career] new profile %s (money %d, respect %d)\n", m_profile.name.c_str(), m_profile.money,
           m_profile.respect);
    if (!Career::SaveCareer(m_profile))
        printf("[Career] could not write the save (continue without a card)\n");
    m_careerExists = Career::CareerExists();
    m_raceMode = 8; // career car select
}

// Career car select accept: the chosen car becomes the starter car (UG2 buys it with the career
// money - the car-lot prices are in no decoded chunk, so the port takes it free, ISSUES #137).
void FEStartupSequence::RecordCareerCar()
{
    if (!m_profile.valid)
        return;
    const std::string &car = m_fe->PreviewCarName();
    if (car.empty())
        return;
    if (!m_profile.Owns(car)) {
        Career::OwnedCar owned;
        owned.model = car;
        m_profile.cars.push_back(owned);
        m_profile.currentCar = int(m_profile.cars.size()) - 1;
        printf("[Career] starter car: %s\n", car.c_str());
    }
    Career::SaveCareer(m_profile);
    m_careerExists = true;
}

void FEStartupSequence::ResumeCareer()
{
    if (!LoadCareerProfile()) {
        printf("[Career] resume: no readable save\n");
        return;
    }
    m_navPending = true; // leaving the front end for the world (one accept press only)
    if (const Career::OwnedCar *car = m_profile.CurrentCar())
        ApplyOwnedCarPreview(*car);
    m_raceMode = 1;  // DAT_00506b50: career
    m_raceType = 9;  // free roam (the crib/city); race number 0 -> no route
    m_raceNumber = 0;
    m_cribRequested = true; // 0x00265970: the career opens in the crib, not on a track
    RequestRace();
}

bool FEStartupSequence::LoadCareerProfile()
{
    // The 0x4fb4ac table also exists for Quick Race without a career save; RECORDS.REC keeps it.
    m_records = Career::BestTable{};
    Career::LoadRecords(m_records);
    if (Career::LoadCareer(m_profile)) {
        m_careerExists = true;
        m_records.MergeFrom(m_profile);
        if (const Career::OwnedCar *car = m_profile.CurrentCar()) {
            m_fe->SetPreviewPaint(car->paintSet ? car->paintSet : PaintCatalog::kGloss,
                                  car->paint); // the career race car uses this colour (#167/#168)
            m_fe->SetPreviewRimPaint(car->partPaintSet[3] ? car->partPaintSet[3] : PaintCatalog::kRims,
                                     car->partPaint[3] ? car->partPaint[3] : car->partPaint[4]);
            m_fe->SetPreviewCaliperPaint(car->partPaintSet[5] ? car->partPaintSet[5] : PaintCatalog::kBrakes,
                                         car->partPaint[5]);
            m_fe->SetPreviewMirrorPaint(car->partPaintSet[2] ? car->partPaintSet[2] : PaintCatalog::kGloss,
                                        car->partPaint[2]);
            m_fe->SetPreviewSpoilerPaint(car->partPaintSet[0] ? car->partPaintSet[0] : PaintCatalog::kGloss,
                                         car->partPaint[0]);
        }
        // Dev aid: NFSU2_EXTRA_CARS=A,B adds owned cars in memory (tests the garage car select).
        if (const char *extra = getenv("NFSU2_EXTRA_CARS")) {
            std::string list = extra;
            for (size_t b = 0; b < list.size();) {
                size_t e = list.find(',', b);
                if (e == std::string::npos)
                    e = list.size();
                if (e > b && !m_profile.Owns(list.substr(b, e - b)))
                    m_profile.cars.push_back({list.substr(b, e - b), 0});
                b = e + 1;
            }
        }
        // Dev aid: NFSU2_DONE_REGION=n marks every event of regions 0..n complete in memory.
        if (const char *dr = getenv("NFSU2_DONE_REGION")) {
            const int upTo = atoi(dr);
            for (const Career::Event &e : Career::EventTable::Get().All())
                if (e.region <= upTo)
                    m_profile.Complete(e.id);
        }
        // Dev aids: NFSU2_BEST_TIME=<race>:<ms> / NFSU2_BEST_SCORE=<race>:<points> seed the
        // 0x4fb4ac table so the world map and QR track select can be checked without a race.
        if (const char *bt = getenv("NFSU2_BEST_TIME")) {
            int race = 0, ms = 0;
            if (sscanf(bt, "%d:%d", &race, &ms) == 2 && ms > 0) {
                m_profile.RecordTime(race, 0, ms);
                m_records.RecordTime(race, 0, ms);
                Career::SaveCareer(m_profile);
                Career::SaveRecords(m_records);
            }
        }
        if (const char *bs = getenv("NFSU2_BEST_SCORE")) {
            int race = 0, score = 0;
            if (sscanf(bs, "%d:%d", &race, &score) == 2 && score > 0) {
                m_profile.RecordScore(race, 0, score);
                m_records.RecordScore(race, 0, score);
                Career::SaveCareer(m_profile);
                Career::SaveRecords(m_records);
            }
        }
        return true;
    }
    m_careerExists = false;
    return false;
}

// 0x0025xxx (race over) -> the event's purse. The event that runs a given race is found with
// 0x002635d8 (the record whose +0x18 race list contains the race number); completion is the
// 0x00268bd8 test on the record's +0x08 id that the world map (0x001ee590) puts at item index 7.
// UG2 pays the purse for winning; the port pays a podium share (2nd 1/2, 3rd 1/4) and only completes
// the event on a win.
void FEStartupSequence::AwardCareerResult(int raceNumber, int place, float seconds, int score)
{
    if (!m_profile.valid || !IsCareerRun() || place <= 0)
        return;
    // The world map's Previous Time (0x00272e18 over the 0x4fb4ac table). The table's value is the
    // best lap (RaceSession::BestLap), matching the QR "Best Lap" label; drift events use the same
    // table through 0x00272ee8, which keeps the largest score instead of the smallest time.
    const Career::Event *timed = Career::EventTable::Get().FindByRace(raceNumber);
    bool recorded = false;
    const int direction = timed ? timed->DirectionFor(raceNumber) : 0;
    if (timed && timed->stage == 5) {
        if (score > 0) {
            m_profile.RecordScore(raceNumber, direction, score);
            m_records.RecordScore(raceNumber, direction, score);
            recorded = true;
        }
    } else if (seconds > 0.0f && timed) {
        const int ms = int(seconds * 1000.0f + 0.5f);
        m_profile.RecordTime(raceNumber, direction, ms);
        m_records.RecordTime(raceNumber, direction, ms);
        recorded = true;
    }
    if (recorded) {
        Career::SaveCareer(m_profile);
        Career::SaveRecords(m_records);
    }
    const Career::Event *ev = Career::EventTable::Get().FindByRace(raceNumber);
    if (!ev) {
        printf("[Career] race %d is not a career event\n", raceNumber);
        return;
    }
    if (m_profile.Completed(ev->id)) {
        printf("[Career] event %08x already complete (race %d, P%d)\n", ev->id, raceNumber, place);
        return;
    }
    int money = 0, respect = 0;
    if (place == 1) {
        money = int(ev->purse);
        respect = int(ev->respect);
    } else if (place == 2) {
        money = int(ev->purse) / 2;
    } else if (place == 3) {
        money = int(ev->purse) / 4;
    }
    if (money == 0 && respect == 0) {
        printf("[Career] event %08x (race %d): P%d, no purse - win it to complete\n", ev->id, raceNumber,
               place);
        return;
    }
    m_profile.money += money;
    m_profile.respect += respect;
    if (place == 1) {
        m_profile.Complete(ev->id);
        MaybeSignSponsor(*ev);
    }
    Career::SaveCareer(m_profile);

    // (The old "stage complete when every event of its +0x0f is done" rule read +0x0f as a career
    // stage; it is the event type (ISSUES #143), so it is gone. What unlocks events/areas in the
    // original is not decoded; the world map offers every event.)
    printf("[Career] event %08x (race %d, type %u) P%d: +%d money +%d respect -> money %d, respect %d (%zu done)%s\n",
           ev->id, raceNumber, unsigned(ev->stage), place, money, respect, m_profile.money, m_profile.respect,
           m_profile.completedEvents.size(), place == 1 ? ", completed" : "");
}

// Sponsor offer after an event win (0x00268a30 -> 0x0026bc60 -> 0x00266338 / 0x002663d8 / 0x00267890).
// The area object (profile + 0x72fc + region * 0x104) counts the completed ordinary races of the area
// (0x002661f0: kind +0x34 == 3 and +0x0d != 1 go to the 35-slot list that 0x002660a0 counts). When the
// area has no sponsor (+0xe8 == 0) and a sponsor's threshold (0x34a18 +0x1c) is met, one of the
// qualifying sponsors is picked at random (0x00151f48 over the count; the float bound 0x002663d8
// compares against is 1e9) and signed: its signing bonus (+0xc) is added to the money (0x0026bbb0 on
// profile + 0x7154). The signing screen itself is not ported; "View Signed Sponsors" lists it.
void FEStartupSequence::MaybeSignSponsor(const Career::Event &ev)
{
    if (ev.kind != 3 || ev.special || ev.region >= Career::Profile::kAreas)
        return;
    if (m_profile.signedSponsors[ev.region] != 0)
        return;
    int completed = 0;
    for (const Career::Event &e : Career::EventTable::Get().All())
        if (e.region == ev.region && e.kind == 3 && !e.special && m_profile.Completed(e.id))
            ++completed;
    const std::vector<const Career::Sponsor *> offers =
        Career::SponsorTable::Get().Offers(ev.region, completed);
    if (offers.empty())
        return;
    const Career::Sponsor *pick = offers[size_t(rand()) % offers.size()];
    m_profile.signedSponsors[ev.region] = pick->hash;
    m_profile.money += pick->signingBonus;
    m_pendingSponsor = pick->hash;
    printf("[Career] area %u: signed sponsor %s (+%d signing bonus) -> money %d\n", unsigned(ev.region),
           pick->name.c_str(), pick->signingBonus, m_profile.money);
}

// Quick Race also feeds the shared 0x4fb4ac table. Only modes 2/4 (Quick Race / split screen) write:
// career results go through AwardCareerResult, and standalone/direct runs are not a player session.
// Without a career save the entry still goes to RECORDS.REC (the port's sidecar), so it survives.
void FEStartupSequence::RecordQuickRaceTime(int raceNumber, bool reverse, float seconds)
{
    if (IsCareerRun() || (m_raceMode != 2 && m_raceMode != 4))
        return;
    if (raceNumber <= 0 || seconds <= 0.0f)
        return;
    const int ms = int(seconds * 1000.0f + 0.5f);
    const int direction = reverse ? 1 : 0;
    m_records.RecordTime(raceNumber, direction, ms);
    Career::SaveRecords(m_records);
    if (m_profile.valid) {
        m_profile.RecordTime(raceNumber, direction, ms);
        Career::SaveCareer(m_profile);
    }
    printf("[Race] best %d %s %d ms\n", raceNumber, reverse ? "reverse" : "forward", ms);
}

// Drift Quick Race: the 0x4fb4ac entry holds points, not milliseconds (0x00272ee8).
void FEStartupSequence::RecordQuickRaceScore(int raceNumber, bool reverse, int score)
{
    if (IsCareerRun() || (m_raceMode != 2 && m_raceMode != 4))
        return;
    if (raceNumber <= 0 || score <= 0)
        return;
    const int direction = reverse ? 1 : 0;
    m_records.RecordScore(raceNumber, direction, score);
    Career::SaveRecords(m_records);
    if (m_profile.valid) {
        m_profile.RecordScore(raceNumber, direction, score);
        Career::SaveCareer(m_profile);
    }
    printf("[Race] best %d %s %d points\n", raceNumber, reverse ? "reverse" : "forward", score);
}

// UI_OptionsMain (create 0x00202428, ctor 0x00202460, AddOptions 0x00202780): an icon carousel
// with one entry per option category, same base as the main menu. AddOption order is the on-screen
// order (as in UI_Main). Each item's handler 0x00200428..0x00200528 sets DAT_004f12cc (the category
// read by UI_Options, 0x00202b68): 0 Audio, 1 Video, 2 Gameplay, 3 Player, 4 Controls, 5 EA GAMES
// TRAX (Online Options 7 on PC, DAT_00506bd0 > 0xb), 6 Trailers/Bonus (0x002005c0), 8 Credits.
void FEStartupSequence::HandleOptionsMainLoaded(FEPackage *pkg)
{
    static const struct {
        uint32_t tex, label;
        int category;
    } kOptions[] = {
        {0xF37AF144, 0xE76CD783, 0}, // Audio
        {0x8A006328, 0xE8E24508, 1}, // Video
        {0x4DF98FB2, 0xD0CF6EE1, 2}, // Gameplay
        {0xD708EFEF, 0xF760EABE, 3}, // Player
        {0xA04A7B26, 0xD77A22D8, 4}, // Controls
        {0xC52CCBF6, 0xDCFB6B36, 5}, // EA GAMES TRAX (PS2; Online Options on PC)
        {0xCAF8F145, 0xB7326E56, 6}, // Trailers / bonus
        {0x51009E20, 0x0905101F, 8}, // Credits
    };
    std::vector<FEIconScroller::Def> defs;
    m_optionCategories.clear();
    for (const auto &o : kOptions) {
        defs.push_back({o.tex, o.label});
        m_optionCategories.push_back(o.category);
    }
    printf("[Menu] OptionsMain: %zu options\n", defs.size());
    m_scroller.Setup(pkg, defs, m_optionsMainLastSelection);
}

void FEStartupSequence::HandleOptionsMainMessage(uint32_t msg, FEPackage *pkg)
{
    if (m_scroller.Empty() || m_scroller.Package() != pkg)
        return;
    if ((msg == kMsgButtonPressed || msg == kMsgPadAccept) && !m_navPending) {
        size_t sel = m_scroller.Selected();
        m_optionsMainLastSelection = sel;
        if (sel >= m_optionCategories.size())
            return;
        m_optionCategory = m_optionCategories[sel];
        // 0x00202560 (the carousel's accept handler) routes the selected category to its own
        // screen: 0/1/2/3 (and 7 Online on PC) use the generic UI_Options list, 4 Controls opens
        // the controller-mapping screen, 5 EA TRAX the jukebox and 8 Credits its own scroll.
        // Category 6 (the PS2 "Trailers"/bonus item) is a special item (vtable 0x46dee8,
        // 0x002565c0 -> Burnout 3 demo) that 0x00202560 leaves to the item class; we show the
        // UI_Options case-6 bonus-trailer list (0x00204078) as the closest ported content.
        const char *dest = "UI_Options.fng";
        switch (m_optionCategory) {
        case 4: dest = "UI_OptionsController.fng"; break;
        case 5: dest = "EA_Trax_Jukebox.fng"; break;
        case 8: dest = "Credits.fng"; break;
        default: break;
        }
        printf("[Menu] option category %d -> %s\n", m_optionCategory, dest);
        NavigateTo(pkg->GetName(), dest);
        return;
    }
    m_scroller.HandleMessage(msg, TickSeconds());
}

// UI_Options (create 0x00202b30, ctor 0x00202b68, dispatch 0x00204160). The ctor sets the title
// (0x42adb44c) to the category label and 0x00204160 hides the calibration screen
// (ColorCal_Logo/ColorCal_Blurb/Colour_Calibration_Backing, the "Player Select"/"Player 1" texts
// and the list's scroll arrows). The Audio list (0x002034c8) creates 7 volume sliders plus the
// Audio Mode toggle (vtable 0x0046ebf8; 0 Mono / 1 Stereo / 2 Dolby Pro Logic II).
// 0x00204160's prelude and the ctor's title step: hide the colour-calibration layer (the screen's
// intro scripts fade the whole Options layer in, so this runs again on FEHASH_INITCOMPLETE).
void FEStartupSequence::ApplyOptionsPreamble()
{
    // 0x00259ee8 / 0x00259f08 resolve through 0x00292980, which also searches library packages.
    // Object names hash uppercase (0x00284610).
    m_fe->SetVisible(0x010cba23, false);
    // 0x00203b18 (Player) re-shows the "Player 1"/"Player Select" (0xc90dc4ab) header and the
    // 0x97b71dde icon; every other category hides them.
    const bool player = m_optionCategory == 3;
    m_fe->SetVisible(0x97b71dde, player);
    m_fe->SetVisible(0x7326e44c, player); // Player 1
    m_fe->SetVisible(0xc90dc4ab, player); // Player Select / EA Messenger (texture set below)
    if (player) {
        // 0x00203b18: mode 8 shows "EA Messenger" (0xb0fcf98e), otherwise "Player Select" (0xb35b3ac3).
        if (FEObject *o = m_fe->FindObject(0xc90dc4ab))
            FENativeInterface::SetTextureHash(o, m_raceMode == 8 ? 0xb0fcf98eu : 0xb35b3ac3u);
    }
    // Video (0x00203770) is the only category that keeps the colour-calibration layer on screen
    // (it re-shows ColorCal_Logo/Blurb/Backing via 0x00259eb0); every other category hides it.
    const bool calibration = m_optionCategory == 1;
    m_fe->SetVisible(bStringHash("COLORCAL_LOGO"), calibration);
    m_fe->SetVisible(bStringHash("COLORCAL_BLURB"), calibration);
    m_fe->SetVisible(bStringHash("COLOUR_CALIBRATION_BACKING"), calibration);
    m_fe->SetVisible(0x444969fd, false); // scroll up
    m_fe->SetVisible(0x444969fe, false); // scroll down
    m_fe->SetVisible(0x688a1551, false); // SCROLLBAR_BACKING (fits on one page)
}

void FEStartupSequence::HandleOptionsLoaded(FEPackage *pkg)
{
    ApplyOptionsPreamble();

    static const uint32_t kTitles[] = {0xE76CD783, // Audio
                                       0xE8E24508, // Video
                                       0xD0CF6EE1, // Gameplay
                                       0xF760EABE, // Player
                                       0,          // Controls: the PS2 disc sets no title and has no handler
                                       0xDCFB6B36, // EA GAMES TRAX
                                       0xD10D7477, // Trailers / bonus
                                       0,          // Online Options (PC)
                                       0x0905101F}; // Credits
    const int cat = m_optionCategory;
    if (cat >= 0 && cat < int(sizeof(kTitles) / sizeof(kTitles[0])) && kTitles[cat])
        FENativeInterface::SetLanguageHash(pkg, 0x42adb44c, kTitles[cat]);

    std::vector<FEWidgetMenu::Widget> w;
    if (cat == 0) {
        static const struct {
            uint32_t label;
            int index;
        } kVolumes[] = {
            {0x26782C3E, 0}, // Master Volume   (0x004f12f4)
            {0x418E681D, 1}, // Menu Music      (0x004f12fc)
            {0xDF21EAC2, 2}, // Game Music      (0x004f1300)
            {0x9E5FB82A, 3}, // Speech & Movie  (0x004f12f8)
            {0xFD487543, 4}, // Sound Effects   (0x004f1304)
            {0x69B745C6, 5}, // Tire            (0x004f130c)
            {0xA2B1F888, 6}, // Engine          (0x004f1308)
        };
        for (const auto &v : kVolumes) {
            const int index = v.index;
            w.push_back({v.label,
                         [this, index](uint32_t &, std::string &text) {
                             char buf[8];
                             snprintf(buf, sizeof(buf), "%d%%", int(m_audioVolume[index] * 100.0f + 0.5f));
                             text = buf;
                         },
                         [this, index](int dir) {
                             // 0x002006d8: min 0, max 1, step uGpffff9644.. (10% in practice).
                             constexpr float kStep = 0.1f;
                             float value = m_audioVolume[index] + float(dir) * kStep;
                             m_audioVolume[index] = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
                         }});
        }
        w.push_back({0x2881AB87, // Audio Mode
                     [this](uint32_t &label, std::string &) {
                         static const uint32_t kModes[] = {0xC50FA35F, 0x55DA8BF8, 0xF6FAFF24};
                         label = kModes[m_audioMode % 3];
                     },
                     [this](int dir) { m_audioMode = (m_audioMode + (dir < 0 ? 2 : 1)) % 3; }});
    } else if (cat == 1) {
        // FUN_00203770: a Widescreen toggle plus two float sliders (Brightness 0x004f12d4,
        // Contrast 0x004f12d8, both clamped to [-15,15] by 0x00201108 / 0x00201218) drawn over
        // the colour-calibration reference image. The slider rows ship as textures, not labels.
        auto onOff = [](bool v) { return v ? 0x0000063cu : 0x0000ccfau; }; // "On" / "Off"
        w.push_back({0xD3588630, // Widescreen (0x00200f88/0x00201008)
                     [this, onOff](uint32_t &l, std::string &) { l = onOff(m_widescreen); },
                     [this](int) { m_widescreen = !m_widescreen; }});
        auto signedValue = [](int v) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%+d", v);
            return std::string(buf);
        };
        w.push_back({0, // Brightness
                     [this, signedValue](uint32_t &, std::string &text) { text = signedValue(m_brightness); },
                     [this](int dir) {
                         m_brightness += dir;
                         if (m_brightness < -15) m_brightness = -15;
                         if (m_brightness > 15) m_brightness = 15;
                     }});
        w.push_back({0, // Contrast
                     [this, signedValue](uint32_t &, std::string &text) { text = signedValue(m_contrast); },
                     [this](int dir) {
                         m_contrast += dir;
                         if (m_contrast < -15) m_contrast = -15;
                         if (m_contrast > 15) m_contrast = 15;
                     }});
    } else if (cat == 2) {
        // FUN_002038c8: six toggles then a Save slider (vtable 0x0046e358), in AddOption order.
        auto onOff = [](bool v) { return v ? 0x0000063cu : 0x0000ccfau; }; // "On" / "Off"
        // 0x002013f0/0x00201490: On/Off, stored 0 or 127.
        w.push_back({0xF26A5CBF, // Jump Camera
                     [this, onOff](uint32_t &l, std::string &) { l = onOff(m_jumpCamera); },
                     [this](int) { m_jumpCamera = !m_jumpCamera; }});
        // 0x002014e8/0x00201568: On/Off.
        w.push_back({0x85A6CE05, // Rearview Mirror
                     [this, onOff](uint32_t &l, std::string &) { l = onOff(m_rearviewMirror); },
                     [this](int) { m_rearviewMirror = !m_rearviewMirror; }});
        // 0x002015b0/0x00201648: MPH (0) / KM/H (1).
        w.push_back({0xF86A6272, // Speedometer Units
                     [this](uint32_t &l, std::string &) { l = m_speedoUnits ? 0x8569A25Fu : 0x8569AB44u; },
                     [this](int) { m_speedoUnits ^= 1; }});
        // 0x00201698/0x00201738: Easy / Medium / Hard.
        w.push_back({0xBC0E6C55, // Career Difficulty
                     [this](uint32_t &l, std::string &) {
                         static const uint32_t kModes[] = {0xDBBB4E8F, 0xCAB262DE, 0xDBBCF37C};
                         l = kModes[m_careerDifficulty % 3];
                     },
                     [this](int dir) { m_careerDifficulty = (m_careerDifficulty + (dir < 0 ? 2 : 1)) % 3; }});
        // 0x00201908/0x002019a8: Off / Static / Rotate.
        w.push_back({0xC6269082, // Explore Map Mode
                     [this](uint32_t &l, std::string &) {
                         static const uint32_t kModes[] = {0x0000CCFA, 0xF75595F2, 0xF4B00E99};
                         l = kModes[m_exploreMapMode % 3];
                     },
                     [this](int dir) { m_exploreMapMode = (m_exploreMapMode + (dir < 0 ? 2 : 1)) % 3; }});
        // 0x002017d0/0x00201888: Off / Static / Rotate; split screen (0x004f12e9) only 0..1.
        w.push_back({0x9FA5EC9E, // Race Map Mode
                     [this](uint32_t &l, std::string &) {
                         static const uint32_t kModes[] = {0x0000CCFA, 0xF75595F2, 0xF4B00E99};
                         l = kModes[m_raceMapMode % 3];
                     },
                     [this](int dir) {
                         const int n = m_raceMode == 4 ? 2 : 3;
                         m_raceMapMode = (m_raceMapMode + (dir < 0 ? n - 1 : 1)) % n;
                     }});
        // 0x00201ad8/0x00201c18: Save slider, states Off (0,0) / Prompt (1,0) / Autosave (1,1).
        w.push_back({0x996463E6, // Save
                     [this](uint32_t &l, std::string &) {
                         static const uint32_t kModes[] = {0x1B7ABF90, 0x1B7ABF8F, 0x1B7ABF8E};
                         l = kModes[m_saveState % 3];
                     },
                     [this](int dir) { m_saveState = (m_saveState + (dir < 0 ? 2 : 1)) % 3; }});
    } else if (cat == 3) {
        // FUN_00203b18: the player/HUD options, in AddOption order. Most rows switch the player
        // block byte 0x004f1330 + idx*0x40 (idx 0 here); Camera/Steer/Split are 0x004f1354/1350/1358.
        auto onOff = [](bool v) { return v ? 0x0000063cu : 0x0000ccfau; }; // "On" / "Off"
        // 0x00256480/0x002563f8: Auto (0x61cb0720) / Manual (0x1b3a2485).
        w.push_back({0xD31407E7, // Transmission
                     [this](uint32_t &l, std::string &) { l = m_pTransmissionManual ? 0x1b3a2485u : 0x61cb0720u; },
                     [this](int) { m_pTransmissionManual = !m_pTransmissionManual; }});
        // 0x00256328/0x002562a8: On/Off.
        w.push_back({0xDF06ED22, // Stability Control
                     [this, onOff](uint32_t &l, std::string &) { l = onOff(m_pStability); },
                     [this](int) { m_pStability = !m_pStability; }});
        // 0x002561c8/0x00256108: Bumper / Hood / Close / Far / Super Far.
        w.push_back({0xF6CCDC5F, // Camera
                     [this](uint32_t &l, std::string &) {
                         static const uint32_t kModes[] = {0xC3E9AE58u, 0x414F19D7u, 0x5AE3441Fu, 0x1EA4CEC2u,
                                                           0x916039B4u};
                         l = kModes[m_pCamera % 5];
                     },
                     [this](int dir) { m_pCamera = (m_pCamera + (dir < 0 ? 4 : 1)) % 5; }});
        // 0x00255820/0x00255788: left stick (0x32675d4f) / right stick (0x649da17f); shows "Wheel"
        // (0xe8f3d566) when a wheel is plugged in, which the native build never has.
        w.push_back({0x93FEE5ED, // Steer With:
                     [this](uint32_t &l, std::string &) { l = m_pSteerRight ? 0x649DA17Fu : 0x32675D4Fu; },
                     [this](int) { m_pSteerRight = !m_pSteerRight; }});
        // 0x00256048/0x00255fe8: On/Off.
        w.push_back({0xAC148579, // Gauges
                     [this, onOff](uint32_t &l, std::string &) { l = onOff(m_pGauges); },
                     [this](int) { m_pGauges = !m_pGauges; }});
        w.push_back({0x82CD8F92, // Race Position
                     [this, onOff](uint32_t &l, std::string &) { l = onOff(m_pRacePosition); },
                     [this](int) { m_pRacePosition = !m_pRacePosition; }});
        w.push_back({0x969F96A5, // Lap Info
                     [this, onOff](uint32_t &l, std::string &) { l = onOff(m_pLapInfo); },
                     [this](int) { m_pLapInfo = !m_pLapInfo; }});
        // 0x00255ba8/0x00255af0: Leader / Lap Record / Best Lap / Last Lap / Off.
        w.push_back({0x084BC378, // Split Time
                     [this](uint32_t &l, std::string &) {
                         static const uint32_t kModes[] = {0xD93F8F55u, 0xC44D3943u, 0x17FAFC32u, 0x1EA459F8u,
                                                           0x0000CCFAu};
                         l = kModes[m_pSplitTime % 5];
                     },
                     [this](int dir) { m_pSplitTime = (m_pSplitTime + (dir < 0 ? 4 : 1)) % 5; }});
        w.push_back({0xC03F9F19, // Score
                     [this, onOff](uint32_t &l, std::string &) { l = onOff(m_pScore); },
                     [this](int) { m_pScore = !m_pScore; }});
        w.push_back({0xC93FBFB5, // Leaderboard
                     [this, onOff](uint32_t &l, std::string &) { l = onOff(m_pLeaderboard); },
                     [this](int) { m_pLeaderboard = !m_pLeaderboard; }});
        w.push_back({0xBFB34120, // Ask for Transmission
                     [this, onOff](uint32_t &l, std::string &) { l = onOff(m_pAskTransmission); },
                     [this](int) { m_pAskTransmission = !m_pAskTransmission; }});
    } else if (cat == 6) {
        // FUN_00204078: three bonus-trailer buttons (labels are literal strings baked into the
        // binary: SSX / NFL Street / NASCAR Thunder; Accept plays the trailer, not ported here).
        for (int i = 0; i < 3; ++i)
            w.push_back({0, [](uint32_t &, std::string &text) { text.clear(); }, [](int) {}});
    }
    printf("[Menu] Options: category %d, %zu rows\n", cat, w.size());
    m_widgets.Setup(pkg, std::move(w));
    if (cat == 1) {
        // The Brightness/Contrast rows have no language label (their names ship as textures in
        // UI_Options.fng), so set the row names literally after the generic title pass.
        FENativeInterface::SetString(pkg, bStringHash("OPTION_NAME_2"), "Brightness");
        FENativeInterface::SetString(pkg, bStringHash("OPTION_NAME_3"), "Contrast");
    } else if (cat == 6) {
        // Trailer row names are literal strings, not language labels.
        FENativeInterface::SetString(pkg, bStringHash("OPTION_NAME_1"), "SSX");
        FENativeInterface::SetString(pkg, bStringHash("OPTION_NAME_2"), "NFL Street");
        FENativeInterface::SetString(pkg, bStringHash("OPTION_NAME_3"), "NASCAR Thunder");
    }
}

void FEStartupSequence::HandleOptionsMessage(uint32_t msg, FEPackage *pkg)
{
    if (m_widgets.Package() != pkg)
        return;
    if (msg == kMsgInitComplete) {
        ApplyOptionsPreamble();
        m_widgets.OnInitComplete();
        return;
    }
    if (msg == kMsgScreenTick) {
        // The list's scroll widgets re-show themselves each tick; keep the calibration layer and
        // the (unneeded) scroll arrows hidden.
        ApplyOptionsPreamble();
        return;
    }
    if (msg == kMsgPadBack) {
        // Leave via the screen stack (Back from UI_Options returns to the options carousel).
        if (!m_navPending && !m_backStack.empty()) {
            std::string prev = m_backStack.back();
            m_backStack.pop_back();
            printf("[Menu] %s -> back to %s\n", pkg->GetName(), prev.c_str());
            m_navPending = true;
            m_engine->QueuePackageSwitch(prev.c_str(), 0xff);
        }
        return;
    }
    if (msg == kMsgButtonPressed || msg == kMsgPadAccept) {
        // Values apply immediately; Accept does not advance.
        if (msg == kMsgButtonPressed)
            printf("[Menu] Options: audio master %d%% mode %d\n", int(m_audioVolume[0] * 100.0f + 0.5f),
                   m_audioMode);
        return;
    }
    m_widgets.HandleMessage(msg);
}

void FEStartupSequence::SwitchFlow(const char *from, const char *to)
{
    if (!to)
        return;
    printf("[Menu] %s -> %s\n", from, to);
    m_widgets.Clear();
    m_navPending = true;
    m_engine->QueuePackageSwitch(to, 0xff);
}

float FEStartupSequence::TickSeconds() const
{
    return m_nowMs > m_lastTickMs ? float(m_nowMs - m_lastTickMs) * 0.001f : 0.016f;
}

void FEStartupSequence::NavigateTo(const char *from, const char *target)
{
    printf("[Menu] %s -> %s\n", from, target);
    m_widgets.Clear();
    m_backStack.push_back(from);
    m_navPending = true;
    m_engine->QueuePackageSwitch(target, 0xff);
}

// StartRace (0x0025d4d8) builds the race parameters, then queues "UnloadingFrontEnd" (0x002e0098)
// and "LoadingRegion" (0x002ddb40). The region bundle comes from the chosen track's race record
// (+0x40); Free Run has no track and uses the city, L4RA.
void FEStartupSequence::RequestRace()
{
    m_raceRegion = "L4RA";
    for (const RaceInfo &r : m_fe->Races())
        if (r.number == m_raceNumber && r.region[0])
            m_raceRegion = r.region;
    // Career opens in the city with no track chosen. The port starts it in the garage/crib region
    // (L4RG, the region #118's free run and #130's lane graph use): the career crib is in it (ISSUES
    // #121 moved garage scenery there) and its route-less spawn is on a road. L4RA's route-less spawn
    // drops the car on a slope (ISSUES #138).
    if (m_raceNumber == 0 && (m_raceMode == 1 || m_raceMode == 8))
        m_raceRegion = "L4RG";
    m_raceRequested = true;
    printf("[Race] start: type %d track %d region %s\n", m_raceType, m_raceNumber, m_raceRegion.c_str());
}

bool FEStartupSequence::TakeRaceRequest(std::string &region)
{
    if (!m_raceRequested)
        return false;
    m_raceRequested = false;
    region = m_raceRegion;
    return true;
}

void FEStartupSequence::ReturnFromRace()
{
    m_backStack.clear();
    m_navPending = false;
    m_lastPad = 0;
    if (m_careerEventRun) {
        // A career event started from the world map (ISSUES #142): back to the crib, as UG2 does.
        m_careerEventRun = false;
        m_raceNumber = 0;
        m_raceType = 9;
        m_cribRequested = true;
        RequestRace();
        printf("[Career] event over -> crib\n");
        return;
    }
    printf("[Menu] race -> UI_Main.fng\n");
    m_engine->PushPackage("UI_Main.fng", 0, 0xff);
}

// In-race pause menu: UI_Pause.fng (factory 0x00512558: create 0x00237418, ctor 0x00237450,
// AddOptions 0x00237b28 -> race options 0x002381e8, messages 0x002374c8). Options are icon
// scroller items (texture hash, label hash) whose Act (vtable slot 5) runs on BUTTON_PRESSED:
// Continue 0x00251e18 (unpause), Restart 0x00251c40 (confirm dialog -> 0xe1a57d51 -> 0x002385f8),
// Options 0x00251bd0 (UI_PauseOptionsMain, not ported: left out), Quit Race 0x002518e8 (confirm ->
// 0x30ed2368 -> 0x00238698). Career-only items (0x00237d28 / 0x00237da0) are not shown in quick
// race. The confirm dialogs are skipped.
void FEStartupSequence::HandlePauseLoaded(FEPackage *pkg)
{
    std::vector<FEIconScroller::Def> defs = {
        {0x12bb5ea2, 0xf6601088}, // Continue
        {0xb295a6b6, 0x80833fc2}, // Restart (0x00237d68: not in drift-trial type 7 or online)
        {0x4c9e34e6, 0xc7e1420c}, // Quit Race
    };
    m_pauseActions = {kPauseResume, kPauseRestart, kPauseQuit};
    m_pauseAction = kPauseNone;
    m_pauseTicks = 0;
    m_scroller.Setup(pkg, defs, 0);
    m_scroller.AllowFade();
}

void FEStartupSequence::HandlePauseMessage(uint32_t msg, FEPackage *pkg)
{
    if (m_scroller.Package() != pkg || m_scroller.Empty())
        return;
    if (msg == kMsgButtonPressed || msg == kMsgPadAccept) {
        size_t sel = m_scroller.Selected();
        if (sel < m_pauseActions.size())
            m_pauseAction = m_pauseActions[sel];
        return;
    }
    if (msg == kMsgScreenTick)
        ++m_pauseTicks;
    // The Start press that opened the menu arrives here too: only a later press resumes.
    if (msg == kMsgPadBack || (msg == kMsgPadStart && m_pauseTicks > 2)) { // 0x002387a0: Back / Start
        m_pauseAction = kPauseResume;
        return;
    }
    m_scroller.HandleMessage(msg, TickSeconds());
}
