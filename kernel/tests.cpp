/* Phase 2 — Greg:: Foundation validation tests
   Outputs to framebuffer (GFX) or VGA depending on mode.
   All tests are self-contained; no game/GUI code is touched.           */

#include "../include/Greg/Greg.h"
#include "../include/kernel_tests.h"
#include "../include/gfx.h"
#include "../include/vga.h"

extern "C" void term_print_int(int n);
extern "C" volatile unsigned long jiffies;

/* ── Output helpers ───────────────────────────────────────────────────── */

namespace {

static int  s_pass  = 0;
static int  s_fail  = 0;
static int  s_gfx_y = 20;   /* current Y position in GFX mode */

static const unsigned int COL_BG    = 0xFF0A0A1A;
static const unsigned int COL_HEAD  = 0xFF00FFFF;
static const unsigned int COL_OK    = 0xFF00FF88;
static const unsigned int COL_FAIL  = 0xFFFF4444;
static const unsigned int COL_LABEL = 0xFFCCCCCC;

static void tprint(const char* s, unsigned int gfx_col)
{
    if (gfx_active()) {
        gfx_draw_str(8, s_gfx_y, s, gfx_col, COL_BG);
        s_gfx_y += 10;
    } else {
        term_print(s);
        term_print("\n");
    }
}

static void tprint_int(int n, unsigned int gfx_col)
{
    if (gfx_active()) {
        /* convert inline — we can't assume term_print_int works in GFX branch */
        char buf[16];
        int i = 0;
        unsigned int uval;
        if (n < 0) { buf[i++] = '-'; uval = static_cast<unsigned int>(-(n + 1)) + 1u; }
        else        { uval = static_cast<unsigned int>(n); }
        if (uval == 0) { buf[i++] = '0'; }
        else {
            char tmp[12]; int k = 0;
            while (uval) { tmp[k++] = static_cast<char>('0' + uval % 10); uval /= 10; }
            while (k--) buf[i++] = tmp[k+1];
        }
        buf[i] = '\0';
        gfx_draw_str(8, s_gfx_y, buf, gfx_col, COL_BG);
        s_gfx_y += 10;
    } else {
        term_print_int(n);
        term_print("\n");
    }
}

static void tlog_header(const char* title)
{
    if (gfx_active()) {
        s_gfx_y += 4;
        gfx_fill_rect(4, s_gfx_y, 400, 2, COL_HEAD);
        s_gfx_y += 6;
    }
    tprint(title, COL_HEAD);
}

static void check(bool cond, const char* name)
{
    if (cond) {
        ++s_pass;
        if (gfx_active()) {
            char line[80];
            /* "[PASS] " + name */
            int i = 0;
            const char* pfx = "[PASS] ";
            while (pfx[i]) { line[i] = pfx[i]; ++i; }
            int j = 0;
            while (name[j] && i < 78) { line[i++] = name[j++]; }
            line[i] = '\0';
            gfx_draw_str(8, s_gfx_y, line, COL_OK, COL_BG);
            s_gfx_y += 10;
        } else {
            term_print("[PASS] "); term_print(name); term_print("\n");
        }
    } else {
        ++s_fail;
        if (gfx_active()) {
            char line[80];
            int i = 0;
            const char* pfx = "[FAIL] ";
            while (pfx[i]) { line[i] = pfx[i]; ++i; }
            int j = 0;
            while (name[j] && i < 78) { line[i++] = name[j++]; }
            line[i] = '\0';
            gfx_draw_str(8, s_gfx_y, line, COL_FAIL, COL_BG);
            s_gfx_y += 10;
        } else {
            term_print("[FAIL] "); term_print(name); term_print("\n");
        }
    }
}

/* ── Dummy structs for ctor/dtor tracking ─────────────────────────────── */

static int s_alive = 0;

struct Dummy {
    int value;
    explicit Dummy(int v = 0) : value(v) { ++s_alive; }
    Dummy(const Dummy& o) : value(o.value) { ++s_alive; }
    Dummy(Dummy&& o) : value(o.value) { ++s_alive; }
    ~Dummy() { --s_alive; }
};

struct RefDummy : public Greg::RefCounted<RefDummy> {
    int value;
    explicit RefDummy(int v = 0) : value(v) { ++s_alive; }
    ~RefDummy() { --s_alive; }
};

/* ── test_vector ──────────────────────────────────────────────────────── */

static void test_vector()
{
    tlog_header("=== Vector<int> ===");

    {
        Greg::Vector<int> v;
        check(v.is_empty(), "empty on construction");
        check(v.size() == 0, "size == 0");

        v.append(10);
        check(!v.is_empty(), "not empty after append");
        check(v.size() == 1, "size == 1");
        check(v[0] == 10, "v[0] == 10");

        v.append(20);
        v.append(30);
        check(v.size() == 3, "size == 3 after 3 appends");
        check(v[2] == 30, "v[2] == 30");
    }

    {
        /* Force realloc: append past INITIAL_CAPACITY (4) */
        Greg::Vector<int> v;
        for (int i = 0; i < 20; ++i)
            v.append(i);
        check(v.size() == 20, "size == 20 after 20 appends (multiple reallocs)");
        bool all_ok = true;
        for (int i = 0; i < 20; ++i)
            if (v[i] != i) { all_ok = false; break; }
        check(all_ok, "all 20 values correct after realloc");
    }

    {
        Greg::Vector<int> v;
        v.append(1); v.append(2); v.append(3);
        v.remove(1);   /* remove v[1] == 2 */
        check(v.size() == 2, "size == 2 after remove");
        check(v[0] == 1 && v[1] == 3, "elements shifted correctly");
    }

    {
        /* Move semantics */
        Greg::Vector<int> a;
        a.append(42);
        Greg::Vector<int> b(Greg::move(a));
        check(b.size() == 1 && b[0] == 42, "move ctor transfers elements");
        check(a.is_empty(), "source empty after move");
    }
}

/* ── test_vector_dtor ─────────────────────────────────────────────────── */

static void test_vector_dtor()
{
    tlog_header("=== Vector<Dummy> dtor ===");

    s_alive = 0;
    {
        Greg::Vector<Dummy> v;
        for (int i = 0; i < 8; ++i)
            v.append(Dummy(i));      /* forces at least one realloc at i==4 */
        check(s_alive == 8, "8 Dummies alive inside vector");
        /* remove one */
        v.remove(0);
        check(s_alive == 7, "7 Dummies after remove(0)");
    } /* vector destructor calls ~Dummy() for remaining 7 */
    check(s_alive == 0, "all Dummies destroyed on vector dtor");
}

/* ── test_string ──────────────────────────────────────────────────────── */

static void test_string()
{
    tlog_header("=== String ===");

    {
        Greg::String s;
        check(s.is_null(), "default ctor: is_null");
        check(s.is_empty(), "default ctor: is_empty");
        check(s.length() == 0, "default ctor: length == 0");
    }

    {
        Greg::String s("hello");
        check(s.length() == 5, "cstr ctor: length == 5");
        check(!s.is_null(), "cstr ctor: not null");
        check(s == "hello", "cstr ctor: == \"hello\"");
    }

    {
        /* Deep copy */
        Greg::String a("world");
        Greg::String b(a);
        check(b == "world", "copy ctor: equal content");
        b.append("!");
        check(a == "world", "copy ctor: deep — original unmodified");
        check(b == "world!", "copy ctor: modified copy correct");
    }

    {
        /* Move */
        Greg::String a("moved");
        Greg::String b(Greg::move(a));
        check(b == "moved", "move ctor: content transferred");
        check(a.is_null(), "move ctor: source nulled");
    }

    {
        /* operator= copy */
        Greg::String a("foo");
        Greg::String b;
        b = a;
        check(b == "foo", "copy assign: correct");
        b = "bar";
        check(a == "foo", "copy assign: deep — original unmodified");
    }

    {
        /* append / concatenation */
        Greg::String s("abc");
        s.append('d');
        check(s == "abcd", "append(char): abcd");
        s.append("ef");
        check(s == "abcdef", "append(cstr): abcdef");
        Greg::String tail("gh");
        s.append(tail);
        check(s == "abcdefgh", "append(String): abcdefgh");
        check(s.length() == 8, "length == 8");
    }

    {
        /* substring */
        Greg::String s("GregOS");
        Greg::String sub = s.substring(4, 2);
        check(sub == "OS", "substring(4,2) == \"OS\"");
        Greg::String sub2 = s.substring(0, 4);
        check(sub2 == "Greg", "substring(0,4) == \"Greg\"");
    }

    {
        /* contains / starts_with / ends_with */
        Greg::String s("kernel");
        check(s.contains('r'), "contains('r')");
        check(!s.contains('z'), "!contains('z')");
        check(s.starts_with("ker"), "starts_with(\"ker\")");
        check(!s.starts_with("nel"), "!starts_with(\"nel\")");
        check(s.ends_with("nel"), "ends_with(\"nel\")");
        check(!s.ends_with("ker"), "!ends_with(\"ker\")");
    }

    {
        /* from_int / to_int */
        Greg::String s = Greg::String::from_int(-42);
        check(s == "-42", "from_int(-42)");
        Greg::String s2 = Greg::String::from_int(0);
        check(s2 == "0", "from_int(0)");
        Greg::String s3 = Greg::String::from_uint(1234);
        check(s3 == "1234", "from_uint(1234)");

        bool ok;
        Greg::String n("123");
        check(n.to_int(&ok) == 123 && ok, "to_int(\"123\") == 123");
        Greg::String neg("-7");
        check(neg.to_int(&ok) == -7 && ok, "to_int(\"-7\") == -7");
        Greg::String bad("abc");
        bad.to_int(&ok);
        check(!ok, "to_int(\"abc\") sets ok=false");
    }
}

/* ── test_ownptr ──────────────────────────────────────────────────────── */

static void test_ownptr()
{
    tlog_header("=== OwnPtr<Dummy> ===");

    s_alive = 0;

    {
        Greg::OwnPtr<Dummy> p;
        check(p.is_null(), "default OwnPtr: is_null");
    }

    {
        Greg::OwnPtr<Dummy> p = Greg::make_own<Dummy>(99);
        check(!p.is_null(), "make_own: not null");
        check(p->value == 99, "make_own: value == 99");
        check(s_alive == 1, "make_own: 1 Dummy alive");
    }
    check(s_alive == 0, "OwnPtr scope exit: Dummy destroyed");

    {
        /* Move */
        Greg::OwnPtr<Dummy> a = Greg::make_own<Dummy>(7);
        Greg::OwnPtr<Dummy> b = Greg::move(a);
        check(a.is_null(), "move: source null");
        check(!b.is_null() && b->value == 7, "move: dest owns object");
        check(s_alive == 1, "move: still 1 alive");
    }
    check(s_alive == 0, "OwnPtr move scope exit: destroyed");

    {
        /* leak_ptr */
        Greg::OwnPtr<Dummy> p = Greg::make_own<Dummy>(3);
        Dummy* raw = p.leak_ptr();
        check(p.is_null(), "leak_ptr: OwnPtr now null");
        check(raw != nullptr && raw->value == 3, "leak_ptr: raw pointer valid");
        check(s_alive == 1, "leak_ptr: Dummy still alive");
        delete raw;   /* manual cleanup since we leaked */
    }
    check(s_alive == 0, "after manual delete: alive == 0");

    {
        /* clear() */
        Greg::OwnPtr<Dummy> p = Greg::make_own<Dummy>(5);
        p.clear();
        check(p.is_null(), "clear: OwnPtr null");
        check(s_alive == 0, "clear: Dummy destroyed");
    }
}

/* ── test_refptr ──────────────────────────────────────────────────────── */

static void test_refptr()
{
    tlog_header("=== RefPtr<RefDummy> ===");

    s_alive = 0;

    {
        Greg::RefPtr<RefDummy> p;
        check(p.is_null(), "default RefPtr: is_null");
    }

    {
        Greg::RefPtr<RefDummy> p = Greg::make_ref<RefDummy>(10);
        check(!p.is_null(), "make_ref: not null");
        check(p->value == 10, "make_ref: value == 10");
        check(p->ref_count() == 1, "make_ref: ref_count == 1");
        check(s_alive == 1, "make_ref: 1 RefDummy alive");

        {
            Greg::RefPtr<RefDummy> q = p;   /* copy → ref++ */
            check(p->ref_count() == 2, "copy: ref_count == 2");
            {
                Greg::RefPtr<RefDummy> r = q; /* another copy */
                check(p->ref_count() == 3, "second copy: ref_count == 3");
                check(s_alive == 1, "copies: still 1 object alive");
            } /* r destroyed → ref-- */
            check(p->ref_count() == 2, "r out of scope: ref_count == 2");
            check(s_alive == 1, "r out of scope: still alive");
        } /* q destroyed → ref-- */
        check(p->ref_count() == 1, "q out of scope: ref_count == 1");
        check(s_alive == 1, "q out of scope: still alive");
    } /* p destroyed → ref_count → 0 → delete */
    check(s_alive == 0, "last RefPtr out of scope: object destroyed");

    {
        /* Move: ref_count stays at 1 */
        Greg::RefPtr<RefDummy> a = Greg::make_ref<RefDummy>(20);
        Greg::RefPtr<RefDummy> b = Greg::move(a);
        check(a.is_null(), "move: source null");
        check(!b.is_null() && b->value == 20, "move: dest valid");
        check(b->ref_count() == 1, "move: ref_count unchanged at 1");
    }
    check(s_alive == 0, "RefPtr move scope exit: destroyed");
}

/* ── test_span ────────────────────────────────────────────────────────── */

static void test_span()
{
    tlog_header("=== Span<int> ===");

    int arr[6] = { 5, 10, 15, 20, 25, 30 };
    Greg::Span<int> s(arr);   /* deduces N=6 */
    check(s.size() == 6, "size == 6 from array ctor");
    check(!s.is_empty(), "not empty");
    check(s[0] == 5 && s.back() == 30, "front/back correct");

    Greg::Span<int> mid = s.subspan(2, 3);   /* {15,20,25} */
    check(mid.size() == 3, "subspan size == 3");
    check(mid[0] == 15 && mid[2] == 25, "subspan values correct");

    Greg::Span<int> clamped = s.subspan(4, 10);   /* clamps to 2 */
    check(clamped.size() == 2, "subspan count clamped to bounds");

    Greg::Span<int> f = s.first(2);
    Greg::Span<int> l = s.last(2);
    check(f[0] == 5 && f[1] == 10, "first(2) correct");
    check(l[0] == 25 && l[1] == 30, "last(2) correct");

    check(s.index_of(20) == 3, "index_of finds element");
    check(s.contains(15) && !s.contains(99), "contains works");

    int sum = 0;
    for (int v : s) sum += v;   /* range-for */
    check(sum == 105, "range-for sums all elements");

    Greg::Span<int> empty;
    check(empty.is_empty() && empty.size() == 0, "default Span is empty");
}

/* ── test_circular_buffer ─────────────────────────────────────────────── */

static void test_circular_buffer()
{
    tlog_header("=== CircularBuffer<int,4> ===");

    Greg::CircularBuffer<int, 4> cb;   /* usable capacity = 3 */
    check(cb.is_empty(), "empty on construction");
    check(cb.capacity() == 3, "capacity == Capacity-1");

    check(cb.enqueue(1), "enqueue 1 ok");
    check(cb.enqueue(2), "enqueue 2 ok");
    check(cb.enqueue(3), "enqueue 3 ok");
    check(cb.is_full(), "full after 3 enqueues");
    check(!cb.enqueue(4), "enqueue on full is rejected");
    check(cb.size() == 3, "size == 3");

    int peeked = 0;
    check(cb.peek(peeked) && peeked == 1, "peek returns head without removing");
    check(cb.size() == 3, "peek did not consume");

    int out = 0;
    check(cb.dequeue(out) && out == 1, "dequeue FIFO order 1");
    check(cb.dequeue(out) && out == 2, "dequeue FIFO order 2");
    check(cb.enqueue(4), "enqueue after dequeue (wrap) ok");
    check(cb.dequeue(out) && out == 3, "dequeue 3");
    check(cb.dequeue(out) && out == 4, "dequeue wrapped element 4");
    check(cb.is_empty(), "empty after draining");
    check(!cb.dequeue(out), "dequeue on empty is rejected");

    /* clear() resets */
    cb.enqueue(7); cb.enqueue(8);
    cb.clear();
    check(cb.is_empty(), "clear empties the buffer");
}

} // anonymous namespace

