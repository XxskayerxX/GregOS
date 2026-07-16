#include "../include/Event.hpp"

/* ── Singleton storage (BSS zero-init) ──────────────────────────────── */
static EventQueue s_queue;
EventQueue& EventQueue::instance() { return s_queue; }

/* ── EventQueue::push ────────────────────────────────────────────────── */
bool EventQueue::push(const Event& e) {
    unsigned char next = (unsigned char)(head_ + 1);
    if (next == tail_) return false;  /* full — drop event */
    buf_[head_] = e;
    head_ = next;
    return true;
}

/* ── EventQueue::pop ─────────────────────────────────────────────────── */
bool EventQueue::pop(Event& out) {
    if (head_ == tail_) return false;  /* empty */
    out  = buf_[tail_];
    tail_ = (unsigned char)(tail_ + 1);
    return true;
}

/* ── extern "C" definitions (C linkage — accessible from kernel.c / keyboard.c) */
extern "C" {

int is_gui_active = 0;

void event_push_key(unsigned char scancode) {
    Event e;
    e.type          = EVT_KEY_PRESS;
    e._p0[0]        = e._p0[1] = e._p0[2] = 0;
    e.timestamp     = 0;
    e.keyboard.key  = 0;
    e.keyboard.scancode = scancode;
    EventQueue::instance().push(e);
}

void event_push_mouse(int x, int y, int dx, int dy, unsigned char buttons) {
    static unsigned char prev_buttons = 0;
    Event e;
    /* Distinguish button state changes from pure cursor movement so that
       Window::on_event and dispatch_event can trigger close/drag/focus. */
    e.type          = (buttons != prev_buttons) ? EVT_MOUSE_BUTTON : EVT_MOUSE_MOVE;
    prev_buttons    = buttons;
    e._p0[0]        = e._p0[1] = e._p0[2] = 0;
    e.timestamp     = 0;
    e.mouse.x       = x;
    e.mouse.y       = y;
    e.mouse.dx      = dx;
    e.mouse.dy      = dy;
    e.mouse.buttons = buttons;
    EventQueue::instance().push(e);
}

int event_pop(Event* out) {
    return EventQueue::instance().pop(*out) ? 1 : 0;
}

int event_empty(void) {
    return EventQueue::instance().empty() ? 1 : 0;
}

} /* extern "C" */
