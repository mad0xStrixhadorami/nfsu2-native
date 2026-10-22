#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "game/indep/Frontend/FEIconScroller.h"
#include "game/indep/Frontend/FEWidgetMenu.h"
#include "game/indep/Frontend/FENative.h"
#include "game/indep/Career/Profile.h"
#include "game/indep/Career/Memcard.h"
#include "game/indep/Career/Events.h"
#include "game/indep/Career/Sponsors.h"

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
    // Shoulder buttons.  0x002bc2f0 (UI_Paint message handler) forwards these to 0x002be5f0,
    // which cycles the base-paint finish GLOSS -> METAL -> PEARL (#168).
    static constexpr uint32_t kMsgPadLTrigger = 0x5073ef13;  // L1
    static constexpr uint32_t kMsgPadRTrigger = 0x260113a7;  // R1
    static constexpr uint32_t kMsgButtonPressed = 0x0c407210; // __BUTTON_PRESSED__

    FEStartupSequence(FEngine *engine, FENativeInterface *fe, bool isPal = true);

    // Car select Accept requested a race; returns the world region to load (e.g. "L4RA") once.
    bool TakeRaceRequest(std::string &region);
    int RaceNumber() const { return m_raceNumber; } // chosen track, 0 for Free Run
    // Back from a race (pause menu Quit Race / post-race Continue): UI_Main.fng.
    void ReturnFromRace();
    // In-race pause menu (UI_Pause.fng, ISSUES.md #122): the chosen action, taken once.
    enum PauseAction { kPauseNone, kPauseResume, kPauseRestart, kPauseQuit };
    PauseAction TakePauseAction()
    {
        PauseAction a = m_pauseAction;
        m_pauseAction = kPauseNone;
        return a;
    }
    int RaceType() const { return m_raceType; } // DAT_004f13b0: 0 circuit .. 10 street x
    int RaceLaps() const { return m_options[m_raceType & 15].laps; }
    int RaceTraffic() const { return m_options[m_raceType & 15].traffic; } // 0 none .. 3 maximum
    int RaceOpponents() const { return m_options[m_raceType & 15].opponents; }
    // Race direction: 0x004f13b0 settings +4 for Quick Race; a career event carries its own slot
    // flag (0x00263848, bit 0) and must not leak into the shared per-type Quick Race settings.
    bool RaceReverse() const { return m_careerEventRun ? m_careerReverse : m_options[m_raceType & 15].reverse; }

    // The front end asked for the career crib (career entry; the world map will ask for the return
    // from a career event the same way). main.cpp takes it once when the world session starts and
    // shows UI_CareerCrib.fng over it.
    bool TakeCribRequest()
    {
        bool r = m_cribRequested;
        m_cribRequested = false;
        return r;
    }

    // The career menu (crib / world map) asked to swap its package (the world map opens from the crib
    // and Back returns to it); main.cpp owns the pushed package, so it performs the swap. Taken once.
    const char *TakeCareerMenuSwitch()
    {
        const char *n = m_careerMenuSwitch;
        m_careerMenuSwitch = nullptr;
        return n;
    }

    // 0x00292280: start the boot flow by pushing the first package in the sequence.
    void Start();

    // The console reads the memory card at boot and keeps the career profile in 0x004f1744; the
    // Quick Race track locks query that profile (0x0026b438 through 0x00271a90, ISSUES #157).
    // Called once the event table is loaded (the dev aids read it).
    void LoadBootProfile();

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
    // True while GenericDialog_SMALL.fng is up: the prompt covers the screen that opened it, and the
    // 3D car preview drawn after the packages must stay behind it (ISSUES #189).
    bool IsMessageDialogOpen() const { return m_dialogPackage != nullptr; }

    // A world session started from a career screen (m_raceMode 1 = Resume, 8 = new career's car
    // select); quick race uses 2/4 and never pays career money.
    bool IsCareerRun() const { return m_raceMode == 1 || m_raceMode == 8; }

    // Race result of a career race: find the 0x34a11 event that runs this race (0x002635d8) and pay
    // its purse/respect, marking it complete on a win. Called from the results screen.
    void AwardCareerResult(int raceNumber, int place, float seconds = 0.0f, int score = 0);
    void MaybeSignSponsor(const Career::Event &ev); // ISSUES #165
    uint32_t m_pendingSponsor = 0;                   // set when a win signed a sponsor

    // Quick Race / non-career best lap for the shared 0x4fb4ac table (0x00272e18). The track select
    // always reads the forward key (item +0x10 is 0), but reverse QR runs are stored under direction
    // 1, as the original table does.
    void RecordQuickRaceTime(int raceNumber, bool reverse, float seconds);
    // Drift result for the same table, read back with 0x00272ee8 (largest score wins).
    void RecordQuickRaceScore(int raceNumber, bool reverse, int score);

