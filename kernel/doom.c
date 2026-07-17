/* kernel/doom.c — KERNEL PANIC: a Doom-64-like bare-metal FPS for GregOS.

   You are L'Intrus, broken into the core of GregOS to steal the kernel; the
   final boss is Greg 1er and his dragon Drakkar. Fixed-point (16.16) raycaster,
   no libc, no floats. Runs on a scheduler thread with is_gui_active=0, reading
   held-key state via kb_scan_down() and mouselook via ps2mouse_take_rel().

   PHASE 1: textured-less DDA raycaster — walls (side + distance shaded / fog),
   solid ceiling & floor, ZQSD move+strafe, mouselook + arrow turning, grid
   collision. Rendered into a low-res buffer upscaled x2 to 800x600 (the Doom 64
   low-res gloom) with a HUD stub. Floors/ceilings texturing, sprites, enemies,
   weapons and the boss come in later phases.

   Fixed-point note: 16.16 multiply/divide would need 64-bit intermediates and
   pull in libgcc (__muldi3/__divdi3). We instead use the hardware imull/idivl
   via inline asm (fixmul/fixdiv), keeping the freestanding build helper-free.  */

#include "../include/gfx.h"
#include "../include/doom.h"

extern volatile unsigned long jiffies;

/* Fullscreen-game input hooks (kernel/PS2Keyboard.cpp, kernel/PS2Mouse.cpp). */
extern void kb_poll_all(void);
extern int  kb_scan_down(int idx);
extern void kb_keystate_reset(void);
extern void kb_game_capture(int on);
extern void ps2mouse_set_capture(int on);
extern void ps2mouse_take_rel(int* dx, int* dy);
extern int  ps2mouse_buttons(void);

/* PC-speaker bridge (kernel/Timer.cpp via include/Kernel/timer_c.h). */
extern void timer_speaker_on(unsigned int hz);
extern void timer_speaker_off(void);

/* ── PS/2 set-1 scancodes (physical positions; layout-independent) ───────── */
#define SC_ESC     0x01
#define SC_Z       0x11   /* forward       */
#define SC_Q       0x1E   /* strafe left   */
#define SC_S       0x1F   /* back          */
#define SC_D       0x20   /* strafe right  */
#define SC_E       0x12   /* interact      */
#define SC_SPACE   0x39   /* fire          */
#define SC_LSHIFT  0x2A   /* run           */
#define SC_LCTRL   0x1D   /* fire (alt)    */
#define SC_LEFT    (0x4B | 0x80)
#define SC_RIGHT   (0x4D | 0x80)
#define SC_UP      (0x48 | 0x80)
#define SC_DOWN    (0x50 | 0x80)

/* ── Fixed point 16.16 ───────────────────────────────────────────────────── */
#define FIXB 16
#define ONE  (1 << FIXB)

static inline int fixmul(int a, int b)          /* (a*b) >> 16, 32-bit safe */
{
    int r;
    __asm__ ("imull %2\n\t"
             "shrdl $16, %%edx, %%eax"
             : "=a"(r) : "a"(a), "r"(b) : "edx", "cc");
    return r;
}

static inline int fixdiv(int a, int b)          /* (a<<16)/b, 32-bit safe    */
{
    int r;
    __asm__ ("movl %%eax, %%edx\n\t"
             "sarl $16, %%edx\n\t"
             "sall $16, %%eax\n\t"
             "idivl %2"
             : "=a"(r) : "a"(a), "r"(b) : "edx", "cc");
    return r;
}

/* ── Trig: quarter sine table, mirrored to a full 1024-step circle ───────── */
#define ANG  1024
#define ANG90 256
static const int QSIN[257] = {
    0, 402, 804, 1206, 1608, 2010, 2412, 2814,
    3216, 3617, 4019, 4420, 4821, 5222, 5623, 6023,
    6424, 6824, 7224, 7623, 8022, 8421, 8820, 9218,
    9616, 10014, 10411, 10808, 11204, 11600, 11996, 12391,
    12785, 13180, 13573, 13966, 14359, 14751, 15143, 15534,
    15924, 16314, 16703, 17091, 17479, 17867, 18253, 18639,
    19024, 19409, 19792, 20175, 20557, 20939, 21320, 21699,
    22078, 22457, 22834, 23210, 23586, 23961, 24335, 24708,
    25080, 25451, 25821, 26190, 26558, 26925, 27291, 27656,
    28020, 28383, 28745, 29106, 29466, 29824, 30182, 30538,
    30893, 31248, 31600, 31952, 32303, 32652, 33000, 33347,
    33692, 34037, 34380, 34721, 35062, 35401, 35738, 36075,
    36410, 36744, 37076, 37407, 37736, 38064, 38391, 38716,
    39040, 39362, 39683, 40002, 40320, 40636, 40951, 41264,
    41576, 41886, 42194, 42501, 42806, 43110, 43412, 43713,
    44011, 44308, 44604, 44898, 45190, 45480, 45769, 46056,
    46341, 46624, 46906, 47186, 47464, 47741, 48015, 48288,
    48559, 48828, 49095, 49361, 49624, 49886, 50146, 50404,
    50660, 50914, 51166, 51417, 51665, 51911, 52156, 52398,
    52639, 52878, 53114, 53349, 53581, 53812, 54040, 54267,
    54491, 54714, 54934, 55152, 55368, 55582, 55794, 56004,
    56212, 56418, 56621, 56823, 57022, 57219, 57414, 57607,
    57798, 57986, 58172, 58356, 58538, 58718, 58896, 59071,
    59244, 59415, 59583, 59750, 59914, 60075, 60235, 60392,
    60547, 60700, 60851, 60999, 61145, 61288, 61429, 61568,
    61705, 61839, 61971, 62101, 62228, 62353, 62476, 62596,
    62714, 62830, 62943, 63054, 63162, 63268, 63372, 63473,
    63572, 63668, 63763, 63854, 63944, 64031, 64115, 64197,
    64277, 64354, 64429, 64501, 64571, 64639, 64704, 64766,
    64827, 64884, 64940, 64993, 65043, 65091, 65137, 65180,
    65220, 65259, 65294, 65328, 65358, 65387, 65413, 65436,
    65457, 65476, 65492, 65505, 65516, 65525, 65531, 65535,
    65536,
};

static int isin(int a)
{
    a &= (ANG - 1);
    if (a < ANG90)      return  QSIN[a];
    if (a < 2 * ANG90)  return  QSIN[2 * ANG90 - a];
    if (a < 3 * ANG90)  return -QSIN[a - 2 * ANG90];
    return                     -QSIN[ANG - a];
}
static int icos(int a) { return isin(a + ANG90); }

/* ── Map (24x24) — 0 empty, >0 wall texture id ───────────────────────────── */
#define MAP_W 24
#define MAP_H 24
static const unsigned char MAP[MAP_W * MAP_H] = {
    /* 3 zones par bandes X : 1-7 Pare-feu (2=circuit), 8-15 Chambre du Kernel
       (3=rune ambre), 16-22 Arene (4=marbre de sang). Bordure = 1 (pierre).
       Topologie identique a la carte validee (re-skin), connexite prouvee
       (flood-fill hote : spawn -> kernel(13,11) -> arene).                   */
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,1,
    1,0,2,2,0,2,2,2,0,0,0,0,3,0,0,3,4,4,0,4,4,4,0,1,
    1,0,2,0,0,0,0,2,0,0,0,0,3,0,0,3,0,0,0,0,0,4,0,1,
    1,0,2,0,2,2,0,2,0,0,0,0,3,0,0,3,0,0,0,0,0,4,0,1,
    1,0,0,0,2,0,0,0,0,0,0,0,3,0,0,3,0,0,0,0,0,4,0,1,
    1,0,2,0,2,2,2,2,0,0,0,0,3,0,0,3,4,4,0,4,4,4,0,1,
    1,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
    1,0,2,2,2,2,2,0,3,3,3,3,3,3,3,3,0,0,0,0,0,0,0,1,
    1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,1,
    1,2,2,2,0,2,2,2,3,3,3,3,3,0,0,3,0,0,0,0,0,0,0,1,
    1,0,0,0,0,0,0,0,0,0,0,0,3,0,0,3,0,0,0,0,0,0,0,1,
    1,0,2,2,2,2,2,2,3,3,0,0,3,0,0,3,4,4,0,4,4,4,4,1,
    1,0,2,0,0,0,0,0,0,3,0,0,3,0,0,0,0,0,0,0,0,0,0,1,
    1,0,2,0,2,2,2,2,0,3,0,0,3,3,3,3,4,4,4,4,4,0,0,1,
    1,0,2,0,2,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,4,0,0,1,
    1,0,2,0,2,0,0,2,0,3,3,3,3,3,3,3,4,4,0,0,4,0,0,1,
    1,0,2,0,2,2,0,2,0,0,0,0,0,0,0,0,0,4,0,0,0,0,0,1,
    1,0,2,0,0,0,0,0,0,3,3,3,3,3,3,3,0,4,0,4,4,4,0,1,
    1,0,2,2,2,2,2,2,3,3,0,0,0,0,0,3,0,0,0,0,0,4,0,1,
    1,0,0,0,0,0,0,0,0,0,0,3,3,3,0,3,4,4,4,4,0,4,0,1,
    1,0,2,2,2,2,2,2,3,3,0,3,0,0,0,0,0,0,0,0,0,0,0,1,
    1,0,0,0,0,0,0,0,0,0,0,3,0,3,3,3,4,4,4,4,4,4,0,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
};

static int is_wall(int cx, int cy)
{
    if (cx < 0 || cy < 0 || cx >= MAP_W || cy >= MAP_H) return 1;
    return MAP[cy * MAP_W + cx] != 0;
}

/* ── Palette ─────────────────────────────────────────────────────────────── */
#define RGB(r,g,b) (((unsigned)(r)<<16)|((unsigned)(g)<<8)|(unsigned)(b))
#define COL_CEIL   RGB(0x0A,0x0A,0x14)
#define COL_FLOOR  RGB(0x14,0x12,0x0E)

