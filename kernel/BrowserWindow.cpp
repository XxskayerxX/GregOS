/* BrowserWindow — "GregNet" native web browser for GregOS.
   Minimal HTTP/1.0 client + HTML layout engine, rendered as a WM window.
   Freestanding: no libc, no exceptions, no RTTI.

   Networking: include/net.h (polled RTL8139 stack). HTTPS unsupported.     */

#include "../include/BrowserWindow.hpp"
#include "../include/WindowManager.hpp"
#include "../include/Graphics.hpp"
#include "../include/Theme.hpp"
#include "../include/gfx.h"
#include "../include/keyboard.h"

/* Network API — declared here (matching include/net.h exactly) rather than
   #included: net.h has a nested block-comment-open sequence in its banner
   that trips -Wcomment under -Wall. We only use these two entry points.   */
extern "C" int net_ready(void);
extern "C" int http_get(const char* url, char** out_body, int* out_len,
                        int max_len, char* final_url, char* content_type);
extern "C" int https_get(const char* url, char** out_body, int* out_len,
                         int max_len, char* final_url, char* content_type);
extern "C" const char* tls_cert_error(void);   /* reason for a -3 cert failure */

extern "C" volatile unsigned long jiffies;
extern "C" void  kfree(void* p);
extern "C" void* kmalloc(unsigned int size);
#include "../include/png_dec.h"

/* ── Palette ─────────────────────────────────────────────────────────────── */
/* Page rendue comme un parchemin (vélin) — assorti au thème des documents. */
static const unsigned int TEXT_COLOR    = Theme::VELLUM_INK;   /* encre sur vélin  */
static const unsigned int LINK_COLOR    = Theme::TEAL_ARCANE;  /* liens GregNet    */
static const unsigned int HEADING_COLOR = Theme::BLOOD;        /* titres rubriqués */
static const unsigned int DIM_COLOR     = Theme::GOLD_DIM;     /* texte discret    */
static const unsigned int HR_COLOR      = Theme::GOLD_DIM;     /* filets           */
static const unsigned int WHITE         = Theme::VELLUM;       /* page = parchemin */

/* ── Frag flags ──────────────────────────────────────────────────────────── */
static const unsigned char FL_BOLD = 0x01;
static const unsigned char FL_UL   = 0x02;
static const unsigned char FL_HR   = 0x04;
static const unsigned char FL_IMG  = 0x08;

static const int IMG_FETCH_MAX = 262144;   /* 256 KB per image download */

static const int HTTP_MAX_LEN = 131072;   /* 128 KB body cap */

/* ── Tiny freestanding string helpers ────────────────────────────────────── */

static int  bw_strlen(const char* s) { int n = 0; while (s[n]) ++n; return n; }
static char bw_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static bool bw_isalnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}
static bool bw_isalpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool bw_streq(const char* a, const char* b)
{
    int i = 0; while (a[i] && b[i]) { if (a[i] != b[i]) return false; ++i; }
    return a[i] == b[i];
}
static bool bw_starts(const char* s, const char* pre)
{
    int i = 0; while (pre[i]) { if (s[i] != pre[i]) return false; ++i; }
    return true;
}
static int bw_strcpy(char* dst, const char* src, int cap)
{
    int i = 0; while (src[i] && i < cap - 1) { dst[i] = src[i]; ++i; }
    dst[i] = '\0'; return i;
}
static int bw_strcat(char* dst, const char* src, int cap)
{
    int i = bw_strlen(dst), j = 0;
    while (src[j] && i < cap - 1) { dst[i++] = src[j++]; }
    dst[i] = '\0'; return i;
}
/* case-insensitive compare of a[0..len) against literal b of exactly len chars */
static bool bw_ci_eq(const char* a, int len, const char* b)
{
    for (int i = 0; i < len; ++i) {
        if (b[i] == '\0') return false;
        if (bw_lower(a[i]) != bw_lower(b[i])) return false;
    }
    return b[len] == '\0';
}

/* status-line builders */
static int s_cat(char* d, int p, int cap, const char* s)
{
    while (*s && p < cap - 1) d[p++] = *s++;
    d[p] = '\0'; return p;
}
static int s_cat_int(char* d, int p, int cap, int v)
{
    if (v < 0) { if (p < cap - 1) d[p++] = '-'; v = -v; }
    char t[16]; int ti = 0;
    if (v == 0) t[ti++] = '0';
    while (v > 0) { t[ti++] = (char)('0' + v % 10); v /= 10; }
    while (ti > 0 && p < cap - 1) d[p++] = t[--ti];
    d[p] = '\0'; return p;
}

/* Does the URL start with a "scheme:" (absolute)? */
static bool bw_has_scheme(const char* s)
{
    if (!bw_isalpha(s[0])) return false;
    int i = 0;
    while (s[i] && s[i] != ':' && s[i] != '/' && s[i] != '?' && s[i] != '#') ++i;
    return s[i] == ':';
}

/* Find "</name" (case-insensitive) from pos; returns index of '<' or -1. */
static int bw_find_close(const char* h, int n, int pos, const char* name)
{
    int ln = bw_strlen(name);
    for (int i = pos; i + 1 < n; ++i) {
        if (h[i] == '<' && h[i + 1] == '/') {
            int k = i + 2, m = 0;
            while (m < ln && k < n && bw_lower(h[k]) == name[m]) { ++k; ++m; }
            if (m == ln) return i;
        }
    }
    return -1;
}

/* Extract attribute `name` value from a tag's attribute string. */
static bool bw_get_attr(const char* a, int alen, const char* name,
                        char* out, int outsz)
{
    int nl = bw_strlen(name);
    int i = 0;
    while (i < alen) {
        while (i < alen && (a[i] == ' ' || a[i] == '\t' || a[i] == '\n' ||
                            a[i] == '\r' || a[i] == '/')) ++i;
        int ns = i;
        while (i < alen && a[i] != '=' && a[i] != ' ' && a[i] != '\t' &&
               a[i] != '\n' && a[i] != '\r' && a[i] != '/') ++i;
        int nlen = i - ns;
        while (i < alen && (a[i] == ' ' || a[i] == '\t')) ++i;
        bool hasval = false;
        int vs = i, vl = 0;
        if (i < alen && a[i] == '=') {
            hasval = true; ++i;
            while (i < alen && (a[i] == ' ' || a[i] == '\t')) ++i;
            if (i < alen && (a[i] == '"' || a[i] == '\'')) {
                char q = a[i]; ++i; vs = i;
                while (i < alen && a[i] != q) ++i;
                vl = i - vs; if (i < alen) ++i;
            } else {
                vs = i;
                while (i < alen && a[i] != ' ' && a[i] != '\t' &&
                       a[i] != '\n' && a[i] != '\r') ++i;
                vl = i - vs;
            }
        }
        if (nlen == nl && bw_ci_eq(a + ns, nlen, name)) {
            int k = 0;
            if (hasval) for (; k < vl && k < outsz - 1; ++k) out[k] = a[vs + k];
            out[k] = '\0';
            return true;
        }
        if (!hasval && i <= ns) ++i;   /* guard against stalls */
    }
    return false;
}

/* Decode one HTML entity starting at h[i]=='&'. Writes decoded chars to out,
   sets *outlen, returns the index just past the entity. */
static int bw_decode_entity(const char* h, int n, int i, char* out, int* outlen)
{
    int j = i + 1;
    if (j < n && h[j] == '#') {
        ++j; int base = 10;
        if (j < n && (h[j] == 'x' || h[j] == 'X')) { base = 16; ++j; }
        int val = 0, digits = 0;
        while (j < n && digits < 7) {
            char d = h[j]; int dv;
            if (d >= '0' && d <= '9') dv = d - '0';
            else if (base == 16 && d >= 'a' && d <= 'f') dv = d - 'a' + 10;
            else if (base == 16 && d >= 'A' && d <= 'F') dv = d - 'A' + 10;
            else break;
            val = val * base + dv; ++j; ++digits;
        }
        if (j < n && h[j] == ';') ++j;
        int ol = 0;
        if (digits == 0) out[ol++] = '&';
        else if (val < 32) out[ol++] = ' ';
        else if (val < 256) out[ol++] = (char)val;
        else out[ol++] = '?';
        out[ol] = '\0'; *outlen = ol; return j;
    }
    int ns = j, cnt = 0;
    while (j < n && cnt < 10 && bw_isalnum(h[j])) { ++j; ++cnt; }
    if (j < n && h[j] == ';') {
        int nl = j - ns; ++j;
        const char* nm = h + ns; int ol = 0;
        if      (bw_ci_eq(nm, nl, "amp"))  out[ol++] = '&';
        else if (bw_ci_eq(nm, nl, "lt"))   out[ol++] = '<';
        else if (bw_ci_eq(nm, nl, "gt"))   out[ol++] = '>';
        else if (bw_ci_eq(nm, nl, "quot")) out[ol++] = '"';
        else if (bw_ci_eq(nm, nl, "apos")) out[ol++] = '\'';
        else if (bw_ci_eq(nm, nl, "nbsp")) out[ol++] = ' ';
        else if (bw_ci_eq(nm, nl, "copy")) { out[ol++] = '('; out[ol++] = 'c'; out[ol++] = ')'; }
        else {
            out[ol++] = '&';
            for (int k = 0; k < nl && ol < 13; ++k) out[ol++] = nm[k];
            if (ol < 14) out[ol++] = ';';
        }
        out[ol] = '\0'; *outlen = ol; return j;
    }
    out[0] = '&'; out[1] = '\0'; *outlen = 1; return i + 1;
}

/* Normalize a path: collapse ".", resolve "..", drop empty segments. */
static void bw_normalize_path(const char* p, char* out, int outsz)
{
    int outlen = 0; out[outlen++] = '/';
    int i = (p[0] == '/') ? 1 : 0;
    bool trailing = false;
    while (p[i]) {
        int s = i;
        while (p[i] && p[i] != '/') ++i;
        int seglen = i - s;
        bool slash = (p[i] == '/');
        if (p[i] == '/') ++i;
        if (seglen == 0) { continue; }
        if (seglen == 1 && p[s] == '.') { trailing = slash; continue; }
        if (seglen == 2 && p[s] == '.' && p[s + 1] == '.') {
            if (outlen > 1) {
                --outlen;                              /* step over trailing '/' */
                while (outlen > 1 && out[outlen - 1] != '/') --outlen;
            }
            trailing = slash; continue;
        }
        for (int k = 0; k < seglen && outlen < outsz - 2; ++k) out[outlen++] = p[s + k];
        if (outlen < outsz - 1) out[outlen++] = '/';
        trailing = slash;
    }
    if (!trailing && outlen > 1 && out[outlen - 1] == '/') --outlen;
    out[outlen] = '\0';
}

/* Resolve `rel` against absolute `base` → out (RFC-3986-ish subset). */
static void bw_url_join(const char* base, const char* rel, char* out, int outsz)
{
    /* strip fragment from rel */
    char r[BrowserWindow::LINK_MAX]; int ri = 0;
    for (int k = 0; rel[k] && rel[k] != '#' && ri < (int)sizeof(r) - 1; ++k) r[ri++] = rel[k];
    r[ri] = '\0';

    if (r[0] == '\0') { bw_strcpy(out, base, outsz); return; }

    if (bw_has_scheme(r)) { bw_strcpy(out, r, outsz); return; }

    /* parse base → scheme, host, path */
    char scheme[16]; char host[256]; char path[BrowserWindow::URL_MAX];
    scheme[0] = host[0] = 0; path[0] = '/'; path[1] = '\0';
    int bi = 0;
    while (base[bi] && base[bi] != ':' && bi < 15) { scheme[bi] = base[bi]; ++bi; }
    scheme[bi] = '\0';
    const char* after = base;
    if (base[bi] == ':' && base[bi + 1] == '/' && base[bi + 2] == '/') after = base + bi + 3;
    else after = base;   /* malformed base; treat rest as host */
    int hi = 0;
    while (after[hi] && after[hi] != '/' && hi < 255) { host[hi] = after[hi]; ++hi; }
    host[hi] = '\0';
    if (after[hi] == '/') bw_strcpy(path, after + hi, sizeof(path));

    if (r[0] == '/' && r[1] == '/') {
        /* protocol-relative */
        int p = s_cat(out, 0, outsz, scheme);
        p = s_cat(out, p, outsz, ":");
        s_cat(out, p, outsz, r);
        return;
    }
    if (r[0] == '/') {
        char norm[BrowserWindow::URL_MAX]; bw_normalize_path(r, norm, sizeof(norm));
        int p = s_cat(out, 0, outsz, scheme);
        p = s_cat(out, p, outsz, "://");
        p = s_cat(out, p, outsz, host);
        s_cat(out, p, outsz, norm);
        return;
    }
    /* relative: take base directory + rel */
    char dir[BrowserWindow::URL_MAX]; int di = 0, lastslash = 0;
    for (int k = 0; path[k] && di < (int)sizeof(dir) - 1; ++k) {
        dir[di++] = path[k];
        if (path[k] == '/') lastslash = di;
    }
    dir[lastslash] = '\0';                 /* keep up to last '/' */
    char comb[BrowserWindow::URL_MAX];
    int cp = bw_strcpy(comb, dir, sizeof(comb));
    if (cp == 0) { comb[0] = '/'; comb[1] = '\0'; }
    bw_strcat(comb, r, sizeof(comb));
    char norm[BrowserWindow::URL_MAX]; bw_normalize_path(comb, norm, sizeof(norm));
    int p = s_cat(out, 0, outsz, scheme);
    p = s_cat(out, p, outsz, "://");
    p = s_cat(out, p, outsz, host);
    s_cat(out, p, outsz, norm);
}

/* ── Internal pages ──────────────────────────────────────────────────────── */

static const char* bw_home_html()
{
    return
    "<title>GregNet</title>"
    "<h1>GregNet</h1>"
    "<p>Bienvenue sur GregNet, le navigateur natif de GregOS.</p>"
    "<pre>"
    "   ==(W{==========-\n"
    "     ||  (.--.)\n"
    "     | \\,|**|,__\n"
    "  ___/-==|  /`\\_.\n"
    " (^(~     `-' -~`\n"
    "</pre>"
    "<h2>Liens rapides</h2>"
    "<ul>"
    "<li><a href=\"https://example.com\">https://example.com - TLS x25519</a></li>"
    "<li><a href=\"https://www.php.net\">https://www.php.net - TLS P-256 (CDN)</a></li>"
    "<li><a href=\"https://www.debian.org\">https://www.debian.org - TLS + gros HTML</a></li>"
    "<li><a href=\"https://en.wikipedia.org\">https://en.wikipedia.org - TLS</a></li>"
    "<li><a href=\"http://httpforever.com\">httpforever.com - HTTP chunked</a></li>"
    "<li><a href=\"greg://about\">A propos de GregNet</a></li>"
    "</ul>"
    "<h2>Images PNG (decodeur maison)</h2>"
    "<ul>"
    "<li><a href=\"http://httpbin.org/image/png\">httpbin.org/image/png - PNG direct en HTTP</a></li>"
    "<li><a href=\"https://www.wikipedia.org/portal/wikipedia.org/assets/img/Wikipedia-logo-v2.png\">Logo Wikipedia - PNG direct en HTTPS</a></li>"
    "</ul>"
    "<h2>Securite - ces liens DOIVENT etre refuses</h2>"
    "<ul>"
    "<li><a href=\"https://expired.badssl.com\">expired.badssl.com - certificat expire</a></li>"
    "<li><a href=\"https://wrong.host.badssl.com\">wrong.host.badssl.com - mauvais nom d'hote</a></li>"
    "<li><a href=\"https://self-signed.badssl.com\">self-signed.badssl.com - auto-signe</a></li>"
    "<li><a href=\"https://untrusted-root.badssl.com\">untrusted-root.badssl.com - racine inconnue</a></li>"
    "</ul>"
    "<hr>"
    "<p>Tapez une adresse http:// ou https:// dans la barre puis Entree. "
    "HTTPS via TLS 1.2 (ECDHE x25519/P-256, AES-128-GCM) avec verification "
    "complete du certificat : chaine validee jusqu'a une racine de confiance "
    "embarquee, nom d'hote, dates, et signature du ServerKeyExchange.</p>";
}

static const char* bw_about_html()
{
    return
    "<title>A propos</title>"
    "<h1>GregNet - A propos</h1>"
    "<p>GregNet v1.0 - navigateur web pour GregOS.</p>"
    "<h3>Fonctions</h3>"
    "<ul>"
    "<li>Moteur de rendu HTML minimal (blocs, listes, liens, gras)</li>"
    "<li>Client HTTP/1.0 via la pile reseau rtl8139 (pas de TLS)</li>"
    "<li>Historique precedent / suivant, defilement clavier et souris</li>"
    "<li>Pages internes greg://home et greg://about</li>"
    "</ul>"
    "<blockquote>Le web, mais en beaucoup plus petit.</blockquote>"
    "<p><a href=\"greg://home\">Retour a l'accueil</a></p>";
}

/* ═══════════════════════════════════════════════════════════════════════════
   Layout engine
   ═══════════════════════════════════════════════════════════════════════════ */

int BrowserWindow::render_h() const
{
    int h = client_h() - TOOLBAR_H - STATUS_H;
    return h > 0 ? h : 0;
}
int BrowserWindow::content_w() const
{
    int w = client_w() - SB_W - 2 * PAD;
    return w >= CW * 4 ? w : CW * 4;
}
int BrowserWindow::content_top() const
{
    return client_y() + TOOLBAR_H + PAD;
}
int BrowserWindow::max_scroll() const
{
    int total = m_content_h + 2 * PAD;
    int m = total - render_h();
    return m > 0 ? m : 0;
}

void BrowserWindow::reset_layout()
{
    m_text_len = 0; m_frag_count = 0; m_link_count = 0; m_content_h = 0;
    m_pen_x = 0; m_pen_y = 0; m_indent = 0;
    m_frag_x = 0; m_frag_start = 0;
    m_at_line_start = true; m_any_content = false; m_gap_done = false;
    m_pre = false; m_in_head = false; m_in_title = false;
    m_bold_depth = 0; m_dim_depth = 0; m_list_depth = 0; m_quote_depth = 0;
    m_heading = 0; m_cur_link = -1; m_word_len = 0;
    m_title[0] = '\0';
    recompute_indent();
    recompute_pen();
}

void BrowserWindow::recompute_indent()
{
    m_indent = m_list_depth * LIST_IND + m_quote_depth * QUOTE_IND;
    if (m_at_line_start) { m_pen_x = m_indent; m_frag_x = m_pen_x; }
}

void BrowserWindow::recompute_pen()
{
    m_pen_link = (short)m_cur_link;
    unsigned char f = 0;
    if (m_cur_link >= 0) f |= FL_UL;
    if (m_bold_depth > 0 || (m_heading >= 1 && m_heading <= 3)) f |= FL_BOLD;
    m_pen_flags = f;
    if (m_cur_link >= 0)                     m_pen_color = LINK_COLOR;
    else if (m_heading >= 1 && m_heading <= 6) m_pen_color = HEADING_COLOR;
    else if (m_dim_depth > 0)                m_pen_color = DIM_COLOR;
    else                                     m_pen_color = TEXT_COLOR;
}

void BrowserWindow::add_frag(int x, int y, int w, int off, int len,
                             unsigned int color, unsigned char flags, short link)
{
    if (m_frag_count >= MAX_FRAGS) return;
    Frag& f = m_frags[m_frag_count++];
    f.x = x; f.y = y; f.w = w; f.off = off; f.len = len;
    f.color = color; f.flags = flags; f.link = link;
}

void BrowserWindow::flush_frag()
{
    if (m_text_len > m_frag_start)
        add_frag(m_frag_x, m_pen_y, m_pen_x - m_frag_x,
                 m_frag_start, m_text_len - m_frag_start,
                 m_pen_color, m_pen_flags, m_pen_link);
    m_frag_start = m_text_len;
    m_frag_x = m_pen_x;
}

void BrowserWindow::put_run(const char* s, int len)
{
    int stored = 0;
    for (int k = 0; k < len; ++k) {
        if (m_text_len < TEXT_CAP - 1) { m_text[m_text_len++] = s[k]; ++stored; }
        else break;
    }
    m_pen_x += stored * CW;
    m_at_line_start = false; m_any_content = true; m_gap_done = false;
}

void BrowserWindow::line_break()
{
    flush_frag();
    m_pen_y += LH;
    m_pen_x = m_indent;
    m_at_line_start = true;
    m_frag_x = m_pen_x;
    m_frag_start = m_text_len;
}

void BrowserWindow::ensure_block(bool gap)
{
    if (!m_any_content) return;
    if (!m_at_line_start) line_break();
    if (gap && !m_gap_done) { m_pen_y += PARA_GAP; m_gap_done = true; }
}

void BrowserWindow::add_hr()
{
    flush_word();
    if (!m_at_line_start) line_break(); else flush_frag();
    add_frag(m_indent, m_pen_y, content_w() - m_indent, 0, 0, HR_COLOR, FL_HR, -1);
    m_pen_y += LH;
    m_pen_x = m_indent; m_at_line_start = true;
    m_frag_x = m_pen_x; m_frag_start = m_text_len;
    m_any_content = true;
}

void BrowserWindow::emit_word(const char* w, int wl)
{
    if (wl <= 0) return;
    int right = content_w();
    int ww = wl * CW;
    int avail = right - m_indent; if (avail < CW) avail = right;

    if (ww > avail) {                          /* hard-break long token */
        int per = avail / CW; if (per < 1) per = 1;
        int idx = 0;
        while (idx < wl) {
            if (!m_at_line_start) line_break();
            int take = (wl - idx < per) ? (wl - idx) : per;
            put_run(w + idx, take);
            idx += take;
            if (idx < wl) line_break();
        }
        return;
    }
    if (!m_at_line_start && m_pen_x + CW + ww > right) line_break();
    if (!m_at_line_start) put_run(" ", 1);
    put_run(w, wl);
}

void BrowserWindow::flush_word()
{
    if (m_word_len > 0) { emit_word(m_word, m_word_len); m_word_len = 0; }
}

