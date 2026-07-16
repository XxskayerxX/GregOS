/* RuneChantWindow — « Chant Runique » for GregOS: the kingdom's first music.
   Two-octave rune keyboard on the PC speaker (PIT channel 2), playable by
   mouse and by the physical AZERTY row; four melodies of the realm run in a
   background scheduler thread so the UI never freezes.

   Sound lifecycle — the whole design in three rules:
   1. ONE owner: s_rc_gen is a generation counter; bumping it cancels whatever
      sound logic is in flight. EVERY sound (melody or single key note) is
      produced by an owner thread — never by the window directly — so no
      rendering state can ever leave the speaker stuck on.
   2. Ownership is bound AT SPAWN TIME: the spawner writes a ticket
      (s_rc_ticket_gen/song) that the thread consumes exactly once under
      cli/sti. A cancel issued before the thread first runs bumps s_rc_gen
      past the ticket, so the ghost exits before touching the speaker; two
      rapid spawns leave a single consumer of the latest ticket.
   3. The thread re-checks its generation INSIDE a cli/sti section around
      every speaker write, and timer_speaker_off() is guaranteed on every
      exit path: natural end (owner check), the canceller (stop_all /
      play_note / start_song), and on_removed() when the window closes.     */

#include "../include/RuneChantWindow.hpp"
#include "../include/WindowManager.hpp"
#include "../include/Graphics.hpp"
#include "../include/Theme.hpp"
#include "../include/gfx.h"
#include "../include/keyboard.h"
#include "../include/runechant_data.h"
#include "../include/Kernel/timer_c.h"
#include "../include/Kernel/Scheduler.hpp"

extern "C" volatile unsigned long jiffies;

/* ── shared state between the window and the sound threads ──────────────── */
static volatile unsigned s_rc_gen        = 0;   /* bump = cancel current sound */
static volatile unsigned s_rc_ticket_gen = 0;   /* spawn-bound ticket (0=used) */
static volatile int      s_rc_ticket_song = -1; /* RC_SONG_COUNT = key note    */
static volatile int      s_rc_playing    = -1;  /* song sounding now, for UI   */

/* single key notes go through the same owner-thread machinery as melodies:
   a key press is a one-note song at pseudo-index RC_SONG_COUNT             */
static rc_note s_rc_keynote = { 440, 16 };
static const rc_song s_rc_keysong = { "", &s_rc_keynote, 1 };

/* live instance — the app is a singleton: all s_rc_* state is file-scope,
   so a second window would fight the first over the speaker              */
static RuneChantWindow* s_rc_live = nullptr;

/* ── palette ─────────────────────────────────────────────────────────────── */
static constexpr unsigned int KEY_IVORY  = 0xD8CCA8u;  /* vellum bone        */
static constexpr unsigned int KEY_IVORY2 = 0xC4B890u;  /* lower edge shade   */
static constexpr unsigned int KEY_EBONY  = 0x14100Cu;
static constexpr unsigned int KEY_LIT    = 0xFF8A30u;  /* struck key (ember) */
static constexpr unsigned int KEY_EDGE   = 0x241C0Cu;

static int rc_slen(const char* s) { int n = 0; while (s[n]) ++n; return n; }

static bool rc_spawn(void (*fn)(void))
{
    return Kernel::Scheduler::instance().create_thread(fn) != nullptr;
}

/* physical key (AZERTY char) → semitone 0..24, or -1 */
static int rc_key_semi(int c)
{
    switch (c) {
        case 'q': case 'Q': return 0;   case 'z': case 'Z': return 1;
        case 's': case 'S': return 2;   case 'e': case 'E': return 3;
        case 'd': case 'D': return 4;
        case 'f': case 'F': return 5;   case 't': case 'T': return 6;
        case 'g': case 'G': return 7;   case 'y': case 'Y': return 8;
        case 'h': case 'H': return 9;   case 'u': case 'U': return 10;
        case 'j': case 'J': return 11;
        case 'k': case 'K': return 12;
    }
    return -1;
}

/* digit row → song 0..3. On AZERTY the UNSHIFTED digit row produces
   & é " ' — accept those too, so the footer's “[1-4]” is honest.          */