private:

    void HandleSplashLoaded(FEPackage *pkg);
    void HandleSplashMessage(uint32_t msg, FEPackage *pkg);

    void HandleMainLoaded(FEPackage *pkg);
    void HandleMainMessage(uint32_t msg, FEPackage *pkg);
    void HandleQRModeSelectLoaded(FEPackage *pkg);
    void HandleQRModeSelectMessage(uint32_t msg, FEPackage *pkg);
    void RequestRace();
    void HandlePauseLoaded(FEPackage *pkg);
    void HandlePauseMessage(uint32_t msg, FEPackage *pkg);
    PauseAction m_pauseAction = kPauseNone;
    std::vector<PauseAction> m_pauseActions; // per scroller option
    int m_pauseTicks = 0;                    // screen ticks since the pause menu opened
    void HandleQRTrackSelectLoaded(FEPackage *pkg);
    void HandleQRTrackSelectMessage(uint32_t msg, FEPackage *pkg);
    void RefreshTrackInfo(FEPackage *pkg);
    void HandleQRModeOptionsLoaded(FEPackage *pkg);
    void HandleQRModeOptionsMessage(uint32_t msg, FEPackage *pkg);
    void HandleQRCarSelectLoaded(FEPackage *pkg);
    void UpdateQRCarSelect(FEPackage *pkg);
    void CycleCarFilter(int direction, FEPackage *pkg);
    void ApplyCarFilterLabel(FEPackage *pkg);
    // Car shop paint flow (ISSUES #167/#168): UI_ChooseCustomizeCategory.fng -> UI_Paint.fng.
    // UI_Paint is two-state (0x002bc2f0): the nine part cards (AddOptions 0x002bcff8) select a
    // colorable part, Accept opens that part's swatch grid (0x002bdf10), Back closes the grid
    // (0x002be150), and Back on the cards leaves the screen.  Base Paint edits the body preview and
    // cycles GLOSS/METAL/PEARL with L1/R1 (0x002be5f0); the other eight parts store their colour
    // per car (#169), but the port's single-mesh car has no model for them to tint.
    void HandleCustomizeCategoryLoaded(FEPackage *pkg);
    void HandleCustomizeCategoryMessage(uint32_t msg, FEPackage *pkg);
    void HandlePerformanceCategoryLoaded(FEPackage *pkg);
    void HandlePerformanceCategoryMessage(uint32_t msg, FEPackage *pkg);
    void HandlePerformancePackageLoaded(FEPackage *pkg);
    void HandlePerformancePackageMessage(uint32_t msg, FEPackage *pkg);
    void RefreshPerformancePackage(FEPackage *pkg);
    void BuyPerformancePackage(FEPackage *pkg);
    // The virtual keyboard (UI_VirtualKeyboard.fng, ISSUES #204): OpenKeyboard pushes it over the
    // current screen; `done` receives the text when DONE / Enter is chosen.
    void OpenKeyboard(const std::string &initial, std::function<void(const std::string &)> done,
                      std::function<void()> cancel = nullptr);
    void HandleKeyboardLoaded(FEPackage *pkg);
    void HandleKeyboardMessage(uint32_t msg, FEPackage *pkg);
    void KeyboardPress(FEPackage *pkg);
    void KeyboardRefresh(FEPackage *pkg);
    void KeyboardFinish(FEPackage *pkg, bool accepted);
public:
    // Typed text from the window (SDL_TEXTINPUT) and the Backspace key; only used while the keyboard is up.
    void OnTextInput(const char *utf8);
    bool KeyboardActive() const { return m_kbActive; }
