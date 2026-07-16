

MULTIBOOT2_MAGIC    equ 0xE85250D6
MULTIBOOT2_ARCH     equ 0

section .multiboot2
align 8
header_start:
    dd MULTIBOOT2_MAGIC
    dd MULTIBOOT2_ARCH
    dd header_end - header_start
    dd 0x100000000 - (MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + (header_end - header_start))

    align 8
.fb_tag:
    dw 5
    dw 0
    dd 20
    dd 800
    dd 600
    dd 32

    align 8
.end_tag:
    dw 0
    dw 0
    dd 8
header_end:


section .bss
align 16
stack_bottom:
    resb 16384
stack_top:


section .text
global _start:function (_start.end - _start)
extern kmain
extern __init_array_start
extern __init_array_end

_start:
    mov esp, stack_top

    ; Call C++ global constructors (.init_array / .ctors) before kmain
    mov  edi, __init_array_start
.ctors_loop:
    cmp  edi, __init_array_end
    jge  .ctors_done
    call [edi]
    add  edi, 4
    jmp  .ctors_loop
.ctors_done:

    push 0
    push ebx
    call kmain
    add esp, 8
    cli
.hang:
    hlt
    jmp .hang
.end:
