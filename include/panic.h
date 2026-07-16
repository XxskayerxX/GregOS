#ifndef PANIC_H
#define PANIC_H

/* Stack frame built by our ISR stubs (Ring-0 same-privilege exception).
   Layout after pusha + CPU-pushed fields (no user-stack push for R0→R0):

   [esp+0 ]  EDI   \
   [esp+4 ]  ESI    |  pusha
   [esp+8 ]  EBP    |
   [esp+12]  ESP*   |  (* value before pusha, not the real interrupted ESP)
   [esp+16]  EBX    |
   [esp+20]  EDX    |
   [esp+24]  ECX    |
   [esp+28]  EAX   /
   [esp+32]  err_code   (CPU-pushed, or 0 for no-error-code exceptions)
   [esp+36]  EIP        (CPU-pushed interrupted instruction pointer)
   [esp+40]  CS         (CPU-pushed)
   [esp+44]  EFLAGS     (CPU-pushed)                                       */

struct panic_regs {
    unsigned int edi, esi, ebp, esp_dummy;
    unsigned int ebx, edx, ecx, eax;
    unsigned int err_code;
    unsigned int eip, cs, eflags;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Called by the ASM stubs; never return. */
void fault0_handler (struct panic_regs* regs, unsigned int cr2);
void fault6_handler (struct panic_regs* regs, unsigned int cr2);
void fault8_handler (struct panic_regs* regs, unsigned int cr2);
void fault13_handler(struct panic_regs* regs, unsigned int cr2);
void fault14_handler(struct panic_regs* regs, unsigned int cr2);

/* Generic entry point — can be called from C for software panics. */
void kernel_panic(struct panic_regs* regs, const char* exc_name,
                  unsigned int cr2);

#ifdef __cplusplus
}
#endif

#endif /* PANIC_H */
