#ifndef KERNEL_SCHEDULER_HPP
#define KERNEL_SCHEDULER_HPP

#include "../Greg/Types.h"

/* ── Assembly context-switch primitive ──────────────────────────────────
   Defined in arch/i386/switch_task.asm (cdecl i386).
   Saves pushf+pusha onto the current stack, writes ESP into *old_esp_ptr,
   loads new_esp, then restores the new thread's context and returns.     */
extern "C" void switch_task(Greg::u32 old_esp_ptr, Greg::u32 new_esp);

/* ── C bridges (callable from kernel.c) ────────────────────────────────*/
extern "C" void scheduler_tick_c(void);              /* called by irq0_handler */
extern "C" void scheduler_spawn(void (*entry)(void));
extern "C" void scheduler_spawn_user(void (*entry)(void));
/* Spawn a Ring-3 thread in its own address space (cr3 = page-dir phys addr). */
extern "C" void scheduler_spawn_user_vm(void (*entry)(void), unsigned int cr3);
/* Spawn a Ring-3 thread at an explicit user entry VA + user ESP in space cr3. */
extern "C" void scheduler_spawn_user_at(unsigned int entry_va,
                                        unsigned int user_esp, unsigned int cr3);
extern "C" void scheduler_activate(void);

/* Physical address of the kernel page directory — set once by paging_install
   (kernel.c) so the scheduler can restore it when switching to a thread that
   has no private address space (cr3 == 0).                                  */
extern "C" unsigned int vm_kernel_cr3(void);

namespace Kernel {

using Greg::u32;
using Greg::u8;

/* ── Thread state ────────────────────────────────────────────────────── */
enum class ThreadState : u8 {
    Invalid = 0,   /* slot not in use                  */
    Ready,         /* runnable, waiting for CPU         */
    Running,       /* currently executing               */
    Blocked,       /* waiting for I/O or event (revivable) */
    Zombie,        /* terminated, awaiting reaping         */
};

/* ── Thread: minimal schedulable context ────────────────────────────── */
struct Thread {
    u32         id;               /* immutable thread ID (= slot index)      */
    u32         esp;              /* saved kernel stack pointer (switch_task) */
    u32         stack_top;        /* kept for compat (= kernel_stack_top)    */
    u32         kernel_stack_top; /* top of kernel stack → written to TSS    */
    u32         user_stack_top;   /* top of user stack; 0 for kernel threads */
    u32         cr3;              /* page-dir phys addr; 0 = kernel address space */
    u32         kstack_alloc;     /* kmalloc'd kernel-stack base to kfree on reap (0 = none) */
    u32         ustack_alloc;     /* kmalloc'd user-stack base to kfree on reap  (0 = none) */
    ThreadState state;            /* current scheduling state                */
    /* All fields default to 0 / Invalid via BSS zero-init.                  */
};

/* ── Scheduler: round-robin preemptive multitasking ──────────────────
   Slot 0   = main kernel thread   (no explicit stack)
   Slot 1–N = background threads   (4 KB kmalloc stacks)

   Lifecycle:
     1. scheduler_spawn(fn)  — create bg thread (before or after activate)
     2. scheduler_activate() — mark main thread Running, enable tick()
     3. irq0_handler calls scheduler_tick_c() every 10 ms
        tick() round-robins to the next Ready thread via switch_task()    */
class Scheduler {
public:
    static constexpr int MAX_BG_THREADS  = 6;
    static constexpr int MAX_THREADS     = 1 + MAX_BG_THREADS; /* incl. main */
    static constexpr u32 THREAD_STACK_SIZE = 4096;             /* 4 KB        */

    static Scheduler& instance();

    /* Create a kernel-mode background thread (kmalloc's a 4 KB stack). */
    Thread* create_thread(void (*entry)(void));

    /* Create a Ring-3 user-mode thread.
       Allocates separate 4 KB kernel stack (for interrupt handling) and
       4 KB user stack (for Ring-3 code).  On first run switch_task lands
       in user_thread_stub which irets to entry in Ring 3.
       cr3 != 0 gives the thread its own page directory (isolated address
       space); cr3 == 0 keeps the shared kernel address space.          */
    Thread* create_user_thread(void (*entry)(void), u32 cr3 = 0);

    /* Create a Ring-3 thread that begins at an explicit user virtual address
       with an explicit user stack pointer — used to run a program loaded into
       its own isolated address space (see vm_create_isolated). Only the kernel
       stack is allocated here; the user stack must already be mapped in cr3.  */
    Thread* create_user_thread_at(u32 entry_va, u32 user_esp, u32 cr3);

    /* Enable scheduling.  Call once, after spawning all initial threads. */
    void activate();

    /* Called from irq0_handler on every PIT tick.
       Selects the next Ready thread (round-robin) and context-switches.  */
    void tick();

    /* Mark the current thread Blocked, then yield to the next ready one.
       Blocked threads are revivable (Phase 5.7 I/O wait); they are NOT reaped. */
    void block_current();

    /* Terminate the current thread: mark it Zombie and yield. Its scheduler
       slot, kernel/user stacks and per-process VM slot are reclaimed by the
       reaper on a later tick (once we are no longer running on its stack).
       Used by sys_exit. Slot 0 (main kernel thread) is never terminated.      */
    void exit_current();

    /* Mark the current thread Blocked WITHOUT yielding (no context switch).
       Used by the CPU fault handlers to kill a faulting Ring-3 process from
       exception context; the caller then sti+hlt's until the next PIT tick
       reschedules away. Returns false if the current thread is the main
       kernel thread (slot 0), which must never be killed.                  */
    bool kill_current();

    bool active() const { return m_active; }

    /* ID of the currently running thread (slot index). Used by SYS_GET_PID. */
    u32 current_id() const {
        return (m_current >= 0 && m_current < MAX_THREADS)
             ? m_threads[m_current].id : 0;
    }

private:
    /* Reclaim every Zombie slot except the one currently running: free its
       kernel/user stacks (kfree) and its per-process VM slot (vm_release_cr3),
       then mark the slot Invalid so create_*_thread can reuse it. Called at the
       top of tick(), so a thread that just exited is reaped on the next tick
       from another thread's context — never while we are on its own stack.    */
    void reap_zombies();

    Thread m_threads[MAX_THREADS]; /* BSS zero-init → all Invalid           */
    int    m_count;                /* live thread count (≥1 after activate) */
    int    m_current;              /* index of the currently running thread */
    bool   m_active;               /* false = tick() is a no-op             */

    static Scheduler s_instance;
};

} /* namespace Kernel */

#endif /* KERNEL_SCHEDULER_HPP */
