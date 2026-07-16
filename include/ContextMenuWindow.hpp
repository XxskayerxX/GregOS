#ifndef CONTEXT_MENU_WINDOW_HPP
#define CONTEXT_MENU_WINDOW_HPP

#include "Window.hpp"

/* ── ContextMenuWindow: right-click desktop popup ───────────────────────────
   Overrides draw() completely — no title bar chrome.
   Opens at the right-click position (screen-edge clamped).
   Dismissed by left-click outside or by WindowManager on focus-loss.       */

class ContextMenuWindow : public Window {
public:
    static constexpr int ITEM_H  = 22;
    static constexpr int MENU_W  = 170;
    static constexpr int N_ITEMS = 12;
    static constexpr int MENU_H  = N_ITEMS * ITEM_H + 4;

    ContextMenuWindow() = default;

    void draw()                  override;
    bool on_event(const Event& e) override;

private:
    int m_hovered { -1 };

    int  item_at(int mx, int my) const;
    void launch_item(int idx);
};

#endif /* CONTEXT_MENU_WINDOW_HPP */
