/* Greg::String — heap-allocated byte string (kmalloc-backed, no STL) */

#include "../../include/Greg/String.hpp"

extern "C" void* kmalloc(unsigned int size);
extern "C" void  kfree(void* ptr);

namespace Greg {

/* ── Private helpers ──────────────────────────────────────────────────── */

usize String::cstrlen(const char* s)
{
    if (!s) return 0;
    usize n = 0;
    while (s[n]) ++n;
    return n;
}

void* String::cstrcpy(char* dst, const char* src, usize n)
{
    for (usize i = 0; i < n; ++i)
        dst[i] = src[i];
    return dst;
}

bool String::allocate(const char* src, usize len)
{
    char* buf = static_cast<char*>(kmalloc(static_cast<unsigned int>(len + 1)));
    if (!buf) return false;
    if (src && len)
        cstrcpy(buf, src, len);
    buf[len] = '\0';
    m_data   = buf;
    m_length = len;
    return true;
}

void String::deallocate()
{
    if (m_data) {
        kfree(m_data);
        m_data   = nullptr;
        m_length = 0;
    }
}

/* ── Constructors / destructor ────────────────────────────────────────── */

String::String(const char* cstr)
{
    if (cstr)
        allocate(cstr, cstrlen(cstr));
}

String::String(const char* cstr, usize length)
{
    if (cstr && length)
        allocate(cstr, length);
}

String::String(const String& other)
{
    if (!other.is_null())
        allocate(other.m_data, other.m_length);
}

String::String(String&& other)
    : m_data(other.m_data)
    , m_length(other.m_length)
{
    other.m_data   = nullptr;
    other.m_length = 0;
}

String::~String()
{
    deallocate();
}

/* ── Assignment ───────────────────────────────────────────────────────── */

String& String::operator=(const String& other)
{
    if (this == &other) return *this;
    deallocate();
    if (!other.is_null())
        allocate(other.m_data, other.m_length);
    return *this;
}

String& String::operator=(String&& other)
{
    if (this == &other) return *this;
    deallocate();
    m_data         = other.m_data;
    m_length       = other.m_length;
    other.m_data   = nullptr;
    other.m_length = 0;
    return *this;
}

String& String::operator=(const char* cstr)
{
    deallocate();
    if (cstr)
        allocate(cstr, cstrlen(cstr));
    return *this;
}

/* ── Comparison ───────────────────────────────────────────────────────── */

bool String::operator==(const String& other) const
{
    if (m_length != other.m_length) return false;
    for (usize i = 0; i < m_length; ++i)
        if (m_data[i] != other.m_data[i]) return false;
    return true;
}

bool String::operator==(const char* cstr) const
{
    if (!cstr) return is_null();
    usize len = cstrlen(cstr);
    if (len != m_length) return false;
    for (usize i = 0; i < m_length; ++i)
        if (m_data[i] != cstr[i]) return false;
    return true;
}

/* ── Mutation ─────────────────────────────────────────────────────────── */

bool String::append(char c)
{
    usize new_len = m_length + 1;
    char* buf = static_cast<char*>(kmalloc(static_cast<unsigned int>(new_len + 1)));
    if (!buf) return false;
    if (m_data)
        cstrcpy(buf, m_data, m_length);
    buf[m_length] = c;
    buf[new_len]  = '\0';
    deallocate();
    m_data   = buf;
    m_length = new_len;
    return true;
}

bool String::append(const char* cstr)
{
    if (!cstr || !cstr[0]) return true;
    usize rhs_len = cstrlen(cstr);
    usize new_len = m_length + rhs_len;
    char* buf = static_cast<char*>(kmalloc(static_cast<unsigned int>(new_len + 1)));
    if (!buf) return false;
    if (m_data)
        cstrcpy(buf, m_data, m_length);
    cstrcpy(buf + m_length, cstr, rhs_len);
    buf[new_len] = '\0';
    deallocate();
    m_data   = buf;
    m_length = new_len;
    return true;
}

bool String::append(const String& other)
{
    return append(other.m_data);
}

bool String::prepend(const char* cstr)
{
    if (!cstr || !cstr[0]) return true;
    usize lhs_len = cstrlen(cstr);
    usize new_len = lhs_len + m_length;
    char* buf = static_cast<char*>(kmalloc(static_cast<unsigned int>(new_len + 1)));
    if (!buf) return false;
    cstrcpy(buf, cstr, lhs_len);
    if (m_data)
        cstrcpy(buf + lhs_len, m_data, m_length);
    buf[new_len] = '\0';
    deallocate();
    m_data   = buf;
    m_length = new_len;
    return true;
}

/* ── Substring / search ───────────────────────────────────────────────── */

String String::substring(usize start, usize count) const
{
    if (!m_data || start >= m_length) return String{};
    usize actual = count;
    if (start + actual > m_length)
        actual = m_length - start;
    return String(m_data + start, actual);
}

bool String::contains(char c) const
{
    for (usize i = 0; i < m_length; ++i)
        if (m_data[i] == c) return true;
    return false;
}

bool String::starts_with(const char* prefix) const
{
    if (!prefix) return true;
    usize plen = cstrlen(prefix);
    if (plen > m_length) return false;
    for (usize i = 0; i < plen; ++i)
        if (m_data[i] != prefix[i]) return false;
    return true;
}

bool String::ends_with(const char* suffix) const
{
    if (!suffix) return true;
    usize slen = cstrlen(suffix);
    if (slen > m_length) return false;
    usize off = m_length - slen;
    for (usize i = 0; i < slen; ++i)
        if (m_data[off + i] != suffix[i]) return false;
    return true;
}

/* ── Conversion ───────────────────────────────────────────────────────── */

/* Converts a non-negative integer n into decimal digits at buf[0..].
   buf must have room for at least 12 chars (10 digits + sign + null).
   Returns number of characters written (excluding null terminator).    */
static usize uint_to_digits(unsigned int n, char* buf)
{
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    char tmp[12];
    usize i = 0;
    while (n) { tmp[i++] = static_cast<char>('0' + (n % 10)); n /= 10; }
    /* reverse */
    for (usize j = 0; j < i; ++j)
        buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
    return i;
}

String String::from_int(int value)
{
    char buf[14];
    usize pos = 0;
    unsigned int uval;
    if (value < 0) {
        buf[pos++] = '-';
        uval = static_cast<unsigned int>(-(value + 1)) + 1u;
    } else {
        uval = static_cast<unsigned int>(value);
    }
    pos += uint_to_digits(uval, buf + pos);
    return String(buf, pos);
}

String String::from_uint(unsigned int value)
{
    char buf[12];
    usize len = uint_to_digits(value, buf);
    return String(buf, len);
}

int String::to_int(bool* ok) const
{
    if (!m_data || m_length == 0) { if (ok) *ok = false; return 0; }
    usize i = 0;
    bool neg = false;
    if (m_data[0] == '-') { neg = true; ++i; }
    if (i >= m_length) { if (ok) *ok = false; return 0; }
    int result = 0;
    for (; i < m_length; ++i) {
        char ch = m_data[i];
        if (ch < '0' || ch > '9') { if (ok) *ok = false; return 0; }
        result = result * 10 + (ch - '0');
    }
    if (ok) *ok = true;
    return neg ? -result : result;
}

unsigned int String::to_uint(bool* ok) const
{
    if (!m_data || m_length == 0) { if (ok) *ok = false; return 0; }
    unsigned int result = 0;
    for (usize i = 0; i < m_length; ++i) {
        char ch = m_data[i];
        if (ch < '0' || ch > '9') { if (ok) *ok = false; return 0; }
        result = result * 10 + static_cast<unsigned int>(ch - '0');
    }
    if (ok) *ok = true;
    return result;
}

/* ── Concatenation ────────────────────────────────────────────────────── */

String String::operator+(const String& other) const
{
    String result(*this);
    result.append(other);
    return result;
}

String String::operator+(const char* cstr) const
{
    String result(*this);
    result.append(cstr);
    return result;
}

} // namespace Greg
