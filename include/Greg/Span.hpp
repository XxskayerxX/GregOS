#ifndef GREG_SPAN_HPP
#define GREG_SPAN_HPP

/* Greg::Span<T> — non-owning view over a contiguous range of T.
   Inspired by SerenityOS AK::Span / C++20 std::span.
   Freestanding: no libc, no exceptions.

   A Span never owns its storage — it is just a {pointer, length} pair.
   The referenced buffer must outlive the Span. Use it to pass slices of
   arrays / Vectors around without copying and without decaying to a raw
   pointer that loses its length.

   Bounds: operator[] and front()/back() are unchecked (same contract as
   std::span). Use subspan()/first()/last(), which clamp, for safe slicing. */

#include "Types.h"

namespace Greg {

template<typename T>
class Span {
public:
    /* ── Construction ───────────────────────────────────────────────── */
    Span() : m_data(nullptr), m_size(0) {}
    Span(T* data, usize size) : m_data(data), m_size(data ? size : 0) {}

    /* Build a Span over a fixed-size C array. */
    template<usize N>
    Span(T (&arr)[N]) : m_data(arr), m_size(N) {}

    /* ── Observers ──────────────────────────────────────────────────── */
    T*       data()        { return m_data; }
    const T* data()  const { return m_data; }
    usize    size()  const { return m_size; }
    bool     is_empty() const { return m_size == 0; }

    T&       operator[](usize i)       { return m_data[i]; }
    const T& operator[](usize i) const { return m_data[i]; }

    T&       front()       { return m_data[0]; }
    const T& front() const { return m_data[0]; }
    T&       back()        { return m_data[m_size - 1]; }
    const T& back()  const { return m_data[m_size - 1]; }

    /* Range-based-for support */
    T*       begin()       { return m_data; }
    T*       end()         { return m_data + m_size; }
    const T* begin() const { return m_data; }
    const T* end()   const { return m_data + m_size; }

    /* ── Safe slicing (all clamp to the current bounds) ─────────────── */
    Span subspan(usize offset, usize count) const {
        if (offset >= m_size) return Span();
        usize avail = m_size - offset;
        if (count > avail) count = avail;
        return Span(m_data + offset, count);
    }
    Span first(usize count) const { return subspan(0, count); }
    Span last(usize count) const {
        if (count > m_size) count = m_size;
        return Span(m_data + (m_size - count), count);
    }

    /* Linear search — returns index or (usize)-1 if not present. */
    usize index_of(const T& needle) const {
        for (usize i = 0; i < m_size; ++i)
            if (m_data[i] == needle) return i;
        return static_cast<usize>(-1);
    }
    bool contains(const T& needle) const {
        return index_of(needle) != static_cast<usize>(-1);
    }

private:
    T*    m_data;
    usize m_size;
};

} /* namespace Greg */

#endif /* GREG_SPAN_HPP */
