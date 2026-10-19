#include "game/indep/Frontend/FEIconScroller.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "Speed/Indep/Src/FEng/FEObject.h"
#include "Speed/Indep/Src/FEng/FEPackage.h"
#include "Speed/Indep/bWare/Inc/Strings.hpp"
#include "game/indep/Frontend/FENative.h"
#include "game/indep/Frontend/FEStartup.h"

void FEIconScroller::Setup(FEPackage *pkg, const std::vector<Def> &defs, size_t defaultSelection,
                           const char *masterName, const char *optionPrefix)
{
    m_pkg = pkg;
    m_options.clear();
    // MenuScreen ctor (0x00294558): hide OL_ICON_GROUP and EA_MESSENGER_GROUP.
    FENativeInterface::SetVisible(pkg, bStringHash("OL_ICON_GROUP"), false);
    FENativeInterface::SetVisible(pkg, bStringHash("EA_MESSENGER_GROUP"), false);

    // IconScroller ctor (0x0029c9c8): read the centre of OPTION_MASTER and hide the masters.
    m_xCenter = m_yCenter = 0.0f;
    m_focusObject = nullptr;
    if (FEObject *optMaster = pkg->FindObjectByHash(bStringHash(masterName))) {
        // 0x0029af60 scrollers keep the (hidden) master as the current button (0x002926f0 ->
        // 0x00291ad8 -> SetCurrentButton), so its BUTTON_PRESSED response receives Accept.
        size_t len = strlen(optionPrefix);
        if (len && optionPrefix[len - 1] != '_')
            m_focusObject = optMaster;
        FENativeInterface::GetCenter(optMaster, m_xCenter, m_yCenter);
        FENativeInterface::SetObjectVisible(optMaster, false);
    }
    if (FEObject *lockMaster = pkg->FindObjectByHash(bStringHash("LOCK_MASTER")))
        FENativeInterface::SetObjectVisible(lockMaster, false);
    if (FEObject *btnMaster = pkg->FindObjectByHash(bStringHash("BUTTON_MASTER")))
        FENativeInterface::SetObjectVisible(btnMaster, false);

    // Option objects: "OPTION_1".."OPTION_n" (prefix ends in '_'), or, for scrollers built by
    // 0x0029af60, every top-level object named exactly optionPrefix, in package order (0x0029b440).
    std::vector<FEObject *> slots;
    size_t prefixLen = strlen(optionPrefix);
    if (prefixLen && optionPrefix[prefixLen - 1] == '_') {
        for (size_t i = 0; i < 64; ++i) {
            char optName[64];
            snprintf(optName, sizeof(optName), "%s%zu", optionPrefix, i + 1);
            slots.push_back(pkg->FindObjectByHash(bStringHash(optName)));
        }
    } else {
        const uint32_t hash = bStringHash(optionPrefix);
        for (FEObject *o = pkg->GetFirstObject(); o; o = o->GetNext())
            if (o->NameHash == hash)
                slots.push_back(o);
    }

    float addPos = 0.0f;
    constexpr float kIconSpacing = 40.0f; // 0x42200000 at 0x0029c9c8
    for (size_t i = 0; i < defs.size() && i < slots.size(); ++i) {
        FEObject *obj = slots[i];
        if (!obj)
            continue;
        // Keep FEngine's spatial button map from overriding the scroller's Left/Right selection.
        obj->Flags |= 0x00800000u;
        FENativeInterface::SetTextureHash(obj, defs[i].texHash);
        Option opt;
        opt.texHash = defs[i].texHash;
        opt.nameHash = defs[i].nameHash;
        opt.disabled = defs[i].disabled;
        opt.obj = obj;
        opt.xPos = addPos;
        FENativeInterface::GetSize(obj, opt.origW, opt.origH);
        opt.origAlpha = obj->GetObjData() ? int(obj->GetObjData()->Col.a) : 255;
        addPos += opt.origW + kIconSpacing;
        m_options.push_back(opt);
    }
    // Hide option objects beyond the options added.
    for (size_t i = defs.size(); i < slots.size(); ++i)
        if (slots[i])
            FENativeInterface::SetObjectVisible(slots[i], false);
    if (m_options.empty())
        return;
    m_selected = defaultSelection < m_options.size() ? defaultSelection : 0;
    m_allowFade = false;
    m_scrollPos = m_scrollTarget = -m_options[m_selected].xPos;
    UpdateSelection();
    Position();
}

void FEIconScroller::AllowFade()
{
    m_allowFade = true;
    Position();
}

