; user.asm — a fully-isolated Ring-3 user program for GregOS.
;
; Assembled as a raw flat binary (`nasm -f bin`) and loaded by the kernel at
; virtual address 0x40000000 into a page-directory where:
;   * this code page (0x40000000) and a stack page (0x40001000) are User pages,
;   * ALL kernel memory is mapped supervisor-only.
; The program talks to the kernel ONLY through INT 0x80 (it never calls a
; kernel function). It prints two lines, then deliberately reads kernel memory
; at 0x00100000 — which faults at CPL=3 because the kernel is supervisor-only
; in its address space. The kernel's fault handler catches that and kills only
; this process, proving true per-process memory isolation.

BITS 32
org 0x40000000

; ── Syscall numbers (mirror Kernel::SyscallNumber) ──
%define SYS_EXIT   1
%define SYS_WRITE  3

_start:
    ; SYS_WRITE(fd=1, msg1, len1)
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, msg1
    mov edx, len1
    int 0x80

    ; SYS_WRITE(fd=1, msg2, len2)
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, msg2
    mov edx, len2
    int 0x80

    ; Try to read kernel memory at 0x00100000 (kernel load base).
    ; Supervisor-only in this address space → #PF at CPL=3 → handler kills us.
    mov eax, [0x00100000]

    ; Only reached if isolation were NOT enforced.
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, msg3
    mov edx, len3
    int 0x80

    mov eax, SYS_EXIT
    xor ebx, ebx
    int 0x80
.hang:
    jmp .hang

msg1: db 10, "[ELF] Bonjour depuis un vrai processus ELF isole (CPL=3)!", 10
len1 equ $ - msg1
msg2: db "[ELF] Je lis la memoire noyau a 0x00100000 (interdit)...", 10
len2 equ $ - msg2
msg3: db "[ELF] Lecture noyau reussie -- PAS isole !", 10
len3 equ $ - msg3