static unsigned shade(unsigned c, int b)         /* scale by b/256 per chan */
{
    int r  = (((c >> 16) & 255) * b) >> 8;
    int g  = (((c >> 8)  & 255) * b) >> 8;
    int bl = ((c & 255) * b) >> 8;
    return RGB(r, g, bl);
}

static unsigned tint(unsigned c, int kr, int kg, int kb)  /* per-zone cast, /256 */
{
    int r  = (((c >> 16) & 255) * kr) >> 8; if (r  > 255) r  = 255;
    int g  = (((c >> 8)  & 255) * kg) >> 8; if (g  > 255) g  = 255;
    int bl = ((c & 255) * kb) >> 8;         if (bl > 255) bl = 255;
    return RGB(r, g, bl);
}

/* ── Procedural 64x64 wall textures (generated once at init) ─────────────── */
#define TEXSZ 64
#define NTEX  5
static unsigned int s_tex[NTEX][TEXSZ * TEXSZ];
static unsigned int s_ftex[TEXSZ * TEXSZ];        /* floor   (base)          */
static unsigned int s_ctex[TEXSZ * TEXSZ];        /* ceiling (base)          */
static unsigned int s_ftex_z[3][TEXSZ * TEXSZ];   /* floor,   per-zone tint  */
static unsigned int s_ctex_z[3][TEXSZ * TEXSZ];   /* ceiling, per-zone tint  */

static unsigned hash2(int x, int y)              /* cheap integer noise */
{
    unsigned h = ((unsigned)x * 73856093u) ^ ((unsigned)y * 19349663u);
    h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
    return h;
}

static void gen_textures(void)
{
    for (int y = 0; y < TEXSZ; ++y)
        for (int x = 0; x < TEXSZ; ++x) {
            unsigned n = hash2(x, y) & 31;
            int i = y * TEXSZ + x;

            /* 1: stone brick — running bond, mortar grooves */
            {
                int row = y / 16;
                int bx  = (x + (row & 1) * 16);
                int mortar = (y % 16 == 0) || (bx % 32 == 0);
                int base = 96 + (int)(n) - 12;
                if (mortar) base = 54;
                s_tex[1][i] = RGB(base, base + 6, base + 14);
            }
            /* 2: phosphor circuit — dark board, green traces + nodes */
            {
                int trace = (x % 16 == 8) || (y % 16 == 8);
                int node  = ((x % 16 == 8) && (y % 16 == 8));
                int g = 40 + (int)(n >> 1);
                unsigned c = RGB(8, g, 20);
                if (trace) c = RGB(24, 170, 60);
                if (node)  c = RGB(120, 255, 140);
                s_tex[2][i] = c;
            }
            /* 3: amber rune slab — gold stone with carved glyph strokes */
            {
                int gx = x & 15, gy = y & 15;
                int glyph = ((gx == 4 || gx == 11) && gy > 2 && gy < 13) ||
                            ((gy == 7) && gx > 3 && gx < 12);
                int base = 120 + (int)(n);
                unsigned c = RGB(base, (base * 3) / 4, 32);
                if (glyph) c = RGB(255, 210, 90);
                s_tex[3][i] = c;
            }
            /* 4: blood marble — dark red with darker veins */
            {
                int vein = ((x + y) % 23 < 2) || ((x - y + 64) % 29 < 2);
                int base = 120 + (int)(n) - 10;
                unsigned c = RGB(base, 24, 22);
                if (vein) c = RGB(60, 8, 8);
                s_tex[4][i] = c;
            }
            s_tex[0][i] = RGB(80, 80, 90);

            /* floor: dark stone flags with grout grid */
            {
                int grout = (x % 32 < 2) || (y % 32 < 2);
                int base = 46 + (int)(n) - 10;
                s_ftex[i] = grout ? RGB(20, 18, 16) : RGB(base, base - 4, base - 10);
            }
            /* ceiling: dark riveted metal panels */
            {
                int panel = (x % 16 == 0) || (y % 16 == 0);
                int rivet = ((x % 16 == 2) && (y % 16 == 2));
                unsigned c = RGB(20, 20, 28);
                if (panel) c = RGB(12, 12, 18);
                if (rivet) c = RGB(60, 62, 72);
                s_ctex[i] = c;
            }
        }

    /* Per-zone colour casts of floor & ceiling, built once from the base:
       0 Pare-feu (phosphor green), 1 Kernel (amber), 2 Arene (blood red). */
    {
        static const int FZ[3][3] = { {190,246,205}, {262,238,150}, {286,150,150} };
        static const int CZ[3][3] = { {190,240,210}, {256,235,160}, {286,150,150} };
        for (int i = 0; i < TEXSZ * TEXSZ; ++i)
            for (int z = 0; z < 3; ++z) {
                s_ftex_z[z][i] = tint(s_ftex[i], FZ[z][0], FZ[z][1], FZ[z][2]);
                s_ctex_z[z][i] = tint(s_ctex[i], CZ[z][0], CZ[z][1], CZ[z][2]);
            }
    }
}

/* ── Sprites (billboard enemies + projectiles + pickups) ─────────────────── */
#define TRANSP 0xFF00FFu                 /* magenta = sprite transparency key */
#define NETYPE 5
enum { ET_SENTINEL, ET_RUNEUR, ET_GOLEM, ET_SPECTRE, ET_CORBEAU };
static unsigned int s_spr[NETYPE][TEXSZ * TEXSZ];
static unsigned int s_spr_bolt[TEXSZ * TEXSZ];
static unsigned int s_spr_ram[TEXSZ * TEXSZ];
static unsigned int s_spr_ammo[TEXSZ * TEXSZ];
static unsigned int s_spr_kernel[TEXSZ * TEXSZ];
static unsigned int s_spr_drak[TEXSZ * TEXSZ];
static unsigned int s_spr_greg[TEXSZ * TEXSZ];

