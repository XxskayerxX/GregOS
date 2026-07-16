#ifndef GREG_VECTOR_HPP
#define GREG_VECTOR_HPP

#include "New.hpp"
#include "Utility.hpp"

namespace Greg {

/* ── Greg::Vector<T> ─────────────────────────────────────────────────────
   Dynamic array template. Inspired by SerenityOS AK::Vector.

   Design decisions:
   - Non-copyable by default (copy is explicit via clone()) — avoids
     silent O(n) allocations; same reasoning as SerenityOS.
   - Move-constructible and move-assignable.
   - Uses placement new for element construction so T's destructor is
     called correctly on removal/reallocation.
   - Fully header-only (templates require it in freestanding C++11).
   - Growth factor: ×2 starting from an INITIAL_CAPACITY of 4.         */

template<typename T>
class Vector {
public:
    static constexpr usize INITIAL_CAPACITY = 4;

    /* ── Construction / destruction ─────────────────────────────────── */
    Vector() = default;

    ~Vector() {
        clear();
        kfree(m_data);
    }

    /* Non-copyable — use clone() for an explicit deep copy */
    Vector(const Vector&) = delete;
    Vector& operator=(const Vector&) = delete;

    /* Move */
    Vector(Vector&& o)
        : m_data(o.m_data), m_size(o.m_size), m_capacity(o.m_capacity)
    {
        o.m_data     = nullptr;
        o.m_size     = 0;
        o.m_capacity = 0;
    }

    Vector& operator=(Vector&& o) {
        if (this != &o) {
            clear();
            kfree(m_data);
            m_data     = o.m_data;
            m_size     = o.m_size;
            m_capacity = o.m_capacity;
            o.m_data     = nullptr;
            o.m_size     = 0;
            o.m_capacity = 0;
        }
        return *this;
    }

    /* ── Append ─────────────────────────────────────────────────────── */
    bool append(const T& value) {
        if (!ensure_capacity(m_size + 1)) return false;
        new (&m_data[m_size]) T(value);
        ++m_size;
        return true;
    }

    bool append(T&& value) {
        if (!ensure_capacity(m_size + 1)) return false;
        new (&m_data[m_size]) T(move(value));
        ++m_size;
        return true;
    }

    /* ── Prepend (O(n)) ─────────────────────────────────────────────── */
    bool prepend(T&& value) {
        if (!ensure_capacity(m_size + 1)) return false;
        if (m_size > 0) {
            new (&m_data[m_size]) T(move(m_data[m_size - 1]));
            for (usize i = m_size - 1; i > 0; --i) {
                m_data[i].~T();
                new (&m_data[i]) T(move(m_data[i - 1]));
            }
            m_data[0].~T();
        }
        new (&m_data[0]) T(move(value));
        ++m_size;
        return true;
    }

    /* ── Element access ─────────────────────────────────────────────── */
    T&       at(usize i)             { return m_data[i]; }
    const T& at(usize i)       const { return m_data[i]; }
    T&       operator[](usize i)             { return m_data[i]; }
    const T& operator[](usize i)       const { return m_data[i]; }

    T&       first()       { return m_data[0]; }
    const T& first() const { return m_data[0]; }
    T&       last()        { return m_data[m_size - 1]; }
    const T& last()  const { return m_data[m_size - 1]; }

    /* ── Iteration ──────────────────────────────────────────────────── */
    T*       begin()       { return m_data; }
    const T* begin() const { return m_data; }
    T*       end()         { return m_data + m_size; }
    const T* end()   const { return m_data + m_size; }

    /* ── Info ───────────────────────────────────────────────────────── */
    usize size()     const { return m_size; }
    bool  is_empty() const { return m_size == 0; }
    usize capacity() const { return m_capacity; }

    /* ── Removal ────────────────────────────────────────────────────── */
    void remove(usize index) {
        if (index >= m_size) return;
        m_data[index].~T();
        for (usize i = index; i < m_size - 1; ++i) {
            new (&m_data[i]) T(move(m_data[i + 1]));
            m_data[i + 1].~T();
        }
        --m_size;
    }

    /* Remove element matching a predicate (first match only) */
    template<typename Predicate>
    bool remove_first_matching(Predicate pred) {
        for (usize i = 0; i < m_size; ++i) {
            if (pred(m_data[i])) { remove(i); return true; }
        }
        return false;
    }

    T take_last() {
        T val = move(m_data[m_size - 1]);
        m_data[m_size - 1].~T();
        --m_size;
        return val;
    }

    T take_first() {
        T val = move(m_data[0]);
        remove(0);
        return val;
    }

    /* ── Clear & reserve ─────────────────────────────────────────────── */
    void clear() {
        for (usize i = 0; i < m_size; ++i)
            m_data[i].~T();
        m_size = 0;
    }

    bool reserve(usize new_capacity) {
        return ensure_capacity(new_capacity);
    }

    /* ── Search ─────────────────────────────────────────────────────── */
    template<typename Predicate>
    T* find_first(Predicate pred) {
        for (usize i = 0; i < m_size; ++i)
            if (pred(m_data[i])) return &m_data[i];
        return nullptr;
    }

    template<typename Predicate>
    const T* find_first(Predicate pred) const {
        for (usize i = 0; i < m_size; ++i)
            if (pred(m_data[i])) return &m_data[i];
        return nullptr;
    }

    bool contains_slow(const T& val) const {
        for (usize i = 0; i < m_size; ++i)
            if (m_data[i] == val) return true;
        return false;
    }

    /* Raw data pointer — for C interop / memcpy */
    T*       data()       { return m_data; }
    const T* data() const { return m_data; }

private:
    bool ensure_capacity(usize needed) {
        if (needed <= m_capacity) return true;

        usize new_cap = m_capacity ? m_capacity * 2 : INITIAL_CAPACITY;
        while (new_cap < needed) new_cap *= 2;

        T* new_data = static_cast<T*>(
            kmalloc(static_cast<unsigned int>(new_cap * sizeof(T))));
        if (!new_data) return false;

        for (usize i = 0; i < m_size; ++i) {
            new (&new_data[i]) T(move(m_data[i]));
            m_data[i].~T();
        }

        kfree(m_data);
        m_data     = new_data;
        m_capacity = new_cap;
        return true;
    }

    T*    m_data     { nullptr };
    usize m_size     { 0 };
    usize m_capacity { 0 };
};

} // namespace Greg

#endif /* GREG_VECTOR_HPP */
