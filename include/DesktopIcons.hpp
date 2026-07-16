#ifndef DESKTOP_ICONS_HPP
#define DESKTOP_ICONS_HPP

/* Single source of truth for desktop icon geometry, shared by the Compositor
   (which draws them) and the WindowManager (which hit-tests clicks). Keeping
   the table here means the two can never desync. `static const` gives each
   translation unit its own identical copy — fine for a read-only table.     */

struct DeskIconDef { const char* label; int x; int y; };

static const int DESK_ICON_W    = 48;
static const int DESK_ICON_H    = 48;
static const int DESK_ICON_LBLH = 16;
static const int DESK_ICON_COUNT = 10;

/* Two columns (x=14, x=94); rows spaced 75px from y=20. */
static const DeskIconDef DESK_ICONS[DESK_ICON_COUNT] = {
    { "Terminal", 14,  20 },   /* 0 */
    { "Fichiers", 14,  95 },   /* 1 */
    { "GregNet",  14, 170 },   /* 2 */
    { "Systeme",  14, 245 },   /* 3 */
    { "Casino",   14, 320 },   /* 4 */
    { "Jeux",     94,  20 },   /* 5 */
    { "Calc",     94,  95 },   /* 6 */
    { "Paint",    94, 170 },   /* 7 */
    { "Horloge",  94, 245 },   /* 8 */
    { "Demineur", 94, 320 },   /* 9 */
};

#endif /* DESKTOP_ICONS_HPP */
