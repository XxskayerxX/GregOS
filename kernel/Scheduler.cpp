/* Kernel Scheduler — round-robin preemptive multitasking
   Context switching via switch_task() (arch/i386/switch_task.asm).
   Freestanding: no libc, no exceptions.

   How context switching works with the IRQ0 call chain
   ─────────────────────────────────────────────────────
   When the PIT fires, irq0_stub:
     1. CPU    : pushes EFLAGS, CS, EIP_t  (hardware interrupt frame)
     2. pusha  : saves all GP regs
     3. call irq0_handler
       → irq0_handler calls scheduler_tick_c() → tick() → switch_task()
       → switch_task saves pushf+pusha onto the CURRENT stack, writes
         ESP into old_thread.esp, loads new_thread.esp, restores context
         and returns — but into the NEW thread's call chain.
     4. The new thread's call chain eventually unwinds back through
        its own irq0_stub frame (popa + iret), resuming at its EIP.

   When we switch BACK to the old thread later, switch_task returns into
   tick() which unwinds to irq0_handler → irq0_stub → popa → iret,
   correctly restoring the thread's registers and instruction pointer.

   Initial stack layout for a freshly created thread (create_thread):
     [esp+0..31]  zeros  (popa frame: EDI,ESI,EBP,×ESP,EBX,EDX,ECX,EAX)
     [esp+32]     0x202  (popf EFLAGS: IF=1, reserved bit always 1)
     [esp+36]     entry  (switch_task's ret jumps here on first run)
     [esp+40]     exit   (if entry() ever returns)                        */

#include "../include/Kernel/Scheduler.hpp"

extern "C" void* kmalloc(unsigned int size);
extern "C" void  kfree(void* ptr);
extern "C" void  tss_set_esp0(unsigned int esp0);
extern "C" void  user_thread_stub(void);
/* Release the per-process VM pool slot backing address space `cr3` (kernel.c).
   Returns 1 if a slot was freed, 0 for the shared kernel address space.        */
extern "C" int   vm_release_cr3(unsigned int cr3);
/* Close every file descriptor owned by thread `tid` (kernel.c) — so a reused
   slot never inherits the previous process's open descriptors.                 */
extern "C" void  fd_release_all(int tid);

