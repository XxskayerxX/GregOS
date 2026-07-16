/* DungeonWindow — « Le Donjon de Drakkar » for GregOS.
   Turn-based roguelike. Procedurally-generated floors (rooms + corridors),
   bump-to-attack combat, gold/potions, descend to face Drakkar's hoard.
   Deterministic xorshift RNG (jiffies-seeded), no libc, no allocation.       */

#include "../include/DungeonWindow.hpp"
#include "../include/WindowManager.hpp"
#include "../include/Graphics.hpp"
#include "../include/Theme.hpp"
#include "../include/gfx.h"
#include "../include/keyboard.h"

extern "C" volatile unsigned long jiffies;

/* ── tiny freestanding string helpers ─────────────────────────────────────── */
static int d_slen(const char* s) { int n = 0; while (s[n]) ++n; return n; }
static char* d_puts(char* p, const char* s) { while (*s) *p++ = *s++; return p; }
static char* d_putn(char* p, int v)
{
    if (v < 0) { *p++ = '-'; v = -v; }
    char tmp[12]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *p++ = tmp[--n];
    return p;
}

/* ── palette ──────────────────────────────────────────────────────────────── */
static constexpr unsigned int MAP_BG    = 0x080604u;   /* client backdrop      */
static constexpr unsigned int CELL_BG   = 0x0A0806u;   /* per-tile background   */
static constexpr unsigned int STATUS_BG = 0x161210u;

/* ── RNG (xorshift32) ─────────────────────────────────────────────────────── */
unsigned int DungeonWindow::rnd()
{
    m_seed ^= m_seed << 13; m_seed ^= m_seed >> 17; m_seed ^= m_seed << 5;
    return m_seed;
}
int DungeonWindow::rnd_range(int lo, int hi)          /* inclusive */
{
    if (hi <= lo) return lo;
    return lo + (int)(rnd() % (unsigned)(hi - lo + 1));
}

/* ── queries ──────────────────────────────────────────────────────────────── */
bool DungeonWindow::passable(int x, int y) const
{
    if (x < 0 || y < 0 || x >= COLS || y >= ROWS) return false;
    return m_tile[y * COLS + x] != WALL;
}
int DungeonWindow::monster_at(int x, int y) const
{
    for (int i = 0; i < m_nmon; ++i)
        if (m_mon[i].alive && m_mon[i].x == x && m_mon[i].y == y) return i;
    return -1;
}
int DungeonWindow::free_floor(int* ox, int* oy)
{
    for (int tries = 0; tries < 400; ++tries) {
        int x = rnd_range(0, COLS - 1), y = rnd_range(0, ROWS - 1);
        if (m_tile[y * COLS + x] != FLOOR)     continue;
        if (x == m_px && y == m_py)            continue;
        if (m_item[y * COLS + x] != ITEM_NONE) continue;
        if (monster_at(x, y) >= 0)             continue;
        *ox = x; *oy = y; return 1;
    }
    return 0;
}

/* ── fog of war ───────────────────────────────────────────────────────────── */
void DungeonWindow::reveal_fov()
{
    for (int y = 0; y < ROWS; ++y)
        for (int x = 0; x < COLS; ++x) {
            int dx = x - m_px, dy = y - m_py;
            if (dx > -FOV_R && dx < FOV_R && dy > -FOV_R && dy < FOV_R)
                m_seen[y * COLS + x] = 1;
        }
}