static void gen_sprites(void)
{
    for (int t = 0; t < NETYPE; ++t)
        for (int j = 0; j < TEXSZ * TEXSZ; ++j) s_spr[t][j] = TRANSP;

    for (int y = 0; y < TEXSZ; ++y)
        for (int x = 0; x < TEXSZ; ++x) {
            int cx = x - 32, i = y * TEXSZ + x, n = (int)(hash2(x, y) & 15);

            /* SENTINELLE — hooded guardian, amber eyes */
            { unsigned c = TRANSP;
              if (y >= 22) { int hw = 6 + (y-22)*20/41;
                  if (cx > -hw && cx < hw) { int s = 34+n; c = RGB(s,s-8,s+8); if ((x&7)==0) c = RGB(16,16,24); } }
              { int dy=y-15; if (cx*cx*3+dy*dy*5<430) c = RGB(26,26,34); }
              { int dy=y-18; if (cx*cx*3+dy*dy*4<150) c = RGB(6,6,8); }
              if (y>=15&&y<=19){ if(cx>=-8&&cx<=-3)c=RGB(255,176,40); if(cx>=3&&cx<=8)c=RGB(255,176,40); }
              if (c!=TRANSP) s_spr[ET_SENTINEL][i]=c; }

            /* RUNEUR — green-robed caster, rune bands, green eyes */
            { unsigned c = TRANSP;
              if (y >= 14) { int hw = 4 + (y-14)*18/49;
                  if (cx > -hw && cx < hw) { int s=18+n; c=RGB(s,s+34,s+16); if (y%9==0) c=RGB(40,255,120); } }
              { int dy=y-11; if (cx*cx*3+dy*dy*4<300) c=RGB(16,40,24); }
              { int dy=y-14; if (cx*cx*3+dy*dy*4<120) c=RGB(4,10,6); }
              if (y>=11&&y<=15){ if(cx>=-7&&cx<=-3)c=RGB(120,255,140); if(cx>=3&&cx<=7)c=RGB(120,255,140); }
              if (c!=TRANSP) s_spr[ET_RUNEUR][i]=c; }

            /* GOLEM — bulky stone tank, cracks, red core */
            { unsigned c = TRANSP;
              if (y >= 18) { int hw = 11 + (y-18)*15/45;
                  if (cx > -hw && cx < hw) { int s=68+n-6; c=RGB(s,s-6,s-14); if ((x+y)%17<2) c=RGB(30,26,22); } }
              { int dy=y-14; if (cx>-9&&cx<9&&dy>-8&&dy<6) c=RGB(64,58,50); }
              { int dy=y-26; if (cx*cx+dy*dy<26) c=RGB(255,60,30); }
              if (y>=11&&y<=15){ if(cx>=-6&&cx<=-2)c=RGB(255,90,40); if(cx>=2&&cx<=6)c=RGB(255,90,40); }
              if (c!=TRANSP) s_spr[ET_GOLEM][i]=c; }

            /* SPECTRE — pale wispy ghost, hollow eyes */
            { unsigned c = TRANSP;
              if (y >= 16) { int hw = 5 + (y-16)*16/47;
                  if (cx > -hw && cx < hw) { if (!(y>48 && ((x + (y&3)) & 3)==0)) { int s=118+n; c=RGB(s,s+12,s+34); } } }
              { int dy=y-13; if (cx*cx*3+dy*dy*4<260) c=RGB(150,160,190); }
              { int dy=y-14; if (cx*cx*3+dy*dy*5<90) c=RGB(20,24,40); }
              if (y>=12&&y<=16){ if(cx>=-6&&cx<=-2)c=RGB(20,40,110); if(cx>=2&&cx<=6)c=RGB(20,40,110); }
              if (c!=TRANSP) s_spr[ET_SPECTRE][i]=c; }

            /* CORBEAU — dark raven, wings, beak, red eyes */
            { unsigned c = TRANSP;
              { int dy=y-32; if (cx*cx*2+dy*dy*3<820) c=RGB(20,20,26); }
              if (y>=24&&y<=42){ int w=2+(y-24);
                  if (x<32 && (32-x)<w && (32-x)>w-6) c=RGB(30,30,38);
                  if (x>32 && (x-32)<w && (x-32)>w-6) c=RGB(30,30,38); }
              { int dy=y-20; if (cx*cx*3+dy*dy*4<120) c=RGB(24,24,30); }
              if (y>=22&&y<=27 && cx>=-2 && cx<=2) c=RGB(200,160,40);
              if (y>=18&&y<=20){ if(cx>=-5&&cx<=-2)c=RGB(255,40,30); if(cx>=2&&cx<=5)c=RGB(255,40,30); }
              if (c!=TRANSP) s_spr[ET_CORBEAU][i]=c; }

            /* runic bolt — glowing green orb */
            { unsigned c=TRANSP; int dy=y-32, d=cx*cx+dy*dy;
              if (d<40) c=RGB(210,255,210); else if (d<120) c=RGB(60,220,90); else if (d<220) c=RGB(20,120,40);
              s_spr_bolt[i]=c; }
            /* RAM chip — health pickup */
            { unsigned c=TRANSP;
              if (x>=20&&x<44&&y>=24&&y<44){ c=RGB(40,120,220); if ((x&3)==0||(y&3)==0) c=RGB(20,60,140); }
              if (x>=26&&x<38&&y>=28&&y<40) c=RGB(120,200,255);
              s_spr_ram[i]=c; }
            /* signal cell — ammo pickup */
            { unsigned c=TRANSP;
              if (x>=24&&x<40&&y>=22&&y<46){ c=RGB(210,185,40); if (y<26||y>42) c=RGB(120,100,20); }
              s_spr_ammo[i]=c; }

            /* THE KERNEL — glowing green core */
            { unsigned c=TRANSP; int dy=y-32;
              if (cx>-15&&cx<15 && dy>-15&&dy<15) {
                  c = RGB(24,110,44);
                  if ((x ^ y) & 4) c = RGB(40,180,64);
                  if (cx>-7&&cx<7 && dy>-7&&dy<7) c = RGB(150,255,160);
                  if (cx>-3&&cx<3 && dy>-3&&dy<3) c = RGB(230,255,230);
              }
              s_spr_kernel[i]=c; }

            /* DRAKKAR — dragon head, ember/blood, horns, fangs, glowing eyes */
            { unsigned c=TRANSP; int dy=y-30;
              if (cx*cx*2 + dy*dy*3 < 1500) { int s=64+n; c=RGB(s+50,s-24,s-30); }
              if ((cx*cx + (y-44)*(y-44)) < 300 && y>=34) c=RGB(96,32,26);   /* snout */
              if (y<20) { if (cx>=-19&&cx<=-12&&y>7) c=RGB(52,42,38);
                          if (cx>= 12&&cx<= 19&&y>7) c=RGB(52,42,38); }        /* horns */
              if ((y>=26&&y<=44) && (x<9 || x>55) && ((x+y)&2)) c=RGB(58,26,24); /* wings */
              if (y>=23&&y<=29){ if(cx>=-13&&cx<=-6)c=RGB(255,210,60);
                                 if(cx>=6&&cx<=13)c=RGB(255,210,60); }          /* eyes */
              if (y>=45&&y<=51){ if(cx>=-9&&cx<=-5)c=RGB(238,238,220);
                                 if(cx>=5&&cx<=9)c=RGB(238,238,220); }          /* fangs */
              s_spr_drak[i]=c; }

            /* GREG 1er — crowned king, gold/amber robe, stern glowing eyes */
            { unsigned c=TRANSP;
              if (y>=24) { int hw=8+(y-24)*18/39;
                  if (cx>-hw&&cx<hw){ int s=118+n; c=RGB(s,(s*3)/4,28); if((x&9)==0)c=RGB(150,110,20);} }
              { int dy=y-18; if (cx*cx*3+dy*dy*4<210) c=RGB(150,120,80); }       /* head */
              if (y>=21&&y<=27 && cx>-7&&cx<7) c=RGB(190,190,200);               /* beard */
              if (y>=5&&y<=13 && cx>-11&&cx<11){ c = ((x&3)<2)?RGB(255,215,80):RGB(196,156,40); } /* crown */
              if (y>=14&&y<=18){ if(cx>=-6&&cx<=-2)c=RGB(255,180,40);
                                 if(cx>=2&&cx<=6)c=RGB(255,180,40); }            /* eyes */
              s_spr_greg[i]=c; }
        }
}

/* ── Enemy / projectile / player combat state ────────────────────────────── */
#define MAXENEM 40
#define MAXPROJ 64
typedef struct { int x, y, hp, alive, cool, hurt, type, wob; } Enemy;
typedef struct { int x, y, vx, vy, alive, dmg; } Proj;
static Enemy s_enem[MAXENEM];
static Proj  s_proj[MAXPROJ];
static int   s_nenem;

/* Pickups — RAM chip = health, signal cell = ammo (sprites already generated). */
#define MAXPICK 16
enum { PK_HEALTH, PK_AMMO };
typedef struct { int x, y, kind, alive; } Pickup;
static Pickup s_pick[MAXPICK];
static int    s_npick;
static int    s_pickmsg;        /* frames the "+VIE"/"+MUN" toast stays up */
static int    s_pickkind;       /* last kind picked, for the toast colour */

typedef struct { int hp, speed, mdmg, ranged; } EDef;
static const EDef EDEF[NETYPE] = {
    /* SENTINEL */ { 60,  ONE/26, 6,  0 },
    /* RUNEUR   */ { 45,  ONE/34, 0,  1 },
    /* GOLEM    */ { 180, ONE/50, 18, 0 },
    /* SPECTRE  */ { 40,  ONE/18, 7,  0 },
    /* CORBEAU  */ { 35,  ONE/15, 5,  1 },
};

static int   s_hp, s_fireanim, s_dead, s_kills, s_flash;
static int   s_weap, s_wcool;
static int   s_ammo_bul, s_ammo_shell, s_ammo_charge;

/* weapons — atype: 0 melee(infinite) 1 bullet 2 shell 3 charge */
enum { W_LAME, W_DECOMP, W_FORK, W_RAFALE, W_RMRF, NWEAP };
typedef struct { const char* name; int atype; int rate; int dmg; int automatic; } WDef;
static const WDef WEAP[NWEAP] = {
    { "LAME SIGKILL", 0, 8,  55, 0 },
    { "DECOMPILEUR",  1, 14, 25, 0 },
    { "FORK BOMB",    2, 34, 16, 0 },
    { "RAFALE -9",    1, 5,  15, 1 },
    { "RM-RF /",      3, 48, 110, 0 },
};
static int* ammo_ptr(int atype)
{
    switch (atype) { case 1: return &s_ammo_bul; case 2: return &s_ammo_shell;
                     case 3: return &s_ammo_charge; default: return 0; }
}

/* ── objective + boss (Greg + Drakkar) ───────────────────────────────────── */
#define DRAK_HP 900
#define GREG_HP 700
static int s_stolen, s_won, s_shake;
static int s_kcx = 13, s_kcy = 11;       /* kernel pedestal cell (amber chamber) */
static int s_bphase;                     /* 0 none, 1 Drakkar, 2 Greg, 3 won */
static int s_drakx, s_draky, s_drakhp, s_drakc, s_drakb, s_drakhurt;
static int s_gregx, s_gregy, s_greghp, s_gregc, s_greghurt;

/* ── SFX: frame-driven PC-speaker sound ──────────────────────────────────────
   No thread — `sfx_tick()` runs once per game frame (~30 ms), so a sound is a
   short list of (hz, frames) segments (hz 0 = silence). A higher-priority sound
   interrupts a lower one; the speaker is touched only at segment boundaries and
   is GUARANTEED off on game exit. Shares the speaker with the Chant Runique, but
   the game is fullscreen so nothing else drives it meanwhile.                   */
typedef struct { short hz; short fr; } SfxSeg;
enum { SFX_NONE, SFX_FIRE, SFX_MELEE, SFX_HIT, SFX_KILL, SFX_PICKUP,
       SFX_HURT, SFX_ALARM, SFX_ROAR, SFX_DEATH, SFX_WIN };

static const SfxSeg SFX_FIRE_S[]  = { {760,1}, {360,1} };
static const SfxSeg SFX_MELEE_S[] = { {210,1}, {140,1} };
static const SfxSeg SFX_HIT_S[]   = { {1180,1} };
static const SfxSeg SFX_KILL_S[]  = { {300,1}, {200,1}, {120,2} };
static const SfxSeg SFX_PICK_S[]  = { {700,2}, {1050,3} };
static const SfxSeg SFX_HURT_S[]  = { {170,2} };
static const SfxSeg SFX_ALARM_S[] = { {880,3}, {440,3}, {880,3}, {440,3}, {880,3}, {440,3} };
static const SfxSeg SFX_ROAR_S[]  = { {90,3}, {70,4}, {55,6}, {80,3} };
static const SfxSeg SFX_DEATH_S[] = { {420,3}, {300,3}, {200,4}, {120,7} };
static const SfxSeg SFX_WIN_S[]   = { {523,4}, {659,4}, {784,4}, {1047,9} };

