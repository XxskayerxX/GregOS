#ifndef GREG_ERROROR_HPP
#define GREG_ERROROR_HPP

/* Greg::ErrorOr<T> — Rust-style Result type.
   Inspired by SerenityOS AK::ErrorOr.
   Freestanding: no libc, no exceptions.

   Usage:
     ErrorOr<int> safe_div(int a, int b) {
         if (b == 0) return ErrorOr<int>::error("division by zero");
         return ErrorOr<int>::ok(a / b);
     }

     auto result = safe_div(10, 2);
     if (result.is_error()) { ... result.error_string() ... }
     else                   { use(result.value()); }

   TRY() macro — early-return on error (analogous to Rust's ? operator):
     #define TRY(expr) ({ auto _r = (expr); if (_r.is_error()) return _r.propagate(); _r.release_value(); })

   Error representation: a simple const char* literal (no heap alloc).
   For richer errors, replace with Greg::String in a future phase.    */

#include "Utility.hpp"
#include "Optional.hpp"

namespace Greg {

/* ── Error type ─────────────────────────────────────────────────────── */

struct Error {
    const char* message;                /* static string — no heap alloc */

    explicit Error(const char* msg) : message(msg ? msg : "unknown error") {}

    bool operator==(const Error& other) const {
        /* Pointer equality — sufficient for compile-time literals */
        return message == other.message;
    }
};

/* ── ErrorOr<T> ──────────────────────────────────────────────────────── */

template<typename T>
class ErrorOr {
public:
    /* ── Success path ────────────────────────────────────────────────── */
    static ErrorOr ok(const T& val) {
        ErrorOr e;
        e.m_is_error = false;
        new (e.value_storage()) T(val);
        return e;
    }
    static ErrorOr ok(T&& val) {
        ErrorOr e;
        e.m_is_error = false;
        new (e.value_storage()) T(move(val));
        return e;
    }

    /* ── Error path ──────────────────────────────────────────────────── */
    static ErrorOr error(const char* msg) {
        ErrorOr e;
        e.m_is_error = true;
        e.m_error    = Error(msg);
        return e;
    }
    static ErrorOr from_error(Error err) {
        ErrorOr e;
        e.m_is_error = true;
        e.m_error    = err;
        return e;
    }

    /* ── Copy / move ─────────────────────────────────────────────────── */
    ErrorOr(const ErrorOr& other) : m_is_error(other.m_is_error), m_error(other.m_error) {
        if (!m_is_error) new (value_storage()) T(other.value());
    }
    ErrorOr(ErrorOr&& other) : m_is_error(other.m_is_error), m_error(other.m_error) {
        if (!m_is_error) {
            new (value_storage()) T(move(other.value_ref()));
            other.m_is_error = true;          /* mark moved-from as error */
        }
    }
    ~ErrorOr() {
        if (!m_is_error) value_ref().~T();
    }

    /* ── Observers ───────────────────────────────────────────────────── */
    bool is_error()   const { return m_is_error; }
    bool is_ok()      const { return !m_is_error; }

    /* Only call when is_ok() */
    T&       value()       { return value_ref(); }
    const T& value() const { return const_cast<ErrorOr*>(this)->value_ref(); }

    T release_value() {
        T tmp(move(value_ref()));
        value_ref().~T();
        m_is_error = true;
        return tmp;
    }

    /* Only call when is_error() */
    Error       error_value() const { return m_error; }
    const char* error_string() const { return m_error.message; }

    /* Propagate error to a caller with a different value type */
    template<typename U>
    ErrorOr<U> propagate() const {
        return ErrorOr<U>::from_error(m_error);
    }

    /* Convert to Optional — discards error information */
    Optional<T> to_optional() const {
        if (m_is_error) return Optional<T>::empty();
        return Optional<T>(value());
    }

private:
    ErrorOr() : m_is_error(true), m_error("") {}

    alignas(T) unsigned char m_value_storage[sizeof(T)];
    bool  m_is_error { true };
    Error m_error    { "" };

    T&    value_ref()    { return *reinterpret_cast<T*>(m_value_storage); }
    void* value_storage(){ return static_cast<void*>(m_value_storage);    }
};

/* ── Specialisation for void (success/failure with no value) ───────── */

template<>
class ErrorOr<void> {
public:
    static ErrorOr ok()                   { return ErrorOr(false, ""); }
    static ErrorOr error(const char* msg) { return ErrorOr(true,  msg ? msg : "error"); }

    bool        is_error()     const { return m_is_error; }
    bool        is_ok()        const { return !m_is_error; }
    Error       error_value()  const { return m_error; }
    const char* error_string() const { return m_error.message; }

    template<typename U>
    ErrorOr<U> propagate() const { return ErrorOr<U>::from_error(m_error); }

private:
    ErrorOr(bool is_err, const char* msg) : m_is_error(is_err), m_error(msg) {}
    bool  m_is_error;
    Error m_error;
};

/* ── TRY macro ───────────────────────────────────────────────────────── */
/* Usage (inside a function returning ErrorOr<U>):
     int val = TRY(some_function_returning_erroror_int());
   If the expression is an error it propagates to the caller.
   Requires a GCC/Clang statement-expression extension (__extension__). */
#define TRY(expr)                                                  \
    __extension__({                                                \
        auto _try_result = (expr);                                 \
        if (_try_result.is_error())                                \
            return _try_result.propagate<decltype(({0;}))>();      \
        _try_result.release_value();                               \
    })

} /* namespace Greg */

#endif /* GREG_ERROROR_HPP */
