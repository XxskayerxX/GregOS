#ifndef THEME_HPP
#define THEME_HPP

/* ── GregOS Visual Theme — "Tube cathodique norrois" ───────────────────────
   Refonte issue du design « GregOS : tube cathodique norrois » — variante de
   rampe 1c « RÈGNE BICÉPHALE » + langage GUI 2a « COUR DU ROI ».

   Principe : le lore devient le système. Le ROI décrète en AMBRE, la MACHINE
   répond en VERT, l'OR fait le métal (sceaux/cartouches), la BRAISE agit
   (feu de Drakkar / erreurs / suppression). Le tout luit sur une obsidienne
   de tube cathodique. Actif vs inactif = LUMINOSITÉ (HI/base vs DIM/ghost),
   jamais la teinte. Une surface = UNE rampe dominante.

   Contraintes moteur : couleurs 0x00RRGGBB, zéro alpha. Le glow se fake par
   glyphe dédoublé (DIM ±1px + cœur), les scanlines par une passe post-compose.
   Police cible : Px437 IBM VGA 8×16 (CP437).                                */

namespace Theme {

    /* ══ Rampes de phosphore (sombre → brillant) ══════════════════════════ */

    /* AMBRE — la voix du ROI : décrets, titres, taskbar royale. */
    constexpr unsigned int AMBER_DEEP = 0x2A2008u;   /* fond de sélection ambre */
    constexpr unsigned int AMBER_DIM  = 0x8A5E10u;   /* halo / libellés discrets */
    constexpr unsigned int AMBER      = 0xFFB000u;   /* base — texte royal      */
    constexpr unsigned int AMBER_HI   = 0xFFD98Cu;   /* accent brillant         */

    /* VERT — la voix de la MACHINE : terminal, sorties, succès. */
    constexpr unsigned int GREEN_DEEP = 0x0C2013u;
    constexpr unsigned int GREEN_DIM  = 0x1E6B38u;
    constexpr unsigned int GREEN      = 0x36FF74u;
    constexpr unsigned int GREEN_HI   = 0xB6FFC9u;

    /* OR — sceaux, cartouches, √ : le métal du royaume. */
    constexpr unsigned int GOLD_DEEP  = 0x241C0Cu;
    constexpr unsigned int GOLD_DIM   = 0x6E5626u;
    constexpr unsigned int GOLD       = 0xC9A24Bu;
    constexpr unsigned int GOLD_HI    = 0xF0C674u;

    /* BRAISE — feu de Drakkar, erreurs, suppression. */
    constexpr unsigned int EMBER_DEEP = 0x200A04u;
    constexpr unsigned int EMBER_DIM  = 0x7A2A10u;
    constexpr unsigned int EMBER      = 0xFF5A1Fu;
    constexpr unsigned int EMBER_HI   = 0xFFB066u;

    /* SANG — écailles de Drakkar : le rouge sang du dragon (distinct de la
       BRAISE orangée qui, elle, est le FEU). Rampe sombre→vif pour le corps
       du dragon, les accents de danger et l'identité « terminal du royaume ». */
    constexpr unsigned int BLOOD_DEEP = 0x1A0505u;
    constexpr unsigned int BLOOD_DIM  = 0x561212u;
    constexpr unsigned int BLOOD_MID  = 0x8B1A1Au;   /* == BLOOD (compat)       */
    constexpr unsigned int BLOOD_HI   = 0xC83028u;

    /* ══ Couleurs fixes ═══════════════════════════════════════════════════ */
    constexpr unsigned int OBSIDIAN    = 0x0B0E0Cu;  /* fond tube / bureau      */
    constexpr unsigned int GOUFFRE     = 0x050706u;  /* ombre portée (bande)    */
    constexpr unsigned int ASH         = 0x4A5548u;  /* texte muet / filets     */
    constexpr unsigned int TEAL_ARCANE = 0x2AA198u;  /* liens (GregNet)         */
    constexpr unsigned int VELLUM      = 0xE8DCC0u;  /* parchemin (documents)   */
    constexpr unsigned int VELLUM_INK  = 0x2A2416u;  /* encre sur vélin         */
    constexpr unsigned int BLOOD       = 0x8B1A1Au;  /* panique / banqueroute   */

    /* ══ Compat : anciens noms remappés sur la palette phosphore ══════════
       On garde les NOMS historiques pour que tout le chrome (window_manager,
       Compositor, chaque *Window) se re-skinne d'un coup, sans casse.        */

    /* ── Bureau ──────────────────────────────────────────────────────────── */
    constexpr unsigned int DESKTOP          = OBSIDIAN;

    /* ── Chrome de fenêtre ───────────────────────────────────────────────── */
    constexpr unsigned int WIN_BG           = OBSIDIAN;   /* panneau / cadre    */
    constexpr unsigned int WIN_BG_PURE      = 0x070C09u;  /* client sombre      */

    /* ── Cadre "métal du royaume" (ex-biseau Win98) ──────────────────────── */
    constexpr unsigned int BEVEL_OUTER_LT   = GOLD_DIM;   /* arête haut/gauche  */
    constexpr unsigned int BEVEL_INNER_LT   = GOLD;       /* liseré d'or        */
    constexpr unsigned int BEVEL_INNER_DK   = GOUFFRE;    /* ombre interne      */
    constexpr unsigned int BEVEL_OUTER_DK   = GOUFFRE;    /* ombre bas/droite   */

    /* ── Barre de titre ──────────────────────────────────────────────────── */
    constexpr unsigned int TITLE_FOCUS_A    = 0x191408u;  /* actif : ambre sombre */
    constexpr unsigned int TITLE_FOCUS_B    = GOLD_DIM;   /* → or                 */
    constexpr unsigned int TITLE_IDLE_A     = 0x10120Eu;  /* inactif : éteint     */
    constexpr unsigned int TITLE_IDLE_B     = GOLD_DEEP;
    constexpr unsigned int TITLE_FG         = GOLD_HI;    /* titre = or brillant  */

    /* ── Boutons de fenêtre ──────────────────────────────────────────────── */
    constexpr unsigned int BTN_FACE         = 0x12100Au;  /* cartouche sombre    */
    constexpr unsigned int BTN_X            = EMBER;      /* × = braise          */

    /* ── Couleurs de texte ───────────────────────────────────────────────── */
    constexpr unsigned int FG_PRIMARY       = AMBER;      /* texte principal     */
    constexpr unsigned int FG_DIM           = ASH;        /* secondaire          */
    constexpr unsigned int FG_ALERT         = EMBER;      /* erreurs             */
    constexpr unsigned int FG_WHITE         = AMBER_HI;   /* accent clair        */
    constexpr unsigned int FG_MUTED         = ASH;        /* très discret        */
    constexpr unsigned int FG_FOLDER        = AMBER;      /* dossiers (royaume)  */

    /* ── Log ambient du bureau (voix machine = vert) ─────────────────────── */
    constexpr unsigned int SYSLOG_DIM       = GREEN_DIM;
    constexpr unsigned int SYSLOG_BRIGHT    = GREEN;

} /* namespace Theme */

#endif /* THEME_HPP */
