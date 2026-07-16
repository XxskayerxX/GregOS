#ifndef KERNEL_SYSCALL_HPP
#define KERNEL_SYSCALL_HPP

/* Kernel::Syscall — INT 0x80 dispatch table for GregOS.
   Freestanding: no libc, no exceptions.

   ABI (Linux i386 convention):
     EAX = syscall number (see SyscallNumber enum)
     EBX = arg 1
     ECX = arg 2
     EDX = arg 3
     ESI = arg 4
     EDI = arg 5
   Return value written to frame->eax before iret restores it.

   ┌────┬────────────────┬─────────────────────────────────────────────────┐
   │ Nr │ Name           │ Arguments                                       │
   ├────┼────────────────┼─────────────────────────────────────────────────┤
   │  1 │ SYS_EXIT       │ ebx=exit_code                                   │
   │  2 │ SYS_FILL_RECT  │ ebx=x  ecx=y  edx=w  esi=h  edi=color          │
   │  3 │ SYS_WRITE      │ ebx=fd  ecx=buf_ptr  edx=len → eax=bytes_written│
   │  4 │ SYS_YIELD      │ (none) — schedule next thread                   │
   │  5 │ SYS_GET_TICKS  │ (none) → eax=jiffies                            │
   │  6 │ SYS_GET_HEAP   │ ebx=out_used_ptr  ecx=out_total_ptr             │
   │  7 │ SYS_MMAP       │ ebx=size → eax=ptr (0 on failure)              │
   │  8 │ SYS_MUNMAP     │ ebx=ptr → eax=0                                 │
   │  9 │ SYS_OPEN       │ ebx=name_ptr  ecx=parent_id → eax=fd (-1 fail) │
   │ 10 │ SYS_READ       │ ebx=fd  ecx=buf_ptr  edx=len → eax=bytes (-1)  │
   │ 11 │ SYS_CLOSE      │ ebx=fd → eax=0                                  │
   │ 12 │ SYS_GET_PID    │ (none) → eax=current thread id                 │
   │ 13 │ SYS_CREATE     │ ebx=name_ptr  ecx=parent_id → eax=fd (-1 fail) │
   │ 14 │ SYS_WRITE_FILE │ ebx=fd  ecx=buf_ptr  edx=len → eax=bytes (-1)  │
   │ 15 │ SYS_LSEEK      │ ebx=fd  ecx=offset  edx=whence → eax=newpos    │
   └────┴────────────────┴─────────────────────────────────────────────────┘

   File descriptors are now small per-process integers (0/1/2 reserved for
   stdin/stdout/stderr; open()/create() return ≥ 3) with a byte offset that
   advances on read/write — see the per-process FD table in kernel.c.        */

#include "../Greg/Types.h"

namespace Kernel {

enum SyscallNumber : Greg::u32 {
    SYS_INVALID    = 0,
    SYS_EXIT       = 1,
    SYS_FILL_RECT  = 2,
    SYS_WRITE      = 3,
    SYS_YIELD      = 4,
    SYS_GET_TICKS  = 5,
    SYS_GET_HEAP   = 6,
    SYS_MMAP       = 7,
    SYS_MUNMAP     = 8,
    SYS_OPEN       = 9,
    SYS_READ       = 10,
    SYS_CLOSE      = 11,
    SYS_GET_PID    = 12,
    SYS_CREATE     = 13,
    SYS_WRITE_FILE = 14,
    SYS_LSEEK      = 15,
    SYS_COUNT,
};

/* Register frame as pushed by pusha in syscall_stub (arch/i386/isr.asm).
   Same memory layout as struct registers_t in kernel.c.                   */
struct SyscallFrame {
    Greg::u32 edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
};

class Syscall {
public:
    /* Main dispatch entry point — called from syscall_handler (C bridge). */
    static void dispatch(SyscallFrame* frame);
};

} /* namespace Kernel */

#endif /* KERNEL_SYSCALL_HPP */