#define SFXN(a) ((short)(sizeof(a) / sizeof((a)[0])))
typedef struct { const SfxSeg* seq; short len; short prio; } SfxDef;
static const SfxDef SFXDB[] = {
    /* SFX_NONE   */ { 0, 0, 0 },
    /* SFX_FIRE   */ { SFX_FIRE_S,  SFXN(SFX_FIRE_S),  1 },
    /* SFX_MELEE  */ { SFX_MELEE_S, SFXN(SFX_MELEE_S), 1 },
    /* SFX_HIT    */ { SFX_HIT_S,   SFXN(SFX_HIT_S),   1 },
    /* SFX_KILL   */ { SFX_KILL_S,  SFXN(SFX_KILL_S),  2 },
    /* SFX_PICKUP */ { SFX_PICK_S,  SFXN(SFX_PICK_S),  2 },
    /* SFX_HURT   */ { SFX_HURT_S,  SFXN(SFX_HURT_S),  2 },
    /* SFX_ALARM  */ { SFX_ALARM_S, SFXN(SFX_ALARM_S), 4 },
    /* SFX_ROAR   */ { SFX_ROAR_S,  SFXN(SFX_ROAR_S),  4 },
    /* SFX_DEATH  */ { SFX_DEATH_S, SFXN(SFX_DEATH_S), 5 },
    /* SFX_WIN    */ { SFX_WIN_S,   SFXN(SFX_WIN_S),   5 },
};

static const SfxSeg* s_sfx_seq;
static int s_sfx_len, s_sfx_i, s_sfx_t, s_sfx_prio;

static void sfx_play(int id)
{
    const SfxDef* d = &SFXDB[id];
    if (!d->seq) return;
    if (s_sfx_seq && s_sfx_i < s_sfx_len && d->prio < s_sfx_prio) return; /* keep louder */
    s_sfx_seq = d->seq; s_sfx_len = d->len; s_sfx_i = 0; s_sfx_prio = d->prio;
    s_sfx_t = d->seq[0].fr;
    if (d->seq[0].hz) timer_speaker_on((unsigned)d->seq[0].hz); else timer_speaker_off();
}

static void sfx_tick(void)
{
    if (!s_sfx_seq) return;                          /* nothing playing */
    if (--s_sfx_t > 0) return;
    if (++s_sfx_i >= s_sfx_len) { s_sfx_seq = 0; s_sfx_prio = 0; timer_speaker_off(); return; }
    s_sfx_t = s_sfx_seq[s_sfx_i].fr;
    if (s_sfx_seq[s_sfx_i].hz) timer_speaker_on((unsigned)s_sfx_seq[s_sfx_i].hz);
    else timer_speaker_off();
}

static void sfx_stop(void) { s_sfx_seq = 0; s_sfx_prio = 0; timer_speaker_off(); }

static void spawn_enemy(int cellx, int celly, int type)
{
    if (s_nenem >= MAXENEM || type < 0 || type >= NETYPE) return;
    Enemy* e = &s_enem[s_nenem++];
    e->x = cellx * ONE + ONE / 2;
    e->y = celly * ONE + ONE / 2;
    e->hp = EDEF[type].hp; e->alive = 1; e->cool = 0; e->hurt = 0;
    e->type = type; e->wob = (cellx * 7 + celly * 13) & 255;
}

static void spawn_pickup(int cellx, int celly, int kind)
{
    if (s_npick >= MAXPICK) return;
    Pickup* p = &s_pick[s_npick++];
    p->x = cellx * ONE + ONE / 2;
    p->y = celly * ONE + ONE / 2;
    p->kind = kind; p->alive = 1;
}

static void spawn_proj(int x, int y, int tx, int ty, int dmg)
{
    int dx = tx - x, dy = ty - y;
    int adx = dx<0?-dx:dx, ady = dy<0?-dy:dy;
    int dist = (adx>ady)?(adx+ady/2):(ady+adx/2); if (dist < ONE) dist = ONE;
    int vx = fixmul(fixdiv(dx, dist), ONE/9);
    int vy = fixmul(fixdiv(dy, dist), ONE/9);
    for (int i = 0; i < MAXPROJ; ++i)
        if (!s_proj[i].alive) {
            s_proj[i].x=x; s_proj[i].y=y; s_proj[i].vx=vx; s_proj[i].vy=vy;
            s_proj[i].alive=1; s_proj[i].dmg=dmg; return;
        }
}

/* ── Render ──────────────────────────────────────────────────────────────── */
#define RW 400
#define RH 240
#define FOV 37837                    /* tan(30deg) in 16.16 (~0.5774)         */

static unsigned int s_view[RW * RH];
static int          s_zbuf[RW];
static int          s_floorcast = 1;             /* fallback toggle           */

/* player state */
static int px, py, pa;               /* pos 16.16, angle 0..ANG-1             */

/* Textured floor & ceiling casting (row-based): fill the whole view, walls are
   then drawn on top by render3d(). Camera height = 0.5 cell. */
static void floorcast(void)
{
    int dirX = icos(pa), dirY = isin(pa);
    int planeX = fixmul(-dirY, FOV), planeY = fixmul(dirX, FOV);
    int rayX0 = dirX - planeX, rayY0 = dirY - planeY;   /* leftmost  ray */
    int rayX1 = dirX + planeX, rayY1 = dirY + planeY;   /* rightmost ray */

    if (!s_floorcast) {
        for (int y = 0; y < RH / 2; ++y)
            for (int x = 0; x < RW; ++x) s_view[y * RW + x] = COL_CEIL;
        for (int y = RH / 2; y < RH; ++y)
            for (int x = 0; x < RW; ++x) s_view[y * RW + x] = COL_FLOOR;
        return;
    }

    for (int y = RH / 2 + 1; y < RH; ++y) {
        int p = y - RH / 2;                             /* 1..RH/2 */
        int rowDist = ((RH / 2) << 16) / p;             /* 16.16 cells */
        int stepX = fixmul(rowDist, rayX1 - rayX0) / RW;
        int stepY = fixmul(rowDist, rayY1 - rayY0) / RW;
        int wx = px + fixmul(rowDist, rayX0);
        int wy = py + fixmul(rowDist, rayY0);

        int distC = rowDist >> 16;
        int bright = 256 - distC * 22; if (bright < 40) bright = 40;
        int cbright = (bright * 3) >> 2;

        unsigned int* fr = s_view + y * RW;             /* floor row       */
        unsigned int* cr = s_view + (RH - 1 - y) * RW;  /* mirrored ceiling */
        for (int x = 0; x < RW; ++x) {
            int cellx = wx >> 16;                        /* zone by X band */
            int z = cellx < 8 ? 0 : (cellx < 16 ? 1 : 2);
            int tx = (wx >> 10) & (TEXSZ - 1);
            int ty = (wy >> 10) & (TEXSZ - 1);
            int idx = ty * TEXSZ + tx;
            fr[x] = shade(s_ftex_z[z][idx], bright);
            cr[x] = shade(s_ctex_z[z][idx], cbright);
            wx += stepX; wy += stepY;
        }
    }
    /* horizon row itself */
    for (int x = 0; x < RW; ++x) s_view[(RH / 2) * RW + x] = COL_FLOOR;
}

/* ── Variable wall heights (multi-hit stacked slices) ─────────────────────────
   Per-cell height in cells; 1 == the original single-height wall. When no tall
   cell is authored (s_has_tall == 0), render3d() short-circuits to the byte-
   identical single-hit path. Floors/ceilings, sprites, the z-buffer and
   collision are ALL untouched — heights only extend the wall silhouette upward. */
#define MAXWALLH 4
static unsigned char s_wallh[MAP_W * MAP_H];
static int           s_has_tall;

static void init_wall_heights(void)
{
    for (int i = 0; i < MAP_W * MAP_H; ++i) s_wallh[i] = MAP[i] ? 1 : 0;
    s_has_tall = 0;
    /* Authored verticality: {x, y, height} — tall gates, chamber pillars,
       arena battlements, firewall accents. Only applied on real wall cells. */
    static const struct { unsigned char x, y, h; } OV[] = {
        {12,1,3},{12,2,3},{12,3,3},{12,4,3},{12,5,3},{12,6,3},   /* tall rune gate  */
        {8,8,3}, {15,8,2},{8,10,3},{12,10,2},{15,10,2},{12,12,3},/* kernel pillars  */
        {16,2,2},{21,2,2},{16,12,3},{21,12,2},{16,14,2},{20,14,2},
        {16,22,3},{21,22,3},                                     /* arena battlements */
        {2,2,2}, {7,2,2}, {2,10,2},{7,10,2},                     /* firewall accents */
    };
    for (unsigned k = 0; k < sizeof(OV) / sizeof(OV[0]); ++k) {
        int h = OV[k].h; if (h < 1) h = 1; if (h > MAXWALLH) h = MAXWALLH;
        int idx = OV[k].y * MAP_W + OV[k].x;
        if (MAP[idx]) { s_wallh[idx] = (unsigned char)h; s_has_tall = 1; }
    }
}