/* ── Public entry point ───────────────────────────────────────────────── */

extern "C" void run_foundation_tests(void)
{
    s_pass = 0;
    s_fail = 0;
    s_gfx_y = 20;

    if (gfx_active())
        gfx_fill_rect(0, 0, 500, 600, COL_BG);

    tlog_header("Greg:: Foundation Tests — Phase 2");

    test_vector();
    test_vector_dtor();
    test_string();
    test_ownptr();
    test_refptr();
    test_span();
    test_circular_buffer();

    /* Summary */
    if (gfx_active()) {
        s_gfx_y += 6;
        gfx_fill_rect(4, s_gfx_y, 400, 2, COL_LABEL);
        s_gfx_y += 6;
    }

    tprint("Results:", COL_LABEL);
    tprint("  PASS: ", COL_OK);   tprint_int(s_pass, COL_OK);
    tprint("  FAIL: ", COL_FAIL); tprint_int(s_fail, COL_FAIL);

    if (s_fail == 0) {
        tprint("ALL TESTS PASSED — Foundation is solid.", COL_OK);
    } else {
        tprint("FAILURES DETECTED — fix before migrating kernel.", COL_FAIL);
        /* Busy-wait ~5 s so user can read results before GUI takes over */
        if (gfx_active()) {
            volatile unsigned long start = jiffies;
            while (jiffies - start < 500UL) { /* spin — 100 Hz × 5 s */ }
        }
    }
}
