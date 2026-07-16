#ifndef KERNEL_THREAD_HPP
#define KERNEL_THREAD_HPP

#include "../Greg/Types.h"
#include "../Greg/String.hpp"

namespace Kernel {

/* Import Greg primitive types into the Kernel namespace for readability */
using Greg::u8;
using Greg::u32;
using Greg::usize;

/* ── Thread state machine ────────────────────────────────────────────────
   Invalid ──► Ready ──► Running ──► Blocked
                  ▲         │           │
                  └─────────┘           │
                                        ▼
                                      Ready
   Any state ──► Dying ──► Dead                                          */
enum class ThreadState : u8 {
    Invalid = 0,
    Ready,       /* in run queue, waiting for CPU time       */
    Running,     /* currently on-CPU                         */
    Blocked,     /* waiting for I/O, sleep, or mutex         */
    Dying,       /* requested termination, not yet cleaned   */
    Dead,        /* stack freed, joinable or reaped          */
};

/* Strongly-typed ID — avoids mixing thread IDs with process IDs */
struct ThreadID {
    u32 value { 0 };
    bool operator==(ThreadID o) const { return value == o.value; }
    bool operator!=(ThreadID o) const { return value != o.value; }
};

/* ── Thread ──────────────────────────────────────────────────────────────
   Represents a schedulable unit of execution.
   In Phase 1 the stack lives in a static k_heap allocation.
   In Phase 3+ we will map a proper kernel stack per thread.

   The scheduler saves/restores m_saved_esp on every context switch.     */
class Thread {
public:
    static constexpr usize STACK_SIZE = 8192;  /* 8 KB kernel stack */

    Thread() = default;
    Thread(Greg::String name, ThreadID tid, u32 entry_point);

    /* ── Identity ─────────────────────────────────────────────────── */
    ThreadID            tid()   const { return m_tid; }
    const Greg::String& name()  const { return m_name; }

    /* ── Scheduling ────────────────────────────────────────────────── */
    ThreadState state()               const { return m_state; }
    void        set_state(ThreadState s)    { m_state = s; }

    u32  saved_esp()          const { return m_saved_esp; }
    void set_saved_esp(u32 esp)     { m_saved_esp = esp; }

    /* ── Accounting ─────────────────────────────────────────────────── */
    u32  ticks_used() const { return m_ticks_used; }
    void tick()             { ++m_ticks_used; }

    /* ── Stack ──────────────────────────────────────────────────────── */
    u8*  stack_base() const { return m_stack; }
    u8*  stack_top()  const { return m_stack + STACK_SIZE; }
    bool has_stack()  const { return m_stack != nullptr; }
    void set_stack(u8* s)   { m_stack = s; }

    bool is_runnable() const {
        return m_state == ThreadState::Ready
            || m_state == ThreadState::Running;
    }

private:
    Greg::String m_name;
    ThreadID     m_tid;
    ThreadState  m_state      { ThreadState::Invalid };
    u32          m_saved_esp  { 0 };
    u32          m_ticks_used { 0 };
    u8*          m_stack      { nullptr };  /* kmalloc'd by scheduler */
};

} // namespace Kernel

#endif /* KERNEL_THREAD_HPP */