void BrowserWindow::handle_tag(const char* name, bool closing,
                               const char* attrs, int attrlen)
{
    flush_word();
    int nl = bw_strlen(name);
    bool is_h = (nl == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6');

    if (closing) {
        if (bw_streq(name, "p") || bw_streq(name, "div")) { ensure_block(true); return; }
        if (is_h) { m_heading = 0; flush_frag(); recompute_pen(); ensure_block(true); return; }
        if (bw_streq(name, "a")) { m_cur_link = -1; flush_frag(); recompute_pen(); return; }
        if (bw_streq(name, "b") || bw_streq(name, "strong")) {
            if (m_bold_depth > 0) --m_bold_depth;
            flush_frag(); recompute_pen(); return; }
        if (bw_streq(name, "i") || bw_streq(name, "em")) {
            if (m_dim_depth > 0) --m_dim_depth;
            flush_frag(); recompute_pen(); return; }
        if (bw_streq(name, "ul") || bw_streq(name, "ol")) {
            if (m_list_depth > 0) --m_list_depth;
            recompute_indent(); ensure_block(false); return; }
        if (bw_streq(name, "blockquote")) {
            if (m_quote_depth > 0) --m_quote_depth;
            recompute_indent(); ensure_block(false); return; }
        if (bw_streq(name, "pre"))   { m_pre = false; line_break(); return; }
        if (bw_streq(name, "title")) { m_in_title = false; return; }
        if (bw_streq(name, "head"))  { m_in_head  = false; return; }
        if (bw_streq(name, "li"))    { return; }
        return;
    }

    if (bw_streq(name, "br")) {
        if (m_at_line_start) { if (m_any_content) m_pen_y += LH; } else line_break();
        return;
    }
    if (bw_streq(name, "p") || bw_streq(name, "div")) { ensure_block(true); return; }
    if (is_h) { ensure_block(true); m_heading = name[1] - '0'; flush_frag(); recompute_pen(); return; }
    if (bw_streq(name, "hr")) { add_hr(); return; }
    if (bw_streq(name, "ul") || bw_streq(name, "ol")) {
        ensure_block(false); ++m_list_depth; recompute_indent(); return; }
    if (bw_streq(name, "li")) {
        if (!m_at_line_start) line_break();
        put_run("\x07", 1);          /* bullet glyph; word adds its own space */
        return;
    }
    if (bw_streq(name, "blockquote")) {
        ensure_block(false); ++m_quote_depth; recompute_indent(); return; }
    if (bw_streq(name, "pre")) { ensure_block(false); m_pre = true; return; }
    if (bw_streq(name, "a")) {
        char href[LINK_MAX];
        if (bw_get_attr(attrs, attrlen, "href", href, LINK_MAX) && href[0] &&
            m_link_count < MAX_LINKS) {
            bw_url_join(m_url, href, m_links[m_link_count], LINK_MAX);
            m_cur_link = m_link_count; ++m_link_count;
        } else {
            m_cur_link = -1;
        }
        flush_frag(); recompute_pen();
        return;
    }
    if (bw_streq(name, "img")) {
        char src[LINK_MAX];
        if (bw_get_attr(attrs, attrlen, "src", src, LINK_MAX) && src[0]) {
            char abs[LINK_MAX];
            bw_url_join(m_url, src, abs, LINK_MAX);
            int idx = load_image(abs);
            if (idx >= 0) { add_img_frag(idx); return; }
        }
        /* undecodable / unfetchable: show the alt text, or a marker */
        char alt[64];
        if (!bw_get_attr(attrs, attrlen, "alt", alt, (int)sizeof(alt)) || !alt[0])
            bw_strcpy(alt, "[image]", (int)sizeof(alt));
        int al = bw_strlen(alt);
        emit_word(alt, al);
        return;
    }
    if (bw_streq(name, "b") || bw_streq(name, "strong")) {
        ++m_bold_depth; flush_frag(); recompute_pen(); return; }
    if (bw_streq(name, "i") || bw_streq(name, "em")) {
        ++m_dim_depth; flush_frag(); recompute_pen(); return; }
    if (bw_streq(name, "title")) { m_in_title = true; m_title[0] = '\0'; return; }
    if (bw_streq(name, "head"))  { m_in_head  = true; return; }
    if (bw_streq(name, "body"))  { m_in_head  = false; return; }
    /* unknown tag → ignored, its text content is preserved */
}

void BrowserWindow::layout(const char* src)
{
    reset_layout();
    m_last_src = src;
    m_layout_w = content_w();

    const char* h = src;
    int n = bw_strlen(src);
    int i = 0;
    while (i < n) {
        char c = h[i];
        if (c == '<') {
            /* comment */
            if (i + 3 < n && h[i + 1] == '!' && h[i + 2] == '-' && h[i + 3] == '-') {
                int k = i + 4;
                while (k + 2 < n && !(h[k] == '-' && h[k + 1] == '-' && h[k + 2] == '>')) ++k;
                i = (k + 2 < n) ? k + 3 : n; continue;
            }
            if (i + 1 < n && (h[i + 1] == '!' || h[i + 1] == '?')) {
                int k = i + 1; while (k < n && h[k] != '>') ++k;
                i = (k < n) ? k + 1 : n; continue;
            }
            int j = i + 1; bool closing = false;
            if (j < n && h[j] == '/') { closing = true; ++j; }
            char name[24]; int nl = 0;
            while (j < n) {
                char d = h[j];
                if (bw_isalnum(d)) { if (nl < 23) name[nl++] = bw_lower(d); ++j; }
                else break;
            }
            name[nl] = '\0';
            int te = j; while (te < n && h[te] != '>') ++te;
            const char* attrs = h + j; int attrlen = te - j;
            int after = (te < n) ? te + 1 : n;
            if (nl == 0) { i = after; continue; }
            if (!closing && (bw_streq(name, "script") || bw_streq(name, "style"))) {
                int k = bw_find_close(h, n, after, name);
                i = (k >= 0) ? k : n; continue;
            }
            handle_tag(name, closing, attrs, attrlen);
            i = after; continue;
        }

        if (m_in_title) {
            if (c == '&') {
                char eb[16]; int el = 0; i = bw_decode_entity(h, n, i, eb, &el);
                for (int q = 0; q < el; ++q) {
                    int L = bw_strlen(m_title);
                    if (L < 126) { m_title[L] = eb[q]; m_title[L + 1] = '\0'; }
                }
                continue;
            }
            int L = bw_strlen(m_title);
            if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
                if (L > 0 && m_title[L - 1] != ' ' && L < 126) { m_title[L] = ' '; m_title[L + 1] = '\0'; }
            } else if (L < 126) { m_title[L] = c; m_title[L + 1] = '\0'; }
            ++i; continue;
        }
        if (m_in_head) { ++i; continue; }

        if (m_pre) {
            if (c == '\n') {
                flush_frag(); m_pen_y += LH; m_pen_x = m_indent;
                m_at_line_start = true; m_frag_x = m_pen_x; m_frag_start = m_text_len;
                m_any_content = true; ++i; continue;
            }
            if (c == '\r') { ++i; continue; }
            if (c == '\t') { put_run("    ", 4); ++i; continue; }
            if (c == '&') { char eb[16]; int el = 0; i = bw_decode_entity(h, n, i, eb, &el); put_run(eb, el); continue; }
            put_run(&c, 1); ++i; continue;
        }

        /* flow mode */
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') { flush_word(); ++i; continue; }
        if (c == '&') {
            char eb[16]; int el = 0; i = bw_decode_entity(h, n, i, eb, &el);
            for (int q = 0; q < el; ++q) if (m_word_len < LINK_MAX - 1) m_word[m_word_len++] = eb[q];
            continue;
        }
        if (m_word_len < LINK_MAX - 1) m_word[m_word_len++] = c;
        ++i;
    }
    flush_word();
    flush_frag();
    m_content_h = m_pen_y + LH;
    if (m_scroll > max_scroll()) m_scroll = max_scroll();
}