namespace Kernel {

/* ── Singleton ────────────────────────────────────────────────────────── */
Scheduler Scheduler::s_instance;
Scheduler& Scheduler::instance() { return s_instance; }

/* ── thread_exit_stub: called when entry() returns ─────────────────── */
static void thread_exit_stub()
{
    /* A kernel thread fell off the end of entry(): terminate it like any other.
       exit_current() marks the slot Zombie and yields; the reaper frees this
       stack and the slot on a later tick from another thread. The hlt loop is
       a safety net until the next tick switches us away for good.           */
    Scheduler::instance().exit_current();
    for (;;) __asm__ volatile("hlt");
}

/* ── activate ─────────────────────────────────────────────────────────── */
void Scheduler::activate()
{
    /* Slot 0 = main kernel thread (no explicit stack to allocate) */
    m_threads[0].id        = 0;
    m_threads[0].esp       = 0;     /* filled when first context switch saves it */
    m_threads[0].stack_top = 0;     /* kernel stack — do not free               */
    m_threads[0].state     = ThreadState::Running;

    m_current = 0;
    m_active  = true;

    /* m_count may already be > 1 if create_thread() was called first.
       Ensure it covers at least the main thread.                    */
    if (m_count < 1) m_count = 1;
}

/* ── create_thread ───────────────────────────────────────────────────── */
Thread* Scheduler::create_thread(void (*entry)(void))
{
    /* Find a free background slot (1 .. MAX_BG_THREADS) */
    int slot = -1;
    for (int i = 1; i < MAX_THREADS; ++i) {
        if (m_threads[i].state == ThreadState::Invalid) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return nullptr;  /* no slot available */

    /* Allocate the thread stack (4 KB) */
    auto* stack = static_cast<u8*>(kmalloc(THREAD_STACK_SIZE));
    if (!stack) return nullptr;    /* OOM */

    /* Build the initial stack frame that switch_task() will resume.
       We push from the TOP of the allocation downward.

       Memory layout after create_thread (low → high):
         stk[+0..31]  = 8 × zero   (popa frame)
         stk[+32]     = 0x202       (popf EFLAGS: IF=1)
         stk[+36]     = entry       (switch_task ret → entry on first run)
         stk[+40]     = exit_stub   (if entry returns)                   */
    auto* stk = reinterpret_cast<u32*>(stack + THREAD_STACK_SIZE);

    *--stk = reinterpret_cast<u32>(thread_exit_stub); /* if entry returns */
    *--stk = reinterpret_cast<u32>(entry);            /* ret target        */
    *--stk = 0x00000202u;                             /* EFLAGS: IF=1      */
    for (int i = 0; i < 8; ++i) *--stk = 0u;         /* popa frame        */

    Thread& t          = m_threads[slot];
    t.id               = static_cast<u32>(slot);
    t.esp              = reinterpret_cast<u32>(stk);
    t.stack_top        = reinterpret_cast<u32>(stack + THREAD_STACK_SIZE);
    t.kernel_stack_top = t.stack_top;   /* kernel thread: one stack only */
    t.user_stack_top   = 0;             /* no user stack                 */
    t.cr3              = 0;             /* shared kernel address space    */
    t.kstack_alloc     = reinterpret_cast<u32>(stack);  /* kfree on reap  */
    t.ustack_alloc     = 0;             /* no user stack to free          */
    t.state            = ThreadState::Ready;

    /* Extend the live count to include this slot */
    if (m_count <= slot) m_count = slot + 1;

    return &t;
}

/* ── tick: preemptive round-robin, called from IRQ0 ─────────────────── */
void Scheduler::tick()
{
    if (!m_active) return;

    /* Reclaim any thread that exited/was killed since the last tick. Safe here:
       we are on some live thread's stack, never on a zombie's (a zombie is only
       ever reaped when it is not m_current).                                    */
    reap_zombies();

    if (m_count <= 1) return;

    /* Find the next Ready (or Running) thread after the current one */
    int next = -1;
    for (int i = 1; i < m_count; ++i) {
        int idx = (m_current + i) % m_count;
        ThreadState s = m_threads[idx].state;
        if (s == ThreadState::Ready || s == ThreadState::Running) {
            next = idx;
            break;
        }
    }
    if (next < 0 || next == m_current) return;  /* nothing to switch to */

    /* Update states */
    if (m_threads[m_current].state == ThreadState::Running)
        m_threads[m_current].state = ThreadState::Ready;
    m_threads[next].state = ThreadState::Running;

    int old   = m_current;
    m_current = next;

    /* Update TSS.esp0 to the NEXT thread's kernel stack top.
       The CPU reads TSS.esp0 whenever a hardware interrupt fires while
       that thread is in Ring 3 — it must point to the top of that
       thread's dedicated kernel stack so the interrupt frame lands there.
       Kernel-only threads have kernel_stack_top = 0; we skip them because
       they never drop to Ring 3 and TSS.esp0 is irrelevant for them.    */
    if (m_threads[next].kernel_stack_top)
        tss_set_esp0(m_threads[next].kernel_stack_top);

    /* Switch to the next thread's address space BEFORE the stack switch.
       cr3 == 0 means "shared kernel address space" (main + kernel threads).
       Every process page directory maps the kernel (0-48 MB identity + the
       framebuffer) exactly like the kernel dir, so the current kernel stack,
       code, GDT/IDT/TSS all stay valid across the CR3 reload.             */
    u32 want_cr3 = m_threads[next].cr3 ? m_threads[next].cr3 : vm_kernel_cr3();
    if (want_cr3) {
        u32 cur_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cur_cr3));
        if (cur_cr3 != want_cr3)
            __asm__ volatile("mov %0, %%cr3" : : "r"(want_cr3) : "memory");
    }

    /* Perform the context switch.
       This call may not return immediately: control transfers to `next`
       and returns here only when the scheduler picks `old` again.      */
    switch_task(
        reinterpret_cast<u32>(&m_threads[old].esp),
        m_threads[next].esp
    );
    /* When we reach here, we are back in thread `old`'s context. */
}

