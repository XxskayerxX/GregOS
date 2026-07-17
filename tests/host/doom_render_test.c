/* doom_render_test.c — host render harness for KERNEL PANIC.

   Compiles the EXACT kernel game code (kernel/doom.c) natively, then:
     1. dumps every generated sprite as a tile sheet (verifies gen_sprites),
     2. renders one first-person frame with the boss (Greg + Drakkar) and an
        enemy in view (verifies billboard projection, depth sort, z-occlusion).

   Same discipline as dungeon_gen_test.c / png_test.c: zero drift between the
   tested code and the embedded code — we include the real translation unit.

   Build:  cc -O2 -w -o /tmp/drt tests/host/doom_render_test.c && /tmp/drt
   Out:    /tmp/doom_sprites.ppm  /tmp/doom_scene.ppm                        */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Stubs for the kernel externs doom.c links against ───────────────────── */
volatile unsigned long jiffies = 0;
void kb_poll_all(void) {}
int  kb_scan_down(int i) { (void)i; return 0; }
void kb_keystate_reset(void) {}
void kb_game_capture(int on) { (void)on; }
void ps2mouse_set_capture(int on) { (void)on; }
void ps2mouse_take_rel(int* dx, int* dy) { *dx = 0; *dy = 0; }
int  ps2mouse_buttons(void) { return 0; }

/* PC-speaker bridge: count writes so the SFX engine can be unit-tested. */
static int g_spk_on_calls, g_spk_off_calls, g_spk_last_hz;
void timer_speaker_on(unsigned int hz) { g_spk_on_calls++; g_spk_last_hz = (int)hz; }
void timer_speaker_off(void) { g_spk_off_calls++; g_spk_last_hz = 0; }

/* gfx back buffer + stubs (only render3d/floorcast/render_sprites are called;
   they write to the internal s_view, never touching these — but they must link). */
static unsigned int g_back[800 * 600];
unsigned int* gfx_get_backbuffer(void) { return g_back; }
int  gfx_width(void)  { return 800; }
int  gfx_height(void) { return 600; }
void gfx_put_pixel(int x, int y, unsigned int c) { (void)x;(void)y;(void)c; }
void gfx_fill_rect(int x,int y,int w,int h,unsigned int c){(void)x;(void)y;(void)w;(void)h;(void)c;}
void gfx_draw_hline(int x,int y,int l,unsigned int c){(void)x;(void)y;(void)l;(void)c;}
void gfx_draw_vline(int x,int y,int l,unsigned int c){(void)x;(void)y;(void)l;(void)c;}
void gfx_draw_rect(int x,int y,int w,int h,unsigned int c){(void)x;(void)y;(void)w;(void)h;(void)c;}
void gfx_draw_char(int x,int y,unsigned char c,unsigned int f,unsigned int b){(void)x;(void)y;(void)c;(void)f;(void)b;}
void gfx_draw_str(int x,int y,const char*s,unsigned int f,unsigned int b){(void)x;(void)y;(void)s;(void)f;(void)b;}
void gfx_swap_buffers(void) {}

/* Pull in the real game code (all statics become visible below). */
#include "../../kernel/doom.c"

