#ifndef RUNECHANT_WINDOW_HPP
#define RUNECHANT_WINDOW_HPP

#include "Window.hpp"

/* ── RuneChantWindow — « Chant Runique » ─────────────────────────────────────
   Le premier son musical de GregOS. Un clavier de deux octaves joue sur le
   haut-parleur PC (PIT canal 2) : à la souris, ou au clavier physique
   (rangée AZERTY : Q S D F G H J K = blanches, Z E T Y U = dieses).
   Quatre melodies du royaume tournent dans un thread ordonnanceur — l'UI ne
   gele jamais. Un seul son a la fois ; annulation par compteur de generation ;
   le haut-parleur est coupe de facon GARANTIE a la fermeture (on_removed).
   Donnees musicales : kernel/runechant_data.c (testees hors-noyau).          */

class RuneChantWindow : public Window {
public:
    RuneChantWindow() = default;

    void draw()                   override;
    bool on_event(const Event& e) override;
    bool handle_char(int c)       override;
    void on_removed()             override;

    static constexpr int WKW    = 30;   /* white key width  */
    static constexpr int WKH    = 110;  /* white key height */
    static constexpr int BKW    = 18;   /* black key width  */
    static constexpr int BKH    = 68;   /* black key height */
    static constexpr int MARGIN = 8;
    static constexpr int HDR_H  = 24;
    static constexpr int BTN_H  = 26;
    static constexpr int FOOT_H = 18;

private:
    int           m_active_semi { -1 }; /* highlighted key (0..24, -1 none) */
    unsigned long m_note_off_j  { 0 };  /* jiffies deadline to silence a key */

    void play_note(int semi);
    void start_song(int idx);
    void stop_all();

    int piano_x() const { return client_x() + MARGIN; }
    int piano_y() const { return client_y() + 4 + HDR_H + 4; }
    int btn_y()   const { return piano_y() + WKH + 10; }

    /* one predicate shared by draw() and on_event() so the size guard and
       the click gate can never drift apart                                 */
    bool layout_fits() const {
        return client_w() >= 2 * MARGIN + 15 * WKW &&
               client_h() >= 4 + HDR_H + 4 + WKH + 10 + BTN_H + 8 + FOOT_H;
    }

    int semi_at(int px, int py) const;  /* screen px → semitone 0..24 or -1 */
    int btn_at(int px, int py)  const;  /* screen px → song 0..3 or -1      */

    void draw_status() const;
    void draw_piano() const;
    void draw_buttons() const;
};

extern "C" void open_runechant_window(void);

#endif /* RUNECHANT_WINDOW_HPP */
