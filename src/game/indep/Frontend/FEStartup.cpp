#include "game/indep/Frontend/FEStartup.h"

#include <cstdio>
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

    if (strcasecmp(name, "UG_LS_Splash.fng") == 0) {
        HandleSplashLoaded(pkg);
        return;
    }
    if (strcasecmp(name, "UI_Main.fng") == 0) {
        printf("[Startup] Reached %s (main menu)\n", name);
        m_finished = true;
        HandleMainLoaded(pkg);
    }
}

void FEStartupSequence::NotificationMessage(uint32_t msg, FEObject * /*obj*/, uint32_t /*param1*/,
                                            FEPackage *pkg)
{
    if (!pkg || !pkg->GetName() || pkg->GetControlMask() == 0)
        return;
    const char *name = pkg->GetName();

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
            printf("[Startup] %s: stand-in movie finished (0x%08x)\n", name, msg);
            Advance(name);
        }
        return;
    }

    // 0x0023b7f8 (LS_PSAMovie.fng) and 0x0023b610 (UG_LS_IntroFMV.fng):
    // advance on movie-done (0xc3960eb9), PAD_ACCEPT (0x406415e3), or PAD_START (0xb5af2461).
    if (strcasecmp(name, "LS_PSAMovie.fng") == 0 || strcasecmp(name, "UG_LS_IntroFMV.fng") == 0) {
        if (msg == kMsgMovieDone || msg == kMsgPadAccept || msg == kMsgPadStart) {
            printf("[Startup] %s: stand-in movie finished/skipped (0x%08x)\n", name, msg);
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
    // Decompiled from:
    //   MenuScreen::MenuScreen     (0x00294558)
    //   IconScroller::IconScroller (0x0029c9c8)
    //   IconScrollerMenu           (0x0029d638)
    //   UIMain::UIMain             (0x001e6b78)
    //   UIMain::AddOptions         (0x001e6ea0)
    // 1. MenuScreen ctor (0x00294558): hide OL_ICON_GROUP and EA_MESSENGER_GROUP.
    FENativeInterface::SetVisible(pkg, bStringHash("OL_ICON_GROUP"), false);
    FENativeInterface::SetVisible(pkg, bStringHash("EA_MESSENGER_GROUP"), false);

    // 2. IconScroller ctor (0x0029c9c8): read center of OPTION_MASTER ("0x475288", 0xb04669e3) and hide it.
    m_mainXCenter = 0.0f;
    m_mainYCenter = 0.0f;
    if (FEObject *optMaster = pkg->FindObjectByHash(bStringHash("OPTION_MASTER"))) {
        FENativeInterface::GetCenter(optMaster, m_mainXCenter, m_mainYCenter);
        FENativeInterface::SetObjectVisible(optMaster, false);
    }
    if (FEObject *lockMaster = pkg->FindObjectByHash(bStringHash("LOCK_MASTER")))
        FENativeInterface::SetObjectVisible(lockMaster, false);
    if (FEObject *btnMaster = pkg->FindObjectByHash(bStringHash("BUTTON_MASTER")))
        FENativeInterface::SetObjectVisible(btnMaster, false);

    // 3. UIMain::AddOptions (0x001e6ea0): 7 main menu options on PAL (0x002dbcb8() != 0).
    static constexpr struct {
        uint32_t texHash;
        uint32_t nameHash;
    } kMainDefs[] = {
        {0x03704f3d, 0x5e7e1192}, // Career
        {0x4e6fbb02, 0x24909fd7}, // Quick Race
        {0x883e25d6, 0x6a2b93eb}, // 2 Player Split-Screen
        {0x771849a5, 0x7b63ff85}, // Online
        {0xb0c46023, 0xa53039a3}, // Customize
        {0x6b303856, 0xbcb18f38}, // Profile Manager
        {0x3058fe37, 0xed1f0b2c}, // Options
    };

    m_mainOptions.clear();
    float addPos = 0.0f;
    constexpr float kIconSpacing = 40.0f; // 0x42200000 at 0x0029c9c8 line 29
    for (size_t i = 0; i < sizeof(kMainDefs) / sizeof(kMainDefs[0]); ++i) {
        char optName[32];
        snprintf(optName, sizeof(optName), "OPTION_%zu", i + 1);
        FEObject *obj = pkg->FindObjectByHash(bStringHash(optName));
        if (!obj)
            continue;
        // Prevent FEngine's spatial button map from overriding IconScroller's Left/Right selection.
        obj->Flags |= 0x00800000u; // FF_DontNavigate
        FENativeInterface::SetTextureHash(obj, kMainDefs[i].texHash);
        MainMenuOption opt;
        opt.texHash = kMainDefs[i].texHash;
        opt.nameHash = kMainDefs[i].nameHash;
        opt.obj = obj;
        opt.xPos = addPos;
        FENativeInterface::GetSize(obj, opt.origW, opt.origH);
        opt.origAlpha = obj->GetObjData() ? int(obj->GetObjData()->Col.a) : 255;
        addPos += opt.origW + kIconSpacing;
        m_mainOptions.push_back(opt);
    }

    if (m_mainOptions.empty())
        return;
    if (m_mainSelected >= m_mainOptions.size())
        m_mainSelected = 1; // 0x001e6ea0 line 69: FEngSetLastButton(pkg, 2) -> Quick Race

    m_mainAllowFade = false;
    m_mainScrollPos = -m_mainOptions[m_mainSelected].xPos;
    m_mainScrollTarget = m_mainScrollPos;
    UpdateMainSelection(pkg);
    PositionMainOptions(pkg);
}

void FEStartupSequence::UpdateMainSelection(FEPackage *pkg)
{
    if (!pkg || m_mainOptions.empty())
        return;
    // 0x0029ce70 (IconScroller::SetSelection):
    for (size_t i = 0; i < m_mainOptions.size(); ++i) {
        if (FEObject *o = m_mainOptions[i].obj) {
            if (i == m_mainSelected)
                o->Flags &= ~0x04000000u;
            else
                o->Flags |= 0x04000000u;
        }
    }
    if (FEObject *selObj = m_mainOptions[m_mainSelected].obj)
        pkg->SetCurrentButton(selObj, true);

    // 0x0029de00 (IconScrollerMenu::RefreshHeader): update ICON_TITLE (0x5e7b09c9).
    FENativeInterface::SetLanguageHash(pkg, 0x5e7b09c9, m_mainOptions[m_mainSelected].nameHash);

    // 0x002991c0 / 0x00299480 (ScrollBar::Refresh) & 0x0029d360 (IconScroller::UpdateArrows):
    FEObject *backing = pkg->FindObjectByHash(bStringHash("SCROLLBAR_BACKING"));
    FEObject *handle = pkg->FindObjectByHash(bStringHash("SCROLLBAR_HANDLE"));
    if (backing && handle && m_mainOptions.size() > 1) {
        float bx = 0.0f, by = 0.0f, bw = 0.0f, bh = 0.0f;
        float hx = 0.0f, hy = 0.0f, hw = 0.0f, hh = 0.0f;
        FENativeInterface::GetCenter(backing, bx, by);
        FENativeInterface::GetSize(backing, bw, bh);
        FENativeInterface::GetCenter(handle, hx, hy);
        FENativeInterface::GetSize(handle, hw, hh);
        float left = bx - bw * 0.5f;
        float t = float(m_mainSelected) / float(m_mainOptions.size() - 1);
        float newX = left + hw * 0.5f + t * (bw - hw);
        FENativeInterface::SetCenter(handle, newX, hy);
    }
    FENativeInterface::SetVisible(pkg, bStringHash("SCROLLBAR_ARROW_1"), m_mainSelected > 0);
    FENativeInterface::SetVisible(pkg, bStringHash("SCROLLBAR_ARROW_2"), m_mainSelected + 1 < m_mainOptions.size());
}

void FEStartupSequence::PositionMainOptions(FEPackage *pkg)
{
    if (!pkg || m_mainOptions.empty())
        return;
    // Decompiled from IconScroller::PositionOption (0x0029d0d0), ClipEdges (0x0029d088),
    // Scale/Fade (0x0029bab0, 0x0029d280):
    constexpr float kScrollerWidth = 480.0f;   // 0x43f00000 in 0x0029d638
    constexpr float kUnselectedScale = 0.80f;  // DAT_00563034 in 0x0029d0d0
    const float halfWidth = kScrollerWidth * 0.5f;
    const float leftEdge = m_mainXCenter - halfWidth;
    const float rightEdge = m_mainXCenter + halfWidth;

    for (size_t i = 0; i < m_mainOptions.size(); ++i) {
        const MainMenuOption &opt = m_mainOptions[i];
        FEObject *obj = opt.obj;
        if (!obj)
            continue;
        float xpos = m_mainXCenter + m_mainScrollPos + opt.xPos;
        // 0x0029d088: clip options outside [fXCenter - fWidth/2, fXCenter + fWidth/2].
        bool inBounds = (xpos >= leftEdge && xpos <= rightEdge);
        FENativeInterface::SetObjectVisible(obj, inBounds);

        float scale = (i == m_mainSelected) ? 1.0f : kUnselectedScale;
        FENativeInterface::SetSize(obj, opt.origW * scale, opt.origH * scale);
        FENativeInterface::SetCenter(obj, xpos, m_mainYCenter);

        // 0x0029bab0 & 0x0029d280: edge fade when bAllowColorAnim is active.
        if (m_mainAllowFade && obj->GetObjData()) {
            float fadeBand = opt.origW * kUnselectedScale;
            float alphaFactor = 1.0f;
            if (xpos < leftEdge || xpos > rightEdge)
                alphaFactor = 0.0f;
            else if (fadeBand > 0.0f && xpos < leftEdge + fadeBand)
                alphaFactor = (xpos - leftEdge) / fadeBand;
            else if (fadeBand > 0.0f && xpos > rightEdge - fadeBand)
                alphaFactor = (rightEdge - xpos) / fadeBand;
            obj->GetObjData()->Col.a = static_cast<int>(opt.origAlpha * alphaFactor);
        }
    }
}

void FEStartupSequence::HandleMainMessage(uint32_t msg, FEPackage *pkg)
{
    if (m_mainOptions.empty())
        return;

    if (msg == kMsgInitComplete) {
        // 0x0029d710 line 30: RefreshHeader() (0x0029de00) when FEHASH_INITCOMPLETE (0x35f8620b) arrives.
        FENativeInterface::SetLanguageHash(pkg, 0x5e7b09c9, m_mainOptions[m_mainSelected].nameHash);
        PositionMainOptions(pkg);
        return;
    }

    // 0x001e6c88: Event Handler (0x47ff4e7c) sends 0x34dc1bcf on intro completion, which queues 0x251bf83e.
    if (msg == 0x34dc1bcf || msg == 0x251bf83e) {
        m_mainAllowFade = true; // 0x0029dcb0 -> 0x0029d038
        FENativeInterface::SetScript(pkg, 0x47ff4e7c, 0x03d8eabc, true);
        PositionMainOptions(pkg);
        return;
    }

    if (msg == kMsgPadLeft) {
        // 0x0029d710 -> 0x002d41e8 -> 0x0029c718 (ScrollWrapped prev):
        m_mainSelected = (m_mainSelected + m_mainOptions.size() - 1) % m_mainOptions.size();
        m_mainScrollTarget = -m_mainOptions[m_mainSelected].xPos;
        UpdateMainSelection(pkg);
        PositionMainOptions(pkg);
        return;
    }

    if (msg == kMsgPadRight) {
        // 0x0029d710 -> 0x002d4208 -> 0x0029c718 (ScrollWrapped next):
        m_mainSelected = (m_mainSelected + 1) % m_mainOptions.size();
        m_mainScrollTarget = -m_mainOptions[m_mainSelected].xPos;
        UpdateMainSelection(pkg);
        PositionMainOptions(pkg);
        return;
    }

    if (msg == kMsgScreenTick) {
        // 0x0029cae8: smoothly interpolate m_mainScrollPos towards m_mainScrollTarget (~0.3s duration).
        float dt = (m_nowMs > m_lastTickMs) ? float(m_nowMs - m_lastTickMs) * 0.001f : 0.016f;
        float diff = m_mainScrollTarget - m_mainScrollPos;
        if (diff > 0.5f || diff < -0.5f) {
            float step = std::min(1.0f, dt * 12.0f);
            m_mainScrollPos += diff * step;
        } else {
            m_mainScrollPos = m_mainScrollTarget;
        }
        PositionMainOptions(pkg);
    }
}
