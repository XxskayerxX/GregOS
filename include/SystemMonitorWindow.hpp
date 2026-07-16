#ifndef SYSTEM_MONITOR_WINDOW_HPP
#define SYSTEM_MONITOR_WINDOW_HPP

/* SystemMonitorWindow — live kernel heap usage graph.
   Samples k_heap_pos every second and feeds a GraphWidget.
   Freestanding: no libc, no exceptions.                                      */

#include "Window.hpp"
#include "GUI/GraphWidget.hpp"

class SystemMonitorWindow : public Window {
public:
    void draw() override;

private:
    /* Graph placed 60px below the client top, spanning most of the width */
    GUI::GraphWidget m_graph { 8, 60, 364, 150 };
    unsigned long    m_last_push { 0 };

    static int sm_itos(unsigned int n, char* buf);
    static int sm_cat(char* dst, int pos, const char* s);
};

extern "C" void open_system_monitor_window(void);

#endif /* SYSTEM_MONITOR_WINDOW_HPP */
