#ifndef SYSTEMWINDOW_HPP
#define SYSTEMWINDOW_HPP

#include "Window.hpp"

/* ── SystemWindow: system info panel (replaces gui_open_sysinfo) ─────────
   Renders GregOS branding + live stats (uptime, time, FS, casino) in the
   client area. Redraws every frame so uptime/time stay current.          */

class SystemWindow : public Window {
public:
    SystemWindow() = default;
    void draw() override;
};

#endif /* SYSTEMWINDOW_HPP */
