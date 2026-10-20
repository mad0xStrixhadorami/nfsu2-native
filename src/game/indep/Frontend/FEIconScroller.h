#pragma once
#include <cstdint>
#include <vector>

#include "Speed/Indep/Src/FEng/FEngine.h"

// UG2's icon carousel used by many menus (main menu, quick race mode select, ...), from
//   IconScroller::IconScroller     (0x0029c9c8)
//   IconScroller::SetSelection     (0x0029ce70)
//   IconScroller::PositionOption   (0x0029d0d0), ClipEdges (0x0029d088), fades (0x0029bab0, 0x0029d280)
//   IconScroller::Update           (0x0029cae8)
//   IconScrollerMenu::RefreshHeader(0x0029de00), ScrollBar::Refresh (0x002991c0 / 0x00299480)
// The package provides OPTION_1..N, OPTION_MASTER, ICON_TITLE, SCROLLBAR_* objects.
class FEIconScroller {
public:
    struct Def {
        uint32_t texHash;  // icon texture
        uint32_t nameHash; // language label shown in ICON_TITLE
        bool disabled = false; // greyed (option +0x24, e.g. View Signed Sponsors with none signed)
    };

    // Bind to a loaded package and add options (IconScrollerMenu::AddOption for each Def).
    // masterName/optionPrefix: object names of the template and the options; IconScroller
    // menus use OPTION_MASTER / OPTION_n, the track list uses ICON_MASTER / TRACK_ICON_n.
    void Setup(FEPackage *pkg, const std::vector<Def> &defs, size_t defaultSelection,
               const char *masterName = "OPTION_MASTER", const char *optionPrefix = "OPTION_");
    // Bind options to existing objects (UI_Paint.fng's 4b4e3888..4b4e3890 cards, which are created
    // by 0x002bcff8 rather than named OPTION_n).  The cards ship invisible with alpha 0, so this
    // gives each one its icon/name and makes it render when Position() brings it on screen.
    void SetupObjects(FEPackage *pkg, const std::vector<Def> &defs,
                      const std::vector<FEObject *> &slots, size_t defaultSelection,
                      const char *masterName = "OPTION_MASTER");
    // Left/Right scroll, INITCOMPLETE header refresh, screen-tick animation. dtSeconds for ticks.
    void HandleMessage(uint32_t msg, float dtSeconds);
    // Start edge fades once the menu's intro animation has finished (0x0029dcb0 -> 0x0029d038).
    void AllowFade();

    bool Empty() const { return m_options.empty(); }
    size_t Selected() const { return m_selected; }
    FEPackage *Package() const { return m_pkg; }

private:
    struct Option {
        uint32_t texHash = 0, nameHash = 0;
        FEObject *obj = nullptr;
        float xPos = 0, origW = 0, origH = 0;
        int origAlpha = 255;
        bool disabled = false;
    };
    void Bind(FEPackage *pkg, const std::vector<Def> &defs, const std::vector<FEObject *> &slots,
              size_t defaultSelection, const char *masterName, bool focusMaster,
              bool forceVisibleAlpha);
    void UpdateSelection();
    void Position();

    FEPackage *m_pkg = nullptr;
    std::vector<Option> m_options;
    size_t m_selected = 0;
    float m_xCenter = 0, m_yCenter = 0;
    float m_scrollPos = 0, m_scrollTarget = 0;
    bool m_allowFade = false;
    FEObject *m_focusObject = nullptr; // current button when it is not the selected option
};
