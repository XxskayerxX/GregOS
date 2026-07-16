#ifndef GREG_OPTIONAL_HPP
#define GREG_OPTIONAL_HPP

/* Greg::Optional<T> — nullable value wrapper.
   Inspired by SerenityOS AK::Optional.
   Freestanding: no libc, no exceptions.

   Design:
   - Storage is an aligned byte array (no default-construction of T).
   - Accessing value() on an empty Optional is undefined behaviour —
     always check has_value() first (same contract as std::optional).
   - value_or(default) is safe on empty.                               */

#include "Utility.hpp"

namespace Greg {

template<typename T>
class Optional {
public:
    /* ── Construction ───────────────────────────────────────────────── */
    Optional() : m_has_value(false) {}

    Optional(const T& val) : m_has_value(true) {
        new (storage()) T(val);
    }
    Optional(T&& val) : m_has_value(true) {
        new (storage()) T(move(val));
    }

    Optional(const Optional& other) : m_has_value(other.m_has_value) {
        if (m_has_value) new (storage()) T(other.value());
    }
    Optional(Optional&& other) : m_has_value(other.m_has_value) {
        if (m_has_value) {
            new (storage()) T(move(other.value_ref()));
            other.reset();
        }
    }

    ~Optional() { reset(); }

    /* ── Assignment ──────────────────────────────────────────────────── */
    Optional& operator=(const Optional& other) {
        if (this != &other) {
            reset();
            if (other.m_has_value) { new (storage()) T(other.value()); m_has_value = true; }
        }
        return *this;
    }
    Optional& operator=(Optional&& other) {
        if (this != &other) {
            reset();
            if (other.m_has_value) {
                new (storage()) T(move(other.value_ref()));
                m_has_value = true;
                other.reset();
            }
        }
        return *this;
    }
    Optional& operator=(const T& val) {
        reset();
        new (storage()) T(val);
        m_has_value = true;
        return *this;
    }

    /* ── Observers ───────────────────────────────────────────────────── */
    bool has_value() const { return m_has_value; }
    explicit operator bool() const { return m_has_value; }

    T&       value()       { return value_ref(); }
    const T& value() const { return const_cast<Optional*>(this)->value_ref(); }

    T value_or(const T& fallback) const {
        return m_has_value ? value() : fallback;
    }

    T release_value() {
        T tmp(move(value_ref()));
        reset();
        return tmp;
    }

    /* Pointer-like access (only valid when has_value()) */
    T*       operator->()       { return &value_ref(); }
    const T* operator->() const { return &const_cast<Optional*>(this)->value_ref(); }
    T&       operator*()        { return value_ref(); }
    const T& operator*()  const { return const_cast<Optional*>(this)->value_ref(); }

    /* ── Factory ─────────────────────────────────────────────────────── */
    static Optional empty() { return Optional(); }

private:
    alignas(T) unsigned char m_storage[sizeof(T)];
    bool m_has_value;

    T& value_ref() { return *reinterpret_cast<T*>(m_storage); }
    void* storage() { return static_cast<void*>(m_storage); }

    void reset() {
        if (m_has_value) {
            value_ref().~T();
            m_has_value = false;
        }
    }
};

/* Convenience factory */
template<typename T>
Optional<T> make_optional(T&& val) { return Optional<T>(forward<T>(val)); }

} /* namespace Greg */

#endif /* GREG_OPTIONAL_HPP */
