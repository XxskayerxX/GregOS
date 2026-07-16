#ifndef KERNEL_PS2MOUSE_HPP
#define KERNEL_PS2MOUSE_HPP

/* Kernel::PS2Mouse — PS/2 mouse driver (IRQ12).
   Inspired by SerenityOS Kernel::PS2MouseDevice.
   Static-only class: no instances, no heap allocation.

   Extracted from kernel/kernel.c (irq12_handler + mouse_init block).
   C bridges in kernel/PS2Mouse.cpp expose the mouse position and buttons
   to kernel.c via ps2mouse_c.h.                                           */

namespace Kernel {

class PS2Mouse {
public:
    /* Enable the PS/2 mouse via the 8042 controller. Must be called after
       IDT is installed and interrupts are enabled (STI).                  */
    static void initialize();

    /* Called from isr.asm irq12_stub. Reads the 3-byte PS/2 packet,
       updates pixel-space (gui_x/y) and text-space (col/row) coordinates,
       and pushes an EVT_MOUSE event onto the EventQueue in GUI mode.      */
    static void handle_irq();

    /* Pixel-space cursor coordinates (800×600 clipped). */
    static int gui_x();
    static int gui_y();

    /* Current button bitmask: bit0=Left, bit1=Right, bit2=Middle.        */
    static int buttons();

    /* Text-mode (80×25) cursor overlay — called by kernel.c when repainting. */
    static void draw();
    static void erase();

private:
    PS2Mouse()  = delete;
    ~PS2Mouse() = delete;

    /* PS/2 controller busy-wait helpers (port 0x64).
       Return true on success, false on timeout.           */
    static bool ps2_wait_write();
    static bool ps2_wait_read();
};

} // namespace Kernel

#endif /* KERNEL_PS2MOUSE_HPP */