/* ═══════════════════════════════════════════════════════════════════════════
   Navigation
   ═══════════════════════════════════════════════════════════════════════════ */

void BrowserWindow::free_body()
{
    if (m_body) {
        /* m_last_src may alias the body buffer (set in layout()); null it so a
           later reflow in draw() can't read freed memory on early-return nav
           paths (https:// / unsupported scheme). */
        if (m_last_src == m_body) m_last_src = nullptr;
        kfree(m_body); m_body = nullptr;
    }
}

/* ── inline images ───────────────────────────────────────────────────────── */

void BrowserWindow::free_images()
{
    for (int i = 0; i < m_img_count; ++i) {
        if (m_imgs[i].px) kfree(m_imgs[i].px);
        m_imgs[i].px = nullptr;
    }
    m_img_count = 0;
}

/* Decode `data` as PNG, scale it to the page width (nearest-neighbour,
   integer), composite alpha over the parchment, and register it in the
   cache under `abs_url`. Returns the image index, or -1 (a failed entry
   is still cached so the same broken URL is not refetched).               */
int BrowserWindow::register_image_from(const char* abs_url,
                                       const unsigned char* data, int len)
{
    if (m_img_count >= MAX_IMGS) return -1;
    Img& im = m_imgs[m_img_count];
    bw_strcpy(im.url, abs_url, (int)sizeof(im.url));
    im.px = nullptr; im.dw = 0; im.dh = 0; im.failed = true;
    int idx = m_img_count++;

    int w = 0, h = 0;
    if (!png_dims(data, len, &w, &h)) return -1;

    int scratch_len = png_scratch_size(w, h);
    unsigned char* scratch = (unsigned char*)kmalloc((unsigned)scratch_len);
    unsigned int*  full    = (unsigned int*)kmalloc((unsigned)w * (unsigned)h * 4u);
    if (scratch && full &&
        png_decode(data, len, scratch, scratch_len, full, WHITE)) {
        int maxw = content_w() - 4; if (maxw < 16) maxw = 16;
        int dw = w, dh = h;
        if (dw > maxw) { dh = (h * maxw) / w; if (dh < 1) dh = 1; dw = maxw; }
        unsigned int* sc = (unsigned int*)kmalloc((unsigned)dw * (unsigned)dh * 4u);
        if (sc) {
            for (int y = 0; y < dh; ++y) {
                const unsigned int* srow = full + (long)((long)y * h / dh) * w;
                unsigned int*       drow = sc + (long)y * dw;
                for (int x = 0; x < dw; ++x) drow[x] = srow[(long)x * w / dw];
            }
            im.px = sc; im.dw = dw; im.dh = dh; im.failed = false;
        }
    }
    if (scratch) kfree(scratch);
    if (full)    kfree(full);
    return im.failed ? -1 : idx;
}

int BrowserWindow::load_image(const char* abs_url)
{
    for (int i = 0; i < m_img_count; ++i)            /* cache (re-layouts) */
        if (bw_streq(m_imgs[i].url, abs_url))
            return m_imgs[i].failed ? -1 : i;
    if (m_img_count >= MAX_IMGS || !net_ready()) return -1;

    bool https = bw_starts(abs_url, "https://");
    if (!https && !bw_starts(abs_url, "http://")) return -1;

    char  final_url[URL_MAX]; final_url[0] = '\0';
    char  ctype[64];          ctype[0] = '\0';
    char* body = nullptr; int blen = 0;
    int code = https
             ? https_get(abs_url, &body, &blen, IMG_FETCH_MAX, final_url, ctype)
             : http_get (abs_url, &body, &blen, IMG_FETCH_MAX, final_url, ctype);

    int idx = -1;
    if (code >= 200 && code < 300 && body && blen > 8)
        idx = register_image_from(abs_url, (const unsigned char*)body, blen);
    else if (m_img_count < MAX_IMGS) {               /* cache the failure  */
        Img& im = m_imgs[m_img_count++];
        bw_strcpy(im.url, abs_url, (int)sizeof(im.url));
        im.px = nullptr; im.failed = true;
    }
    if (body) kfree(body);
    return idx;
}

/* Place a decoded image as a block frag at the current pen position —
   same pen mechanics as add_hr() (the reference block element).           */
void BrowserWindow::add_img_frag(int idx)
{
    if (idx < 0 || idx >= m_img_count || m_imgs[idx].failed) return;
    Img& im = m_imgs[idx];
    flush_word();
    if (!m_at_line_start) line_break(); else flush_frag();
    add_frag(m_indent, m_pen_y, im.dw, idx, im.dh, 0, FL_IMG, -1);
    m_pen_y += im.dh + 4;
    m_pen_x = m_indent; m_at_line_start = true;
    m_frag_x = m_pen_x; m_frag_start = m_text_len;
    m_any_content = true;
    m_gap_done    = false;
}

void BrowserWindow::set_error_page(const char* title, const char* l1,
                                   const char* l2, const char* l3)
{
    int p = 0; char* b = m_errbuf; int cap = (int)sizeof(m_errbuf);
    p = s_cat(b, p, cap, "<h1>"); p = s_cat(b, p, cap, title); p = s_cat(b, p, cap, "</h1>");
    if (l1 && l1[0]) { p = s_cat(b, p, cap, "<p>"); p = s_cat(b, p, cap, l1); p = s_cat(b, p, cap, "</p>"); }
    if (l2 && l2[0]) { p = s_cat(b, p, cap, "<p>"); p = s_cat(b, p, cap, l2); p = s_cat(b, p, cap, "</p>"); }
    if (l3 && l3[0]) { p = s_cat(b, p, cap, "<p>"); p = s_cat(b, p, cap, l3); p = s_cat(b, p, cap, "</p>"); }
    s_cat(b, p, cap, "<hr><p><a href=\"greg://home\">Retour a l'accueil</a></p>");
    m_scroll = 0;
    layout(m_errbuf);
}

