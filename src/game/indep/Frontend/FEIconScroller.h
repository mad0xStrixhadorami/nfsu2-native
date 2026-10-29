#pragma once
#include <cstdint>
#include <string>
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
        std::string text;      // shown in ICON_TITLE instead of the language label when non-empty
    };

    // Bind to a loaded package and add options (IconScrollerMenu::AddOption for each Def).
    // masterName/optionPrefix: object names of the template and the options; IconScroller
    // menus use OPTION_MASTER / OPTION_n, the track list uses ICON_MASTER / TRACK_ICON_n.
    void Setup(FEPackage *pkg, const std::vector<Def> &defs, size_t defaultSelection,
               const char *masterName = "OPTION_MASTER", const char *optionPrefix = "OPTION_",
               float spacing = 40.0f);
    // Bind options to existing objects (UI_Paint.fng's 4b4e3888..4b4e3890 cards, which are created
    // by 0x002bcff8 rather than named OPTION_n).  The cards ship invisible with alpha 0, so this
    // gives each one its icon/name and makes it render when Position() brings it on screen.
    void SetupObjects(FEPackage *pkg, const std::vector<Def> &defs,
                      const std::vector<FEObject *> &slots, size_t defaultSelection,
                      const char *masterName = "OPTION_MASTER");
    // The row anchor the scroller centres the selected option on.  IconScroller::IconScroller
    // (0x0029c9c8) reads it from the screen's option master; the Profile Manager has no master
    // (its cards are built at runtime), so its caller supplies the row centre instead (#195).
    void SetLayoutCenter(float x, float y);
    // Left/Right scroll, INITCOMPLETE header refresh, screen-tick animation. dtSeconds for ticks.
    void HandleMessage(uint32_t msg, float dtSeconds);
    // Start edge fades once the menu's intro animation has finished (0x0029dcb0 -> 0x0029d038).
    void AllowFade();

    // Replaces an option's title text (and refreshes ICON_TITLE if it is the selected one).
    void SetOptionText(size_t index, const std::string &text);
    // Replaces an option's icon texture.
    void SetOptionIcon(size_t index, uint32_t texHash);
    bool Empty() const { return m_options.empty(); }
    size_t Selected() const { return m_selected; }
    size_t Count() const { return m_options.size(); }
    // An option's label for a native list (ISSUES #361): its own text, else the language label of its name hash (via `lang`).
    template <typename F> std::string Label(size_t i, F &&lang) const
    {
        if (i >= m_options.size())
            return {};
        return !m_options[i].text.empty() ? m_options[i].text : lang(m_options[i].nameHash);
    }
    bool Disabled(size_t i) const { return i < m_options.size() && m_options[i].disabled; }
    FEPackage *Package() const { return m_pkg; }

private:
    struct Option {
        uint32_t texHash = 0, nameHash = 0;
        std::string text;
        FEObject *obj = nullptr;
        float xPos = 0, origW = 0, origH = 0;
        int origAlpha = 255;
        bool disabled = false;
    };
    void Bind(FEPackage *pkg, const std::vector<Def> &defs, const std::vector<FEObject *> &slots,
              size_t defaultSelection, const char *masterName, bool focusMaster,
              bool forceVisibleAlpha, float spacing = 40.0f);
    void UpdateSelection();
    void RefreshTitle();
    void Position();

    FEPackage *m_pkg = nullptr;
    std::vector<Option> m_options;
    size_t m_selected = 0;
    float m_xCenter = 0, m_yCenter = 0;
    float m_scrollPos = 0, m_scrollTarget = 0;
    bool m_allowFade = false;
    FEObject *m_focusObject = nullptr; // current button when it is not the selected option
};