/* ── procedural generation (rooms + L-corridors → guaranteed connected) ─────── */
void DungeonWindow::gen_level()
{
    for (int i = 0; i < ROWS * COLS; ++i) { m_tile[i] = WALL; m_seen[i] = 0; m_item[i] = ITEM_NONE; }
    m_nmon = 0;

    int nrooms = rnd_range(8, 12);
    int prev_cx = 0, prev_cy = 0, first_cx = 0, first_cy = 0, last_cx = 0, last_cy = 0;
    for (int r = 0; r < nrooms; ++r) {
        int rw = rnd_range(4, 8), rh = rnd_range(4, 6);
        int rx = rnd_range(1, COLS - rw - 2), ry = rnd_range(1, ROWS - rh - 2);
        for (int y = ry; y < ry + rh; ++y)
            for (int x = rx; x < rx + rw; ++x) m_tile[y * COLS + x] = FLOOR;
        int cx = rx + rw / 2, cy = ry + rh / 2;
        if (r == 0) { first_cx = cx; first_cy = cy; }
        else {
            int x0 = cx < prev_cx ? cx : prev_cx, x1 = cx > prev_cx ? cx : prev_cx;
            for (int x = x0; x <= x1; ++x) m_tile[prev_cy * COLS + x] = FLOOR;   /* H */
            int y0 = cy < prev_cy ? cy : prev_cy, y1 = cy > prev_cy ? cy : prev_cy;
            for (int y = y0; y <= y1; ++y) m_tile[y * COLS + cx] = FLOOR;        /* V */
        }
        prev_cx = cx; prev_cy = cy; last_cx = cx; last_cy = cy;
    }
    m_px = first_cx; m_py = first_cy;

    bool final_floor = (m_floor >= MAX_FLOOR);
    if (!final_floor)
        m_tile[last_cy * COLS + last_cx] = STAIRS;
    else {
        /* Drakkar garde le trésor au fond du dernier étage */
        Monster& d = m_mon[m_nmon++];
        d.x = last_cx; d.y = last_cy; d.hp = 30; d.atk = 6; d.glyph = 'D'; d.alive = 1;
        m_item[last_cy * COLS + last_cx] = ITEM_GOLD;
    }

    int want = rnd_range(4, 6 + m_floor);
    for (int k = 0; k < want && m_nmon < MAX_MON; ++k) {
        int mx, my; if (!free_floor(&mx, &my)) break;
        Monster& m = m_mon[m_nmon++];
        m.x = mx; m.y = my; m.alive = 1;
        int t = rnd_range(1, m_floor + 2);          /* étage haut → plus coriace */
        if (t <= 2)      { m.glyph = 'r'; m.hp = 3; m.atk = 1; }
        else if (t <= 4) { m.glyph = 'g'; m.hp = 6; m.atk = 2; }
        else             { m.glyph = 'S'; m.hp = 9; m.atk = 3; }
    }

    int golds = rnd_range(2, 4), pots = rnd_range(1, 2);
    for (int k = 0; k < golds; ++k) { int ix, iy; if (free_floor(&ix, &iy)) m_item[iy * COLS + ix] = ITEM_GOLD; }
    for (int k = 0; k < pots;  ++k) { int ix, iy; if (free_floor(&ix, &iy)) m_item[iy * COLS + ix] = ITEM_POTION; }

    reveal_fov();
}

/* ── lifecycle ────────────────────────────────────────────────────────────── */
void DungeonWindow::new_game()
{
    m_maxhp = 20; m_hp = 20; m_atk = 4; m_gold = 0; m_floor = 1; m_pot = 1;
    m_status = PLAYING; m_turns = 0;
    m_seed = (unsigned)jiffies * 2654435761u + 1u;
    if (m_seed == 0) m_seed = 0x1234567u;
    gen_level();
}
void DungeonWindow::descend()
{
    ++m_floor;
    gen_level();
}
void DungeonWindow::quaff_potion()
{
    if (m_status != PLAYING || m_pot <= 0) return;
    --m_pot;
    m_hp += 8; if (m_hp > m_maxhp) m_hp = m_maxhp;
    ++m_turns;
    monsters_turn();
}