void BrowserWindow::navigate(const char* urlin, bool push)
{
    m_hover[0] = '\0';

    char url[URL_MAX];
    /* trim leading spaces */
    { int s = 0; while (urlin[s] == ' ' || urlin[s] == '\t') ++s; bw_strcpy(url, urlin + s, URL_MAX); }
    { int e = bw_strlen(url); while (e > 0 && (url[e - 1] == ' ' || url[e - 1] == '\t')) url[--e] = '\0'; }
    if (url[0] == '\0') return;

    free_body();   /* release previous document buffer on every navigation */
    free_images(); /* and the previous page's decoded images               */

    /* internal pseudo-pages */
    if (bw_starts(url, "greg://")) {
        const char* page = url + 7;
        if (bw_streq(page, "about") || bw_streq(page, "about/")) {
            bw_strcpy(m_url, "greg://about", URL_MAX);
            m_scroll = 0; layout(bw_about_html());
        } else {
            bw_strcpy(m_url, "greg://home", URL_MAX);
            m_scroll = 0; layout(bw_home_html());
        }
        bw_strcpy(m_status, "Page interne GregNet", (int)sizeof(m_status));
        m_scroll = 0;
        bw_strcpy(m_addr, m_url, URL_MAX); m_addr_len = bw_strlen(m_addr); m_addr_focus = false;
        if (push) push_history(m_url);
        return;
    }

    if (!bw_has_scheme(url)) {
        char tmp[URL_MAX]; bw_strcpy(tmp, "http://", URL_MAX);
        bw_strcat(tmp, url, URL_MAX); bw_strcpy(url, tmp, URL_MAX);
    }
    bool is_https = bw_starts(url, "https://");
    if (!is_https && !bw_starts(url, "http://")) {
        bw_strcpy(m_status, "Schema non supporte (http/https)", (int)sizeof(m_status));
        return;
    }

    if (!net_ready()) {
        bw_strcpy(m_url, url, URL_MAX);
        set_error_page("Reseau indisponible",
                       "La carte reseau n'est pas prete.",
                       "Lancez GregOS avec:  make run",
                       "(une carte rtl8139 est requise pour http).");
        bw_strcpy(m_status, "Reseau indisponible", (int)sizeof(m_status));
        bw_strcpy(m_addr, m_url, URL_MAX); m_addr_len = bw_strlen(m_addr); m_addr_focus = false;
        if (push) push_history(m_url);
        return;
    }

    /* show loading state before the blocking fetch */
    { int p = s_cat(m_status, 0, (int)sizeof(m_status), "Chargement ");
      p = s_cat(m_status, p, (int)sizeof(m_status), url);
      s_cat(m_status, p, (int)sizeof(m_status), " ..."); }
    draw_status_bar();
    gfx_swap_buffers();

    char  final_url[URL_MAX]; final_url[0] = '\0';
    char  ctype[64];          ctype[0] = '\0';
    int   len = 0; char* body = nullptr;
    int   code = is_https
               ? https_get(url, &body, &len, HTTP_MAX_LEN, final_url, ctype)
               : http_get (url, &body, &len, HTTP_MAX_LEN, final_url, ctype);
    m_body = body;

    /* Classify explicit error codes BEFORE the generic no-body case: on any
       failure `body` is null, so testing !body first would mask them all. */
    if (code < 0) {
        free_body();
        bw_strcpy(m_url, url, URL_MAX);
        if (code == -3)
            set_error_page("Certificat invalide", tls_cert_error(), url,
                           "Connexion refusee : l'identite du serveur n'a pas pu etre verifiee (risque MITM).");
        else if (code == -2)
            set_error_page("Echec TLS", "La poignee de main TLS a echoue.", url,
                           "Le serveur ne parle peut-etre pas ECDHE-x25519 / AES-128-GCM.");
        else
            set_error_page("URL invalide", "Adresse malformee ou non supportee.", url, "");
        bw_strcpy(m_status, code == -3 ? "Certificat invalide"
                          : code == -2 ? "Echec TLS/HTTPS" : "URL invalide", (int)sizeof(m_status));
        bw_strcpy(m_addr, m_url, URL_MAX); m_addr_len = bw_strlen(m_addr); m_addr_focus = false;
        if (push) push_history(m_url);
        return;
    }
    if (code == 0 || !body) {
        free_body();
        bw_strcpy(m_url, url, URL_MAX);
        set_error_page("Erreur reseau",
                       "Impossible de contacter le serveur.", url,
                       "Verifiez le nom d'hote ou la connexion.");
        bw_strcpy(m_status, "Erreur reseau", (int)sizeof(m_status));
        bw_strcpy(m_addr, m_url, URL_MAX); m_addr_len = bw_strlen(m_addr); m_addr_focus = false;
        if (push) push_history(m_url);
        return;
    }

    /* success */
    bw_strcpy(m_url, (final_url[0] ? final_url : url), URL_MAX);
    m_scroll = 0;

    /* direct image document: the whole body is a PNG → synthesize a page
       around it (the <img> loader will hit the cache, no refetch)          */
    if (len > 8 && (unsigned char)body[0] == 0x89 &&
        body[1] == 'P' && body[2] == 'N' && body[3] == 'G') {
        int ok = register_image_from(m_url, (const unsigned char*)body, len);
        int p = 0; char* b2 = m_errbuf; int cap = (int)sizeof(m_errbuf);
        if (ok >= 0) {
            p = s_cat(b2, p, cap, "<h2>Image PNG</h2><p><img src=\"");
            p = s_cat(b2, p, cap, m_url);
            p = s_cat(b2, p, cap, "\"></p>");
        } else {
            p = s_cat(b2, p, cap,
                      "<h1>Image illisible</h1><p>Le PNG n'a pas pu etre decode "
                      "(entrelace, 16 bits ou corrompu).</p>");
        }
        s_cat(b2, p, cap, "<hr><p><a href=\"greg://home\">Retour a l'accueil</a></p>");
        layout(m_errbuf);
        bw_strcpy(m_status, ok >= 0 ? "Image PNG decodee" : "PNG non decodable",
                  (int)sizeof(m_status));
        bw_strcpy(m_addr, m_url, URL_MAX); m_addr_len = bw_strlen(m_addr);
        m_addr_focus = false;
        if (push) push_history(m_url);
        return;
    }

    layout(m_body);

    /* status: HTTP code, size, content-type, title */
    { int p = s_cat(m_status, 0, (int)sizeof(m_status), "HTTP ");
      p = s_cat_int(m_status, p, (int)sizeof(m_status), code);
      p = s_cat(m_status, p, (int)sizeof(m_status), "  ");
      p = s_cat_int(m_status, p, (int)sizeof(m_status), len);
      p = s_cat(m_status, p, (int)sizeof(m_status), " octets");
      if (ctype[0]) { p = s_cat(m_status, p, (int)sizeof(m_status), "  [");
                      p = s_cat(m_status, p, (int)sizeof(m_status), ctype);
                      p = s_cat(m_status, p, (int)sizeof(m_status), "]"); }
      if (m_title[0]) { p = s_cat(m_status, p, (int)sizeof(m_status), "  -  ");
                        s_cat(m_status, p, (int)sizeof(m_status), m_title); } }

    bw_strcpy(m_addr, m_url, URL_MAX); m_addr_len = bw_strlen(m_addr); m_addr_focus = false;
    if (push) push_history(m_url);
}

void BrowserWindow::push_history(const char* url)
{
    if (m_hist_count == 0) {
        bw_strcpy(m_hist[0], url, URL_MAX);
        m_hist_count = 1; m_hist_pos = 0;
        return;
    }
    ++m_hist_pos;
    if (m_hist_pos >= HIST_MAX) {
        for (int i = 0; i < HIST_MAX - 1; ++i) bw_strcpy(m_hist[i], m_hist[i + 1], URL_MAX);
        m_hist_pos = HIST_MAX - 1;
    }
    bw_strcpy(m_hist[m_hist_pos], url, URL_MAX);
    m_hist_count = m_hist_pos + 1;
}