private:
    void HandleSlidersLoaded(FEPackage *pkg);
    void HandleSlidersMessage(uint32_t msg, FEPackage *pkg);
    void RefreshSliders(FEPackage *pkg);
    void AdjustSlider(FEPackage *pkg, int dir);
    void FinishSliders(FEPackage *pkg);
    void HandleNosLoaded(FEPackage *pkg);
    void HandleNosMessage(uint32_t msg, FEPackage *pkg);
    void RefreshNos(FEPackage *pkg);
    void AdjustNos(FEPackage *pkg, int field, int dir);
    void HandleCardMenuLoaded(FEPackage *pkg);
    void HandleCardMenuMessage(uint32_t msg, FEPackage *pkg);
    void HandleBuyPartsLoaded(FEPackage *pkg);
    void HandleBuyPartsMessage(uint32_t msg, FEPackage *pkg);
    void RefreshBuyParts(FEPackage *pkg);
    void UpdateBuyPartsStats(FEPackage *pkg);
    void BuySelectedPerfPart(FEPackage *pkg);
    bool PerfPartAvailable(uint16_t id);
    void PerfPackagePrice(const PerfPackage &pkg, int &total, int &saving);
    // UI_GenericParts_Browser.fng (factory 0x002a50c0, ctor 0x002a50f8, refresh 0x002a6be0): the
    // Body Shop.  CATEGORY_MASTER/CATEGORY_ICON is the part-slot carousel (0x002a76e8 in
    // 0x002a7e70), MINI_MASTER/PARTS_ICON the item carousel; the selected item's name and price go
    // into the package's own PART_NAME (0xd28b9316) and PRICE (0x380b632) strings.  Every slot
    // card the port can mount is wired up: front/rear bumpers, side skirts, spoilers, hoods, roof
    // scoops, headlights, taillights, exhaust tips, wide-body kits and Carbon Fiber (#185-#193).
    void HandlePartsBrowserLoaded(FEPackage *pkg);
    void HandlePartsBrowserMessage(uint32_t msg, FEPackage *pkg);
    void RefreshPartsBrowserList(FEPackage *pkg);
    void RefreshPartsBrowserInfo(FEPackage *pkg);
    void BuyPartsBrowserPart(FEPackage *pkg);
    Career::OwnedCar *ShopCar();
    // One row of the Body Shop's PARTS_ICON list: the catalogue type it came from, the record's
    // index inside that type, and the slot card's icon (thumbnails are placeholders, #185).
    struct PartsShopItem {
        uint32_t type = 0;
        uint16_t index = 0;
        uint32_t icon = 0;
    };
    const CarPart *SelectedPartsShopPart() const;
    void PreviewPartsBrowserPart(const CarPart &part);
    void PreviewOwnedParts(const Career::OwnedCar *car);
    static bool OwnedCarHasPart(const Career::OwnedCar &car, const CarPart &part);
    // GenericDialog_SMALL.fng (0x0025fc70/0x00260958): the shop's "not enough bank" prompt, which
    // 0x002a7278 opens when the career balance is below the part's price (ISSUES #189).
    void ShowMessageDialog(uint32_t messageLabel);
    // The same one-button prompt with a port-written message.  The split-screen flow is the only
    // user (#194): the original starts a two-player race there, which the port has no second
    // viewport or player for.  The game's own "Not Available" text cannot be borrowed because the
    // language bank keys it by an internal label hash, not by bStringHash of the visible string.
    void ShowMessageDialogText(const char *text);
    // Yes / No prompt (GenericDialog_SMALL's two-button form); `onYes` runs when Yes is chosen.
    void ShowYesNoDialog(const char *text, std::function<void()> onYes);
    void HandleDialogLoaded(FEPackage *pkg);
    bool CloseMessageDialog();
    FEPackage *m_dialogPackage = nullptr;   // pushed over the shop while the prompt is up
    // UI_ChoosePaintCategory.fng (ctor 0x002ba798, AddOptions 0x002bb0f0): Graphics Shop's
    // Paint / Vinyls / Decals / Install Setup carousel.  The port implements the Paint branch;
    // the other three are kept visible as disabled options until their screens are ported.
    void HandlePaintCategoryLoaded(FEPackage *pkg);
    void HandlePaintCategoryMessage(uint32_t msg, FEPackage *pkg);
    void HandlePaintLoaded(FEPackage *pkg);
    void HandlePaintMessage(uint32_t msg, FEPackage *pkg);
    void RefreshPaintSwatches(FEPackage *pkg);
    void PositionPaintHighlight(FEPackage *pkg);
    void RefreshPaintLabel(FEPackage *pkg);
    void RefreshPaintCost(FEPackage *pkg); // 0x002b3e10/0x002bb0a0 total-cost field (#176)
    void EnterPaintGrid(FEPackage *pkg);
    void LeavePaintGrid(FEPackage *pkg, bool restorePreview);
    void ShowPaintGrid(FEPackage *pkg, bool show);
    void RefreshRimPreview(); // Rims/Spinners card -> preview wheel colour (#172)
    void RefreshCaliperPreview(); // Brakes card -> preview caliper colour (#173)
    void RefreshMirrorPreview();  // Mirrors card -> preview mirror colour (#174)
    void RefreshSpoilerPreview(); // Spoiler card -> preview spoiler colour (#175)
    uint32_t PartDefaultSet(size_t part) const;
    bool CyclePaintFinish(int direction);
    void ApplyPaint(size_t part, uint32_t set, uint32_t index);
    bool PopScreen(); // back to the package saved by NavigateTo
    static constexpr size_t kPaintPartCount = 9; // 0x002bcff8 card order
    size_t m_performanceCategorySel = 0;
    uint32_t m_perfCategory = 0;            // the chosen 0x34a15 category (DAT_00514570)
    FEIconScroller m_perfScroller;          // the package cards (#198)
    std::vector<FEObject *> m_perfCards;
    std::vector<int> m_perfCardType;        // 0 first card, 1 package, 2 individual parts
    std::vector<uint32_t> m_perfCardLevel;
    std::vector<uint32_t> m_perfCardLabel;
    size_t m_perfCardSel = 0;
    char m_perfBank[24] = {};
    struct KbKey {
        FEObject *obj = nullptr;
        std::string label;
        float x = 0, y = 0;
    };
    bool m_kbActive = false;
    std::string m_kbText;
    std::vector<KbKey> m_kbKeys;
    size_t m_kbSel = 0;
    bool m_kbCaps = true, m_kbShift = false;
    std::function<void(const std::string &)> m_kbDone;
    std::function<void()> m_kbCancel;
    bool m_dialogYesNo = false;
    int m_dialogYesSel = 0; // 0 = Yes, 1 = No
    std::function<void()> m_dialogYesAction;
    FEIconScroller m_cardScroller;                  // the tuning shop's card menus (#206)
    std::vector<FEObject *> m_cardMenuCards;
    std::vector<std::function<void()>> m_cardActions;
    struct TuneRow {
        int field;
        uint32_t left, right;
    };
    std::unique_ptr<Tune::Session> m_tuneSession;   // the open tuning screen's session (#214)
    std::vector<TuneRow> m_tuneRows;
    size_t m_tuneSel = 0, m_tuneTop = 0;
    int m_tuneCard = 0;                             // 0 Suspension, 1 Drivetrain, 2 Aerodynamics, 3 Tires & Brakes
    bool m_profileSub = false; // the Profile Manager's Create / Delete screen (0x0023cd50)
    std::vector<const PerfPart *> m_buyParts; // the individual-parts list (#199)
    size_t m_buySel = 0, m_buyTop = 0;
    size_t m_customizeCategorySel = 0; // last category carousel slot (0 body .. 4 dyno)
    // Body Shop state (#185): the selected part-slot card and the item selection inside it.
    FEIconScroller m_partsCategoryScroller; // CATEGORY_MASTER / CATEGORY_ICON
    FEIconScroller m_partsScroller;         // MINI_MASTER / PARTS_ICON
    size_t m_partsCategorySel = 0;          // 0 Spoilers, 1 Carbon, 2 Roof, 3 Exhaust, 4 Hoods, 5 Headlights, 6 Taillights
    size_t m_partsSel = 0;                  // selected item in m_partsShopItems
    std::vector<PartsShopItem> m_partsShopItems; // records of the current card in PartCatalog
    FEIconScroller m_paintCategoryScroller; // the four 4b4e3888..8b Graphics Shop cards
    size_t m_paintCategorySel = 0;          // 0 Paint, 1 Vinyls, 2 Decals, 3 Install Setup
    FEIconScroller m_paintScroller;    // the nine 4b4e3888..90 part cards
    size_t m_paintPart = 0;            // selected card (0 Base Paint .. 8 Muffler Tip)
    bool m_paintGridActive = false;    // [0x67] in 0x002bc2f0
    uint32_t m_paintSet = 0;           // current finish set (PaintCatalog::kGloss/Metal/Pearl, #168)
    uint32_t m_paintSel = 0;           // 0-based swatch index in the current set
    uint32_t m_gridOriginalSet = 0;    // finish when the grid opened (grid Back restores it)
    uint32_t m_gridOriginalPaint = 0;  // paint when the grid opened (grid Back restores it)
    uint32_t m_gridOriginalRimSet = 0; // preview rim finish when the grid opened
    uint32_t m_gridOriginalRim = 0;    // preview rim paint when the grid opened
    uint32_t m_gridOriginalCaliperSet = 0; // preview caliper finish when the grid opened
    uint32_t m_gridOriginalCaliper = 0;    // preview caliper paint when the grid opened
    uint32_t m_gridOriginalMirrorSet = 0;  // preview mirror finish when the grid opened
    uint32_t m_gridOriginalMirror = 0;     // preview mirror paint when the grid opened
    uint32_t m_gridOriginalSpoilerSet = 0; // preview spoiler finish when the grid opened
    uint32_t m_gridOriginalSpoiler = 0;    // preview spoiler paint when the grid opened
    uint32_t m_gridOriginalRoofSet = 0;    // preview roof scoop finish when the grid opened
    uint32_t m_gridOriginalRoof = 0;       // preview roof scoop paint when the grid opened
    uint32_t m_gridOriginalEngineSet = 0;  // preview engine accent finish when the grid opened
    uint32_t m_gridOriginalEngine = 0;     // preview engine accent paint when the grid opened
    uint32_t m_gridOriginalExhaustSet = 0; // preview muffler tip finish when the grid opened
    uint32_t m_gridOriginalExhaust = 0;    // preview muffler tip paint when the grid opened
    uint32_t m_partSet[kPaintPartCount] = {}; // finish per card (session + saved per-car)
    uint32_t m_partSel[kPaintPartCount] = {}; // 0-based swatch per card (0 = stock/first)
    uint32_t m_partPaintIndex[kPaintPartCount] = {}; // 1-based saved paint per card (0 = stock)
    bool m_careerExists = false; // DAT_004f0e70: a saved career is present
    bool m_cribRequested = false; // the next world session opens in UI_CareerCrib.fng
    void HandleStartCareerLoaded(FEPackage *pkg);
    void HandleStartCareerMessage(uint32_t msg, FEPackage *pkg);
    // UI_CareerCrib.fng (factory 0x001e74d0, ctor 0x001e7508, AddOptions 0x001e79d8): the career hub
    // (Return to Explore Mode / World Map / Car Select / ...). The original enters the world and shows
    // this package over the 3D crib; main.cpp pushes it at the start of a career free-roam session.
    void HandleCareerCribLoaded(FEPackage *pkg);
    void HandleCareerCribMessage(uint32_t msg, FEPackage *pkg);
    size_t m_cribLastSelection = 0;
    // UI_CareerWorldMap.fng (create 0x001ee0b8, ctor 0x001ee0f0, list 0x001ee590): the career event
    // picker (ISSUES #142). The area index (DAT_004f022c / this+0x254) filters the 0x34a11 events.
    void HandleCareerWorldMapLoaded(FEPackage *pkg);
    void HandleCareerWorldMapMessage(uint32_t msg, FEPackage *pkg);
    void RefreshCareerWorldMap(FEPackage *pkg);
    void StartCareerEvent(const Career::Event &ev);
    const char *m_careerMenuSwitch = nullptr;
    // UI_CareerCarSelect.fng (create 0x001e9ef8, ctor 0x001e9f30, messages 0x001ea0c0, slots 0x001ead20):
    // the garage car picker over the profile's owned cars (ISSUES #148).
    void HandleCreditsLoaded(FEPackage *pkg);
    void HandleCareerCarSelectLoaded(FEPackage *pkg);
    void HandleCareerCarSelectMessage(uint32_t msg, FEPackage *pkg);
    void RefreshCareerCarSelect(FEPackage *pkg);
    // Preview model + body paint of an owned car (ISSUES #167: OwnedCar::paint selects a
    // GLOBALB GLOSS color; 0 keeps the stock paint).
    void ApplyOwnedCarPreview(const Career::OwnedCar &car);
    size_t m_csSel = 0;
    // UI_RewardsSponsor.fng: the signed sponsors, one scroller option per area with a sponsor (#165).
    void HandleSponsorViewLoaded(FEPackage *pkg);
    void HandleSponsorViewMessage(uint32_t msg, FEPackage *pkg);
    void RefreshSponsorView(FEPackage *pkg);
    void ShowSponsorTerms(FEPackage *pkg);
    std::vector<int> m_svAreas;
    std::vector<std::string> m_svLines; // wrapped SPONSOR_DETAILS text
    size_t m_svScroll = 0;
    int m_wmArea = 1;
    size_t m_wmSel = 0;
    std::vector<const Career::Event *> m_wmEvents;
    bool m_careerEventRun = false; // the running world session is a career event started from the map
    bool m_careerReverse = false;  // the event slot's direction flag (0x00263848)
    std::vector<bool> m_trackLocked; // per m_tracks entry: Quick Race lock (0x001facc0 item +8, #157)
    std::vector<int> m_cribActions;
    // Memory-card step (MC_Main.fng, create 0x00225f50). UG2 routes New Career (0x00259328, mode
    // 0x21961) and Career Garage (0x002592c0, mode 0xc11) through it; the port performs the card
    // operation natively and continues to the screen the option asked for.
    void HandleMemcardLoaded(FEPackage *pkg);
    void HandleMemcardMessage(uint32_t msg, FEPackage *pkg);
    void OpenMemcardList();   // MC_Main's CALL button: push MC_List.fng over it (0x00225fb8)
    void FillMemcardList(FEPackage *pkg); // 0x00228348: the 8 file rows from the card
    void ApplyMemcardList(FEPackage *pkg, bool log); // reveal the rows/frame (re-run before each draw)
    void CloseMemcardScreens(); // unload both stacked card screens before leaving them
    void ChooseMemcardFile(); // MC_List Accept: use the highlighted save
