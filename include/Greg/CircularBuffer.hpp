#ifndef GREG_CIRCULAR_BUFFER_HPP
#define GREG_CIRCULAR_BUFFER_HPP

/* Greg::CircularBuffer<T, Capacity> — fixed-size ring buffer.
   Inspired by SerenityOS AK::CircularQueue; sized for the EventQueue use case.
   Freestanding: no libc, no exceptions.

   ── Concurrency ──
   Designed for the classic kernel pattern: ONE producer (an ISR pushing
   events) and ONE consumer (the main loop draining them). On x86 a single
   naturally-aligned 32-bit load/store is atomic, and because the producer
   only ever writes m_tail while the consumer only ever writes m_head, no
   lock is required for a single-producer / single-consumer pair. Both
   indices are `volatile` so the compiler cannot cache them across the
   ISR/main-loop boundary.

   Do NOT use with multiple producers or multiple consumers without an
   external lock — the lock-free guarantee is SPSC only.

   ── Capacity ──
   One slot is intentionally left unused so full and empty are
   distinguishable without a shared count variable (which both sides would
   have to write). Usable capacity is therefore Capacity - 1. */

#include "Types.h"

namespace Greg {

template<typename T, usize Capacity>
class CircularBuffer {
public:
    CircularBuffer() : m_head(0), m_tail(0) {}

    /* Producer side ── enqueue one element.
       Returns false (dropping the element) when the buffer is full.   */
    bool enqueue(const T& value) {
        usize tail = m_tail;
        usize next = advance(tail);
        if (next == m_head) return false;   /* full — would collide with head */
        m_storage[tail] = value;
        m_tail = next;                      /* publish AFTER the slot is written */
        return true;
    }

    /* Consumer side ── dequeue one element into `out`.
       Returns false when the buffer is empty.                          */
    bool dequeue(T& out) {
        usize head = m_head;
        if (head == m_tail) return false;   /* empty */
        out = m_storage[head];
        m_head = advance(head);             /* release the slot AFTER the read */
        return true;
    }

    /* Non-destructive look at the next element to be dequeued. */
    bool peek(T& out) const {
        usize head = m_head;
        if (head == m_tail) return false;
        out = m_storage[head];
        return true;
    }

    /* ── Observers ──────────────────────────────────────────────────── */
    bool is_empty() const { return m_head == m_tail; }
    bool is_full()  const { return advance(m_tail) == m_head; }

    /* Live element count. Safe to call from either side; may momentarily
       read a value one off the truth if the other side mutates during the
       call — acceptable for diagnostics / high-water tracking.          */
    usize size() const {
        usize h = m_head, t = m_tail;
        return (t + Capacity - h) % Capacity;
    }

    static constexpr usize capacity() { return Capacity - 1; }

    void clear() { m_head = m_tail; }   /* consumer-side reset */

private:
    static usize advance(usize i) { return (i + 1) % Capacity; }

    T                m_storage[Capacity];
    volatile usize   m_head;   /* written by consumer only */
    volatile usize   m_tail;   /* written by producer only */
};

} /* namespace Greg */

#endif /* GREG_CIRCULAR_BUFFER_HPP */
