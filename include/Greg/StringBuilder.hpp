#ifndef GREG_STRINGBUILDER_HPP
#define GREG_STRINGBUILDER_HPP

/* Greg::StringBuilder — efficient incremental string construction.
   Inspired by SerenityOS AK::StringBuilder.
   Freestanding: no libc, no exceptions.

   Uses a Greg::Vector<char> as backing store (grows via kmalloc).
   build() returns a Greg::String snapshot (heap copy, null-terminated).

   Usage:
     StringBuilder sb;
     sb.append("EIP: 0x");
     sb.append_hex(regs.eip);
     sb.append('\n');
     Greg::String s = sb.build();                                   */

#include "String.hpp"
#include "Vector.hpp"
#include "Types.h"

namespace Greg {

class StringBuilder {
public:
    StringBuilder() = default;

    /* ── Append primitives ──────────────────────────────────────────── */

    StringBuilder& append(char c) {
        m_buf.append(c);
        return *this;
    }

    StringBuilder& append(const char* s) {
        if (!s) return *this;
        while (*s) m_buf.append(*s++);
        return *this;
    }

    StringBuilder& append(const String& s) {
        return append(s.characters());
    }

    /* Append a signed 32-bit integer in decimal */
    StringBuilder& append_int(i32 v) {
        if (v < 0) { append('-'); v = -v; }
        append_uint((u32)v);
        return *this;
    }

    /* Append an unsigned 32-bit integer in decimal */
    StringBuilder& append_uint(u32 v) {
        if (v == 0) { append('0'); return *this; }
        char tmp[12]; int ti = 0;
        while (v > 0) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
        for (int k = ti - 1; k >= 0; --k) append(tmp[k]);
        return *this;
    }

    /* Append an unsigned 32-bit integer in uppercase hex with 0x prefix */
    StringBuilder& append_hex(u32 v) {
        append('0'); append('x');
        for (int shift = 28; shift >= 0; shift -= 4) {
            u32 n = (v >> shift) & 0xFu;
            append((char)(n < 10 ? '0' + n : 'A' + n - 10));
        }
        return *this;
    }

    /* Append an unsigned 32-bit integer in hex, no leading zeros, no prefix */
    StringBuilder& append_hex_short(u32 v) {
        if (v == 0) { append('0'); return *this; }
        /* find highest nibble */
        int top = 28;
        while (top > 0 && ((v >> top) & 0xFu) == 0) top -= 4;
        for (int shift = top; shift >= 0; shift -= 4) {
            u32 n = (v >> shift) & 0xFu;
            append((char)(n < 10 ? '0' + n : 'A' + n - 10));
        }
        return *this;
    }

    /* Append N repetitions of a character */
    StringBuilder& append_repeated(char c, usize n) {
        for (usize i = 0; i < n; ++i) append(c);
        return *this;
    }

    /* ── Observers ──────────────────────────────────────────────────── */

    usize length() const { return m_buf.size(); }
    bool  is_empty() const { return m_buf.size() == 0; }

    /* Direct view (NOT null-terminated — use build() for a safe String) */
    const char* data() const { return m_buf.is_empty() ? "" : &m_buf[0]; }

    /* ── Build ───────────────────────────────────────────────────────── */

    /* Returns a null-terminated heap copy. */
    String build() const {
        return String(data(), length());
    }

    /* Clear the buffer for reuse */
    void clear() { m_buf.clear(); }

private:
    Vector<char> m_buf;
};

} /* namespace Greg */

#endif /* GREG_STRINGBUILDER_HPP */
