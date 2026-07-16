#ifndef GREG_STRING_HPP
#define GREG_STRING_HPP

#include "New.hpp"
#include "Utility.hpp"

namespace Greg {

/* ── Greg::String ────────────────────────────────────────────────────────
   Heap-allocated, null-terminated, byte string.  Inspired by SerenityOS
   AK::String (value semantics, always owns its buffer via kmalloc).

   Key design decisions vs. SerenityOS:
   - No COW (Copy-On-Write): simpler, safer in a single-process kernel.
   - No ErrorOr<> yet: failures (OOM) silently produce empty strings.
   - Implementation lives in kernel/Greg/String.cpp.

   All constructors/methods that allocate call the internal allocate()
   helper which uses kmalloc, so the String never touches the STL heap.  */

class String {
public:
    /* ── Construction ──────────────────────────────────────────────── */
    String() = default;
    String(const char* cstr);
    String(const char* cstr, usize length);
    String(const String& other);
    String(String&& other);
    ~String();

    /* ── Assignment ─────────────────────────────────────────────────── */
    String& operator=(const String& other);
    String& operator=(String&& other);
    String& operator=(const char* cstr);

    /* ── Accessors ──────────────────────────────────────────────────── */
    /* SerenityOS convention: characters() instead of c_str() */
    const char* characters() const { return m_data ? m_data : ""; }
    usize       length()     const { return m_length; }
    bool        is_empty()   const { return m_length == 0; }
    bool        is_null()    const { return m_data == nullptr; }

    char operator[](usize i) const { return m_data[i]; }

    /* ── Comparison ─────────────────────────────────────────────────── */
    bool operator==(const String& other) const;
    bool operator==(const char* cstr)   const;
    bool operator!=(const String& other) const { return !(*this == other); }
    bool operator!=(const char* cstr)   const  { return !(*this == cstr); }

    /* ── Mutation ───────────────────────────────────────────────────── */
    bool append(char c);
    bool append(const char* cstr);
    bool append(const String& other);
    bool prepend(const char* cstr);

    /* ── Substring / search ─────────────────────────────────────────── */
    String substring(usize start, usize count) const;
    bool   contains(char c) const;
    bool   starts_with(const char* prefix) const;
    bool   ends_with(const char* suffix)   const;

    /* ── Conversion ─────────────────────────────────────────────────── */
    static String from_int(int value);
    static String from_uint(unsigned int value);
    int          to_int(bool* ok = nullptr) const;
    unsigned int to_uint(bool* ok = nullptr) const;

    /* ── Iteration helpers ──────────────────────────────────────────── */
    const char* begin() const { return m_data; }
    const char* end()   const { return m_data + m_length; }

    /* ── Concatenation operator ─────────────────────────────────────── */
    String operator+(const String& other) const;
    String operator+(const char* cstr)   const;
    String& operator+=(const String& other) { append(other); return *this; }
    String& operator+=(const char* cstr)   { append(cstr); return *this; }

private:
    /* Allocate and copy at most `len` bytes from `src` + null terminator */
    bool allocate(const char* src, usize len);
    void deallocate();

    /* Freestanding strlen — never use <string.h> directly */
    static usize cstrlen(const char* s);
    /* Freestanding memcpy */
    static void* cstrcpy(char* dst, const char* src, usize n);

    char*  m_data   { nullptr };
    usize  m_length { 0 };
};

} // namespace Greg

#endif /* GREG_STRING_HPP */