public:
    // The card dialog's scripts rewrite the row colours every frame; re-apply the reveal after them.
    void PreRender();
private:
    void BeginCareer(const std::string &name = "PLAYER");          // New Career: create the profile and go to career car select
    void RecordCareerCar();      // career car select accept: store the starter car and save
    void ResumeCareer();         // Resume Career: load the profile and enter the city
    bool LoadCareerProfile();    // load from the card into m_profile (keeps m_careerExists in sync)
    Career::Profile m_profile;   // 0x004f1744 (0xac54 bytes) - see Career/Profile.h
    // The 0x4fb4ac table as the UI reads it. Seeded from m_profile and from RECORDS.REC, so QR times
    // survive without a career save (see Career/Profile.h BestTable).
    Career::BestTable m_records;
    std::string m_memcardNext;   // screen the memory-card step goes to when it finishes
    bool m_memcardCreate = false; // the step creates a profile (New Career) instead of loading one
    std::vector<Career::CardFile> m_memcardFiles; // the card's saves, as MC_List.fng shows them
    int m_memcardSel = 0;
    std::string m_memcardPick;   // the chosen save (empty = none)
    bool m_memcardListOpen = false; // MC_List.fng is pushed over MC_Main.fng
    bool m_memcardFromProfileManager = false; // MC_List.fng was opened by the Profile Manager (#195)
    // UI_ProfileManager.fng (ctor 0x0023c9a0, options 0x0023cbb8): the main menu's three cards
    // (Save / Profile / Load) plus the current profile name.  The cards are built at runtime by the
    // original, so the port clones the package's card art per option (#195).
    void HandleProfileManagerLoaded(FEPackage *pkg);
    void HandleProfileManagerMessage(uint32_t msg, FEPackage *pkg);
    void ProfileManagerAccept();
    void CreateProfileWithKeyboard();
    void DeleteCurrentProfile();
    FEIconScroller m_profileScroller;
    std::vector<FEObject *> m_profileCards; // the cloned card images bound to m_profileScroller
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
    size_t m_startCareerLastSelection = 0;
    std::vector<int> m_careerActions;
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
        bool reverse = false; // settings +4 (0x0025c7b0): 0 Forward / 1 Reverse
    };
    RaceOptions m_options[16];
    FEWidgetMenu m_widgets;
    uint32_t m_lastPad = 0;
    bool m_raceRequested = false;
    std::string m_raceRegion;             // DAT_004f13b0: 0 circuit .. 10 street x

    // Menu navigation: screens to return to on Back, and a guard so one press acts once
    // (BUTTON_PRESSED and PAD_ACCEPT both arrive for the same press).
    std::vector<std::string> m_backStack;
    bool m_navPending = false;
};