static int rc_key_song(int c)
{
    switch (c) {
        case '1': case '&':          return 0;
        case '2': case 0xE9:         return 1;   /* é */
        case '3': case '"':          return 2;
        case '4': case '\'':         return 3;
    }
    return -1;
}

/* white-key index preceding a black semitone (shared draw/hit geometry) */
static int rc_white_before(int black_semi)
{
    for (int i = 0; i < RC_NWHITE; ++i)
        if (RC_WHITE_SEMI[i] == black_semi - 1) return i;
    return -1;
}

/* ── the sound owner thread (melodies AND single key notes) ─────────────── */
static void rc_melody_thread()
{
    /* Atomically consume the spawn ticket. s_rc_gen is pre-incremented before
       every spawn, so a live ticket is never 0; a cancel issued between spawn
       and this first run has bumped s_rc_gen past the ticket → exit silently.
       Consuming (=0) guarantees one thread per ticket even after two rapid
       spawns: the loser finds 0 and exits before touching the speaker.      */
    asm volatile("cli");
    unsigned my  = s_rc_ticket_gen;
    int      idx = s_rc_ticket_song;
    s_rc_ticket_gen = 0;
    if (my == 0 || s_rc_gen != my ||
        idx < 0 || idx > RC_SONG_COUNT) { asm volatile("sti"); return; }
    asm volatile("sti");

    const rc_song* sg = (idx == RC_SONG_COUNT) ? &s_rc_keysong : &RC_SONGS[idx];

    for (int n = 0; n < sg->len; ++n) {
        /* atomically: still the owner? then program the speaker. The cli/sti
           pair prevents this thread, if cancelled while preempted right here,
           from re-arming a stale note over the canceller's new sound.        */
        asm volatile("cli");
        if (s_rc_gen != my) { asm volatile("sti"); return; }
        if (sg->notes[n].hz) timer_speaker_on(sg->notes[n].hz);
        else                 timer_speaker_off();
        asm volatile("sti");

        unsigned long until = jiffies + sg->notes[n].dur_j;
        while ((long)(jiffies - until) < 0)          /* wrap-safe */
            if (s_rc_gen != my) return;      /* cancelled mid-note: the
                                                canceller owns the speaker */
    }

    asm volatile("cli");
    if (s_rc_gen == my) {                    /* natural end, still owner */
        timer_speaker_off();
        s_rc_playing = -1;
    }
    asm volatile("sti");
}

/* ── sound control (window side, main thread) ────────────────────────────── */
void RuneChantWindow::stop_all()
{
    ++s_rc_gen;                              /* cancel thread / pending ticket */
    s_rc_ticket_gen = 0;
    s_rc_playing    = -1;
    timer_speaker_off();
    m_note_off_j  = 0;
    m_active_semi = -1;
}

void RuneChantWindow::play_note(int semi)
{
    if (semi < 0 || semi >= RC_NOTE_COUNT) return;
    ++s_rc_gen;                              /* a key silences any melody */
    s_rc_playing = -1;
    timer_speaker_off();                     /* we are the canceller      */

    s_rc_keynote.hz    = RC_NOTE_HZ[semi];
    s_rc_keynote.dur_j = 16;                 /* ~160 ms                   */
    s_rc_ticket_gen  = s_rc_gen;
    s_rc_ticket_song = RC_SONG_COUNT;        /* pseudo-index: key note    */
    if (!rc_spawn(rc_melody_thread)) {       /* no slot: silent no-op     */
        s_rc_ticket_gen = 0;
        return;
    }
    m_active_semi = semi;                    /* cosmetic highlight only   */
    m_note_off_j  = jiffies + 16;
}

void RuneChantWindow::start_song(int idx)
{
    if (idx < 0 || idx >= RC_SONG_COUNT) return;
    ++s_rc_gen;                              /* cancel whatever plays */
    m_note_off_j  = 0;
    m_active_semi = -1;
    s_rc_playing  = idx;                     /* instant status feedback */
    s_rc_ticket_gen  = s_rc_gen;
    s_rc_ticket_song = idx;
    if (!rc_spawn(rc_melody_thread))         /* no owner will ever exist: */
        stop_all();                          /* silence + clear 'playing' */
}