void FEIconScroller::HandleMessage(uint32_t msg, float dt)
{
    if (m_options.empty())
        return;
    if (msg == FEStartupSequence::kMsgInitComplete) {
        // 0x0029d710: RefreshHeader() (0x0029de00) on FEHASH_INITCOMPLETE.
        FENativeInterface::SetLanguageHash(m_pkg, 0x5e7b09c9, m_options[m_selected].nameHash);
        Position();
    } else if (msg == FEStartupSequence::kMsgPadLeft || msg == FEStartupSequence::kMsgPadRight) {
        // 0x0029d710 -> 0x002d41e8 / 0x002d4208 -> 0x0029c718 (ScrollWrapped prev / next)
        size_t n = m_options.size();
        m_selected = msg == FEStartupSequence::kMsgPadLeft ? (m_selected + n - 1) % n : (m_selected + 1) % n;
        m_scrollTarget = -m_options[m_selected].xPos;
        UpdateSelection();
        Position();
    } else if (msg == FEStartupSequence::kMsgScreenTick) {
        // 0x0029cae8: ease the scroll position towards the target.
        float diff = m_scrollTarget - m_scrollPos;
        if (diff > 0.5f || diff < -0.5f)
            m_scrollPos += diff * std::min(1.0f, dt * 12.0f);
        else
            m_scrollPos = m_scrollTarget;
        Position();
    }
}

void FEIconScroller::UpdateSelection()
{
    // 0x0029ce70 (IconScroller::SetSelection)
    for (size_t i = 0; i < m_options.size(); ++i)
        if (FEObject *o = m_options[i].obj) {
            if (i == m_selected)
                o->Flags &= ~0x04000000u;
            else
                o->Flags |= 0x04000000u;
        }
    if (FEObject *focus = m_focusObject ? m_focusObject : m_options[m_selected].obj)
        m_pkg->SetCurrentButton(focus, true);

    // 0x0029de00 (RefreshHeader): ICON_TITLE (0x5e7b09c9)
    FENativeInterface::SetLanguageHash(m_pkg, 0x5e7b09c9, m_options[m_selected].nameHash);

    // 0x002991c0 / 0x00299480 (ScrollBar::Refresh) and 0x0029d360 (UpdateArrows)
    FEObject *backing = m_pkg->FindObjectByHash(bStringHash("SCROLLBAR_BACKING"));
    FEObject *handle = m_pkg->FindObjectByHash(bStringHash("SCROLLBAR_HANDLE"));
    if (backing && handle && m_options.size() > 1) {
        float bx, by, bw, bh, hx, hy, hw, hh;
        FENativeInterface::GetCenter(backing, bx, by);
        FENativeInterface::GetSize(backing, bw, bh);
        FENativeInterface::GetCenter(handle, hx, hy);
        FENativeInterface::GetSize(handle, hw, hh);
        float t = float(m_selected) / float(m_options.size() - 1);
        FENativeInterface::SetCenter(handle, bx - bw * 0.5f + hw * 0.5f + t * (bw - hw), hy);
    }
    FENativeInterface::SetVisible(m_pkg, bStringHash("SCROLLBAR_ARROW_1"), m_selected > 0);
    FENativeInterface::SetVisible(m_pkg, bStringHash("SCROLLBAR_ARROW_2"), m_selected + 1 < m_options.size());
}

void FEIconScroller::Position()
{
    // IconScroller::PositionOption (0x0029d0d0), ClipEdges (0x0029d088), fades (0x0029bab0, 0x0029d280)
    constexpr float kScrollerWidth = 480.0f;  // 0x43f00000 in 0x0029d638
    constexpr float kUnselectedScale = 0.80f; // DAT_00563034
    const float left = m_xCenter - kScrollerWidth * 0.5f;
    const float right = m_xCenter + kScrollerWidth * 0.5f;
    for (size_t i = 0; i < m_options.size(); ++i) {
        const Option &opt = m_options[i];
        FEObject *obj = opt.obj;
        if (!obj)
            continue;
        float x = m_xCenter + m_scrollPos + opt.xPos;
        FENativeInterface::SetObjectVisible(obj, x >= left && x <= right);
        float scale = i == m_selected ? 1.0f : kUnselectedScale;
        FENativeInterface::SetSize(obj, opt.origW * scale, opt.origH * scale);
        FENativeInterface::SetCenter(obj, x, m_yCenter);
        if (m_allowFade && obj->GetObjData()) {
            float band = opt.origW * kUnselectedScale, f = 1.0f;
            if (x < left || x > right)
                f = 0.0f;
            else if (band > 0 && x < left + band)
                f = (x - left) / band;
            else if (band > 0 && x > right - band)
                f = (right - x) / band;
            obj->GetObjData()->Col.a = static_cast<int>(opt.origAlpha * f * (opt.disabled ? 0.4f : 1.0f));
        }
    }
}
