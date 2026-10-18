#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "game/indep/Frontend/FEIconScroller.h"
#include "game/indep/Frontend/FEWidgetMenu.h"
#include "game/indep/Frontend/FENative.h"

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
    void HandleQRTrackSelectLoaded(FEPackage *pkg);
    void HandleQRTrackSelectMessage(uint32_t msg, FEPackage *pkg);
    void RefreshTrackInfo(FEPackage *pkg);
    void HandleQRModeOptionsLoaded(FEPackage *pkg);
    void HandleQRModeOptionsMessage(uint32_t msg, FEPackage *pkg);
    void HandleQRCarSelectLoaded(FEPackage *pkg);
    void CycleCarFilter(int direction, FEPackage *pkg);
    void ApplyCarFilterLabel(FEPackage *pkg);
    void HandleOptionsMainLoaded(FEPackage *pkg);
    void HandleOptionsMainMessage(uint32_t msg, FEPackage *pkg);
    void HandleOptionsLoaded(FEPackage *pkg);
    void HandleOptionsMessage(uint32_t msg, FEPackage *pkg);
    void ApplyOptionsPreamble();
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
    // UI_OptionsMain (0x00202460/0x00202780): option carousel. Each item sets DAT_004f12cc to
    // its category (read by UI_Options, 0x00202b68) before switching there.
    size_t m_optionsMainLastSelection = 0;
    std::vector<int> m_optionCategories;
    int m_optionCategory = 0;
    // UI_Options audio settings (0x004f12f4, 0x004f12f8, 0x004f12fc, 0x004f1300, 0x004f1304,
    // 0x004f1308, 0x004f130c) and audio mode (0x004f131c: 0 Mono, 1 Stereo, 2 Dolby Pro Logic II).
    // Template init 0x002006d8 etc. clamps to [0,1] with a per-row step (uGpffff9644..).
    float m_audioVolume[7] = {0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f};
    int m_audioMode = 1;
    // UI_Options Gameplay toggles (0x004f12ec Jump Camera stored 0/127, 0x004f12e0 Rearview
    // Mirror, 0x004f12e8 Speedometer Units 0 MPH / 1 KM-H, 0x004f15e4 Career Difficulty 0..2,
    // 0x004f12ea Explore Map Mode, 0x004f12e9 Race Map Mode, 0x004fb4a0/a4 Save 0 Off/1 Prompt/
    // 2 Autosave). All are BSS and default to 0 in the binary.
    bool m_jumpCamera = false;
    bool m_rearviewMirror = false;
    int m_speedoUnits = 0;
    int m_careerDifficulty = 0;
    int m_exploreMapMode = 0;
    int m_raceMapMode = 0;
    int m_saveState = 0;
    // UI_Options Video settings (0x00203770): Widescreen toggle (0x004f12dc), Brightness
    // (0x004f12d4 float) and Contrast (0x004f12d8 float), each clamped to [-15,15] by the slider
    // getters 0x00201108 / 0x00201218 against the colour-calibration reference image.
    bool m_widescreen = false;
    int m_brightness = 0;
    int m_contrast = 0;
    // UI_Options Player settings (0x00203b18), one 0x40-byte block per player (0x004f1330 + idx*0x40;
    // Camera/Steer/Split live at 0x004f1354/0x004f1350/0x004f1358 + idx*0x10). Index 0 only here.
    bool m_pTransmissionManual = false; // 0x004f1359: 0 Auto / 1 Manual
    bool m_pStability = false;          // 0x004f135a: Stability Control
    int m_pCamera = 0;                  // 0x004f1354: 0 Bumper / 1 Hood / 2 Close / 3 Far / 4 Super Far
    bool m_pSteerRight = false;         // 0x004f1350: 0 left stick / 1 right stick
    bool m_pGauges = false;             // 0x004f1330
    bool m_pRacePosition = false;       // 0x004f1334
    bool m_pLapInfo = false;            // 0x004f1338
    int m_pSplitTime = 0;               // 0x004f1358: 0 Leader / 1 Lap Record / 2 Best Lap / 3 Last Lap / 4 Off
    bool m_pScore = false;              // 0x004f133c
    bool m_pLeaderboard = false;        // 0x004f1344
    bool m_pAskTransmission = false;    // 0x004f1348
    int m_raceMode = 0;             // DAT_00506b50: 1 career, 2 quick race, 4 split screen, 0x10 options, 0x20 customize
    int m_raceType = 0;
    // DAT_00566764 (0x001f50f0 / 0x001f44f0): car-select filter. 1 Stock Cars, 2 My Cars,
    // 3 All Cars, 4 Career, 8 SPONSOR CARS, 0x10 Ranked Cars -- an ownership category, not a
    // tuner/muscle class. Only Stock/All have entries without a profile save.
    int m_carFilterState = 1;
    int m_raceNumber = 0;             // chosen track (race settings at 0x004f13b0)
    std::vector<RaceInfo> m_tracks;   // tracks offered by the track select screen
    // Per race type settings (0x004f13b0 + 0x20 + type * 0x20), nfsmw RaceSettings::Default values.
    struct RaceOptions {
        int laps = 2, opponents = 3, difficulty = 1, traffic = 1;
        bool lapKO = false, catchUp = true;
    };
    RaceOptions m_options[16];
    FEWidgetMenu m_widgets;
    uint32_t m_lastPad = 0;             // DAT_004f13b0: 0 circuit .. 10 street x

    // Menu navigation: screens to return to on Back, and a guard so one press acts once
    // (BUTTON_PRESSED and PAD_ACCEPT both arrive for the same press).
    std::vector<std::string> m_backStack;
    bool m_navPending = false;
};
