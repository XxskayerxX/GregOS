/* GamesWindow — windowed arcade-game launcher for GregOS.
   Freestanding: no libc, no exceptions.                                   */

#include "../include/GamesWindow.hpp"
#include "../include/WindowManager.hpp"
#include "../include/Graphics.hpp"
#include "../include/Theme.hpp"
#include "../include/gfx.h"
#include "../include/keyboard.h"
#include "../include/tty.h"
#include "../include/GamesApp.hpp"
#include "../include/Kernel/Compositor.hpp"

extern "C" volatile unsigned long jiffies;
extern "C" void launch_arcade_game_async(int n);

/* ── Palette ──────────────────────────────────────────────────────────── */
static constexpr unsigned int GW_BG   = 0x08081Au; /* dark navy             */
static constexpr unsigned int GW_ROW  = 0x0E0E28u; /* alternating row shade */
static constexpr unsigned int GW_SEL  = 0x002090u; /* selection blue        */
static constexpr unsigned int GW_GOLD = 0xFFCC00u; /* key badge gold        */
static constexpr unsigned int GW_DIM  = 0x5050A0u; /* dim text / hints      */
static constexpr unsigned int GW_CYAN = 0x40A0FFu; /* selected name colour  */

/* ── Game table ───────────────────────────────────────────────────────── */
struct GameInfo { const char* key; const char* name; const char* desc; };
static const GameInfo GAMES[GamesWindow::N_GAMES] = {
    { "1", "Snake",       "Serpent classique — mangez les pommes"      },
    { "2", "Tetris",      "Blocs tombants — alignez les lignes"         },
    { "3", "Pong",        "Tennis retro — gagnez le point"              },
    { "4", "Invaders",    "Vague d'aliens — defendez la Terre"          },
    { "5", "Breakout",    "Casse-briques — demolissez le mur"           },
    { "6", "2048",        "Fusion de tuiles — atteignez 2048"           },
    { "7", "Minesweeper", "Demineur — evitez les mines"                 },
    { "8", "Simon",       "Memoire sequentielle — repetez le motif"     },
    { "9", "Matrix",      "Pluie de code — plongez dans la simulation"  },
    { "0", "Clock",       "Horloge ASCII — le temps tourne"             },
    { "K", "Kernel Panic","FPS 3D Doom — volez le kernel, tuez Greg"     },
};

/* ── draw ─────────────────────────────────────────────────────────────── */

void GamesWindow::draw()
{
    Window::draw();

    Graphics& g  = Graphics::instance();
    int cx = client_x(), cy = client_y(), cw = client_w();

    /* Dark client background */
    g.fill_rect(cx, cy, cw, client_h(), GW_BG);

    /* Header strip */
    int hdr_y = cy;
    g.fill_rect(cx, hdr_y, cw, 22, 0x04041Au);
    g.draw_str(cx + 6, hdr_y + 3,
               "Choisissez un jeu", GW_DIM, GFX_TRANSPARENT);
    g.draw_str(cx + cw - 25*8 - 4, hdr_y + 3,
               "[ENTREE] Lancer  [ESC] Fermer", GW_DIM, GFX_TRANSPARENT);
    g.draw_hline(cx, hdr_y + 22, cw, GW_DIM);

    /* Game rows */
    int list_y = hdr_y + 23;
    for (int i = 0; i < N_GAMES; ++i) {
        bool sel = (i == m_selected);
        int  ry  = list_y + i * ROW_H;
        unsigned int bg = sel ? GW_SEL : (i % 2 ? GW_ROW : GW_BG);
        g.fill_rect(cx, ry, cw, ROW_H, bg);

        /* Key badge */
        unsigned int badge_bg = sel ? GW_GOLD : GW_DIM;
        g.fill_rect(cx + 6, ry + 6, 16, 16, badge_bg);
        g.draw_char(cx + 6 + 4, ry + 6, GAMES[i].key[0],
                    sel ? 0x000000u : 0xFFFFFFu, GFX_TRANSPARENT);

        /* Name + description */
        unsigned int name_fg = sel ? GW_CYAN : 0xCCCCEEu;
        unsigned int desc_fg = sel ? 0x88AAFFu : 0x404070u;
        g.draw_str(cx + 28, ry + 3,  GAMES[i].name, name_fg, GFX_TRANSPARENT);
        g.draw_str(cx + 28, ry + 14, GAMES[i].desc, desc_fg, GFX_TRANSPARENT);
    }

    /* Bottom hint */
    int foot_y = list_y + N_GAMES * ROW_H;
    g.fill_rect(cx, foot_y, cw, 16, 0x04041Au);
    g.draw_hline(cx, foot_y, cw, GW_DIM);
    g.draw_str(cx + 4, foot_y + 1,
               "[UP/BAS] Naviguer  [1-9,0] Direct",
               GW_DIM, GFX_TRANSPARENT);
}

