; switch_task(uint32_t old_esp_ptr, uint32_t new_esp)  — cdecl i386
;
; Saves the current execution context (all GP regs + EFLAGS) onto the
; current stack, writes the resulting ESP into *old_esp_ptr, then loads
; new_esp as the new stack pointer and restores the new task's context.
;
; Stack layout on entry (before any push):
;   [esp+0]  return address
;   [esp+4]  old_esp_ptr  (pointer: &thread.esp of the running thread)
;   [esp+8]  new_esp      (value:   thread.esp  of the next thread)
;
; After pushf (4 B) + pusha (32 B) = 36 B pushed, params shift to:
;   [esp+36]  return address
;   [esp+40]  old_esp_ptr
;   [esp+44]  new_esp
;
; New-thread initial stack expected by this routine:
;   [esp+0..31]  — 8 zero dwords  (popa frame)
;   [esp+32]     — 0x00000202     (popf EFLAGS: IF=1, reserved bit 1)
;   [esp+36]     — entry_point    (ret will jump here on first run)
;   [esp+40]     — exit_stub      (if entry_point ever returns)

global switch_task
section .text

switch_task:
    pushf                       ; save EFLAGS
    pusha                       ; save EDI,ESI,EBP,ESP,EBX,EDX,ECX,EAX

    mov eax, [esp + 40]         ; eax = old_esp_ptr
    mov ecx, [esp + 44]         ; ecx = new_esp  (load before clobbering esp)

    mov [eax], esp              ; *old_esp_ptr = current esp  (save this task)
    mov esp, ecx                ; esp = new_esp               (switch to new task)

    popa                        ; restore new task's GP registers
    popf                        ; restore new task's EFLAGS (sets IF if needed)
    ret                         ; jump to new task's return address