static void render3d(void)
{
    int dirX = icos(pa), dirY = isin(pa);
    int planeX = fixmul(-dirY, FOV), planeY = fixmul(dirX, FOV);

    for (int x = 0; x < RW; ++x) {
        int cameraX = ((2 * x - RW) << 16) / RW;          /* [-ONE, ONE)      */
        int rayX = dirX + fixmul(planeX, cameraX);
        int rayY = dirY + fixmul(planeY, cameraX);

        int mapX = px >> 16, mapY = py >> 16;
        int deltaX = (rayX > -256 && rayX < 256) ? (ONE * 64) : fixdiv(ONE, rayX);
        int deltaY = (rayY > -256 && rayY < 256) ? (ONE * 64) : fixdiv(ONE, rayY);
        if (deltaX < 0) deltaX = -deltaX;
        if (deltaY < 0) deltaY = -deltaY;

        int fracX = px - (mapX << 16), fracY = py - (mapY << 16);
        int stepX, stepY, sideX, sideY;
        if (rayX < 0) { stepX = -1; sideX = fixmul(fracX, deltaX); }
        else          { stepX =  1; sideX = fixmul(ONE - fracX, deltaX); }
        if (rayY < 0) { stepY = -1; sideY = fixmul(fracY, deltaY); }
        else          { stepY =  1; sideY = fixmul(ONE - fracY, deltaY); }

        /* Near→far stacked wall slices. Only the FIRST hit writes the z-buffer
           (nearest perp) — sprite occlusion & collision are byte-for-byte
           unchanged. Taller walls behind nearer shorter ones peek over them; a
           running ceilRow clip means no row is painted twice (fill <= RH/col).
           With s_has_tall == 0 this is one hit + one band + break == the old
           single-height render exactly.                                       */
        int ceilRow = RH;                       /* rows [0, ceilRow) still open above */
        int side = 0, firstHit = 1, guard = 0, hits = 0;
        for (;;) {
            if (sideX < sideY) { sideX += deltaX; mapX += stepX; side = 0; }
            else               { sideY += deltaY; mapY += stepY; side = 1; }
            int oob  = (mapX < 0 || mapY < 0 || mapX >= MAP_W || mapY >= MAP_H);
            int tile = oob ? 1 : MAP[mapY * MAP_W + mapX];
            if (tile) {
                int perp = (side == 0) ? (sideX - deltaX) : (sideY - deltaY);
                if (perp < 256) perp = 256;
                if (firstHit) { s_zbuf[x] = perp; firstHit = 0; }

                int lineH = (RH << 16) / perp;
                if (lineH < 1) lineH = 1;
                int wallH = oob ? 1 : s_wallh[mapY * MAP_W + mapX];
                int bot = RH / 2 + lineH / 2;                  /* floor line (== old de) */
                int top = bot - wallH * lineH;                 /* wallH==1 → old ds      */
                int dTop = top < 0 ? 0 : top;
                int dBot = bot > ceilRow ? ceilRow : bot;      /* clip into the open band */
                if (dTop < dBot) {
                    int distC = perp >> 16;
                    int bright = 256 - distC * 22; if (bright < 40) bright = 40;
                    if (side == 1) bright = (bright * 3) >> 2;  /* Y-walls darker */

                    int wallX = (side == 0) ? (py + fixmul(perp, rayY))
                                            : (px + fixmul(perp, rayX));
                    int texX = (wallX >> 10) & (TEXSZ - 1);
                    if ((side == 0 && rayX > 0) || (side == 1 && rayY < 0))
                        texX = (TEXSZ - 1) - texX;
                    int texId = (tile > 0 && tile < NTEX) ? tile : 0;
                    const unsigned int* tex = s_tex[texId];

                    int texStep = (TEXSZ << 16) / lineH;
                    int texPos  = (dTop - top) * texStep;       /* crest = texY 0 */
                    unsigned int* vp = s_view + x;
                    for (int y = dTop; y < dBot; ++y) {
                        int texY = (texPos >> 16) & (TEXSZ - 1);
                        texPos += texStep;
                        vp[y * RW] = shade(tex[texY * TEXSZ + texX], bright);
                    }
                }
                if (top <= ceilRow) ceilRow = top < 0 ? 0 : top; /* raise open frontier */
                if (!s_has_tall) break;                          /* fast path == today  */
                if (ceilRow <= 0 || oob || ++hits >= 6) break;
            }
            if (++guard > 80) break;
        }
    }
}

/* line of sight between two 16.16 world points (quarter-cell march) */
static int los(int x0, int y0, int x1, int y1)
{
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = ((adx > ady ? adx : ady) >> 14);
    if (steps < 1) return 1;
    int sx = dx / steps, sy = dy / steps, cx = x0, cy = y0;
    for (int i = 0; i < steps; ++i) {
        cx += sx; cy += sy;
        if (is_wall(cx >> 16, cy >> 16)) return 0;
    }
    return 1;
}

/* one billboard sprite (world pos, texture, size %, vertical offset %). */
static int g_dirX, g_dirY, g_planeX, g_planeY, g_invDet;

static void draw_billboard(int wx, int wy, const unsigned int* tex,
                           int sz, int yoff, int hurt)
{
    int spx = wx - px, spy = wy - py;
    int ty = fixmul(g_invDet, fixmul(-g_planeY, spx) + fixmul(g_planeX, spy));
    if (ty < ONE / 6) return;
    int tx = fixmul(g_invDet, fixmul(g_dirY, spx) - fixmul(g_dirX, spy));
    int screenX = (RW / 2 * (ONE + fixdiv(tx, ty))) >> 16;
    int base = (RH << 16) / ty;
    int sh = base * sz / 100; if (sh < 2) return;
    int sw = sh;
    int bottom = RH / 2 + base / 2 - base * yoff / 100;
    int y0 = bottom - sh, x0 = screenX - sw / 2;
    int cy0 = y0 < 0 ? 0 : y0, cy1 = bottom > RH ? RH : bottom;
    int fog = 256 - (ty >> 16) * 22; if (fog < 40) fog = 40;
    int txStep = (TEXSZ << 16) / sw, tyStep = (TEXSZ << 16) / sh;
    for (int x = x0; x < x0 + sw; ++x) {
        if (x < 0 || x >= RW) continue;
        if (ty >= s_zbuf[x]) continue;                   /* behind a wall */
        int texX = ((x - x0) * txStep) >> 16;
        if (texX < 0 || texX >= TEXSZ) continue;
        int texPos = (cy0 - y0) * tyStep;
        for (int y = cy0; y < cy1; ++y) {
            int texY = texPos >> 16; texPos += tyStep;
            if (texY < 0 || texY >= TEXSZ) continue;
            unsigned c = tex[texY * TEXSZ + texX];
            if (c == TRANSP) continue;
            if (hurt) c = RGB(255, 255, 255);
            s_view[y * RW + x] = shade(c, fog);
        }
    }
}

/* enemies (by type) + projectiles, depth-sorted far->near, z-occluded */
struct Draw { int x, y; const unsigned int* tex; int sz, yoff, hurt, d; };
static struct Draw s_dl[MAXENEM + MAXPROJ + MAXPICK + 8];

static void render_sprites(void)
{
    g_dirX = icos(pa); g_dirY = isin(pa);
    g_planeX = fixmul(-g_dirY, FOV); g_planeY = fixmul(g_dirX, FOV);
    int det = fixmul(g_planeX, g_dirY) - fixmul(g_dirX, g_planeY);
    if (det > -64 && det < 64) return;
    g_invDet = fixdiv(ONE, det);

    int n = 0;
    for (int i = 0; i < s_nenem; ++i) {
        Enemy* e = &s_enem[i]; if (!e->alive) continue;
        int dx = (e->x - px) >> 8, dy = (e->y - py) >> 8;
        s_dl[n].x = e->x; s_dl[n].y = e->y; s_dl[n].tex = s_spr[e->type];
        s_dl[n].sz  = (e->type==ET_GOLEM)?150 : (e->type==ET_CORBEAU)?72 : 100;
        s_dl[n].yoff = (e->type==ET_CORBEAU)?45 : 0;
        s_dl[n].hurt = e->hurt; s_dl[n].d = dx*dx + dy*dy; n++;
    }
    for (int i = 0; i < MAXPROJ; ++i) {
        Proj* p = &s_proj[i]; if (!p->alive) continue;
        int dx = (p->x - px) >> 8, dy = (p->y - py) >> 8;
        s_dl[n].x = p->x; s_dl[n].y = p->y; s_dl[n].tex = s_spr_bolt;
        s_dl[n].sz = 28; s_dl[n].yoff = 22; s_dl[n].hurt = 0;
        s_dl[n].d = dx*dx + dy*dy; n++;
    }
    for (int i = 0; i < s_npick; ++i) {                  /* floor pickups, gentle bob */
        Pickup* p = &s_pick[i]; if (!p->alive) continue;
        int dx = (p->x - px) >> 8, dy = (p->y - py) >> 8;
        s_dl[n].x = p->x; s_dl[n].y = p->y;
        s_dl[n].tex = (p->kind == PK_HEALTH) ? s_spr_ram : s_spr_ammo;
        s_dl[n].sz = 46; s_dl[n].yoff = 6 + (int)((jiffies >> 2) & 3);
        s_dl[n].hurt = 0; s_dl[n].d = dx*dx + dy*dy; n++;
    }
    if (!s_stolen) {                                     /* the kernel, on its pedestal */
        int kx = s_kcx*ONE + ONE/2, ky = s_kcy*ONE + ONE/2;
        int dx = (kx - px) >> 8, dy = (ky - py) >> 8;
        s_dl[n].x = kx; s_dl[n].y = ky; s_dl[n].tex = s_spr_kernel;
        s_dl[n].sz = 60; s_dl[n].yoff = 14; s_dl[n].hurt = 0;
        s_dl[n].d = dx*dx + dy*dy; n++;
    }
    if (s_bphase >= 1 && s_drakhp > 0) {                 /* Drakkar — huge */
        int dx = (s_drakx - px) >> 8, dy = (s_draky - py) >> 8;
        s_dl[n].x = s_drakx; s_dl[n].y = s_draky; s_dl[n].tex = s_spr_drak;
        s_dl[n].sz = 230; s_dl[n].yoff = 28; s_dl[n].hurt = s_drakhurt;
        s_dl[n].d = dx*dx + dy*dy; n++;
    }
    if (s_bphase >= 1 && s_bphase < 3) {                 /* Greg 1er */
        int dx = (s_gregx - px) >> 8, dy = (s_gregy - py) >> 8;
        s_dl[n].x = s_gregx; s_dl[n].y = s_gregy; s_dl[n].tex = s_spr_greg;
        s_dl[n].sz = 150; s_dl[n].yoff = 0; s_dl[n].hurt = s_greghurt;
        s_dl[n].d = dx*dx + dy*dy; n++;
    }
    for (int a = 1; a < n; ++a) {                        /* insertion sort */
        struct Draw t = s_dl[a]; int b = a - 1;
        while (b >= 0 && s_dl[b].d < t.d) { s_dl[b+1] = s_dl[b]; --b; }
        s_dl[b+1] = t;
    }
    for (int k = 0; k < n; ++k)
        draw_billboard(s_dl[k].x, s_dl[k].y, s_dl[k].tex,
                       s_dl[k].sz, s_dl[k].yoff, s_dl[k].hurt);
}

