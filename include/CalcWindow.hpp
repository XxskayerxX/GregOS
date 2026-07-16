#ifndef CALC_WINDOW_HPP
#define CALC_WINDOW_HPP

#include "Window.hpp"
/* No GUI::Button — CalcWindow draws its own color-coded buttons. */

class CalcWindow : public Window {
public:
    void draw()                   override;
    bool on_event(const Event& e) override;
    bool handle_char(int c)       override;
    void on_key(char k);

    static constexpr int COLS    =  5;
    static constexpr int ROWS    =  6;
    static constexpr int BTN_W   = 52;
    static constexpr int BTN_H   = 42;
    static constexpr int BTN_GAP =  4;
    static constexpr int BTN_X0  =  4;
    static constexpr int BTN_Y0  = 76;
    static constexpr int DISP_H  = 72;

private:
    static constexpr int MAX_EXPR = 64;

    char      m_expr[MAX_EXPR + 1]   {};
    char      m_history[MAX_EXPR + 4] {};
    char      m_result[32]           {};
    bool      m_show_result          {};
    long long m_memory               {};

    struct BtnDef { const char* label; char key; unsigned int color; };
    static const BtnDef s_buttons[ROWS * COLS];

    void         evaluate();
    void         append_expr(char c);
    long long    parse_result() const;
    void         instant_fn(char k);
    int          btn_at(int px, int py) const;
    void         draw_button(int idx) const;

    static int            cw_slen(const char* s);
    static int            cw_itos(long long n, char* buf);
    static long long      cw_isqrt(long long n);
    static unsigned long long cw_udiv64(unsigned long long a, unsigned long long b,
                                        unsigned long long* rem);
    static long long      cw_div64(long long a, long long b);
    static long long      cw_mod64(long long a, long long b);
};

extern "C" void open_calc_window(void);

#endif /* CALC_WINDOW_HPP */
