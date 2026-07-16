#ifndef CASINO_WINDOW_HPP
#define CASINO_WINDOW_HPP

#include "Window.hpp"

class CasinoWindow : public Window {
public:
    void draw() override;
    bool on_event(const Event& e) override;
    bool handle_char(int c) override;

private:
    char          m_status[72] {};
    unsigned long m_status_until { 0 };  /* jiffies expiry for status message */
};

#endif /* CASINO_WINDOW_HPP */