#define AIMTAN   9830                    /* ~tan(8.5deg) 16.16 (pistol cone)   */
#define WIDETAN  39000                   /* ~tan(31deg)  16.16 (melee cone)    */

static void damage_enemy(int i, int dmg)
{
    s_enem[i].hp -= dmg; s_enem[i].hurt = 2;
    if (s_enem[i].hp <= 0) { s_enem[i].alive = 0; s_kills++; sfx_play(SFX_KILL); }
    else                     sfx_play(SFX_HIT);
}

/* hitscan along `ang`: damage nearest enemy inside cone `coneT`, range `maxr`, LOS-clear */
static void hitscan(int ang, int dmg, int coneT, int maxr)
{
    int dirX = icos(ang), dirY = isin(ang);
    int best = -1, bestd = 0x7FFFFFFF;
    for (int i = 0; i < s_nenem; ++i) {
        if (!s_enem[i].alive) continue;
        int dx = s_enem[i].x - px, dy = s_enem[i].y - py;
        int fwd = fixmul(dx, dirX) + fixmul(dy, dirY);
        if (fwd <= 0 || fwd > maxr) continue;
        int lat = fixmul(dx, -dirY) + fixmul(dy, dirX);
        int alat = lat < 0 ? -lat : lat;
        if (alat > fixmul(fwd, coneT)) continue;
        if (!los(px, py, s_enem[i].x, s_enem[i].y)) continue;
        if (fwd < bestd) { bestd = fwd; best = i; }
    }
    /* the active boss is a wide, easier-to-hit target (best == -2) */
    int bx = 0, by = 0, bactive = 0;
    if      (s_bphase == 1 && s_drakhp > 0) { bx = s_drakx; by = s_draky; bactive = 1; }
    else if (s_bphase == 2 && s_greghp > 0) { bx = s_gregx; by = s_gregy; bactive = 1; }
    if (bactive) {
        int dx = bx - px, dy = by - py;
        int fwd = fixmul(dx, dirX) + fixmul(dy, dirY);
        if (fwd > 0 && fwd <= maxr) {
            int lat = fixmul(dx, -dirY) + fixmul(dy, dirX);
            int alat = lat < 0 ? -lat : lat;
            if (alat <= fixmul(fwd, coneT + 8000) && los(px, py, bx, by) && fwd < bestd)
                { bestd = fwd; best = -2; }
        }
    }
    if (best == -2) {
        if (s_bphase == 1) { s_drakhp -= dmg; s_drakhurt = 2; }
        else               { s_greghp -= dmg; s_greghurt = 2; }
    } else if (best >= 0) damage_enemy(best, dmg);
}

static void fire_weapon(void)
{
    if (s_wcool > 0) return;
    const WDef* w = &WEAP[s_weap];
    int* ap = ammo_ptr(w->atype);
    if (ap && *ap <= 0) return;
    if (ap) (*ap)--;
    s_wcool = w->rate; s_fireanim = 4;

    if (s_weap == W_LAME) {
        hitscan(pa, w->dmg, WIDETAN, ONE * 3 / 2);
    } else if (s_weap == W_FORK) {
        for (int p = -3; p <= 3; ++p)                    /* 7-pellet spread */
            hitscan((pa + p * 6) & (ANG - 1), w->dmg, AIMTAN, 40 * ONE);
    } else if (s_weap == W_RMRF) {
        for (int i = 0; i < s_nenem; ++i) {              /* AoE blast */
            if (!s_enem[i].alive) continue;
            int dx = (px - s_enem[i].x) >> 8, dy = (py - s_enem[i].y) >> 8;
            int r = (5 * ONE) >> 8;
            if (dx*dx + dy*dy < r*r && los(px, py, s_enem[i].x, s_enem[i].y))
                damage_enemy(i, w->dmg);
        }
        { int rr = (5 * ONE) >> 8;                        /* boss caught in blast */
          if (s_bphase == 1 && s_drakhp > 0) {
              int dx = (px - s_drakx) >> 8, dy = (py - s_draky) >> 8;
              if (dx*dx + dy*dy < rr*rr) { s_drakhp -= w->dmg; s_drakhurt = 2; }
          } else if (s_bphase == 2 && s_greghp > 0) {
              int dx = (px - s_gregx) >> 8, dy = (py - s_gregy) >> 8;
              if (dx*dx + dy*dy < rr*rr) { s_greghp -= w->dmg; s_greghurt = 2; }
          } }
        s_flash = 6;
    } else {
        hitscan(pa, w->dmg, AIMTAN, 64 * ONE);           /* pistol / rafale */
    }
    sfx_play(s_weap == W_LAME ? SFX_MELEE : SFX_FIRE);
}

#define AGGRO  (10 * ONE)
static void update_proj(void)
{
    for (int i = 0; i < MAXPROJ; ++i) {
        Proj* p = &s_proj[i]; if (!p->alive) continue;
        p->x += p->vx; p->y += p->vy;
        if (is_wall(p->x >> 16, p->y >> 16)) { p->alive = 0; continue; }
        int dx = px - p->x, dy = py - p->y;
        int adx = dx<0?-dx:dx, ady = dy<0?-dy:dy;
        if (adx < ONE/3 && ady < ONE/3) { s_hp -= p->dmg; p->alive = 0; }
    }
}

static void enemy_ai(void)
{
    for (int i = 0; i < s_nenem; ++i) {
        Enemy* e = &s_enem[i]; if (!e->alive) continue;
        if (e->hurt) e->hurt--;
        e->wob = (e->wob + 5) & 255;
        const EDef* d = &EDEF[e->type];
        int dx = px - e->x, dy = py - e->y;
        int adx = dx<0?-dx:dx, ady = dy<0?-dy:dy;
        int dist = (adx > ady) ? (adx + ady/2) : (ady + adx/2);
        if (dist > ONE/2 && dist < AGGRO && los(e->x, e->y, px, py)) {
            int sx = fixmul(fixdiv(dx, dist), d->speed);
            int sy = fixmul(fixdiv(dy, dist), d->speed);
            if (d->ranged && dist < 4 * ONE) { sx = -sx; sy = -sy; }   /* kite */
            if (!is_wall((e->x + sx) >> 16, e->y >> 16)) e->x += sx;
            if (!is_wall(e->x >> 16, (e->y + sy) >> 16)) e->y += sy;
            if (d->ranged) {
                if (e->cool <= 0) {
                    spawn_proj(e->x, e->y, px, py, e->type==ET_CORBEAU?5:9);
                    e->cool = (e->type==ET_CORBEAU) ? 50 : 75;
                }
            } else if (dist < ONE*3/4 && e->cool <= 0) {
                s_hp -= d->mdmg; e->cool = (e->type==ET_GOLEM) ? 70 : 55;
            }
        }
        if (e->cool > 0) e->cool--;
    }
    update_proj();
    /* Winning takes precedence: if the killing blow on the boss lands the same
       frame a hit would drop us, we go out victorious (never win AND die). */
    if (s_hp <= 0 && !s_won) { s_hp = 0; s_dead = 1; }
}

/* Walk-over pickups: RAM chip restores 25 VIE (cap 100), signal cell refills
   ammo. A short toast is shown in the HUD (`s_pickmsg`). */
static void update_pickups(void)
{
    for (int i = 0; i < s_npick; ++i) {
        Pickup* p = &s_pick[i]; if (!p->alive) continue;
        int dx = px - p->x, dy = py - p->y;
        int adx = dx<0?-dx:dx, ady = dy<0?-dy:dy;
        if (adx < ONE*3/5 && ady < ONE*3/5) {
            if (p->kind == PK_HEALTH) { s_hp += 25; if (s_hp > 100) s_hp = 100; }
            else { s_ammo_bul += 30; s_ammo_shell += 6; }
            p->alive = 0; s_pickmsg = 45; s_pickkind = p->kind;
            sfx_play(SFX_PICKUP);
        }
    }
    /* s_pickmsg counts down in the main loop (ungated), not here — so the toast
       can't freeze on the end screen when this function stops being called. */
}

/* ── Boss: Greg 1er & le dragon Drakkar (2 phases) ───────────────────────────
   The boss flies/floats and ignores walls so the arena works on any map layout;
   its projectiles DO collide with walls, so cover and line-of-sight matter.     */
#define BOSS_STAND (ONE * 5 / 2)         /* standoff so it doesn't smother you  */

static void boss_move_toward(int* bx, int* by, int speed, int standoff)
{
    int dx = px - *bx, dy = py - *by;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int dist = (adx > ady) ? (adx + ady/2) : (ady + adx/2); if (dist < ONE) dist = ONE;
    if (dist <= standoff) return;
    *bx += fixmul(fixdiv(dx, dist), speed);
    *by += fixmul(fixdiv(dy, dist), speed);
}

static void boss_breath(int bx, int by, int dmg, int spread)
{
    for (int k = -2; k <= 2; ++k)                    /* 5-bolt fan of fire */
        spawn_proj(bx, by, px + k*spread, py + k*spread, dmg);
}

static void steal_kernel(void)
{
    if (s_stolen) return;
    s_stolen = 1; s_shake = 18;
    s_bphase = 1;                                    /* summon the guardians */
    s_drakhp = DRAK_HP; s_greghp = GREG_HP;
    s_drakx = s_kcx * ONE + ONE/2; s_draky = s_kcy * ONE + ONE/2;
    s_gregx = s_drakx;             s_gregy = s_draky;
    s_drakc = 20; s_gregc = 40; s_drakb = 0;
    s_drakhurt = 0; s_greghurt = 0;
    spawn_enemy(11, 13, ET_SENTINEL);                /* alarm: purge units wake */
    spawn_enemy(14, 13, ET_SPECTRE);
    sfx_play(SFX_ALARM);
}

