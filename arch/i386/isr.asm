global irq0_stub
global irq1_stub
global irq12_stub
global irq_net_stub
global gdt_flush
global tss_flush
global jump_usermode
global syscall_stub
global user_thread_stub
global isr0_stub
global isr6_stub
global isr8_stub
global isr13_stub
global isr14_stub
extern irq0_handler
extern irq1_handler
extern irq12_handler
extern irq_net_handler
extern syscall_handler
extern fault0_handler
extern fault6_handler
extern fault8_handler
extern fault13_handler
extern fault14_handler
section .text

; ── Segment-fixup note ─────────────────────────────────────────────────────
; When the CPU returns from Ring 0 to Ring 3 via iret it automatically zeroes
; any data segment register whose descriptor DPL < new CPL.  So if DS=0x10
; (kernel, DPL=0) on iret to Ring 3 (CPL=3), the CPU zeroes DS → #GP on
; the very next user data access.
;
; Fix-up pattern used in every interrupt stub:
;   • On entry  : reload DS/ES = 0x10 (kernel) so Ring-0 C code always
;                 runs with a valid kernel data segment regardless of which
;                 thread was interrupted.
;   • On return : test CS (saved at [esp+36] after pusha) for RPL=3;
;                 if returning to Ring 3 reload DS/ES/FS/GS = 0x23 (user).
;
; EAX is freely usable between pusha and popa because popa will restore it
; from the frame — any clobber here is invisible to the interrupted task.
;
; Stack layout after pusha (interrupt fired from any ring):
;   [esp+0..31]  pusha frame   (EDI,ESI,EBP,ESP_orig,EBX,EDX,ECX,EAX)
;   [esp+32]     EIP           (interrupted instruction pointer)
;   [esp+36]     CS            (interrupted code segment)
;   [esp+40]     EFLAGS
;   [esp+44]     ESP_user      (only present when ring change: Ring3→Ring0)
;   [esp+48]     SS_user       (only present when ring change)
; ───────────────────────────────────────────────────────────────────────────

; IRQ0 — PIT timer tick (100 Hz)
irq0_stub:
    pusha
    mov ax, 0x10            ; reload kernel DS in case we came from Ring 3
    mov ds, ax
    mov es, ax
    call irq0_handler
    test byte [esp+36], 0x02    ; CS RPL bit 1 set → returning to Ring 3
    jz .k0
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
.k0:
    popa
    iret

; IRQ1 — PS/2 keyboard
irq1_stub:
    pusha
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    call irq1_handler
    test byte [esp+36], 0x02
    jz .k1
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
.k1:
    popa
    iret

; RTL8139 network RX — vector chosen at runtime from PCI config 0x3C
; (installed by kernel_net_irq_install, called from net_init)
irq_net_stub:
    pusha
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    call irq_net_handler
    test byte [esp+36], 0x02
    jz .knet
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
.knet:
    popa
    iret

; IRQ12 — PS/2 mouse
irq12_stub:
    pusha
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    call irq12_handler
    test byte [esp+36], 0x02
    jz .k12
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
.k12:
    popa
    iret

; Load TSS — selector 0x2B = GDT index 5 (0x28), RPL=3
tss_flush:
    mov ax, 0x2B
    ltr ax
    ret

; jump_usermode(uint32_t eip, uint32_t user_esp) — cdecl
; One-shot privilege drop used for the initial Ring-3 test.
jump_usermode:
    cli
    mov eax, [esp+4]
    mov ecx, [esp+8]
    mov dx, 0x23
    mov ds, dx
    mov es, dx
    mov fs, dx
    mov gs, dx
    push 0x23
    push ecx
    pushfd
    or dword [esp], 0x200
    push 0x1B
    push eax
    iret

