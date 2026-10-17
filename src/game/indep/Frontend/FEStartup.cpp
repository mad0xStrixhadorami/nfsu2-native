#include "game/indep/Frontend/FEStartup.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "Speed/Indep/bWare/Inc/Strings.hpp"
#include "game/indep/Frontend/FENative.h"

namespace {

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
    if (strcasecmp(name, "UI_OptionsMain.fng") == 0) {
        HandleOptionsMainLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_Options.fng") == 0) {
        HandleOptionsLoaded(pkg);
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
    if (pressed && active && active->GetName() && active->IsInputEnabled() &&
        strcasecmp(active->GetName(), "UI_QRCarSelect.fng") == 0) {
        // Stand-in: the real carousel uses UG2 car-select messages (0x36db742/0x36db743).
        if (pressed & FENativeInterface::kPadLeft)
            m_fe->CycleCarPreview(-1);
        else if (pressed & FENativeInterface::kPadRight)
            m_fe->CycleCarPreview(1);
        return;
    }
    FEPackage *list = m_widgets.Package();
    if (pressed && list && list->IsInputEnabled() && m_engine->GetPackageList()->GetFirstPackage() == list) {
        static const struct { uint32_t bit, msg; } kMap[] = {{0x01, kMsgPadUp},    {0x02, kMsgPadDown},
                                                             {0x04, kMsgPadLeft},  {0x08, kMsgPadRight},
                                                             {0x10, kMsgPadAccept}, {0x20, kMsgPadBack}};
        for (auto &m : kMap)
            if (pressed & m.bit) {
                if (strcasecmp(list->GetName(), "UI_Options.fng") == 0)
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
    if (strcasecmp(screen, "UI_QRCarSelect.fng") == 0 && m_raceMode != 0x20)
        return m_raceType == 9 ? "UI_QRModeSelect.fng" : "UI_QRModeOptions.fng";
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
    std::vector<FEIconScroller::Def> defs;
    m_tracks.clear();
    for (const RaceInfo &r : m_fe->Races()) {
        if (!TrackOffered(r, m_raceType))
            continue;
        // Locked tracks (0x00271a90, career progress) are not modelled: all are offered.
        char label[32];
        snprintf(label, sizeof(label), "TRACK_NAME_%d", r.number);
        defs.push_back({TrackIcon(r, m_raceType), bStringHash(label)});
        m_tracks.push_back(r);
    }
    size_t slots = 0;
    for (FEObject *o = pkg->GetFirstObject(); o; o = o->GetNext())
        slots += o->NameHash == bStringHash("TRACK_ICON");
    printf("[Menu] track select: %zu tracks for race type %d (%zu TRACK_ICON objects)\n", m_tracks.size(), m_raceType, slots);
    m_scroller.Setup(pkg, defs, 0, "ICON_MASTER", "TRACK_ICON"); // 0x0029af60 args
    // Title per mode (0x42adb44c): quick race / split screen track select.
    FENativeInterface::SetLanguageHash(pkg, 0x42adb44c, m_raceMode == 4 ? 0x1bf733bd : 0xe5d63bb5);
    RefreshTrackInfo(pkg);
}

void FEStartupSequence::RefreshTrackInfo(FEPackage *pkg)
{
    if (m_tracks.empty())
        return;
    const RaceInfo &r = m_tracks[m_scroller.Selected()];
    char label[32];
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
}

void FEStartupSequence::HandleQRTrackSelectMessage(uint32_t msg, FEPackage *pkg)
{
    if (m_scroller.Package() != pkg)
        return;
    if ((msg == kMsgButtonPressed || msg == kMsgPadAccept) && !m_navPending) {
        if (!m_tracks.empty()) {
            m_raceNumber = m_tracks[m_scroller.Selected()].number; // 0x001fa998: race settings
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
        printf("[Menu] options: laps %d, opponents %d, difficulty %d, traffic %d, lap KO %d, catch up %d\n", o.laps,
               o.opponents, o.difficulty, o.traffic, o.lapKO, o.catchUp);
        SwitchFlow(pkg->GetName(), QuickRaceNext(pkg->GetName()));
        return;
    }
    if (msg == kMsgPadBack && !m_navPending) {
        SwitchFlow(pkg->GetName(), QuickRaceBack(pkg->GetName()));
        return;
    }
    m_widgets.HandleMessage(msg);
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
        printf("[Menu] option category %d\n", m_optionCategory);
        NavigateTo(pkg->GetName(), "UI_Options.fng");
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
    m_fe->SetVisible(0x97b71dde, false); // JOY_EVENT_BUTTON1
    m_fe->SetVisible(0x7326e44c, false); // Player 1
    m_fe->SetVisible(0xc90dc4ab, false); // Player Select
    m_fe->SetVisible(bStringHash("COLORCAL_LOGO"), false);
    m_fe->SetVisible(bStringHash("COLORCAL_BLURB"), false);
    m_fe->SetVisible(bStringHash("COLOUR_CALIBRATION_BACKING"), false);
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
                                       0,          // Controls (title set elsewhere)
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
    }
    printf("[Menu] Options: category %d, %zu rows\n", cat, w.size());
    m_widgets.Setup(pkg, std::move(w));
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
    m_backStack.push_back(from);
    m_navPending = true;
    m_engine->QueuePackageSwitch(target, 0xff);
}
