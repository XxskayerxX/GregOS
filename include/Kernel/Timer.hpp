#ifndef KERNEL_TIMER_HPP
#define KERNEL_TIMER_HPP

#include "../Greg/Types.h"

namespace Kernel {

using Greg::u32;

/* ── Timer ───────────────────────────────────────────────────────────────
   Encapsulation of the 8253/8254 Programmable Interval Timer (PIT).

   Channel 0  →  IRQ0  →  jiffies counter  (scheduling heartbeat)
   Channel 2  →  gate-controlled PC speaker tone generation

   Static-only class (no instances): wraps direct port I/O with a
   meaningful API. Implementation goes in kernel/Timer.cpp (Phase 2).

   Port map:
     0x40  Channel 0 data port
     0x41  Channel 1 (unused — DRAM refresh, legacy)
     0x42  Channel 2 data port  (speaker frequency)
     0x43  Mode/command register
     0x61  System control port B  (speaker gate + NMI bits)         */

class Timer {
public:
    /* PIT oscillator frequency (hardware constant) */
    static constexpr u32 PIT_BASE_HZ   = 1193182;

    /* Scheduler heartbeat — 100 IRQ0 ticks per second */
    static constexpr u32 SCHEDULER_HZ  = 100;

    /* ── Channel 0 (IRQ0 / scheduler) ──────────────────────────────── */

    /* Program channel 0 for the desired tick rate.
       Called once during kernel init, before IDT is loaded.           */
    static void initialize(u32 hz = SCHEDULER_HZ);

    /* Read the current jiffies counter.
       Atomic on 32-bit x86 if compiler generates a single MOV.       */
    static u32 jiffies();

    /* Convert a jiffies delta to wall-clock milliseconds */
    static u32 elapsed_ms(u32 since_jiffies) {
        return (jiffies() - since_jiffies) * 1000u / SCHEDULER_HZ;
    }

    /* Busy-wait for approximately `ms` milliseconds (10 ms resolution) */
    static void delay_ms(u32 ms);

    /* ── Channel 2 (PC speaker) ─────────────────────────────────────── */

    /* Set speaker frequency (Hz) via PIT channel 2 */
    static void speaker_set_freq(u32 hz);

    /* Open / close the speaker gate on port 0x61 */
    static void speaker_on();
    static void speaker_off();

    /* Convenience: beep at `hz` for `duration_ms` milliseconds */
    static void beep(u32 hz, u32 duration_ms);

private:
    Timer()  = delete;   /* utility class — no instances */
    ~Timer() = delete;
};

} // namespace Kernel

#endif /* KERNEL_TIMER_HPP */