/* ── launch ───────────────────────────────────────────────────────────── */

void GamesWindow::launch()
{
    int n = m_selected;
    request_close();

    /* Phase 3: register an ArcadeApp with the Compositor so normal desktop
       rendering is suspended while the game thread owns the framebuffer.  */
    auto app = Greg::make_ref<ArcadeApp>();
    Kernel::Compositor::instance().add_app(Greg::move(app));

    /* Spawn game in its own Scheduler thread — non-blocking. */
    launch_arcade_game_async(n);
}

/* ── handle_char ─────────────────────────────────────────────────────── */

bool GamesWindow::handle_char(int c)
{
    if (c == KEY_ESC || c == 'q') { request_close(); return true; }

    if (c == KEY_UP && m_selected > 0)            { --m_selected; return true; }
    if (c == KEY_DOWN && m_selected < N_GAMES-1)  { ++m_selected; return true; }
    if (c == '\n' || c == '\r')                   { launch();      return true; }

    /* Direct number keys: jump + launch */
    if (c >= '1' && c <= '9') { m_selected = c - '1'; launch(); return true; }
    if (c == '0')              { m_selected = 9;       launch(); return true; }
    if (c == 'k' || c == 'K')  { m_selected = 10;      launch(); return true; }

    return false;
}

/* ── on_event ─────────────────────────────────────────────────────────── */

bool GamesWindow::on_event(const Event& e)
{
    if (e.type == EVT_MOUSE_BUTTON && (e.mouse.buttons & 0x01)) {
        int mx = e.mouse.x, my = e.mouse.y;

        /* Chrome (title / close) → delegate to base */
        if (my < client_y()) return Window::on_event(e);

        int list_top = client_y() + 23;
        int row = (my - list_top) / ROW_H;
        if (my >= list_top && row >= 0 && row < N_GAMES &&
            mx >= client_x() && mx < client_x() + client_w()) {

            /* Double-click detection */
            if (row == m_last_click_r && jiffies - m_last_click_j < 50u) {
                m_last_click_r = -1;
                m_selected = row;
                launch();
            } else {
                m_selected     = row;
                m_last_click_r = row;
                m_last_click_j = jiffies;
            }
            return true;
        }
    }
    return Window::on_event(e);
}

/* ── C bridge ────────────────────────────────────────────────────────── */

extern "C" void open_games_window(void)
{
    static constexpr int W = 420;
    static constexpr int H = Window::TITLE_H + Window::BORDER_W
                           + 23 + GamesWindow::N_GAMES * GamesWindow::ROW_H + 16;

    auto win = Greg::make_ref<GamesWindow>();
    int sw = gfx_width(), sh = gfx_height();
    int wx = (sw - W) / 2;
    int wy = (sh - H) / 2;
    if (wx < 0) wx = 0;
    if (wy < 0) wy = 0;
    win->setup(wx, wy, W, H, "Arcade GregOS", GW_BG);
    win->set_focused(true);
    WindowManager::instance().add_window(Greg::move(win));
}