/* ── create_user_thread ──────────────────────────────────────────────── */
Thread* Scheduler::create_user_thread(void (*entry)(void), u32 cr3)
{
    int slot = -1;
    for (int i = 1; i < MAX_THREADS; ++i) {
        if (m_threads[i].state == ThreadState::Invalid) { slot = i; break; }
    }
    if (slot < 0) return nullptr;

    auto* kstack = static_cast<u8*>(kmalloc(THREAD_STACK_SIZE));
    auto* ustack = static_cast<u8*>(kmalloc(THREAD_STACK_SIZE));
    if (!kstack || !ustack) return nullptr;

    u32 kstack_top = reinterpret_cast<u32>(kstack + THREAD_STACK_SIZE);
    u32 ustack_top = reinterpret_cast<u32>(ustack + THREAD_STACK_SIZE);

    /* Build the kernel stack from the top downward.
       The layout (low → high addresses after all pushes) is:

         [esp+0..31]  8 × 0       popa frame consumed by switch_task
         [esp+32]     0x202       popf EFLAGS (IF=1)
         [esp+36]     stub_addr   switch_task's 'ret' jumps here
         ── user_thread_stub then executes 'iret' using the frame below ──
         [esp+40]     entry       Ring-3 EIP
         [esp+44]     0x1B        Ring-3 CS  (user code, DPL=3)
         [esp+48]     0x202       Ring-3 EFLAGS (IF=1)
         [esp+52]     ustack_top  Ring-3 ESP
         [esp+56]     0x23        Ring-3 SS  (user data, DPL=3)              */

    auto* stk = reinterpret_cast<u32*>(kstack_top);

    /* Ring-3 iret frame (pushed first = highest addresses) */
    *--stk = 0x00000023u;                           /* SS_user               */
    *--stk = ustack_top;                            /* ESP_user              */
    *--stk = 0x00000202u;                           /* EFLAGS: IF=1          */
    *--stk = 0x0000001Bu;                           /* CS_user               */
    *--stk = reinterpret_cast<u32>(entry);          /* EIP_user              */

    /* switch_task resume frame */
    *--stk = reinterpret_cast<u32>(user_thread_stub); /* ret target          */
    *--stk = 0x00000202u;                             /* popf EFLAGS         */
    for (int i = 0; i < 8; ++i) *--stk = 0u;         /* popa frame          */

    Thread& t          = m_threads[slot];
    t.id               = static_cast<u32>(slot);
    t.esp              = reinterpret_cast<u32>(stk);  /* initial kernel ESP  */
    t.stack_top        = kstack_top;
    t.kernel_stack_top = kstack_top;   /* TSS.esp0 for this thread           */
    t.user_stack_top   = ustack_top;
    t.cr3              = cr3;           /* 0 = shared kernel address space    */
    t.kstack_alloc     = reinterpret_cast<u32>(kstack);  /* both stacks are   */
    t.ustack_alloc     = reinterpret_cast<u32>(ustack);  /* kernel-heap allocs */
    t.state            = ThreadState::Ready;

    if (m_count <= slot) m_count = slot + 1;
    return &t;
}

/* ── create_user_thread_at ────────────────────────────────────────────── */
Thread* Scheduler::create_user_thread_at(u32 entry_va, u32 user_esp, u32 cr3)
{
    int slot = -1;
    for (int i = 1; i < MAX_THREADS; ++i) {
        if (m_threads[i].state == ThreadState::Invalid) { slot = i; break; }
    }
    if (slot < 0) return nullptr;

    /* Only the kernel stack is allocated here (for interrupt/syscall entry).
       The user stack lives in the process's own address space (mapped in cr3)
       at user_esp — it must NOT be a kernel-heap pointer.                    */
    auto* kstack = static_cast<u8*>(kmalloc(THREAD_STACK_SIZE));
    if (!kstack) return nullptr;
    u32 kstack_top = reinterpret_cast<u32>(kstack + THREAD_STACK_SIZE);

    auto* stk = reinterpret_cast<u32*>(kstack_top);

    /* Ring-3 iret frame (see create_user_thread for the layout). */
    *--stk = 0x00000023u;                             /* SS_user               */
    *--stk = user_esp;                                /* ESP_user (user-space) */
    *--stk = 0x00000202u;                             /* EFLAGS: IF=1          */
    *--stk = 0x0000001Bu;                             /* CS_user               */
    *--stk = entry_va;                                /* EIP_user (user-space) */

    /* switch_task resume frame */
    *--stk = reinterpret_cast<u32>(user_thread_stub);
    *--stk = 0x00000202u;
    for (int i = 0; i < 8; ++i) *--stk = 0u;

    Thread& t          = m_threads[slot];
    t.id               = static_cast<u32>(slot);
    t.esp              = reinterpret_cast<u32>(stk);
    t.stack_top        = kstack_top;
    t.kernel_stack_top = kstack_top;
    t.user_stack_top   = user_esp;
    t.cr3              = cr3;
    t.kstack_alloc     = reinterpret_cast<u32>(kstack);  /* kfree on reap        */
    t.ustack_alloc     = 0;   /* user stack lives in the process's own space —   */
                              /* it is NOT a kernel-heap pointer; do not kfree it */
    t.state            = ThreadState::Ready;

    if (m_count <= slot) m_count = slot + 1;
    return &t;
}