void RuneChantWindow::on_removed()
{
    stop_all();                              /* GUARANTEED silence on close */
    if (s_rc_live == this) s_rc_live = nullptr;
}

/* ── input ───────────────────────────────────────────────────────────────── */
bool RuneChantWindow::handle_char(int c)
{
    if (c == KEY_ESC) {
        if (s_rc_playing >= 0) { stop_all(); return true; }
        request_close();
        return true;
    }
    int song = rc_key_song(c);
    if (song >= 0) { start_song(song); return true; }

    int semi = rc_key_semi(c);
    if (semi >= 0) { play_note(semi); return true; }
    return false;
}

bool RuneChantWindow::on_event(const Event& e)
{
    if (e.type == EVT_MOUSE_BUTTON && (e.mouse.buttons & 0x01) &&
        layout_fits() && hit_test(e.mouse.x, e.mouse.y)) {
        int semi = semi_at(e.mouse.x, e.mouse.y);
        if (semi >= 0) { play_note(semi); return true; }
        int b = btn_at(e.mouse.x, e.mouse.y);
        if (b >= 0) { start_song(b); return true; }
    }
    return Window::on_event(e);
}

/* ── geometry / hit-testing (shared with drawing) ────────────────────────── */
int RuneChantWindow::semi_at(int px, int py) const
{
    int rx = px - piano_x(), ry = py - piano_y();
    if (rx < 0 || ry < 0 || rx >= RC_NWHITE * WKW || ry >= WKH) return -1;

    if (ry < BKH)                            /* black keys overlay the whites */
        for (int b = 0; b < RC_NBLACK; ++b) {
            int wi = rc_white_before(RC_BLACK_SEMI[b]);
            int bx0 = (wi + 1) * WKW - BKW / 2;
            if (rx >= bx0 && rx < bx0 + BKW) return RC_BLACK_SEMI[b];
        }
    int wi = rx / WKW;
    if (wi >= RC_NWHITE) return -1;
    return RC_WHITE_SEMI[wi];
}

int RuneChantWindow::btn_at(int px, int py) const
{
    int bw = (RC_NWHITE * WKW - 6 * (RC_SONG_COUNT - 1)) / RC_SONG_COUNT;
    int ry = py - btn_y();
    if (ry < 0 || ry >= BTN_H) return -1;
    for (int i = 0; i < RC_SONG_COUNT; ++i) {
        int bx0 = piano_x() + i * (bw + 6);
        if (px >= bx0 && px < bx0 + bw) return i;
    }
    return -1;
}

/* ── rendering ───────────────────────────────────────────────────────────── */
void RuneChantWindow::draw_status() const
{
    Graphics& g = Graphics::instance();
    int sy = client_y() + 4;
    int playing = s_rc_playing;              /* single snapshot: the thread
                                                can flip it at any moment   */
    if (playing >= 0 && playing < RC_SONG_COUNT) {
        char buf[48]; char* p = buf;
        *p++ = 0x0E; *p++ = ' ';             /* CP437 beamed-note glyph */
        const char* n = RC_SONGS[playing].name;
        while (*n && p < buf + 47) *p++ = *n++;
        *p = 0;
        g.draw_str(client_x() + MARGIN, sy + 4, buf, 0xFF8A30u, GFX_TRANSPARENT);
    } else {
        g.draw_str(client_x() + MARGIN, sy + 4, "Pret a chanter",
                   Theme::AMBER, GFX_TRANSPARENT);
    }
    const char* hint = "[Echap] Stop / Quitter";
    g.draw_str(client_x() + client_w() - MARGIN - rc_slen(hint) * 8, sy + 4,
               hint, Theme::ASH, GFX_TRANSPARENT);
}