/* ── player / monster turns ───────────────────────────────────────────────── */
void DungeonWindow::step(int dx, int dy)
{
    if (m_status != PLAYING) return;
    int nx = m_px + dx, ny = m_py + dy;

    int mi = monster_at(nx, ny);
    if (mi >= 0) {                                  /* bump = attaque */
        Monster& m = m_mon[mi];
        m.hp -= m_atk + rnd_range(0, 2);
        if (m.hp <= 0) {
            m.alive = 0; m_gold += rnd_range(2, 8);
            if (m.glyph == 'D') { m_status = WON; return; }
        }
    } else if (passable(nx, ny)) {
        m_px = nx; m_py = ny;
        int it = m_item[ny * COLS + nx];
        if (it == ITEM_GOLD)        { m_gold += rnd_range(5, 15); m_item[ny * COLS + nx] = ITEM_NONE; }
        else if (it == ITEM_POTION) { ++m_pot;                    m_item[ny * COLS + nx] = ITEM_NONE; }
        reveal_fov();
        if (tile(nx, ny) == STAIRS && m_floor < MAX_FLOOR) { descend(); return; }
    } else {
        return;                                     /* mur : pas de tour consommé */
    }
    ++m_turns;
    monsters_turn();
}
void DungeonWindow::monsters_turn()
{
    for (int i = 0; i < m_nmon; ++i) {
        Monster& m = m_mon[i];
        if (!m.alive) continue;

        int adx = m_px - m.x, ady = m_py - m.y;
        int aax = adx < 0 ? -adx : adx, aay = ady < 0 ? -ady : ady;

        if (aax > 8 || aay > 8) continue;           /* trop loin → endormi */

        if ((adx == 0 && aay == 1) || (ady == 0 && aax == 1)) {   /* adjacent → frappe */
            m_hp -= m.atk;
            if (m_hp <= 0) { m_hp = 0; m_status = DEAD; return; }
            continue;
        }
        int ddx = (adx > 0) - (adx < 0);
        int ddy = (ady > 0) - (ady < 0);
        if (aax > aay) ddy = 0; else ddx = 0;       /* privilégie l'axe le plus long */
        int tx = m.x + ddx, ty = m.y + ddy;
        if (passable(tx, ty) && monster_at(tx, ty) < 0 && !(tx == m_px && ty == m_py))
            { m.x = tx; m.y = ty; }
    }
}

/* ── input ────────────────────────────────────────────────────────────────── */
bool DungeonWindow::handle_char(int c)
{
    if (c == KEY_ESC)              { request_close(); return true; }
    if (c == 'n' || c == 'N')      { new_game();      return true; }
    if (c == 'p' || c == 'P')      { quaff_potion();  return true; }
    if (m_status != PLAYING)       { return true; }   /* fin de partie : avaler les touches */

    switch (c) {
        case KEY_UP:    case 'z': case 'Z': case 'w': case 'W': step(0, -1); return true;
        case KEY_DOWN:  case 's': case 'S':                     step(0,  1); return true;
        case KEY_LEFT:  case 'q': case 'Q': case 'a': case 'A': step(-1, 0); return true;
        case KEY_RIGHT: case 'd': case 'D':                     step( 1, 0); return true;
    }
    return false;
}
bool DungeonWindow::on_event(const Event& e)
{
    return Window::on_event(e);        /* déplacement au clavier ; souris = chrome fenêtre */
}

/* ── rendering ────────────────────────────────────────────────────────────── */
void DungeonWindow::draw_map() const
{
    Graphics& g = Graphics::instance();
    const int R = FOV_R, ox = map_x(), oy = map_y();

    for (int y = 0; y < ROWS; ++y)
        for (int x = 0; x < COLS; ++x) {
            int idx = y * COLS + x;
            if (!m_seen[idx]) continue;
            int dx = x - m_px, dy = y - m_py;
            bool inview = (dx > -R && dx < R && dy > -R && dy < R);
            int px = ox + x * CELL_W, py = oy + y * CELL_H;
            g.fill_rect(px, py, CELL_W, CELL_H, CELL_BG);

            unsigned char gl; unsigned int fg;
            switch (m_tile[idx]) {
                case WALL:   gl = 0xB1; fg = inview ? 0x6B5E48u : 0x241E18u; break; /* ▒ */
                case DOOR:   gl = '+';  fg = inview ? 0x9A7A3Au : 0x3A2E18u; break;
                case STAIRS: gl = '>';  fg = inview ? 0xFFCC44u : 0x4A3A14u; break;
                default:     gl = 0xFA; fg = inview ? 0x4A4030u : 0x1A160Fu; break; /* · sol */
            }
            if (inview && m_tile[idx] != WALL && m_item[idx] != ITEM_NONE) {
                if (m_item[idx] == ITEM_GOLD) { gl = '$'; fg = 0xFFD24Au; }
                else                          { gl = '!'; fg = 0x50E0C0u; }
            }
            g.draw_char(px, py, gl, fg, GFX_TRANSPARENT);
        }

    for (int i = 0; i < m_nmon; ++i) {
        const Monster& m = m_mon[i];
        if (!m.alive) continue;
        int dx = m.x - m_px, dy = m.y - m_py;
        if (!(dx > -R && dx < R && dy > -R && dy < R)) continue;
        unsigned int fg = (m.glyph == 'D') ? 0xE03020u
                        : (m.glyph == 'S') ? 0xC0A0E0u
                        : (m.glyph == 'g') ? 0x60C060u : 0xC08040u;
        int px = ox + m.x * CELL_W, py = oy + m.y * CELL_H;
        g.fill_rect(px, py, CELL_W, CELL_H, CELL_BG);
        g.draw_char(px, py, m.glyph, fg, GFX_TRANSPARENT);
    }

    g.fill_rect(ox + m_px * CELL_W, oy + m_py * CELL_H, CELL_W, CELL_H, CELL_BG);
    g.draw_char(ox + m_px * CELL_W, oy + m_py * CELL_H, '@', 0xFFD24Au, GFX_TRANSPARENT);
}

