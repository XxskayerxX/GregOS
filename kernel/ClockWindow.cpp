/* ClockWindow — analog clock, digital readout, month calendar, stopwatch.
   Freestanding: no libc, no libm. Integer trig via 60-entry lookup tables.  */

#include "../include/ClockWindow.hpp"
#include "../include/WindowManager.hpp"
#include "../include/Graphics.hpp"
#include "../include/Theme.hpp"
#include "../include/gfx.h"
#include "../include/keyboard.h"

extern "C" volatile unsigned long jiffies;
extern "C" unsigned char get_rtc_register(int reg);
extern "C" int bcd_to_bin(unsigned char bcd);

/* ── trig tables: value = fn(i*6°) * 1024, i = 0..59 ─────────────────────── */
static const short SIN60[60] = {
        0,   107,   213,   316,   416,   512,   602,   685,   761,   828,
      887,   935,   974,  1002,  1018,  1024,  1018,  1002,   974,   935,
      887,   828,   761,   685,   602,   512,   416,   316,   213,   107,
        0,  -107,  -213,  -316,  -416,  -512,  -602,  -685,  -761,  -828,
     -887,  -935,  -974, -1002, -1018, -1024, -1018, -1002,  -974,  -935,
     -887,  -828,  -761,  -685,  -602,  -512,  -416,  -316,  -213,  -107,
};
static const short COS60[60] = {
     1024,  1018,  1002,   974,   935,   887,   828,   761,   685,   602,
      512,   416,   316,   213,   107,     0,  -107,  -213,  -316,  -416,
     -512,  -602,  -685,  -761,  -828,  -887,  -935,  -974, -1002, -1018,
    -1024, -1018, -1002,  -974,  -935,  -887,  -828,  -761,  -685,  -602,
     -512,  -416,  -316,  -213,  -107,     0,   107,   213,   316,   416,
      512,   602,   685,   761,   828,   887,   935,   974,  1002,  1018,
};

static const char* const MONTHS[13] = { "",
    "Janvier","Fevrier","Mars","Avril","Mai","Juin",
    "Juillet","Aout","Septembre","Octobre","Novembre","Decembre" };
static const char* const WEEKDAYS[7] = {
    "Lundi","Mardi","Mercredi","Jeudi","Vendredi","Samedi","Dimanche" };

/* ── helpers ─────────────────────────────────────────────────────────────── */
static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }

static int days_in_month(int y, int m) {
    static const int d[13] = { 0,31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m == 2 && is_leap(y)) return 29;
    return (m >= 1 && m <= 12) ? d[m] : 30;
}

