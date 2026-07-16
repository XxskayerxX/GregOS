#ifndef KERNEL_TIMER_C_H
#define KERNEL_TIMER_C_H

/* C-visible bridge to Kernel::Timer (implemented in kernel/Timer.cpp).
   Include this from .c files; C++ files should use Timer.hpp directly. */

#ifdef __cplusplus
extern "C" {
#endif

/* Program PIT channel 0 for 100 Hz (call once, before STI) */
void timer_initialize_100hz(void);

/* Read jiffies counter (incremented 100x/s by IRQ0) */
unsigned int timer_jiffies(void);

/* Busy-wait for approximately ms milliseconds (10 ms resolution) */
void timer_delay_ms(unsigned int ms);

/* Set PC speaker frequency and open/close the gate */
void timer_speaker_on(unsigned int hz);
void timer_speaker_off(void);

/* Convenience: emit a tone at hz for duration_ms milliseconds */
void timer_beep(unsigned int hz, unsigned int duration_ms);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_TIMER_C_H */
