#ifndef EVENT_HPP
#define EVENT_HPP

#include "event.h"   /* Event typedef + extern "C" wrappers */

/* ── EventQueue: interrupt-safe single-producer/single-consumer ring buffer
   Capacity 256: volatile unsigned char indices wrap naturally at 256.
   ISR pushes (head_, producer); main loop pops (tail_, consumer).
   On x86, aligned byte reads/writes are atomic — no explicit lock needed. */

class EventQueue {
public:
    bool push(const Event& e);
    bool pop(Event& out);
    bool empty() const { return head_ == tail_; }

    static EventQueue& instance();

private:
    static constexpr int CAPACITY = 256;

    Event                  buf_[CAPACITY];
    volatile unsigned char head_;  /* written by ISR (producer) */
    volatile unsigned char tail_;  /* written by main loop (consumer) */
    /* zero-init from BSS — no constructor needed */
};

#endif /* EVENT_HPP */
