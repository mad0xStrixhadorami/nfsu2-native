#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "Speed/Indep/Src/FEng/FEObject.h"
#include "Speed/Indep/Src/FEng/FEngine.h"

// UG2's option list ("UIWidgetMenu", base ctor 0x0029e780): rows of OPTION_NAME_n / OPTION_DATA_n
// with LEFT_ARROW_n / RIGHT_ARROW_n, laid out from TITLE_MASTER / DATA_MASTER (row height 24 +
// gap 4, 0x0029e780 fields 0x48 / 0x4d). Up/Down moves the cursor, Left/Right changes the value
// through each widget's Act function (vtable slot 5), Draw (slot 9) refreshes the value text.
class FEWidgetMenu {
public:
    struct Widget {
        uint32_t titleLabel;
        // Returns the value text: either a language label hash (label != 0) or literal text.
        std::function<void(uint32_t &label, std::string &text)> draw;
        std::function<void(int dir)> act; // dir -1 = left, +1 = right
    };

    void Setup(FEPackage *pkg, std::vector<Widget> widgets);
    // Drop the current list when its screen is left.  The engine can reuse a package's address
    // for the next screen, so a stale m_pkg is indistinguishable from the new top package by
    // pointer alone (ISSUES #179).
    void Clear();
    // Returns true if the message was consumed (Up/Down/Left/Right).
    bool HandleMessage(uint32_t msg);
    // FEHASH_INITCOMPLETE: the screen's intro scripts are done; set every row's highlight state.
    void OnInitComplete();
    FEPackage *Package() const { return m_pkg; }

private:
    void Refresh();

    FEPackage *m_pkg = nullptr;
    std::vector<Widget> m_widgets;
    size_t m_selected = 0;
    size_t m_highlighted = SIZE_MAX; // row whose Highlight script is running
};
