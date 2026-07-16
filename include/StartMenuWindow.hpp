#ifndef STARTMENUWINDOW_HPP
#define STARTMENUWINDOW_HPP

#include "Window.hpp"

/* ── StartMenuWindow: popup app launcher above the taskbar ──────────────
   Overrides draw() entirely — no title bar chrome.
   Pops up at x=0, y = screen_h - TASKBAR_H - MENU_H.
   Toggle: WindowManager::toggle_start_menu() (GregOS button click).
   Dismiss: clicking outside (WindowManager focus-loss logic).           */

class StartMenuWindow : public Window {
public:
    static constexpr int MENU_W   = 160;
    static constexpr int HEADER_H = 24;
    static constexpr int ITEM_H   = 22;
    static constexpr int N_ITEMS  = 13;
    /* HEADER_H + 2px separator + N items + 4px bottom padding */
    static constexpr int MENU_H   = HEADER_H + 2 + N_ITEMS * ITEM_H + 4;

    StartMenuWindow() = default;

    void draw()              override;
    bool on_event(const Event& e) override;

private:
    int m_hovered { -1 };

    int  item_at(int mx, int my) const;
    void launch_item(int idx);
};

#endif /* STARTMENUWINDOW_HPP */