/* Zeller → Monday-first column index 0..6 (0=Lundi). */
static int weekday_monday0(int y, int m, int day) {
    if (m < 3) { m += 12; y -= 1; }
    int K = y % 100, J = y / 100;
    int h = (day + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7; /* 0=Sat */
    return (h + 5) % 7;  /* 0=Mon .. 6=Sun */
}

static void two(char* b, int v) { b[0] = (char)('0' + (v / 10) % 10); b[1] = (char)('0' + v % 10); }

/* Bold string: draw twice, 1px offset. */
static void bold_str(Graphics& g, int x, int y, const char* s, unsigned int fg, unsigned int bg) {
    g.draw_str(x, y, s, fg, bg);
    g.draw_str(x + 1, y, s, fg, GFX_TRANSPARENT);
}

/* Thick line (Bresenham) with square nib of half-width t. */
static void thick_line(Graphics& g, int x0, int y0, int x1, int y1, int t, unsigned int col) {
    int dx = x1 - x0, dy = y1 - y0;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        for (int oy = -t; oy <= t; ++oy)
            for (int ox = -t; ox <= t; ++ox)
                g.put_pixel(x0 + ox, y0 + oy, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

static void ring(Graphics& g, int cx, int cy, int r, unsigned int col) {
    int x = r, y = 0, err = 0;
    while (x >= y) {
        int pts[8][2] = { {cx+x,cy+y},{cx+y,cy+x},{cx-y,cy+x},{cx-x,cy+y},
                          {cx-x,cy-y},{cx-y,cy-x},{cx+y,cy-x},{cx+x,cy-y} };
        for (int i = 0; i < 8; ++i) g.put_pixel(pts[i][0], pts[i][1], col);
        y++; if (err <= 0) err += 2*y + 1; if (err > 0) { x--; err -= 2*x + 1; }
    }
}

/* ── calendar sync ───────────────────────────────────────────────────────── */
void ClockWindow::sync_calendar() {
    if (m_cal_year == 0) {
        int mon  = bcd_to_bin(get_rtc_register(0x08));
        int year = 2000 + bcd_to_bin(get_rtc_register(0x09));
        m_cal_month = (mon >= 1 && mon <= 12) ? mon : 1;
        m_cal_year  = year;
    }
}

/* ── analog clock ────────────────────────────────────────────────────────── */
void ClockWindow::draw_analog(int cx, int cy, int r, int hh, int mm, int ss) {
    Graphics& g = Graphics::instance();
    g.fill_rect(cx - r, cy - r, 2*r, 2*r, Theme::WIN_BG_PURE); /* dark face square bg */
    /* Face disc — cadran de laiton sombre. */
    for (int dy = -r; dy <= r; ++dy) {
        int span = 0, rr = r*r - dy*dy;
        int xx = 0; while ((xx+1)*(xx+1) <= rr) xx++; span = xx;
        g.draw_hline(cx - span, cy + dy, 2*span + 1, Theme::GOLD_DEEP);
    }
    ring(g, cx, cy, r,     Theme::GOLD);
    ring(g, cx, cy, r - 1, Theme::GOLD);

    /* Hour ticks. */
    for (int i = 0; i < 60; ++i) {
        int outer = r - 2;
        int inner = (i % 5 == 0) ? r - 9 : r - 5;
        int x1 = cx + SIN60[i]*outer/1024, y1 = cy - COS60[i]*outer/1024;
        int x2 = cx + SIN60[i]*inner/1024, y2 = cy - COS60[i]*inner/1024;
        thick_line(g, x1, y1, x2, y2, (i % 5 == 0) ? 1 : 0,
                   (i % 5 == 0) ? Theme::GOLD_HI : Theme::GOLD_DIM);
    }

    int hpos = ((hh % 12) * 5 + mm / 12) % 60;
    int hlen = r * 1 / 2, mlen = r * 3 / 4, slen = r * 4 / 5;
    thick_line(g, cx, cy, cx + SIN60[hpos]*hlen/1024, cy - COS60[hpos]*hlen/1024, 2, Theme::AMBER);
    thick_line(g, cx, cy, cx + SIN60[mm]*mlen/1024,   cy - COS60[mm]*mlen/1024,   1, Theme::AMBER);
    thick_line(g, cx, cy, cx + SIN60[ss]*slen/1024,   cy - COS60[ss]*slen/1024,   0, Theme::EMBER);
    g.fill_rect(cx - 2, cy - 2, 5, 5, Theme::EMBER);
}

/* ── calendar ────────────────────────────────────────────────────────────── */
void ClockWindow::draw_calendar(int x, int y, int w) {
    Graphics& g = Graphics::instance();
    int today = bcd_to_bin(get_rtc_register(0x07));
    int rtc_m = bcd_to_bin(get_rtc_register(0x08));
    int rtc_y = 2000 + bcd_to_bin(get_rtc_register(0x09));

    char hdr[32]; int hp = 0;
    const char* mn = MONTHS[m_cal_month];
    while (mn[hp]) { hdr[hp] = mn[hp]; hp++; }
    hdr[hp++] = ' ';
    hdr[hp++] = (char)('0' + (m_cal_year / 1000) % 10);
    hdr[hp++] = (char)('0' + (m_cal_year / 100) % 10);
    hdr[hp++] = (char)('0' + (m_cal_year / 10) % 10);
    hdr[hp++] = (char)('0' + m_cal_year % 10);
    hdr[hp] = '\0';
    int hx = x + (w - hp * 8) / 2;
    bold_str(g, hx, y, hdr, Theme::GOLD_HI, GFX_TRANSPARENT);

    static const char* dow[7] = { "Lu","Ma","Me","Je","Ve","Sa","Di" };
    int cellw = w / 7;
    int gy = y + 20;
    for (int c = 0; c < 7; ++c)
        g.draw_str(x + c*cellw + (cellw-16)/2, gy, dow[c],
                   c >= 5 ? Theme::EMBER : Theme::AMBER, GFX_TRANSPARENT);
    gy += 18;

    int first = weekday_monday0(m_cal_year, m_cal_month, 1);
    int ndays = days_in_month(m_cal_year, m_cal_month);
    int col = first, row = 0;
    char db[3];
    for (int d = 1; d <= ndays; ++d) {
        int cx = x + col*cellw, cy = gy + row*18;
        bool is_today = (d == today && m_cal_month == rtc_m && m_cal_year == rtc_y);
        if (is_today) g.fill_rect(cx + 1, cy - 1, cellw - 2, 17, Theme::AMBER_DEEP);
        two(db, d); db[2] = '\0';
        if (d < 10) { db[0] = ' '; }
        g.draw_str(cx + (cellw-16)/2, cy, db,
                   is_today ? Theme::AMBER_HI : (col >= 5 ? Theme::EMBER_DIM : Theme::AMBER),
                   GFX_TRANSPARENT);
        if (++col > 6) { col = 0; ++row; }
    }
}

/* ── stopwatch ───────────────────────────────────────────────────────────── */
void ClockWindow::draw_stopwatch(int x, int y, int w) {
    Graphics& g = Graphics::instance();
    (void)w;
    unsigned long el = m_sw_acc + (m_sw_run ? (jiffies - m_sw_start) : 0);
    unsigned long cs = el % 100;              /* centiseconds (100 Hz)        */
    unsigned long tot = el / 100;
    unsigned long mm = tot / 60, sec = tot % 60;
    char t[16];
    two(t, (int)(mm % 100)); t[2] = ':';
    two(t + 3, (int)sec);    t[5] = '.';
    two(t + 6, (int)cs);     t[8] = '\0';
    g.draw_str(x, y + 4, "Chrono", Theme::AMBER, GFX_TRANSPARENT);
    bold_str(g, x + 64, y + 4, t, m_sw_run ? Theme::GREEN : Theme::AMBER, GFX_TRANSPARENT);
}

/* ── button geometry ─────────────────────────────────────────────────────── */
int ClockWindow::bar_y() const     { return client_y() + 250; }
int ClockWindow::sw_y() const      { return client_y() + client_h() - 30; }
int ClockWindow::btn_prev_x() const  { return client_x() + 8; }
int ClockWindow::btn_next_x() const  { return client_x() + 8 + 34; }
int ClockWindow::btn_today_x() const { return client_x() + client_w() - 96; }
int ClockWindow::btn_sw_x() const    { return client_x() + client_w() - 150; }
int ClockWindow::btn_swrz_x() const  { return client_x() + client_w() - 70; }

static void draw_btn(Graphics& g, int x, int y, int w, int h, const char* s, bool down) {
    g.fill_rect(x, y, w, h, Theme::WIN_BG);
    unsigned int lt = down ? Theme::BEVEL_OUTER_DK : Theme::BEVEL_OUTER_LT;
    unsigned int dk = down ? Theme::BEVEL_OUTER_LT : Theme::BEVEL_OUTER_DK;
    g.draw_hline(x, y, w, lt); g.draw_vline(x, y, h, lt);
    g.draw_hline(x, y + h - 1, w, dk); g.draw_vline(x + w - 1, y, h, dk);
    int sl = 0; while (s[sl]) sl++;
    g.draw_str(x + (w - sl*8)/2, y + (h-16)/2, s, Theme::AMBER, GFX_TRANSPARENT);
}

/* ── draw ────────────────────────────────────────────────────────────────── */
void ClockWindow::draw() {
    Window::draw();
    sync_calendar();
    Graphics& g = Graphics::instance();

    int hh = bcd_to_bin(get_rtc_register(0x04));
    int mm = bcd_to_bin(get_rtc_register(0x02));
    int ss = bcd_to_bin(get_rtc_register(0x00));
    int day = bcd_to_bin(get_rtc_register(0x07));
    int mon = bcd_to_bin(get_rtc_register(0x08));
    int yr  = 2000 + bcd_to_bin(get_rtc_register(0x09));

    int cx0 = client_x(), cy0 = client_y(), cw = client_w();

    /* Analog clock centred in the upper area. */
    int r = 90;
    draw_analog(cx0 + cw/2, cy0 + 12 + r, r, hh, mm, ss);

    /* Digital HH:MM:SS. */
    char tb[9]; two(tb, hh); tb[2]=':'; two(tb+3, mm); tb[5]=':'; two(tb+6, ss); tb[8]='\0';
    int ty = cy0 + 12 + 2*r + 8;
    /* draw 2x scale-ish by bolding; centre it */
    int tw = 8 * 8;
    bold_str(g, cx0 + (cw - tw)/2, ty, tb, Theme::GREEN_HI, GFX_TRANSPARENT);

    /* Date line: "Weekday DD Month YYYY". */
    int wd = weekday_monday0(yr, mon, day);
    char date[48]; int dp = 0;
    const char* wn = WEEKDAYS[wd];
    while (wn[dp] && dp < 12) { date[dp] = wn[dp]; dp++; }
    date[dp++] = ' ';
    if (day >= 10) date[dp++] = (char)('0' + day/10);
    date[dp++] = (char)('0' + day%10);
    date[dp++] = ' ';
    const char* mn2 = MONTHS[(mon>=1&&mon<=12)?mon:1];
    for (int i = 0; mn2[i]; ++i) date[dp++] = mn2[i];
    date[dp++] = ' ';
    date[dp++] = (char)('0' + (yr/1000)%10); date[dp++] = (char)('0' + (yr/100)%10);
    date[dp++] = (char)('0' + (yr/10)%10);    date[dp++] = (char)('0' + yr%10);
    date[dp] = '\0';
    int dw = dp * 8;
    g.draw_str(cx0 + (cw - dw)/2, ty + 20, date, Theme::AMBER, GFX_TRANSPARENT);

    /* Calendar navigation buttons. */
    int by = bar_y();
    draw_btn(g, btn_prev_x(),  by, 30, 20, "<",         false);
    draw_btn(g, btn_next_x(),  by, 30, 20, ">",         false);
    draw_btn(g, btn_today_x(), by, 88, 20, "Auj.",      false);

    /* Calendar. */
    draw_calendar(cx0 + 8, by + 26, cw - 16);

    /* Stopwatch row. */
    int sy = sw_y();
    draw_stopwatch(cx0 + 8, sy - 2, cw);
    draw_btn(g, btn_sw_x(),   sy, 72, 22, m_sw_run ? "Stop" : "Start", false);
    draw_btn(g, btn_swrz_x(), sy, 60, 22, "Raz", false);
}

/* ── events ──────────────────────────────────────────────────────────────── */
static bool in_btn(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

bool ClockWindow::on_event(const Event& e) {
    if (e.type == EVT_MOUSE_BUTTON && (e.mouse.buttons & 0x01) && hit_test(e.mouse.x, e.mouse.y)) {
        int mx = e.mouse.x, my = e.mouse.y;
        int by = bar_y(), sy = sw_y();
        if (in_btn(mx, my, btn_prev_x(), by, 30, 20)) {
            if (--m_cal_month < 1) { m_cal_month = 12; m_cal_year--; } return true;
        }
        if (in_btn(mx, my, btn_next_x(), by, 30, 20)) {
            if (++m_cal_month > 12) { m_cal_month = 1; m_cal_year++; } return true;
        }
        if (in_btn(mx, my, btn_today_x(), by, 88, 20)) {
            m_cal_year = 0; sync_calendar(); return true;
        }
        if (in_btn(mx, my, btn_sw_x(), sy, 72, 22)) {
            if (m_sw_run) { m_sw_acc += jiffies - m_sw_start; m_sw_run = false; }
            else { m_sw_start = jiffies; m_sw_run = true; }
            return true;
        }
        if (in_btn(mx, my, btn_swrz_x(), sy, 60, 22)) {
            m_sw_run = false; m_sw_acc = 0; m_sw_start = jiffies; return true;
        }
    }
    return Window::on_event(e);
}

bool ClockWindow::handle_char(int c) {
    if (c == KEY_LEFT)  { if (--m_cal_month < 1) { m_cal_month = 12; m_cal_year--; } return true; }
    if (c == KEY_RIGHT) { if (++m_cal_month > 12){ m_cal_month = 1;  m_cal_year++; } return true; }
    if (c == 't' || c == 'T') { m_cal_year = 0; sync_calendar(); return true; }
    if (c == ' ') {
        if (m_sw_run) { m_sw_acc += jiffies - m_sw_start; m_sw_run = false; }
        else { m_sw_start = jiffies; m_sw_run = true; }
        return true;
    }
    if (c == KEY_ESC) { request_close(); return true; }
    return false;
}

/* ── factory ─────────────────────────────────────────────────────────────── */
extern "C" void open_clock_window(void) {
    auto w = Greg::make_ref<ClockWindow>();
    w->setup(210, 40, 400, 452, "Horloge", Theme::WIN_BG);
    w->set_focused(true);
    WindowManager::instance().add_window(Greg::move(w));
}
