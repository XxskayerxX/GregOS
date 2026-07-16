#ifndef KERNEL_PS2MOUSE_C_H
#define KERNEL_PS2MOUSE_C_H

/* C-visible bridge to Kernel::PS2Mouse (implemented in kernel/PS2Mouse.cpp).
   Include this from .c files that need mouse state.                       */

#ifdef __cplusplus
extern "C" {
#endif

void ps2mouse_init(void);

/* Pixel-space cursor position, updated by IRQ12. */
int  ps2mouse_gui_x(void);
int  ps2mouse_gui_y(void);

/* Button bitmask: bit0=Left, bit1=Right, bit2=Middle. */
int  ps2mouse_buttons(void);

/* Text-mode cursor overlay — show/hide the cursor sprite on the VGA cell. */
void ps2mouse_cursor_show(void);
void ps2mouse_cursor_hide(void);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_PS2MOUSE_C_H */
