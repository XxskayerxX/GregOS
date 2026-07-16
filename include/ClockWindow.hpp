#ifndef CLOCK_WINDOW_HPP
#define CLOCK_WINDOW_HPP

#include "Window.hpp"

/* ClockWindow — analog clock + digital time + month calendar + stopwatch.
   All integer math (no libm). Reads the RTC every frame in draw().          */

class ClockWindow : public Window {
public:
    void draw()                   override;
    bool on_event(const Event& e) override;
    bool handle_char(int c)       override;

private:
    /* Calendar navigation offset relative to the current RTC month. */
    int  m_cal_year  { 0 };     /* 0 = uninitialised → sync to RTC on draw */
    int  m_cal_month { 0 };     /* 1..12 */

    /* Stopwatch (jiffies-based). */
    bool          m_sw_run     { false };
    unsigned long m_sw_start   { 0 };   /* jiffies at last start           */
    unsigned long m_sw_acc     { 0 };   /* accumulated jiffies while paused */

    void sync_calendar();
    void draw_analog(int cx, int cy, int r, int hh, int mm, int ss);
    void draw_calendar(int x, int y, int w);
    void draw_stopwatch(int x, int y, int w);

    int  btn_prev_x() const;
    int  btn_next_x() const;
    int  btn_today_x() const;
    int  btn_sw_x()  const;
    int  btn_swrz_x() const;
    int  bar_y()     const;     /* y of the calendar-nav button row         */
    int  sw_y()      const;     /* y of the stopwatch button row            */
};

extern "C" void open_clock_window(void);

#endif /* CLOCK_WINDOW_HPP */