static void boss_update(void)
{
    if (s_drakhurt) s_drakhurt--;
    if (s_greghurt) s_greghurt--;

    if (s_bphase == 1) {                             /* DRAKKAR — Greg protected */
        s_drakb = (s_drakb + 6) & (ANG - 1);
        boss_move_toward(&s_drakx, &s_draky, ONE/22, BOSS_STAND);
        boss_move_toward(&s_gregx, &s_gregy, ONE/70, ONE*4);   /* Greg drifts in */
        if (s_drakc <= 0) {
            if (los(s_drakx, s_draky, px, py)) {
                boss_breath(s_drakx, s_draky, 10, ONE/2);
                s_drakc = 48; s_shake = 5;
            } else s_drakc = 12;
        }
        if (s_drakc > 0) s_drakc--;
        if (s_drakhp <= 0) { s_bphase = 2; s_shake = 24; s_gregc = 26; sfx_play(SFX_ROAR); }
    } else if (s_bphase == 2) {                      /* GREG — enraged, vulnerable */
        boss_move_toward(&s_gregx, &s_gregy, ONE/30, ONE*2);
        if (s_gregc <= 0) {
            if (los(s_gregx, s_gregy, px, py)) {
                for (int k = -1; k <= 1; ++k)                  /* runic volley */
                    spawn_proj(s_gregx, s_gregy, px + k*ONE, py + k*ONE, 8);
                s_gregc = 40; s_shake = 4;
            } else s_gregc = 12;
        }
        if (s_gregc > 0) s_gregc--;
        int dx = px - s_gregx, dy = py - s_gregy;               /* melee aura */
        int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
        int dist = (adx > ady) ? (adx + ady/2) : (ady + adx/2);
        if (dist < ONE*3/2 && (jiffies & 7) == 0) s_hp -= 2;
        if (s_greghp <= 0) { s_bphase = 3; s_won = 1; }
    }
}

/* first-person weapon (full-res, into the back buffer, bob + muzzle flash) */
static void draw_weapon(void)
{
    unsigned int* back = gfx_get_backbuffer();
    int SW = gfx_width();
    int viewH = RH * 2;
    int cx = SW / 2 + (int)((jiffies >> 1) & 7) - 3;      /* subtle bob */
    int top = viewH - 110;

    unsigned body, barrel; int bw;
    switch (s_weap) {
        case W_LAME:   body = RGB(38,38,48); barrel = RGB(190,210,230); bw = 6;  break;
        case W_FORK:   body = RGB(46,40,34); barrel = RGB(74,62,50);    bw = 22; break;
        case W_RAFALE: body = RGB(28,32,42); barrel = RGB(50,54,66);    bw = 9;  break;
        case W_RMRF:   body = RGB(52,28,28); barrel = RGB(150,44,40);   bw = 17; break;
        default:       body = RGB(38,42,52); barrel = RGB(58,62,74);    bw = 12; break;
    }

    for (int y = top + 46; y < viewH; ++y)                /* body */
        for (int x = cx - 46; x < cx + 46; ++x) back[y * SW + x] = body;
    for (int y = top + 70; y < viewH; ++y)                /* grip */
        for (int x = cx + 8; x < cx + 30; ++x) back[y * SW + x] = RGB(24, 26, 34);

    if (s_weap == W_LAME) {                               /* blade */
        for (int y = top - 40; y < top + 50; ++y) {
            int hw = 2 + (top + 50 - y) / 9;
            for (int x = cx - hw; x < cx + hw; ++x) back[y * SW + x] = barrel;
        }
    } else {
        for (int y = top; y < top + 50; ++y)              /* barrel */
            for (int x = cx - bw; x < cx + bw; ++x) back[y * SW + x] = barrel;
    }
    for (int y = top + 6; y < top + 12; ++y)              /* sight glow */
        for (int x = cx - 3; x < cx + 3; ++x) back[y * SW + x] = RGB(60, 255, 110);

    if (s_fireanim > 0 && s_weap != W_LAME) {             /* muzzle flash */
        int fx = cx, fy = top - 6;
        unsigned hot = (s_weap==W_RMRF)?RGB(255,120,90):RGB(255,245,180);
        unsigned mid = (s_weap==W_RMRF)?RGB(220,50,40):RGB(255,190,70);
        for (int y = fy - 26; y < fy + 10; ++y)
            for (int x = fx - 28; x < fx + 28; ++x) {
                if (x < 0 || x >= SW || y < 0 || y >= viewH) continue;
                int d = (x - fx) * (x - fx) + (y - fy) * (y - fy);
                if (d < 500) back[y * SW + x] = (d < 180) ? hot : mid;
            }
    }
}

static void blit_upscale(void)
{
    unsigned int* back = gfx_get_backbuffer();
    int SW = gfx_width();
    for (int vy = 0; vy < RH; ++vy) {
        const unsigned int* s = s_view + vy * RW;
        unsigned int* d0 = back + (vy * 2) * SW;
        unsigned int* d1 = d0 + SW;
        for (int vx = 0; vx < RW; ++vx) {
            unsigned int c = s[vx];
            int dx = vx * 2;
            d0[dx] = c; d0[dx + 1] = c; d1[dx] = c; d1[dx + 1] = c;
        }
    }
}