; INT 0x80 syscall dispatcher — IDT gate DPL=3 so Ring 3 can invoke it.
; After pusha: [esp+0..31]=pusha frame, [esp+32]=EIP_user, [esp+36]=CS_user.
; We pass a pointer to the pusha frame so syscall_handler reads real registers.
syscall_stub:
    pusha
    mov eax, esp            ; pointer to struct registers_t (pusha'd frame)
    push eax
    call syscall_handler
    add esp, 4
    ; Always returning to Ring 3 — reload user data segments before iret
    ; so the CPU does not zero DS on the privilege change.
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popa
    iret

; user_thread_stub — entered via switch_task's 'ret' on a user thread's
; first run.  At this point the kernel stack holds a Ring-3 iret frame:
;   [esp+0]   EIP_user   (entry point)
;   [esp+4]   0x1B       (user CS, RPL=3)
;   [esp+8]   0x202      (EFLAGS: IF=1)
;   [esp+12]  ESP_user   (top of user stack)
;   [esp+16]  0x23       (user SS, RPL=3)
; Reload segment registers before iret so the CPU accepts the return.
user_thread_stub:
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    iret

; ── CPU Exception stubs ────────────────────────────────────────────────────
;
; Calling convention used here (cdecl):
;   handler(struct panic_regs* regs, uint32_t cr2)
;   → push cr2 first (2nd arg), then push regs ptr (1st arg), then call.
;
; Exceptions WITH error code (CPU already pushed it before EIP/CS/EFLAGS):
;   ISR 8, 13, 14 — CPU stack on entry: [error_code, EIP, CS, EFLAGS]
;
; Exceptions WITHOUT error code:
;   ISR 0, 6 — we push a fake 0 to keep the struct layout identical.
;
; After pusha the struct panic_regs sits at esp:
;   [esp+ 0] EDI .. [esp+28] EAX   (pusha frame)
;   [esp+32] err_code
;   [esp+36] EIP, [esp+40] CS, [esp+44] EFLAGS
;
; We then push cr2 (dec esp by 4), so struct base = esp+4.
; Then push regs ptr = esp+4 (after push).
; Call → inside handler: [esp+4]=regs_ptr, [esp+8]=cr2.
; ───────────────────────────────────────────────────────────────────────────

%macro ISR_NOERR 2          ; %1 = stub label, %2 = C handler name
%1:
    push dword 0            ; fake error code
    pusha
    push dword 0            ; cr2 = 0
    lea  eax, [esp+4]       ; &struct panic_regs
    push eax
    call %2
.hang:
    cli
    hlt
    jmp .hang
%endmacro

%macro ISR_ERR 2            ; %1 = stub label, %2 = C handler name
%1:
    ; CPU already pushed error_code — do NOT push fake 0
    pusha
    push dword 0            ; cr2 = 0 (may be overridden for ISR 14)
    lea  eax, [esp+4]       ; &struct panic_regs
    push eax
    call %2
.hang:
    cli
    hlt
    jmp .hang
%endmacro

ISR_NOERR isr0_stub,  fault0_handler   ; #DE  Divide Error
ISR_NOERR isr6_stub,  fault6_handler   ; #UD  Invalid Opcode
ISR_ERR   isr8_stub,  fault8_handler   ; #DF  Double Fault

; ISR 13 — General Protection Fault (error code, no CR2)
isr13_stub:
    pusha
    push dword 0            ; cr2 = 0 (not applicable)
    lea  eax, [esp+4]
    push eax
    call fault13_handler
.hang13:
    cli
    hlt
    jmp .hang13

; ISR 14 — Page Fault (error code + CR2 holds faulting address)
isr14_stub:
    pusha
    mov  eax, cr2           ; read faulting address before any other push
    push eax                ; cr2 (2nd arg)
    lea  eax, [esp+4]       ; &struct panic_regs (1st arg)
    push eax
    call fault14_handler
.hang14:
    cli
    hlt
    jmp .hang14

; Load a new GDT (pointer in [esp+4]) and reload segment registers
gdt_flush:
    mov eax, [esp+4]
    lgdt [eax]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.flush
.flush:
    ret
