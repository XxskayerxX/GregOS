/* kernel/Timer.cpp — Kernel::Timer implementation
   Encapsulates the 8253/8254 PIT and PC speaker.
   Freestanding: no libc, no exceptions.

   Port map:
     0x40  PIT channel 0 data    (IRQ0 / jiffies)
     0x42  PIT channel 2 data    (speaker frequency)
     0x43  PIT mode/command register
     0x61  System control port B (bits 0-1: speaker gate + channel-2 gate) */

#include "../include/Kernel/Timer.hpp"
#include "../include/ports.h"

/* jiffies is owned by kernel.c — we read it here as an extern.
   Declared volatile so the compiler re-reads it on every access.     */
extern "C" volatile unsigned long jiffies;

namespace Kernel {

/* ── Channel 0: IRQ0 / scheduler heartbeat ─────────────────────────── */

void Timer::initialize(u32 hz)
{
    u32 divisor = PIT_BASE_HZ / hz;
    port_byte_out(0x43, 0x36);                          /* ch0, lobyte/hibyte, square wave */
    port_byte_out(0x40, (unsigned char)(divisor & 0xFF));
    port_byte_out(0x40, (unsigned char)(divisor >> 8));
}

u32 Timer::jiffies()
{
    return (u32)::jiffies;
}

void Timer::delay_ms(u32 ms)
{
    /* 10 ms resolution (one PIT tick = 10 ms at 100 Hz).
       Uses HLT to yield the CPU between ticks instead of spinning. */
    u32 ticks = (ms + 9) / 10;          /* round up to nearest tick */
    u32 target = jiffies() + ticks;
    while (jiffies() < target)
        __asm__ volatile("hlt");
}

/* ── Channel 2: PC speaker ──────────────────────────────────────────── */

void Timer::speaker_set_freq(u32 hz)
{
    if (hz == 0) { speaker_off(); return; }
    u32 divisor = PIT_BASE_HZ / hz;
    port_byte_out(0x43, 0xB6);                          /* ch2, lobyte/hibyte, square wave */
    port_byte_out(0x42, (unsigned char)(divisor & 0xFF));
    port_byte_out(0x42, (unsigned char)(divisor >> 8));
}

void Timer::speaker_on()
{
    port_byte_out(0x61, port_byte_in(0x61) | 0x03u);   /* bits 0+1: ch2-gate + speaker */
}

void Timer::speaker_off()
{
    port_byte_out(0x61, port_byte_in(0x61) & ~0x03u);
}

void Timer::beep(u32 hz, u32 duration_ms)
{
    speaker_set_freq(hz);
    speaker_on();
    delay_ms(duration_ms);
    speaker_off();
}

} /* namespace Kernel */

/* ── C bridges ──────────────────────────────────────────────────────────── */

extern "C" void timer_initialize_100hz(void)
{
    Kernel::Timer::initialize(100);
}

extern "C" unsigned int timer_jiffies(void)
{
    return Kernel::Timer::jiffies();
}

extern "C" void timer_delay_ms(unsigned int ms)
{
    Kernel::Timer::delay_ms(ms);
}

extern "C" void timer_speaker_on(unsigned int hz)
{
    Kernel::Timer::speaker_set_freq(hz);
    Kernel::Timer::speaker_on();
}

extern "C" void timer_speaker_off(void)
{
    Kernel::Timer::speaker_off();
}

extern "C" void timer_beep(unsigned int hz, unsigned int duration_ms)
{
    Kernel::Timer::beep(hz, duration_ms);
}