void BrowserWindow::go_back()
{
    if (m_hist_pos > 0) {
        --m_hist_pos;
        char u[URL_MAX]; bw_strcpy(u, m_hist[m_hist_pos], URL_MAX);
        navigate(u, false);
    }
}
void BrowserWindow::go_forward()
{
    if (m_hist_pos < m_hist_count - 1) {
        ++m_hist_pos;
        char u[URL_MAX]; bw_strcpy(u, m_hist[m_hist_pos], URL_MAX);
        navigate(u, false);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Drawing
   ═══════════════════════════════════════════════════════════════════════════ */

BrowserWindow::TbRects BrowserWindow::tb_rects() const
{
    TbRects r;
    int cx = client_x(), cw = client_w();
    r.h = 22; r.y = client_y() + 3; r.btn_w = 22;
    r.back_x = cx + 4;
    r.fwd_x  = r.back_x + r.btn_w + 2;
    r.rel_x  = r.fwd_x + r.btn_w + 2;
    r.go_w   = 34;
    r.go_x   = cx + cw - 4 - r.go_w;
    r.fld_x  = r.rel_x + r.btn_w + 4;
    r.fld_w  = r.go_x - 6 - r.fld_x;
    if (r.fld_w < 40) r.fld_w = 40;
    return r;
}

void BrowserWindow::draw_btn(int x, int y, int w, int h, const char* label, bool enabled)
{
    Graphics& g = Graphics::instance();
    g.fill_rect(x, y, w, h, Theme::BTN_FACE);
    g.draw_hline(x,       y,       w, Theme::BEVEL_OUTER_LT);
    g.draw_vline(x,       y,       h, Theme::BEVEL_OUTER_LT);
    g.draw_hline(x,       y + h - 1, w, Theme::BEVEL_OUTER_DK);
    g.draw_vline(x + w - 1, y,     h, Theme::BEVEL_OUTER_DK);
    int lw = bw_strlen(label) * CW;
    unsigned int fg = enabled ? Theme::AMBER : Theme::FG_MUTED;
    g.draw_str(x + (w - lw) / 2, y + (h - 16) / 2, label, fg, GFX_TRANSPARENT);
}

void BrowserWindow::draw_toolbar()
{
    Graphics& g = Graphics::instance();
    int tx = client_x(), ty = client_y(), tw = client_w();
    g.fill_rect(tx, ty, tw, TOOLBAR_H, Theme::WIN_BG);
    g.draw_hline(tx, ty + TOOLBAR_H - 1, tw, Theme::BEVEL_OUTER_DK);

    TbRects r = tb_rects();
    bool can_back = m_hist_pos > 0;
    bool can_fwd  = m_hist_pos < m_hist_count - 1;
    draw_btn(r.back_x, r.y, r.btn_w, r.h, "<", can_back);
    draw_btn(r.fwd_x,  r.y, r.btn_w, r.h, ">", can_fwd);
    draw_btn(r.rel_x,  r.y, r.btn_w, r.h, "O", true);
    draw_btn(r.go_x,   r.y, r.go_w,  r.h, "GO", true);

    /* address field */
    g.fill_rect(r.fld_x, r.y, r.fld_w, r.h, WHITE);
    g.draw_rect(r.fld_x, r.y, r.fld_w, r.h, m_addr_focus ? Theme::GOLD : Theme::BEVEL_OUTER_DK);

    int vis = (r.fld_w - 8) / CW; if (vis < 1) vis = 1;
    int start = (m_addr_len > vis) ? m_addr_len - vis : 0;
    char vbuf[128]; int q = 0;
    for (int k = start; k < m_addr_len && q < 127; ++k) vbuf[q++] = m_addr[k];
    vbuf[q] = '\0';
    int cx1, cy1, cx2, cy2; g.get_clip(cx1, cy1, cx2, cy2);
    g.set_clip(r.fld_x + 2, r.y, r.fld_w - 4, r.h);
    g.draw_str(r.fld_x + 4, r.y + (r.h - 16) / 2, vbuf, Theme::VELLUM_INK, GFX_TRANSPARENT);
    if (m_addr_focus && (jiffies / 50) % 2 == 0) {
        int cxp = r.fld_x + 4 + (m_addr_len - start) * CW;
        g.draw_vline(cxp, r.y + 3, r.h - 6, Theme::VELLUM_INK);
    }
    g.set_clip_raw(cx1, cy1, cx2, cy2);
}

void BrowserWindow::draw_content()
{
    Graphics& g = Graphics::instance();
    int vx = client_x(), vy = client_y() + TOOLBAR_H;
    int vw = client_w() - SB_W, vh = render_h();
    if (vh <= 0) return;

    g.fill_rect(vx, vy, vw, vh, WHITE);
    int cx1, cy1, cx2, cy2; g.get_clip(cx1, cy1, cx2, cy2);
    g.set_clip(vx, vy, vw, vh);

    int cx0  = client_x() + PAD;
    int ctop = content_top();
    for (int i = 0; i < m_frag_count; ++i) {
        Frag& f = m_frags[i];
        int sy = ctop - m_scroll + f.y;
        int fh = (f.flags & FL_IMG) ? f.len : LH;   /* culling height */
        if (sy + fh < vy || sy > vy + vh) continue;
        int sx = cx0 + f.x;
        if (f.flags & FL_IMG) {
            if (f.off >= 0 && f.off < m_img_count && m_imgs[f.off].px)
                g.blit_opaque(sx, sy, f.w, f.len, m_imgs[f.off].px);
            continue;
        }
        if (f.flags & FL_HR) {
            g.draw_hline(sx, sy + LH / 2, f.w, HR_COLOR);
            continue;
        }
        int bl = f.len; if (bl > 259) bl = 259;
        char buf[260];
        for (int k = 0; k < bl; ++k) buf[k] = m_text[f.off + k];
        buf[bl] = '\0';
        int tyy = sy + (LH - 16) / 2;
        g.draw_str(sx, tyy, buf, f.color, WHITE);
        if (f.flags & FL_BOLD) g.draw_str(sx + 1, tyy, buf, f.color, GFX_TRANSPARENT);
        if (f.flags & FL_UL)   g.draw_hline(sx, sy + LH - 3, f.w, f.color);
    }
    g.set_clip_raw(cx1, cy1, cx2, cy2);
}

void BrowserWindow::draw_scrollbar()
{
    Graphics& g = Graphics::instance();
    int sbx = client_x() + client_w() - SB_W;
    int sby = client_y() + TOOLBAR_H;
    int sbh = render_h();
    if (sbh <= 0) return;

    g.fill_rect(sbx, sby, SB_W, sbh, Theme::WIN_BG);
    g.draw_vline(sbx, sby, sbh, Theme::BEVEL_INNER_DK);

    int total = m_content_h + 2 * PAD;
    if (total <= sbh) return;
    int th = sbh * sbh / total; if (th < 16) th = 16; if (th > sbh) th = sbh;
    int ms = max_scroll();
    int ty = sby + (ms > 0 ? (sbh - th) * m_scroll / ms : 0);
    g.fill_rect(sbx + 2, ty, SB_W - 3, th, Theme::BTN_FACE);
    g.draw_hline(sbx + 2, ty,          SB_W - 3, Theme::BEVEL_OUTER_LT);
    g.draw_vline(sbx + 2, ty,          th,       Theme::BEVEL_OUTER_LT);
    g.draw_hline(sbx + 2, ty + th - 1, SB_W - 3, Theme::BEVEL_OUTER_DK);
    g.draw_vline(sbx + SB_W - 1, ty,   th,       Theme::BEVEL_OUTER_DK);
}

void BrowserWindow::draw_status_bar()
{
    Graphics& g = Graphics::instance();
    int sx = client_x(), sw = client_w();
    int sy = client_y() + client_h() - STATUS_H;
    g.fill_rect(sx, sy, sw, STATUS_H, Theme::WIN_BG);
    g.draw_hline(sx, sy, sw, Theme::BEVEL_INNER_LT);
    const char* txt = m_hover[0] ? m_hover : m_status;
    int cx1, cy1, cx2, cy2;
    g.get_clip(cx1, cy1, cx2, cy2);
    g.set_clip(sx, sy, sw, STATUS_H);
    g.draw_str(sx + 6, sy + (STATUS_H - 16) / 2, txt, 0x202020u, GFX_TRANSPARENT);
    g.set_clip_raw(cx1, cy1, cx2, cy2);
}

void BrowserWindow::draw()
{
    Window::draw();   /* chrome + white client fill */

    /* re-flow if the window width changed since the last layout */
    if (m_last_src && content_w() != m_layout_w) {
        int keep = m_scroll;
        layout(m_last_src);
        m_scroll = (keep > max_scroll()) ? max_scroll() : keep;
    }

    draw_content();
    draw_scrollbar();
    draw_toolbar();
    draw_status_bar();
}

/* ═══════════════════════════════════════════════════════════════════════════
   Input
   ═══════════════════════════════════════════════════════════════════════════ */

void BrowserWindow::scroll_by(int dy)
{
    m_scroll += dy;
    int ms = max_scroll();
    if (m_scroll > ms) m_scroll = ms;
    if (m_scroll < 0)  m_scroll = 0;
}

int BrowserWindow::link_at(int mx, int my) const
{
    int cx0 = client_x() + PAD;
    int ctop = content_top();
    int content_x = mx - cx0;
    int content_y = my - ctop + m_scroll;
    for (int i = 0; i < m_frag_count; ++i) {
        const Frag& f = m_frags[i];
        if (f.link < 0) continue;
        if (content_x >= f.x && content_x < f.x + f.w &&
            content_y >= f.y && content_y < f.y + LH)
            return f.link;
    }
    return -1;
}

void BrowserWindow::update_hover(int mx, int my)
{
    int vy = client_y() + TOOLBAR_H, vh = render_h();
    if (mx < client_x() || mx >= client_x() + client_w() - SB_W ||
        my < vy || my >= vy + vh) { m_hover[0] = '\0'; return; }
    int idx = link_at(mx, my);
    if (idx >= 0) bw_strcpy(m_hover, m_links[idx], URL_MAX);
    else m_hover[0] = '\0';
}

void BrowserWindow::activate_link(int idx)
{
    if (idx < 0 || idx >= m_link_count) return;
    const char* u = m_links[idx];
    if (bw_starts(u, "mailto:") || bw_starts(u, "javascript:") || bw_starts(u, "tel:")) {
        bw_strcpy(m_status, "Lien non supporte", (int)sizeof(m_status));
        return;
    }
    char tmp[URL_MAX]; bw_strcpy(tmp, u, URL_MAX);
    navigate(tmp, true);
}

void BrowserWindow::handle_toolbar_click(int mx, int my)
{
    TbRects r = tb_rects();
    m_addr_focus = false;
    if (my < r.y || my >= r.y + r.h) return;
    if (mx >= r.back_x && mx < r.back_x + r.btn_w) { go_back();  return; }
    if (mx >= r.fwd_x  && mx < r.fwd_x  + r.btn_w) { go_forward(); return; }
    if (mx >= r.rel_x  && mx < r.rel_x  + r.btn_w) {
        char u[URL_MAX]; bw_strcpy(u, m_url, URL_MAX); navigate(u, false); return; }
    if (mx >= r.fld_x  && mx < r.fld_x  + r.fld_w) { m_addr_focus = true; return; }
    if (mx >= r.go_x   && mx < r.go_x   + r.go_w) {
        char u[URL_MAX]; bw_strcpy(u, m_addr, URL_MAX); navigate(u, true); return; }
}

void BrowserWindow::handle_scroll_click(int my)
{
    int sby = client_y() + TOOLBAR_H, sbh = render_h();
    int total = m_content_h + 2 * PAD;
    if (total <= sbh) return;
    int page = render_h() - LH; if (page < LH) page = LH;
    int th = sbh * sbh / total; if (th < 16) th = 16;
    int ms = max_scroll();
    int ty = sby + (ms > 0 ? (sbh - th) * m_scroll / ms : 0);
    if (my < ty)            scroll_by(-page);
    else if (my > ty + th)  scroll_by(page);
}

bool BrowserWindow::on_event(const Event& e)
{
    if (e.type == EVT_MOUSE_MOVE) {
        update_hover(e.mouse.x, e.mouse.y);
        return Window::on_event(e);
    }
    if (e.type == EVT_MOUSE_BUTTON) {
        bool pressed = (e.mouse.buttons & 0x01) != 0;
        int mx = e.mouse.x, my = e.mouse.y;
        if (!pressed)          return Window::on_event(e);
        if (!hit_test(mx, my)) return false;
        if (my < client_y())   return Window::on_event(e);   /* title/border */

        int cy = client_y();
        if (my < cy + TOOLBAR_H) { handle_toolbar_click(mx, my); return true; }

        int sbx = client_x() + client_w() - SB_W;
        int vy  = cy + TOOLBAR_H, vh = render_h();
        if (mx >= sbx && my >= vy && my < vy + vh) {
            m_addr_focus = false; handle_scroll_click(my); return true;
        }
        if (my >= cy + client_h() - STATUS_H) { m_addr_focus = false; return true; }

        /* content click */
        m_addr_focus = false;
        int li = link_at(mx, my);
        if (li >= 0) activate_link(li);
        return true;
    }
    return Window::on_event(e);
}

bool BrowserWindow::handle_char(int c)
{
    if (m_addr_focus) {
        if (c == '\r' || c == '\n') {
            m_addr_focus = false;
            char u[URL_MAX]; bw_strcpy(u, m_addr, URL_MAX);
            navigate(u, true);
            return true;
        }
        if (c == KEY_ESC) {
            m_addr_focus = false;
            bw_strcpy(m_addr, m_url, URL_MAX); m_addr_len = bw_strlen(m_addr);
            return true;
        }
        if (c == '\b' || c == 127) {
            if (m_addr_len > 0) m_addr[--m_addr_len] = '\0';
            return true;
        }
        if (c >= 32 && c < 127) {
            if (m_addr_len < URL_MAX - 1) { m_addr[m_addr_len++] = (char)c; m_addr[m_addr_len] = '\0'; }
            return true;
        }
        /* fall through: nav keys still scroll */
    }

    if (c == KEY_UP)   { scroll_by(-LH); return true; }
    if (c == KEY_DOWN) { scroll_by(LH);  return true; }
    if (c == KEY_PGUP) { scroll_by(-(render_h() - LH)); return true; }
    if (c == KEY_PGDN) { scroll_by(render_h() - LH);    return true; }
    if (c == KEY_HOME) { m_scroll = 0;            return true; }
    if (c == KEY_END)  { m_scroll = max_scroll(); return true; }
    if (c == KEY_ESC)  { request_close();         return true; }
    return false;
}

void BrowserWindow::on_removed()
{
    free_body();
    free_images();
}

void BrowserWindow::init_browser()
{
    navigate("greg://home", true);
}

/* ── open_browser_window: extern "C" bridge ──────────────────────────────── */

extern "C" void open_browser_window_url(const char* url)
{
    auto w = Greg::make_ref<BrowserWindow>();
    int sw = gfx_width(), sh = gfx_height();
    int ww = 660, wh = 480;
    int wx = (sw - ww) / 2, wy = (sh - wh) / 2;
    if (wx < 0) wx = 0;
    if (wy < 0) wy = 0;
    w->setup(wx, wy, ww, wh, "GregNet - Navigateur", Theme::WIN_BG);
    if (url && url[0]) {
        char full[512]; int p = 0;
        bool has_scheme = (url[0]=='h'&&url[1]=='t'&&url[2]=='t'&&url[3]=='p') ||
                          (url[0]=='g'&&url[1]=='r'&&url[2]=='e'&&url[3]=='g');
        if (!has_scheme) { const char* pfx = "http://"; while (pfx[p]) { full[p]=pfx[p]; ++p; } }
        for (int i = 0; url[i] && p < 510; ++i) full[p++] = url[i];
        full[p] = '\0';
        w->open_url(full);
    } else {
        w->init_browser();
    }
    w->set_focused(true);
    WindowManager::instance().add_window(Greg::move(w));
}

extern "C" void open_browser_window(void)
{
    open_browser_window_url(0);
}
