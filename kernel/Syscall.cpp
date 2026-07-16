/* Kernel::Syscall — INT 0x80 dispatch table.
   Replaces the syscall_handler() stub previously defined in kernel.c.
   Freestanding: no libc, no exceptions.                                   */

#include "../include/Kernel/Syscall.hpp"
#include "../include/Kernel/MemoryManager.hpp"
#include "../include/Kernel/Scheduler.hpp"

extern "C" volatile unsigned long jiffies;
extern "C" void gfx_fill_rect(int x, int y, int w, int h, unsigned int color);
extern "C" void term_putc(char c);
/* VFS bridge (implemented in kernel.c) — see include/vfs.h */
extern "C" int  vfs_find(const char* name, int parent_id);
extern "C" int  vfs_create_file(const char* name, int parent_id);
/* Per-process file-descriptor table (kernel.c). open()/create() map a VFS entry
   to a small fd (≥ 3); read/write advance a per-fd offset; lseek repositions.   */
extern "C" int  fd_open_id(int tid, int vfs_id);
extern "C" int  fd_read(int tid, int fd, char* buf, int len);
extern "C" int  fd_write(int tid, int fd, const char* buf, int len);
extern "C" int  fd_close(int tid, int fd);
extern "C" int  fd_lseek(int tid, int fd, int off, int whence);
/* Per-process user heap (kernel.c). vm_mmap_current returns a user VA in the
   calling isolated process's own address space, or 0 if the caller runs in the
   shared kernel address space (→ fall back to the kernel heap).              */
extern "C" unsigned int vm_mmap_current(unsigned int size);
extern "C" int          vm_munmap_current(unsigned int addr);
/* User-pointer validation (kernel.c). vm_validate_user_range returns 1 iff every
   page of [addr, addr+len) is Present + User in the current cr3 (and R/W when
   need_write). vm_copy_user_string safely copies a NUL-terminated user string
   into a kernel buffer. Together they are GregOS's copy_from_user/to_user guard:
   a Ring-3 caller can never make a syscall touch kernel memory via a raw pointer. */
extern "C" int vm_validate_user_range(unsigned int addr, unsigned int len, int need_write);
extern "C" int vm_copy_user_string(char* dst, unsigned int uaddr, int maxlen);

/* SyscallFrame must mirror the pusha layout (8 × 32-bit registers = 32 bytes).
   This assertion catches any accidental struct divergence from registers_t
   defined in kernel.c, which has an identical memory layout.               */
static_assert(sizeof(Kernel::SyscallFrame) == 8 * sizeof(Greg::u32),
    "SyscallFrame size mismatch: must match the pusha register frame");

