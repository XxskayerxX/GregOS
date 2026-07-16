#ifndef KERNEL_PS2KEYBOARD_HPP
#define KERNEL_PS2KEYBOARD_HPP

/* Kernel::PS2Keyboard — PS/2 keyboard driver (IRQ1).
   Inspired by SerenityOS Kernel::PS2KeyboardDevice.
   Static-only class: no instances, no heap allocation.

   Replaces drivers/keyboard.c entirely.
   C bridges in kernel/PS2Keyboard.cpp match the keyboard.h API so the rest
   of the codebase (kernel.c, WindowManager) calls the same symbols.        */

namespace Kernel {

class PS2Keyboard {
public:
    /* Called from isr.asm irq1_stub. Reads port 0x60 and routes the scancode
       to the EventQueue (GUI mode) or the raw ring buffer (shell mode).    */
    static void handle_irq();

    /* Returns the next character from the injection buffer (GUI mode) or
       the raw ring buffer (shell mode). Returns 0 when nothing is pending.
       In GUI mode, pumps the EventQueue and issues an HLT before returning 0. */
    static int  get_char();

    /* Push a pre-processed character into the GUI injection buffer.
       Called by TerminalWindow::handle_char() to bridge WM events to the shell. */
    static void inject_char(int c);

    /* Discard all pending injected characters. */
    static void inject_flush();

    /* Translate a raw PS/2 scancode (set 1) to an ASCII or KEY_* value.
       Returns 0 for non-printable / modifier-only scancodes.              */
    static int  scancode_to_char(unsigned char scancode);

    /* IDT-active flag is the C bridge variable kb_idt_active (keyboard.h).
       When 0, get_char() falls back to polling port 0x64 directly.
       Set by kernel.c: kb_idt_active = 1 after lidt + sti.               */

private:
    PS2Keyboard()  = delete;
    ~PS2Keyboard() = delete;

    static int process_scancode(unsigned char scancode);
};

} // namespace Kernel

#endif /* KERNEL_PS2KEYBOARD_HPP */
