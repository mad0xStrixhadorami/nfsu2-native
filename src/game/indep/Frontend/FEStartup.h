#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "game/indep/Frontend/FEIconScroller.h"

#include "Speed/Indep/Src/FEng/FEObject.h"
#include "Speed/Indep/Src/FEng/FEPackage.h"
#include "Speed/Indep/Src/FEng/FEngine.h"

class FENativeInterface;

// Front-end startup sequence and screen handlers, decompiled from UG2 PAL:
//   - BootFlowManager (0x0023ae10, 0x0023b0b8, 0x0023b0e0, 0x0023b118, 0x0023b258)
//   - PAL startup package table at 0x004e6bd0 (NTSC at 0x004e6ba0)
//   - Screen handlers:
//       LS_LangSelect.fng   (0x0023bf18) - language select (stand-in: skipped)
//       MC_Bootup.fng       (0x0023b410) - memory-card boot check (stand-in: skipped)
//       LS_EAlogo.fng       (0x0023b710) - EA logo movie (advances on 0xc3960eb9)
//       LS_THXMovie.fng     (0x0023b908) - THX movie (advances on 0xc3960eb9)
//       LS_PSAMovie.fng     (0x0023b7f8) - PSA movie (advances on 0xc3960eb9 / Accept / Start)
//       UG_LS_IntroFMV.fng  (0x0023b610) - intro FMV (advances on 0xc3960eb9 / Accept / Start)
//       UG_LS_Splash.fng    (0x0023c2d8, 0x0023c4a0) - title screen
//       MC_Background.fng   (0x0023d098 in 0x0023b118) - background memcard check (skipped)
//       UI_Main.fng         - main menu destination
class FEStartupSequence {
public:
    static constexpr uint32_t kMsgInitComplete = 0x35f8620b; // FEHASH_INITCOMPLETE
    static constexpr uint32_t kMsgScreenTick = 0xc98356ba;   // FEMSG_SCREEN_TICK
    static constexpr uint32_t kMsgMovieDone = 0xc3960eb9;    // movie finished / missing (0x00290cb8)
    static constexpr uint32_t kMsgPadAccept = 0x406415e3;    // __PAD_ACCEPT__
    static constexpr uint32_t kMsgPadBack = 0x911ab364;      // __PAD_BACK__
    static constexpr uint32_t kMsgPadStart = 0xb5af2461;     // __PAD_START__
    static constexpr uint32_t kMsgPadLeft = 0x9120409e;      // __PAD_LEFT__
    static constexpr uint32_t kMsgPadRight = 0xb5971bf1;     // __PAD_RIGHT__
    static constexpr uint32_t kMsgPadUp = 0x72619778;        // __PAD_UP__
    static constexpr uint32_t kMsgPadDown = 0x911c0a4b;      // __PAD_DOWN__
    static constexpr uint32_t kMsgButtonPressed = 0x0c407210; // __BUTTON_PRESSED__

    FEStartupSequence(FEngine *engine, FENativeInterface *fe, bool isPal = true);

    // 0x00292280: start the boot flow by pushing the first package in the sequence.
    void Start();

    // 0x0023b118: advance from fromPkgName to the next package in the startup sequence.
    void Advance(const char *fromPkgName);

    // 0x0023b258: jump back to a named screen in the startup sequence (e.g. UG_LS_IntroFMV.fng).
    bool JumpToScreen(const char *fromPkgName, const char *targetPkgName);

    // Called from FENativeInterface::PackageWasLoaded (0x00290db8 -> 0x0028e518).
    void OnPackageLoaded(FEPackage *pkg);

    // Called from FENativeInterface::NotificationMessage (0x0028e5f0).
    void NotificationMessage(uint32_t msg, FEObject *obj, uint32_t param1, FEPackage *pkg);

    // Broadcast FEMSG_SCREEN_TICK (0xc98356ba) to active screens (FEPackageManager::Tick).
    void Tick(uint32_t nowMs);

    bool IsFinished() const { return m_finished; }
    const char *CurrentPackage() const;

private:

    void HandleSplashLoaded(FEPackage *pkg);
    void HandleSplashMessage(uint32_t msg, FEPackage *pkg);

    void HandleMainLoaded(FEPackage *pkg);
    void HandleMainMessage(uint32_t msg, FEPackage *pkg);
    void HandleQRModeSelectLoaded(FEPackage *pkg);
    void HandleQRModeSelectMessage(uint32_t msg, FEPackage *pkg);
    const char *QuickRaceNext(const char *screen) const;
    const char *QuickRaceBack(const char *screen) const;
    void SwitchFlow(const char *from, const char *to);
    float TickSeconds() const;
    void NavigateTo(const char *from, const char *target);

    FEngine *m_engine;
    FENativeInterface *m_fe;
    bool m_isPal;
    std::vector<std::string> m_screens;
    size_t m_current = 0;
    bool m_finished = false;

    // UG_LS_Splash state (0x0023c2d8 / 0x0023c4a0):
    uint32_t m_nowMs = 0;
    uint32_t m_lastTickMs = 0;
    uint32_t m_splashStartMs = 0;     // param_1[0x15]
    uint32_t m_copyrightStartMs = 0;  // param_1[0x14]
    bool m_allowContinue = false;     // iGpffffd838 (0x00566e28)
    bool m_splashFadedIn = false;
    bool m_splashAdvanced = false;

    // Icon carousel of the current menu screen (UI_Main, UI_QRModeSelect, ...).
    FEIconScroller m_scroller;
    size_t m_mainLastSelection = 1; // 0x001e6ea0: FEngSetLastButton(pkg, 2) -> Quick Race
    size_t m_modeLastSelection = 0;
    std::vector<int> m_modeTypes;   // race type per mode option
    int m_raceMode = 0;             // DAT_00506b50: 1 career, 2 quick race, 4 split screen, 0x10 options, 0x20 customize
    int m_raceType = 0;             // DAT_004f13b0: 0 circuit .. 10 street x

    // Menu navigation: screens to return to on Back, and a guard so one press acts once
    // (BUTTON_PRESSED and PAD_ACCEPT both arrive for the same press).
    std::vector<std::string> m_backStack;
    bool m_navPending = false;
};