namespace Kernel {

/* ── sys_exit ────────────────────────────────────────────────────────────── */
static void sys_exit(SyscallFrame* f)
{
    (void)f->ebx;   /* exit code — reserved for Phase 5 Process::exit() */
    /* Terminate the thread: mark it Zombie and yield. Its scheduler slot,
       stacks and per-process VM slot are reclaimed by the reaper on the next
       tick from another thread. The hlt loop is a safety net until then.  */
    Scheduler::instance().exit_current();
    for (;;) __asm__ volatile("hlt");
}

/* ── sys_fill_rect ───────────────────────────────────────────────────────── */
static void sys_fill_rect(SyscallFrame* f)
{
    gfx_fill_rect(static_cast<int>(f->ebx),
                  static_cast<int>(f->ecx),
                  static_cast<int>(f->edx),
                  static_cast<int>(f->esi),
                  f->edi);
}

/* ── sys_write ───────────────────────────────────────────────────────────── */
static void sys_write(SyscallFrame* f)
{
    /* fd=ebx (1=stdout only for now). The buffer is a user pointer: reject it
       unless the whole range is user-readable, so a Ring-3 caller cannot leak
       kernel memory to the terminal.                                          */
    unsigned int uaddr = f->ecx;
    unsigned int len   = f->edx;
    if (len == 0)          { f->eax = 0; return; }
    if (len > 65536u)      { f->eax = static_cast<Greg::u32>(-1); return; }  /* runaway */
    if (!vm_validate_user_range(uaddr, len, /*need_write=*/0)) {
        f->eax = static_cast<Greg::u32>(-1); return;
    }
    const char* buf = reinterpret_cast<const char*>(uaddr);
    for (unsigned int i = 0; i < len; ++i)
        term_putc(buf[i]);
    f->eax = len;
}

/* ── sys_yield ───────────────────────────────────────────────────────────── */
static void sys_yield(SyscallFrame*)
{
    Scheduler::instance().tick();
}

/* ── sys_get_ticks ───────────────────────────────────────────────────────── */
static void sys_get_ticks(SyscallFrame* f)
{
    f->eax = static_cast<Greg::u32>(jiffies);
}

/* ── sys_get_heap ────────────────────────────────────────────────────────── */
static void sys_get_heap(SyscallFrame* f)
{
    /* ebx/ecx are user pointers the kernel writes into: each must be a
       user-writable 4-byte slot, or the syscall is refused (no kernel write). */
    if (f->ebx && !vm_validate_user_range(f->ebx, sizeof(Greg::u32), /*need_write=*/1)) {
        f->eax = static_cast<Greg::u32>(-1); return;
    }
    if (f->ecx && !vm_validate_user_range(f->ecx, sizeof(Greg::u32), /*need_write=*/1)) {
        f->eax = static_cast<Greg::u32>(-1); return;
    }
    auto* used_out  = reinterpret_cast<Greg::u32*>(f->ebx);
    auto* total_out = reinterpret_cast<Greg::u32*>(f->ecx);
    if (used_out)  *used_out  = MemoryManager::instance().used_bytes();
    if (total_out) *total_out = MemoryManager::instance().total_bytes();
    f->eax = 0;
}

/* Ledger of kernel-heap pointers handed out by the sys_mmap shared-space
   fallback. sys_munmap will ONLY free an address that appears here, so a Ring-3
   caller cannot pass an arbitrary pointer to the kernel allocator's free().
   Isolated processes never touch this table (they use the per-process user heap
   via vm_mmap_current/vm_munmap_current).                                      */
static const int MAX_KHEAP_MMAP = 32;
static Greg::u32 s_kheap_mmap[MAX_KHEAP_MMAP];   /* BSS zero-init; 0 = empty */

static bool kheap_track(Greg::u32 p)
{
    for (int i = 0; i < MAX_KHEAP_MMAP; ++i)
        if (s_kheap_mmap[i] == 0) { s_kheap_mmap[i] = p; return true; }
    return false;   /* table full → cannot track (caller must free immediately) */
}
static bool kheap_untrack(Greg::u32 p)
{
    for (int i = 0; i < MAX_KHEAP_MMAP; ++i)
        if (s_kheap_mmap[i] == p) { s_kheap_mmap[i] = 0; return true; }
    return false;
}

/* ── sys_mmap ────────────────────────────────────────────────────────────── */
static void sys_mmap(SyscallFrame* f)
{
    /* ebx = requested size (capped at 1 MB). For an *isolated* process, map
       fresh User pages into its own address space and return that user VA. For
       a shared-space thread (cr3 == kernel dir), vm_mmap_current returns 0 and
       we fall back to the kernel heap — which such a thread can still touch.   */
    unsigned int size = f->ebx;
    if (size == 0 || size > (1u << 20)) { f->eax = 0; return; }
    unsigned int uva = vm_mmap_current(size);
    if (uva) { f->eax = uva; return; }
    void* p = MemoryManager::instance().allocate(size);
    Greg::u32 pv = reinterpret_cast<Greg::u32>(p);
    if (pv && !kheap_track(pv)) {   /* ledger full → don't leak an untrackable block */
        MemoryManager::instance().free(p);
        f->eax = 0; return;
    }
    f->eax = pv;
}

/* ── sys_munmap ──────────────────────────────────────────────────────────── */
static void sys_munmap(SyscallFrame* f)
{
    unsigned int addr = f->ebx;
    if (!addr)                    { f->eax = 0; return; }
    if (vm_munmap_current(addr))  { f->eax = 0; return; }   /* per-process user heap */
    /* Otherwise it can only be a kernel-heap block we issued. Refuse anything
       not in the ledger — never free an unvalidated user address.            */
    if (kheap_untrack(addr)) {
        MemoryManager::instance().free(reinterpret_cast<void*>(addr));
        f->eax = 0; return;
    }
    f->eax = static_cast<Greg::u32>(-1);   /* not a valid mmap handout → reject */
}

/* ── sys_open ────────────────────────────────────────────────────────────── */
static void sys_open(SyscallFrame* f)
{
    /* ebx = name ptr, ecx = parent id (0 = current dir). Copy the name safely,
       resolve it to a VFS entry, then allocate a small per-process descriptor
       (≥ 3) pointing at it — the returned fd is a process-local integer, not the
       VFS id.                                                                 */
    char name[64];
    if (vm_copy_user_string(name, f->ebx, sizeof(name)) < 0) {
        f->eax = static_cast<Greg::u32>(-1); return;
    }
    int vfs_id = vfs_find(name, static_cast<int>(f->ecx));
    if (vfs_id < 0) { f->eax = static_cast<Greg::u32>(-1); return; }
    int fd = fd_open_id(static_cast<int>(Scheduler::instance().current_id()), vfs_id);
    f->eax = static_cast<Greg::u32>(fd);   /* -1 → 0xFFFFFFFF on failure */
}

/* ── sys_read ────────────────────────────────────────────────────────────── */
static void sys_read(SyscallFrame* f)
{
    /* ebx = fd, ecx = buf, edx = len. buf is where the kernel WRITES file bytes:
       reject unless the whole range is user-writable. The read starts at the
       descriptor's current offset and advances it.                           */
    int   fd  = static_cast<int>(f->ebx);
    int   len = static_cast<int>(f->edx);
    if (len <= 0 || len > 65536) { f->eax = static_cast<Greg::u32>(-1); return; }
    if (!vm_validate_user_range(f->ecx, static_cast<unsigned int>(len), /*need_write=*/1)) {
        f->eax = static_cast<Greg::u32>(-1); return;
    }
    char* buf = reinterpret_cast<char*>(f->ecx);
    int n = fd_read(static_cast<int>(Scheduler::instance().current_id()), fd, buf, len);
    f->eax = static_cast<Greg::u32>(n);
}

/* ── sys_close ───────────────────────────────────────────────────────────── */
static void sys_close(SyscallFrame* f)
{
    /* ebx = fd. Release the per-process descriptor (0/1/2 are no-op success). */
    int fd = static_cast<int>(f->ebx);
    f->eax = static_cast<Greg::u32>(
        fd_close(static_cast<int>(Scheduler::instance().current_id()), fd));
}

/* ── sys_lseek ───────────────────────────────────────────────────────────── */
static void sys_lseek(SyscallFrame* f)
{
    /* ebx = fd, ecx = offset, edx = whence (0=SET,1=CUR,2=END). → new offset. */
    int newpos = fd_lseek(static_cast<int>(Scheduler::instance().current_id()),
                          static_cast<int>(f->ebx),
                          static_cast<int>(f->ecx),
                          static_cast<int>(f->edx));
    f->eax = static_cast<Greg::u32>(newpos);
}

/* ── sys_create ──────────────────────────────────────────────────────────── */
static void sys_create(SyscallFrame* f)
{
    /* ebx = name ptr, ecx = parent id (0 = current dir). Open-or-create: if the
       file already exists we return its id so repeated runs don't exhaust the
       VFS; otherwise a fresh empty file is created. Ring-3 callers can then hand
       the returned "fd" to sys_write_file — the write path mirrors open/read.  */
    char name[64];
    if (vm_copy_user_string(name, f->ebx, sizeof(name)) < 0) {
        f->eax = static_cast<Greg::u32>(-1); return;
    }
    int parent = static_cast<int>(f->ecx);
    int vfs_id = vfs_find(name, parent);
    if (vfs_id < 0) vfs_id = vfs_create_file(name, parent);
    if (vfs_id < 0) { f->eax = static_cast<Greg::u32>(-1); return; }
    int fd = fd_open_id(static_cast<int>(Scheduler::instance().current_id()), vfs_id);
    f->eax = static_cast<Greg::u32>(fd);
}

/* ── sys_write_file ──────────────────────────────────────────────────────── */
static void sys_write_file(SyscallFrame* f)
{
    /* ebx = fd, ecx = buf, edx = len. buf is the user source the kernel READS:
       reject unless the whole range is user-readable, so a Ring-3 caller cannot
       write kernel memory into a file (info leak). The write starts at the
       descriptor's current offset and advances it.                           */
    int fd  = static_cast<int>(f->ebx);
    int len = static_cast<int>(f->edx);
    if (len < 0 || len > 65536) { f->eax = static_cast<Greg::u32>(-1); return; }
    if (len > 0 && !vm_validate_user_range(f->ecx, static_cast<unsigned int>(len), /*need_write=*/0)) {
        f->eax = static_cast<Greg::u32>(-1); return;
    }
    const char* buf = reinterpret_cast<const char*>(f->ecx);
    int n = fd_write(static_cast<int>(Scheduler::instance().current_id()), fd, buf, len);
    f->eax = static_cast<Greg::u32>(n);
}

/* ── sys_get_pid ─────────────────────────────────────────────────────────── */
static void sys_get_pid(SyscallFrame* f)
{
    f->eax = Scheduler::instance().current_id();
}

/* ── Dispatch table ──────────────────────────────────────────────────────── */
using SyscallFn = void(*)(SyscallFrame*);

static const SyscallFn s_table[SYS_COUNT] = {
    nullptr,        /* 0  SYS_INVALID    */
    sys_exit,       /* 1  SYS_EXIT       */
    sys_fill_rect,  /* 2  SYS_FILL_RECT  */
    sys_write,      /* 3  SYS_WRITE      */
    sys_yield,      /* 4  SYS_YIELD      */
    sys_get_ticks,  /* 5  SYS_GET_TICKS  */
    sys_get_heap,   /* 6  SYS_GET_HEAP   */
    sys_mmap,       /* 7  SYS_MMAP       */
    sys_munmap,     /* 8  SYS_MUNMAP     */
    sys_open,       /* 9  SYS_OPEN       */
    sys_read,       /* 10 SYS_READ       */
    sys_close,      /* 11 SYS_CLOSE      */
    sys_get_pid,    /* 12 SYS_GET_PID    */
    sys_create,     /* 13 SYS_CREATE     */
    sys_write_file, /* 14 SYS_WRITE_FILE */
    sys_lseek,      /* 15 SYS_LSEEK      */
};

void Syscall::dispatch(SyscallFrame* frame)
{
    Greg::u32 nr = frame->eax;
    if (nr < static_cast<Greg::u32>(SYS_COUNT) && s_table[nr])
        s_table[nr](frame);
    /* Unknown syscall: EAX left unchanged (caller receives the nr back). */
}

} /* namespace Kernel */

/* ── C bridge ────────────────────────────────────────────────────────────────
   Called by syscall_stub (arch/i386/isr.asm) with a pointer to the pusha
   frame on the kernel stack.  The cast is safe: SyscallFrame and
   struct registers_t (kernel.c) have identical POD layout.                 */
extern "C" void syscall_handler(void* frame_ptr)
{
    Kernel::Syscall::dispatch(
        static_cast<Kernel::SyscallFrame*>(frame_ptr));
}
