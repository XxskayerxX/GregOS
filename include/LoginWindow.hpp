#ifndef LOGINWINDOW_HPP
#define LOGINWINDOW_HPP

#include "Window.hpp"

/* ── LoginWindow ────────────────────────────────────────────────────────────
   Full-screen modal login.  Two phases:
     PHASE_LOADING — animated progress bar (≈2.2 s), absorbs all input
     PHASE_LOGIN   — password field revealed, keyboard captured

   On correct password ("admin") sets g_login_done=1 and requests close.
   The main loop in kmain detects g_login_done and calls gui_desktop_init(). */

class LoginWindow : public Window {
public:
    /* Timing (jiffies = 100 Hz) */
    static constexpr unsigned long LOAD_J  = 450;  /* 4.5 s — DRAKKAR rises */
    static constexpr unsigned long PAUSE_J = 100;  /* 1.0 s pause at 100%  */
    static constexpr unsigned long ERR_J   =  80;  /* 0.8 s error display  */

    LoginWindow();

    void draw()             override;
    bool on_event(const Event& e) override { (void)e; return true; /* modal */ }
    bool handle_char(int c) override;

private:
    enum Phase { PHASE_LOADING, PHASE_LOGIN };

    void draw_loading_box(int pct);
    void draw_login_box();
    void draw_footer();

    Phase         m_phase;
    unsigned long m_boot_j;   /* jiffies at first draw — animation base */
    char          m_pwd[32];
    int           m_pwd_len;
    unsigned long m_err_j;    /* jiffies at last wrong attempt (0 = none) */
};

#endif /* LOGINWINDOW_HPP */
