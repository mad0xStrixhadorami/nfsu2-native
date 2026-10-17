#include "game/indep/Frontend/FEWidgetMenu.h"

#include <cstdio>
#include <cstdlib>

#include "Speed/Indep/Src/FEng/FEScript.h"

#include "Speed/Indep/Src/FEng/FEObject.h"
#include "Speed/Indep/Src/FEng/FEPackage.h"
#include "Speed/Indep/bWare/Inc/Strings.hpp"
#include "game/indep/Frontend/FENative.h"
#include "game/indep/Frontend/FEStartup.h"

namespace {
uint32_t RowHash(const char *prefix, size_t row)
{
    char name[48];
    snprintf(name, sizeof(name), "%s%zu", prefix, row + 1);
    return bStringHash(name);
}

constexpr float kRowHeight = 24.0f; // 0x41c00000
constexpr float kRowGap = 4.0f;     // 0x40800000
constexpr size_t kMaxRows = 11;     // OPTION_NAME_1..11 in the packages
} // namespace

void FEWidgetMenu::Setup(FEPackage *pkg, std::vector<Widget> widgets)
{
    m_pkg = pkg;
    m_widgets = std::move(widgets);
    m_selected = 0;
    m_highlighted = SIZE_MAX;

    float nameX = 0, nameY = 0, dataX = 0, dataY = 0;
    FEObject *titleMaster = pkg->FindObjectByHash(bStringHash("TITLE_MASTER"));
    FEObject *dataMaster = pkg->FindObjectByHash(bStringHash("DATA_MASTER"));
    if (titleMaster)
        FENativeInterface::GetCenter(titleMaster, nameX, nameY);
    if (dataMaster)
        FENativeInterface::GetCenter(dataMaster, dataX, dataY);
    FENativeInterface::SetObjectVisible(titleMaster, false);
    FENativeInterface::SetObjectVisible(dataMaster, false);

    static const char *const kRowObjects[] = {"OPTION_NAME_", "OPTION_DATA_", "OPTION_BACKING_", "LEFT_ARROW_",
                                              "RIGHT_ARROW_"};
    for (size_t row = 0; row < kMaxRows; ++row) {
        bool used = row < m_widgets.size();
        for (const char *prefix : kRowObjects)
            FENativeInterface::SetVisible(pkg, RowHash(prefix, row), used);
        if (!used)
            continue;
        float y = row * (kRowHeight + kRowGap);
        if (titleMaster)
            if (FEObject *o = pkg->FindObjectByHash(RowHash("OPTION_NAME_", row)))
                FENativeInterface::SetCenter(o, nameX, nameY + y);
        if (dataMaster)
            if (FEObject *o = pkg->FindObjectByHash(RowHash("OPTION_DATA_", row)))
                FENativeInterface::SetCenter(o, dataX, dataY + y);
        FENativeInterface::SetLanguageHash(pkg, RowHash("OPTION_NAME_", row), m_widgets[row].titleLabel);
    }
    if (getenv("NFSU2_FEDEBUG"))
        for (const char *prefix : {"OPTION_NAME_", "OPTION_DATA_", "OPTION_BACKING_", "LEFT_ARROW_"})
            if (FEObject *o = pkg->FindObjectByHash(RowHash(prefix, 0))) {
                fprintf(stderr, "[widget] %s1 type %d scripts:", prefix, int(o->Type));
                for (FEScript *sc = o->GetFirstScript(); sc; sc = static_cast<FEScript *>(sc->GetNext()))
                    fprintf(stderr, " %08x", sc->ID);
                fprintf(stderr, "\n");
            }
    Refresh();
}

void FEWidgetMenu::OnInitComplete()
{
    m_highlighted = SIZE_MAX;
    Refresh();
}

bool FEWidgetMenu::HandleMessage(uint32_t msg)
{
    if (m_widgets.empty())
        return false;
    const size_t n = m_widgets.size();
    switch (msg) {
    case FEStartupSequence::kMsgPadUp:
        m_selected = (m_selected + n - 1) % n;
        break;
    case FEStartupSequence::kMsgPadDown:
        m_selected = (m_selected + 1) % n;
        break;
    case FEStartupSequence::kMsgPadLeft:
        m_widgets[m_selected].act(-1);
        break;
    case FEStartupSequence::kMsgPadRight:
        m_widgets[m_selected].act(+1);
        break;
    default:
        return false;
    }
    Refresh();
    return true;
}

void FEWidgetMenu::Refresh()
{
    for (size_t row = 0; row < m_widgets.size(); ++row) {
        uint32_t label = 0;
        std::string text;
        m_widgets[row].draw(label, text);
        if (label)
            FENativeInterface::SetLanguageHash(m_pkg, RowHash("OPTION_DATA_", row), label);
        else
            FENativeInterface::SetString(m_pkg, RowHash("OPTION_DATA_", row), text.c_str());
    }
    // Row highlight: the row objects carry "Highlight" (0x249db7b7) / "UnHighlight" (0x7ab5521a)
    // scripts; run them when the selection changes (restarting them every frame would freeze them).
    if (m_highlighted != m_selected) {
        static const char *const kRowObjects[] = {"OPTION_NAME_", "OPTION_DATA_", "OPTION_BACKING_", "LEFT_ARROW_",
                                                  "RIGHT_ARROW_"};
        for (const char *prefix : kRowObjects) {
            if (m_highlighted == SIZE_MAX) // first refresh: every row starts un-highlighted
                for (size_t row = 0; row < m_widgets.size(); ++row)
                    FENativeInterface::SetScript(m_pkg, RowHash(prefix, row), 0x7ab5521a, true);
            else if (m_highlighted < m_widgets.size())
                FENativeInterface::SetScript(m_pkg, RowHash(prefix, m_highlighted), 0x7ab5521a, true);
            FENativeInterface::SetScript(m_pkg, RowHash(prefix, m_selected), 0x249db7b7, true);
        }
        m_highlighted = m_selected;
    }
}