/* ── PPM helpers ─────────────────────────────────────────────────────────── */
static void write_ppm(const char* path, const unsigned int* px, int w, int h)
{
    FILE* f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) {
        unsigned c = px[i];
        unsigned char rgb[3] = { (c >> 16) & 255, (c >> 8) & 255, c & 255 };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* Flood-fill the ACTUAL embedded MAP via is_wall(); returns 1 if (tx,ty) is
   reachable from the spawn cell (1,1) through open cells. */
static char g_seen[MAP_H][MAP_W];
static int map_reachable(int tx, int ty)
{
    for (int y = 0; y < MAP_H; ++y) for (int x = 0; x < MAP_W; ++x) g_seen[y][x] = 0;
    int qx[MAP_W*MAP_H], qy[MAP_W*MAP_H], h = 0, t = 0;
    qx[t] = 1; qy[t] = 1; t++; g_seen[1][1] = 1;
    while (h < t) {
        int x = qx[h], y = qy[h]; h++;
        static const int dx[4] = {1,-1,0,0}, dy[4] = {0,0,1,-1};
        for (int k = 0; k < 4; ++k) {
            int nx = x+dx[k], ny = y+dy[k];
            if (nx>=0 && ny>=0 && nx<MAP_W && ny<MAP_H && !g_seen[ny][nx] && !is_wall(nx,ny)) {
                g_seen[ny][nx] = 1; qx[t]=nx; qy[t]=ny; t++;
            }
        }
    }
    return g_seen[ty][tx];
}

int main(void)
{
    gen_textures();
    gen_sprites();

    /* 0) Embedded-map connectivity: spawn -> kernel pedestal -> arena. */
    if (is_wall(1,1))               { printf("FAIL: spawn (1,1) is a wall\n"); return 1; }
    if (is_wall(s_kcx, s_kcy))      { printf("FAIL: kernel cell is a wall\n"); return 1; }
    if (!map_reachable(s_kcx,s_kcy)){ printf("FAIL: kernel unreachable from spawn\n"); return 1; }
    if (!map_reachable(20,20))      { printf("FAIL: arena (20,20) unreachable\n"); return 1; }
    printf("map connectivity OK: spawn(1,1) -> kernel(%d,%d) -> arena(20,20)\n", s_kcx, s_kcy);

    /* 1) Sprite sheet: 11 tiles side by side, 64x64 each. */
    const unsigned int* tiles[] = {
        s_spr[ET_SENTINEL], s_spr[ET_RUNEUR], s_spr[ET_GOLEM],
        s_spr[ET_SPECTRE],  s_spr[ET_CORBEAU], s_spr_kernel,
        s_spr_drak, s_spr_greg, s_spr_ammo, s_spr_ram, s_spr_bolt,
    };
    int nt = sizeof(tiles) / sizeof(tiles[0]);
    int SW = nt * TEXSZ, SH = TEXSZ;
    unsigned int* sheet = malloc(SW * SH * sizeof(unsigned int));
    for (int t = 0; t < nt; ++t)
        for (int y = 0; y < TEXSZ; ++y)
            for (int x = 0; x < TEXSZ; ++x)
                sheet[y * SW + t * TEXSZ + x] = tiles[t][y * TEXSZ + x];
    write_ppm("/tmp/doom_sprites.ppm", sheet, SW, SH);
    free(sheet);

    /* 2) First-person scene: player in the open corridor at row y=9, facing
          east (+x); Drakkar far-right, Greg near-left, a Golem farther back. */
    px = 5 * ONE + ONE / 2;
    py = 9 * ONE + ONE / 2;
    pa = 0;

    s_stolen = 1;                       /* kernel already taken → boss active */
    s_bphase = 1;
    s_drakhp = DRAK_HP; s_greghp = GREG_HP;
    s_drakhurt = 0; s_greghurt = 0;
    s_gregx = 7  * ONE + ONE / 2; s_gregy = 9 * ONE + ONE / 2 - ONE / 4;
    s_drakx = 10 * ONE + ONE / 2; s_draky = 9 * ONE + ONE / 2 + ONE / 4;

    s_nenem = 0;
    for (int i = 0; i < MAXPROJ; ++i) s_proj[i].alive = 0;
    spawn_enemy(13, 9, ET_GOLEM);       /* farther down the corridor */

    floorcast();
    render3d();
    render_sprites();
    write_ppm("/tmp/doom_scene.ppm", s_view, RW, RH);

    /* 2b) One clean frame per zone to eyeball wall theme + floor/ceiling tint. */
    s_bphase = 0; s_stolen = 1; s_nenem = 0; s_npick = 0;
    for (int i = 0; i < MAXPROJ; ++i) s_proj[i].alive = 0;
    struct { int cx, cy, a; const char* f; } shots[3] = {
        { 4,  9,  0,   "/tmp/doom_zone1.ppm" },   /* Pare-feu — circuit/green */
        { 11, 11, 0,   "/tmp/doom_zone2.ppm" },   /* Kernel   — rune/amber    */
        { 17, 13, 256, "/tmp/doom_zone3.ppm" },   /* Arene    — blood/red     */
    };
    for (int s = 0; s < 3; ++s) {
        px = shots[s].cx*ONE + ONE/2; py = shots[s].cy*ONE + ONE/2; pa = shots[s].a;
        floorcast(); render3d(); render_sprites();
        write_ppm(shots[s].f, s_view, RW, RH);
    }

    /* Sanity: the boss sprites must contain non-transparent pixels. */
    int drak_px = 0, greg_px = 0, kern_px = 0;
    for (int i = 0; i < TEXSZ * TEXSZ; ++i) {
        if (s_spr_drak[i]   != TRANSP) drak_px++;
        if (s_spr_greg[i]   != TRANSP) greg_px++;
        if (s_spr_kernel[i] != TRANSP) kern_px++;
    }
    printf("Drakkar pixels: %d\n", drak_px);
    printf("Greg pixels:    %d\n", greg_px);
    printf("Kernel pixels:  %d\n", kern_px);
    printf("wrote /tmp/doom_sprites.ppm (%dx%d) and /tmp/doom_scene.ppm (%dx%d)\n",
           SW, SH, RW, RH);
    if (drak_px < 200 || greg_px < 200 || kern_px < 50) {
        printf("FAIL: a boss/objective sprite is (near-)empty\n");
        return 1;
    }

    /* 3) Mission-spine logic sim: steal → phase 1 (Drakkar) → phase 2 (Greg)
          → win. Player stands one cell west of the pedestal facing east; the
          SIGKILL blade (infinite ammo, 1.5-cell reach) hits whatever boss is
          active. Player made invincible to isolate the boss-death path.       */
    s_hp = 1000000; s_dead = 0; s_won = 0; s_stolen = 0; s_bphase = 0; s_kills = 0;
    s_weap = W_LAME; s_wcool = 0;
    s_nenem = 0;
    for (int i = 0; i < MAXPROJ; ++i) s_proj[i].alive = 0;
    px = (s_kcx + 1) * ONE + ONE / 2;   /* one cell EAST of the kernel (open, LOS) */
    py = s_kcy * ONE + ONE / 2;
    pa = ANG / 2;                        /* face west, toward the pedestal/boss */

    /* Before stealing: the boss must not exist and firing must do nothing. */
    fire_weapon();
    if (s_bphase != 0) { printf("FAIL: boss active before steal\n"); return 1; }

    steal_kernel();
    if (!s_stolen || s_bphase != 1 || s_drakhp != DRAK_HP) {
        printf("FAIL: steal did not summon Drakkar (phase 1)\n"); return 1;
    }

    int saw_p1 = 0, saw_p2 = 0, drak_fell_step = -1;
    for (int step = 0; step < 4000 && !s_won; ++step) {
        if (s_bphase == 1) saw_p1 = 1;
        if (s_bphase == 2 && drak_fell_step < 0) { saw_p2 = 1; drak_fell_step = step; }
        s_wcool = 0;
        fire_weapon();
        boss_update();
    }

    printf("sim: bphase=%d won=%d drakhp=%d greghp=%d saw_p1=%d saw_p2=%d fell@%d\n",
           s_bphase, s_won, s_drakhp, s_greghp, saw_p1, saw_p2, drak_fell_step);
    if (!saw_p1 || !saw_p2 || !s_won || s_bphase != 3) {
        printf("FAIL: mission spine did not reach victory through both phases\n");
        return 1;
    }
    if (s_drakhp > 0) { printf("FAIL: Drakkar survived phase 2\n"); return 1; }

    /* 4) rm-rf AoE must also wound the boss (separate code path from hitscan). */
    s_stolen = 0; s_bphase = 0; s_won = 0;
    steal_kernel();
    int hp_before = s_drakhp;
    s_weap = W_RMRF; s_wcool = 0; s_ammo_charge = 5;
    fire_weapon();                       /* boss is ~1 cell away, inside blast */
    if (s_drakhp >= hp_before) { printf("FAIL: rm-rf did not damage the boss\n"); return 1; }
    printf("rm-rf: drakhp %d -> %d (blast hit)\n", hp_before, s_drakhp);

    /* 5) Regression: the killing blow on Greg must not also flag death when the
          phase-2 aura tick / a stray hit would drop us the SAME frame — win wins.
          Mirror the main loop order: fire_weapon() -> boss_update() -> enemy_ai(). */
    s_won = 0; s_dead = 0; s_bphase = 2; s_kills = 0;
    s_greghp = 1;                       /* one blade hit from dead */
    s_hp = 1;                           /* one aura tick from dead */
    s_gregc = 5;                        /* don't matter — no volley needed */
    s_nenem = 0;
    for (int i = 0; i < MAXPROJ; ++i) s_proj[i].alive = 0;
    px = 5 * ONE + ONE / 2; py = 9 * ONE + ONE / 2; pa = 0;   /* facing east */
    s_gregx = px + ONE; s_gregy = py;   /* 1 cell ahead: in melee + aura range */
    s_weap = W_LAME; s_wcool = 0;
    jiffies = 0;                        /* (jiffies & 7) == 0 -> aura tick fires */
    fire_weapon();                      /* SIGKILL blade finishes Greg */
    boss_update();                      /* phase-2 aura ticks, then win check */
    enemy_ai();                         /* would have set s_dead on hp<=0 */
    printf("win/death excl: won=%d dead=%d hp=%d greghp=%d bphase=%d\n",
           s_won, s_dead, s_hp, s_greghp, s_bphase);
    if (!s_won || s_dead) {
        printf("FAIL: win and death both set (overlapping overlays)\n"); return 1;
    }

    /* 6) Pickups: RAM chip restores VIE (cap 100), signal cell refills ammo,
          each consumed exactly once. */
    s_npick = 0; s_pickmsg = 0;
    s_hp = 40; s_ammo_bul = 5; s_ammo_shell = 2;
    px = 8 * ONE + ONE/2; py = 8 * ONE + ONE/2;
    spawn_pickup(8, 8, PK_HEALTH);   /* both sit on the player */
    spawn_pickup(8, 8, PK_AMMO);
    update_pickups();
    int hp_after = s_hp, bul_after = s_ammo_bul;
    update_pickups();                /* already consumed → must not double-apply */
    printf("pickups: hp 40->%d bul 5->%d alive0=%d alive1=%d\n",
           hp_after, bul_after, s_pick[0].alive, s_pick[1].alive);
    if (hp_after != 65 || bul_after != 35 || s_ammo_bul != 35 ||
        s_pick[0].alive || s_pick[1].alive) {
        printf("FAIL: pickup effect/consumption wrong\n"); return 1;
    }
    s_npick = 0; s_hp = 90; px = 3*ONE+ONE/2; py = 3*ONE+ONE/2;
    spawn_pickup(3, 3, PK_HEALTH); update_pickups();
    if (s_hp != 100) { printf("FAIL: health cap wrong (%d)\n", s_hp); return 1; }
    printf("pickup health cap OK (90+25 -> %d)\n", s_hp);

    /* 7) SFX engine: segment sequencing, priority interrupt, guaranteed-off. */
    s_sfx_seq = 0; s_sfx_prio = 0; g_spk_last_hz = 0;
    sfx_play(SFX_FIRE);                          /* seg0=760/1, seg1=360/1 */
    if (g_spk_last_hz != 760) { printf("FAIL: fire seg0 hz=%d\n", g_spk_last_hz); return 1; }
    sfx_tick();                                  /* -> seg1 */
    if (g_spk_last_hz != 360) { printf("FAIL: fire seg1 hz=%d\n", g_spk_last_hz); return 1; }
    sfx_tick();                                  /* -> end -> speaker off */
    if (s_sfx_seq != 0 || g_spk_last_hz != 0) { printf("FAIL: fire not ended/off\n"); return 1; }
    sfx_play(SFX_FIRE);                          /* prio 1 */
    sfx_play(SFX_DEATH);                         /* prio 5 interrupts */
    if (s_sfx_prio != 5) { printf("FAIL: death(5) didn't interrupt fire(1)\n"); return 1; }
    sfx_play(SFX_FIRE);                          /* prio 1 must be ignored */
    if (s_sfx_prio != 5) { printf("FAIL: fire(1) wrongly interrupted death(5)\n"); return 1; }
    sfx_stop();
    if (s_sfx_seq != 0 || g_spk_last_hz != 0) { printf("FAIL: sfx_stop left sound on\n"); return 1; }
    printf("sfx: sequencing + priority + stop OK\n");

    /* 8) Variable-height identity: the multi-hit stacking path with EVERY cell
          height 1 must reproduce the single-hit fast path pixel-for-pixel (this
          proves the stacking + ceilRow math reduces correctly, not merely that
          the s_has_tall gate skips it). Checked from several viewpoints. */
    {
        s_stolen = 1; s_bphase = 0; s_nenem = 0; s_npick = 0;
        for (int i = 0; i < MAXPROJ; ++i) s_proj[i].alive = 0;
        for (int i = 0; i < MAP_W*MAP_H; ++i) s_wallh[i] = MAP[i] ? 1 : 0;
        static unsigned int A[RW*RH], B[RW*RH];
        int views[6][3] = { {5,9,0},{5,9,300},{9,9,512},{18,9,0},{3,3,700},{11,11,256} };
        int total_diff = 0;
        for (int v = 0; v < 6; ++v) {
            px = views[v][0]*ONE+ONE/2; py = views[v][1]*ONE+ONE/2; pa = views[v][2];
            s_has_tall = 0; floorcast(); render3d();
            for (int i = 0; i < RW*RH; ++i) A[i] = s_view[i];
            s_has_tall = 1; floorcast(); render3d();
            for (int i = 0; i < RW*RH; ++i) B[i] = s_view[i];
            int d = 0; for (int i = 0; i < RW*RH; ++i) if (A[i] != B[i]) d++;
            total_diff += d;
        }
        printf("height identity: %d differing pixels across 6 views (want 0)\n", total_diff);
        if (total_diff) { printf("FAIL: stacking != single-hit at height 1\n"); return 1; }
    }

    /* 10) Regression (review finding): a health chip is applied BEFORE the lethal
           blow, so it can actually save you and never heals a corpse
           (invariant s_dead ⇒ s_hp<=0). Mirrors the loop order used in the game:
           update_pickups() → boss_update() → enemy_ai(). */
    s_won = 0; s_dead = 0; s_bphase = 0; s_stolen = 1;
    s_nenem = 0; s_npick = 0;
    for (int i = 0; i < MAXPROJ; ++i) s_proj[i].alive = 0;
    px = 585 * ONE / 100; py = 9 * ONE + ONE/2;    /* ~5.85 cells, row 9 corridor */
    spawn_enemy(6, 9, ET_SENTINEL); s_enem[0].cool = 0;  /* adjacent, melee-ready */
    s_hp = 5;
    spawn_pickup(5, 9, PK_HEALTH);                 /* chip on the player's cell */
    update_pickups(); boss_update(); enemy_ai();
    printf("heal-before-death: hp=%d dead=%d (chip must save)\n", s_hp, s_dead);
    if (s_dead || s_hp <= 0) { printf("FAIL: chip did not save (healed corpse / wasted save)\n"); return 1; }

    s_dead = 0; s_nenem = 0; s_npick = 0; s_hp = 5;      /* control: no chip → lethal */
    spawn_enemy(6, 9, ET_SENTINEL); s_enem[0].cool = 0;
    update_pickups(); boss_update(); enemy_ai();
    printf("control (no chip):   hp=%d dead=%d\n", s_hp, s_dead);
    if (!s_dead || s_hp != 0) { printf("FAIL: control should die with hp==0\n"); return 1; }

    /* 9) A frame with the authored tall walls, for eyeballing the verticality. */
    init_wall_heights();
    if (!s_has_tall) { printf("FAIL: no tall cells authored\n"); return 1; }
    s_stolen = 1; s_bphase = 0; s_nenem = 0; s_npick = 0;
    px = 9*ONE + ONE/2; py = 4*ONE + ONE/2; pa = 0;   /* face the tall rune gate (col12) */
    floorcast(); render3d(); render_sprites();
    write_ppm("/tmp/doom_tall.ppm", s_view, RW, RH);
    printf("wrote /tmp/doom_tall.ppm (authored tall walls)\n");

    printf("OK\n");
    return 0;
}