void RuneChantWindow::draw_piano() const
{
    Graphics& g = Graphics::instance();
    int bx = piano_x(), by = piano_y();
    static const char WHITE_LBL[9] = "QSDFGHJK";   /* first octave only */

    for (int i = 0; i < RC_NWHITE; ++i) {
        int x = bx + i * WKW;
        bool lit = (m_active_semi == RC_WHITE_SEMI[i]);
        g.fill_rect(x, by, WKW - 1, WKH, lit ? KEY_LIT : KEY_IVORY);
        if (!lit) g.fill_rect(x, by + WKH - 14, WKW - 1, 14, KEY_IVORY2);
        g.draw_rect(x, by, WKW - 1, WKH, KEY_EDGE);
        if (i < 8)
            g.draw_char(x + (WKW - 9) / 2, by + WKH - 32, WHITE_LBL[i],
                        0x6B5223u, GFX_TRANSPARENT);
    }
    for (int b = 0; b < RC_NBLACK; ++b) {
        int wi = rc_white_before(RC_BLACK_SEMI[b]);
        int x  = bx + (wi + 1) * WKW - BKW / 2;
        bool lit = (m_active_semi == RC_BLACK_SEMI[b]);
        g.fill_rect(x, by, BKW, BKH, lit ? KEY_LIT : KEY_EBONY);
        g.draw_rect(x, by, BKW, BKH, Theme::GOLD_DEEP);
    }
    g.draw_rect(bx - 1, by - 1, RC_NWHITE * WKW + 1, WKH + 2, Theme::GOLD_DEEP);
}

void RuneChantWindow::draw_buttons() const
{
    Graphics& g = Graphics::instance();
    int playing = s_rc_playing;              /* single snapshot */
    int bw = (RC_NWHITE * WKW - 6 * (RC_SONG_COUNT - 1)) / RC_SONG_COUNT;
    for (int i = 0; i < RC_SONG_COUNT; ++i) {
        int x = piano_x() + i * (bw + 6), y = btn_y();
        bool on = (playing == i);
        g.fill_rect(x, y, bw, BTN_H, on ? 0x2A1408u : 0x141006u);
        g.draw_rect(x, y, bw, BTN_H, on ? KEY_LIT : Theme::GOLD_DEEP);
        char lbl[24]; char* p = lbl;
        *p++ = (char)('1' + i); *p++ = ' ';
        const char* n = RC_SONGS[i].name;
        while (*n && p < lbl + 22) *p++ = *n++;   /* truncated to fit */
        *p = 0;
        int max_chars = (bw - 8) / 8;
        if (rc_slen(lbl) > max_chars) lbl[max_chars] = 0;
        g.draw_str(x + 4, y + (BTN_H - 16) / 2, lbl,
                   on ? 0xFFB060u : Theme::AMBER, GFX_TRANSPARENT);
    }
}

void RuneChantWindow::draw()
{
    Window::draw();
    Graphics& g = Graphics::instance();
    g.fill_rect(client_x(), client_y(), client_w(), client_h(), 0x0A0806u);

    /* fade the struck-key highlight — purely cosmetic: the speaker itself is
       owned and released by the sound thread, never by the renderer         */
    if (m_active_semi >= 0 && (long)(jiffies - m_note_off_j) >= 0) {
        m_active_semi = -1;
        m_note_off_j  = 0;
    }

    /* fixed layout: don't paint past a user-shrunk frame */
    if (!layout_fits()) {
        g.draw_str(client_x() + 6, client_y() + 6, "Trop petit",
                   Theme::ASH, GFX_TRANSPARENT);
        return;
    }

    draw_status();
    draw_piano();
    draw_buttons();

    const char* foot = "Clavier: Q S D F G H J K  +  Z E T Y U   [1-4] Melodies";
    g.draw_str(client_x() + MARGIN, btn_y() + BTN_H + 8, foot,
               Theme::GOLD_DEEP, GFX_TRANSPARENT);
}

/* ── factory ─────────────────────────────────────────────────────────────── */
extern "C" void open_runechant_window(void)
{
    if (s_rc_live) return;                   /* singleton: shared sound state */
    auto win = Greg::make_ref<RuneChantWindow>();
    /* client: 8 + 15*30 + 8 = 466 wide ; 4+24+4 + 110 + 10 + 26 + 8+18+4 = 208
       window: 466+4 = 470 × 208+24 = 232                                     */
    win->setup(180, 140, 470, 232, "Chant Runique", 0x0A0806u);
    s_rc_live = win.ptr();
    WindowManager::instance().add_window(Greg::move(win));
}