void DungeonWindow::draw_status() const
{
    Graphics& g = Graphics::instance();
    int sy = client_y() + ROWS * CELL_H;
    g.fill_rect(client_x(), sy, client_w(), STATUS_H, STATUS_BG);
    g.draw_hline(client_x(), sy, client_w(), Theme::GOLD_DEEP);

    char buf[96]; char* p = buf;
    p = d_puts(p, "PV ");      p = d_putn(p, m_hp); *p++ = '/'; p = d_putn(p, m_maxhp);
    p = d_puts(p, "  Or ");    p = d_putn(p, m_gold);
    p = d_puts(p, "  Etage "); p = d_putn(p, m_floor); *p++ = '/'; p = d_putn(p, MAX_FLOOR);
    p = d_puts(p, "  Potions "); p = d_putn(p, m_pot);
    *p = 0;

    unsigned int hpcol = (m_hp * 3 <= m_maxhp) ? 0xE04030u : 0xE8C060u;
    g.draw_str(client_x() + 6, sy + 3, buf, hpcol, GFX_TRANSPARENT);
}

void DungeonWindow::draw_gameover() const
{
    Graphics& g = Graphics::instance();
    int pw = 300, ph = 90;
    int px = client_x() + (client_w() - pw) / 2;
    int py = client_y() + (ROWS * CELL_H - ph) / 2;
    g.fill_rect(px, py, pw, ph, CELL_BG);
    unsigned int border = (m_status == WON) ? 0xFFCC44u : 0x8B1A1Au;
    g.draw_rect(px, py, pw, ph, border);
    g.draw_rect(px + 1, py + 1, pw - 2, ph - 2, border);

    const char* t1 = (m_status == WON) ? "LE TRESOR DE DRAKKAR EST A VOUS !"
                                       : "LE DONJON VOUS A ENGLOUTI";
    const char* t2 = "[N] Nouvelle expedition   [Echap] Quitter";
    unsigned int c1 = (m_status == WON) ? 0xFFD24Au : 0xE04030u;
    int l1 = d_slen(t1) * 8, l2 = d_slen(t2) * 8;
    g.draw_str(px + (pw - l1) / 2, py + 26, t1, c1, GFX_TRANSPARENT);
    g.draw_str(px + (pw - l2) / 2, py + 52, t2, 0xB0A890u, GFX_TRANSPARENT);
}

void DungeonWindow::draw()
{
    Window::draw();
    Graphics& g = Graphics::instance();

    /* fixed 48×26-tile layout: don't paint past a user-shrunk frame */
    if (client_w() < COLS * CELL_W || client_h() < ROWS * CELL_H + STATUS_H) {
        g.fill_rect(client_x(), client_y(), client_w(), client_h(), MAP_BG);
        g.draw_str(client_x() + 6, client_y() + 6, "Trop petit",
                   0xB0A890u, GFX_TRANSPARENT);
        return;
    }

    g.fill_rect(client_x(), client_y(), client_w(), ROWS * CELL_H, MAP_BG);
    draw_map();
    draw_status();
    if (m_status != PLAYING) draw_gameover();
}

/* ── factory ──────────────────────────────────────────────────────────────── */
extern "C" void open_dungeon_window(void)
{
    auto win = Greg::make_ref<DungeonWindow>();
    /* client_w = COLS*CELL_W = 384 → window 388
       client_h = ROWS*CELL_H + STATUS_H = 416 + 22 = 438 → window ≈ 462 */
    win->setup(120, 40, 388, 462, "Le Donjon de Drakkar", 0x0A0806u);
    WindowManager::instance().add_window(Greg::move(win));
}