static void u2s(int v, char* b)
{
    if (v < 0) v = 0;
    char t[12]; int n = 0;
    if (!v) { b[0] = '0'; b[1] = 0; return; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    int i = 0; while (n) b[i++] = t[--n]; b[i] = 0;
}

static void draw_boss_bar(void)
{
    if (s_bphase < 1 || s_bphase > 2) return;
    int SW = gfx_width();
    const char* nm; int hp, maxhp; unsigned col;
    if (s_bphase == 1) { nm = "DRAKKAR";  hp = s_drakhp; maxhp = DRAK_HP; col = RGB(0xE0,0x50,0x30); }
    else               { nm = "GREG 1er"; hp = s_greghp; maxhp = GREG_HP; col = RGB(0xFF,0xC0,0x40); }
    if (hp < 0) hp = 0;
    int len = 0; while (nm[len]) len++;
    int bw = 480, bx = SW/2 - bw/2, by = 16;
    gfx_fill_rect(bx-4, by-4, bw+8, 22, RGB(0x10,0x08,0x08));
    gfx_fill_rect(bx, by, bw, 14, RGB(0x2A,0x14,0x14));
    gfx_fill_rect(bx, by, hp*bw/maxhp, 14, col);
    gfx_draw_str(SW/2 - len*4, by - 20, nm, col, GFX_TRANSPARENT);
}

static void draw_hud(void)
{
    int SW = gfx_width(), SH = gfx_height();
    int y0 = RH * 2;                                  /* 480 */
    char b[16];
    gfx_fill_rect(0, y0, SW, SH - y0, RGB(0x06, 0x06, 0x0C));
    gfx_draw_hline(0, y0, SW, RGB(0x1A, 0x60, 0x24));

    unsigned hpc = s_hp > 50 ? RGB(0x60, 0xFF, 0x70)
                 : (s_hp > 20 ? RGB(0xFF, 0xC0, 0x40) : RGB(0xFF, 0x50, 0x40));
    gfx_draw_str(16, y0 + 12, "VIE", RGB(0x30, 0xFF, 0x60), GFX_TRANSPARENT);
    u2s(s_hp, b); gfx_draw_str(56, y0 + 12, b, hpc, GFX_TRANSPARENT);
    gfx_fill_rect(16, y0 + 34, 160, 10, RGB(0x20, 0x10, 0x10));
    gfx_fill_rect(16, y0 + 34, s_hp > 100 ? 160 : s_hp * 160 / 100, 10, hpc);

    gfx_draw_str(230, y0 + 12, "MUN", RGB(0x30, 0xFF, 0x60), GFX_TRANSPARENT);
    int* ap = ammo_ptr(WEAP[s_weap].atype);
    if (ap) { u2s(*ap, b); }
    else    { b[0]='I'; b[1]='N'; b[2]='F'; b[3]=0; }
    gfx_draw_str(270, y0 + 12, b, RGB(0xFF, 0xE0, 0x80), GFX_TRANSPARENT);
    gfx_draw_str(230, y0 + 34, WEAP[s_weap].name, RGB(0x2A, 0x80, 0x38), GFX_TRANSPARENT);
    { char nb[4]; nb[0]=(char)('1'+s_weap); nb[1]=0;
      gfx_draw_str(210, y0 + 34, nb, RGB(0xFF,0xE0,0x80), GFX_TRANSPARENT); }

    const char* obj = !s_stolen ? "VOLE LE KERNEL"
                    : (s_won ? "VICTOIRE !" : "TUE GREG+DRAKKAR");
    gfx_draw_str(SW - 26 * 8, y0 + 12, obj,
                 RGB(0xC8, 0x96, 0x30), GFX_TRANSPARENT);
    gfx_draw_str(SW - 26 * 8, y0 + 34, "PURGES:", RGB(0x30, 0xFF, 0x60), GFX_TRANSPARENT);
    u2s(s_kills, b); gfx_draw_str(SW - 26 * 8 + 64, y0 + 34, b,
                                  RGB(0xFF, 0xE0, 0x80), GFX_TRANSPARENT);
}

/* ── Movement ────────────────────────────────────────────────────────────── */
#define SPEED   (ONE / 12)
#define RAD     (ONE / 4)
#define TURN    12

static void try_move(int ndx, int ndy)
{
    int nxp = px + ndx;
    int cx = (ndx > 0 ? nxp + RAD : nxp - RAD) >> 16;
    if (!is_wall(cx, py >> 16)) px = nxp;
    int nyp = py + ndy;
    int cy = (ndy > 0 ? nyp + RAD : nyp - RAD) >> 16;
    if (!is_wall(px >> 16, cy)) py = nyp;
}

void gg_kernel_panic(void)
{
    kb_game_capture(1);
    kb_keystate_reset();
    ps2mouse_set_capture(1);
    gen_textures();
    gen_sprites();
    init_wall_heights();

    px = ONE + ONE / 2;              /* start cell (1,1), open corridor east   */
    py = ONE + ONE / 2;
    pa = 0;

    s_hp = 100; s_fireanim = 0; s_dead = 0; s_kills = 0; s_flash = 0;
    s_weap = W_DECOMP; s_wcool = 0;
    s_ammo_bul = 80; s_ammo_shell = 24; s_ammo_charge = 3;
    s_stolen = 0; s_won = 0; s_shake = 0; s_bphase = 0;
    s_drakhurt = 0; s_greghurt = 0;
    s_sfx_seq = 0; s_sfx_prio = 0; timer_speaker_off();
    s_nenem = 0;
    for (int i = 0; i < MAXPROJ; ++i) s_proj[i].alive = 0;
    /* Enemies spread across the 3 zones (all cells open + reachable, verified). */
    spawn_enemy(5, 9,  ET_SENTINEL); spawn_enemy(4, 7,  ET_RUNEUR);   /* Pare-feu */
    spawn_enemy(2, 20, ET_GOLEM);
    spawn_enemy(9, 9,  ET_SPECTRE);  spawn_enemy(14, 13, ET_SENTINEL);/* Kernel   */
    spawn_enemy(11, 13, ET_RUNEUR);
    spawn_enemy(17, 9, ET_CORBEAU);  spawn_enemy(20, 20, ET_SPECTRE); /* Arene    */

    s_npick = 0; s_pickmsg = 0;
    spawn_pickup(5, 7,  PK_HEALTH);  spawn_pickup(2, 22,  PK_AMMO);   /* Pare-feu */
    spawn_pickup(11, 11, PK_HEALTH); spawn_pickup(10, 9,  PK_AMMO);   /* Kernel   */
    spawn_pickup(18, 9, PK_HEALTH);  spawn_pickup(19, 9,  PK_AMMO);   /* Arene    */

    int prevfire = 0;
    int prev_hp = s_hp, prev_dead = 0, prev_won = 0;
    unsigned long frame_at = jiffies;

    for (;;) {
        kb_poll_all();
        if (kb_scan_down(SC_ESC)) break;

        if (!s_dead && !s_won) {
            int mdx = 0, mdy = 0;
            ps2mouse_take_rel(&mdx, &mdy);
            pa += mdx * 2;                               /* mouselook         */
            if (kb_scan_down(SC_LEFT))  pa -= TURN;
            if (kb_scan_down(SC_RIGHT)) pa += TURN;
            pa &= (ANG - 1);

            int dirX = icos(pa), dirY = isin(pa);
            int strX = -dirY, strY = dirX;               /* strafe unit vec   */
            int mv = 0, sv = 0;
            int sp = kb_scan_down(SC_LSHIFT) ? (SPEED * 7 / 4) : SPEED;
            if (kb_scan_down(SC_Z) || kb_scan_down(SC_UP))   mv += sp;
            if (kb_scan_down(SC_S) || kb_scan_down(SC_DOWN)) mv -= sp;
            if (kb_scan_down(SC_Q)) sv -= sp;
            if (kb_scan_down(SC_D)) sv += sp;
            if (mv || sv)
                try_move(fixmul(dirX, mv) + fixmul(strX, sv),
                         fixmul(dirY, mv) + fixmul(strY, sv));

            for (int wk = 0; wk < NWEAP; ++wk)               /* weapon switch 1-5 */
                if (kb_scan_down(0x02 + wk)) s_weap = wk;

            int firenow = kb_scan_down(SC_SPACE) || kb_scan_down(SC_LCTRL) ||
                          (ps2mouse_buttons() & 1);
            if (WEAP[s_weap].automatic) { if (firenow) fire_weapon(); }
            else if (firenow && !prevfire) fire_weapon();
            prevfire = firenow;
            if (s_wcool > 0) s_wcool--;

            if (kb_scan_down(SC_E) && !s_stolen) {           /* steal the kernel */
                int dx = px - (s_kcx*ONE + ONE/2), dy = py - (s_kcy*ONE + ONE/2);
                int adx = dx<0?-dx:dx, ady = dy<0?-dy:dy;
                if (adx < ONE*3/2 && ady < ONE*3/2) steal_kernel();
            }
            update_pickups();               /* heal BEFORE damage: a chip can save you, */
            if (s_bphase >= 1 && s_bphase < 3) boss_update();  /* and never heals a corpse */
            enemy_ai();
        }

        /* Render-driving timers tick EVERY frame (even on the end screens), so
           the rm-rf flash / muzzle glow / pickup toast never freeze on-screen —
           their consumers below are ungated. (s_wcool/s_fireanim-for-fire logic
           only matters while alive, hence gated above / ticked here for render.) */
        if (s_fireanim > 0) s_fireanim--;
        if (s_flash > 0)    s_flash--;
        if (s_pickmsg > 0)  s_pickmsg--;

        if (s_hp < prev_hp && !s_dead) sfx_play(SFX_HURT);   /* damage feedback */
        if (!prev_won  && s_won)       sfx_play(SFX_WIN);
        if (!prev_dead && s_dead)      sfx_play(SFX_DEATH);
        prev_hp = s_hp; prev_dead = s_dead; prev_won = s_won;
        sfx_tick();

        floorcast();
        render3d();
        render_sprites();
        blit_upscale();
        draw_weapon();
        if (s_flash > 0) {                                   /* rm-rf flash */
            unsigned int* back = gfx_get_backbuffer();
            int SW = gfx_width(), vH = RH * 2;
            for (int y = 0; y < vH; ++y)
                for (int x = 0; x < SW; ++x) {
                    unsigned c = back[y * SW + x];
                    int r = ((c>>16)&255)+70; if (r>255) r=255;
                    int g = ((c>>8)&255)+50;  if (g>255) g=255;
                    int bl = (c&255)+40;      if (bl>255) bl=255;
                    back[y * SW + x] = RGB(r, g, bl);
                }
        }
        if (s_shake > 0) {                                   /* boss impact — amber jolt */
            unsigned int* back = gfx_get_backbuffer();
            int SW = gfx_width(), vH = RH * 2;
            int add = s_shake * 4;
            for (int y = 0; y < vH; ++y)
                for (int x = 0; x < SW; ++x) {
                    unsigned c = back[y * SW + x];
                    int r = ((c>>16)&255)+add;  if (r>255) r=255;
                    int g = ((c>>8)&255)+add/2; if (g>255) g=255;
                    back[y * SW + x] = RGB(r, g, (c&255));
                }
            s_shake--;
        }
        draw_hud();
        draw_boss_bar();

        if (!s_stolen && !s_dead) {                          /* steal prompt */
            int dx = px - (s_kcx*ONE + ONE/2), dy = py - (s_kcy*ONE + ONE/2);
            int adx = dx<0?-dx:dx, ady = dy<0?-dy:dy;
            if (adx < ONE*2 && ady < ONE*2) {
                int SW = gfx_width();
                gfx_draw_str(SW/2 - 12*8, RH*2 - 64, " [E] VOLER LE KERNEL ",
                             RGB(0x70, 0xFF, 0x90), RGB(0x04, 0x12, 0x08));
            }
        }

        if (s_pickmsg > 0) {                                 /* pickup toast */
            int SW = gfx_width();
            const char* msg = (s_pickkind == PK_HEALTH) ? " +25 VIE (RAM) "
                                                        : " +MUNITIONS (CELLULE) ";
            unsigned col = (s_pickkind == PK_HEALTH) ? RGB(0x70,0xFF,0x90)
                                                     : RGB(0xFF,0xE0,0x80);
            int len = 0; while (msg[len]) len++;
            gfx_draw_str(SW/2 - len*4, RH*2 - 92, msg, col, RGB(0x08,0x0C,0x10));
        }

        if (s_won) {
            int SW = gfx_width(), SH = gfx_height();
            gfx_fill_rect(0, SH/2 - 52, SW, 104, RGB(0x06, 0x1A, 0x0A));
            gfx_draw_hline(0, SH/2 - 52, SW, RGB(0x40, 0xFF, 0x70));
            gfx_draw_hline(0, SH/2 + 51, SW, RGB(0x40, 0xFF, 0x70));
            gfx_draw_str(SW/2 - 13*8, SH/2 - 34, "LE KERNEL EST A TOI, INTRUS",
                         RGB(0x70, 0xFF, 0x90), GFX_TRANSPARENT);
            gfx_draw_str(SW/2 - 15*8, SH/2 - 12, "GREG 1er ET DRAKKAR SONT TOMBES",
                         RGB(0xFF, 0xE0, 0x90), GFX_TRANSPARENT);
            char b[16]; u2s(s_kills, b);
            gfx_draw_str(SW/2 - 8*8, SH/2 + 12, "PURGES:", RGB(0x60,0xC0,0x70), GFX_TRANSPARENT);
            gfx_draw_str(SW/2 - 8*8 + 64, SH/2 + 12, b, RGB(0xFF,0xE0,0x90), GFX_TRANSPARENT);
            gfx_draw_str(SW/2 - 8*8, SH/2 + 32, "ESC pour quitter",
                         RGB(0xFF, 0xC0, 0x40), GFX_TRANSPARENT);
        }

        if (s_dead && !s_won) {
            int SW = gfx_width(), SH = gfx_height();
            gfx_fill_rect(0, SH / 2 - 40, SW, 80, RGB(0x30, 0x00, 0x00));
            gfx_draw_str(SW / 2 - 21 * 8, SH / 2 - 20, "KERNEL PANIC - TU AS ETE PURGE",
                         RGB(0xFF, 0x60, 0x50), GFX_TRANSPARENT);
            gfx_draw_str(SW / 2 - 8 * 8, SH / 2 + 4, "ESC pour quitter",
                         RGB(0xFF, 0xC0, 0x40), GFX_TRANSPARENT);
        }

        gfx_swap_buffers();

        frame_at += 3;                                   /* ~33 fps           */
        while ((long)(jiffies - frame_at) < 0) { }
    }

    sfx_stop();                                          /* guarantee silence */
    ps2mouse_set_capture(0);
    kb_game_capture(0);
}
