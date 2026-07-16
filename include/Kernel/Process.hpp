#ifndef KERNEL_PROCESS_HPP
#define KERNEL_PROCESS_HPP

#include "../Greg/Types.h"
#include "../Greg/String.hpp"
#include "../Greg/Vector.hpp"
#include "../Greg/OwnPtr.hpp"
#include "../Greg/RefCounted.hpp"
#include "Thread.hpp"

namespace Kernel {

using Greg::u8;
using Greg::u32;

/* ── Process state ───────────────────────────────────────────────────── */
enum class ProcessState : u8 {
    Invalid = 0,
    Running,   /* has at least one Running thread              */
    Runnable,  /* all threads Ready or Blocked                 */
    Dying,     /* exit() called, threads being torn down       */
    Dead,      /* fully cleaned up, waiting for parent wait()  */
};

/* Strongly-typed PID */
struct ProcessID {
    u32 value { 0 };
    bool operator==(ProcessID o) const { return value == o.value; }
    bool operator!=(ProcessID o) const { return value != o.value; }
    bool is_kernel()             const { return value == 0; }
};

/* ── Process ─────────────────────────────────────────────────────────────
   A process owns one or more threads and an address-space description.
   In Phase 1, GregOS is single-process (the kernel IS the only process).
   In Phase 3+ each spawned task will be a Process with its own Thread.

   Inherits RefCounted so it can be held by RefPtr<Process>.            */
class Process : public Greg::RefCounted<Process> {
public:
    static constexpr u32 KERNEL_PID = 0;

    Process(Greg::String name, ProcessID pid);

    /* ── Identity ─────────────────────────────────────────────────── */
    ProcessID           pid()  const { return m_pid; }
    const Greg::String& name() const { return m_name; }

    /* ── State ──────────────────────────────────────────────────────── */
    ProcessState state()             const { return m_state; }
    void set_state(ProcessState s)         { m_state = s; }

    /* ── Exit code ──────────────────────────────────────────────────── */
    int  exit_code()    const { return m_exit_code; }
    void set_exit_code(int c) { m_exit_code = c; }

    /* ── Scheduling stats ───────────────────────────────────────────── */
    u32  ticks_total() const { return m_ticks_total; }
    void tick()              { ++m_ticks_total; }

    /* ── Thread registry (Phase 3+) ─────────────────────────────────── */
    bool add_thread(Greg::OwnPtr<Thread> t) {
        return m_threads.append(Greg::move(t));
    }

    Greg::Vector<Greg::OwnPtr<Thread>>& threads() { return m_threads; }

    Thread* main_thread() {
        return m_threads.is_empty() ? nullptr : m_threads[0].ptr();
    }

private:
    Greg::String                       m_name;
    ProcessID                          m_pid;
    ProcessState                       m_state       { ProcessState::Runnable };
    int                                m_exit_code   { 0 };
    u32                                m_ticks_total { 0 };
    Greg::Vector<Greg::OwnPtr<Thread>> m_threads;
};

} // namespace Kernel

#endif /* KERNEL_PROCESS_HPP */