/* ── reap_zombies ─────────────────────────────────────────────────────── */
void Scheduler::reap_zombies()
{
    for (int i = 1; i < MAX_THREADS; ++i) {
        if (i == m_current) continue;                       /* never our own stack */
        if (m_threads[i].state != ThreadState::Zombie) continue;

        Thread& t = m_threads[i];
        fd_release_all(i);                                   /* close its FDs       */
        if (t.cr3)          vm_release_cr3(t.cr3);           /* free VM pool slot   */
        if (t.kstack_alloc) kfree(reinterpret_cast<void*>(t.kstack_alloc));
        if (t.ustack_alloc) kfree(reinterpret_cast<void*>(t.ustack_alloc));

        /* Wipe the slot so create_*_thread can reuse it. */
        t.id = 0; t.esp = 0; t.stack_top = 0; t.kernel_stack_top = 0;
        t.user_stack_top = 0; t.cr3 = 0; t.kstack_alloc = 0; t.ustack_alloc = 0;
        t.state = ThreadState::Invalid;
    }
}

/* ── exit_current ─────────────────────────────────────────────────────── */
void Scheduler::exit_current()
{
    /* Terminate the running thread. Slot 0 (main) must never exit. */
    if (m_current > 0 && m_current < MAX_THREADS)
        m_threads[m_current].state = ThreadState::Zombie;
    tick();   /* yield; reaped on a later tick from another thread's context */
}

/* ── block_current ────────────────────────────────────────────────────── */
void Scheduler::block_current()
{
    /* Mark the running thread Blocked so tick() never picks it again,
       then immediately yield so another Ready thread can run.          */
    if (m_current >= 0 && m_current < MAX_THREADS)
        m_threads[m_current].state = ThreadState::Blocked;
    tick();
}

/* ── kill_current ─────────────────────────────────────────────────────── */
bool Scheduler::kill_current()
{
    /* Refuse to kill slot 0 (the main kernel thread) or when scheduling is
       off — in either case the caller should panic instead of recovering. */
    if (!m_active || m_current <= 0 || m_current >= MAX_THREADS)
        return false;
    m_threads[m_current].state = ThreadState::Zombie;   /* reaped on next tick */
    return true;   /* caller sti+hlt's; next tick() switches away for good */
}

} /* namespace Kernel */

/* ── C bridges for kernel.c ─────────────────────────────────────────── */

extern "C" void scheduler_tick_c(void)
{
    Kernel::Scheduler::instance().tick();
}

extern "C" void scheduler_spawn(void (*entry)(void))
{
    Kernel::Scheduler::instance().create_thread(entry);
}

extern "C" void scheduler_spawn_user(void (*entry)(void))
{
    Kernel::Scheduler::instance().create_user_thread(entry);
}

extern "C" void scheduler_spawn_user_vm(void (*entry)(void), unsigned int cr3)
{
    Kernel::Scheduler::instance().create_user_thread(entry, cr3);
}

extern "C" void scheduler_spawn_user_at(unsigned int entry_va,
                                        unsigned int user_esp, unsigned int cr3)
{
    Kernel::Scheduler::instance().create_user_thread_at(entry_va, user_esp, cr3);
}

extern "C" void scheduler_activate(void)
{
    Kernel::Scheduler::instance().activate();
}

/* Kill the current thread from CPU-fault context. Returns 1 if a background
   (Ring-3) thread was marked dead, 0 if it was the main thread (→ panic).  */
extern "C" int scheduler_kill_current(void)
{
    return Kernel::Scheduler::instance().kill_current() ? 1 : 0;
}

/* ID (slot index) of the currently running thread — for fault diagnostics. */
extern "C" unsigned int scheduler_current_id(void)
{
    return Kernel::Scheduler::instance().current_id();
}
