#ifndef EVENT_H
#define EVENT_H

/* Event type constants */
#define EVT_NONE         0
#define EVT_KEY_PRESS    1
#define EVT_KEY_RELEASE  2
#define EVT_MOUSE_MOVE   3
#define EVT_MOUSE_BUTTON 4

/* Portable event record — same layout in C and C++.
   Anonymous outer union is a C11/GCC extension; inner struct members are named. */
typedef struct {
    unsigned char type;       /* EVT_* constant */
    unsigned char _p0[3];
    unsigned int  timestamp;  /* jiffies snapshot (0 if not set) */
    union {
        struct { int key; unsigned char scancode; } keyboard;
        struct { int x, y, dx, dy; unsigned char buttons; } mouse;
    };
} Event;

/* Set to 1 while the pixel-space GUI event loop is active.
   ISRs check this to route input to EventQueue vs. kb_buf. */
#ifdef __cplusplus
extern "C" {
#endif

extern int is_gui_active;

/* Push a raw PS/2 keyboard scancode (called from keyboard ISR) */
void event_push_key(unsigned char scancode);

/* Push pixel-space mouse state (called from irq12_handler) */
void event_push_mouse(int x, int y, int dx, int dy, unsigned char buttons);

/* Pop one event — returns 1 if an event was available, 0 if empty */
int event_pop(Event* out);

/* Returns 1 if the queue is empty */
int event_empty(void);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_H */
