#ifndef KEYBOARD_H
#define KEYBOARD_H

#ifdef __cplusplus
extern "C" {
#endif

#define KEY_UP     128
#define KEY_DOWN   129
#define KEY_LEFT   130
#define KEY_RIGHT  131
#define KEY_CTRL_S 132
#define KEY_CTRL_C 133
#define KEY_PGUP   134
#define KEY_PGDN   135
#define KEY_HOME   136
#define KEY_END    137
#define KEY_DELETE 138
#define KEY_INSERT 139
#define KEY_ESC    0x1B

int  get_monitor_char(void);

/* Convert a raw PS/2 scancode to a character (or 0 if non-printable).
   Used by WindowManager::route_key() to process keyboard events.       */
int  kb_scancode_to_char(unsigned char sc);

/* Inject a pre-processed character into the GUI injection buffer so that
   get_monitor_char() returns it on the next call. Called by
   TerminalWindow::handle_char() to bridge WM keyboard events to the shell. */
void kb_inject_char(int c);
void kb_inject_flush(void);   /* discard all pending injected chars */

extern int kb_idt_active;

void irq1_handler(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
