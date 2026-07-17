#include "../include/vga.h"
#include "../include/keyboard.h"
#include "../include/ports.h"
#include "../include/gfx.h"
#include "../include/event.h"
#include "../include/kernel_tests.h"
#include "../include/tty.h"
#include "../include/vfs.h"
#include "../include/Kernel/timer_c.h"
#include "../include/Kernel/ps2mouse_c.h"
#include "../include/net.h"
#include "../include/ata.h"
#include "../include/acpi.h"
#include "../include/elf.h"
#include "../include/user_bin_data.h"     /* embedded flat isolated Ring-3 program */
#include "../include/userapp_elf_data.h"  /* embedded C ELF isolated Ring-3 program */


#define BUFFER_SIZE       256
#define MAX_FILES         64
#define FILENAME_SIZE     24
#define FILE_CONTENT_SIZE 8192   /* per-file byte cap; fits real ELF programs (> 4 KB) */
#define HEADER_HEIGHT     3
#define HISTORY_SIZE      20
#define MAX_ENV_VARS      16
#define ENV_VAR_LEN       32

#define KEY_ESC   0x1B
#define KEY_TAB   '\t'

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

#define TYPE_FILE 0
#define TYPE_DIR  1


typedef struct {
    char name[FILENAME_SIZE];
    char content[FILE_CONTENT_SIZE];
    int  size;
    int  type;
    int  parent_id;
    int  id;
    int  exists;
} File;

File         file_system[MAX_FILES];
int          current_dir_id = 0;
unsigned int rand_seed      = 12345;


char history[HISTORY_SIZE][BUFFER_SIZE];
int  history_count = 0;

int  history_nav   = -1;


char env_keys[MAX_ENV_VARS][ENV_VAR_LEN];
char env_vals[MAX_ENV_VARS][ENV_VAR_LEN];
int  env_count = 0;


#define MAX_ALIASES 16
char alias_names[MAX_ALIASES][32];
char alias_cmds [MAX_ALIASES][BUFFER_SIZE];
int  alias_count = 0;


const char* pipe_stdin = 0;

static int casino_balance = 1000;
static int casino_best    = 1000;

static char gregos_passwd[32]   = "admin";
static char gregos_hostname[64] = "gregos";
static int  last_exit_code      = 0;
static char current_user[32]    = "root";

#define HEAP_SIZE (32 * 1024 * 1024)
static unsigned char k_heap[HEAP_SIZE];

/* ── Free-list allocator ──────────────────────────────────────────────────
   Header (8 bytes) precedes every payload.  Blocks are contiguous; the next
   block is found by advancing (header + payload) bytes from the current one.
   kfree() coalesces adjacent free blocks in a single forward pass.          */
typedef struct KBlock { unsigned int size; unsigned int used; } KBlock;
#define KBLOCK_HDR ((unsigned int)sizeof(KBlock))   /* 8 */

static int k_heap_ready = 0;

static void heap_init(void) {
    KBlock* b = (KBlock*)k_heap;
    b->size = HEAP_SIZE - KBLOCK_HDR;
    b->used = 0;
    k_heap_ready = 1;
}

void* kmalloc(unsigned int size) {
    if (size == 0) return 0;
    size = (size + 3) & ~3u;
    if (!k_heap_ready) heap_init();
    KBlock* b = (KBlock*)k_heap;
    while ((unsigned char*)b < k_heap + HEAP_SIZE) {
        if (!b->used && b->size >= size) {
            /* Split only if the remainder fits a header + at least 4 bytes */
            if (b->size >= size + KBLOCK_HDR + 4) {
                KBlock* nx = (KBlock*)((unsigned char*)b + KBLOCK_HDR + size);
                nx->size = b->size - size - KBLOCK_HDR;
                nx->used = 0;
                b->size  = size;
            }
            b->used = 1;
            return (void*)((unsigned char*)b + KBLOCK_HDR);
        }
        b = (KBlock*)((unsigned char*)b + KBLOCK_HDR + b->size);
    }
    return 0;
}

void kfree(void* p) {
    if (!p) return;
    KBlock* b = (KBlock*)((unsigned char*)p - KBLOCK_HDR);
    if (!b->used) return;   /* double-free guard */
    b->used = 0;
    /* Forward pass: coalesce every pair of adjacent free blocks */
    b = (KBlock*)k_heap;
    while ((unsigned char*)b < k_heap + HEAP_SIZE) {
        KBlock* nx = (KBlock*)((unsigned char*)b + KBLOCK_HDR + b->size);
        if (!b->used && (unsigned char*)nx < k_heap + HEAP_SIZE && !nx->used)
            b->size += KBLOCK_HDR + nx->size; /* merge, don't advance */
        else
            b = nx;
    }
}

unsigned int kmalloc_used(void) {
    if (!k_heap_ready) return 0;
    unsigned int used = 0;
    const KBlock* b = (const KBlock*)k_heap;
    while ((const unsigned char*)b < k_heap + HEAP_SIZE) {
        if (b->used) used += KBLOCK_HDR + b->size;
        b = (const KBlock*)((const unsigned char*)b + KBLOCK_HDR + b->size);
    }
    return used;
}
unsigned int kmalloc_total(void) { return HEAP_SIZE; }

static int           current_theme   = 0;
static unsigned char theme_fg[]  = { 0x0F, 0x0A, 0x0E, 0x00, 0x0F };
static unsigned char theme_bg[]  = { 0x01, 0x00, 0x04, 0x03, 0x05 };
static unsigned char theme_sep[] = { 0x09, 0x02, 0x0C, 0x0B, 0x0D };
static const char*   theme_name[]= { "default","matrix","fire","ice","violet" };
#define NUM_THEMES 5


void execute_command(char* input);
void draw_interface(void);
void itoa(int n, char* buf);
int  fs_save(void);
int  fs_load(void);
void fs_sync(void);
void fs_mark_dirty(void);
int  fs_is_dirty(void);
void fs_format(void);
void cmd_sort_flags(const char* fname, int reverse, int numeric, int unique);
static void start_games_gui(void);
static void start_fileexplorer(void);

#define MB2_TAG_FB   8
#define MB2_TAG_END  0

typedef struct {
    unsigned int   type;
    unsigned int   size;
} __attribute__((packed)) MB2Tag;

typedef struct {
    unsigned int   type;
    unsigned int   size;
    unsigned long long fb_addr;
    unsigned int   fb_pitch;
    unsigned int   fb_width;
    unsigned int   fb_height;
    unsigned char  fb_bpp;
    unsigned char  fb_type;
    unsigned short reserved;
} __attribute__((packed)) MB2TagFB;

/* ── GregOS / Drakkar colour palette ─────────────────────────────── */
#define XP_DESK    GFX_RGB(0x06,0x0C,0x1C)   /* midnight navy wallpaper    */
#define XP_TBA     GFX_RGB(0x08,0x2A,0x0E)   /* dark forest green (title L) */
#define XP_TBB     GFX_RGB(0x1A,0x68,0x22)   /* brighter green  (title R)  */
#define XP_BORD    GFX_RGB(0x04,0x14,0x06)   /* very dark green border     */
#define XP_BODY    GFX_RGB(0x10,0x18,0x10)   /* dark body                  */
#define XP_BODSH   GFX_RGB(0x08,0x10,0x08)   /* body shadow                */
#define XP_BODLT   GFX_RGB(0x28,0x50,0x28)   /* body light edge            */
#define XP_TSK_T   GFX_RGB(0x06,0x08,0x06)   /* near-black taskbar top     */
#define XP_TSK_B   GFX_RGB(0x02,0x04,0x02)   /* black taskbar bottom       */
#define XP_TSK_HL  GFX_RGB(0x18,0x60,0x18)   /* green highlight line       */
#define XP_SRTG    GFX_RGB(0x16,0x78,0x16)   /* Greg button light green    */
#define XP_SRTD    GFX_RGB(0x08,0x44,0x08)   /* Greg button dark green     */
#define XP_CLSG    GFX_RGB(0xE5,0x48,0x34)   /* close btn light (keep red) */
#define XP_CLSD    GFX_RGB(0xC8,0x12,0x0B)   /* close btn dark             */
#define XP_BTNB    GFX_RGB(0x14,0x44,0x1C)   /* dark green btn light       */
#define XP_BTND    GFX_RGB(0x08,0x24,0x0E)   /* dark green btn dark        */
#define TWHT       GFX_RGB(0xFF,0xFF,0xFF)
#define TSHD       GFX_RGB(0x00,0x00,0x50)
#define TERM_BG    GFX_RGB(0x0C,0x0C,0x10)
#define TERM_FG    GFX_RGB(0xC8,0xFF,0xC8)
#define TERM_CUR   GFX_RGB(0x00,0xFF,0x00)
#define ICON_SEL   GFX_RGB(0x10,0x28,0x80)
/* keep these aliases for legacy code inside commands */
#define GUI_TERM_BG  TERM_BG
#define GUI_TERM_FG  TERM_FG
#define GUI_TERM_CURSOR TERM_CUR

/* ── Layout ──────────────────────────────────────────────────────── */
#define TITLE_H    25
#define WIN_BORDER  3
#define BTN_W      22
#define BTN_H      19
#define TASKBAR_H  30
#define ICON_W     48
#define ICON_H     48
#define ICON_LABEL_H 16
#define N_ICONS     5

/* keep old names used in command code */
#define BORDER      WIN_BORDER
#define BTN_SZ      BTN_W

#define TWIN_COLS  80
#define TWIN_ROWS  30

typedef struct {
    int x, y, w, h;
    const char* title;
    char  buf[TWIN_ROWS][TWIN_COLS + 1];
    int   cur_row, cur_col;
    int   scroll_off;
    int   focused;
} GUIWindow;

static GUIWindow g_twin;     /* interactive terminal (VGA mirror-connected) */
static GUIWindow g_wstatic;  /* read-only info window (files/system/games)  */
static int gui_mode      = 0;
static int gui_game_mode = 0; /* 1 = TUI game running, blit VGA→fb in irq0 */
static int g_in_launcher = 0; /* suppress desktop redraw when returning from launcher */

/* desk_state: 0=desktop  1=terminal  2=static info window */
static int desk_state = 0;
static int sel_icon   = 0;

static void gui_window_draw(GUIWindow* win);
static void gui_twin_clear(GUIWindow* win);
static void gui_twin_flush(GUIWindow* win);
static void gui_twin_putc(GUIWindow* win, char c);
static void gui_twin_print(GUIWindow* win, const char* s);
static void gui_desktop_draw(void);
static void gui_desktop_init(void);
static void gui_taskbar_update(void);
static void gui_vga_blit(void);
static void gui_game_start(void);
static void gui_refresh(void);
static void gui_open_files(void);
static void gui_open_sysinfo(void);
static void gui_open_games(void);


int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, int n) {
    for (int i = 0; i < n; i++) {
        if (s1[i] != s2[i]) return (unsigned char)s1[i] - (unsigned char)s2[i];
        if (!s1[i]) return 0;
    }
    return 0;
}

void strcpy(char* dest, const char* src) {
    int i = 0;
    while (src[i]) { dest[i] = src[i]; i++; }
    dest[i] = '\0';
}

void strncpy(char* dest, const char* src, int n) {
    int i = 0;
    while (i < n-1 && src[i]) { dest[i] = src[i]; i++; }
    dest[i] = '\0';
}

void strcat(char* dest, const char* src) {
    int d = 0, s = 0;
    while (dest[d]) d++;
    while (src[s]) { dest[d++] = src[s++]; }
    dest[d] = '\0';
}

int strlen(const char* str) {
    int n = 0; while (str[n]) n++; return n;
}

int startswith(const char* str, const char* prefix) {
    while (*prefix) { if (*prefix++ != *str++) return 0; }
    return 1;
}


int strcontains(const char* haystack, const char* needle) {
    int nlen = strlen(needle);
    for (int i = 0; haystack[i]; i++) {
        if (strncmp(haystack + i, needle, nlen) == 0) return 1;
    }
    return 0;
}

static int char_lower(char c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return (unsigned char)c;
}

static int strcontains_nocase(const char* hay, const char* needle) {
    int nlen = strlen(needle);
    for (int i = 0; hay[i]; i++) {
        int ok = 1;
        for (int j = 0; j < nlen; j++) {
            if (!hay[i+j] || char_lower(hay[i+j]) != char_lower(needle[j])) { ok=0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

void expand_env(const char* in, char* out, int maxlen) {
    static int expand_depth = 0;
    int i = 0, j = 0;
    while (in[i] && j < maxlen - 1) {
        if (in[i] == '$') {
            i++;
            if (in[i] == '(' && expand_depth < 4) {
                i++;
                char cmd[BUFFER_SIZE]; int ci = 0; int depth = 1;
                while (in[i] && depth > 0 && ci < BUFFER_SIZE-1) {
                    if (in[i] == '(') depth++;
                    else if (in[i] == ')') { if (--depth == 0) { i++; break; } }
                    cmd[ci++] = in[i++];
                }
                cmd[ci] = '\0';
                expand_depth++;
                term_capture_start();
                execute_command(cmd);
                const char* cap = term_capture_end();
                expand_depth--;
                int cl = strlen(cap);
                while (cl > 0 && (cap[cl-1] == '\n' || cap[cl-1] == '\r')) cl--;
                if (j + cl < maxlen - 1) { for (int k=0;k<cl;k++) out[j++]=cap[k]; }
            } else if (in[i] == '?') {
                i++;
                char nb[12]; itoa(last_exit_code, nb);
                int vl = strlen(nb);
                if (j + vl < maxlen-1) { strcpy(out+j, nb); j += vl; }
            } else if (in[i] == '$') {
                i++; out[j++] = '1';
            } else if (in[i] == '0') {
                i++;
                const char* s = "gregsh";
                while (*s && j < maxlen-1) out[j++] = *s++;
            } else {
                char varname[ENV_VAR_LEN]; int k = 0;
                while (in[i] && (in[i]=='_'||(in[i]>='A'&&in[i]<='Z')||(in[i]>='a'&&in[i]<='z')||(in[i]>='0'&&in[i]<='9')) && k < ENV_VAR_LEN-1)
                    varname[k++] = in[i++];
                varname[k] = '\0';
                int found = 0;
                for (int e = 0; e < env_count; e++) {
                    if (strcmp(env_keys[e], varname) == 0) {
                        int vl = strlen(env_vals[e]);
                        if (j + vl < maxlen-1) { strcpy(out+j, env_vals[e]); j += vl; }
                        found = 1; break;
                    }
                }
                (void)found;
            }
        } else {
            out[j++] = in[i++];
        }
    }
    out[j] = '\0';
}

int atoi(const char* str) {
    int res = 0, neg = 0, i = 0;
    if (str[0] == '-') { neg = 1; i = 1; }
    for (; str[i]; i++) {
        if (str[i] >= '0' && str[i] <= '9') res = res * 10 + str[i] - '0';
    }
    return neg ? -res : res;
}

void itoa(int n, char* buf) {
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    if (n == -2147483648) { strcpy(buf, "-2147483648"); return; }

    char tmp[12]; int i = 0;
    if (n < 0) { buf[0] = '-'; buf++; n = -n; }
    while (n > 0) { tmp[i++] = (n % 10) + '0'; n /= 10; }
    for (int j = 0; j < i; j++) buf[j] = tmp[i-1-j];
    buf[i] = '\0';
}

void term_print_int(int n) {
    char buf[12]; itoa(n, buf); term_print(buf);
}

void memset(void* ptr, int val, int size) {
    unsigned char* p = (unsigned char*)ptr;
    for (int i = 0; i < size; i++) p[i] = (unsigned char)val;
}

void* memcpy(void* dst, const void* src, unsigned int n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (unsigned int i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

int memcmp(const void* a, const void* b, unsigned int n) {
    const unsigned char* pa = (const unsigned char*)a;
    const unsigned char* pb = (const unsigned char*)b;
    for (unsigned int i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}


unsigned char get_rtc_register(int reg) {
    port_byte_out(CMOS_ADDRESS, reg);
    return port_byte_in(CMOS_DATA);
}

int bcd_to_bin(unsigned char bcd) {
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

void get_time_string(char* buffer) {
    unsigned char h = bcd_to_bin(get_rtc_register(0x04));
    unsigned char m = bcd_to_bin(get_rtc_register(0x02));
    rand_seed += bcd_to_bin(get_rtc_register(0x00));
    buffer[0] = (h/10)+'0'; buffer[1] = (h%10)+'0';
    buffer[2] = ':';
    buffer[3] = (m/10)+'0'; buffer[4] = (m%10)+'0';
    buffer[5] = '\0';
}

void get_date_string(char* buffer) {
    unsigned char day  = bcd_to_bin(get_rtc_register(0x07));
    unsigned char mon  = bcd_to_bin(get_rtc_register(0x08));
    unsigned char year = bcd_to_bin(get_rtc_register(0x09));
    buffer[0] = (day/10)+'0'; buffer[1] = (day%10)+'0';
    buffer[2] = '/';
    buffer[3] = (mon/10)+'0'; buffer[4] = (mon%10)+'0';
    buffer[5] = '/';
    buffer[6] = '2'; buffer[7] = '0';
    buffer[8] = (year/10)+'0'; buffer[9] = (year%10)+'0';
    buffer[10] = '\0';
}


int rand(void) {
    rand_seed = rand_seed * 1103515245 + 12345;
    return (unsigned int)(rand_seed / 65536) % 32768;
}


void get_cwd_string(char* buffer) {
    if (current_dir_id == 0) { strcpy(buffer, "/"); return; }
    for (int i = 0; i < MAX_FILES; i++) {
        if (file_system[i].exists && file_system[i].type == TYPE_DIR
            && file_system[i].id == current_dir_id) {
            buffer[0] = '/';
            strcpy(buffer+1, file_system[i].name);
            return;
        }
    }
    strcpy(buffer, "/");
}


int find_file_path(const char* path) {
    if (!path || !path[0]) return -1;
    int dir = current_dir_id;

    if (path[0] == '/') {
        dir = 0;

        path++;
    }



    char comp[FILENAME_SIZE];
    while (path[0]) {
        int j = 0;
        while (path[j] && path[j] != '/') { if (j < FILENAME_SIZE-1) comp[j] = path[j]; j++; }
        comp[j] = '\0';
        path += j;
        if (path[0] == '/') path++;
        if (!comp[0] || strcmp(comp, ".") == 0) continue;
        if (strcmp(comp, "..") == 0) {
            if (dir == 0) continue;
            for (int i = 0; i < MAX_FILES; i++)
                if (file_system[i].exists && file_system[i].type == TYPE_DIR && file_system[i].id == dir)
                    { dir = file_system[i].parent_id; break; }
            continue;
        }


        if (!path[0]) {
            for (int i = 0; i < MAX_FILES; i++)
                if (file_system[i].exists && strcmp(file_system[i].name, comp) == 0 && file_system[i].parent_id == dir)
                    return i;
            return -1;
        }


        int found = 0;
        for (int i = 0; i < MAX_FILES; i++)
            if (file_system[i].exists && file_system[i].type == TYPE_DIR
                && strcmp(file_system[i].name, comp) == 0 && file_system[i].parent_id == dir)
                { dir = file_system[i].id; found = 1; break; }
        if (!found) return -1;
    }


    for (int i = 0; i < MAX_FILES; i++)
        if (file_system[i].exists && file_system[i].type == TYPE_DIR && file_system[i].id == dir)
            return i;
    return -1;
}

int find_file(const char* name) {
    if (name[0] == '/' || (name[0] == '.' && (name[1] == '/' || name[1] == '.')))
        return find_file_path(name);
    for (int i = 0; i < MAX_FILES; i++) {
        if (file_system[i].exists
            && strcmp(file_system[i].name, name) == 0
            && file_system[i].parent_id == current_dir_id)
            return i;
    }
    return -1;
}

int next_free_slot(void) {
    for (int i = 0; i < MAX_FILES; i++)
        if (!file_system[i].exists) return i;
    return -1;
}


void reboot(void) {
    unsigned char good = 0x02;
    while (good & 0x02) good = port_byte_in(0x64);
    port_byte_out(0x64, 0xFE);
    __asm__("hlt");
}


struct gdt_entry {
    unsigned short limit_low;
    unsigned short base_low;
    unsigned char  base_mid;
    unsigned char  access;
    unsigned char  granularity;
    unsigned char  base_high;
} __attribute__((packed));

struct gdt_ptr {
    unsigned short limit;
    unsigned int   base;
} __attribute__((packed));

/* 32-bit Task State Segment — 104 bytes, CPU uses ss0/esp0 on Ring3→Ring0 */
struct tss_entry_t {
    unsigned int prev_tss;
    unsigned int esp0, ss0;
    unsigned int esp1, ss1;
    unsigned int esp2, ss2;
    unsigned int cr3;
    unsigned int eip, eflags;
    unsigned int eax, ecx, edx, ebx;
    unsigned int esp, ebp, esi, edi;
    unsigned int es, cs, ss, ds, fs, gs;
    unsigned int ldt;
    unsigned short trap;
    unsigned short iomap_base;
} __attribute__((packed));

static struct gdt_entry   g_gdt[6];
static struct gdt_ptr     g_gdtp;
static unsigned char      k_ring0_stack[4096];
static struct tss_entry_t tss_entry;

extern void gdt_flush(struct gdt_ptr*);
extern void tss_flush(void);
extern void jump_usermode(unsigned int eip, unsigned int user_esp);
extern void syscall_stub(void);

/* syscall_handler() is implemented in kernel/Syscall.cpp
   (Kernel::Syscall::dispatch — see include/Kernel/Syscall.hpp).
   The register-frame struct kept here documents the pusha layout.         */
struct registers_t {
    unsigned int edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
};
/* extern void syscall_handler(void* frame); — defined in kernel/Syscall.cpp */

/* ── user_gui_test_app — runs entirely in Ring 3 (CPL=3) ─────────────────
   Bounces a 40×40 red square across the top of the VBE framebuffer by
   calling sys_fill_rect (INT 0x80, eax=2) on every iteration.
   The scheduler pre-empts it every 10 ms so the GUI stays responsive.   */
static void __attribute__((unused)) user_gui_test_app(void) {
    unsigned int x   = 10u;
    unsigned int col = 0x00CC0000u;   /* red */
    int dir = 1;
    while (1) {
        /* "b"=EBX  "D"=EDI — compiler places x/col there directly.
           eax/ecx/edx/esi are set inside the asm body and listed as clobbers. */
        __asm__ volatile (
            "movl $2,  %%eax\n"    /* sys_fill_rect          */
            "movl $10, %%ecx\n"    /* y = 10 (below taskbar) */
            "movl $40, %%edx\n"    /* w                      */
            "movl $40, %%esi\n"    /* h                      */
            "int  $0x80\n"
            : : "b"(x), "D"(col)
            : "eax", "ecx", "edx", "esi"
        );
        for (volatile unsigned int d = 0; d < 150000u; ++d);
        if (dir > 0) { x += 4u; if (x >= 756u) { dir = -1; col = 0x000000CCu; } }
        else         { x -= 4u; if (x <= 10u)  { dir =  1; col = 0x00CC0000u; } }
    }
}

/* ── user_ring3_demo — a real Ring-3 (CPL=3) program ─────────────────────
   Exercises the Phase-4 syscall interface end-to-end from userland. It never
   touches kernel memory or calls kernel functions directly — every effect
   (print, allocate, draw, query time/pid, exit) goes through INT 0x80.
   Spawned on demand by the shell `ring3` command; runs once, then SYS_EXIT. */

/* Linux-i386 syscall numbers — mirror Kernel::SyscallNumber (Syscall.hpp).  */
#define SC_EXIT       1u
#define SC_FILL_RECT  2u
#define SC_WRITE      3u
#define SC_GET_TICKS  5u
#define SC_MMAP       7u
#define SC_MUNMAP     8u
#define SC_GET_PID   12u

/* ≤4-register syscall wrappers (eax=nr, ebx/ecx/edx=args). FILL_RECT needs a
   5th register (edi), so it is issued inline with immediates instead.        */
static inline unsigned int u_sc0(unsigned int nr) {
    unsigned int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr) : "memory"); return r;
}
static inline unsigned int u_sc1(unsigned int nr, unsigned int a) {
    unsigned int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "b"(a) : "memory"); return r;
}
static inline unsigned int u_sc3(unsigned int nr, unsigned int a, unsigned int b, unsigned int c) {
    unsigned int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "b"(a), "c"(b), "d"(c) : "memory"); return r;
}

/* Self-contained (no kernel calls): pure computation on the user stack. */
static int u_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }
static int u_utoa(unsigned int v, char* b) {
    char t[12]; int i = 0;
    if (!v) { b[0] = '0'; b[1] = 0; return 1; }
    while (v && i < 11) { t[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0; while (i) b[j++] = t[--i]; b[j] = 0; return j;
}
static void u_utohex(unsigned int v, char* b) {
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; i++) {
        unsigned int n = (v >> ((7 - i) * 4)) & 0xFu;
        b[2 + i] = (char)(n < 10 ? '0' + n : 'A' + n - 10);
    }
    b[10] = '\0';
}
static void u_write(const char* s) {
    u_sc3(SC_WRITE, 1u, (unsigned int)s, (unsigned int)u_strlen(s));
}

static void user_ring3_demo(void) {
    char num[16];
    u_write("\n[Ring3] Bonjour depuis l'espace utilisateur (CPL=3)!\n");

    unsigned int pid = u_sc0(SC_GET_PID);
    u_write("[Ring3] SYS_GET_PID   -> pid=");     u_utoa(pid, num);   u_write(num); u_write("\n");

    unsigned int ticks = u_sc0(SC_GET_TICKS);
    u_write("[Ring3] SYS_GET_TICKS -> jiffies="); u_utoa(ticks, num); u_write(num); u_write("\n");

    /* SYS_MMAP -> write pattern -> read back -> SYS_MUNMAP */
    unsigned int p = u_sc1(SC_MMAP, 256u);
    int ok = 0;
    if (p) {
        volatile unsigned char* mem = (volatile unsigned char*)p;
        for (int i = 0; i < 256; i++) mem[i] = (unsigned char)(i ^ 0x5A);
        ok = 1;
        for (int i = 0; i < 256; i++) if (mem[i] != (unsigned char)(i ^ 0x5A)) { ok = 0; break; }
        u_sc1(SC_MUNMAP, p);
    }
    u_write(ok ? "[Ring3] SYS_MMAP 256B ecrit+relu+SYS_MUNMAP : OK\n"
               : "[Ring3] SYS_MMAP a echoue\n");

    /* Confirmation rectangle top-right via SYS_FILL_RECT (immediates in asm). */
    unsigned int color = ok ? 0x0000CC00u : 0x00CC0000u;
    __asm__ volatile(
        "movl $2,   %%eax\n"
        "movl $748, %%ebx\n"
        "movl $4,   %%ecx\n"
        "movl $44,  %%edx\n"
        "movl $12,  %%esi\n"
        "int  $0x80\n"
        : : "D"(color) : "eax", "ebx", "ecx", "edx", "esi", "memory"
    );

    u_write("[Ring3] Termine - appel de SYS_EXIT.\n");
    u_sc1(SC_EXIT, 0u);
    for (;;) { }   /* SYS_EXIT never returns; guard against fallthrough */
}

/* ── user_ring3_crash — a *misbehaving* Ring-3 process ───────────────────
   Reads an unmapped address (0x60000000, well past the 48 MB identity map).
   At CPL=3 this raises a Page Fault the kernel's fault handler now catches:
   it kills only this process and the shell keeps running — proof that a
   faulty userland program can no longer take the whole system down.       */
static void user_ring3_crash(void) {
    u_write("\n[Ring3] Lecture d'une adresse non mappee (0x60000000)...\n");
    for (volatile unsigned int d = 0; d < 3000000u; ++d);  /* let the line flush */
    volatile unsigned int* bad = (volatile unsigned int*)0x60000000u;
    unsigned int v = *bad;                 /* #PF at CPL=3 → handler kills us */
    (void)v;
    u_write("[Ring3] (cette ligne ne doit jamais s'afficher)\n");
    u_sc1(SC_EXIT, 0u);
    for (;;) { }
}

/* ── user_vm_demo — runs in its OWN address space (private page directory) ─
   Writes a pid-derived magic to VA 0xC0000000 and re-reads it in a loop. Two
   instances run concurrently, each mapping a DIFFERENT physical page at this
   SAME virtual address, so each only ever reads back its own magic — proof
   of per-process address-space isolation.                                   */
static void user_vm_demo(void) {
    volatile unsigned int* page = (volatile unsigned int*)0xC0000000u;
    unsigned int pid   = u_sc0(SC_GET_PID);
    unsigned int magic = (pid << 24) | 0x00ABCDEu;   /* distinct per pid */
    char buf[16];

    *page = magic;
    int ok = 1;
    for (int r = 0; r < 10; r++) {
        for (volatile unsigned int d = 0; d < 700000u; ++d);  /* let the sibling run */
        if (*page != magic) { ok = 0; break; }   /* someone else's write leaked in? */
        *page = magic;
    }

    u_write("[VM] pid=");            u_utoa(pid, buf);    u_write(buf);
    u_write(" a ecrit ");            u_utohex(magic, buf); u_write(buf);
    u_write(" @0xC0000000, relu ");  u_utohex(*page, buf); u_write(buf);
    u_write(ok ? "  -> espace isole OK\n" : "  -> COLLISION (pas isole)!\n");
    u_sc1(SC_EXIT, 0u);
    for (;;) { }
}

static void gdt_set_entry(int i, unsigned int base, unsigned int limit,
                          unsigned char access, unsigned char gran) {
    g_gdt[i].base_low    = base & 0xFFFF;
    g_gdt[i].base_mid    = (base >> 16) & 0xFF;
    g_gdt[i].base_high   = (base >> 24) & 0xFF;
    g_gdt[i].limit_low   = limit & 0xFFFF;
    g_gdt[i].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    g_gdt[i].access      = access;
}

static void tss_init(void) {
    unsigned char* p = (unsigned char*)&tss_entry;
    for (unsigned int i = 0; i < sizeof(tss_entry); i++) p[i] = 0;
    tss_entry.ss0        = 0x10;   /* kernel data selector */
    tss_entry.esp0       = (unsigned int)(k_ring0_stack + sizeof(k_ring0_stack));
    tss_entry.iomap_base = (unsigned short)sizeof(tss_entry);
}

void tss_set_esp0(unsigned int esp0) { tss_entry.esp0 = esp0; }

static void gdt_install(void) {
    g_gdtp.limit = (unsigned short)(sizeof(g_gdt) - 1);
    g_gdtp.base  = (unsigned int)g_gdt;
    gdt_set_entry(0, 0, 0,       0x00, 0x00);   /* null         → 0x00 */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xCF);   /* kernel code  → 0x08 */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xCF);   /* kernel data  → 0x10 */
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xCF);   /* user code    → 0x1B (DPL=3) */
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xCF);   /* user data    → 0x23 (DPL=3) */
    tss_init();
    gdt_set_entry(5,                             /* TSS          → 0x2B */
        (unsigned int)&tss_entry,
        (unsigned int)sizeof(tss_entry) - 1,
        0x89, 0x00);   /* P=1 DPL=0 S=0 Type=1001 (32-bit TSS Available) */
    gdt_flush(&g_gdtp);
    tss_flush();
}

static unsigned int page_dir [1024] __attribute__((aligned(4096)));
static unsigned int page_tab0[1024] __attribute__((aligned(4096)));

static unsigned int fb_phys_for_paging = 0;

/* Physical address of the kernel page directory (identity-mapped, so == its
   virtual address). Published to the scheduler via vm_kernel_cr3() so it can
   restore the kernel address space when switching to a non-isolated thread. */
unsigned int g_kernel_cr3 = 0;

static void paging_install(void) {
    unsigned int cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 0x10u;
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));

    /* 0x07 = Present | R/W | User — bit 2 required for Ring 3 access */
    for (int i = 0; i < 1024; i++)
        page_tab0[i] = (unsigned int)(i * 0x1000) | 0x07u;
    page_dir[0] = (unsigned int)page_tab0 | 0x07u;
    for (int i = 1; i < 1024; i++) page_dir[i] = 0x02u;

    /* Identity-map 4-48 MB via PSE 4 MB pages.
       Kernel BSS (k_heap 32 MB + back-buffers + .rodata BMP) can reach ~37 MB;
       map 11 entries (4-48 MB) to leave comfortable headroom. */
    for (unsigned int i = 1; i <= 11; i++)
        page_dir[i] = (i << 22) | 0x87u;   /* 0x87 = PSE | Present | R/W | User */

    if (fb_phys_for_paging != 0) {
        unsigned int pde  = fb_phys_for_paging >> 22;
        unsigned int fb4m = fb_phys_for_paging & 0xFFC00000u;
        if (pde < 1024)     page_dir[pde]   = fb4m              | 0x87u;
        if (pde+1 < 1024)   page_dir[pde+1] = (fb4m+0x400000u) | 0x87u;
    }

    g_kernel_cr3 = (unsigned int)page_dir;
    __asm__ volatile("mov %0, %%cr3" : : "r"((unsigned int)page_dir));
    unsigned int cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

/* Identity-map (supervisor, 4 MB PSE) the region containing `phys` in the
   KERNEL page directory, then flush the TLB. Used by the ACPI driver: QEMU
   places its tables near the top of low RAM (e.g. ~255 MB with -m 256M),
   far above the 0-48 MB boot identity map. Idempotent; only ever extends
   the kernel space, never user-visible mappings (0x83 = PSE|RW|P, no User).
   Boot-time only (before per-process directories are cloned).             */
void paging_map_4mb(unsigned int phys) {
    unsigned int pde = phys >> 22;
    if (page_dir[pde] & 1u) return;                 /* already mapped        */
    page_dir[pde] = (phys & 0xFFC00000u) | 0x83u;
    unsigned int cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");   /* TLB flush */
}

/* ── Per-process virtual memory (Phase 4) ────────────────────────────────
   Each isolated user process gets its own page directory: a clone of the
   kernel page_dir (so kernel code/data/stacks/GDT/IDT/TSS + framebuffer stay
   mapped identically in every address space) PLUS one private 4 KB page
   mapped at virtual address 0xC0000000. Two processes therefore see DIFFERENT
   physical memory at the SAME virtual address — the essence of per-process
   address spaces. The scheduler reloads CR3 on every context switch.       */
#define VM_MAX_PROC  6            /* one per background thread slot */
#define VM_USER_VA   0xC0000000u  /* PDE index 768, PTE index 0     */
static unsigned int proc_pgdir[VM_MAX_PROC][1024] __attribute__((aligned(4096)));
static unsigned int proc_pgtab[VM_MAX_PROC][1024] __attribute__((aligned(4096)));
static unsigned int proc_priv [VM_MAX_PROC][1024] __attribute__((aligned(4096)));
static int          proc_used [VM_MAX_PROC];
static int          proc_heapnext[VM_MAX_PROC];   /* per-process user-heap bump */

/* Build a fresh isolated address space; returns its page-dir physical address
   (== virtual, identity-mapped) to use as cr3, or 0 if the pool is full.    */
unsigned int vm_create_addrspace(void) {
    int idx = -1;
    for (int i = 0; i < VM_MAX_PROC; i++) if (!proc_used[i]) { idx = i; break; }
    if (idx < 0) return 0;
    proc_used[idx] = 1;

    /* Clone every kernel PDE so the whole kernel stays mapped identically. */
    for (int i = 0; i < 1024; i++) proc_pgdir[idx][i] = page_dir[i];

    /* Private page, zero-filled, wired in at VA 0xC0000000 (User|RW|Present). */
    for (int i = 0; i < 1024; i++) proc_priv[idx][i] = 0;
    proc_pgtab[idx][0] = (unsigned int)proc_priv[idx] | 0x07u;
    for (int i = 1; i < 1024; i++) proc_pgtab[idx][i] = 0x02u;  /* not present */
    proc_pgdir[idx][VM_USER_VA >> 22] = (unsigned int)proc_pgtab[idx] | 0x07u;

    return (unsigned int)proc_pgdir[idx];
}

/* C bridge read by Kernel::Scheduler::tick() to restore the kernel dir. */
unsigned int vm_kernel_cr3(void) { return g_kernel_cr3; }

/* ── Fully-isolated address space (exec) ─────────────────────────────────
   Unlike vm_create_addrspace() (which keeps the kernel User-accessible for
   the simple vmtest demo), this builds a *hardened* space for a real user
   program: every kernel PDE is cloned with the User bit CLEARED, so the
   kernel becomes supervisor-only — a Ring-3 process cannot read or write any
   kernel memory. The program's own code and stack live in dedicated User
   pages at fixed virtual addresses:

     VA 0x40000000  user code+data (one 4 KB page, the flat binary is copied here)
     VA 0x40001000  user stack     (one 4 KB page; ESP starts at 0x40002000)

   Both fall in PDE 256 (0x40000000 >> 22), so one page table covers them.   */
#define VM_UCODE_VA  0x40000000u
#define VM_USTK_VA   0x40001000u
#define VM_USTK_TOP  0x40002000u
static unsigned int proc_ucode[VM_MAX_PROC][1024] __attribute__((aligned(4096)));
static unsigned int proc_ustk [VM_MAX_PROC][1024] __attribute__((aligned(4096)));

unsigned int vm_create_isolated(const unsigned char* code, unsigned int code_len) {
    int idx = -1;
    for (int i = 0; i < VM_MAX_PROC; i++) if (!proc_used[i]) { idx = i; break; }
    if (idx < 0) return 0;
    if (code_len > 4096u) return 0;   /* one code page only */
    proc_used[idx] = 1;

    /* Clone kernel PDEs, clearing the User bit (bit 2) on present entries so
       the kernel is supervisor-only in this space.                         */
    for (int i = 0; i < 1024; i++) {
        unsigned int e = page_dir[i];
        if (e & 1u) e &= ~0x04u;      /* present → drop User → supervisor-only */
        proc_pgdir[idx][i] = e;
    }

    /* Copy the flat program into the user code page; zero the stack page. */
    unsigned char* dst = (unsigned char*)proc_ucode[idx];
    for (unsigned int i = 0; i < code_len; i++) dst[i] = code[i];
    for (unsigned int i = code_len; i < 4096u; i++) dst[i] = 0;
    for (int i = 0; i < 1024; i++) proc_ustk[idx][i] = 0;

    /* Page table for PDE 256: PTE 0 → code page, PTE 1 → stack page (User). */
    for (int i = 0; i < 1024; i++) proc_pgtab[idx][i] = 0x02u;   /* not present */
    proc_pgtab[idx][0] = (unsigned int)proc_ucode[idx] | 0x07u;  /* User|RW|P   */
    proc_pgtab[idx][1] = (unsigned int)proc_ustk [idx] | 0x07u;
    proc_pgdir[idx][VM_UCODE_VA >> 22] = (unsigned int)proc_pgtab[idx] | 0x07u;

    return (unsigned int)proc_pgdir[idx];
}

/* ── Load a real ELF program into an isolated address space (exec of an ELF) ─
   Parses a static ET_EXEC (linked at 0x40000000), copies its PT_LOAD segments
   into private User pages, adds a User stack page, and hardens the space so the
   kernel is supervisor-only. Writes the entry point + initial user ESP to the
   out-params and returns the new cr3, or 0 on any failure.                    */
#define ELF_MAX_PAGES  8
#define ELF_STK_VA     0x40200000u
#define ELF_STK_TOP    0x40201000u
static unsigned int proc_elfpg[VM_MAX_PROC][ELF_MAX_PAGES][1024] __attribute__((aligned(4096)));

unsigned int vm_exec_elf_args(const unsigned char* elf, unsigned int len,
                              const char* const* argv, int argc,
                              unsigned int* out_entry, unsigned int* out_esp) {
    if (len < sizeof(Elf32_Ehdr)) return 0;
    const Elf32_Ehdr* eh = (const Elf32_Ehdr*)elf;
    if (eh->e_ident[0] != ELF_MAGIC0 || eh->e_ident[1] != ELF_MAGIC1 ||
        eh->e_ident[2] != ELF_MAGIC2 || eh->e_ident[3] != ELF_MAGIC3) return 0;
    if (eh->e_ident[4] != ELFCLASS32 || eh->e_machine != EM_386 ||
        eh->e_type != ET_EXEC) return 0;
    if (eh->e_phoff == 0 || eh->e_phnum == 0) return 0;

    /* Span of all PT_LOAD segments. */
    unsigned int min_va = 0xFFFFFFFFu, max_va = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr* ph = (const Elf32_Phdr*)(elf + eh->e_phoff + (unsigned)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        if (ph->p_vaddr < min_va) min_va = ph->p_vaddr;
        if (ph->p_vaddr + ph->p_memsz > max_va) max_va = ph->p_vaddr + ph->p_memsz;
    }
    if (min_va == 0xFFFFFFFFu) return 0;
    unsigned int base = min_va & ~0xFFFu;
    if ((base >> 22) != (VM_UCODE_VA >> 22)) return 0;   /* must live in PDE 256 */
    unsigned int npages = (max_va - base + 4095u) / 4096u;
    if (npages == 0 || npages > (ELF_MAX_PAGES - 1)) return 0;   /* reserve 1 for stack */

    int idx = -1;
    for (int i = 0; i < VM_MAX_PROC; i++) if (!proc_used[i]) { idx = i; break; }
    if (idx < 0) return 0;
    proc_used[idx] = 1;

    /* Zero the pool, then copy each segment to (p_vaddr - base). */
    unsigned char* flat = (unsigned char*)proc_elfpg[idx];
    for (unsigned int i = 0; i < ELF_MAX_PAGES * 4096u; i++) flat[i] = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr* ph = (const Elf32_Phdr*)(elf + eh->e_phoff + (unsigned)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_filesz == 0) continue;
        if (ph->p_offset + ph->p_filesz > len)          { proc_used[idx] = 0; return 0; }
        unsigned int dst = ph->p_vaddr - base;
        if (dst + ph->p_filesz > npages * 4096u)         { proc_used[idx] = 0; return 0; }
        for (unsigned int b = 0; b < ph->p_filesz; b++) flat[dst + b] = elf[ph->p_offset + b];
    }

    /* Harden: clone kernel PDEs with User cleared (kernel = supervisor-only). */
    for (int i = 0; i < 1024; i++) {
        unsigned int e = page_dir[i];
        if (e & 1u) e &= ~0x04u;
        proc_pgdir[idx][i] = e;
    }
    for (int i = 0; i < 1024; i++) proc_pgtab[idx][i] = 0x02u;
    unsigned int base_pte = (base >> 12) & 0x3FFu;
    for (unsigned int i = 0; i < npages; i++)
        proc_pgtab[idx][base_pte + i] = (unsigned int)proc_elfpg[idx][i] | 0x07u;
    proc_pgtab[idx][(ELF_STK_VA >> 12) & 0x3FFu] =
        (unsigned int)proc_elfpg[idx][ELF_MAX_PAGES - 1] | 0x07u;
    proc_pgdir[idx][base >> 22] = (unsigned int)proc_pgtab[idx] | 0x07u;

    proc_heapnext[idx] = 0;    /* fresh per-process heap */

    /* Build the initial user stack: argc, the argv[] pointer array (ending in
       NULL), and the argument strings, all inside the process's own stack page.
       ESP points at argc, so a naked _start can read argc = *(int*)esp and
       argv = (char**)(esp+4). Always built, even for argc == 0, so _start never
       dereferences past the top of the stack page.                          */
    unsigned char* stk = (unsigned char*)proc_elfpg[idx][ELF_MAX_PAGES - 1];
    if (argc < 0) argc = 0;
    if (argc > 8) argc = 8;                 /* bound the argv array */
    unsigned int sp = 4096;                 /* byte offset = top of the stack page */
    unsigned int str_va[8];
    for (int i = 0; i < argc; i++) {
        int slen = 0; while (argv[i][slen]) slen++;
        if (sp < (unsigned)(slen + 1) + 64u) { argc = i; break; }   /* out of room */
        sp -= (unsigned)(slen + 1);
        for (int j = 0; j <= slen; j++) stk[sp + j] = (unsigned char)argv[i][j];
        str_va[i] = ELF_STK_VA + sp;
    }
    sp &= ~3u;                              /* 4-byte align the pointer frame */
    sp -= (1u + (unsigned)argc + 1u) * 4u;  /* argc + argv[argc] + NULL */
    unsigned int* w = (unsigned int*)(stk + sp);
    w[0] = (unsigned int)argc;
    for (int i = 0; i < argc; i++) w[1 + i] = str_va[i];
    w[1 + argc] = 0;                        /* argv[argc] = NULL */

    *out_entry = eh->e_entry;
    *out_esp   = ELF_STK_VA + sp;
    return (unsigned int)proc_pgdir[idx];
}

/* No-argument convenience wrapper. */
unsigned int vm_exec_elf(const unsigned char* elf, unsigned int len,
                         unsigned int* out_entry, unsigned int* out_esp) {
    return vm_exec_elf_args(elf, len, 0, 0, out_entry, out_esp);
}

/* ── Per-process user heap (SYS_MMAP into the caller's own address space) ──
   The earlier flat SYS_MMAP handed back kernel-heap memory, which an isolated
   process cannot touch (kernel is supervisor-only). These functions instead map
   fresh User pages into the *calling* process's own page directory at a user VA
   in the 0x50000000 region, so an isolated program gets memory it can actually
   use. Called from Kernel::Syscall::sys_mmap/sys_munmap via C bridges.

   Allocation is a simple per-process bump allocator over HEAP_PAGES frames;
   munmap unmaps the page at the given VA. The syscall runs at CPL=0 in the
   caller's address space, so the current CR3 identifies which process it is.  */
#define HEAP_VA_BASE  0x50000000u          /* PDE 320 (0x50000000 >> 22) */
#define HEAP_PAGES    8
static unsigned int proc_heappg [VM_MAX_PROC][HEAP_PAGES][1024] __attribute__((aligned(4096)));
static unsigned int proc_heappt [VM_MAX_PROC][1024] __attribute__((aligned(4096)));
/* proc_heapnext[] declared with the VM pools above (used by vm_exec_elf too). */

static int vm_slot_for_cr3(unsigned int cr3) {
    for (int i = 0; i < VM_MAX_PROC; i++)
        if (proc_used[i] && (unsigned int)proc_pgdir[i] == cr3) return i;
    return -1;
}

/* Release the per-process VM pool slot backing address space `cr3` so it can be
   reused by a future exec. Called by the scheduler's reaper when a Ring-3
   process exits or is killed (see Scheduler::reap_zombies). The page-directory
   and page-table pools are static kernel memory, so "freeing" a slot just marks
   it available again and resets its user-heap bump pointer — no kfree here. It
   is only ever called for a cr3 that is NOT the one currently loaded (the reaper
   never reaps the running thread), so clearing the slot is safe. Returns 1 if a
   slot was released, 0 if cr3 was the shared kernel space (nothing to do).     */
int vm_release_cr3(unsigned int cr3) {
    if (cr3 == 0 || cr3 == g_kernel_cr3) return 0;
    int idx = vm_slot_for_cr3(cr3);
    if (idx < 0) return 0;
    proc_used[idx]     = 0;
    proc_heapnext[idx] = 0;
    return 1;
}

unsigned int vm_mmap_current(unsigned int size) {
    unsigned int cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    int idx = vm_slot_for_cr3(cr3);
    if (idx < 0 || size == 0) return 0;                /* not isolated → caller falls back */
    unsigned int npages = (size + 4095u) / 4096u;
    if ((unsigned int)proc_heapnext[idx] + npages > HEAP_PAGES) return 0;   /* heap full */

    unsigned int start = (unsigned int)proc_heapnext[idx];
    unsigned int base_pte = (HEAP_VA_BASE >> 12) & 0x3FFu;   /* = 0 */
    for (unsigned int k = 0; k < npages; k++) {
        unsigned int* frame = proc_heappg[idx][start + k];
        for (int j = 0; j < 1024; j++) frame[j] = 0;
        proc_heappt[idx][base_pte + start + k] = (unsigned int)frame | 0x07u;  /* User|RW|P */
    }
    proc_heapnext[idx] += (int)npages;
    proc_pgdir[idx][HEAP_VA_BASE >> 22] = (unsigned int)proc_heappt[idx] | 0x07u;
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");   /* flush TLB */
    return HEAP_VA_BASE + start * 4096u;
}

int vm_munmap_current(unsigned int addr) {
    unsigned int cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    int idx = vm_slot_for_cr3(cr3);
    if (idx < 0) return 0;
    if (addr < HEAP_VA_BASE || addr >= HEAP_VA_BASE + HEAP_PAGES * 4096u) return 0;
    unsigned int pg = (addr - HEAP_VA_BASE) / 4096u;
    proc_heappt[idx][((HEAP_VA_BASE >> 12) & 0x3FFu) + pg] = 0x02u;   /* unmap → not present */
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    return 1;
}

/* ── User-pointer validation (copy_from_user / copy_to_user foundation) ──────
   A Ring-3 process passes raw pointers as syscall arguments. Because the syscall
   handler runs at CPL 0 *inside the caller's own address space*, a naive
   dereference would let userland aim a pointer at kernel memory — arbitrary
   read / write / free. vm_validate_user_range walks the CURRENT cr3's page
   tables and accepts a range only if every page in it is Present + User (U/S=1)
   and, when writing, R/W=1. For an isolated process the kernel is mapped
   supervisor-only (User bit cleared), so kernel pointers are rejected; a shared-
   space thread (kernel User-mapped — the legacy ring3/vmtest demos) validates
   against its own map, which is correct for it. Single CPU + a syscall entry
   that is not re-entered means there is no TOCTOU window between validation and
   use here (to revisit under SMP — see ROADMAP.md Phase 10).                   */
int vm_validate_user_range(unsigned int addr, unsigned int len, int need_write) {
    if (len == 0) return 1;                       /* nothing dereferenced      */
    if (addr + len < addr) return 0;              /* range wraps the address space */
    unsigned int cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    unsigned int* pd = (unsigned int*)(cr3 & 0xFFFFF000u);
    unsigned int first = addr & ~0xFFFu;
    unsigned int last  = (addr + len - 1u) & ~0xFFFu;
    for (unsigned int va = first; ; va += 0x1000u) {
        unsigned int pde = pd[va >> 22];
        if (!(pde & 0x1u)) return 0;              /* PDE not present           */
        if (!(pde & 0x4u)) return 0;              /* supervisor-only → deny    */
        if (need_write && !(pde & 0x2u)) return 0;
        if (!(pde & 0x80u)) {                     /* not a 4 MB PSE page       */
            unsigned int* pt  = (unsigned int*)(pde & 0xFFFFF000u);
            unsigned int  pte = pt[(va >> 12) & 0x3FFu];
            if (!(pte & 0x1u)) return 0;          /* PTE not present           */
            if (!(pte & 0x4u)) return 0;          /* supervisor-only → deny    */
            if (need_write && !(pte & 0x2u)) return 0;
        }
        if (va == last) break;
    }
    return 1;
}

/* Copy a NUL-terminated string from user space into a kernel buffer, validating
   each byte's page as it goes. Returns the string length (excluding the NUL) on
   success, or -1 if the pointer is invalid or the string is not terminated
   within maxlen bytes. dst must hold at least maxlen bytes.                    */
int vm_copy_user_string(char* dst, unsigned int uaddr, int maxlen) {
    if (!dst || maxlen <= 0) return -1;
    for (int i = 0; i < maxlen; i++) {
        if (!vm_validate_user_range(uaddr + (unsigned int)i, 1u, 0)) return -1;
        char c = *(const volatile char*)(uaddr + (unsigned int)i);
        dst[i] = c;
        if (c == 0) return i;
    }
    return -1;   /* not NUL-terminated within maxlen → reject */
}

struct idt_entry {
    unsigned short offset_low;
    unsigned short selector;
    unsigned char  zero;
    unsigned char  type_attr;
    unsigned short offset_high;
} __attribute__((packed));

struct idt_ptr {
    unsigned short limit;
    unsigned int   base;
} __attribute__((packed));

#define IDT_SIZE 256
static struct idt_entry g_idt[IDT_SIZE];
static struct idt_ptr   g_idtp;

static int    mouse_prev_buttons = 0;    /* edge detection (main loop only) */

static unsigned long __attribute__((unused)) last_full_repaint = 0;
static int    cursor_prev_x = -1;
static int    cursor_prev_y = -1;
static unsigned int cursor_save[16][8];

/* PS/2 mouse driver moved to kernel/PS2Mouse.cpp */

volatile unsigned long jiffies = 0;

/* C bridge to the C++ Scheduler (kernel/Scheduler.cpp) */
extern void scheduler_tick_c(void);
extern void scheduler_spawn(void (*entry)(void));
extern void scheduler_spawn_user(void (*entry)(void));
extern void scheduler_spawn_user_vm(void (*entry)(void), unsigned int cr3);
extern void scheduler_spawn_user_at(unsigned int entry_va, unsigned int user_esp, unsigned int cr3);
extern unsigned int vm_create_addrspace(void);
extern unsigned int vm_create_isolated(const unsigned char* code, unsigned int code_len);
extern unsigned int vm_exec_elf(const unsigned char* elf, unsigned int len,
                                unsigned int* out_entry, unsigned int* out_esp);
extern unsigned int vm_exec_elf_args(const unsigned char* elf, unsigned int len,
                                     const char* const* argv, int argc,
                                     unsigned int* out_entry, unsigned int* out_esp);
extern void scheduler_activate(void);

/* ── Background validation thread ──────────────────────────────────────
   Writes a spinning indicator to VGA text position [79] (top-right).
   Visible only when the screen is in VGA text mode (e.g. early boot or
   text fallback); in VBE/GUI mode it writes to the raw memory address.
   The point is purely to prove the scheduler context-switches correctly.  */
static void __attribute__((unused)) test_thread_func(void) {
    volatile unsigned short* vga = (volatile unsigned short*)0xB8000;
    /* Attribute 0x0A = bright green on black */
    static const unsigned char spin[4] = { '-', '\\', '|', '/' };
    unsigned int n = 0;
    while (1) {
        unsigned short cell = (unsigned short)(0x0A00u | spin[n & 3]);
        vga[79] = cell;          /* VGA text row 0, column 79 (top-right) */
        ++n;
        /* Burn ~5 ms at ~1 GHz to make the spinner visible at 100 fps */
        for (volatile unsigned int i = 0; i < 2000000u; i++) {}
    }
}

void irq0_handler(void) {
    jiffies++;
    if (gui_game_mode && !(jiffies & 1)) gui_vga_blit(); /* ~50fps VGA→fb blit */
    port_byte_out(0x20, 0x20);   /* EOI — re-arm PIC before the tick */
    scheduler_tick_c();          /* preempt current thread if needed  */
}

extern void irq0_stub(void);
extern void irq1_stub(void);
extern void irq12_stub(void);
extern void isr0_stub(void);
extern void isr6_stub(void);
extern void isr8_stub(void);
extern void isr13_stub(void);
extern void isr14_stub(void);

static void idt_set(unsigned short cs, int vec, void* handler) {
    unsigned int h = (unsigned int)handler;
    g_idt[vec].offset_low  = h & 0xFFFF;
    g_idt[vec].selector    = cs;
    g_idt[vec].zero        = 0;
    g_idt[vec].type_attr   = 0x8E;   /* P=1 DPL=0 32-bit interrupt gate */
    g_idt[vec].offset_high = (h >> 16) & 0xFFFF;
}

/* Install the RTL8139 RX interrupt gate. Called by net_init() once the NIC's
   IRQ line is known (PCI config 0x3C) — long after idt_install(), hence this
   runtime hook rather than a static idt_set() call. Gate is DPL=0 (0x8E).   */
extern void irq_net_stub(void);
void kernel_net_irq_install(unsigned int vector) {
    unsigned short cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    if (vector < IDT_SIZE) idt_set(cs, (int)vector, irq_net_stub);
}

/* DPL=3 gate — Ring 3 code can call this vector with INT */
static void idt_set_user(unsigned short cs, int vec, void* handler) {
    unsigned int h = (unsigned int)handler;
    g_idt[vec].offset_low  = h & 0xFFFF;
    g_idt[vec].selector    = cs;
    g_idt[vec].zero        = 0;
    g_idt[vec].type_attr   = 0xEE;   /* P=1 DPL=3 32-bit interrupt gate */
    g_idt[vec].offset_high = (h >> 16) & 0xFFFF;
}

static void idt_install(void) {

    unsigned short cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));

    g_idtp.limit = (unsigned short)(sizeof(g_idt) - 1);
    g_idtp.base  = (unsigned int)g_idt;

    for (int i = 0; i < IDT_SIZE; i++) {
        g_idt[i].offset_low = 0; g_idt[i].selector  = 0;
        g_idt[i].zero       = 0; g_idt[i].type_attr = 0;
        g_idt[i].offset_high = 0;
    }

    port_byte_out(0x20, 0x11); port_byte_out(0xA0, 0x11);
    port_byte_out(0x21, 0x20); port_byte_out(0xA1, 0x28);
    port_byte_out(0x21, 0x04); port_byte_out(0xA1, 0x02);
    port_byte_out(0x21, 0x01); port_byte_out(0xA1, 0x01);
    port_byte_out(0x21, 0xF8); port_byte_out(0xA1, 0xFF);

    /* CPU exceptions (ISR 0-31) */
    idt_set(cs, 0x00, isr0_stub);   /* #DE  Divide Error          */
    idt_set(cs, 0x06, isr6_stub);   /* #UD  Invalid Opcode        */
    idt_set(cs, 0x08, isr8_stub);   /* #DF  Double Fault          */
    idt_set(cs, 0x0D, isr13_stub);  /* #GP  General Protection    */
    idt_set(cs, 0x0E, isr14_stub);  /* #PF  Page Fault            */

    /* Hardware IRQs (PIC remapped to 0x20-0x2F) */
    idt_set(cs, 0x20, irq0_stub);
    idt_set(cs, 0x21, irq1_stub);
    idt_set(cs, 0x2C, irq12_stub);
    idt_set_user(cs, 0x80, syscall_stub);   /* INT 0x80 — callable from Ring 3 */

    timer_initialize_100hz();

    __asm__ volatile("lidt %0" : : "m"(g_idtp));
    kb_idt_active = 1;
    __asm__ volatile("sti");
}


static void nop_delay(int n);
static void hs_update(int game_idx, int score);

#define HS_NGAMES 10
static int hs_scores[HS_NGAMES] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};


static void beep_ok(void)      { timer_beep(880, 80);  timer_beep(1320, 120); }
static void beep_fail(void)    { timer_beep(330, 150); timer_beep(220, 200); }
static void beep_gameover(void){ timer_beep(440, 100); timer_beep(330, 100); timer_beep(220, 200); }
static void beep_tick(void)    { timer_beep(1200, 15); }
static void beep_nav(void)     { timer_beep(880, 12); }
static void beep_theme(void)   { timer_beep(523, 30); timer_beep(659, 30); timer_beep(784, 50); }
static void beep_notify(void)  { timer_beep(659, 40); timer_beep(880, 60); }

typedef struct { unsigned int f; unsigned int d; } MNote;

#define MQ  280000000
#define ME  140000000
#define MS   70000000
#define MH  560000000
#define MW 1120000000u
#define MdQ 420000000
#define MdE 210000000

static void music_play(const MNote* m) {
    for (int i = 0; m[i].d; i++) {
        int key = get_monitor_char();
        if (key == 0x1B) break;
        if (m[i].f) {
            timer_speaker_on(m[i].f);
            nop_delay(m[i].d);
            timer_speaker_off();
        } else {
            nop_delay(m[i].d);
        }
        nop_delay(18000000);
    }
    timer_speaker_off();
}

static const MNote sng_tetris[] = {
    {659,MQ},{494,ME},{523,ME},{587,MQ},{523,ME},{494,ME},
    {440,MQ},{440,ME},{523,ME},{659,MQ},{587,ME},{523,ME},
    {494,MdQ},{523,ME},{587,MQ},{659,MQ},
    {523,MQ},{440,MQ},{440,MQ},{0,ME},
    {587,MQ},{698,ME},{880,MQ},{784,ME},{698,ME},
    {659,MdQ},{523,ME},{659,MQ},{587,ME},{523,ME},
    {494,MQ},{494,ME},{523,ME},{587,MQ},{659,MQ},
    {523,MQ},{440,MQ},{440,MQ},{0,MQ},
    {0,0}
};

static const MNote sng_mario[] = {
    {659,ME},{659,ME},{0,ME},{659,ME},{0,ME},{523,ME},{659,MQ},
    {784,MH},{392,MH},
    {523,MQ},{0,ME},{392,MQ},{0,ME},{330,MdQ},{0,ME},
    {440,MQ},{494,MQ},{466,ME},{440,MQ},
    {392,MdE},{659,MdE},{784,MdE},{880,MH},
    {698,MQ},{784,MQ},{0,ME},{659,MQ},{523,MQ},{440,MQ},{392,MH},
    {0,0}
};

static const MNote sng_imperial[] = {
    {392,MQ},{392,MQ},{392,MQ},{311,MdE},{466,MS},
    {392,MQ},{311,MdE},{466,MS},{392,MH},
    {587,MQ},{587,MQ},{587,MQ},{622,MdE},{466,MS},
    {370,MQ},{311,MdE},{466,MS},{392,MH},
    {784,MQ},{392,ME},{392,MS},{784,MQ},{740,MdE},{698,MS},
    {659,ME},{622,ME},{659,MQ},{0,ME},{415,ME},{622,MQ},{587,MdE},{554,MS},
    {523,ME},{466,ME},{523,MQ},{0,ME},{311,ME},{370,MQ},{311,MdE},{392,MS},
    {392,MQ},{392,MdE},{784,MS},{0,ME},{392,MdE},{784,MS},{784,MH},
    {0,0}
};

static const MNote sng_zelda[] = {
    {523,MS},{659,MS},{784,MS},{1047,MdQ},{0,ME},
    {523,MS},{659,MS},{784,MS},{1047,MdQ},{0,MH},
    {784,MQ},{523,ME},{784,MQ},{1047,MH},
    {880,MQ},{784,ME},{698,ME},{784,MQ},{0,MH},
    {523,MS},{659,MS},{784,MS},{1047,MdQ},{0,ME},
    {880,MQ},{784,ME},{698,MdQ},{0,ME},
    {659,MH},{523,MH},
    {0,0}
};

static const MNote sng_gregos[] = {
    {523,ME},{659,ME},{784,ME},{1047,MQ},{784,ME},{659,ME},
    {523,MQ},{0,ME},{659,MQ},{784,MQ},{880,MH},
    {784,ME},{659,ME},{523,ME},{440,MQ},{0,ME},{523,ME},{659,ME},
    {784,MH},{0,MQ},
    {880,ME},{880,ME},{784,ME},{659,MQ},{784,ME},{880,ME},
    {1047,MH},{0,MH},
    {0,0}
};

static const struct { const char* name; const MNote* song; const char* desc; }
music_catalog[] = {
    {"tetris",   sng_tetris,   "Tetris       — Korobeiniki"},
    {"mario",    sng_mario,    "Mario        — Super Mario Bros theme"},
    {"imperial", sng_imperial, "Imperial     — Star Wars Imperial March"},
    {"zelda",    sng_zelda,    "Zelda        — Legend of Zelda theme"},
    {"gregos",   sng_gregos,   "GregOS       — Fanfare originale"},
    {0, 0, 0}
};

void cmd_music(const char* arg) {
    if (!arg || !arg[0] || strcmp(arg, "list") == 0) {
        term_set_color(0x0B, 0x00);
        term_print("\x0E Systeme Audio GregOS — PC Speaker\n\n");
        term_set_color(0x0F, 0x00);
        for (int i = 0; music_catalog[i].name; i++) {
            term_set_color(0x0E, 0x00); term_print("  ");
            term_print(music_catalog[i].name);
            term_set_color(0x07, 0x00); term_print("  ");
            term_print(music_catalog[i].desc);
            term_putc('\n');
        }
        term_set_color(0x08, 0x00);
        term_print("\nUsage: music <nom>  |  music list\n");
        term_print("       ESC pendant la lecture pour stopper\n");
        term_set_color(0x0F, 0x00);
        return;
    }
    for (int i = 0; music_catalog[i].name; i++) {
        if (strcmp(arg, music_catalog[i].name) == 0) {
            term_set_color(0x0E, 0x00);
            term_print("\x0E ");
            term_print(music_catalog[i].desc);
            term_print(" \x0E\n");
            term_set_color(0x0F, 0x00);
            music_play(music_catalog[i].song);
            return;
        }
    }
    term_set_color(0x0C, 0x00);
    term_print("Chanson inconnue. Tapez 'music list'.\n");
    term_set_color(0x0F, 0x00);
}

#undef MQ
#undef ME
#undef MS
#undef MH
#undef MW
#undef MdQ
#undef MdE


static void ui_fill(int x, int y, int w, int h, unsigned char fg, unsigned char bg) {
    term_set_color(fg, bg);
    for (int row = y; row < y + h; row++) {
        term_move_cursor(x, row);
        for (int col = 0; col < w; col++) term_putc(' ');
    }
}

static void __attribute__((unused)) ui_box(int x, int y, int w, int h, unsigned char fg, unsigned char bg) {
    term_set_color(fg, bg);
    term_move_cursor(x, y);
    term_putc('\xda');
    for (int i = 0; i < w - 2; i++) term_putc('\xc4');
    term_putc('\xbf');
    for (int row = y + 1; row < y + h - 1; row++) {
        term_move_cursor(x, row);          term_putc('\xb3');
        term_move_cursor(x + w - 1, row);  term_putc('\xb3');
    }
    term_move_cursor(x, y + h - 1);
    term_putc('\xc0');
    for (int i = 0; i < w - 2; i++) term_putc('\xc4');
    term_putc('\xd9');
}

static void ui_dbox(int x, int y, int w, int h, unsigned char fg, unsigned char bg) {
    term_set_color(fg, bg);
    term_move_cursor(x, y);
    term_putc('\xda');
    for (int i = 0; i < w - 2; i++) term_putc('\xc4');
    term_putc('\xbf');
    for (int row = y + 1; row < y + h - 1; row++) {
        term_move_cursor(x, row);          term_putc('\xb3');
        term_move_cursor(x + w - 1, row);  term_putc('\xb3');
    }
    term_move_cursor(x, y + h - 1);
    term_putc('\xc0');
    for (int i = 0; i < w - 2; i++) term_putc('\xc4');
    term_putc('\xd9');
}

static void ui_center_in(int x, int y, int w, const char* str,
                         unsigned char fg, unsigned char bg) {
    int len = strlen(str);
    int pad = (w - len) / 2;
    if (pad < 0) pad = 0;
    term_set_color(fg, bg);
    term_move_cursor(x + pad, y);
    term_print(str);
}

static void __attribute__((unused))
ui_hline(int y, char ch, unsigned char fg, unsigned char bg) {
    term_set_color(fg, bg);
    term_move_cursor(0, y);
    for (int i = 0; i < VGA_WIDTH; i++) term_putc(ch);
}

static void ui_progress(int x, int y, int w, int cur, int total,
                        unsigned char fg, unsigned char bg) {
    if (total <= 0) total = 1;
    int filled = (cur * w) / total;
    if (filled > w) filled = w;
    term_set_color(fg, bg);
    term_move_cursor(x, y);
    for (int i = 0; i < w; i++) term_putc(i < filled ? '\xdb' : '\xc4');
}


void draw_interface(void) {
    /* In GUI mode: never run the VGA text-mode header drawing.
       term_putc/term_set_color calls here would corrupt the TerminalEmulator
       buffer because tty_putc_hook intercepts them. Return early.         */
    if (gui_mode) {
        gui_game_mode = 0;
        if (!g_in_launcher) wm_draw();
        return;
    }
    term_init();
    unsigned char hfg = theme_fg[current_theme];
    unsigned char hbg = theme_bg[current_theme];
    unsigned char sfg = theme_sep[current_theme];

    char time_str[12]; get_time_string(time_str);
    char date_str[12]; get_date_string(date_str);

    term_set_color(hfg, hbg);
    term_move_cursor(0, 0);
    for (int i = 0; i < VGA_WIDTH; i++) term_putc(' ');
    term_move_cursor(0, 0);
    term_putc('\xdb'); term_putc('\xdb');
    term_set_color(0x0F, hbg); term_print(" GregOS");
    term_set_color(hfg, hbg);  term_print(" v2.0 ");
    term_putc('\xb3');
    term_set_color(0x0E, hbg); term_print(" \x04 ");
    term_set_color(hfg, hbg);  term_print(theme_name[current_theme]);
    term_set_color(0x0E, hbg); term_print(" \x04 ");
    term_set_color(hfg, hbg);

    int right_start = VGA_WIDTH - 2 - 10 - 1 - 5;
    term_move_cursor(right_start, 0);
    term_print(date_str);
    term_putc(' ');
    term_set_color(0x0F, hbg); term_print(time_str);
    term_set_color(hfg, hbg);
    term_putc(' ');
    term_putc('\xdb'); term_putc('\xdb');

    char cwd[32]; get_cwd_string(cwd);
    int fc = 0;
    for (int i = 0; i < MAX_FILES; i++) if (file_system[i].exists) fc++;

    term_set_color(hfg, hbg);
    term_move_cursor(0, 1);
    for (int i = 0; i < VGA_WIDTH; i++) term_putc(' ');
    term_move_cursor(0, 1);
    term_putc('\xdb'); term_putc('\xdb');
    term_set_color(0x0A, hbg); term_print(" root");
    term_set_color(hfg, hbg);  term_putc('\xc4');
    term_set_color(0x0B, hbg); term_print("@gregos");
    term_set_color(hfg, hbg);  term_putc(':');
    term_set_color(0x0E, hbg); term_print(cwd);
    term_set_color(hfg, hbg);  term_putc(' ');

    term_move_cursor(VGA_WIDTH - 24, 1);
    term_set_color(hfg, hbg); term_print("GC:");
    term_set_color(casino_balance < 100 ? 0x0C : 0x0A, hbg);
    term_print_int(casino_balance);
    term_set_color(hfg, hbg); term_print(" \xb3 f:");
    term_print_int(fc); term_putc('/'); term_print_int(MAX_FILES);
    term_putc(' ');
    term_putc('\xdb'); term_putc('\xdb');

    term_set_color(sfg, 0x00);
    term_move_cursor(0, 2);
    term_putc('\xc3');
    for (int i = 1; i < VGA_WIDTH - 1; i++) term_putc('\xc4');
    term_putc('\xb4');

    unsigned long sec = jiffies / 100;
    int uh = (int)(sec / 3600);
    int um = (int)((sec % 3600) / 60);

    term_set_color(hfg, hbg);
    term_move_cursor(0, VGA_HEIGHT - 1);
    for (int i = 0; i < VGA_WIDTH; i++) term_putc(' ');
    term_move_cursor(1, VGA_HEIGHT - 1);
    term_print("TAB=complete \xb3 \x18\x19=hist \xb3 ^C=cancel \xb3 ESC=exit");
    term_move_cursor(VGA_WIDTH - 26, VGA_HEIGHT - 1);
    term_print("up:");
    term_print_int(uh); term_putc('h');
    term_print_int(um); term_putc('m');
    term_print(" \xb3 help=cmds ");

    term_set_scroll_region(HEADER_HEIGHT, VGA_HEIGHT - 2);
    term_set_color(0x0F, 0x00);
    term_move_cursor(0, HEADER_HEIGHT);
    ps2mouse_cursor_show();
}


void print_logo(void) {
    term_print("\x1b[33m");
    term_print("    ==(W{==========-\n");
    term_print("      ||  (.--.)    ");
    term_print("\x1b[0m GregOS v2\n");
    term_print("\x1b[33m");
    term_print("      | \\_,|**|,__ \n");
    term_print("   ___/-==|  /`\\_. \n");
    term_print(" (^(~     `-'  _-~` \n");
    term_print("\x1b[0m\n");
}


void history_push(const char* cmd) {
    if (cmd[0] == '\0') return;


    if (history_count > 0 &&
        strcmp(history[(history_count-1) % HISTORY_SIZE], cmd) == 0) return;
    strcpy(history[history_count % HISTORY_SIZE], cmd);
    history_count++;
    history_nav = -1;
}


const char* history_get(int offset) {
    if (offset < 0 || offset >= history_count || offset >= HISTORY_SIZE) return 0;
    int idx = ((history_count - 1 - offset) % HISTORY_SIZE + HISTORY_SIZE) % HISTORY_SIZE;
    return history[idx];
}


void print_prompt(void) {
    char cwd[32]; get_cwd_string(cwd);
    /* Block 1: green bg + black text — user@host */
    term_print("\x1b[30;42m root \x1b[0m");
    /* Block 2: blue bg + bright-white text — current path */
    term_print("\x1b[97;44m ");
    term_print(cwd);
    term_print(" \x1b[0m");
    /* Chevron in bright green */
    term_print("\x1b[92m > \x1b[0m");
}


static const char* cmd_list[] = {
    "ls","cd","mkdir","touch","rm","cat","nano","sh","cp","mv","pwd",
    "echo","grep","wc","find","hexdump","history","tree","neofetch",
    "fortune","man","uname","sysinfo","date","whoami","calc","snake",
    "tetris","shutdown","reboot","clear","help","env","setenv",
    "matrix","cowsay","clock","banner","ps","top","df","free",
    "head","tail","sort","tac","sleep","stat","rev","uptime",
    "yes","seq","lolcat","write","invaders","theme",
    "diff","rot13","chmod","alias","unalias","which","base64","pong",
    "breakout","2048","minesweeper","simon","casino","roulette","blackjack","slots","plinko","atm","music","bc","cut","tr","sed","scores",
    "more","passwd","export","unset",
    "cal","ping","ifconfig","watch","sudo","tee","ln",
    "file","du","lspci","dmesg","id","groups","hostname","mount","umount",
    "crontab","type","wget","curl","traceroute","nslookup","nmap","ssh",
    "pkg","fdisk","expr","useradd","su","strace","kill","killall",
    "jobs","bg","fg","printenv","source","true","false","logout","exit",
    "test","printf","uniq","xargs","awk","time","mouse",
    "less","nl","paste",
    "sync","format","ring3","ring3crash","vmtest","exec","elfrun","run","browser","gregnet","web","paint","horloge","net","host","acpi",
    0
};

void do_tab_complete(char* buf, int* plen) {
    int len = *plen;
    if (len == 0) return;



    const char* matches[16];
    int  nmatch = 0;



    for (int i = 0; cmd_list[i]; i++) {
        if (strncmp(buf, cmd_list[i], len) == 0) {
            if (nmatch < 16) matches[nmatch++] = cmd_list[i];
        }
    }



    for (int i = 0; i < MAX_FILES && nmatch < 16; i++) {
        if (file_system[i].exists && file_system[i].parent_id == current_dir_id) {
            if (strncmp(buf, file_system[i].name, len) == 0)
                matches[nmatch++] = file_system[i].name;
        }
    }

    if (nmatch == 0) { beep_fail(); return; }

    if (nmatch == 1) {
        beep_tick();
        int ml = strlen(matches[0]);
        for (int i = len; i < ml; i++) {
            term_putc(matches[0][i]);
            buf[*plen] = matches[0][i];
            (*plen)++;
        }
        buf[*plen] = '\0';
    } else {
        beep_nav();
        term_putc('\n');
        for (int i = 0; i < nmatch; i++) {
            term_print(matches[i]);
            term_putc(' ');
        }
        term_putc('\n');

        print_prompt();
        term_print(buf);
    }
}


static void nano_draw(const char* content, int len, int cur, int view) {
    int ROWS = VGA_HEIGHT - 1;
    term_move_cursor(0, 1);
    int row = 0, col = 0, srow = 1;
    unsigned char saved_fg = 0x0F, saved_bg = 0x00;
    int cursor_drawn = 0;
    for (int r = 1; r < VGA_HEIGHT; r++) {
        term_move_cursor(0, r);
        for (int c2 = 0; c2 < VGA_WIDTH; c2++) term_putc(' ');
    }
    term_move_cursor(0, 1);
    for (int i = 0; i <= len && srow <= ROWS; i++) {
        if (row >= view && row < view + ROWS) {
            if (i == cur && !cursor_drawn) {
                term_set_color(0x00, 0x0F);
                term_move_cursor(col, row - view + 1);
                char ch = (i < len) ? content[i] : ' ';
                if (ch == '\n') ch = ' ';
                term_putc(ch);
                term_set_color(0x0F, 0x00);
                cursor_drawn = 1;
                if (i < len && content[i] != '\n') { col++; i++; }
                else if (i < len && content[i] == '\n') {
                    row++; col = 0; srow++;
                    if (srow <= ROWS) term_move_cursor(0, row - view + 1);
                    i++;
                }
                if (i > len) break;
                if (srow > ROWS) break;
            }
            if (i < len) {
                char ch = content[i];
                if (ch == '\n') {
                    row++; col = 0; srow++;
                    if (srow <= ROWS) term_move_cursor(0, row - view + 1);
                } else {
                    term_move_cursor(col, row - view + 1);
                    term_putc(ch >= 32 ? ch : '?');
                    col++;
                    if (col >= VGA_WIDTH) { col = 0; row++; srow++;
                        if (srow <= ROWS) term_move_cursor(0, row - view + 1);
                    }
                }
            } else if (i == len && !cursor_drawn) {
                term_set_color(0x00, 0x0F); term_putc(' '); term_set_color(0x0F, 0x00);
                cursor_drawn = 1;
                break;
            }
        } else if (i < len) {
            if (content[i] == '\n') { row++; col = 0; }
            else { col++; if (col >= VGA_WIDTH) { col = 0; row++; } }
        }
    }
    (void)saved_fg; (void)saved_bg;
}

void open_editor(int fi) {
    if (gui_mode) {
        tty_clear();   /* clear TerminalEmulator grid; skip VGA term_init in GUI */
    } else {
        term_init();
        gui_game_start();
    }
    char* content = file_system[fi].content;
    int len = strlen(content);
    int cur = len;
    int view = 0;
    int saved_flag = 0;

    auto void draw_header(void);
    void draw_header(void) {
        term_set_color(0x00, 0x07);
        term_move_cursor(0, 0);
        for (int i = 0; i < VGA_WIDTH; i++) term_putc(' ');
        term_move_cursor(1, 0); term_putc('\xdb'); term_putc('\xdb');
        term_print(" nano: "); term_print(file_system[fi].name);
        if (saved_flag) { term_move_cursor(VGA_WIDTH-15,0); term_set_color(0x02,0x07); term_print("  [Saved]  "); }
        term_move_cursor(VGA_WIDTH-26, 0); term_set_color(0x00,0x07);
        term_print("Ctrl+S=Save  ESC=Exit ");
        term_putc('\xdb'); term_putc('\xdb');
        term_set_color(0x0F, 0x00);
    }

    auto int cur_line(void);
    int cur_line(void) {
        int ln = 0;
        for (int i = 0; i < cur; i++) if (content[i] == '\n') ln++;
        return ln;
    }

    draw_header();
    nano_draw(content, len, cur, view);
    if (gui_mode) wm_draw();

    while (1) {
        int c = get_monitor_char();
        if (c == 0) { nop_delay(50000); continue; }
        saved_flag = 0;

        if (c == KEY_ESC) {
            content[len] = '\0'; file_system[fi].size = len;
            beep_tick(); draw_interface();
            term_print("\x1b[33mSaved & exit.\x1b[0m\n");
            return;
        }
        if (c == KEY_CTRL_S) {
            content[len] = '\0'; file_system[fi].size = len;
            beep_ok(); saved_flag = 1;
            draw_header(); nano_draw(content, len, cur, view);
            if (gui_mode) wm_draw();
            continue;
        }

        if (c == KEY_LEFT  && cur > 0)  cur--;
        else if (c == KEY_RIGHT && cur < len) cur++;
        else if (c == KEY_UP) {
            int col = 0, i = cur - 1;
            while (i >= 0 && content[i] != '\n') { col++; i--; }
            if (i >= 0) {
                int prev_end = i - 1;
                int prev_col = 0;
                while (prev_end >= 0 && content[prev_end] != '\n') { prev_col++; prev_end--; }
                if (col > prev_col) col = prev_col;
                cur = prev_end + 1 + col;
            }
        } else if (c == KEY_DOWN) {
            int i = cur;
            while (i < len && content[i] != '\n') i++;
            if (i < len) {
                int nl_pos = i; i++;
                int col = 0, j = cur;
                while (j > 0 && content[j-1] != '\n') { col++; j--; }
                int nc = nl_pos + 1 + col;
                int line_end = nl_pos + 1;
                while (line_end < len && content[line_end] != '\n') line_end++;
                if (nc > line_end) nc = line_end;
                cur = nc;
            }
        } else if (c == KEY_HOME || c == 1) {
            while (cur > 0 && content[cur-1] != '\n') cur--;
        } else if (c == KEY_END || c == 5) {
            while (cur < len && content[cur] != '\n') cur++;
        } else if (c == '\b' && cur > 0) {
            for (int i = cur-1; i < len-1; i++) content[i] = content[i+1];
            len--; cur--; content[len] = '\0';
        } else if (c == KEY_DELETE && cur < len) {
            for (int i = cur; i < len-1; i++) content[i] = content[i+1];
            len--; content[len] = '\0';
        } else if ((c == '\n' || c == '\r') && len < FILE_CONTENT_SIZE-1) {
            for (int i = len; i > cur; i--) content[i] = content[i-1];
            content[cur++] = '\n'; len++;
        } else if (c >= 32 && c <= 126 && len < FILE_CONTENT_SIZE-1) {
            for (int i = len; i > cur; i--) content[i] = content[i-1];
            content[cur++] = c; len++;
        }

        int cl = cur_line();
        if (cl < view) view = cl;
        if (cl >= view + (VGA_HEIGHT - 1)) view = cl - (VGA_HEIGHT - 2);
        draw_header();
        nano_draw(content, len, cur, view);
        if (gui_mode) wm_draw();
    }
}


#define SC_MAXLINES 128
static char  sc_buf[FILE_CONTENT_SIZE + 256];
static char* sc_lines[SC_MAXLINES];
static int   sc_n;

static void sc_set_var(const char* name, const char* val) {
    for (int i = 0; i < env_count; i++) {
        if (strcmp(env_keys[i], name) == 0) { strncpy(env_vals[i], val, ENV_VAR_LEN-1); return; }
    }
    if (env_count < MAX_ENV_VARS) {
        strncpy(env_keys[env_count], name, ENV_VAR_LEN-1);
        strncpy(env_vals[env_count], val, ENV_VAR_LEN-1);
        env_count++;
    }
}

static void sc_split(const char* content) {
    int len = strlen(content);
    if (len >= FILE_CONTENT_SIZE + 255) len = FILE_CONTENT_SIZE + 254;
    int i; for (i = 0; i < len; i++) sc_buf[i] = content[i]; sc_buf[i] = '\0';
    sc_n = 0;
    char* p = sc_buf;
    while (*p && sc_n < SC_MAXLINES) {
        while (*p == '\n') p++;
        if (!*p) break;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') continue;
        if (*p == '#') { while (*p && *p != '\n') p++; continue; }
        sc_lines[sc_n++] = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') *p++ = '\0';
    }
}

static int sc_find(int from, const char* kw) {
    int depth = 0;
    for (int i = from; i < sc_n; i++) {
        const char* ln = sc_lines[i];
        if (!ln || !*ln || *ln == '#') continue;
        if (startswith(ln,"if ")||strcmp(ln,"if")==0||startswith(ln,"while ")||startswith(ln,"for ")||startswith(ln,"case ")) depth++;
        if (depth == 0) {
            int kl = strlen(kw);
            if (strcmp(ln, kw) == 0 || (strncmp(ln, kw, kl)==0 && (ln[kl]==' '||ln[kl]==';'||ln[kl]=='\0')))
                return i;
        }
        if (depth > 0 && (strcmp(ln,"fi")==0||strcmp(ln,"done")==0||strcmp(ln,"esac")==0)) depth--;
    }
    return -1;
}

static char* sc_cond(const char* line) {
    static char cond[BUFFER_SIZE];
    const char* p = line;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    strncpy(cond, p, BUFFER_SIZE-1); cond[BUFFER_SIZE-1] = '\0';
    for (int i = strlen(cond)-1; i >= 0; i--) {
        if (cond[i] == ';') { cond[i] = '\0'; break; }
    }
    int l = strlen(cond);
    while (l > 0 && cond[l-1] == ' ') cond[--l] = '\0';
    return cond;
}

static void sc_exec(int from, int to);
static void sc_exec(int from, int to) {
    int i = from;
    while (i < to) {
        char* line = sc_lines[i];
        if (!line || !*line || *line == '#') { i++; continue; }
        if (strcmp(line,"then")==0||strcmp(line,"do")==0||
            strcmp(line,"fi")==0||strcmp(line,"done")==0||strcmp(line,"else")==0||
            strcmp(line,"esac")==0||strcmp(line,"in")==0) { i++; continue; }

        if (startswith(line, "if ")) {
            char* cond = sc_cond(line);
            int then_i = sc_find(i+1,"then");
            int else_i = sc_find((then_i>=0?then_i:i)+1,"else");
            int fi_i   = sc_find((then_i>=0?then_i:i)+1,"fi");
            last_exit_code = 0;
            char exp[BUFFER_SIZE]; expand_env(cond, exp, BUFFER_SIZE);
            execute_command(exp);
            int bs = then_i>=0 ? then_i+1 : i+1;
            int be = else_i>=0 ? else_i : (fi_i>=0 ? fi_i : to);
            if (last_exit_code == 0) sc_exec(bs, be);
            else if (else_i >= 0) sc_exec(else_i+1, fi_i>=0?fi_i:to);
            i = fi_i>=0 ? fi_i+1 : to; continue;
        }
        if (startswith(line, "while ")) {
            char* cond = sc_cond(line);
            int do_i   = sc_find(i+1,"do");
            int done_i = sc_find(do_i>=0?do_i+1:i+1,"done");
            int bs = do_i>=0 ? do_i+1 : i+1;
            int be = done_i>=0 ? done_i : to;
            for (int iter = 0; iter < 5000; iter++) {
                int kc = get_monitor_char();
                if (kc == 0x1B || kc == KEY_CTRL_C) break;
                last_exit_code = 0;
                char exp[BUFFER_SIZE]; expand_env(cond, exp, BUFFER_SIZE);
                execute_command(exp);
                if (last_exit_code != 0) break;
                sc_exec(bs, be);
            }
            i = done_i>=0 ? done_i+1 : to; continue;
        }
        if (startswith(line, "for ")) {
            const char* p = line + 4; while (*p==' ') p++;
            char vn[32]; int vi = 0;
            while (*p && *p!=' ' && vi<31) vn[vi++] = *p++;
            vn[vi] = '\0';
            while (*p==' ') p++;
            if (startswith(p,"in ")) p+=3; else if (startswith(p,"in\t")) p+=3;
            char words[BUFFER_SIZE]; strncpy(words, p, BUFFER_SIZE-1); words[BUFFER_SIZE-1]='\0';
            for (int ci=strlen(words)-1;ci>=0;ci--) { if(words[ci]==';'){words[ci]='\0';break;} }
            char exp_words[BUFFER_SIZE]; expand_env(words, exp_words, BUFFER_SIZE);
            int do_i   = sc_find(i+1,"do");
            int done_i = sc_find(do_i>=0?do_i+1:i+1,"done");
            int bs = do_i>=0?do_i+1:i+1, be = done_i>=0?done_i:to;
            char* wp = exp_words;
            while (*wp) {
                while (*wp==' ') wp++;
                if (!*wp) break;
                char word[64]; int wi=0;
                while (*wp&&*wp!=' '&&wi<63) word[wi++]=*wp++;
                word[wi]='\0';
                sc_set_var(vn, word);
                sc_exec(bs, be);
            }
            i = done_i>=0?done_i+1:to; continue;
        }
        if (startswith(line, "read ")) {
            const char* var = line+5; while (*var==' ') var++;
            char val[BUFFER_SIZE]; int vi2=0;
            while (vi2<BUFFER_SIZE-1) {
                int c = get_monitor_char(); if (!c){nop_delay(100000);continue;}
                if (c=='\n'||c=='\r') break;
                if (c=='\b'&&vi2>0){vi2--;term_putc('\b');continue;}
                if (c>=32&&c<127){val[vi2++]=(char)c;term_putc(c);}
            }
            val[vi2]='\0'; term_putc('\n');
            sc_set_var(var, val);
            i++; continue;
        }
        if (startswith(line, "local ") || startswith(line, "export ")) {
            char exp[BUFFER_SIZE]; expand_env(line, exp, BUFFER_SIZE);
            execute_command(exp);
            i++; continue;
        }
        if (startswith(line, "case ")) {
            const char* p = line + 5; while (*p==' ') p++;
            char subj_raw[64]; int sri=0;
            while (*p && *p!=' ' && !startswith(p," in") && sri<63) subj_raw[sri++]=*p++;
            subj_raw[sri]='\0';
            char subj[64]; expand_env(subj_raw, subj, 64);
            int esac_i = sc_find(i+1, "esac");
            int end = esac_i>=0 ? esac_i : to;
            int matched = 0;
            for (int ci = i+1; ci < end && !matched; ci++) {
                char* cl = sc_lines[ci];
                if (!cl || !*cl) continue;
                int clen = strlen(cl);
                if (strcmp(cl,"esac")==0) break;
                if (cl[clen-1]==')') {
                    char pat[64];
                    int plen = clen-1; if (plen > 63) plen = 63;
                    strncpy(pat, cl, plen+1); pat[plen]='\0';
                    int hit = (strcmp(pat,"*")==0) || (strcmp(pat,subj)==0);
                    if (!hit && pat[0]=='*' && strlen(pat)>1) {
                        const char* suf = pat+1;
                        int sl=strlen(subj), tl=strlen(suf);
                        hit = (sl>=tl && strcmp(subj+sl-tl,suf)==0);
                    }
                    if (!hit && pat[strlen(pat)-1]=='*') {
                        char pfx[64]; strncpy(pfx,pat,strlen(pat)-1); pfx[strlen(pat)-1]='\0';
                        hit = startswith(subj, pfx);
                    }
                    if (hit) {
                        matched = 1;
                        int body_start = ci+1;
                        int body_end = body_start;
                        while (body_end < end) {
                            char* bl = sc_lines[body_end];
                            if (!bl) { body_end++; continue; }
                            int bl2 = strlen(bl);
                            if (bl2>=2 && bl[bl2-1]==';' && bl[bl2-2]==';') {
                                sc_lines[body_end][bl2-2]='\0';
                                if (bl2-2>0) { sc_exec(body_start, body_end); execute_command(sc_lines[body_end]); }
                                else sc_exec(body_start, body_end);
                                sc_lines[body_end][bl2-2]=';';
                                break;
                            }
                            body_end++;
                        }
                        if (body_end >= end) sc_exec(body_start, end);
                    }
                }
            }
            i = end+1; continue;
        }
        if (strcmp(line,"esac")==0||strcmp(line,"in")==0) { i++; continue; }
        char exp[BUFFER_SIZE]; expand_env(line, exp, BUFFER_SIZE);
        execute_command(exp);
        i++;
    }
}

void run_script(const char* content) {
    sc_split(content);
    sc_exec(0, sc_n);
}


#define SNAKE_TICK    50000000

#define TETRIS_TICK   30000000

#define INVADERS_TICK 25000000

#define PONG_TICK     20000000

#define MATRIX_TICK   15000000


static void nop_delay(int n) { for (int i = 0; i < n; i++) __asm__ volatile("nop"); }


#define SN_W    40
#define SN_H    20
#define SN_OX    1
#define SN_OY    2
#define SN_MAX 300

void start_snake(void) {
    term_init();
    gui_game_start();
    static int sx[SN_MAX], sy[SN_MAX];
    int slen = 4;
    int dx = 1, dy = 0, ndx = 1, ndy = 0;
    int fx = SN_W/2+6, fy = SN_H/2;
    int score = 0, paused = 0, over = 0;

    sx[0]=SN_W/2; sy[0]=SN_H/2;
    for (int i=1;i<slen;i++) { sx[i]=sx[0]-i; sy[i]=sy[0]; }



    term_move_cursor(0,0);
    term_set_color(0x0A,0x00);
    term_print("SNAKE  Z/Q/S/D or arrows=dir  P=pause  ESC=quit");



    term_set_color(0x07,0x00);
    term_move_cursor(SN_OX-1, SN_OY-1);
    term_putc('\xDA'); for(int i=0;i<SN_W;i++) term_putc('\xC4'); term_putc('\xBF');
    for (int y=0;y<SN_H;y++) {
        term_move_cursor(SN_OX-1, SN_OY+y); term_putc('\xB3');
        term_move_cursor(SN_OX+SN_W, SN_OY+y); term_putc('\xB3');
    }
    term_move_cursor(SN_OX-1, SN_OY+SN_H);
    term_putc('\xC0'); for(int i=0;i<SN_W;i++) term_putc('\xC4'); term_putc('\xD9');



    term_move_cursor(SN_OX+SN_W+2, SN_OY+1);
    term_set_color(0x0E,0x00); term_print("Score:");
    term_move_cursor(SN_OX+SN_W+2, SN_OY+4);
    term_set_color(0x0B,0x00); term_print("Level:");
    term_move_cursor(SN_OX+SN_W+2, SN_OY+7);
    term_set_color(0x08,0x00); term_print("Len:");



    term_move_cursor(SN_OX+sx[0], SN_OY+sy[0]);
    term_set_color(0x0A,0x00); term_putc('@');
    for (int i=1;i<slen;i++) {
        term_move_cursor(SN_OX+sx[i], SN_OY+sy[i]);
        term_set_color(0x02,0x00); term_putc('o');
    }
    term_move_cursor(SN_OX+fx, SN_OY+fy);
    term_set_color(0x0C,0x00); term_putc('*');

    while (!over) {
        int tick = SNAKE_TICK - (score/50)*3000000;
        if (tick < 15000000) tick = 15000000;
        nop_delay(tick);

        int c = get_monitor_char();
        if (c == KEY_ESC) { draw_interface(); return; }
        if (c=='p'||c=='P') {
            paused = !paused;
            term_move_cursor(SN_OX+SN_W/2-3, SN_OY+SN_H/2);
            if (paused) { term_set_color(0x0E,0x00); term_print("PAUSED"); }
            else        { term_set_color(0x00,0x00); term_print("      "); }
        }
        if (paused) continue;

        if ((c=='z'||c==KEY_UP)    && dy==0) { ndx=0; ndy=-1; }
        if ((c=='s'||c==KEY_DOWN)  && dy==0) { ndx=0; ndy=1;  }
        if ((c=='q'||c==KEY_LEFT)  && dx==0) { ndx=-1;ndy=0;  }
        if ((c=='d'||c==KEY_RIGHT) && dx==0) { ndx=1; ndy=0;  }
        dx=ndx; dy=ndy;

        int tx=sx[slen-1], ty=sy[slen-1];
        for (int i=slen-1;i>0;i--) { sx[i]=sx[i-1]; sy[i]=sy[i-1]; }
        sx[0]+=dx; sy[0]+=dy;

        if (sx[0]<0||sx[0]>=SN_W||sy[0]<0||sy[0]>=SN_H) { over=1; break; }
        for (int i=1;i<slen;i++) if(sx[0]==sx[i]&&sy[0]==sy[i]) { over=1; break; }
        if (over) break;

        int ate=(sx[0]==fx&&sy[0]==fy);
        if (!ate) {
            term_move_cursor(SN_OX+tx, SN_OY+ty);
            term_set_color(0x00,0x00); term_putc(' ');
        } else {
            score+=10;
            if (slen<SN_MAX-1) slen++;


            for (int tries=0;tries<500;tries++) {
                int nfx=rand()%SN_W, nfy=rand()%SN_H;
                int ok=1;
                for (int i=0;i<slen;i++) if(sx[i]==nfx&&sy[i]==nfy){ok=0;break;}
                if (ok) { fx=nfx; fy=nfy; break; }
            }
            term_move_cursor(SN_OX+fx, SN_OY+fy);
            term_set_color(0x0C,0x00); term_putc('*');
            term_move_cursor(SN_OX+SN_W+2, SN_OY+2);
            term_set_color(0x0F,0x00); term_print_int(score); term_print("   ");
            int lvl=1+score/50;
            term_move_cursor(SN_OX+SN_W+2, SN_OY+5);
            term_print_int(lvl); term_print(" ");
            term_move_cursor(SN_OX+SN_W+2, SN_OY+8);
            term_print_int(slen); term_print("   ");
        }
        if (slen>1) {
            term_move_cursor(SN_OX+sx[1], SN_OY+sy[1]);
            term_set_color(0x02,0x00); term_putc('o');
        }
        term_move_cursor(SN_OX+sx[0], SN_OY+sy[0]);
        term_set_color(0x0A,0x00); term_putc('@');
    }

    hs_update(0, score);
    beep_gameover();
    term_move_cursor(SN_OX+SN_W/2-4, SN_OY+SN_H/2-1);
    term_set_color(0x0C,0x00); term_print("GAME OVER");
    term_move_cursor(SN_OX+SN_W/2-4, SN_OY+SN_H/2);
    term_set_color(0x0F,0x00); term_print("Score:"); term_print_int(score);
    if (score == hs_scores[0]) { term_set_color(0x0E,0x00); term_print(" \xdb NEW BEST!"); }
    term_move_cursor(SN_OX+SN_W/2-7, SN_OY+SN_H/2+2);
    term_set_color(0x08,0x00); term_print("Press any key...");
    while(!get_monitor_char());
    draw_interface();
}
#undef SN_W
#undef SN_H
#undef SN_OX
#undef SN_OY
#undef SN_MAX


static unsigned char tetrominos[7][4] = {
    { 0x0, 0xF, 0x0, 0x0 },

    { 0x0, 0x6, 0x6, 0x0 },

    { 0x0, 0xE, 0x4, 0x0 },

    { 0x0, 0x6, 0xC, 0x0 },

    { 0x0, 0xC, 0x6, 0x0 },

    { 0x0, 0xE, 0x2, 0x0 },

    { 0x0, 0xE, 0x8, 0x0 },

};

#define TBOARD_W 10
#define TBOARD_H 20

static unsigned char tboard[TBOARD_H][TBOARD_W];
static int t_colors[7] = { 3, 6, 5, 2, 4, 1, 7 };

static const char* t_color_esc[8] = {
    "\x1b[0m",  "\x1b[34m", "\x1b[32m", "\x1b[36m",
    "\x1b[31m", "\x1b[35m", "\x1b[33m", "\x1b[37m",
};

void t_get_cells(int piece, int rot, int pr, int pc, int out[4][2]) {
    unsigned char rows[4];
    for (int r=0;r<4;r++) rows[r]=tetrominos[piece][r];
    for (int i=0;i<rot;i++) {
        unsigned char nr[4]={0,0,0,0};
        for (int r=0;r<4;r++)
            for (int c=0;c<4;c++)
                if (rows[r]&(1<<(3-c))) nr[c]|=(1<<r);
        for (int r=0;r<4;r++) rows[r]=nr[r];
    }
    int n=0;
    for (int r=0;r<4&&n<4;r++)
        for (int c=0;c<4&&n<4;c++)
            if (rows[r]&(1<<(3-c))) { out[n][0]=pr+r; out[n][1]=pc+c; n++; }
}

int t_valid(int cells[4][2]) {
    for (int i=0;i<4;i++) {
        int r=cells[i][0], c=cells[i][1];
        if (r<0||r>=TBOARD_H||c<0||c>=TBOARD_W) return 0;
        if (tboard[r][c]) return 0;
    }
    return 1;
}

void t_lock(int cells[4][2], int color) {
    for (int i=0;i<4;i++) tboard[cells[i][0]][cells[i][1]]=(unsigned char)(color+1);
}

int t_clear_lines(void) {
    int cleared=0;
    for (int r=TBOARD_H-1;r>=0;r--) {
        int full=1;
        for (int c=0;c<TBOARD_W;c++) if(!tboard[r][c]){full=0;break;}
        if (full) {
            cleared++;
            for (int rr=r;rr>0;rr--)
                for (int c=0;c<TBOARD_W;c++) tboard[rr][c]=tboard[rr-1][c];
            for (int c=0;c<TBOARD_W;c++) tboard[0][c]=0;
            r++;
        }
    }
    return cleared;
}

void t_draw(int pr, int pc, int piece, int rot, int gpr,
            int score, int level, int next_piece, int lines) {
    int bx=1, by=1;
    int cur[4][2], ghost[4][2], nxt[4][2];
    t_get_cells(piece, rot, pr, pc, cur);
    t_get_cells(piece, rot, gpr, pc, ghost);
    t_get_cells(next_piece, 0, 0, 0, nxt);

    term_move_cursor(bx-1, by-1);
    term_set_color(0x07,0x00);
    term_print("+----------+  TETRIS\n");
    for (int r=0;r<TBOARD_H;r++) {
        term_move_cursor(bx-1, by+r);
        term_set_color(0x07,0x00); term_putc('|');
        for (int c=0;c<TBOARD_W;c++) {
            int is_cur=0, is_ghost=0;
            for (int k=0;k<4;k++) {
                if (cur[k][0]==r&&cur[k][1]==c) { is_cur=1; break; }
            }
            if (!is_cur) for (int k=0;k<4;k++) {
                if (ghost[k][0]==r&&ghost[k][1]==c) { is_ghost=1; break; }
            }
            if (is_cur) {
                term_print(t_color_esc[t_colors[piece]]);
                term_putc('#'); term_print("\x1b[0m");
            } else if (is_ghost && !tboard[r][c]) {
                term_set_color(0x08,0x00); term_putc(':'); term_set_color(0x07,0x00);
            } else if (tboard[r][c]) {
                term_print(t_color_esc[tboard[r][c]]);
                term_putc('#'); term_print("\x1b[0m");
            } else {
                term_set_color(0x08,0x00); term_putc('.'); term_set_color(0x07,0x00);
            }
        }
        term_set_color(0x07,0x00); term_putc('|');
        if (r==1)  { term_set_color(0x0E,0x00); term_print("  Score:"); }
        if (r==2)  { term_set_color(0x0F,0x00); term_print("  "); term_print_int(score); term_print("  "); }
        if (r==4)  { term_set_color(0x0E,0x00); term_print("  Level:"); }
        if (r==5)  { term_set_color(0x0F,0x00); term_print("  "); term_print_int(level); term_print(" "); }
        if (r==7)  { term_set_color(0x0E,0x00); term_print("  Lines:"); }
        if (r==8)  { term_set_color(0x0F,0x00); term_print("  "); term_print_int(lines); term_print(" "); }
        if (r==10) { term_set_color(0x0B,0x00); term_print("  Next:"); }
        if (r>=11&&r<=14) {
            int nr=r-11;
            term_print("  ");
            for (int nc=0;nc<4;nc++) {
                int filled=0;
                for (int k=0;k<4;k++) if(nxt[k][0]==nr&&nxt[k][1]==nc){filled=1;break;}
                if (filled) {
                    term_print(t_color_esc[t_colors[next_piece]]);
                    term_putc('#'); term_print("\x1b[0m");
                } else {
                    term_set_color(0x08,0x00); term_putc('.'); term_set_color(0x07,0x00);
                }
            }
        }
        if (r==16) { term_set_color(0x08,0x00); term_print("  Q/D=move Z=rot"); }
        if (r==17) { term_set_color(0x08,0x00); term_print("  S=soft SPACE=drop"); }
        if (r==18) { term_set_color(0x08,0x00); term_print("  P=pause ESC=quit"); }
        term_set_color(0x07,0x00); term_putc('\n');
    }
    term_move_cursor(bx-1, by+TBOARD_H);
    term_print("+----------+\n");
}

void start_tetris(void) {
    term_init();
    gui_game_start();
    memset(tboard, 0, sizeof(tboard));
    int score=0, level=1, lines_total=0;
    int piece=rand()%7, rot=0, pr=0, pc=TBOARD_W/2-2;
    int next_piece=rand()%7;
    int drop_delay=16, drop_counter=0, paused=0;

    while (1) {


        int gpr=pr;
        {
            int gc[4][2];
            while(1) {
                t_get_cells(piece,rot,gpr+1,pc,gc);
                if(t_valid(gc)) gpr++; else break;
            }
        }
        t_draw(pr,pc,piece,rot,gpr,score,level,next_piece,lines_total);
        nop_delay(TETRIS_TICK);
        int c=get_monitor_char();

        if (c==KEY_ESC) { draw_interface(); return; }
        if (c=='p'||c=='P') {
            paused=!paused;
            if (paused) {
                term_move_cursor(2, TBOARD_H/2+1);
                term_set_color(0x0E,0x00); term_print("  PAUSED  ");
            }
        }
        if (paused) continue;

        int ncells[4][2];
        if (c=='q'||c=='d'||c=='z') {
            int npc=pc+(c=='q'?-1:c=='d'?1:0);
            int nrot=(c=='z')?(rot+1)%4:rot;
            t_get_cells(piece,nrot,pr,npc,ncells);
            if (t_valid(ncells)) { pc=npc; rot=nrot; }
        }
        if (c=='s') {
            t_get_cells(piece,rot,pr+1,pc,ncells);
            if (t_valid(ncells)) { pr++; score++; drop_counter=0; }
        }
        if (c==' ') {
            while(1) {
                t_get_cells(piece,rot,pr+1,pc,ncells);
                if(!t_valid(ncells)) break;
                pr++; score+=2;
            }
            drop_counter=drop_delay;
        }

        drop_counter++;
        if (drop_counter>=drop_delay) {
            drop_counter=0;
            t_get_cells(piece,rot,pr+1,pc,ncells);
            if (t_valid(ncells)) {
                pr++;
            } else {
                int lcells[4][2];
                t_get_cells(piece,rot,pr,pc,lcells);
                t_lock(lcells,t_colors[piece]);
                int cleared=t_clear_lines();
                lines_total+=cleared;
                int pts[5]={0,100,300,500,800};
                if(cleared<5) score+=pts[cleared]*level;
                level=1+lines_total/10;
                drop_delay=16-(level-1)*2;
                if(drop_delay<3) drop_delay=3;
                piece=next_piece; next_piece=rand()%7;
                pr=0; pc=TBOARD_W/2-2; rot=0;
                t_get_cells(piece,rot,pr,pc,ncells);
                if(!t_valid(ncells)) {
                    hs_update(1, score);
                    beep_gameover();
                    t_draw(pr,pc,piece,rot,gpr,score,level,next_piece,lines_total);
                    term_move_cursor(0,TBOARD_H+3);
                    term_set_color(0x0C,0x00); term_print("GAME OVER!  Score:");
                    term_print_int(score); term_print("  Lines:"); term_print_int(lines_total);
                    if (score == hs_scores[1]) { term_set_color(0x0E,0x00); term_print("  \xdb NEW BEST!"); }
                    term_putc('\n');
                    while(!get_monitor_char());
                    draw_interface(); return;
                }
            }
        }
    }
}


static void typewrite(const char* s, int delay) {
    for (int i = 0; s[i]; i++) { term_putc(s[i]); nop_delay(delay); }
}


static const char* greg_frame1[] = {
    "                            ==(W{==========-      /===-                    ",
    "                              ||  (.--.)         /===-_---~~~~~~~~~------__",
    "                              | \\_,|**|,__      |===-~___            _,-' `",
    "                 -==\\\\        `\\ ' `--'   ),    `//~\\\\   ~~~~`---.___.-~~  ",
    "             ______-==|        /`\\_ .__/\\ \\    | |  \\\\          _-~`       ",
    "       __--~~~  ,-/-==\\\\      (   | .  |~~~~|   | |   `\\       ,'          ",
    "    _-~       /'    |  \\\\     )__/==0==-\\<>/   / /      \\     /            ",
    "  .'        /       |   \\\\      /~\\___/~~\\/  /' /        \\  /'             ",
    " /  ____  /         |    \\`\\.__/-~~   \\  |_/'  /          \\/'              ",
    "/-'~    ~~~~~---__  |     ~-/~         ( )   /'        _--~`               ",
    "                  \\_|      /        _) | ;  ),   __--~~                    ",
    "                    '~~--_/      _-~/- |/ \\   '-~ \\                        ",
    "                   {\\__--_/}    / \\\\_>-|)<__\\      \\                      ",
    "                   /'   (_/  _-~  | |__>--<__|      |                      ",
    "                  |   _/) )-~     | |__>--<__|      |                      ",
    "                  / /~ ,_/       / /__>---<__/      |                      ",
    "                 o-o _//        /-~_>---<__-~      /                       ",
    "                 (^(~          /~_>---<__-      _-~                        ",
    "                ,/|           /__>--<__/     _-~                           ",
    "             ,//('(          |__>--<__|     /  -GREG & DRAKKAR-  .----_    ",
    "            ( ( '))          |__>--<__|    |                   /' _---_~\\  ",
    "         `-)) )) (           |__>--<__|    |               /'  /     ~\\`\\ ",
    0
};

static const char* greg_frame2[] = {
    "                            ==(W{==========-     >>===-~                   ",
    "                              ||  (.--.)        >>===-_---~*~*~*~*~~~---__ ",
    "                              | \\_,|**|,__     >>===-~*~~             _,-' `",
    "                 -==\\\\        `\\ ' `--'   ),    `//~\\\\   ~~~~`---.___.-~~  ",
    "             ______-==|        /`\\_ .__/\\ \\    | |  \\\\          _-~`       ",
    "       __--~~~  ,-/-==\\\\      (   | .  |~~~~|   | |   `\\       ,'          ",
    "    _-~       /'    |  \\\\     )__/==0==-\\<>/   / /      \\     /            ",
    "  .'        /       |   \\\\      /~\\___/~~\\/  /' /        \\  /'             ",
    " /  ____  /         |    \\`\\.__/-~~   \\  |_/'  /          \\/'              ",
    "/-'~    ~~~~~---__  |     ~-/~         ( )   /'        _--~`               ",
    "                  \\_|      /        _) | ;  ),   __--~~                    ",
    "                    '~~--_/      _-~/- |/ \\   '-~ \\                        ",
    "                   {\\__--_/}    / \\\\_>-|)<__\\      \\                      ",
    "                   /'   (_/  _-~  | |__>--<__|      |                      ",
    "                  |   _/) )-~     | |__>--<__|      |                      ",
    "                  / /~ ,_/       / /__>---<__/      |                      ",
    "                 o-o _//        /-~_>---<__-~      /                       ",
    "                 (^(~          /~_>---<__-      _-~                        ",
    "                ,/|           /__>--<__/     _-~                           ",
    "             ,//('(          |__>--<__|     /  -GREG & DRAKKAR-  .----_    ",
    "            ( ( '))          |__>--<__|    |                   /' _---_~\\  ",
    "         `-)) )) (           |__>--<__|    |               /'  /     ~\\`\\ ",
    0
};

static void greg_draw_frame(const char** frame, int start_y,
                             unsigned char body_col, unsigned char rider_col,
                             unsigned char fire_col) {
    for (int i = 0; frame[i]; i++) {
        term_move_cursor(0, start_y + i);
        const char* line = frame[i];
        for (int x = 0; line[x]; x++) {
            char c = line[x];
            if (i < 3 && x < 52)
                term_set_color(rider_col, 0x00);
            else if (c == '~' || c == '*' || (c == '>' && x > 35))
                term_set_color(fire_col, 0x00);
            else
                term_set_color(body_col, 0x00);
            term_putc(c);
        }
    }
}

static void greg_fill_row(int y, unsigned char fg, unsigned char bg) {
    term_set_color(fg, bg);
    term_move_cursor(0, y);
    for (int x = 0; x < VGA_WIDTH; x++) term_putc(' ');
}

static void greg_title(const char* msg, unsigned char fg, unsigned char bg) {
    greg_fill_row(0, fg, bg);
    term_set_color(fg, bg);
    int len = 0; while (msg[len]) len++;
    int sx = (VGA_WIDTH - len) / 2;
    if (sx < 0) sx = 0;
    term_move_cursor(sx, 0);
    term_print(msg);
}

static void greg_type(int col, int row, unsigned char fg, const char* msg) {
    term_set_color(fg, 0x00);
    term_move_cursor(col, row);
    for (int i = 0; msg[i]; i++) { term_putc(msg[i]); nop_delay(5000000); }
}

static void greg_clear_lore(void) {
    term_set_color(0x00, 0x00);
    term_move_cursor(0, 23);
    for (int x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    term_move_cursor(0, 24);
    for (int x = 0; x < VGA_WIDTH; x++) term_putc(' ');
}

static void greg_animation(void) {
    term_init();
    term_set_color(0x00, 0x00);
    for (int y = 0; y < VGA_HEIGHT; y++) {
        term_move_cursor(0, y);
        for (int x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    }

    int start_y = 1;

    greg_title("[ GREGOS :: GREG 1er & DRAKKAR, GARDIENS DU KERNEL ]", 0x0F, 0x01);

    for (int i = 0; greg_frame1[i]; i++) {
        term_set_color(i < 3 ? 0x0E : 0x0A, 0x00);
        term_move_cursor(0, start_y + i);
        term_print(greg_frame1[i]);
        timer_delay_ms(30);
    }

    timer_delay_ms(800);

    greg_type(2, 23, 0x0F,
        "GREG 1er, Seigneur du Kernel, invoque depuis le neant du premier boot...");
    timer_delay_ms(600);
    greg_type(2, 24, 0x0B,
        "DRAKKAR, forge dans les flammes du compilateur original, est sa monture.");

    timer_delay_ms(900);

    greg_title("[ GREG LEVE SA LANCE  --  DRAKKAR CRACHE LE FEU ! ]", 0x00, 0x0C);
    greg_draw_frame(greg_frame2, start_y, 0x0A, 0x0E, 0x0C);
    beep_ok();
    timer_delay_ms(700);
    greg_draw_frame(greg_frame1, start_y, 0x0A, 0x0E, 0x02);
    timer_delay_ms(400);

    greg_draw_frame(greg_frame2, start_y, 0x0A, 0x0F, 0x0E);
    beep_ok();
    timer_delay_ms(700);
    greg_draw_frame(greg_frame1, start_y, 0x0A, 0x0E, 0x02);
    timer_delay_ms(400);

    greg_clear_lore();
    greg_title("[ GREGOS :: GREG 1er & DRAKKAR, GARDIENS DU KERNEL ]", 0x0F, 0x01);
    timer_delay_ms(400);

    greg_type(2, 23, 0x0E,
        "Ensemble, ils regnent sur chaque cycle d'horloge de GregOS depuis l'aube.");
    timer_delay_ms(600);
    greg_type(2, 24, 0x0A,
        "Nul bug ne leur resiste. Nul segfault ne survit a leur regard de braise.");

    timer_delay_ms(900);

    greg_title("[ GREG LEVE SA LANCE  --  DRAKKAR CRACHE LE FEU ! ]", 0x00, 0x0C);
    greg_draw_frame(greg_frame2, start_y, 0x0C, 0x0F, 0x0E);
    beep_ok();
    timer_delay_ms(800);
    greg_draw_frame(greg_frame1, start_y, 0x0A, 0x0E, 0x02);
    timer_delay_ms(500);

    greg_clear_lore();
    greg_title("[ GREGOS :: GREG 1er & DRAKKAR, GARDIENS DU KERNEL ]", 0x0F, 0x01);
    timer_delay_ms(400);

    greg_type(2, 23, 0x0D,
        "Les dragons ne meurent pas. Les kernels non plus. GregOS est eternel.");
    timer_delay_ms(600);
    greg_type(2, 24, 0x07,
        "                  -- Legende du Void Numerique, vers 0x0000 --");

    timer_delay_ms(900);

    greg_title("[ GREG LEVE SA LANCE  --  DRAKKAR CRACHE LE FEU ! ]", 0x00, 0x0C);
    greg_draw_frame(greg_frame2, start_y, 0x0F, 0x0F, 0x0E);
    beep_ok();
    timer_delay_ms(1000);
    greg_draw_frame(greg_frame1, start_y, 0x0A, 0x0E, 0x02);
    timer_delay_ms(400);

    greg_clear_lore();
    term_set_color(0x0E, 0x00);
    term_move_cursor(0, 0);
    for (int x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    term_set_color(0x0E, 0x00);
    term_move_cursor(12, 0);
    term_print(">>> Appuyez sur une touche pour entrer dans GregOS... <<<");
}

static const char* cwin_vic[] = {
    "    \\o/    G R E G  1 e r  &  D R A K K A R    \\o/    ",
    "     |        ~~  V I C T O I R E  ! ~~          |    ",
    "    / \\                                          / \\   ",
    "                        ,_,                           ",
    "                .----( 0 0 )----.                     ",
    "               /    `_ > > _'    \\                    ",
    "              |   B R A V O  !    |                   ",
    "  >>====~     |  C H A M P I O N  |    >>====~        ",
    "   >>====~     `__________________'   >>====~         ",
    0
};

static const char* cwin_jp[] = {
    "  $$  $$$   *** J A C K P O T ! ***   $$$  $$         ",
    " $ $$$ $$$                            $$$ $$$ $       ",
    "  $$  $$$    ,--------------------,   $$$  $$         ",
    "             /    (0 0)   (0 0)    \\                  ",
    "  $$  $$$   |      \\  /\\/\\  /      |  $$$  $$        ",
    "   $$  $$   |       \\/    \\/        |   $$  $         ",
    "   $$  $$    \\                      /   $$  $         ",
    "  $$  $$$    `____________________'   $$$  $$         ",
    "  >>====~                              >>====~         ",
    "   >>====~   G R E G  & D R A K K A R  >>====~        ",
    "    >>====~    $ $  J A C K P O T  $ $  >>====~       ",
    0
};

static void casino_win_anim(int jp) {
    const char** frame = jp ? cwin_jp : cwin_vic;
    int nlines = 0; while (frame[nlines]) nlines++;
    int x0 = 2, y0 = 4;
    int cols = 76;
    int rows = nlines + 4;
    unsigned char bfg = jp ? 0x0E : 0x0A;
    unsigned char bg  = 0x00;

    term_set_color(bfg, bg);
    term_move_cursor(x0, y0);
    term_putc('\xda');
    for (int i = 1; i < cols - 1; i++) term_putc('\xc4');
    term_putc('\xbf');

    for (int r = 1; r < rows - 1; r++) {
        term_set_color(bfg, bg);
        term_move_cursor(x0, y0 + r);
        term_putc('\xb3');
        term_set_color(0x07, bg);
        for (int i = 1; i < cols - 1; i++) term_putc(' ');
        term_set_color(bfg, bg);
        term_putc('\xb3');
    }

    term_set_color(bfg, bg);
    term_move_cursor(x0, y0 + rows - 1);
    term_putc('\xc0');
    for (int i = 1; i < cols - 1; i++) term_putc('\xc4');
    term_putc('\xd9');

    for (int i = 0; frame[i]; i++) {
        term_set_color(i % 2 == 0 ? bfg : (jp ? 0x06 : 0x0F), bg);
        term_move_cursor(x0 + 2, y0 + 2 + i);
        term_print(frame[i]);
    }

    const char* title = jp ? "  \xdb\xdb\xdb  J A C K P O T ! ! !  \xdb\xdb\xdb  "
                           : "  \xdb  V I C T O I R E  !  \xdb  ";
    int tlen = 0; while (title[tlen]) tlen++;
    int tx = x0 + (cols - tlen) / 2;
    if (tx < x0 + 2) tx = x0 + 2;
    for (int f = 0; f < 8; f++) {
        term_set_color(f % 2 == 0 ? 0x0F : bfg, bg);
        term_move_cursor(tx, y0);
        term_print(title);
        nop_delay(180000000);
    }

    for (int t = 0; t < 40; t++) {
        char k = get_monitor_char();
        if (k == 0x1B || k == ' ' || k == '\r') break;
        nop_delay(60000000);
    }
}

static void boot_ok(const char* msg) {
    term_print("\x1b[32m[ OK ]\x1b[0m ");
    typewrite(msg, 3000000);
    term_putc('\n');
    nop_delay(40000000);
}


static const char* hs_names[HS_NGAMES] = {
    "Snake", "Tetris", "Invaders", "Breakout", "2048", "Minesweeper", "Simon", "Casino", "Blackjack", "Slots"
};

static void hs_update(int game_idx, int score) {
    if (game_idx < 0 || game_idx >= HS_NGAMES) return;
    if (score > hs_scores[game_idx]) hs_scores[game_idx] = score;
}

static void cmd_scores(void) {
    beep_notify();
    const int SW = 48;  /* inner width between borders */

    /* Find max score for relative bar scaling */
    int maxs = 1;
    for (int i = 0; i < HS_NGAMES; i++)
        if (hs_scores[i] > maxs) maxs = hs_scores[i];

    term_set_color(0x0E, 0x00);
    term_putc('\xda'); for(int _i=0;_i<SW;_i++) term_putc('\xc4'); term_putc('\xbf'); term_putc('\n');

    /* Title */
    term_putc('\xb3');
    term_set_color(0x0F,0x00); term_print("   \x0f\x0f\x0f  HALL OF FAME  GregOS  \x0f\x0f\x0f   ");
    term_set_color(0x0E,0x00); term_print("               \xb3\n");

    term_putc('\xc3'); for(int _i=0;_i<SW;_i++) term_putc('\xc4'); term_putc('\xb4'); term_putc('\n');

    /* Column headers */
    term_putc('\xb3');
    term_set_color(0x08,0x00); term_print("  ");
    term_set_color(0x0B,0x00); term_print("JEU             ");
    term_set_color(0x0E,0x00); term_print("SCORE         ");
    term_set_color(0x08,0x00); term_print("PROGRESSION\n");

    term_set_color(0x0E,0x00);
    term_putc('\xc3'); for(int _i=0;_i<SW;_i++) term_putc('\xc4'); term_putc('\xb4'); term_putc('\n');

    int any = 0;
    for (int i = 0; i < HS_NGAMES; i++) {
        if (hs_scores[i] > 0) any = 1;
        term_set_color(0x0E,0x00); term_putc('\xb3');
        term_set_color(0x0B,0x00); term_print("  ");
        term_print(hs_names[i]);
        int nlen = 0; const char* _np = hs_names[i]; while(*_np++) nlen++;
        for (int p = nlen; p < 14; p++) term_putc(' ');
        if (hs_scores[i] > 0) {
            term_set_color(0x0A,0x00);
            term_print_int(hs_scores[i]);
            int slen = 0; int _t = hs_scores[i]; while(_t){slen++;_t/=10;}
            for (int p = slen; p < 12; p++) term_putc(' ');
            int bars = (hs_scores[i] * 10) / maxs;
            if (bars < 1) bars = 1;
            if (bars > 10) bars = 10;
            term_set_color(0x0A,0x00); for(int b=0;b<bars;b++) term_putc('\xdb');
            term_set_color(0x08,0x00); for(int b=bars;b<10;b++) term_putc('\xb0');
        } else {
            term_set_color(0x08,0x00);
            term_print("  ---          ");
            for(int b=0;b<10;b++) term_putc('\xb0');
        }
        term_putc('\n');
    }

    term_set_color(0x0E,0x00);
    term_putc('\xc0'); for(int _i=0;_i<SW;_i++) term_putc('\xc4'); term_putc('\xd9'); term_putc('\n');

    if (!any) {
        term_set_color(0x08,0x00);
        term_print("  Lance des jeux depuis l'app Jeux ou Casino!\n");
    }
    term_set_color(0x0F,0x00);
}


void login_screen(void) {

    term_init();
    term_set_color(0x0F, 0x01);
    for (int y = 0; y < VGA_HEIGHT; y++) {
        term_move_cursor(0, y);
        for (int x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    }

    static const char* splash[] = {
        " \xdb\xdb\xdb\xdb\xdb  \xdb\xdb\xdb\xdb\xdb  \xdb\xdb\xdb\xdb\xdb  \xdb\xdb\xdb\xdb\xdb  \xdb\xdb  \xdb\xdb  \xdb\xdb\xdb\xdb\xdb ",
        " \xdb\xdb     \xdb\xdb  \xdb\xdb  \xdb\xdb  \xdb\xdb\xdb    \xdb\xdb  \xdb\xdb  \xdb\xdb     ",
        " \xdb\xdb\xdb\xdb\xdb  \xdb\xdb\xdb\xdb\xdb  \xdb\xdb  \xdb\xdb  \xdb\xdb\xdb    \xdb\xdb  \xdb\xdb  \xdb\xdb\xdb\xdb  ",
        " \xdb\xdb     \xdb\xdb  \xdb\xdb  \xdb\xdb  \xdb\xdb  \xdb\xdb  \xdb\xdb  \xdb\xdb  \xdb\xdb     ",
        " \xdb\xdb\xdb\xdb\xdb  \xdb\xdb\xdb\xdb\xdb  \xdb\xdb\xdb\xdb\xdb  \xdb\xdb\xdb\xdb\xdb   \xdb\xdb\xdb\xdb   \xdb\xdb\xdb\xdb\xdb ",
        0
    };
    int splash_rows = 0;
    for (int i = 0; splash[i]; i++) splash_rows++;
    int sy = (VGA_HEIGHT - splash_rows - 4) / 2;

    term_set_color(0x0B, 0x01);
    for (int i = 0; splash[i]; i++) {
        int slen = 0; while (splash[i][slen]) slen++;
        int sx = (VGA_WIDTH - slen) / 2;
        term_move_cursor(sx, sy + i);
        term_print(splash[i]);
    }
    term_set_color(0x09, 0x01);
    term_move_cursor((VGA_WIDTH - 36) / 2, sy + splash_rows + 1);
    term_print("  Custom 32-bit x86 Operating System  ");



    static const unsigned char pulse_cols[] = {
        0x09, 0x0B, 0x0F, 0x0E, 0x0C, 0x0E, 0x0F, 0x0B, 0x09, 0x01,
        0x09, 0x0B, 0x0F, 0x0E, 0x0C, 0x0E, 0x0F, 0x0B, 0x09, 0x01,
        0x09, 0x0B, 0x0F, 0x0B, 0x09
    };
    for (int p = 0; p < 25; p++) {
        term_set_color(pulse_cols[p], 0x01);
        for (int i = 0; splash[i]; i++) {
            int slen = 0; while (splash[i][slen]) slen++;
            int sx2 = (VGA_WIDTH - slen) / 2;
            term_move_cursor(sx2, sy + i);
            term_print(splash[i]);
        }


        term_set_color(0x07, 0x01);
        term_move_cursor((VGA_WIDTH - 24) / 2, sy + splash_rows + 3);
        if      (p % 4 == 0) term_print("Booting...              ");
        else if (p % 4 == 1) term_print("Booting....             ");
        else if (p % 4 == 2) term_print("Booting.....            ");
        else                  term_print("Booting......           ");
        timer_delay_ms(80);
    }

    timer_delay_ms(300);


    term_init();
    term_set_color(0x0F, 0x00);



    const char* logo_lines[] = {
        "\x1b[33m          ==(W{==========-\n",
        "            ||  (.--.)   \x1b[0m\n",
        "\x1b[33m            | \\_,|**|,__ \n",
        "         ___/-==|  /`\\_.___\n",
        "       (^(~     `-'   _-~` \n",
        "\x1b[0m\n",
        "\x1b[0m  \x1b[33mG\x1b[0m \x1b[33mR\x1b[0m \x1b[33mE\x1b[0m \x1b[33mG\x1b[0m \x1b[33mO\x1b[0m \x1b[33mS\x1b[0m  \x1b[0mv2  --  Seigneur du Kernel\n",
        "\x1b[0m\n",
        0
    };
    for (int i = 0; logo_lines[i]; i++) {
        typewrite(logo_lines[i], 700000);
        timer_delay_ms(20);
    }



    term_putc('\n');
    boot_ok("Initializing kernel...");
    boot_ok("Loading VGA driver...");
    boot_ok("Starting PS/2 keyboard driver...");
    boot_ok("Mounting gregfs (64 slots)...");
    boot_ok("Starting gregsh...");
    timer_delay_ms(200);



    greg_animation();



    term_init();
    int bx = 18, by = 8, bw = 44, bh = 9;
    ui_fill(bx, by, bw, bh, 0x0F, 0x01);
    ui_dbox(bx, by, bw, bh, 0x0B, 0x01);
    ui_center_in(bx+1, by+1, bw-2, "\xdb\xdb GregOS v2.0 \xdb\xdb", 0x0F, 0x01);
    ui_center_in(bx+1, by+2, bw-2, "Secure Authentication", 0x08, 0x01);


    term_set_color(0x0B, 0x01);
    term_move_cursor(bx, by+3);
    term_putc('\xc3');
    for (int i = 0; i < bw-2; i++) term_putc('\xc4');
    term_putc('\xb4');

    char login_name[20];
    while (1) {


        ui_fill(bx+1, by+5, bw-2, 2, 0x0F, 0x01);
        term_set_color(0x0B, 0x01);
        term_move_cursor(bx,      by+5); term_putc('\xb3');
        term_move_cursor(bx+bw-1, by+5); term_putc('\xb3');
        term_move_cursor(bx,      by+6); term_putc('\xb3');
        term_move_cursor(bx+bw-1, by+6); term_putc('\xb3');



        term_set_color(0x0E, 0x01);
        term_move_cursor(bx+2, by+5);
        term_print("login:    ");
        term_set_color(0x0F, 0x01);
        char login[20]; int li = 0;
        while (1) {
            int lc = get_monitor_char();
            if (lc == '\n')                      { login[li]='\0'; break; }
            if (lc == '\b' && li > 0)            { li--; term_putc('\b'); }
            else if (lc && lc!='\t' && li < 15)  { login[li++]=(char)lc; term_putc((char)lc); }
            nop_delay(300000);
        }



        term_set_color(0x0E, 0x01);
        term_move_cursor(bx+2, by+6);
        term_print("password: ");
        term_set_color(0x08, 0x01);
        char pw[20]; int pi = 0;
        while (1) {
            int lc = get_monitor_char();
            if (lc == '\n')                     { pw[pi]='\0'; break; }
            if (lc == '\b' && pi > 0)           { pi--; term_putc('\b'); }
            else if (lc && lc!='\t' && pi < 15) { pw[pi++]=(char)lc; term_putc('\xf9'); }
            nop_delay(300000);
        }

        if (strcmp(pw, gregos_passwd) == 0) {
            strncpy(login_name, login, 19); login_name[19]='\0';


            ui_fill(bx+1, by+5, bw-2, 2, 0x0F, 0x01);
            term_set_color(0x0B, 0x01);
            term_move_cursor(bx,      by+5); term_putc('\xb3');
            term_move_cursor(bx+bw-1, by+5); term_putc('\xb3');
            term_move_cursor(bx,      by+6); term_putc('\xb3');
            term_move_cursor(bx+bw-1, by+6); term_putc('\xb3');
            beep_ok();
            ui_center_in(bx+1, by+5, bw-2, "Authentication successful!", 0x0A, 0x01);
            term_set_color(0x0A, 0x01);
            term_move_cursor(bx+2, by+6);
            term_print("Loading ");
            int bar_w = bw - 14;
            for (int p = 0; p <= bar_w; p++) {
                ui_progress(bx+10, by+6, bar_w, p, bar_w, 0x0A, 0x01);
                nop_delay(20000000);
            }
            nop_delay(100000000);
            return;
        }


        ui_fill(bx+1, by+5, bw-2, 2, 0x0F, 0x01);
        term_set_color(0x0B, 0x01);
        term_move_cursor(bx,      by+5); term_putc('\xb3');
        term_move_cursor(bx+bw-1, by+5); term_putc('\xb3');
        term_move_cursor(bx,      by+6); term_putc('\xb3');
        term_move_cursor(bx+bw-1, by+6); term_putc('\xb3');
        beep_fail();
        ui_center_in(bx+1, by+5, bw-2, "Authentication failed. Try again.", 0x0C, 0x01);
        nop_delay(10000000);
    }
}


static const char* fortunes[] = {
    "The best error message is the one that never shows up.",
    "It works on my machine.",
    "rm -rf / --no-preserve-root (don't try this at home)",
    "There are only 2 hard problems in CS: cache invalidation and naming things.",
    "To understand recursion, you must first understand recursion.",
    "Premature optimization is the root of all evil. -- Knuth",
    "Unix is user-friendly. It's just very selective about who its friends are.",
    "Real programmers count from 0.",
    "A computer is like a mischievous genie: it gives you what you ask for,\nnot what you want.",
    "Keep it simple, stupid.",
    0
};


typedef struct { const char* name; const char* desc; } ManEntry;
static ManEntry man_pages[] = {
    {"ls",      "ls — list directory contents\nUsage: ls"},
    {"cd",      "cd — change directory\nUsage: cd <dir>  or  cd .."},
    {"mkdir",   "mkdir — make directory\nUsage: mkdir <name>"},
    {"touch",   "touch — create empty file\nUsage: touch <name>"},
    {"rm",      "rm — remove file\nUsage: rm <name>"},
    {"cat",     "cat — print file contents\nUsage: cat <file>"},
    {"nano",    "nano — text editor\nUsage: nano <file>\nCtrl+S=Save  ESC=Exit"},
    {"cp",      "cp — copy file\nUsage: cp <src> <dest>"},
    {"mv",      "mv — rename/move file\nUsage: mv <old> <new>"},
    {"grep",    "grep — search in file or recursively\nUsage: grep [-i] [-n] [-r] <pattern> [file]"},
    {"wc",      "wc — word/char/line count\nUsage: wc <file>"},
    {"find",    "find — find files\nUsage: find [path] [-name pat] [-type f|d]"},
    {"bc",      "bc — expression calculator\nUsage: bc 3+4*2  or  echo '3+4' | bc"},
    {"cut",     "cut — extract fields from lines\nUsage: cut -d: -f1 <file>  or  cmd | cut -d: -f2"},
    {"tr",      "tr — translate characters\nUsage: tr a-z A-Z  or  echo text | tr a-z A-Z"},
    {"sed",     "sed — stream editor (basic substitution)\nUsage: sed 's/old/new/' [file]  or  cmd | sed 's/old/new/g'"},
    {"hexdump", "hexdump — hex view of file\nUsage: hexdump <file>"},
    {"echo",    "echo — print text / write to file\nUsage: echo <text>\n       echo <text> > <file>\n       echo <text> >> <file>"},
    {"calc",    "calc — simple calculator\nUsage: calc 5 + 3   (ops: + - * /)"},
    {"snake",      "snake — snake game v2\nControls: Z/Q/S/D or arrows  P=pause  ESC=quit"},
    {"tetris",     "tetris — tetris v2 (ghost piece, hard drop)\nControls: Q/D=move Z=rotate S=soft SPACE=hard-drop P=pause ESC=quit"},
    {"breakout",   "breakout — brick breaker game\nControls: Q/D or arrows=move  P=pause  ESC=quit"},
    {"2048",       "2048 — sliding tile puzzle\nControls: Z/Q/S/D or arrows  ESC=quit"},
    {"minesweeper","minesweeper — 9x9 grid, 10 mines\nControls: ZQSD=move SPACE=reveal F=flag ESC=quit"},
    {"simon",      "simon — musical memory game (1-4 = buttons, ESC = quit)\nMatch the sequence. Gets longer each round. PC speaker required."},
    {"roulette",   "roulette — casino roulette avec GregCoins\nPariez sur rouge/noir/pair/impair/manque/passe/douzaine/plein.\nDemarrez avec 1000 GC. Commandes: R N P I M T D X ESC"},
    {"blackjack",  "blackjack — blackjack casino avec GregCoins\nH=Tirer S=Rester D=Doubler ESC=Quitter\nBlackjack naturel paye 3:2. Croupier tire jusqu a 17."},
    {"casino",     "casino — lobby du Casino GregOS\n[1] Blackjack  [2] Roulette  [3] Slots  [4] Plinko\n[ESC] Quitter"},
    {"slots",      "slots — machine a sous avec GregCoins\n[ESPACE]=Lancer  [+/-]=ajuster mise  [ESC]=quitter\nJackpot GRG x3 = 100x. Balance partagee avec tous les jeux casino."},
    {"plinko",     "plinko — Plinko avec GregCoins\n[ESPACE]=Lancer  [+/-]=ajuster mise  [ESC]=Retour\nGains: 8x 4x 2x | X = perdu. 8 rangees de chevilles."},
    {"atm",        "atm — distributeur de GregCoins\nAjoutez jusqu a 1000 GC a votre solde casino."},
    {"music",      "music — PC Speaker music player\nUsage: music <nom>  |  music list\nSongs: tetris mario imperial zelda gregos  |  ESC = stop"},
    {"sh",      "sh — run script file\nUsage: sh <file>"},
    {"env",     "env — show environment variables\nUsage: env"},
    {"setenv",  "setenv — set environment variable\nUsage: setenv KEY VALUE"},
    {"history", "history — show command history\nUsage: history"},
    {"tree",    "tree — show directory tree\nUsage: tree"},
    {"uname",   "uname — print system info\nUsage: uname"},
    {"fortune", "fortune — display a random quote\nUsage: fortune"},
    {"neofetch","neofetch — system info with logo\nUsage: neofetch"},
    {"matrix",  "matrix — Matrix rain animation\nUsage: matrix  ESC=quit"},
    {"cowsay",  "cowsay — ASCII art cow\nUsage: cowsay <text>"},
    {"clock",   "clock — live ASCII clock\nUsage: clock  ESC=quit"},
    {"banner",  "banner — large ASCII text\nUsage: banner <text>"},
    {"ps",      "ps — list processes (fake)\nUsage: ps"},
    {"top",     "top — live system monitor\nUsage: top  ESC/q=quit"},
    {"df",      "df — filesystem disk usage\nUsage: df"},
    {"free",    "free — memory usage\nUsage: free"},
    {"head",    "head — first N lines of file\nUsage: head [-N] <file>  (default N=10)"},
    {"tail",    "tail — last N lines of file\nUsage: tail [-N] <file>  (default N=10)"},
    {"sort",    "sort — sort file lines alphabetically\nUsage: sort <file>"},
    {"tac",     "tac — reverse cat (lines in reverse)\nUsage: tac <file>"},
    {"sleep",   "sleep — wait N seconds\nUsage: sleep <N>"},
    {"stat",    "stat — file metadata\nUsage: stat <file>"},
    {"rev",     "rev — reverse a string\nUsage: rev <text>"},
    {"uptime",  "uptime — time since boot (command count)\nUsage: uptime"},
    {"yes",     "yes — repeat text forever (20 lines)\nUsage: yes [text]"},
    {"seq",     "seq — print a range of numbers\nUsage: seq <N>  or  seq <M> <N>"},
    {"lolcat",  "lolcat — rainbow colored text\nUsage: lolcat <text>"},
    {"write",   "write — write text directly to file\nUsage: write <file> <content>"},
    {"diff",    "diff — compare two files line by line\nUsage: diff <file1> <file2>"},
    {"rot13",   "rot13 — ROT13 cipher on text\nUsage: rot13 <text>"},
    {"chmod",   "chmod — change file permissions (simulated)\nUsage: chmod <perms> <file>"},
    {"alias",   "alias — define command shortcuts\nUsage: alias name=command\n       alias (list)"},
    {"unalias", "unalias — remove an alias\nUsage: unalias <name>"},
    {"which",   "which — show command location\nUsage: which <command>"},
    {"base64",  "base64 — encode/decode base64\nUsage: base64 <text>  |  base64 -d <b64>"},
    {"pong",    "pong — Pong v2 (ball acceleration, rally counter)\nControls: Z/S or UP/DN=player paddle  P=pause  ESC=quit"},
    {"invaders","invaders — Space Invaders v2 (4 rows, animated)\nControls: Q/D=move SPACE=shoot P=pause ESC=quit"},
    {0, 0}
};


void start_matrix(void) {
    term_init();
    gui_game_start();
    int head[VGA_WIDTH], len[VGA_WIDTH], wait[VGA_WIDTH], spd[VGA_WIDTH];
    for (int i = 0; i < VGA_WIDTH; i++) {
        head[i] = -(rand() % (VGA_HEIGHT+5));
        len[i]  = 4 + rand() % 14;
        spd[i]  = 1 + rand() % 3;
        wait[i] = rand() % spd[i];
    }
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@#$%&*+=<>";
    int nc = 46;

    term_move_cursor(20, 12);
    term_set_color(0x0F, 0x00);
    term_print("[ESC to exit]");

    while (1) {
        int c = get_monitor_char();
        if (c == KEY_ESC) { term_set_color(0x0F,0x00); term_init(); draw_interface(); return; }

        for (int col = 0; col < VGA_WIDTH; col++) {
            wait[col]--;
            if (wait[col] > 0) continue;
            wait[col] = spd[col];

            int h = head[col];


            if (h >= 0 && h < VGA_HEIGHT) {
                term_move_cursor(col, h);
                term_set_color(0x0F, 0x00);
                term_putc(chars[rand() % nc]);
            }


            if (h-1 >= 0 && h-1 < VGA_HEIGHT) {
                term_move_cursor(col, h-1);
                term_set_color(0x0A, 0x00);
                term_putc(chars[rand() % nc]);
            }


            for (int j = 2; j < len[col]; j++) {
                int row = h - j;
                if (row >= 0 && row < VGA_HEIGHT) {
                    term_move_cursor(col, row);
                    term_set_color(0x02, 0x00);
                    term_putc(chars[rand() % nc]);
                }
            }


            int tail = h - len[col];
            if (tail >= 0 && tail < VGA_HEIGHT) {
                term_move_cursor(col, tail);
                term_set_color(0x00, 0x00);
                term_putc(' ');
            }
            head[col]++;
            if (head[col] - len[col] > VGA_HEIGHT + 3) {
                head[col] = -(rand() % (VGA_HEIGHT));
                len[col]  = 4 + rand() % 14;
                spd[col]  = 1 + rand() % 3;
            }
        }
        nop_delay(MATRIX_TICK);
    }
}


void cmd_cowsay(const char* text) {
    int len = strlen(text);
    term_print(" ");
    for (int i = 0; i < len+2; i++) term_putc('_');
    term_putc('\n');
    term_print("< "); term_print(text); term_print(" >\n");
    term_print(" ");
    for (int i = 0; i < len+2; i++) term_putc('-');
    term_putc('\n');
    term_print("        \\   ^__^\n");
    term_print("         \\  (oo)\\_______\n");
    term_print("            (__)\\       )\\/\\\n");
    term_print("                ||----w |\n");
    term_print("                ||     ||\n");
}


static const char* digit_font[10][5] = {
    {" ## ","#  #","#  #","#  #"," ## "},
    {"  # "," ## ","  # ","  # "," ###"},
    {" ## ","   #"," ## ","#   ","####"},
    {" ## ","   #"," ## ","   #"," ## "},
    {"#  #","#  #","####","   #","   #"},
    {"####","#   ","### ","   #","### "},
    {" ## ","#   ","### ","#  #"," ## "},
    {"####","  # ","  # "," #  "," #  "},
    {" ## ","#  #"," ## ","#  #"," ## "},
    {" ## ","#  #","####","   #"," ## "},
};

void print_big_char(char c, int x, int y) {
    if (c == ':') {
        term_move_cursor(x, y+1); term_print("\x1b[33m#\x1b[0m");
        term_move_cursor(x, y+3); term_print("\x1b[33m#\x1b[0m");
        return;
    }
    if (c < '0' || c > '9') return;
    int d = c - '0';
    for (int row = 0; row < 5; row++) {
        term_move_cursor(x, y + row);
        term_print("\x1b[33m");
        term_print(digit_font[d][row]);
        term_print("\x1b[0m");
    }
}

void start_clock(void) {
    term_init();
    gui_game_start();
    term_move_cursor(18, 20);
    term_set_color(0x08, 0x00);
    term_print("[ Press ESC to exit ]");

    char prev[6] = ""; char cur[6];
    while (1) {
        int c = get_monitor_char();
        if (c == KEY_ESC) { draw_interface(); return; }

        get_time_string(cur);
        if (strcmp(prev, cur) != 0) {
            strcpy(prev, cur);


            int sx = (VGA_WIDTH - 30) / 2;
            int sy = (VGA_HEIGHT - 5) / 2 - 2;


            int offsets[] = {0, 5, 10, 15, 20};
            for (int i = 0; i < 5; i++) {
                print_big_char(cur[i], sx + offsets[i], sy);
            }


            char ds[12]; get_date_string(ds);
            term_move_cursor((VGA_WIDTH - 10) / 2, sy + 7);
            term_print("\x1b[36m");
            term_print(ds);
            term_print("\x1b[0m");
        }
        for (int i = 0; i < 500000; i++) __asm__ volatile("nop");
    }
}


static const char* letter_font[26][5] = {
    {" ## ","#  #","####","#  #","#  #"},

    {"### ","#  #","### ","#  #","### "},

    {" ###","#   ","#   ","#   "," ###"},

    {"### ","#  #","#  #","#  #","### "},

    {"####","#   ","### ","#   ","####"},

    {"####","#   ","### ","#   ","#   "},

    {" ###","#   ","# ##","#  #"," ###"},

    {"#  #","#  #","####","#  #","#  #"},

    {"####"," ## "," ## "," ## ","####"},

    {" ###","   #","   #","#  #"," ## "},

    {"#  #","# # ","##  ","# # ","#  #"},

    {"#   ","#   ","#   ","#   ","####"},

    {"#  #","####","#  #","#  #","#  #"},

    {"#  #","## #","# ##","#  #","#  #"},

    {" ## ","#  #","#  #","#  #"," ## "},

    {"### ","#  #","### ","#   ","#   "},

    {" ## ","#  #","# ##"," ## ","   #"},

    {"### ","#  #","### ","# # ","#  #"},

    {" ###","#   "," ## ","   #","### "},

    {"####"," ## "," ## "," ## "," ## "},

    {"#  #","#  #","#  #","#  #"," ## "},

    {"#  #","#  #"," ## "," ## ","  # "},

    {"#  #","#  #","#  #","####"," ## "},

    {"#  #"," ## ","  # "," ## ","#  #"},

    {"#  #"," ## ","  # ","  # ","  # "},

    {"####","   #"," ## ","#   ","####"},

};

static const unsigned char banner_colors[] = {
    0x0C,0x0E,0x0A,0x0B,0x09,0x0D,0x0C,0x0E,0x0A,0x0B
};

void cmd_banner(const char* text) {
    if (!text || !text[0]) { term_print("Usage: banner <text>\n"); return; }
    int len = strlen(text);
    int ci = 0;
    for (int row = 0; row < 5; row++) {
        for (int i = 0; i < len; i++) {
            char ch = text[i];
            if (ch >= 'a' && ch <= 'z') ch -= 32;
            term_set_color(banner_colors[(ci+i) % 10], 0x00);
            if (ch >= 'A' && ch <= 'Z') {
                term_print(letter_font[ch-'A'][row]);
                term_putc(' ');
            } else if (ch >= '0' && ch <= '9') {
                term_print(digit_font[ch-'0'][row]);
                term_putc(' ');
            } else {
                term_print("     ");
            }
        }
        ci++;
        term_set_color(0x0F, 0x00);
        term_putc('\n');
    }
}


void cmd_ps(void) {
    term_print("\x1b[36m  PID  TTY    STAT  TIME    COMMAND\x1b[0m\n");
    term_print("    1  tty0   Ss    0:00    /sbin/init\n");
    term_print("    2  tty0   S     0:00    [kthreadd]\n");
    term_print("    3  tty0   S     0:00    [ksoftirqd/0]\n");
    term_print("    7  tty0   S     0:00    [migration/0]\n");
    term_print("   42  tty0   S     0:00    gregsh\n");
    term_print("   43  tty0   R+    0:00    ps\n");
}


void cmd_top(void) {
    char ts[12]; get_time_string(ts);
    term_print("\x1b[36m  PID  USER   %CPU  %MEM  COMMAND\x1b[0m\n");
    static const char* procs[] = {
        "    1  root    0.0   0.1  init",
        "    2  root    0.0   0.0  kthreadd",
        "    3  root    0.0   0.0  ksoftirqd",
        "    7  root    0.0   0.0  migration",
        "   42  root    0.1   0.2  gregsh",
        "   43  root    0.2   0.1  top",
    };
    for (int i = 0; i < 6; i++) {
        term_print(procs[i]);
        term_putc('\n');
    }
    term_print("\x1b[33mMem:\x1b[0m 120/160 MB  |  Load: 0.12  |  Time: ");
    term_print(ts);
    term_putc('\n');
}


void cmd_df(void) {
    int used=0, used_bytes=0;
    for (int i=0;i<MAX_FILES;i++) {
        if (file_system[i].exists) { used++; used_bytes += file_system[i].size; }
    }
    int total_bytes = MAX_FILES * FILE_CONTENT_SIZE;
    int pct = (used_bytes * 100) / (total_bytes ? total_bytes : 1);
    term_print("\x1b[36mFilesystem     Size    Used    Avail   Use%  Mounted\x1b[0m\n");
    term_print("gregfs         ");
    term_print_int(total_bytes/1024); term_print("K    ");
    term_print_int(used_bytes/1024);  term_print("K    ");
    term_print_int((total_bytes-used_bytes)/1024); term_print("K    ");
    term_print_int(pct); term_print("%    /\n");
    term_print("\nFiles: ");
    term_print_int(used); term_print("/"); term_print_int(MAX_FILES); term_putc('\n');
}


void cmd_free(void) {
    unsigned int used  = kmalloc_used();
    unsigned int free_ = HEAP_SIZE - used;
    term_print("\x1b[36m         total    used    free\x1b[0m\n");
    term_print("Heap:    ");
    term_set_color(0x0F, 0x00); term_print_int(HEAP_SIZE);
    term_print("   ");
    term_set_color(used > HEAP_SIZE * 3 / 4 ? 0x0C : 0x0A, 0x00); term_print_int(used);
    term_print("   ");
    term_set_color(0x0B, 0x00); term_print_int(free_);
    term_set_color(0x07, 0x00); term_print(" bytes\n");
    term_print("Stack:   16384 bytes (static)\n");
    term_print("VGA:     4000 bytes (0xB8000)\n");
    term_set_color(0x0F, 0x00);
}


void cmd_head(const char* fname, int n) {
    const char* c = 0;
    if ((!fname || !fname[0]) && pipe_stdin) { c = pipe_stdin; }
    else {
        int idx = find_file(fname);
        if (idx == -1) { term_print("head: file not found.\n"); return; }
        c = file_system[idx].content;
    }
    int lines = 0, i = 0;
    char line[BUFFER_SIZE]; int li = 0;
    while (c[i] && lines < n) {
        if (c[i] == '\n') {
            line[li] = '\0'; term_print(line); term_putc('\n');
            li = 0; lines++;
        } else if (li < BUFFER_SIZE-1) line[li++] = c[i];
        i++;
    }
    if (li > 0 && lines < n) { line[li]='\0'; term_print(line); term_putc('\n'); }
}

void cmd_tail(const char* fname, int n) {
    const char* c = 0;
    if ((!fname || !fname[0]) && pipe_stdin) { c = pipe_stdin; }
    else {
        int idx = find_file(fname);
        if (idx == -1) { term_print("tail: file not found.\n"); return; }
        c = file_system[idx].content;
    }


    char lines[32][BUFFER_SIZE]; int nlns = 0;
    int i = 0, li = 0;
    while (c[i]) {
        if (c[i] == '\n') {
            if (nlns < 32) { lines[nlns][li]='\0'; nlns++; }
            li = 0;
        } else if (li < BUFFER_SIZE-1) lines[nlns][li++] = c[i];
        i++;
    }
    if (li > 0 && nlns < 32) { lines[nlns][li]='\0'; nlns++; }
    int start = nlns - n; if (start < 0) start = 0;
    for (int j = start; j < nlns; j++) { term_print(lines[j]); term_putc('\n'); }
}

void cmd_tac(const char* fname) {
    const char* c = 0;
    if ((!fname || !fname[0]) && pipe_stdin) { c = pipe_stdin; }
    else {
        int idx = find_file(fname);
        if (idx == -1) { term_print("tac: file not found.\n"); return; }
        c = file_system[idx].content;
    }
    char lines[32][BUFFER_SIZE]; int nlns = 0;
    int i = 0, li = 0;
    while (c[i]) {
        if (c[i] == '\n') {
            if (nlns < 32) { lines[nlns][li]='\0'; nlns++; }
            li = 0;
        } else if (li < BUFFER_SIZE-1) lines[nlns][li++] = c[i];
        i++;
    }
    if (li > 0 && nlns < 32) { lines[nlns][li]='\0'; nlns++; }
    for (int j = nlns-1; j >= 0; j--) { term_print(lines[j]); term_putc('\n'); }
}

void cmd_sort(const char* fname, int reverse) {
    cmd_sort_flags(fname, reverse, 0, 0);
}

void cmd_sort_flags(const char* fname, int reverse, int numeric, int unique) {
    const char* c = 0;
    if ((!fname || !fname[0]) && pipe_stdin) { c = pipe_stdin; }
    else {
        int idx = find_file(fname); if (idx < 0) idx = find_file_path(fname);
        if (idx == -1) { term_print("sort: file not found.\n"); return; }
        c = file_system[idx].content;
    }
    char lines[64][BUFFER_SIZE]; int nlns = 0;
    int i = 0, li = 0;
    while (c[i]) {
        if (c[i] == '\n') {
            if (nlns < 64) { lines[nlns][li]='\0'; nlns++; }
            li = 0;
        } else if (li < BUFFER_SIZE-1) lines[nlns][li++] = c[i];
        i++;
    }
    if (li > 0 && nlns < 64) { lines[nlns][li]='\0'; nlns++; }

    for (int a = 1; a < nlns; a++) {
        char tmp[BUFFER_SIZE]; strcpy(tmp, lines[a]);
        int b = a - 1;
        int cmp;
        while (b >= 0) {
            if (numeric) cmp = atoi(lines[b]) - atoi(tmp);
            else cmp = strcmp(lines[b], tmp);
            if (cmp <= 0) break;
            strcpy(lines[b+1], lines[b]); b--;
        }
        strcpy(lines[b+1], tmp);
    }
    const char* prev = 0;
    if (reverse) {
        for (int j = nlns-1; j >= 0; j--) {
            if (unique && prev && strcmp(lines[j], prev) == 0) continue;
            term_print(lines[j]); term_putc('\n'); prev = lines[j];
        }
    } else {
        for (int j = 0; j < nlns; j++) {
            if (unique && prev && strcmp(lines[j], prev) == 0) continue;
            term_print(lines[j]); term_putc('\n'); prev = lines[j];
        }
    }
}


void cmd_diff(const char* f1, const char* f2) {
    int i1 = find_file(f1), i2 = find_file(f2);
    if (i1==-1) { term_print("diff: "); term_print(f1); term_print(": not found\n"); return; }
    if (i2==-1) { term_print("diff: "); term_print(f2); term_print(": not found\n"); return; }



    char l1[32][BUFFER_SIZE], l2[32][BUFFER_SIZE];
    int n1=0, n2=0;
    const char* c;
    int li, k;

    c = file_system[i1].content; li=0; k=0;
    while (c[k] && n1<32) {
        if (c[k]=='\n') { l1[n1][li]='\0'; n1++; li=0; }
        else if (li<BUFFER_SIZE-1) l1[n1][li++]=c[k];
        k++;
    }
    if (li>0 && n1<32) { l1[n1][li]='\0'; n1++; }

    c = file_system[i2].content; li=0; k=0;
    while (c[k] && n2<32) {
        if (c[k]=='\n') { l2[n2][li]='\0'; n2++; li=0; }
        else if (li<BUFFER_SIZE-1) l2[n2][li++]=c[k];
        k++;
    }
    if (li>0 && n2<32) { l2[n2][li]='\0'; n2++; }

    int max = n1 > n2 ? n1 : n2;
    int diffs = 0;
    for (int j = 0; j < max; j++) {
        const char* la = (j < n1) ? l1[j] : "";
        const char* lb = (j < n2) ? l2[j] : "";
        if (strcmp(la, lb) != 0) {
            diffs++;
            term_set_color(0x08, 0x00); term_print_int(j+1); term_print("c"); term_print_int(j+1); term_print("\n");
            if (j < n1) { term_set_color(0x0C,0x00); term_print("< "); term_set_color(0x0F,0x00); term_print(la); term_putc('\n'); }
            term_set_color(0x08,0x00); term_print("---\n");
            if (j < n2) { term_set_color(0x0A,0x00); term_print("> "); term_set_color(0x0F,0x00); term_print(lb); term_putc('\n'); }
        }
    }
    if (!diffs) { term_set_color(0x0A,0x00); term_print("Files are identical.\n"); }
    else { term_set_color(0x08,0x00); term_print_int(diffs); term_print(" difference(s)\n"); }
    term_set_color(0x0F,0x00);
}


void cmd_sleep(int sec) {
    if (sec <= 0) return;
    term_print("Sleeping ");
    term_print_int(sec);
    term_print("s  ");
    term_set_color(0x08, 0x00);
    term_putc('[');
    int bar_w = 30;
    for (int i = 0; i < bar_w; i++) term_putc('\xc4');

    term_putc(']');


    for (int i = 0; i < bar_w + 1; i++) term_putc('\b');
    term_set_color(theme_sep[current_theme], 0x00);
    for (int s = 0; s < bar_w; s++) {
        for (int i = 0; i < (sec * 5000000) / bar_w; i++) __asm__ volatile("nop");
        term_putc('\xdb');

    }


    term_set_color(0x0F, 0x00);
    term_putc(']');
    term_print(" \x1b[32mdone\x1b[0m\n");
}


void cmd_stat(const char* fname) {
    int idx = find_file(fname);
    if (idx == -1) { term_print("stat: file not found.\n"); return; }
    char ts[12], ds[12]; get_time_string(ts); get_date_string(ds);
    term_print("File:   "); term_print(file_system[idx].name); term_putc('\n');
    term_print("Type:   "); term_print(file_system[idx].type==TYPE_DIR?"directory":"regular file"); term_putc('\n');
    term_print("Size:   "); term_print_int(file_system[idx].size); term_print(" bytes\n");
    term_print("ID:     "); term_print_int(file_system[idx].id); term_putc('\n');
    term_print("Modify: "); term_print(ds); term_putc(' '); term_print(ts); term_putc('\n');
    term_print("Access: root\n");
}


void cmd_rev(const char* text) {
    int n = strlen(text);
    for (int i = n-1; i >= 0; i--) term_putc(text[i]);
    term_putc('\n');
}


void cmd_uptime(void) {
    unsigned long j = jiffies;
    unsigned long s = j / 100;
    unsigned long m = s / 60; s %= 60;
    unsigned long h = m / 60; m %= 60;
    term_print("up ");
    if (h > 0) { term_print_int((int)h); term_print("h "); }
    if (h > 0 || m > 0) { term_print_int((int)m); term_print("m "); }
    term_print_int((int)s); term_print("s");
    term_set_color(0x08, 0x00);
    term_print("  ("); term_print_int((int)j); term_print(" jiffies @ 100 Hz)\n");
    term_set_color(0x0F, 0x00);
}


#define SI_ROWS     4
#define SI_COLS    11
#define SI_HSEP     6
#define SI_VSEP     2
#define SI_STARTX   2
#define SI_STARTY   2
#define SI_PY      22
#define SI_MAX_BUL  5

static void si_hud(int score, int lives, int alive) {
    term_set_color(0x0F,0x01);
    term_move_cursor(0,0);
    for(int i=0;i<VGA_WIDTH;i++) term_putc(' ');
    term_move_cursor(1,0);  term_print("INVADERS");
    term_move_cursor(14,0); term_print("Score:"); term_print_int(score); term_print("  ");
    term_move_cursor(32,0); term_print("Lives:");
    term_set_color(0x0A,0x01);
    for(int l=0;l<lives;l++) term_print("(^)");
    term_move_cursor(56,0);
    term_set_color(0x0F,0x01);
    term_print("Left:"); term_print_int(alive); term_print("  ");
    term_set_color(0x0F,0x00);
}

static void si_draw(int r, int c, int gx, int gy, int alive, int frame) {
    int ix=SI_STARTX+gx+c*SI_HSEP;
    int iy=SI_STARTY+gy+r*SI_VSEP;
    if(ix<0||ix>=VGA_WIDTH-4||iy<1||iy>=VGA_HEIGHT) return;
    term_move_cursor(ix,iy);
    if(!alive) { term_print("    "); return; }
    int anim=frame&1;
    if(r==0) {
        term_set_color(0x0C,0x00);
        term_print(anim ? "/=\\" : "\\=/");
    } else if(r==1) {
        term_set_color(0x0D,0x00);
        term_print(anim ? "(o)" : "[o]");
    } else if(r==2) {
        term_set_color(0x0E,0x00);
        term_print(anim ? "<W>" : "{W}");
    } else {
        term_set_color(0x09,0x00);
        term_print(anim ? "vVv" : "^A^");
    }
    term_set_color(0x0F,0x00);
}

void start_invaders(void) {
    term_init();
    gui_game_start();
    static int inv[SI_ROWS][SI_COLS];
    for(int r=0;r<SI_ROWS;r++) for(int c=0;c<SI_COLS;c++) inv[r][c]=1;

    int gx=0,gy=0,gdir=1;
    int px=38;
    int pbx=-1,pby=-1;
    int ebx[SI_MAX_BUL],eby[SI_MAX_BUL];
    for(int i=0;i<SI_MAX_BUL;i++){ebx[i]=-1;eby[i]=-1;}

    int score=0,lives=3,alive=SI_ROWS*SI_COLS,frame=0,move_every=40;
    int anim_frame=0;



    term_set_color(0x02,0x00);
    for(int b=0;b<4;b++) {
        term_move_cursor(6+b*18,SI_PY-2);
        term_print("\xdb\xdb\xdb\xdb");
    }
    term_set_color(0x08,0x00);
    term_move_cursor(0,23);
    term_print("Q/LEFT=move  D/RIGHT=move  SPACE=shoot  P=pause  ESC=quit");
    term_set_color(0x0F,0x00);

    for(int r=0;r<SI_ROWS;r++)
        for(int c=0;c<SI_COLS;c++)
            si_draw(r,c,gx,gy,1,0);
    si_hud(score,lives,alive);

    int paused=0;
    while(lives>0&&alive>0) {
        int ch=get_monitor_char();
        if(ch==KEY_ESC){draw_interface();return;}
        if(ch=='p'||ch=='P') paused=!paused;
        if(paused) { nop_delay(INVADERS_TICK); continue; }

        if((ch=='q'||ch==KEY_LEFT)&&px>2)           px--;
        if((ch=='d'||ch==KEY_RIGHT)&&px<VGA_WIDTH-5) px++;
        if(ch==' '&&pbx==-1){pbx=px+1;pby=SI_PY-1;}



        if(pbx!=-1) {
            term_move_cursor(pbx,pby); term_putc(' ');
            pby--;
            if(pby<SI_STARTY+1){pbx=-1;}
            else {
                int hit=0;
                for(int r=0;r<SI_ROWS&&!hit;r++) {
                    for(int c=0;c<SI_COLS&&!hit;c++) {
                        if(!inv[r][c]) continue;
                        int ix=SI_STARTX+gx+c*SI_HSEP;
                        int iy=SI_STARTY+gy+r*SI_VSEP;
                        if(pbx>=ix&&pbx<=ix+2&&pby==iy) {
                            inv[r][c]=0; alive--;
                            score+=(SI_ROWS-r)*10+(SI_ROWS*SI_COLS-alive);
                            pbx=-1; hit=1;
                            term_move_cursor(ix,iy);
                            term_set_color(0x0E,0x00); term_print("***");
                            nop_delay(10000000);
                            si_draw(r,c,gx,gy,0,anim_frame);
                            si_hud(score,lives,alive);
                        }
                    }
                }
                if(!hit&&pbx!=-1){
                    term_move_cursor(pbx,pby);
                    term_set_color(0x0F,0x00); term_putc('|');
                }
            }
        }



        for(int i=0;i<SI_MAX_BUL;i++) {
            if(ebx[i]==-1) continue;
            term_move_cursor(ebx[i],eby[i]); term_putc(' ');
            eby[i]++;
            if(eby[i]>=SI_PY) {
                if(ebx[i]>=px&&ebx[i]<=px+2) {
                    lives--;
                    term_move_cursor(px,SI_PY);
                    term_set_color(0x0C,0x00); term_print("XXX");
                    nop_delay(100000000);
                    term_move_cursor(px,SI_PY); term_print("   ");
                    si_hud(score,lives,alive);
                }
                ebx[i]=-1;
            } else {
                term_move_cursor(ebx[i],eby[i]);
                term_set_color(0x0C,0x00); term_putc('!');
            }
        }



        if(alive>0&&frame%move_every==0) {
            anim_frame^=1;
            for(int r=0;r<SI_ROWS;r++)
                for(int c=0;c<SI_COLS;c++)
                    if(inv[r][c]) si_draw(r,c,gx,gy,0,anim_frame);
            int lc=SI_COLS,rc=-1;
            for(int r=0;r<SI_ROWS;r++)
                for(int c=0;c<SI_COLS;c++)
                    if(inv[r][c]){if(c<lc)lc=c;if(c>rc)rc=c;}
            int lx=SI_STARTX+gx+lc*SI_HSEP;
            int rx=SI_STARTX+gx+rc*SI_HSEP+2;
            if(gdir==1&&rx>=VGA_WIDTH-2){gdir=-1;gy+=2;}
            else if(gdir==-1&&lx<=1){gdir=1;gy+=2;}
            else gx+=gdir;
            for(int r=0;r<SI_ROWS;r++)
                for(int c=0;c<SI_COLS;c++)
                    if(inv[r][c]&&SI_STARTY+gy+r*SI_VSEP>=SI_PY-1) lives=0;
            for(int r=0;r<SI_ROWS;r++)
                for(int c=0;c<SI_COLS;c++)
                    if(inv[r][c]) si_draw(r,c,gx,gy,1,anim_frame);
            move_every=3+(alive*37)/(SI_ROWS*SI_COLS);
        }



        if(frame%20==0&&alive>0) {
            int slot=-1;
            for(int i=0;i<SI_MAX_BUL;i++) if(ebx[i]==-1){slot=i;break;}
            if(slot!=-1) {
                int col=rand()%SI_COLS;
                for(int r=SI_ROWS-1;r>=0;r--) {
                    if(inv[r][col]) {
                        ebx[slot]=SI_STARTX+gx+col*SI_HSEP+1;
                        eby[slot]=SI_STARTY+gy+r*SI_VSEP+1;
                        break;
                    }
                }
            }
        }

        term_move_cursor(px,SI_PY);
        term_set_color(0x0A,0x00); term_print("(^)");
        term_set_color(0x0F,0x00);
        frame++;
        nop_delay(INVADERS_TICK);
    }

    hs_update(2, score);
    if (alive == 0) beep_ok(); else beep_gameover();
    int oy=9;
    if(alive==0) {
        term_set_color(0x0A,0x00);
    } else {
        term_set_color(0x0C,0x00);
    }
    term_move_cursor(28,oy);
    term_print("\xda\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xbf");
    term_move_cursor(28,oy+1);
    if(alive==0) term_print("\xb3  YOU WIN!  Score:");
    else         term_print("\xb3 GAME OVER  Score:");
    term_print_int(score);
    if (score == hs_scores[2]) { term_set_color(0x0E,0x00); term_print(" \xdb BEST"); }
    term_set_color(alive==0?0x0A:0x0C,0x00);
    term_print("  \xb3");
    term_move_cursor(28,oy+2);
    term_print("\xc0\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xd9");
    term_set_color(0x0F,0x00);
    term_move_cursor(30,oy+4); term_print("Press any key...");
    while(!get_monitor_char());
    draw_interface();
}
#undef SI_ROWS
#undef SI_COLS
#undef SI_HSEP
#undef SI_VSEP
#undef SI_STARTX
#undef SI_STARTY
#undef SI_PY
#undef SI_MAX_BUL


#define PONG_W  72
#define PONG_H  21
#define PONG_OX  4
#define PONG_OY  2

void start_pong(void) {
    term_init();
    gui_game_start();

    term_set_color(0x0F,0x01);
    term_move_cursor(0,0);
    for(int i=0;i<VGA_WIDTH;i++) term_putc(' ');
    term_move_cursor(1,0);
    term_print("PONG  Z/S or UP/DN=player(right)  P=pause  ESC=quit  First to 7!");

    int lpad=PONG_H/2-2, rpad=PONG_H/2-2, pad_h=5;
    int bx=PONG_W/2, by=PONG_H/2;
    int bdx=1, bdy=1;
    int lscore=0, rscore=0, frame=0, paused=0;
    int rally=0;



    term_set_color(0x07,0x00);
    term_move_cursor(PONG_OX-1,PONG_OY-1);
    term_putc('\xDA'); for(int i=0;i<PONG_W;i++) term_putc('\xC4'); term_putc('\xBF');
    for(int y=0;y<PONG_H;y++) {
        term_move_cursor(PONG_OX-1,PONG_OY+y); term_putc('\xB3');
        term_move_cursor(PONG_OX+PONG_W,PONG_OY+y); term_putc('\xB3');
    }
    term_move_cursor(PONG_OX-1,PONG_OY+PONG_H);
    term_putc('\xC0'); for(int i=0;i<PONG_W;i++) term_putc('\xC4'); term_putc('\xD9');

    int prev_bx=bx, prev_by=by;
    int prev_lpad=lpad, prev_rpad=rpad;

    while(1) {
        int c=get_monitor_char();
        if(c==KEY_ESC){draw_interface();return;}
        if(c=='p'||c=='P') {
            paused=!paused;
            term_move_cursor(PONG_OX+PONG_W/2-3,PONG_OY+PONG_H/2);
            if(paused){term_set_color(0x0E,0x00);term_print("PAUSED");}
            else{term_set_color(0x00,0x00);term_print("      ");}
        }
        if(paused){nop_delay(PONG_TICK);continue;}



        term_move_cursor(PONG_OX+prev_bx,PONG_OY+prev_by);
        term_putc(' ');
        for(int y=0;y<PONG_H;y++) {
            if(y>=prev_lpad&&y<prev_lpad+pad_h) {
                term_move_cursor(PONG_OX,PONG_OY+y);
                term_putc(' ');
            }
            if(y>=prev_rpad&&y<prev_rpad+pad_h) {
                term_move_cursor(PONG_OX+PONG_W-1,PONG_OY+y);
                term_putc(' ');
            }
        }

        if((c=='z'||c==KEY_UP)&&rpad>0) rpad--;
        if((c=='s'||c==KEY_DOWN)&&rpad+pad_h<PONG_H) rpad++;



        int ai_lag=4-(lscore-rscore)/2;
        if(ai_lag<1)ai_lag=1;
        if(frame%ai_lag==0) {
            int mid=lpad+pad_h/2;
            if(by>mid&&lpad+pad_h<PONG_H) lpad++;
            else if(by<mid&&lpad>0) lpad--;
        }

        prev_bx=bx; prev_by=by;
        prev_lpad=lpad; prev_rpad=rpad;

        bx+=bdx; by+=bdy;
        if(by<=0){by=0;bdy=1;}
        if(by>=PONG_H-1){by=PONG_H-1;bdy=-1;}



        if(bx>=PONG_W-1&&by>=rpad&&by<rpad+pad_h) {
            bx=PONG_W-2; bdx=-1;
            int rel=by-rpad;
            bdy=(rel<pad_h/3)?-1:(rel>=2*pad_h/3)?1:bdy;
            rally++;
        }


        if(bx<=0&&by>=lpad&&by<lpad+pad_h) {
            bx=1; bdx=1;
            int rel=by-lpad;
            bdy=(rel<pad_h/3)?-1:(rel>=2*pad_h/3)?1:bdy;
            rally++;
        }



        if(bx>=PONG_W){lscore++;bx=PONG_W/2;by=PONG_H/2;bdx=-1;rally=0;}
        if(bx<0)      {rscore++;bx=PONG_W/2;by=PONG_H/2;bdx=1; rally=0;}
        if(bx<0) bx=0;
        if(bx>=PONG_W) bx=PONG_W-1;



        for(int y=0;y<PONG_H;y++) {
            term_move_cursor(PONG_OX+PONG_W/2,PONG_OY+y);
            term_set_color(0x08,0x00); term_putc(y%2?':':' ');
        }



        for(int y=0;y<PONG_H;y++) {
            if(y>=lpad&&y<lpad+pad_h) {
                term_move_cursor(PONG_OX,PONG_OY+y);
                term_set_color(0x09,0x00); term_putc('\xDB');
            }
            if(y>=rpad&&y<rpad+pad_h) {
                term_move_cursor(PONG_OX+PONG_W-1,PONG_OY+y);
                term_set_color(0x0A,0x00); term_putc('\xDB');
            }
        }



        term_move_cursor(PONG_OX+bx,PONG_OY+by);
        term_set_color(rally>8?0x0F:0x0E,0x00); term_putc('O');



        term_move_cursor(PONG_OX+PONG_W/2-10,PONG_OY+PONG_H+1);
        term_set_color(0x09,0x00); term_print("AI:"); term_print_int(lscore);
        term_set_color(0x07,0x00); term_print("  vs  ");
        term_set_color(0x0A,0x00); term_print("YOU:"); term_print_int(rscore);
        term_set_color(0x08,0x00);
        if(rally>0){term_print("  rally:");term_print_int(rally);}
        else{term_print("            ");}

        if(lscore>=7||rscore>=7) {
            term_move_cursor(PONG_OX+PONG_W/2-6,PONG_OY+PONG_H/2);
            if(rscore>=7){term_set_color(0x0A,0x00);term_print("  YOU WIN!  ");}
            else{term_set_color(0x0C,0x00);term_print(" AI WINS... ");}
            term_set_color(0x0F,0x00);
            term_move_cursor(PONG_OX+PONG_W/2-7,PONG_OY+PONG_H/2+2);
            term_print("Press any key...");
            while(!get_monitor_char());
            draw_interface(); return;
        }

        frame++;


        int tick=PONG_TICK-(rally*800000);
        if(tick<6000000) tick=6000000;
        nop_delay(tick);
    }
}
#undef PONG_W
#undef PONG_H
#undef PONG_OX
#undef PONG_OY


#define BRK_W    70
#define BRK_H    20
#define BRK_OX    5
#define BRK_OY    3
#define BRK_ROWS  6
#define BRK_COLS 14
#define BRK_BW    5
#define BRK_PAD   8

void start_breakout(void) {
    term_init();
    gui_game_start();
    static unsigned char bricks[BRK_ROWS][BRK_COLS];
    for(int r=0;r<BRK_ROWS;r++) for(int c=0;c<BRK_COLS;c++) bricks[r][c]=1;
    int alive=BRK_ROWS*BRK_COLS, score=0, lives=3;
    int pad_x=BRK_W/2-BRK_PAD/2;
    int bx=BRK_W/2, by=BRK_H-3;
    int bdx=1, bdy=-1;
    static const unsigned char brk_col[BRK_ROWS]={0x0C,0x0E,0x0D,0x0B,0x0A,0x09};
    static const int brk_pts[BRK_ROWS]={60,50,40,30,20,10};



    term_move_cursor(0,0);
    term_set_color(0x0B,0x00);
    term_print("BREAKOUT  Q/LEFT=move  D/RIGHT=move  P=pause  ESC=quit");



    term_set_color(0x07,0x00);
    term_move_cursor(BRK_OX-1,BRK_OY-1);
    term_putc('\xDA'); for(int i=0;i<BRK_W;i++) term_putc('\xC4'); term_putc('\xBF');
    for(int y=0;y<BRK_H;y++){
        term_move_cursor(BRK_OX-1,BRK_OY+y); term_putc('\xB3');
        term_move_cursor(BRK_OX+BRK_W,BRK_OY+y); term_putc('\xB3');
    }
    term_move_cursor(BRK_OX-1,BRK_OY+BRK_H);
    term_putc('\xC0'); for(int i=0;i<BRK_W;i++) term_putc('\xC4'); term_putc('\xD9');



    for(int r=0;r<BRK_ROWS;r++) {
        for(int c=0;c<BRK_COLS;c++) {
            term_move_cursor(BRK_OX+c*BRK_BW, BRK_OY+r+1);
            term_set_color(brk_col[r],0x00);
            term_print("[===]");
        }
    }



    term_move_cursor(1,1);
    term_set_color(0x0E,0x00); term_print("Score:0      ");
    term_move_cursor(20,1);
    term_set_color(0x0C,0x00); term_print("Lives:3 ");

    int paused=0;
    while(lives>0&&alive>0) {


        for(int x=0;x<BRK_W;x++) {
            term_move_cursor(BRK_OX+x, BRK_OY+BRK_H-1);
            if(x>=pad_x&&x<pad_x+BRK_PAD){term_set_color(0x0A,0x00);term_putc('\xDB');}
            else{term_set_color(0x00,0x00);term_putc(' ');}
        }


        term_move_cursor(BRK_OX+bx, BRK_OY+by);
        term_set_color(0x0F,0x00); term_putc('O');

        nop_delay(PONG_TICK);
        int c=get_monitor_char();
        if(c==KEY_ESC){draw_interface();return;}
        if(c=='p'||c=='P'){
            paused=!paused;
            term_move_cursor(BRK_OX+BRK_W/2-3, BRK_OY+BRK_H/2);
            if(paused){term_set_color(0x0E,0x00);term_print("PAUSED");}
            else{term_set_color(0x00,0x00);term_print("      ");}
        }
        if(paused) continue;



        term_move_cursor(BRK_OX+bx, BRK_OY+by);
        term_putc(' ');



        if((c=='q'||c==KEY_LEFT)&&pad_x>0) pad_x--;
        if((c=='d'||c==KEY_RIGHT)&&pad_x+BRK_PAD<BRK_W) pad_x++;



        bx+=bdx; by+=bdy;



        if(bx<=0){bx=0;bdx=1;}
        if(bx>=BRK_W-1){bx=BRK_W-1;bdx=-1;}
        if(by<=0){by=0;bdy=1;}



        if(by==BRK_H-1&&bx>=pad_x&&bx<pad_x+BRK_PAD) {
            bdy=-1;
            int rel=bx-pad_x;
            if(rel<BRK_PAD/3) bdx=-1;
            else if(rel>=2*BRK_PAD/3) bdx=1;
        }



        if(by>=BRK_H) {
            lives--;
            term_move_cursor(20,1);
            term_set_color(0x0C,0x00); term_print("Lives:"); term_print_int(lives); term_print(" ");
            bx=pad_x+BRK_PAD/2; by=BRK_H-3; bdy=-1;
            if(lives>0) {
                term_move_cursor(BRK_OX+BRK_W/2-7, BRK_OY+BRK_H/2);
                term_set_color(0x0E,0x00); term_print("SPACE to launch");
                while(1) {
                    int kc=get_monitor_char();
                    if(kc==KEY_ESC){draw_interface();return;}
                    if(kc==' '){
                        term_move_cursor(BRK_OX+BRK_W/2-7, BRK_OY+BRK_H/2);
                        term_set_color(0x00,0x00); term_print("               ");
                        break;
                    }
                    nop_delay(100000);
                }
            }
            continue;
        }



        if(by>=1&&by<=BRK_ROWS) {
            int br=by-1;
            int bc=bx/BRK_BW;
            if(br>=0&&br<BRK_ROWS&&bc>=0&&bc<BRK_COLS&&bricks[br][bc]) {
                bricks[br][bc]=0; alive--;
                score+=brk_pts[br];
                bdy=-bdy;
                term_move_cursor(BRK_OX+bc*BRK_BW, BRK_OY+br+1);
                term_set_color(0x00,0x00); term_print("     ");
                term_move_cursor(1,1);
                term_set_color(0x0E,0x00); term_print("Score:"); term_print_int(score); term_print("   ");
            }
        }
    }

    hs_update(3, score);
    if (alive == 0) beep_ok(); else beep_gameover();
    term_move_cursor(BRK_OX+BRK_W/2-9, BRK_OY+BRK_H/2);
    if(alive==0){term_set_color(0x0A,0x00);term_print("YOU WIN! Score:");}
    else{term_set_color(0x0C,0x00);term_print("GAME OVER! Score:");}
    term_print_int(score);
    if (score == hs_scores[3]) { term_set_color(0x0E,0x00); term_print(" \xdb BEST!"); }
    term_move_cursor(BRK_OX+BRK_W/2-7, BRK_OY+BRK_H/2+2);
    term_set_color(0x08,0x00); term_print("Press any key...");
    while(!get_monitor_char());
    draw_interface();
}
#undef BRK_W
#undef BRK_H
#undef BRK_OX
#undef BRK_OY
#undef BRK_ROWS
#undef BRK_COLS
#undef BRK_BW
#undef BRK_PAD


#define G48_SZ 4
#define G48_CW  7

#define G48_CH  3


static unsigned char g48_color(int val) {
    int n=0; int v=val; while(v>1){v>>=1;n++;}
    static const unsigned char cols[]={0x07,0x0F,0x0E,0x0B,0x0A,0x0D,0x0C,0x09,0x0B,0x0A,0x0E,0x0F,0x0F};
    if(n<13) return cols[n];
    return 0x0F;
}

static void g48_hline(int left, int mid, int right) {
    term_set_color(0x07,0x00);
    term_putc(left);
    for(int c=0;c<G48_SZ;c++) {
        for(int i=0;i<G48_CW;i++) term_putc('\xC4');
        term_putc(c<G48_SZ-1?mid:right);
    }
}

static void g48_draw(int board[G48_SZ][G48_SZ], int score, int best) {
    int ox=(VGA_WIDTH-(G48_SZ*G48_CW+G48_SZ+1))/2;
    int oy=3;

    term_move_cursor(ox,oy-2);
    term_set_color(0x0F,0x00); term_print("2048  ");
    term_set_color(0x0E,0x00); term_print("Score:"); term_print_int(score);
    term_print("  Best:"); term_print_int(best); term_print("   ");
    term_move_cursor(ox,oy-1);
    term_set_color(0x08,0x00);
    term_print("Z/UP  S/DN  Q/LT  D/RT  ESC=quit");

    for(int r=0;r<G48_SZ;r++) {


        term_move_cursor(ox, oy+r*(G48_CH+1));
        if(r==0) g48_hline('\xDA','\xC2','\xBF');
        else     g48_hline('\xC3','\xC5','\xB4');



        for(int cy=0;cy<G48_CH;cy++) {
            term_move_cursor(ox, oy+r*(G48_CH+1)+1+cy);
            for(int c=0;c<G48_SZ;c++) {
                term_set_color(0x07,0x00); term_putc('\xB3');
                int val=board[r][c];
                unsigned char col=val?g48_color(val):0x08;
                term_set_color(col,0x00);
                if(cy==G48_CH/2&&val) {
                    char tmp[16]; itoa(val,tmp);
                    int tlen=strlen(tmp);
                    int pad=(G48_CW-tlen)/2;
                    for(int i=0;i<pad;i++) term_putc(' ');
                    term_print(tmp);
                    for(int i=0;i<G48_CW-pad-tlen;i++) term_putc(' ');
                } else {
                    for(int i=0;i<G48_CW;i++) term_putc(' ');
                }
            }
            term_set_color(0x07,0x00); term_putc('\xB3');
        }
    }


    term_move_cursor(ox, oy+G48_SZ*(G48_CH+1));
    g48_hline('\xC0','\xC1','\xD9');
}

static int g48_slide_left(int row[G48_SZ], int *sc) {
    int buf[G48_SZ]={0,0,0,0}, j=0;
    for(int i=0;i<G48_SZ;i++) if(row[i]) buf[j++]=row[i];
    for(int i=0;i<G48_SZ-1;i++) {
        if(buf[i]&&buf[i]==buf[i+1]) {
            buf[i]*=2; *sc+=buf[i]; buf[i+1]=0;


            for(int k=i+1;k<G48_SZ-1;k++) buf[k]=buf[k+1];
            buf[G48_SZ-1]=0;
        }
    }
    int moved=0;
    for(int i=0;i<G48_SZ;i++){if(row[i]!=buf[i])moved=1; row[i]=buf[i];}
    return moved;
}

static void g48_spawn(int board[G48_SZ][G48_SZ]) {
    int empties=0;
    for(int r=0;r<G48_SZ;r++) for(int c=0;c<G48_SZ;c++) if(!board[r][c]) empties++;
    if(!empties) return;
    int pick=rand()%empties, idx=0;
    for(int r=0;r<G48_SZ;r++) for(int c=0;c<G48_SZ;c++) {
        if(!board[r][c]) { if(idx==pick){board[r][c]=(rand()%10<9)?2:4;return;} idx++; }
    }
}

static int g48_can_move(int board[G48_SZ][G48_SZ]) {
    for(int r=0;r<G48_SZ;r++) for(int c=0;c<G48_SZ;c++) {
        if(!board[r][c]) return 1;
        if(c<G48_SZ-1&&board[r][c]==board[r][c+1]) return 1;
        if(r<G48_SZ-1&&board[r][c]==board[r+1][c]) return 1;
    }
    return 0;
}

void start_2048(void) {
    term_init();
    gui_game_start();
    static int board[G48_SZ][G48_SZ];
    memset(board,0,sizeof(board));
    int score=0, best=0;
    g48_spawn(board); g48_spawn(board);
    g48_draw(board,score,best);

    while(1) {
        int c=0;
        while(!c) { nop_delay(50000); c=get_monitor_char(); }
        if(c==KEY_ESC){draw_interface();return;}

        int moved=0, tmp[G48_SZ];

        if(c=='q'||c==KEY_LEFT) {
            for(int r=0;r<G48_SZ;r++) moved|=g48_slide_left(board[r],&score);
        } else if(c=='d'||c==KEY_RIGHT) {
            for(int r=0;r<G48_SZ;r++) {


                for(int i=0;i<G48_SZ;i++) tmp[i]=board[r][G48_SZ-1-i];
                int m=g48_slide_left(tmp,&score);
                if(m){moved=1;for(int i=0;i<G48_SZ;i++) board[r][G48_SZ-1-i]=tmp[i];}
            }
        } else if(c=='z'||c==KEY_UP) {
            for(int col=0;col<G48_SZ;col++) {
                for(int i=0;i<G48_SZ;i++) tmp[i]=board[i][col];
                int m=g48_slide_left(tmp,&score);
                if(m){moved=1;for(int i=0;i<G48_SZ;i++) board[i][col]=tmp[i];}
            }
        } else if(c=='s'||c==KEY_DOWN) {
            for(int col=0;col<G48_SZ;col++) {
                for(int i=0;i<G48_SZ;i++) tmp[i]=board[G48_SZ-1-i][col];
                int m=g48_slide_left(tmp,&score);
                if(m){moved=1;for(int i=0;i<G48_SZ;i++) board[G48_SZ-1-i][col]=tmp[i];}
            }
        }

        if(moved) {
            if(score>best) best=score;
            g48_spawn(board);
        }
        g48_draw(board,score,best);



        for(int r=0;r<G48_SZ;r++) for(int cc=0;cc<G48_SZ;cc++) {
            if(board[r][cc]==2048) {
                hs_update(4, score);
                beep_ok();
                int ox=(VGA_WIDTH-(G48_SZ*G48_CW+G48_SZ+1))/2;
                term_move_cursor(ox+2,14);
                term_set_color(0x0A,0x00); term_print("You reached 2048! Congrats!");
                term_move_cursor(ox+2,15);
                term_set_color(0x08,0x00); term_print("Press any key to exit...");
                while(!get_monitor_char());
                draw_interface(); return;
            }
        }
        if(!g48_can_move(board)) {
            hs_update(4, score);
            beep_gameover();
            int ox=(VGA_WIDTH-(G48_SZ*G48_CW+G48_SZ+1))/2;
            term_move_cursor(ox+4,14);
            term_set_color(0x0C,0x00); term_print("GAME OVER! Score:"); term_print_int(score);
            if (score == hs_scores[4]) { term_set_color(0x0E,0x00); term_print(" \xdb BEST!"); }
            term_move_cursor(ox+4,15);
            term_set_color(0x08,0x00); term_print("Press any key...");
            while(!get_monitor_char());
            draw_interface(); return;
        }
    }
}
#undef G48_SZ
#undef G48_CW
#undef G48_CH


#define MS_W     9
#define MS_H     9
#define MS_MINES 10
#define MS_OX   29
#define MS_OY    4

static unsigned char ms_mine[MS_H][MS_W];
static unsigned char ms_rev[MS_H][MS_W];
static unsigned char ms_flag[MS_H][MS_W];
static int ms_adj[MS_H][MS_W];

static void ms_flood(int x, int y) {
    static int qx[81], qy[81];
    int qh=0, qt=0;
    if(ms_rev[y][x]||ms_mine[y][x]) return;
    ms_rev[y][x]=1; qx[qt]=x; qy[qt]=y; qt++;
    while(qh<qt) {
        int cx=qx[qh],cy=qy[qh]; qh++;
        if(ms_adj[cy][cx]==0) {
            for(int dr=-1;dr<=1;dr++) for(int dc=-1;dc<=1;dc++) {
                int nx=cx+dc,ny=cy+dr;
                if(nx>=0&&nx<MS_W&&ny>=0&&ny<MS_H&&!ms_rev[ny][nx]&&!ms_mine[ny][nx]) {
                    ms_rev[ny][nx]=1;
                    if(qt<81){qx[qt]=nx;qy[qt]=ny;qt++;}
                }
            }
        }
    }
}

static void ms_draw(int cx, int cy, int flags_left, int game_over, int won) {


    term_move_cursor(MS_OX-2, MS_OY-3);
    term_set_color(0x0B,0x00); term_print("MINESWEEPER");
    term_move_cursor(MS_OX+12, MS_OY-3);
    term_set_color(0x0C,0x00); term_print("\xf4 Mines:"); term_print_int(flags_left); term_print("  ");

    term_move_cursor(MS_OX-2, MS_OY-2);
    term_set_color(0x08,0x00);
    term_print("ZQSD/arrows=move  SPACE=reveal  F=flag  ESC=quit");



    term_set_color(0x07,0x00);
    term_move_cursor(MS_OX-1, MS_OY-1);
    term_putc('\xDA'); for(int i=0;i<MS_W*3+1;i++) term_putc('\xC4'); term_putc('\xBF');
    for(int r=0;r<MS_H;r++) {
        term_move_cursor(MS_OX-1, MS_OY+r);
        term_putc('\xB3');
        for(int c=0;c<MS_W;c++) {
            int sel=(c==cx&&r==cy);
            if(ms_rev[r][c]) {
                if(ms_mine[r][c]) {
                    if(sel) term_set_color(0x4F,0x00); else term_set_color(0x0C,0x00);
                    term_print(" * ");
                } else {
                    int n=ms_adj[r][c];
                    static const unsigned char nc[]={0x08,0x09,0x0A,0x0C,0x09,0x0C,0x0B,0x07,0x08};
                    term_set_color(n?nc[n]:0x08,0x00);
                    if(sel) term_set_color(0x70,0x00);
                    term_putc(' ');
                    if(n) { term_putc('0'+n); } else { term_putc('.'); }
                    term_putc(' ');
                }
            } else if(ms_flag[r][c]) {
                term_set_color(sel?0x4E:0x0E,0x00); term_print(" \xf4 ");
            } else {
                term_set_color(sel?0x70:0x07,0x00); term_print(" \xdb ");
            }
        }
        term_set_color(0x07,0x00); term_putc('\xB3'); term_putc('\n');
    }
    term_move_cursor(MS_OX-1, MS_OY+MS_H);
    term_putc('\xC0'); for(int i=0;i<MS_W*3+1;i++) term_putc('\xC4'); term_putc('\xD9');

    if(game_over) {
        term_move_cursor(MS_OX, MS_OY+MS_H+2);
        term_set_color(0x0C,0x00); term_print("BOOM! Game over. Press any key...");
    }
    if(won) {
        term_move_cursor(MS_OX, MS_OY+MS_H+2);
        term_set_color(0x0A,0x00); term_print("YOU WIN! All clear. Press any key...");
    }
}

void start_minesweeper(void) {
    term_init();
    gui_game_start();
    memset(ms_mine,0,sizeof(ms_mine));
    memset(ms_rev,0,sizeof(ms_rev));
    memset(ms_flag,0,sizeof(ms_flag));
    memset(ms_adj,0,sizeof(ms_adj));
    int cx=MS_W/2, cy=MS_H/2;
    int first=1, flags_left=MS_MINES;
    int game_over=0, won=0;

    ms_draw(cx,cy,flags_left,0,0);

    while(!game_over&&!won) {
        int c=0;
        while(!c){nop_delay(50000);c=get_monitor_char();}
        if(c==KEY_ESC){draw_interface();return;}

        if((c=='z'||c==KEY_UP)&&cy>0)    cy--;
        if((c=='s'||c==KEY_DOWN)&&cy<MS_H-1) cy++;
        if((c=='q'||c==KEY_LEFT)&&cx>0)  cx--;
        if((c=='d'||c==KEY_RIGHT)&&cx<MS_W-1) cx++;

        if(c==' '||c=='\n') {
            if(!ms_flag[cy][cx]&&!ms_rev[cy][cx]) {
                if(first) {
                    first=0;
                    int placed=0;
                    while(placed<MS_MINES) {
                        int mx=rand()%MS_W, my=rand()%MS_H;
                        if((mx==cx&&my==cy)||ms_mine[my][mx]) continue;
                        ms_mine[my][mx]=1; placed++;
                    }
                    for(int r=0;r<MS_H;r++) for(int cc=0;cc<MS_W;cc++) {
                        int cnt=0;
                        for(int dr=-1;dr<=1;dr++) for(int dc=-1;dc<=1;dc++) {
                            int nr=r+dr,nc=cc+dc;
                            if(nr>=0&&nr<MS_H&&nc>=0&&nc<MS_W&&ms_mine[nr][nc]) cnt++;
                        }
                        ms_adj[r][cc]=cnt;
                    }
                }
                if(ms_mine[cy][cx]) {
                    ms_rev[cy][cx]=1;
                    game_over=1;


                    for(int r=0;r<MS_H;r++) for(int cc=0;cc<MS_W;cc++)
                        if(ms_mine[r][cc]) ms_rev[r][cc]=1;
                } else {
                    ms_flood(cx,cy);
                }


                if(!game_over) {
                    int left=0;
                    for(int r=0;r<MS_H;r++) for(int cc=0;cc<MS_W;cc++)
                        if(!ms_mine[r][cc]&&!ms_rev[r][cc]) left++;
                    if(!left) won=1;
                }
            }
        }
        if(c=='f'||c=='F') {
            if(!ms_rev[cy][cx]) {
                ms_flag[cy][cx]=!ms_flag[cy][cx];
                flags_left+=(ms_flag[cy][cx]?-1:1);
            }
        }
        ms_draw(cx,cy,flags_left,game_over,won);
    }

    if (won) { hs_update(5, 1); beep_ok(); }
    else beep_gameover();
    while(!get_monitor_char());
    draw_interface();
}
#undef MS_W
#undef MS_H
#undef MS_MINES
#undef MS_OX
#undef MS_OY


#define SIMON_MAX 32
#define SIMON_BX1 1
#define SIMON_BX2 42
#define SIMON_BY1 2
#define SIMON_BY2 13
#define SIMON_BW  37
#define SIMON_BH  9

static const unsigned int  simon_freqs[4] = { 523, 659, 784, 1047 };
static const unsigned char simon_dim_fg[4] = { 0x04, 0x02, 0x06, 0x01 };
static const unsigned char simon_lit_fg[4] = { 0x0F, 0x0F, 0x00, 0x0F };
static const unsigned char simon_lit_bg[4] = { 0x04, 0x02, 0x06, 0x01 };
static const char* simon_lbl[4] = {
    "[ 1 ]   R O U G E",
    "[ 2 ]   V E R T",
    "[ 3 ]   J A U N E",
    "[ 4 ]   B L E U"
};

static void simon_btn(int b, int lit) {
    int bx = (b == 0 || b == 2) ? SIMON_BX1 : SIMON_BX2;
    int by = (b == 0 || b == 1) ? SIMON_BY1 : SIMON_BY2;
    unsigned char fg = lit ? simon_lit_fg[b] : simon_dim_fg[b];
    unsigned char bg = lit ? simon_lit_bg[b] : 0x00;
    ui_fill(bx, by, SIMON_BW, SIMON_BH, fg, bg);
    const char* lbl = simon_lbl[b];
    int llen = 0; while (lbl[llen]) llen++;
    term_set_color(fg, bg);
    term_move_cursor(bx + (SIMON_BW - llen) / 2, by + SIMON_BH / 2);
    term_print(lbl);
}

static void simon_flash(int b, int dur) {
    simon_btn(b, 1);
    timer_speaker_on(simon_freqs[b]);
    nop_delay(dur);
    timer_speaker_off();
    nop_delay(dur / 4);
    simon_btn(b, 0);
    nop_delay(40000000);
}

static void simon_hdr(int level, int score) {
    term_set_color(0x0F, 0x01);
    term_move_cursor(0, 0);
    for (int x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    term_move_cursor(2, 0);
    term_set_color(0x0B, 0x01); term_print("SIMON");
    term_set_color(0x07, 0x01); term_print("  |  Niveau: ");
    term_set_color(0x0E, 0x01); term_print_int(level);
    term_set_color(0x07, 0x01); term_print("  Score: ");
    term_set_color(0x0A, 0x01); term_print_int(score);
    term_set_color(0x08, 0x01);
    term_move_cursor(54, 0);
    term_print("[ 1 2 3 4 = jouer  ESC = quit ]");
}

static void simon_msg(const char* msg, unsigned char col) {
    term_set_color(0x00, 0x00);
    term_move_cursor(0, 23);
    for (int x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    term_set_color(col, 0x00);
    int mlen = 0; while (msg[mlen]) mlen++;
    term_move_cursor((VGA_WIDTH - mlen) / 2, 23);
    term_print(msg);
}

void start_simon(void) {
    term_init();
    gui_game_start();
    term_set_color(0x00, 0x00);
    for (int y = 0; y < VGA_HEIGHT; y++) {
        term_move_cursor(0, y);
        for (int x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    }

    int seq[SIMON_MAX];
    int level = 0, score = 0;
    unsigned int rng = (unsigned int)jiffies * 1664525u + 1013904223u;

    for (int b = 0; b < 4; b++) simon_btn(b, 0);

    for (int b = 0; b < 4; b++) simon_flash(b, 100000000);
    for (int b = 3; b >= 0; b--) simon_flash(b, 80000000);

    while (level < SIMON_MAX) {
        rng = rng * 1664525u + 1013904223u;
        seq[level++] = (int)((rng >> 16) & 3);

        simon_hdr(level, score);
        simon_msg("  Regardez la sequence...  ", 0x07);
        nop_delay(500000000);

        int fdur = level <= 5  ? 350000000 :
                   level <= 10 ? 260000000 :
                   level <= 15 ? 200000000 : 150000000;
        for (int i = 0; i < level; i++)
            simon_flash(seq[i], fdur);

        simon_msg("  A vous ! Repetez la sequence...  ", 0x0E);

        for (int i = 0; i < level; ) {
            int key; do { key = get_monitor_char(); nop_delay(300000); } while (!key);
            if (key == 0x1B) { hs_update(6, score); goto simon_done; }
            int p = (key == '1') ? 0 : (key == '2') ? 1 :
                    (key == '3') ? 2 : (key == '4') ? 3 : -1;
            if (p < 0) continue;
            simon_flash(p, 150000000);
            if (p != seq[i]) {
                simon_msg("  MAUVAISE REPONSE !  ", 0x0C);
                beep_fail(); nop_delay(150000000); beep_fail();
                hs_update(6, score);
                term_set_color(0x0C, 0x00);
                term_move_cursor(29, 10); term_print("  GAME  OVER  ");
                term_move_cursor(27, 11); term_print("  Niveau atteint: ");
                term_print_int(level - 1); term_print("  ");
                term_move_cursor(28, 12); term_print("  Score final: ");
                term_print_int(score); term_print("  ");
                if (score == hs_scores[6]) {
                    term_set_color(0x0E, 0x00);
                    term_move_cursor(31, 13); term_print("\xdb NEW BEST \xdb");
                }
                nop_delay(300000000); beep_gameover(); nop_delay(800000000);
                goto simon_done;
            }
            score++; i++;
        }
        simon_msg("  CORRECT !  ", 0x0A);
        beep_ok();
        nop_delay(400000000);
    }

    simon_msg("  FELICITATIONS ! Simon complete !  ", 0x0E);
    hs_update(6, score);
    for (int f = 0; f < 3; f++)
        for (int b = 0; b < 4; b++) simon_flash(b, 80000000);
    nop_delay(500000000);

simon_done:
    term_init();
    draw_interface();
}

#undef SIMON_MAX
#undef SIMON_BX1
#undef SIMON_BX2
#undef SIMON_BY1
#undef SIMON_BY2
#undef SIMON_BW
#undef SIMON_BH


#define ROUL_START   1000
#define ROUL_BX      4
#define ROUL_BW      72
#define ROUL_DRUM_Y  4

static const unsigned char rl_reds[] = {
    1,3,5,7,9,12,14,16,18,19,21,23,25,27,30,32,34,36
};

static int rl_color(int n) {
    if (!n) return 0;
    for (int i = 0; i < 18; i++) if (rl_reds[i] == (unsigned char)n) return 1;
    return 2;
}

static unsigned char rl_fg(int n) {
    int c = rl_color(n);
    return c == 0 ? 0x0A : c == 1 ? 0x0C : 0x0F;
}

static void rl_print_slot(int n, int highlight) {
    int c = rl_color(n);
    unsigned char fg = rl_fg(n);
    unsigned char bg = highlight ? (c==0 ? 0x02 : c==1 ? 0x04 : 0x08) : 0x00;
    term_set_color(fg, bg);
    if (n < 10) term_putc(' ');
    term_print_int(n);
    term_putc('\xFE');
    term_putc(' ');
    term_set_color(0x07, 0x00);
}

static void rl_draw_drum_nums(int* d, int final) {
    int inner = ROUL_BW - 2;
    int sx = ROUL_BX + 1 + (inner - 7 * 4) / 2;
    term_set_color(0x00, 0x00);
    term_move_cursor(ROUL_BX + 1, ROUL_DRUM_Y + 2);
    for (int x = 0; x < inner; x++) term_putc(' ');
    term_move_cursor(sx, ROUL_DRUM_Y + 2);
    for (int i = 0; i < 7; i++)
        rl_print_slot(d[i], final && i == 3);
    if (final) {
        int c = rl_color(d[3]);
        const char* cname = c == 0 ? "VERT" : c == 1 ? "ROUGE" : "NOIR";
        term_set_color(rl_fg(d[3]), 0x00);
        term_move_cursor(2, ROUL_DRUM_Y + 5);
        for (int x = 0; x < VGA_WIDTH - 4; x++) term_putc(' ');
        term_move_cursor(2, ROUL_DRUM_Y + 5);
        term_print("  Résultat : ");
        rl_print_slot(d[3], 1);
        term_set_color(rl_fg(d[3]), 0x00);
        term_print("  "); term_print(cname); term_print("  ");
        term_set_color(0x07, 0x00);
    }
}

static void rl_draw_box(void) {
    int bx = ROUL_BX, bw = ROUL_BW, by = ROUL_DRUM_Y;
    term_set_color(0x07, 0x00);
    term_move_cursor(bx, by);
    term_putc('\xda');
    for (int x = 0; x < bw-2; x++) term_putc('\xc4');
    term_putc('\xbf');
    for (int r = 1; r <= 4; r++) {
        term_move_cursor(bx, by + r); term_putc('\xb3');
        term_set_color(0x00, 0x00);
        for (int x = 0; x < bw-2; x++) term_putc(' ');
        term_set_color(0x07, 0x00);
        term_move_cursor(bx + bw - 1, by + r); term_putc('\xb3');
    }
    term_move_cursor(bx, by + 5);
    term_set_color(0x07, 0x00);
    term_putc('\xc0');
    for (int x = 0; x < bw-2; x++) term_putc('\xc4');
    term_putc('\xd9');

    term_set_color(0x0B, 0x00);
    const char* title = " ROUE DE LA FORTUNE ";
    int tlen = 0; while (title[tlen]) tlen++;
    term_move_cursor(bx + (bw - tlen) / 2, by);
    term_print(title);
}

static void rl_hdr(int bal, int best, int tour) {
    term_set_color(0x0F, 0x01);
    term_move_cursor(0, 0);
    for (int x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    term_set_color(0x0E, 0x01); term_move_cursor(2, 0);  term_print("CASINO GREGOS");
    term_set_color(0x0F, 0x01); term_print("  \xB3  Solde: ");
    term_set_color(bal < 200 ? 0x0C : 0x0A, 0x01); term_print_int(bal); term_print(" GC");
    term_set_color(0x0F, 0x01); term_print("  \xB3  Meilleur: ");
    term_set_color(0x0E, 0x01); term_print_int(best); term_print(" GC");
    term_set_color(0x0F, 0x01); term_print("  \xB3  Tour: ");
    term_set_color(0x0B, 0x01); term_print_int(tour);
    term_set_color(0x08, 0x01); term_move_cursor(68, 0); term_print("ESC=quitter");
}


static void rl_msg(const char* s, unsigned char col, int row) {
    term_set_color(0x00, 0x00);
    term_move_cursor(0, row);
    for (int x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    term_set_color(col, 0x00);
    int len = 0; while (s[len]) len++;
    term_move_cursor((VGA_WIDTH - len) / 2, row);
    term_print(s);
}

static int rl_read_mise(int max) {
    int n = 0, digits = 0;
    term_set_color(0x0F, 0x00);
    term_move_cursor(2, 17); term_print("Votre mise: ");
    term_set_color(0x0E, 0x00);
    while (1) {
        int k = get_monitor_char(); nop_delay(200000);
        if (!k) continue;
        if (k == 0x1B) return -1;
        if ((k == '\n' || k == '\r') && digits > 0) return n > max ? max : n < 1 ? 1 : n;
        if (k == '\b' && digits > 0) {
            digits--; n /= 10;
            term_putc('\b'); term_putc(' '); term_putc('\b');
        } else if (k >= '0' && k <= '9' && digits < 5) {
            int nn = n * 10 + (k - '0');
            if (nn <= max) { n = nn; digits++; term_putc((char)k); }
        }
    }
}

static void rl_draw_bet_menu(void) {
    int x;
    term_set_color(0x00, 0x00);
    for (int y = 12; y <= 16; y++) {
        term_move_cursor(0, y);
        for (x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    }
    term_set_color(0x0B, 0x00);
    term_move_cursor(2, 12); term_print("TYPE DE PARI :");
    term_set_color(0x0E, 0x00);
    term_move_cursor(2, 13);
    term_print("[R]ouge x1  [N]oir x1  [P]air x1  [I]mpair x1");
    term_move_cursor(2, 14);
    term_print("[M]anque(1-18) x1  p[A]sse(19-36) x1");
    term_move_cursor(2, 15);
    term_print("[D]ouzaine x2  [X] Numero exact x35  [ESC] quitter");
}

static int rl_read_bet(int* param) {
    int x, k, num, d, nv;
    char c;
    *param = 0;
    rl_draw_bet_menu();
    while (1) {
        do { k = get_monitor_char(); nop_delay(200000); } while (!k);
        if (k == 0x1B) return -1;
        c = (k >= 'A' && k <= 'Z') ? (char)(k + 32) : (char)k;
        if (c == 'r') { rl_msg("  ROUGE (x1)  ", 0x0C, 21); return 0; }
        if (c == 'n') { rl_msg("  NOIR (x1)  ",  0x0F, 21); return 1; }
        if (c == 'p') { rl_msg("  PAIR (x1)  ",  0x0B, 21); return 2; }
        if (c == 'i') { rl_msg("  IMPAIR (x1)  ",0x0B, 21); return 3; }
        if (c == 'm') { rl_msg("  MANQUE 1-18 (x1)  ", 0x0E, 21); return 4; }
        if (c == 'a') { rl_msg("  PASSE 19-36 (x1)  ", 0x0E, 21); return 5; }
        if (c == 'd') {
            rl_msg("  Douzaine ? [1]=1-12  [2]=13-24  [3]=25-36  ", 0x0E, 21);
            while (1) {
                do { k = get_monitor_char(); nop_delay(200000); } while (!k);
                if (k == 0x1B) return -1;
                if (k >= '1' && k <= '3') {
                    *param = k - '0';
                    rl_msg("  DOUZAINE (x2)  ", 0x0B, 21);
                    return 6;
                }
            }
        }
        if (c == 'x') {
            rl_msg("  Numero (0-36) puis ENTER :  ", 0x0E, 21);
            term_set_color(0x00, 0x00);
            term_move_cursor(0, 22); for (x = 0; x < VGA_WIDTH; x++) term_putc(' ');
            term_set_color(0x0E, 0x00);
            term_move_cursor(2, 22); term_print("> ");
            num = 0; d = 0;
            while (1) {
                do { k = get_monitor_char(); nop_delay(200000); } while (!k);
                if (k == 0x1B) { return -1; }
                if ((k == '\n' || k == '\r') && d > 0) {
                    *param = num;
                    rl_msg("  PLEIN (x35)  ", 0x0E, 21);
                    term_set_color(0x00, 0x00);
                    term_move_cursor(0, 22); for (x = 0; x < VGA_WIDTH; x++) term_putc(' ');
                    return 7;
                }
                if ((k == '\b' || k == 127) && d > 0) {
                    d--; num /= 10;
                    term_move_cursor(4, 22); term_print("   ");
                    term_move_cursor(4, 22); if (num > 0 || d > 0) term_print_int(num);
                } else if (k >= '0' && k <= '9' && d < 2) {
                    nv = num * 10 + (k - '0');
                    if (nv <= 36) { num = nv; d++; term_putc((char)k); }
                }
            }
        }
    }
}

static void rl_spin(int result, unsigned int* prng) {
    int drum[7];
    for (int i = 0; i < 7; i++) {
        *prng = (*prng) * 1664525u + 1013904223u;
        drum[i] = (int)((*prng >> 16) % 37);
    }
    int steps = 70;
    for (int s = 0; s < steps; s++) {
        for (int i = 0; i < 6; i++) drum[i] = drum[i+1];
        if (s == steps - 4)
            drum[6] = result;
        else {
            *prng = (*prng) * 1664525u + 1013904223u;
            drum[6] = (int)((*prng >> 16) % 37);
        }
        rl_draw_drum_nums(drum, s == steps - 1);
        int delay = s < 25 ? 12000000 :
                    s < 45 ? 30000000 :
                    s < 57 ? 65000000 :
                    s < 64 ? 130000000 : 220000000;
        nop_delay(delay);
    }
}

static int rl_wins(int result, int type, int param) {
    switch (type) {
        case 0: return result && rl_color(result) == 1;
        case 1: return result && rl_color(result) == 2;
        case 2: return result && (result % 2 == 0);
        case 3: return result && (result % 2 == 1);
        case 4: return result >= 1 && result <= 18;
        case 5: return result >= 19 && result <= 36;
        case 6: return result && (result - 1) / 12 + 1 == param;
        case 7: return result == param;
    }
    return 0;
}

static int rl_mult(int type) {
    if (type <= 5) return 1;
    if (type == 6) return 2;
    return 35;
}

void start_roulette(void) {
    term_init();
    gui_game_start();
    term_set_color(0x00, 0x00);
    for (int y = 0; y < VGA_HEIGHT; y++) {
        term_move_cursor(0, y);
        for (int x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    }
    rl_draw_box();

    int balance = casino_balance, best = casino_best, tour = 0;
    unsigned int rng = (unsigned int)jiffies * 2654435761u;
    rl_hdr(balance, best, tour);
    rl_draw_bet_menu();
    rl_msg("  Bienvenue au Casino GregOS !  Bonne chance !  ", 0x0E, 21);

    while (balance > 0) {
        term_set_color(0x00, 0x00);
        term_move_cursor(0, 17); for (int x = 0; x < VGA_WIDTH; x++) term_putc(' ');

        int mise = rl_read_mise(balance);
        if (mise < 0) goto roul_done;

        int param = 0;
        int btype = rl_read_bet(&param);
        if (btype < 0) goto roul_done;

        rl_msg("  La roue tourne...  ", 0x07, 21);
        nop_delay(300000000);

        rng = rng * 1664525u + 1013904223u;
        int result = (int)((rng >> 16) % 37);

        rl_spin(result, &rng);
        tour++;

        int won = rl_wins(result, btype, param);
        int gain = won ? mise * rl_mult(btype) : -mise;
        balance += gain;
        if (balance > best) best = balance;
        if (balance < 0) balance = 0;
        hs_update(7, best);
        rl_hdr(balance, best, tour);

        if (won) {
            beep_ok();
            term_set_color(0x0A, 0x00);
            term_move_cursor(2, 21);
            term_print("  \xDB GAGNE ! +");
            term_print_int(mise * rl_mult(btype));
            term_print(" GC  \xDB  Solde: ");
            term_print_int(balance);
            term_print(" GC  ");
            if (rl_mult(btype) == 35) {
                term_set_color(0x0E, 0x00); term_print("  \xDB PLEIN ! \xDB");
                casino_win_anim(1);
            } else {
                casino_win_anim(0);
            }
        } else {
            beep_fail();
            term_set_color(0x0C, 0x00);
            term_move_cursor(2, 21);
            term_print("  PERDU. -");
            term_print_int(mise);
            term_print(" GC  \xB3  Solde: ");
            term_print_int(balance);
            term_print(" GC  ");
        }
        term_set_color(0x07, 0x00);
        term_move_cursor(2, 22);
        term_print("ENTER = nouveau tour  |  ESC = quitter");
        int k;
        do { k = get_monitor_char(); nop_delay(300000); } while (!k);
        if (k == 0x1B) goto roul_done;

        if (balance == 0) {
            beep_gameover();
            rl_msg("  BANQUEROUTE ! Vous n'avez plus de GregCoins.  ", 0x0C, 21);
            term_set_color(0x07, 0x00);
            term_move_cursor(20, 22);
            term_print("Meilleur solde : "); term_print_int(best); term_print(" GC");
            nop_delay(300000000);
            do { k = get_monitor_char(); nop_delay(300000); } while (!k);
            goto roul_done;
        }
        term_set_color(0x00, 0x00);
        term_move_cursor(0, 21); for (int x = 0; x < VGA_WIDTH; x++) term_putc(' ');
        term_move_cursor(0, 22); for (int x = 0; x < VGA_WIDTH; x++) term_putc(' ');
        term_move_cursor(0, ROUL_DRUM_Y + 5);
        for (int x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    }

roul_done:
    casino_balance = balance;
    if (best > casino_best) casino_best = best;
    hs_update(7, casino_best);
    term_init();
    draw_interface();
}

#undef ROUL_START
#undef ROUL_BX
#undef ROUL_BW
#undef ROUL_DRUM_Y


#define BJ_START  1000
#define BJ_MAX_H  12
#define BJ_CW     5
#define BJ_CG     1
#define BJ_DY     2
#define BJ_PY     7

static int bj_deck[52];
static int bj_dpos;

static void bj_shuffle(unsigned int* r) {
    int i, j, t;
    for (i = 0; i < 52; i++) bj_deck[i] = i;
    for (i = 51; i > 0; i--) {
        *r = *r * 1664525u + 1013904223u;
        j = (int)((*r >> 16) % (unsigned int)(i + 1));
        t = bj_deck[i]; bj_deck[i] = bj_deck[j]; bj_deck[j] = t;
    }
    bj_dpos = 0;
}

static int bj_deal_card(void) {
    if (bj_dpos >= 52) bj_dpos = 0;
    return bj_deck[bj_dpos++];
}

static int bj_hand_val(int* h, int n) {
    int i, v = 0, a = 0, r;
    for (i = 0; i < n; i++) {
        r = h[i] % 13;
        if (r == 0) { v += 11; a++; }
        else if (r >= 9) v += 10;
        else v += r + 1;
    }
    while (v > 21 && a > 0) { v -= 10; a--; }
    return v;
}

static void bj_draw_card(int x, int y, int card, int hidden) {
    static const char* bj_rn[13] = {"A","2","3","4","5","6","7","8","9","10","J","Q","K"};
    static const char  bj_sc[4]  = {'\x03','\x04','\x05','\x06'};
    int suit, rank;
    unsigned char fg;
    const char* rn;
    if (hidden) {
        term_set_color(0x0F, 0x01);
        term_move_cursor(x, y);   term_print("\xda\xc4\xc4\xc4\xbf");
        term_set_color(0x09, 0x01);
        term_move_cursor(x, y+1); term_print("\xb3\xB0\xB0\xB0\xb3");
        term_set_color(0x0F, 0x01);
        term_move_cursor(x, y+2); term_print("\xc0\xc4\xc4\xc4\xd9");
        return;
    }
    suit = card / 13;
    rank = card % 13;
    fg   = (suit < 2) ? 0x0C : 0x0F;
    rn   = bj_rn[rank];
    term_set_color(0x07, 0x00);
    term_move_cursor(x, y);   term_print("\xda\xc4\xc4\xc4\xbf");
    term_move_cursor(x, y+2); term_print("\xc0\xc4\xc4\xc4\xd9");
    term_move_cursor(x, y+1); term_putc('\xb3');
    term_set_color(fg, 0x00);
    term_putc(rn[0]);
    term_putc(rn[1] ? rn[1] : ' ');
    term_putc(bj_sc[suit]);
    term_set_color(0x07, 0x00);
    term_putc('\xb3');
}

static void bj_draw_hand(int* h, int n, int y, int hide_first) {
    int i;
    for (i = 0; i < n; i++)
        bj_draw_card(2 + i * (BJ_CW + BJ_CG), y, h[i], i == 0 && hide_first);
}

static void bj_hdr(int bal, int best, int hands) {
    int x;
    term_set_color(0x0F, 0x01);
    term_move_cursor(0, 0);
    for (x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    term_move_cursor(1, 0);
    term_print("\x06 BLACKJACK \x06  Balance: ");
    term_print_int(bal);
    term_print(" GC  |  Record: ");
    term_print_int(best);
    term_print(" GC  |  Mains: ");
    term_print_int(hands);
    term_set_color(0x07, 0x00);
}

static void bj_separator(int y, const char* lbl, int score, int show) {
    int x;
    term_set_color(0x0E, 0x00);
    term_move_cursor(0, y);
    for (x = 0; x < VGA_WIDTH; x++) term_putc('\xc4');
    term_move_cursor(2, y);
    term_set_color(0x0F, 0x00);
    term_putc(' '); term_print(lbl);
    if (show) {
        term_print(": ");
        term_print_int(score);
        if      (score > 21) { term_set_color(0x0C, 0x00); term_print(" BUST"); }
        else if (score == 21){ term_set_color(0x0E, 0x00); term_print(" \xDB"); }
    }
    term_putc(' ');
    term_set_color(0x07, 0x00);
}

static void bj_msg(const char* s, unsigned char col, int y) {
    int x, len = 0;
    while (s[len]) len++;
    term_set_color(0x00, 0x00);
    term_move_cursor(0, y);
    for (x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    term_set_color(col, 0x00);
    term_move_cursor((VGA_WIDTH - len) / 2, y);
    term_print(s);
    term_set_color(0x07, 0x00);
}

static void bj_clear_rows(int y1, int y2) {
    int x, y;
    term_set_color(0x00, 0x00);
    for (y = y1; y <= y2; y++) {
        term_move_cursor(0, y);
        for (x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    }
}

static int bj_read_mise(int max) {
    int mise = 0, k, nv;
    bj_clear_rows(16, VGA_HEIGHT - 1);
    term_set_color(0x07, 0x00);
    term_move_cursor(2, 17);
    term_print("Votre mise (1-");
    term_print_int(max);
    term_print(" GC)  ENTER = confirmer  ESC = quitter");
    term_move_cursor(2, 19);
    term_print("> ");
    for (;;) {
        do { k = get_monitor_char(); nop_delay(300000); } while (!k);
        if (k == 0x1B) return -1;
        if ((k == '\r' || k == '\n') && mise >= 1) return mise;
        if ((k == '\b' || k == 127) && mise > 0) {
            mise /= 10;
            term_move_cursor(2, 19); term_print(">        ");
            term_move_cursor(2, 19); term_print("> ");
            if (mise > 0) term_print_int(mise);
        } else if (k >= '0' && k <= '9') {
            nv = mise * 10 + (k - '0');
            if (nv <= max && nv <= 9999) {
                mise = nv;
                term_move_cursor(2, 19); term_print(">        ");
                term_move_cursor(2, 19); term_print("> ");
                term_print_int(mise);
            }
        }
    }
}

void start_blackjack(void) {
    int player[BJ_MAX_H], dealer[BJ_MAX_H];
    int pc, dc, pval, dval, balance, best, hands, mise, can_double, result, k, x, y;
    unsigned int rng;

    term_init();
    gui_game_start();
    term_set_color(0x00, 0x00);
    for (y = 0; y < VGA_HEIGHT; y++) {
        term_move_cursor(0, y);
        for (x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    }

    balance = casino_balance; best = casino_best; hands = 0;
    rng = (unsigned int)jiffies * 2654435761u;
    bj_shuffle(&rng);

    bj_hdr(balance, best, hands);
    bj_msg("  \x06 Bienvenue au Blackjack GregOS !  Bonne chance ! \x06  ", 0x0E, 12);

    while (balance > 0) {
        bj_clear_rows(1, VGA_HEIGHT - 1);
        bj_hdr(balance, best, hands);

        if (bj_dpos > 40) bj_shuffle(&rng);

        mise = bj_read_mise(balance);
        if (mise < 0) goto bj_done;

        hands++;
        bj_hdr(balance, best, hands);
        bj_clear_rows(16, VGA_HEIGHT - 1);

        pc = 0; dc = 0;
        player[pc++] = bj_deal_card();
        dealer[dc++] = bj_deal_card();
        player[pc++] = bj_deal_card();
        dealer[dc++] = bj_deal_card();

        pval = bj_hand_val(player, pc);
        dval = bj_hand_val(dealer, dc);

        bj_separator(1, "CROUPIER", 0, 0);
        bj_clear_rows(BJ_DY, BJ_DY + 2);
        bj_draw_hand(dealer, dc, BJ_DY, 1);

        bj_separator(6, "JOUEUR", pval, 1);
        bj_clear_rows(BJ_PY, BJ_PY + 2);
        bj_draw_hand(player, pc, BJ_PY, 0);

        if (pval == 21) {
            bj_clear_rows(BJ_DY, BJ_DY + 2);
            bj_draw_hand(dealer, dc, BJ_DY, 0);
            bj_separator(1, "CROUPIER", dval, 1);
            if (dval == 21) {
                bj_msg("  BLACKJACK EGAL ! Push - mise remboursee.  ", 0x0E, 11);
                beep_ok();
            } else {
                int gain = (mise * 3) / 2;
                balance += gain;
                if (balance > best) best = balance;
                hs_update(8, best);
                bj_hdr(balance, best, hands);
                bj_msg("  \xDB\xDB BLACKJACK ! Payement 3:2 \xDB\xDB  ", 0x0E, 11);
                term_set_color(0x0E, 0x00);
                term_move_cursor(2, 12);
                term_print("+ "); term_print_int(gain); term_print(" GC");
                beep_ok();
                casino_win_anim(1);
            }
            goto bj_next;
        }

        can_double = (pc == 2 && mise * 2 <= balance);

        for (;;) {
            bj_clear_rows(11, 15);
            term_set_color(0x07, 0x00);
            term_move_cursor(2, 11);
            term_print("[H] Tirer  [S] Rester");
            if (can_double) term_print("  [D] Doubler");
            term_print("  [ESC] Quitter");

            do { k = get_monitor_char(); nop_delay(300000); } while (!k);
            if (k == 0x1B) goto bj_done;

            if (k == 'h' || k == 'H') {
                player[pc++] = bj_deal_card();
                pval = bj_hand_val(player, pc);
                bj_clear_rows(BJ_PY, BJ_PY + 2);
                bj_draw_hand(player, pc, BJ_PY, 0);
                bj_separator(6, "JOUEUR", pval, 1);
                can_double = 0;
                if (pval >= 21) break;
            } else if (k == 's' || k == 'S') {
                break;
            } else if ((k == 'd' || k == 'D') && can_double) {
                mise *= 2;
                player[pc++] = bj_deal_card();
                pval = bj_hand_val(player, pc);
                bj_clear_rows(BJ_PY, BJ_PY + 2);
                bj_draw_hand(player, pc, BJ_PY, 0);
                bj_separator(6, "JOUEUR", pval, 1);
                bj_msg("  Double ! Mise doublee.  ", 0x0E, 13);
                break;
            }
        }

        if (pval > 21) {
            bj_clear_rows(BJ_DY, BJ_DY + 2);
            bj_draw_hand(dealer, dc, BJ_DY, 0);
            dval = bj_hand_val(dealer, dc);
            bj_separator(1, "CROUPIER", dval, 1);
            balance -= mise;
            if (balance < 0) balance = 0;
            bj_hdr(balance, best, hands);
            hs_update(8, best);
            beep_fail();
            bj_msg("  BUST ! Vous depassez 21.  ", 0x0C, 11);
            term_set_color(0x0C, 0x00);
            term_move_cursor(2, 12);
            term_print("- "); term_print_int(mise); term_print(" GC");
            goto bj_next;
        }

        bj_clear_rows(BJ_DY, BJ_DY + 2);
        bj_draw_hand(dealer, dc, BJ_DY, 0);
        dval = bj_hand_val(dealer, dc);
        bj_separator(1, "CROUPIER", dval, 1);
        nop_delay(400000000);

        while (dval < 17) {
            dealer[dc++] = bj_deal_card();
            dval = bj_hand_val(dealer, dc);
            bj_clear_rows(BJ_DY, BJ_DY + 2);
            bj_draw_hand(dealer, dc, BJ_DY, 0);
            bj_separator(1, "CROUPIER", dval, 1);
            nop_delay(400000000);
        }

        if      (dval > 21 || pval > dval) result = 1;
        else if (pval < dval)              result = 0;
        else                               result = 2;

        if (result == 1) {
            balance += mise;
            if (balance > best) best = balance;
            hs_update(8, best);
            bj_hdr(balance, best, hands);
            beep_ok();
            bj_msg("  \xDB GAGNE !  ", 0x0A, 11);
            term_set_color(0x0A, 0x00);
            term_move_cursor(2, 12);
            term_print("+ "); term_print_int(mise); term_print(" GC");
            casino_win_anim(0);
        } else if (result == 0) {
            balance -= mise;
            if (balance < 0) balance = 0;
            bj_hdr(balance, best, hands);
            beep_fail();
            bj_msg("  PERDU.  ", 0x0C, 11);
            term_set_color(0x0C, 0x00);
            term_move_cursor(2, 12);
            term_print("- "); term_print_int(mise); term_print(" GC");
        } else {
            bj_msg("  EGALITE ! Mise remboursee.  ", 0x0E, 11);
        }
        hs_update(8, best);

        bj_next:
        bj_clear_rows(16, VGA_HEIGHT - 1);
        if (balance == 0) {
            beep_gameover();
            bj_msg("  BANQUEROUTE ! Plus de GregCoins.  ", 0x0C, 17);
            term_set_color(0x07, 0x00);
            term_move_cursor(20, 18);
            term_print("Record: "); term_print_int(best); term_print(" GC");
            nop_delay(300000000);
            do { k = get_monitor_char(); nop_delay(300000); } while (!k);
            goto bj_done;
        }
        term_set_color(0x07, 0x00);
        term_move_cursor(2, 17);
        term_print("ENTER = nouvelle main  |  ESC = quitter");
        do { k = get_monitor_char(); nop_delay(300000); } while (!k);
        if (k == 0x1B) goto bj_done;
    }

    bj_done:
    casino_balance = balance;
    if (best > casino_best) casino_best = best;
    hs_update(8, casino_best);
    term_init();
    draw_interface();
}

#undef BJ_START
#undef BJ_MAX_H
#undef BJ_CW
#undef BJ_CG
#undef BJ_DY
#undef BJ_PY


#define SL_NSYMS   7
#define SL_REELS   3
#define SL_STRIP   24
#define SL_WIN_Y   7
#define SL_RX1     20
#define SL_RX2     34
#define SL_RX3     48
#define SL_RW      10
#define SL_START   1000

static const char* sl_str[SL_NSYMS]        = {"GRG","777"," \x03 "," \x04 "," \x06 ","BAR"," \x05 "};
static const unsigned char sl_col[SL_NSYMS] = {0x0E, 0x0E, 0x0C,   0x0D,   0x0F,   0x0A,  0x0B};
static const unsigned char sl_wt[SL_NSYMS]  = {1,    3,    5,      5,      5,      6,     5};
static const int sl_pay3[SL_NSYMS]          = {100,  40,   15,     12,     8,      5,     10};

static int sl_rnd_sym(unsigned int* r) {
    *r = *r * 1664525u + 1013904223u;
    unsigned int v = (*r >> 16) % 30u;
    unsigned int acc = 0;
    for (int i = 0; i < SL_NSYMS; i++) {
        acc += sl_wt[i];
        if (v < acc) return i;
    }
    return SL_NSYMS - 1;
}

static void sl_draw_reel(int rx, int y, int sym, int bright) {
    unsigned char fg = bright ? sl_col[sym] : 0x08;
    unsigned char bg = bright ? 0x00 : 0x00;
    term_set_color(bright ? 0x07 : 0x08, 0x00);
    term_move_cursor(rx, y);     term_print("\xda\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xbf");
    term_move_cursor(rx, y+2);   term_print("\xc0\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xc4\xd9");
    term_move_cursor(rx, y+1);   term_putc('\xb3');
    term_set_color(fg, bg);
    term_print("   ");
    term_print(sl_str[sym]);
    term_print("   ");
    term_set_color(bright ? 0x07 : 0x08, 0x00);
    term_putc('\xb3');
}


static void sl_hdr(int bal, int best) {
    int x;
    term_set_color(0x0F, 0x01);
    term_move_cursor(0, 0);
    for (x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    term_move_cursor(1, 0);
    term_print("\x0F MACHINE A SOUS \x0F  Balance: ");
    term_print_int(bal);
    term_print(" GC  |  Record: ");
    term_print_int(best);
    term_print(" GC");
    term_set_color(0x07, 0x00);
}

static void sl_msg(const char* s, unsigned char col, int y) {
    int x, len = 0;
    while (s[len]) len++;
    term_set_color(0x00, 0x00);
    term_move_cursor(0, y);
    for (x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    term_set_color(col, 0x00);
    term_move_cursor((VGA_WIDTH - len) / 2, y);
    term_print(s);
    term_set_color(0x07, 0x00);
}

static void sl_draw_static(void) {
    int x;
    term_set_color(0x0B, 0x00);
    term_move_cursor(0, 2);
    for (x = 0; x < VGA_WIDTH; x++) term_putc('\xc4');
    int tlen = 0; const char* t = " \x0F MACHINE A SOUS \x0F ";
    while (t[tlen]) tlen++;
    term_move_cursor((VGA_WIDTH - tlen) / 2, 2);
    term_set_color(0x0F, 0x00);
    term_print(t);

    term_set_color(0x08, 0x00);
    term_move_cursor(SL_RX1 - 3, SL_WIN_Y + 1);
    term_print("\xAF\xAF");
    term_move_cursor(SL_RX1 + SL_RW * SL_REELS + (SL_REELS - 1) * 2 - SL_RW + 1, SL_WIN_Y + 1);

    term_set_color(0x07, 0x00);
    term_move_cursor(2, 12);
    for (x = 0; x < VGA_WIDTH - 4; x++) term_putc('\xC4');
    term_move_cursor(2, 12); term_set_color(0x0B, 0x00); term_print(" GAINS ");
    term_set_color(0x08, 0x00);
    term_move_cursor(2, 13);
    term_print("GRG\xd7""3=JACKPOT 100x  777\xd7""3=40x  \x03\x03\x03=15x  \x04\x04\x04=12x");
    term_move_cursor(2, 14);
    term_print("\x05\x05\x05=10x  \x06\x06\x06=8x  BAR\xd7""3=5x  2 pareils=1x (remboursé)");
}

static void sl_spin_anim(int* result, unsigned int* rng) {
    int rx[3] = {SL_RX1, SL_RX2, SL_RX3};
    int stops[3] = {80, 100, 120};
    int cur[3]   = {0, 0, 0};
    int stopped[3] = {0, 0, 0};

    for (int s = 0; s < 120; s++) {
        for (int r = 0; r < 3; r++) {
            if (stopped[r]) continue;
            if (s >= stops[r]) {
                stopped[r] = 1;
                cur[r] = result[r];
                sl_draw_reel(rx[r], SL_WIN_Y, result[r], 1);
            } else {
                cur[r] = sl_rnd_sym(rng);
                sl_draw_reel(rx[r], SL_WIN_Y, cur[r], 0);
            }
        }
        int delay = s < 40  ? 8000000 :
                    s < 70  ? 20000000 :
                    s < 95  ? 55000000 :
                    s < 110 ? 120000000 : 200000000;
        nop_delay(delay);
    }
}

void start_slots(void) {
    int x, y, mise, k, result[3], sym0, sym1, sym2;
    unsigned int rng;

    term_init();
    gui_game_start();
    term_set_color(0x00, 0x00);
    for (y = 0; y < VGA_HEIGHT; y++) {
        term_move_cursor(0, y);
        for (x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    }

    int balance = casino_balance, best = casino_best;
    rng = (unsigned int)jiffies * 2654435761u ^ 0xDEADBEEFu;
    mise = 10;

    sl_hdr(balance, best);
    sl_draw_static();

    result[0] = result[1] = result[2] = 0;
    sl_draw_reel(SL_RX1, SL_WIN_Y, 0, 1);
    sl_draw_reel(SL_RX2, SL_WIN_Y, 0, 1);
    sl_draw_reel(SL_RX3, SL_WIN_Y, 0, 1);

    sl_msg("  Bienvenue !  [ESPACE] Tourner  [+/-] Mise  [ESC] Quitter  ", 0x0E, 17);

    while (balance > 0) {
        term_set_color(0x07, 0x00);
        term_move_cursor(2, 16);
        term_print("Mise: ");
        term_set_color(0x0E, 0x00);
        term_print_int(mise);
        term_set_color(0x07, 0x00);
        term_print(" GC  [+] augmenter  [-] diminuer  [ESPACE] lancer  [ESC] quitter");

        do { k = get_monitor_char(); nop_delay(300000); } while (!k);
        if (k == 0x1B) goto sl_done;

        if (k == '+' || k == '=') {
            int nm = mise + (mise < 10 ? 1 : mise < 50 ? 5 : 10);
            if (nm > balance) nm = balance;
            if (nm > 500) nm = 500;
            mise = nm;
            continue;
        }
        if (k == '-') {
            int nm = mise - (mise <= 10 ? 1 : mise <= 50 ? 5 : 10);
            if (nm < 1) nm = 1;
            mise = nm;
            continue;
        }
        if (k != ' ') continue;

        if (mise > balance) mise = balance;

        sl_msg("  Bonne chance !  ", 0x07, 17);

        result[0] = sl_rnd_sym(&rng);
        result[1] = sl_rnd_sym(&rng);
        result[2] = sl_rnd_sym(&rng);

        sl_spin_anim(result, &rng);

        sym0 = result[0]; sym1 = result[1]; sym2 = result[2];

        int gain = 0;
        if (sym0 == sym1 && sym1 == sym2) {
            gain = mise * sl_pay3[sym0];
            if (sl_pay3[sym0] >= 100) {
                beep_ok(); nop_delay(100000000); beep_ok(); nop_delay(100000000); beep_ok();
                sl_msg("  \xDB\xDB\xDB JACKPOT !!!! \xDB\xDB\xDB  ", 0x0E, 17);
                casino_win_anim(1);
            } else {
                beep_ok();
                sl_msg("  \xDB GAGNE !  ", 0x0A, 17);
                casino_win_anim(0);
            }
        } else if (sym0 == sym1 || sym1 == sym2 || sym0 == sym2) {
            gain = mise;
            sl_msg("  2 pareils - mise remboursee  ", 0x0B, 17);
        } else {
            gain = -mise;
            beep_fail();
            sl_msg("  Perdu.  ", 0x0C, 17);
        }

        balance += gain;
        if (balance < 0) balance = 0;
        if (balance > best) best = balance;
        casino_balance = balance;
        if (best > casino_best) casino_best = best;
        hs_update(9, casino_best);
        sl_hdr(balance, best);

        term_set_color(gain > 0 ? 0x0A : gain < 0 ? 0x0C : 0x0B, 0x00);
        term_move_cursor(2, 18);
        if (gain > 0) { term_print("+ "); term_print_int(gain); }
        else if (gain < 0) { term_print("- "); term_print_int(-gain); }
        else { term_print("= "); term_print_int(gain); }
        term_print(" GC  ->  Solde: ");
        term_print_int(balance);
        term_print(" GC   ");
        term_set_color(0x07, 0x00);

        if (balance == 0) {
            beep_gameover();
            sl_msg("  BANQUEROUTE ! Tapez 'atm' dans le shell pour recharger.  ", 0x0C, 20);
            nop_delay(300000000);
            do { k = get_monitor_char(); nop_delay(300000); } while (!k);
            goto sl_done;
        }
        if (mise > balance) mise = balance;
    }

    sl_done:
    casino_balance = balance;
    if (best > casino_best) casino_best = best;
    hs_update(9, casino_best);
    term_init();
    draw_interface();
}

#undef SL_NSYMS
#undef SL_REELS
#undef SL_STRIP
#undef SL_WIN_Y
#undef SL_RX1
#undef SL_RX2
#undef SL_RX3
#undef SL_RW
#undef SL_START


void cmd_distributeur(void) {
    int x, k, amount, d, nv;
    term_set_color(0x0F, 0x01);
    term_move_cursor(0, 0);
    for (x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    term_move_cursor(2, 0);
    term_print("\xFE DISTRIBUTEUR DE GREGCOINS \xFE  Solde actuel: ");
    term_set_color(casino_balance < 100 ? 0x0C : 0x0A, 0x01);
    term_print_int(casino_balance);
    term_print(" GC");
    term_set_color(0x07, 0x00);

    term_move_cursor(2, 3);
    term_print("Retrait (max 1000 GC, ENTER=confirmer, ESC=annuler):");
    term_move_cursor(2, 5);
    term_print("> ");
    amount = 0; d = 0;
    for (;;) {
        do { k = get_monitor_char(); nop_delay(300000); } while (!k);
        if (k == 0x1B) { term_print("\n"); return; }
        if ((k == '\r' || k == '\n') && d > 0) break;
        if ((k == '\b' || k == 127) && d > 0) {
            d--; amount /= 10;
            term_putc('\b'); term_putc(' '); term_putc('\b');
        } else if (k >= '0' && k <= '9' && d < 4) {
            nv = amount * 10 + (k - '0');
            if (nv <= 1000) { amount = nv; d++; term_putc((char)k); }
        }
    }
    if (amount < 1) { term_print("\nMontant invalide.\n"); return; }
    casino_balance += amount;
    if (casino_balance > casino_best) casino_best = casino_balance;
    term_set_color(0x0A, 0x00);
    term_print("\n+ ");
    term_print_int(amount);
    term_print(" GC ajoutes. Nouveau solde: ");
    term_print_int(casino_balance);
    term_print(" GC\n");
    term_set_color(0x07, 0x00);
    draw_interface();
}


#define PL_ROWS   8
#define PL_CX     40
#define PL_PY     2
#define PL_NSLOTS 9
#define PL_SL_Y   (PL_PY + PL_ROWS + 1)

static const int           pl_mult[PL_NSLOTS] = {8, 4, 2, 0, 0, 0, 2, 4, 8};
static const unsigned char pl_col [PL_NSLOTS] = {0x0E,0x0A,0x0B,0x08,0x08,0x08,0x0B,0x0A,0x0E};

static void pl_hdr(int bal, int best) {
    int x;
    term_set_color(0x0F, 0x01);
    term_move_cursor(0, 0);
    for (x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    term_move_cursor(2, 0);
    term_print("\x04 PLINKO \x04  Balance: ");
    term_set_color(bal < 100 ? 0x0C : 0x0A, 0x01);
    term_print_int(bal);
    term_set_color(0x0F, 0x01);
    term_print(" GC  |  Record: ");
    term_set_color(0x0E, 0x01);
    term_print_int(best);
    term_print(" GC");
    term_set_color(0x07, 0x00);
}

static void pl_draw_board(int hi) {
    int r, j, x, y, i;
    term_set_color(0x00, 0x00);
    for (y = 1; y <= PL_SL_Y + 1; y++) {
        term_move_cursor(0, y);
        for (x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    }
    term_set_color(0x0B, 0x00);
    term_move_cursor(0, 1);
    for (x = 0; x < VGA_WIDTH; x++) term_putc('\xc4');
    const char* t = " \x04 PLINKO \x04 ";
    int tl = 0; while (t[tl]) tl++;
    term_move_cursor((VGA_WIDTH - tl) / 2, 1);
    term_set_color(0x0F, 0x00);
    term_print(t);
    for (r = 1; r <= PL_ROWS; r++) {
        for (j = 0; j < r; j++) {
            x = PL_CX - (r - 1) + 2 * j;
            y = PL_PY + r - 1;
            term_set_color(0x07, 0x00);
            term_move_cursor(x, y);
            term_putc('\xFA');
        }
    }
    int floor_y = PL_PY + PL_ROWS;
    term_set_color(0x07, 0x00);
    term_move_cursor(30, floor_y);
    for (x = 30; x <= 50; x++) term_putc('\xC4');
    for (i = 0; i <= PL_NSLOTS; i++) {
        x = 31 + 2 * i;
        term_move_cursor(x, floor_y);
        term_putc('\xC2');
    }
    for (i = 0; i < PL_NSLOTS; i++) {
        x = 32 + 2 * i;
        unsigned char fg = (hi == i) ? 0x0F : pl_col[i];
        unsigned char bg = (hi == i) ? (pl_mult[i] > 0 ? 0x02 : 0x04) : 0x00;
        term_set_color(fg, bg);
        term_move_cursor(x, PL_SL_Y);
        if (pl_mult[i] == 0) term_putc('X');
        else { term_print_int(pl_mult[i]); }
    }
    term_set_color(0x07, 0x00);
}

static void pl_msg(const char* s, unsigned char col, int y) {
    int x, len = 0;
    while (s[len]) len++;
    term_set_color(0x00, 0x00);
    term_move_cursor(0, y);
    for (x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    term_set_color(col, 0x00);
    term_move_cursor((VGA_WIDTH - len) / 2, y);
    term_print(s);
    term_set_color(0x07, 0x00);
}

static void pl_draw_controls(int mise) {
    int x;
    term_set_color(0x00, 0x00);
    term_move_cursor(0, 15);
    for (x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    term_set_color(0x07, 0x00);
    term_move_cursor(2, 15);
    term_print("Mise: ");
    term_set_color(0x0E, 0x00);
    term_print_int(mise);
    term_set_color(0x07, 0x00);
    term_print(" GC  [+] augmenter  [-] diminuer  [ESPACE] Lancer  [ESC] Retour");
}

void start_plinko(void) {
    int x, y, k, r, bx, slot, gain, mise;
    unsigned int rng;

    term_init();
    gui_game_start();
    term_set_color(0x00, 0x00);
    for (y = 0; y < VGA_HEIGHT; y++) {
        term_move_cursor(0, y);
        for (x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    }

    int balance = casino_balance, best = casino_best;
    rng = (unsigned int)jiffies * 2654435761u ^ 0xC0FFEE00u;
    mise = 10;
    if (mise > balance) mise = balance;
    if (mise < 1) mise = 1;

    pl_hdr(balance, best);
    pl_draw_board(-1);
    pl_draw_controls(mise);
    pl_msg("  Gains: 8x  \xb3  4x  \xb3  2x  \xb3  X=perdu    ESPACE = lancer  ", 0x07, 13);

    while (balance > 0) {
        do { k = get_monitor_char(); nop_delay(300000); } while (!k);
        if (k == 0x1B) goto pl_done;

        if (k == '+' || k == '=') {
            int nm = mise + (mise < 10 ? 1 : mise < 50 ? 5 : 10);
            if (nm > balance) nm = balance;
            if (nm > 500) nm = 500;
            mise = nm; pl_draw_controls(mise); continue;
        }
        if (k == '-') {
            int nm = mise - (mise <= 10 ? 1 : mise <= 50 ? 5 : 10);
            if (nm < 1) nm = 1;
            mise = nm; pl_draw_controls(mise); continue;
        }
        if (k != ' ') continue;

        term_set_color(0x00, 0x00);
        for (y = 12; y <= 14; y++) {
            term_move_cursor(0, y); for (x = 0; x < VGA_WIDTH; x++) term_putc(' ');
        }

        bx = PL_CX;
        term_set_color(0x0E, 0x00);
        term_move_cursor(bx, 1); term_putc('O');
        nop_delay(80000000);
        term_move_cursor(bx, 1); term_putc(' ');

        for (r = 0; r < PL_ROWS; r++) {
            int py = PL_PY + r;
            if (r > 0) {
                term_set_color(0x0E, 0x00);
                term_move_cursor(bx, py - 1); term_putc('\xF9');
                nop_delay(50000000);
                term_set_color(0x00, 0x00);
                term_move_cursor(bx, py - 1); term_putc(' ');
            }
            term_set_color(0x0E, 0x00);
            term_move_cursor(bx, py); term_putc('O');
            nop_delay(90000000);
            term_set_color(0x07, 0x00);
            term_move_cursor(bx, py); term_putc('\xFA');
            rng = rng * 1664525u + 1013904223u;
            bx += (int)((rng >> 16) & 1u) ? 1 : -1;
        }
        term_set_color(0x0E, 0x00);
        term_move_cursor(bx, PL_PY + PL_ROWS); term_putc('O');
        nop_delay(180000000);
        term_set_color(0x00, 0x00);
        term_move_cursor(bx, PL_PY + PL_ROWS); term_putc(' ');

        slot = (bx - (PL_CX - PL_ROWS)) / 2;
        if (slot < 0) slot = 0;
        if (slot >= PL_NSLOTS) slot = PL_NSLOTS - 1;

        gain = pl_mult[slot] > 0 ? mise * pl_mult[slot] : -mise;
        balance += gain;
        if (balance < 0) balance = 0;
        if (balance > best) best = balance;
        casino_balance = balance;
        if (best > casino_best) casino_best = best;
        hs_update(7, casino_best);

        pl_draw_board(slot);
        pl_hdr(balance, best);

        if (gain > 0) {
            if (pl_mult[slot] >= 8) {
                beep_ok(); nop_delay(80000000); beep_ok();
                pl_msg("  \xDB JACKPOT ! \xDB  ", 0x0E, 12);
                casino_win_anim(1);
            } else {
                beep_ok();
                pl_msg("  GAGNE !  ", 0x0A, 12);
                casino_win_anim(0);
            }
            term_set_color(0x0A, 0x00);
        } else {
            beep_fail();
            pl_msg("  PERDU.  ", 0x0C, 12);
            term_set_color(0x0C, 0x00);
        }
        term_move_cursor(2, 13);
        if (gain > 0) { term_print("+ "); term_print_int(gain); }
        else          { term_print("- "); term_print_int(mise); }
        term_print(" GC  ->  Solde: "); term_print_int(balance);
        term_print(" GC   ("); term_print_int(pl_mult[slot]); term_print("x)  ");
        term_set_color(0x07, 0x00);

        if (balance == 0) {
            beep_gameover();
            pl_msg("  BANQUEROUTE !  ", 0x0C, 14);
            nop_delay(300000000);
            do { k = get_monitor_char(); nop_delay(300000); } while (!k);
            goto pl_done;
        }
        if (mise > balance) mise = balance;
        pl_draw_controls(mise);
    }

pl_done:
    casino_balance = balance;
    if (best > casino_best) casino_best = best;
    hs_update(7, casino_best);
    term_init();
    draw_interface();
}

#undef PL_ROWS
#undef PL_CX
#undef PL_PY
#undef PL_NSLOTS
#undef PL_SL_Y


static void casino_draw_lobby(void) {
    int x, y, i;
    static const char* gname[4] = {"ROULETTE","BLACKJACK","MACHINE A SOUS","PLINKO"};
    static const char* gdesc[4] = {"Rouge/Noir/Plein/Douzaine","Tirez 21 ! Natural = 3:2 ","777=Jackpot 40x !        ","Lachez la balle !        "};
    static const unsigned char gcol[4] = {0x0C, 0x0E, 0x0A, 0x0B};
    int gx[4] = {4, 28, 52, 4};
    int gy[4] = {7, 7, 7, 11};

    term_set_color(0x0F, 0x01);
    term_move_cursor(0, 0);
    for (x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    term_move_cursor(2, 0);
    term_set_color(0x0E, 0x01); term_print("\x04\x03 CASINO GREGOS \x03\x04");
    term_set_color(0x0F, 0x01); term_print("  Solde: ");
    term_set_color(casino_balance < 100 ? 0x0C : 0x0A, 0x01);
    term_print_int(casino_balance);
    term_set_color(0x0F, 0x01); term_print(" GC  |  Record: ");
    term_set_color(0x0E, 0x01); term_print_int(casino_best); term_print(" GC");
    term_set_color(0x07, 0x00);

    term_set_color(0x00, 0x00);
    for (y = 1; y < VGA_HEIGHT; y++) {
        term_move_cursor(0, y);
        for (x = 0; x < VGA_WIDTH; x++) term_putc(' ');
    }

    term_set_color(0x07, 0x00);
    term_move_cursor(1, 1); term_putc('\xda');
    for (x = 2; x < VGA_WIDTH - 1; x++) term_putc('\xc4');
    term_putc('\xbf');
    for (y = 2; y <= 16; y++) {
        term_move_cursor(1, y); term_putc('\xb3');
        term_move_cursor(VGA_WIDTH - 2, y); term_putc('\xb3');
    }
    term_move_cursor(1, 17); term_putc('\xc0');
    for (x = 2; x < VGA_WIDTH - 1; x++) term_putc('\xc4');
    term_putc('\xd9');

    term_set_color(0x0E, 0x00);
    const char* t = " \x04 \x03 CASINO GREGOS \x03 \x04 ";
    int tl = 0; while (t[tl]) tl++;
    term_move_cursor((VGA_WIDTH - tl) / 2, 3);
    term_print(t);

    term_set_color(0x08, 0x00);
    term_move_cursor(2, 5);
    for (x = 2; x < VGA_WIDTH - 2; x++) term_putc('\xC4');
    term_move_cursor(2, 5); term_set_color(0x0B, 0x00); term_print(" CHOISISSEZ UN JEU ");

    for (i = 0; i < 4; i++) {
        term_set_color(0x0F, 0x00);
        term_move_cursor(gx[i], gy[i]);
        term_putc('['); term_set_color(gcol[i], 0x00); term_putc('1' + i);
        term_set_color(0x0F, 0x00); term_print("] ");
        term_set_color(gcol[i], 0x00); term_print(gname[i]);
        term_set_color(0x08, 0x00);
        term_move_cursor(gx[i] + 4, gy[i] + 1);
        term_print(gdesc[i]);
    }

    term_set_color(0x0F, 0x00);
    term_move_cursor(28, 11); term_print("[5]");
    term_set_color(0x0B, 0x00); term_print(" ATM");
    term_set_color(0x08, 0x00);
    term_move_cursor(32, 12); term_print("+1000 GregCoins");

    term_set_color(0x0F, 0x00);
    term_move_cursor(52, 11); term_print("[6] / [ESC]");
    term_set_color(0x07, 0x00); term_print(" QUITTER");
    term_set_color(0x08, 0x00);
    term_move_cursor(52, 12); term_print("Retour au shell");

    term_set_color(0x08, 0x00);
    term_move_cursor(2, 14);
    for (x = 2; x < VGA_WIDTH - 2; x++) term_putc('\xC4');
    term_move_cursor(2, 14); term_set_color(0x08, 0x00);
    term_print(" Solde: "); term_set_color(casino_balance < 100 ? 0x0C : 0x0A, 0x00);
    term_print_int(casino_balance); term_print(" GC");
    term_set_color(0x08, 0x00); term_print("  |  Record: "); term_set_color(0x0E, 0x00);
    term_print_int(casino_best); term_print(" GC");
    term_set_color(0x07, 0x00);
}

void start_casino(void) {
    int k;
    term_init();
    gui_game_start();
    while (1) {
        casino_draw_lobby();
        do { k = get_monitor_char(); nop_delay(300000); } while (!k);
        switch (k) {
            case '1': start_roulette();  break;
            case '2': start_blackjack(); break;
            case '3': start_slots();     break;
            case '4': start_plinko();    break;
            case '5':
                casino_balance += 1000;
                if (casino_balance > casino_best) casino_best = casino_balance;
                hs_update(7, casino_best);
                break;
            case '6': case 0x1B: goto casino_exit;
        }
        term_init();
    }
casino_exit:
    term_init();
    draw_interface();
}

/* ═══════════════════ GRAPHICAL CASINO APP ═══════════════════════════
   Fully gfx_-based casino: Blackjack, Roulette, Machine à Sous, Plinko.
   No VGA-blit mode — all rendering done with gfx_ primitives directly.
   ═══════════════════════════════════════════════════════════════════ */

/* ── Palette ── */
#define CG_BG     GFX_RGB(0x0B,0x2A,0x0F)
#define CG_FELT   GFX_RGB(0x14,0x5A,0x1F)
#define CG_TABLE  GFX_RGB(0x0F,0x44,0x18)
#define CG_GOLD   GFX_RGB(0xFF,0xD7,0x00)
#define CG_GOLD2  GFX_RGB(0x88,0x74,0x00)
#define CG_CREAM  GFX_RGB(0xFF,0xF8,0xE0)
#define CG_RED_C  GFX_RGB(0xCC,0x11,0x11)
#define CG_BLK_C  GFX_RGB(0x11,0x11,0x11)
#define CG_NAVY   GFX_RGB(0x1A,0x3A,0x8A)
#define CG_BORD   GFX_RGB(0x88,0x88,0x88)
#define CG_CARD_W 72
#define CG_CARD_H 104

/* ── Card helpers ── */
static const char* cg_val[14] = {
    "","A","2","3","4","5","6","7","8","9","10","J","Q","K"
};
static const char cg_sc[4] = { 'S','H','D','C' };
static inline int cg_red(int suit) { return suit==1||suit==2; }

/* Deck */
static int cg_deck[52]; static int cg_deckp = 0;
static void cg_shuffle(void) {
    for (int i=0;i<52;i++) cg_deck[i]=i;
    for (int i=51;i>0;i--) { int j=rand()%(i+1); int t=cg_deck[i]; cg_deck[i]=cg_deck[j]; cg_deck[j]=t; }
    cg_deckp = 0;
}
static int cg_deal(void) { if (cg_deckp>=52) cg_shuffle(); return cg_deck[cg_deckp++]; }

/* Hand value (blackjack) */
static int cg_hval(int* h, int n) {
    int t=0, ac=0;
    for (int i=0;i<n;i++) {
        int v=h[i]%13+1;
        if (v==1) { ac++; t+=11; } else if (v>=10) t+=10; else t+=v;
    }
    while (t>21&&ac>0) { t-=10; ac--; }
    return t;
}

/* Draw suit symbol in a ~20×22 area centred at (cx,cy) */
static void cg_sym(int cx, int cy, int suit, unsigned int col) {
    switch (suit) {
    case 1: { /* Heart: two rect bumps + downward triangle */
        gfx_fill_rect(cx-10, cy,   9, 8, col);
        gfx_fill_rect(cx+1,  cy,   9, 8, col);
        int w[12]={18,18,16,14,12,10,8,6,4,2,0,0};
        for (int i=0;i<12;i++) if(w[i]) gfx_fill_rect(cx-w[i]/2, cy+7+i, w[i], 2, col);
        break; }
    case 2: { /* Diamond: rhombus */
        int p[22]={2,4,6,8,10,12,14,16,18,20,22,20,18,16,14,12,10,8,6,4,2,0};
        for (int i=0;i<22;i++) if(p[i]) gfx_fill_rect(cx-p[i]/2, cy+i, p[i], 1, col);
        break; }
    case 0: { /* Spade: upward triangle + two bottom bumps + stem */
        int w[10]={20,18,16,14,12,10,8,6,4,2};
        for (int i=0;i<10;i++) gfx_fill_rect(cx-w[i]/2, cy+2+(9-i), w[i], 2, col);
        gfx_fill_rect(cx-10, cy+10, 9, 7, col);
        gfx_fill_rect(cx+1,  cy+10, 9, 7, col);
        gfx_fill_rect(cx-2,  cy+16, 4, 5, col);
        gfx_fill_rect(cx-7,  cy+20, 14,3, col);
        break; }
    case 3: { /* Club: 3 circles + stem */
        gfx_fill_rect(cx-10, cy+5,  10,10, col);
        gfx_fill_rect(cx+0,  cy+5,  10,10, col);
        gfx_fill_rect(cx-5,  cy,    10,10, col);
        gfx_fill_rect(cx-8,  cy+11, 16, 6, col);
        gfx_fill_rect(cx-2,  cy+16,  4, 5, col);
        gfx_fill_rect(cx-7,  cy+20, 14, 3, col);
        break; }
    }
}

/* Draw a playing card. card=0-51. facedown=1 shows back. */
static void cg_card(int x, int y, int card, int facedown) {
    int cw=CG_CARD_W, ch=CG_CARD_H;
    gfx_fill_rect(x+3, y+3, cw, ch, GFX_RGB(0,0,0));           /* shadow */
    gfx_fill_rect(x, y, cw, ch, facedown ? CG_NAVY : CG_CREAM);
    gfx_draw_rect(x,   y,   cw,   ch,   CG_BORD);
    gfx_draw_rect(x+1, y+1, cw-2, ch-2,
                  facedown ? GFX_RGB(0x3A,0x6A,0xCA) : GFX_RGB(0xCC,0xCC,0xCC));
    if (facedown) {
        /* Striped back */
        for (int d=8; d<cw+ch-8; d+=10)
            for (int k=0;k<4;k++)
                for (int r=4; r<ch-4; r++) {
                    int bx=x+4+d+k-r;
                    if (bx>=x+4 && bx<x+cw-4)
                        gfx_put_pixel(bx, y+r, GFX_RGB(0x2A,0x5A,0xBA));
                }
        gfx_draw_rect(x+4, y+4, cw-8, ch-8, GFX_RGB(0x5A,0x8A,0xEA));
        return;
    }
    int val=card%13+1, suit=card/13;
    unsigned int sc = cg_red(suit) ? CG_RED_C : CG_BLK_C;
    const char* vs = cg_val[val];
    char sb[2] = {cg_sc[suit], 0};
    gfx_draw_str(x+4, y+5,           vs, sc, CG_CREAM);
    gfx_draw_str(x+4, y+5+GFX_FONT_H,sb, sc, CG_CREAM);
    cg_sym(x+cw/2, y+ch/2-10, suit, sc);
    int vl=strlen(vs);
    gfx_draw_str(x+cw-4-vl*GFX_FONT_W, y+ch-5-2*GFX_FONT_H, vs, sc, CG_CREAM);
    gfx_draw_str(x+cw-4-GFX_FONT_W,    y+ch-5-GFX_FONT_H,   sb, sc, CG_CREAM);
}

/* Draw text centered at cx */
static void cg_cen(int cx, int y, const char* s, unsigned int fg, unsigned int bg) {
    gfx_draw_str(cx - strlen(s)*GFX_FONT_W/2, y, s, fg, bg);
}

/* Header bar (y=0..48) */
static void cg_hdr(const char* title, int balance) {
    int W=gfx_width();
    gfx_fill_rect(0, 0, W, 48, CG_BG);
    gfx_draw_hline(0, 48, W, CG_GOLD2);
    gfx_draw_str(12, 8,  "Casino GregOS", CG_GOLD, CG_BG);
    gfx_draw_str(12, 26, title,           GFX_RGB(0xAA,0xFF,0xAA), CG_BG);
    char buf[28]; buf[0]='B'; buf[1]='a'; buf[2]='l'; buf[3]=':'; buf[4]=' ';
    itoa(balance, buf+5); int p=strlen(buf);
    buf[p]=' '; buf[p+1]='G'; buf[p+2]='C'; buf[p+3]='\0'; p+=3;
    gfx_draw_str(W-8-p*GFX_FONT_W, 16, buf,
                 balance<50 ? GFX_RGB(0xFF,0x55,0x55) : GFX_RGB(0x55,0xFF,0x55), CG_BG);
    gfx_draw_str(W-8-11*GFX_FONT_W, 32, "[ESC] Lobby", GFX_RGB(0x55,0x77,0x55), CG_BG);
}

/* Control strip at bottom (y=H-88..H) */
static void cg_ctrl(const char* msg, const char* keys) {
    int W=gfx_width(), H=gfx_height();
    gfx_fill_rect(0, H-88, W, 88, CG_BG);
    gfx_draw_hline(0, H-88, W, CG_GOLD2);
    if (msg && msg[0]) {
        unsigned int mc = GFX_RGB(0xFF,0xDD,0x00);
        cg_cen(W/2, H-72, msg, mc, CG_BG);
    }
    if (keys && keys[0])
        gfx_draw_str(16, H-48, keys, GFX_RGB(0xAA,0xFF,0xAA), CG_BG);
}

/* Game button */
static void cg_btn(int x, int y, int w, int h, unsigned int bg, unsigned int hl,
                   const char* key, const char* label, const char* sub) {
    gfx_fill_rect(x+4, y+4, w, h, GFX_RGB(0,0,0));
    gfx_fill_rect(x, y, w, h, bg);
    gfx_draw_rect(x,   y,   w,   h,   hl);
    gfx_draw_rect(x+1, y+1, w-2, h-2, hl);
    gfx_fill_rect(x+8, y+8, 22, 20, hl);
    cg_cen(x+19, y+10, key, CG_BG, hl);
    cg_cen(x+w/2, y+h/2-GFX_FONT_H-4, label, GFX_RGB(0xFF,0xFF,0xFF), bg);
    cg_cen(x+w/2, y+h/2+4,             sub,   GFX_RGB(0xAA,0xCC,0xAA), bg);
}

/* ── Casino Lobby ── */
static void __attribute__((unused)) cg_lobby(void) {
    int W=gfx_width(), H=gfx_height();
    gfx_gradient_rect(0, 48, W, H-48, GFX_RGB(0x0B,0x1A,0x10), CG_FELT, 1);
    /* Title */
    cg_cen(W/2, 64, "~~ CASINO GREGOS ~~", CG_GOLD, GFX_RGB(0x0B,0x1A,0x10));
    gfx_draw_hline(W/2-160, 84, 320, CG_GOLD2);
    gfx_draw_hline(W/2-160, 86, 320, CG_GOLD2);
    /* 2×2 grid of game buttons */
    int bw=280, bh=140, gap=28;
    int gx=W/2-bw-gap/2, gy=98;
    cg_btn(gx,        gy,       bw,bh, GFX_RGB(0x18,0x08,0x04), GFX_RGB(0xCC,0x44,0x00),
           "1", "BLACKJACK",    "Battez le croupier (21)");
    cg_btn(gx+bw+gap, gy,       bw,bh, GFX_RGB(0x04,0x12,0x04), GFX_RGB(0x22,0xAA,0x22),
           "2", "ROULETTE",     "Rouge, noir, pair...");
    cg_btn(gx,        gy+bh+gap, bw,bh, GFX_RGB(0x16,0x06,0x20), GFX_RGB(0x88,0x22,0xCC),
           "3", "MACHINE A SOUS","Alignez les symboles");
    cg_btn(gx+bw+gap, gy+bh+gap, bw,bh, GFX_RGB(0x00,0x10,0x20), GFX_RGB(0x22,0x66,0xCC),
           "4", "PLINKO",       "Faites tomber les jetons");
    /* Bottom info */
    gfx_draw_hline(0, H-96, W, CG_GOLD2);
    gfx_fill_rect(0, H-95, W, 95, CG_BG);
    char buf[24]; buf[0]='S'; buf[1]='o'; buf[2]='l'; buf[3]='d'; buf[4]='e'; buf[5]=':'; buf[6]=' ';
    itoa(casino_balance, buf+7); int p=strlen(buf);
    buf[p]=' '; buf[p+1]='G'; buf[p+2]='C'; buf[p+3]='\0';
    cg_cen(W/2, H-72, buf, casino_balance<50 ? GFX_RGB(0xFF,0x55,0x55) : CG_GOLD, CG_BG);
    gfx_draw_str(W/2-11*GFX_FONT_W, H-44, "[ESC] Quitter le Casino",
                 GFX_RGB(0x55,0x77,0x55), CG_BG);
    if (casino_balance <= 0)
        cg_cen(W/2, H-20, "[ATM] Tape 'atm' dans le terminal pour recharger",
               GFX_RGB(0xFF,0x88,0x00), CG_BG);
}

/* ── Graphical Blackjack ── */
static void cg_draw_bj(int* deal, int dc, int* play, int pc,
                        int bal, int bet, int show_deal, int phase, const char* msg) {
    int W=gfx_width(), H=gfx_height();
    gfx_gradient_rect(0, 48, W, H-88, GFX_RGB(0x06,0x28,0x0A), CG_TABLE, 1);
    /* Table felt */
    gfx_fill_rect(40, 64, W-80, H-88-64, CG_TABLE);
    gfx_draw_rect(40, 64, W-80, H-88-64, CG_GOLD2);
    gfx_draw_rect(42, 66, W-84, H-88-68, CG_GOLD2);
    gfx_draw_hline(40, 200, W-80, CG_GOLD2);

    /* Dealer */
    gfx_draw_str(56, 72, "CROUPIER", GFX_RGB(0xCC,0xCC,0xCC), CG_TABLE);
    if (show_deal && dc>0) {
        int dv=cg_hval(deal,dc);
        char dvs[16]=" "; itoa(dv,dvs+1);
        gfx_draw_str(56+9*GFX_FONT_W, 72, dvs,
                     dv>21 ? GFX_RGB(0xFF,0x55,0x55) : GFX_RGB(0xFF,0xFF,0xFF), CG_TABLE);
    }
    int cx=W/2-(dc*(CG_CARD_W+8))/2;
    for (int i=0;i<dc;i++) cg_card(cx+i*(CG_CARD_W+8), 92, deal[i], i==0&&!show_deal);

    /* Player */
    gfx_draw_str(56, 210, "VOUS", GFX_RGB(0xCC,0xCC,0xCC), CG_TABLE);
    if (pc>0) {
        int pv=cg_hval(play,pc);
        char pvs[16]=" "; itoa(pv,pvs+1);
        gfx_draw_str(56+5*GFX_FONT_W, 210, pvs,
                     pv>21 ? GFX_RGB(0xFF,0x55,0x55) : GFX_RGB(0xFF,0xFF,0xFF), CG_TABLE);
    }
    cx=W/2-(pc*(CG_CARD_W+8))/2;
    for (int i=0;i<pc;i++) cg_card(cx+i*(CG_CARD_W+8), 228, play[i], 0);

    /* Bet/balance panel */
    gfx_fill_rect(40, H-88-52, W-80, 48, GFX_RGB(0x08,0x24,0x0C));
    gfx_draw_rect(40, H-88-52, W-80, 48, CG_GOLD2);
    char bs[32]; bs[0]='M'; bs[1]='i'; bs[2]='s'; bs[3]='e'; bs[4]=':'; bs[5]=' ';
    itoa(bet, bs+6); int p=strlen(bs);
    bs[p]=' '; bs[p+1]='G'; bs[p+2]='C'; bs[p+3]='|'; bs[p+4]=' ';
    bs[p+5]='B'; bs[p+6]='a'; bs[p+7]='l'; bs[p+8]=':'; bs[p+9]=' ';
    itoa(bal, bs+p+10);
    gfx_draw_str(56, H-88-38, bs, CG_GOLD, GFX_RGB(0x08,0x24,0x0C));

    /* Controls */
    if (phase==0)
        cg_ctrl(msg, "[H/J] Mise -10/+10   [ESPACE] Distribuer   [ESC] Lobby");
    else if (phase==1)
        cg_ctrl(msg, "[H] Tirer   [S] Rester   [D] Doubler (x2)   [ESC] Lobby");
    else
        cg_ctrl(msg, "[ESPACE] Rejouer   [ESC] Lobby");
}

static void cg_blackjack(void) {
    int deal[12], play[12], dc=0, pc=0;
    int bal=casino_balance, bet=50;
    if (bet>bal) bet=bal<10?10:bal;
    int phase=0; const char* msg="";
    cg_shuffle();
    while (1) {
        cg_hdr("Blackjack - 21", bal);
        cg_draw_bj(deal,dc,play,pc,bal,bet, phase!=1, phase, msg);
        gfx_swap_buffers();
        int k; do { k=get_monitor_char(); nop_delay(80000); } while (!k);
        if (k==0x1B) break;
        if (phase==0) {
            if ((k=='h'||k=='H') && bet>10) bet-=10;
            if ((k=='j'||k=='J') && bet+10<=bal) bet+=10;
            if (k==' ' && bal>=bet && bet>0) {
                dc=pc=0;
                deal[dc++]=cg_deal(); play[pc++]=cg_deal();
                deal[dc++]=cg_deal(); play[pc++]=cg_deal();
                bal-=bet; phase=1; msg="";
                if (cg_hval(play,pc)==21) {
                    phase=2;
                    if (cg_hval(deal,dc)==21) { bal+=bet; msg="EGALITE - Double BJ"; }
                    else { bal+=bet+bet+bet/2; msg="BLACKJACK ! Paye 3:2 !"; }
                }
            }
        } else if (phase==1) {
            if (k=='h'||k=='H') {
                if (pc<11) play[pc++]=cg_deal();
                if (cg_hval(play,pc)>21) { phase=2; msg="BUST ! Vous avez depasse 21."; }
            } else if (k=='s'||k=='S') {
                while (cg_hval(deal,dc)<17&&dc<11) deal[dc++]=cg_deal();
                int dv=cg_hval(deal,dc), pv=cg_hval(play,pc);
                if (dv>21||pv>dv) { bal+=bet*2; msg="VICTOIRE !"; }
                else if (pv==dv)  { bal+=bet;   msg="EGALITE"; }
                else               {             msg="PERDU"; }
                phase=2;
            } else if ((k=='d'||k=='D') && pc==2 && bal>=bet) {
                bal-=bet; bet*=2;
                if (pc<11) play[pc++]=cg_deal();
                while (cg_hval(deal,dc)<17&&dc<11) deal[dc++]=cg_deal();
                int dv=cg_hval(deal,dc), pv=cg_hval(play,pc);
                if (pv>21)          {             msg="BUST ! Depasse 21."; }
                else if (dv>21||pv>dv) { bal+=bet*2; msg="VICTOIRE ! Double !"; }
                else if (pv==dv)    { bal+=bet;   msg="EGALITE"; }
                else                {             msg="PERDU"; }
                phase=2;
            }
        } else {
            if (k==' ') {
                dc=pc=0; phase=0; msg="";
                if (bal<=0) bal=500;
                if (bet>bal) bet=(bal/10)*10;
                if (bet<10) bet=10;
            }
        }
    }
    casino_balance=bal;
    if (bal>casino_best) casino_best=bal;
    hs_update(8, casino_best);
}

/* ── Graphical Roulette ── */
static unsigned int rl_col(int n) {
    if (n==0) return GFX_RGB(0x00,0xAA,0x00);
    static const unsigned char reds[]={1,3,5,7,9,12,14,16,18,19,21,23,25,27,30,32,34,36};
    for (int i=0;i<18;i++) if (reds[i]==(unsigned char)n) return CG_RED_C;
    return GFX_RGB(0x22,0x22,0x22);
}

static void cg_draw_rl(int result, int phase, int bet, int btype, int bal, const char* msg, int chosen_num) {
    int W=gfx_width(), H=gfx_height();
    gfx_gradient_rect(0, 48, W, H-88, GFX_RGB(0x06,0x18,0x06), CG_TABLE, 1);

    /* Number strip — highlight active position with gold fill */
    int ox=80, oy=64;
    gfx_fill_rect(ox-2, oy-2, 37*17+4, 62, CG_GOLD2);
    for (int n=0;n<37;n++) {
        unsigned int c=rl_col(n);
        int hi = (n==result && phase>0);
        unsigned int cell_bg = hi ? CG_GOLD : c;
        gfx_fill_rect(ox+n*17, oy, 16, 60, cell_bg);
        char ns[4]; ns[0]='0'+n/10; ns[1]='0'+n%10; ns[2]='\0';
        gfx_draw_str(ox+n*17+(n<10?4:0), oy+22, n<10?ns+1:ns,
                     hi ? GFX_RGB(0x00,0x00,0x00) : GFX_RGB(0xFF,0xFF,0xFF),
                     cell_bg);
    }

    /* Bet-type buttons — 7 types (width=84, gap=4, step=88) */
    static const char* blab[7]={"ROUGE","NOIR","PAIR","IMPAIR","1-18","19-36","EXACT"};
    static const unsigned int bbg[7]={
        GFX_RGB(0xAA,0x11,0x11), GFX_RGB(0x22,0x22,0x22),
        GFX_RGB(0x22,0x44,0x88), GFX_RGB(0x44,0x22,0x88),
        GFX_RGB(0x22,0x66,0x22), GFX_RGB(0x22,0x44,0x66),
        GFX_RGB(0x44,0x30,0x00)
    };
    int py=oy+72;
    for (int i=0;i<7;i++) {
        unsigned int bg = (i==btype) ? CG_GOLD : bbg[i];
        unsigned int fg = (i==btype) ? CG_BLK_C : GFX_RGB(0xFF,0xFF,0xFF);
        gfx_fill_rect(ox+i*88, py, 84, 36, bg);
        gfx_draw_rect(ox+i*88, py, 84, 36, CG_GOLD2);
        cg_cen(ox+i*88+42, py+10, blab[i], fg, bg);
    }
    /* Show chosen number when EXACT mode is active */
    if (btype==6) {
        char ns2[6]; ns2[0]='N'; ns2[1]='=';
        if (chosen_num < 10) { ns2[2]=' '; ns2[3]='0'+chosen_num; ns2[4]='\0'; }
        else { ns2[2]='0'+chosen_num/10; ns2[3]='0'+chosen_num%10; ns2[4]='\0'; }
        gfx_draw_str(ox+7*88+6, py+10, ns2, CG_GOLD, bbg[6]);
    }
    gfx_draw_str(ox, py+44,
                 "[1-6] Type de pari  [7] Numero exact  [H/J] Mise ou Chiffre",
                 GFX_RGB(0xBB,0xBB,0xBB), CG_TABLE);

    /* Bet/balance */
    gfx_fill_rect(40, H-88-52, W-80, 48, GFX_RGB(0x08,0x24,0x0C));
    gfx_draw_rect(40, H-88-52, W-80, 48, CG_GOLD2);
    char bs[32]; bs[0]='M'; bs[1]='i'; bs[2]='s'; bs[3]='e'; bs[4]=':'; bs[5]=' ';
    itoa(bet, bs+6); int p=strlen(bs);
    bs[p]=' '; bs[p+1]='G'; bs[p+2]='C'; bs[p+3]='|'; bs[p+4]=' ';
    bs[p+5]='B'; bs[p+6]='a'; bs[p+7]='l'; bs[p+8]=':'; bs[p+9]=' ';
    itoa(bal, bs+p+10);
    gfx_draw_str(56, H-88-38, bs, CG_GOLD, GFX_RGB(0x08,0x24,0x0C));

    if (phase==0)
        cg_ctrl(msg, "[H/J] Mise/Chiffre  [1-7] Type de pari  [ESPACE] Tourner  [ESC] Lobby");
    else
        cg_ctrl(msg, "[ESPACE] Rejouer   [ESC] Lobby");
}

static int rl_win(int r, int bt) {
    if (r==0) return 0;
    switch(bt) {
        case 0: return rl_col(r)==CG_RED_C;
        case 1: return rl_col(r)!=CG_RED_C && r!=0;
        case 2: return r%2==0;
        case 3: return r%2==1;
        case 4: return r>=1&&r<=18;
        case 5: return r>=19&&r<=36;
    }
    return 0;
}

static void cg_roulette(void) {
    int bal=casino_balance, bet=50, btype=0, result=0, phase=0;
    int chosen_num = 18;
    if (bet>bal) bet=bal;
    const char* msg="";
    while (1) {
        cg_hdr("Roulette", bal);
        cg_draw_rl(result, phase, bet, btype, bal, msg, chosen_num);
        gfx_swap_buffers();
        int k; do { k=get_monitor_char(); nop_delay(80000); } while (!k);
        if (k==0x1B) break;
        if (phase==0) {
            if (k>='1' && k<='7') btype=k-'1';
            if (k=='h'||k=='H') {
                if (btype==6) { if (chosen_num>0) chosen_num--; }
                else if (bet>10) bet-=10;
            }
            if (k=='j'||k=='J') {
                if (btype==6) { if (chosen_num<36) chosen_num++; }
                else if (bet+10<=bal) bet+=10;
            }
            if (k==' ' && bal>=bet && bet>0) {
                bal -= bet;
                /* Pre-compute landing number so animation ends exactly on it */
                int steps = 25 + rand()%20;
                int cur = rand()%37;
                result = (cur + steps) % 37;
                /* Deceleration animation — pitch drops, delay grows per step */
                int delay = 300000;
                for (int sp=0; sp<steps; sp++) {
                    cur = (cur+1) % 37;
                    cg_hdr("Roulette", bal);
                    cg_draw_rl(cur, 1, bet, btype, bal, "~ ~ ~", chosen_num);
                    gfx_swap_buffers();
                    unsigned int freq = 1500u - (unsigned int)((sp * 900) / steps);
                    if (freq < 600u) freq = 600u;
                    timer_speaker_on(freq); nop_delay(150000); timer_speaker_off();
                    nop_delay(delay);
                    delay += 120000;
                    if (delay > 2500000) delay = 2500000;
                }
                /* Win/lose */
                int won = (btype==6) ? (result==chosen_num) : rl_win(result, btype);
                if (won) {
                    int payout = (btype==6) ? bet*36 : bet*2;
                    bal += payout;
                    msg = (btype==6) ? "VICTOIRE ! Numero exact ! (x35)" : "VICTOIRE !";
                    timer_beep(523, 20);
                    timer_beep(659, 20);
                    timer_beep(784, 20);
                    timer_beep(1047, 40);
                } else {
                    msg = "PERDU";
                    timer_beep(330, 50);
                    timer_beep(220, 80);
                }
                phase=1;
            }
        } else {
            if (k==' ') {
                phase=0; msg="";
                if (bal<=0) bal=500;
                if (bet>bal) bet=(bal/10)*10;
                if (bet<10) bet=10;
            }
        }
    }
    casino_balance=bal;
    if (bal>casino_best) casino_best=bal;
}

/* ── Graphical Machine à Sous ── */
static const char* cg_sl_sym[6]={"777","BAR","BEL","CHR","GRG","ORA"};
static unsigned int cg_sl_col[6]={
    GFX_RGB(0xFF,0xD7,0x00), GFX_RGB(0xFF,0xFF,0xFF),
    GFX_RGB(0xFF,0xAA,0x00), GFX_RGB(0xFF,0x44,0x44),
    GFX_RGB(0x44,0xFF,0x88), GFX_RGB(0xFF,0x88,0x00)
};

static void cg_draw_sl(int r0, int r1, int r2, int bet, int bal, int spin, const char* msg) {
    int W=gfx_width(), H=gfx_height();
    gfx_gradient_rect(0, 48, W, H-88, GFX_RGB(0x16,0x06,0x26), GFX_RGB(0x22,0x0A,0x40), 1);
    /* Machine frame */
    int mx=W/2-200, my=70, mw=400, mh=220;
    gfx_fill_rect(mx+4, my+4, mw, mh, GFX_RGB(0,0,0));
    gfx_fill_rect(mx, my, mw, mh, GFX_RGB(0x1E,0x0C,0x36));
    gfx_draw_rect(mx,   my,   mw,   mh,   GFX_RGB(0xAA,0x44,0xFF));
    gfx_draw_rect(mx+2, my+2, mw-4, mh-4, GFX_RGB(0x66,0x22,0xAA));
    cg_cen(W/2, my+12, "-- MACHINE A SOUS --", GFX_RGB(0xFF,0xD7,0x00), GFX_RGB(0x1E,0x0C,0x36));
    /* Three reels */
    int rw=108, rh=110, ry=my+40;
    int rx[3]={mx+16, mx+146, mx+276};
    int syms[3]={r0,r1,r2};
    for (int i=0;i<3;i++) {
        int s=syms[i];
        gfx_fill_rect(rx[i]+3, ry+3, rw, rh, GFX_RGB(0,0,0));
        gfx_fill_rect(rx[i], ry, rw, rh, GFX_RGB(0x0C,0x04,0x18));
        gfx_draw_rect(rx[i], ry, rw, rh, GFX_RGB(0xAA,0x44,0xFF));
        if (spin)
            cg_cen(rx[i]+rw/2, ry+rh/2-8, "...", GFX_RGB(0x55,0x55,0x55), GFX_RGB(0x0C,0x04,0x18));
        else {
            cg_cen(rx[i]+rw/2, ry+rh/2-GFX_FONT_H/2,
                   cg_sl_sym[s], cg_sl_col[s], GFX_RGB(0x0C,0x04,0x18));
        }
    }
    /* Paytable */
    gfx_draw_str(mx, my+mh+8,
                 "GRGx3=100x  777x3=50x  BARx3=20x  BELx3=10x  x3=5x  x2=2x",
                 GFX_RGB(0x88,0x66,0xAA), GFX_RGB(0x16,0x06,0x26));
    /* Bet/balance */
    gfx_fill_rect(40, H-88-52, W-80, 48, GFX_RGB(0x14,0x06,0x22));
    gfx_draw_rect(40, H-88-52, W-80, 48, GFX_RGB(0x88,0x22,0xCC));
    char bs[32]; bs[0]='M'; bs[1]='i'; bs[2]='s'; bs[3]='e'; bs[4]=':'; bs[5]=' ';
    itoa(bet, bs+6); int p=strlen(bs);
    bs[p]=' '; bs[p+1]='G'; bs[p+2]='C'; bs[p+3]='|'; bs[p+4]=' ';
    bs[p+5]='B'; bs[p+6]='a'; bs[p+7]='l'; bs[p+8]=':'; bs[p+9]=' ';
    itoa(bal, bs+p+10);
    gfx_draw_str(56, H-88-38, bs, CG_GOLD, GFX_RGB(0x14,0x06,0x22));
    cg_ctrl(msg, "[ESPACE] Lancer   [H/J] Mise -10/+10   [ESC] Lobby");
}

static void cg_slots(void) {
    int bal=casino_balance, bet=50, r0=0, r1=0, r2=0;
    if (bet>bal) bet=bal;
    const char* msg="";
    while (1) {
        cg_hdr("Machine a sous", bal);
        cg_draw_sl(r0, r1, r2, bet, bal, 0, msg);
        gfx_swap_buffers();
        int k; do { k=get_monitor_char(); nop_delay(80000); } while (!k);
        if (k==0x1B) break;
        if ((k=='h'||k=='H') && bet>10) bet-=10;
        if ((k=='j'||k=='J') && bet+10<=bal) bet+=10;
        if (k==' ' && bal>=bet && bet>0) {
            bal-=bet;
            for (int sp=0;sp<8;sp++) {
                cg_hdr("Machine a sous", bal);
                cg_draw_sl(rand()%6, rand()%6, rand()%6, bet, bal, 1, "");
                gfx_swap_buffers();
                nop_delay(3500000);
            }
            r0=rand()%6; r1=rand()%6; r2=rand()%6;
            int win=0;
            if (r0==r1&&r1==r2) {
                if (r0==4) win=bet*100;      /* GRG */
                else if (r0==0) win=bet*50;  /* 777 */
                else if (r0==1) win=bet*20;  /* BAR */
                else if (r0==2) win=bet*10;  /* BEL */
                else win=bet*5;
            } else if (r0==r1||r1==r2||r0==r2) win=bet*2;
            if (win>0) { bal+=win; msg="VICTOIRE !"; }
            else       { msg="Pas de chance..."; }
        }
    }
    casino_balance=bal;
    if (bal>casino_best) casino_best=bal;
    hs_update(9, casino_best);
}

/* ── Casino C bridge (used by CasinoWindow / PokerWindow) ── */
int casino_get_balance(void) { return casino_balance; }

void casino_modify_balance(int delta) {
    casino_balance += delta;
    if (casino_balance < 0)          casino_balance = 0;
    if (casino_balance > casino_best) casino_best = casino_balance;
}

void launch_casino_game(int n)  /* 0=Blackjack 1=Roulette 2=Slots 3=Plinko */
{
    /* Bypass the GUI EventQueue: route IRQ1 directly to the legacy kb_buf
       so the blocking game loops can read keyboard input via get_monitor_char(). */
    is_gui_active = 0;
    switch (n) {
        case 0: cg_blackjack(); break;
        case 1: cg_roulette();  break;
        case 2: cg_slots();     break;
        case 3: gui_game_start(); start_plinko(); break;
    }
    /* Restore GUI routing, flush stale keys, repaint the desktop. */
    is_gui_active = 1;
    kb_inject_flush();
    draw_interface();
}

/* ── Main Casino GUI entry point ── */

/* ═══════════════════ GRAPHICAL GAMES APP ════════════════════════════
   Native gfx_-based game implementations.
   g_in_launcher=1 while active → draw_interface() won't flash the desktop.
   ═══════════════════════════════════════════════════════════════════ */

/* ── Common helpers ── */
#define GG_HDR_H  48
#define GG_CTL_H  50
#define GG_BG     GFX_RGB(0x04,0x04,0x10)
#define GG_BORDER GFX_RGB(0x22,0x44,0x88)

/* Game-area clear */
static void gg_clear(void) {
    gfx_fill_rect(0, GG_HDR_H, gfx_width(), gfx_height()-GG_HDR_H, GG_BG);
}
/* Game header bar */
static void gg_hdr(const char* title, int score, int hi) {
    int W=gfx_width();
    gfx_fill_rect(0, 0, W, GG_HDR_H, GFX_RGB(0x08,0x08,0x20));
    gfx_draw_hline(0, GG_HDR_H-1, W, GG_BORDER);
    gfx_draw_str(12, 10, title, GFX_RGB(0x88,0xCC,0xFF), GFX_RGB(0x08,0x08,0x20));
    char buf[32]; buf[0]='S'; buf[1]='c'; buf[2]='o'; buf[3]='r'; buf[4]='e'; buf[5]=':'; buf[6]=' ';
    itoa(score, buf+7); int p=strlen(buf);
    buf[p]=' '; buf[p+1]='H'; buf[p+2]='i'; buf[p+3]=':'; buf[p+4]=' '; itoa(hi,buf+p+5);
    gfx_draw_str(W/2-strlen(buf)*GFX_FONT_W/2, 10, buf, GFX_RGB(0xFF,0xD7,0x00), GFX_RGB(0x08,0x08,0x20));
    gfx_draw_str(W-12-11*GFX_FONT_W, 10, "[ESC] Menu",
                 GFX_RGB(0x55,0x77,0xAA), GFX_RGB(0x08,0x08,0x20));
}
/* Game-over overlay */
static void gg_gameover(const char* line1, const char* line2) {
    int W=gfx_width(), H=gfx_height();
    int bw=340, bh=80, bx=(W-bw)/2, by=(H-bh)/2;
    gfx_fill_rect(bx, by, bw, bh, GFX_RGB(0x10,0x10,0x30));
    gfx_draw_rect(bx, by, bw, bh, GFX_RGB(0xFF,0xD7,0x00));
    gfx_draw_str(bx+(bw-strlen(line1)*GFX_FONT_W)/2, by+10, line1, GFX_RGB(0xFF,0xD7,0x00), GFX_RGB(0x10,0x10,0x30));
    gfx_draw_str(bx+(bw-strlen(line2)*GFX_FONT_W)/2, by+34, line2, GFX_RGB(0xCC,0xFF,0xCC), GFX_RGB(0x10,0x10,0x30));
    gfx_draw_str(bx+(bw-23*GFX_FONT_W)/2, by+54, "[ESPACE] Rejouer  [ESC] Menu",
                 GFX_RGB(0x88,0x88,0xAA), GFX_RGB(0x10,0x10,0x30));
    gfx_swap_buffers();
}
/* Wait for SPACE or ESC after game over. Returns 1=replay, 0=exit. */
static int gg_wait_replay(void) {
    while (1) {
        int k; do { k=get_monitor_char(); nop_delay(50000); } while (!k);
        if (k==' ') return 1;
        if (k==0x1B) return 0;
    }
}

/* ──────────── SNAKE ──────────── */
#define GGS_W   38
#define GGS_H   26
#define GGS_CS  16
#define GGS_MAX 300
static void ggs_draw_board(int* sx, int* sy, int slen, int fx, int fy) {
    int W=gfx_width();
    int ox=(W-GGS_W*GGS_CS)/2, oy=GG_HDR_H+4;
    /* border */
    gfx_draw_rect(ox-2, oy-2, GGS_W*GGS_CS+4, GGS_H*GGS_CS+4, GG_BORDER);
    /* clear grid */
    gfx_fill_rect(ox, oy, GGS_W*GGS_CS, GGS_H*GGS_CS, GFX_RGB(0x08,0x10,0x08));
    /* food */
    gfx_fill_rect(ox+fx*GGS_CS+2, oy+fy*GGS_CS+2, GGS_CS-4, GGS_CS-4, GFX_RGB(0xFF,0x44,0x00));
    /* snake body */
    for (int i=slen-1;i>=0;i--) {
        unsigned int c = (i==0) ? GFX_RGB(0x44,0xFF,0x44) : GFX_RGB(0x10,0xAA,0x10);
        gfx_fill_rect(ox+sx[i]*GGS_CS+1, oy+sy[i]*GGS_CS+1, GGS_CS-2, GGS_CS-2, c);
    }
}
static void gg_snake(void) {
    static int sx[GGS_MAX], sy[GGS_MAX];
    int slen, dx, dy, ndx, ndy, fx, fy, score, hi=0, over, paused;
    unsigned long move_at;
restart:
    slen=4; dx=1; dy=0; ndx=1; ndy=0; score=0; over=0; paused=0;
    sx[0]=GGS_W/2; sy[0]=GGS_H/2;
    for (int i=1;i<slen;i++) { sx[i]=sx[0]-i; sy[i]=sy[0]; }
    fx=GGS_W/2+8; fy=GGS_H/2;
    move_at = jiffies + 12; /* start at ~120ms/step */
    gg_clear();
    while (!over) {
        /* Non-blocking input poll */
        int k = get_monitor_char();
        if (k==0x1B) return;
        if (k=='p'||k=='P') { paused=!paused; }
        if (!paused) {
            if (k==KEY_UP    &&dy!=1)  { ndx=0; ndy=-1; }
            if (k==KEY_DOWN  &&dy!=-1) { ndx=0; ndy=1;  }
            if (k==KEY_LEFT  &&dx!=1)  { ndx=-1;ndy=0;  }
            if (k==KEY_RIGHT &&dx!=-1) { ndx=1; ndy=0;  }
        }
        /* Move on jiffies timer — snake advances independently of input */
        if (!paused && (long)(jiffies - move_at) >= 0) {
            unsigned long speed = 12UL - (unsigned long)(score / 100);
            if ((long)speed < 4) speed = 4;
            move_at = jiffies + speed;
            for (int i=slen-1;i>0;i--) { sx[i]=sx[i-1]; sy[i]=sy[i-1]; }
            dx=ndx; dy=ndy; sx[0]+=dx; sy[0]+=dy;
            if (sx[0]<0||sx[0]>=GGS_W||sy[0]<0||sy[0]>=GGS_H) over=1;
            for (int i=1;i<slen&&!over;i++) if(sx[i]==sx[0]&&sy[i]==sy[0]) over=1;
            if (sx[0]==fx&&sy[0]==fy) {
                score+=10; if (slen<GGS_MAX-1) slen++;
                do { fx=rand()%GGS_W; fy=rand()%GGS_H; } while(fx==sx[0]&&fy==sy[0]);
            }
        }
        /* Draw at ~100fps (no added delay — jiffies controls game speed) */
        gg_hdr("Snake  [P]=pause  Fleches=direction", score, hi);
        ggs_draw_board(sx,sy,slen,fx,fy);
        gfx_swap_buffers();
        nop_delay(200000); /* ~0.2ms idle between polls */
    }
    if (score>hi) hi=score;
    char sc[16]; itoa(score, sc);
    gg_gameover("GAME OVER - Snake", sc);
    if (gg_wait_replay()) goto restart;
}

/* ──────────── TETRIS ──────────── */
#define GGT_CS  22   /* cell size */
#define GGT_W   TBOARD_W
#define GGT_H   TBOARD_H
static unsigned char ggt_board[GGT_H][GGT_W];
static const int ggt_pieces[7][4][2] = {
    {{0,1},{1,1},{2,1},{3,1}}, /* I */
    {{0,0},{0,1},{1,1},{2,1}}, /* J */
    {{2,0},{0,1},{1,1},{2,1}}, /* L */
    {{1,0},{2,0},{1,1},{2,1}}, /* O */
    {{1,0},{2,0},{0,1},{1,1}}, /* S */
    {{1,0},{0,1},{1,1},{2,1}}, /* T */
    {{0,0},{1,0},{1,1},{2,1}}, /* Z */
};
static unsigned int ggt_cols[8] = {
    GFX_RGB(0x00,0xEE,0xEE), /* I cyan */
    GFX_RGB(0x00,0x00,0xCC), /* J blue */
    GFX_RGB(0xFF,0x88,0x00), /* L orange */
    GFX_RGB(0xFF,0xFF,0x00), /* O yellow */
    GFX_RGB(0x00,0xCC,0x00), /* S green */
    GFX_RGB(0xAA,0x00,0xCC), /* T purple */
    GFX_RGB(0xCC,0x00,0x00), /* Z red */
    GFX_RGB(0x18,0x18,0x18), /* empty */
};
static void ggt_cell(int ox, int oy, int x, int y, int col) {
    gfx_fill_rect(ox+x*GGT_CS+1, oy+y*GGT_CS+1, GGT_CS-2, GGT_CS-2, ggt_cols[col]);
}
/* Matrix-based rotation helpers — piece is stored as 4 {col,row} offsets
   inside a 4×4 bounding box. CW: (x,y)→(3-y,x). CCW: (x,y)→(y,3-x). */
static void ggt_init_piece(int piece, int cur[4][2]) {
    for(int i=0;i<4;i++){cur[i][0]=ggt_pieces[piece][i][0];cur[i][1]=ggt_pieces[piece][i][1];}
}
static void ggt_rotate_cw(int cur[4][2]) {
    for(int i=0;i<4;i++){int x=cur[i][0],y=cur[i][1];cur[i][0]=3-y;cur[i][1]=x;}
}
static void ggt_rotate_ccw(int cur[4][2]) {
    for(int i=0;i<4;i++){int x=cur[i][0],y=cur[i][1];cur[i][0]=y;cur[i][1]=3-x;}
}
static int ggt_valid2(int cur[4][2], int pr, int pc) {
    for(int i=0;i<4;i++){
        int nx=cur[i][0]+pc, ny=cur[i][1]+pr;
        if(nx<0||nx>=GGT_W||ny>=GGT_H) return 0;
        if(ny>=0&&ggt_board[ny][nx]) return 0;
    }
    return 1;
}
static void ggt_stamp2(int cur[4][2], int pr, int pc, int col) {
    for(int i=0;i<4;i++){
        int nx=cur[i][0]+pc, ny=cur[i][1]+pr;
        if(ny>=0&&ny<GGT_H&&nx>=0&&nx<GGT_W) ggt_board[ny][nx]=(unsigned char)col;
    }
}
static int ggt_clear_lines(void) {
    int cleared=0;
    for (int y=GGT_H-1;y>=0;y--) {
        int full=1;
        for (int x=0;x<GGT_W;x++) if (!ggt_board[y][x]) { full=0; break; }
        if (full) {
            for (int r=y;r>0;r--) for (int x=0;x<GGT_W;x++) ggt_board[r][x]=ggt_board[r-1][x];
            for (int x=0;x<GGT_W;x++) ggt_board[0][x]=0;
            cleared++; y++;
        }
    }
    return cleared;
}
static void gg_tetris(void) {
    int W=gfx_width();
    int ox=(W-GGT_W*GGT_CS)/2, oy=GG_HDR_H+4;
    int score=0, hi=0, level=1, lines=0;
    /* cur/nxt declared before restart so goto doesn't jump over declarations */
    int cur[4][2], nxt[4][2];
restart:
    memset(ggt_board,0,sizeof(ggt_board));
    score=0; level=1; lines=0;
    int piece=rand()%7, nxt_piece=rand()%7, pr=0, pc=GGT_W/2-2, over=0;
    ggt_init_piece(piece, cur);
    ggt_init_piece(nxt_piece, nxt);
    unsigned long drop_at = jiffies + 20;
    gg_clear();
    while (!over) {
        int k=get_monitor_char();
        if (k==0x1B) return;
        /* Absolute movement — always LEFT=pc-1, RIGHT=pc+1, unaffected by rotation */
        if (k==KEY_LEFT  && ggt_valid2(cur,pr,pc-1)) pc--;
        if (k==KEY_RIGHT && ggt_valid2(cur,pr,pc+1)) pc++;
        /* CW rotation (UP arrow) */
        if (k==KEY_UP) {
            int tmp[4][2];
            for(int i=0;i<4;i++){tmp[i][0]=cur[i][0];tmp[i][1]=cur[i][1];}
            ggt_rotate_cw(tmp);
            if (ggt_valid2(tmp,pr,pc)) for(int i=0;i<4;i++){cur[i][0]=tmp[i][0];cur[i][1]=tmp[i][1];}
        }
        /* CCW rotation (Z or A) */
        if (k=='z'||k=='Z'||k=='a'||k=='A') {
            int tmp[4][2];
            for(int i=0;i<4;i++){tmp[i][0]=cur[i][0];tmp[i][1]=cur[i][1];}
            ggt_rotate_ccw(tmp);
            if (ggt_valid2(tmp,pr,pc)) for(int i=0;i<4;i++){cur[i][0]=tmp[i][0];cur[i][1]=tmp[i][1];}
        }
        if (k==KEY_DOWN && ggt_valid2(cur,pr+1,pc)) { pr++; drop_at=jiffies+2; }
        if (k==' ') { while(ggt_valid2(cur,pr+1,pc)) pr++; drop_at=0; }
        /* Gravity */
        if ((long)(jiffies - drop_at) >= 0) {
            int delay = 20 - (level-1)*2; if(delay<4) delay=4;
            drop_at = jiffies + (unsigned long)delay;
            if (ggt_valid2(cur,pr+1,pc)) {
                pr++;
            } else {
                ggt_stamp2(cur,pr,pc,piece+1);
                int cl=ggt_clear_lines();
                lines+=cl;
                int pts[5]={0,40,100,300,1200};
                score += pts[cl<5?cl:4]*level;
                level=1+lines/10;
                piece=nxt_piece; nxt_piece=rand()%7;
                pr=0; pc=GGT_W/2-2;
                ggt_init_piece(piece, cur);
                ggt_init_piece(nxt_piece, nxt);
                drop_at = jiffies + (unsigned long)(20-(level-1)*2 < 4 ? 4 : 20-(level-1)*2);
                if (!ggt_valid2(cur,pr,pc)) over=1;
            }
        }
        /* Draw board */
        gfx_fill_rect(ox-2, oy-2, GGT_W*GGT_CS+4, GGT_H*GGT_CS+4, GG_BORDER);
        gfx_fill_rect(ox, oy, GGT_W*GGT_CS, GGT_H*GGT_CS, GFX_RGB(0x08,0x08,0x08));
        for (int y=0;y<GGT_H;y++) for (int x=0;x<GGT_W;x++) ggt_cell(ox,oy,x,y,ggt_board[y][x]?ggt_board[y][x]-1:7);
        /* Draw active piece using absolute matrix coordinates */
        for (int i=0;i<4;i++) {
            int nx=cur[i][0]+pc, ny=cur[i][1]+pr;
            if (ny>=0) ggt_cell(ox,oy,nx,ny,piece);
        }
        /* Side panel */
        int px=ox+GGT_W*GGT_CS+12, py=oy+10;
        gfx_fill_rect(px, py, 6*GGT_CS, 120, GFX_RGB(0x0C,0x0C,0x20));
        gfx_draw_rect(px, py, 6*GGT_CS, 120, GG_BORDER);
        gfx_draw_str(px+8, py+8, "NEXT", GFX_RGB(0xAA,0xAA,0xFF), GFX_RGB(0x0C,0x0C,0x20));
        for (int i=0;i<4;i++) {
            int dx=ggt_pieces[nxt_piece][i][0], dy=ggt_pieces[nxt_piece][i][1];
            gfx_fill_rect(px+8+(dx+1)*GGT_CS, py+30+dy*GGT_CS, GGT_CS-2, GGT_CS-2, ggt_cols[nxt_piece]);
        }
        gfx_draw_str(px+4, py+130, "Lvl:", GFX_RGB(0xAA,0xFF,0xAA), GG_BG); char ls[16]; itoa(level,ls); gfx_draw_str(px+36,py+130,ls,GFX_RGB(0xFF,0xFF,0xFF),GG_BG);
        gfx_draw_str(px+4, py+148, "Lig:", GFX_RGB(0xAA,0xFF,0xAA), GG_BG); itoa(lines,ls); gfx_draw_str(px+36,py+148,ls,GFX_RGB(0xFF,0xFF,0xFF),GG_BG);
        gg_hdr("Tetris  UP=CW  Z/A=CCW  SPC=drop", score, hi);
        gfx_swap_buffers();
        nop_delay(2000000);
    }
    if (score>hi) hi=score;
    char sc[16]; itoa(score,sc);
    gg_gameover("GAME OVER - Tetris", sc);
    if (gg_wait_replay()) goto restart;
}

/* ──────────── PONG ──────────── */
static void gg_pong(void) {
    int W=gfx_width(), H=gfx_height();
    int gw=W-80, gh=H-GG_HDR_H-60;
    int ox=40, oy=GG_HDR_H+10;
    int padw=12, padh=70;
    int p1y=(gh-padh)/2, p2y=(gh-padh)/2;
    int bx=gw/2, by=gh/2, bdx=3, bdy=2;
    int bsz=10;
    int sc1=0, sc2=0, hi=0;
restart:
    sc1=0; sc2=0; p1y=p2y=(gh-padh)/2;
    bx=gw/2; by=gh/2; bdx=3; bdy=2;
    gg_clear();
    while (1) {
        int k=get_monitor_char();
        if (k==0x1B) return;
        if (k=='z'||k=='Z'||k==KEY_UP)    p1y-=20;
        if (k=='s'||k=='S'||k==KEY_DOWN)   p1y+=20;
        if (p1y<0) p1y=0;
        if (p1y+padh>gh) p1y=gh-padh;
        /* AI p2 */
        if (p2y+padh/2 < by) p2y+=4;
        else if (p2y+padh/2 > by) p2y-=4;
        if (p2y<0) p2y=0;
        if (p2y+padh>gh) p2y=gh-padh;
        /* ball */
        bx+=bdx; by+=bdy;
        if (by<=0||by>=gh-bsz) bdy=-bdy;
        /* p1 collision */
        if (bx<=padw && by+bsz>=p1y && by<=p1y+padh) { bdx=(bdx<0?-bdx:bdx); bx=padw+1; }
        /* p2 collision */
        if (bx+bsz>=gw-padw && by+bsz>=p2y && by<=p2y+padh) { bdx=-(bdx<0?-bdx:bdx); bx=gw-padw-bsz-1; }
        /* score */
        if (bx<0) { sc2++; bx=gw/2; by=gh/2; bdx=3; bdy=2; }
        if (bx>gw) { sc1++; bx=gw/2; by=gh/2; bdx=-3; bdy=2; }
        if (sc1>=7||sc2>=7) break;
        /* draw */
        gfx_fill_rect(ox, oy, gw, gh, GFX_RGB(0x06,0x06,0x06));
        gfx_draw_rect(ox, oy, gw, gh, GG_BORDER);
        /* net */
        for (int y=0;y<gh;y+=16) gfx_fill_rect(ox+gw/2-1, oy+y, 2, 8, GFX_RGB(0x44,0x44,0x44));
        /* paddles */
        gfx_fill_rect(ox+2,         oy+p1y, padw, padh, GFX_RGB(0x44,0xAA,0xFF));
        gfx_fill_rect(ox+gw-padw-2, oy+p2y, padw, padh, GFX_RGB(0xFF,0x88,0x44));
        /* ball */
        gfx_fill_rect(ox+bx, oy+by, bsz, bsz, GFX_RGB(0xFF,0xFF,0xFF));
        /* score */
        char s1[4], s2[4]; itoa(sc1,s1); itoa(sc2,s2);
        gfx_draw_str(ox+gw/2-40, oy+10, s1, GFX_RGB(0xFF,0xFF,0xFF), GFX_RGB(0x06,0x06,0x06));
        gfx_draw_str(ox+gw/2+24, oy+10, s2, GFX_RGB(0xFF,0xFF,0xFF), GFX_RGB(0x06,0x06,0x06));
        gg_hdr("Pong  [Z/S]=gauche  [UP/DN]=droite", sc1*10, hi);
        gfx_swap_buffers();
        nop_delay(3500000);
    }
    int winner = sc1>=7 ? 1 : 2;
    char ws[24]; itoa(winner,ws); /* repurpose */
    if (sc1>=7) { gg_gameover("VICTOIRE ! Joueur 1", "Premier a 7 !"); }
    else         { gg_gameover("Victoire AI Joueur 2", "Premier a 7 !"); }
    if (sc1>hi) hi=sc1;
    if (sc2>hi) hi=sc2;
    if (gg_wait_replay()) goto restart;
}

/* ──────────── BREAKOUT ──────────── */
#define GGB_ROWS  6
#define GGB_COLS 14
#define GGB_CS   44
#define GGB_CH   14
static void gg_breakout(void) {
    int W=gfx_width(), H=gfx_height();
    int ox=(W-GGB_COLS*GGB_CS)/2, oy=GG_HDR_H+10;
    unsigned char bricks[GGB_ROWS][GGB_COLS];
    int score=0, hi=0, lives=3;
restart:
    for (int r=0;r<GGB_ROWS;r++) for (int c=0;c<GGB_COLS;c++) bricks[r][c]=1;
    int alive=GGB_ROWS*GGB_COLS, score2=0;
    int padx=(W-80)/2, padw=80, pady=H-GG_HDR_H-80, padh=10;
    float bx=(float)W/2, by=(float)(pady-20), bvx=2.5f, bvy=-3.0f;
    int bsz=8;
    score=score2; score=0;
    gg_clear();
    while (alive>0&&lives>0) {
        int k=get_monitor_char();
        if (k==0x1B) return;
        if (k==KEY_LEFT) { padx-=20; if(padx<ox) padx=ox; }
        if (k==KEY_RIGHT){ padx+=20; if(padx+padw>ox+GGB_COLS*GGB_CS) padx=ox+GGB_COLS*GGB_CS-padw; }
        bx+=bvx; by+=bvy;
        /* walls */
        if ((int)bx<=ox) { bx=(float)ox+1; bvx=bvx<0?-bvx:bvx; }
        if ((int)bx>=ox+GGB_COLS*GGB_CS-bsz) { bx=(float)(ox+GGB_COLS*GGB_CS-bsz-1); bvx=bvx>0?-bvx:bvx; }
        if ((int)by<=GG_HDR_H) { by=(float)(GG_HDR_H+1); bvy=bvy<0?-bvy:bvy; }
        /* paddle */
        if ((int)by+bsz>=pady&&(int)by<pady+padh&&(int)bx+bsz>=padx&&(int)bx<=padx+padw) {
            { int iv=(int)bvy; bvy=(float)(iv<0?iv:-iv); } bvx+=((bx+bsz/2)-(padx+padw/2))*0.04f;
        }
        /* floor */
        if ((int)by>H) { lives--; bx=(float)W/2; by=(float)(pady-20); bvx=2.5f; bvy=-3.0f; nop_delay(50000000); }
        /* brick collision — 4-corner AABB */
        { int corners_x[4]={(int)bx,(int)bx+bsz-1,(int)bx,(int)bx+bsz-1};
          int corners_y[4]={(int)by,(int)by,(int)by+bsz-1,(int)by+bsz-1};
          for (int ki=0;ki<4;ki++) {
            if (corners_y[ki]<oy || corners_x[ki]<ox) continue;
            int br=(corners_y[ki]-oy)/GGB_CH, bc=(corners_x[ki]-ox)/GGB_CS;
            if (br>=0&&br<GGB_ROWS&&bc>=0&&bc<GGB_COLS&&bricks[br][bc]) {
                bricks[br][bc]=0; alive--; score+=10*(GGB_ROWS-br+1); bvy=-bvy; break;
            }
          }
        }
        /* draw */
        gfx_fill_rect(ox, oy, GGB_COLS*GGB_CS, GGB_ROWS*GGB_CH+H, GFX_RGB(0x04,0x04,0x10));
        static unsigned int bcols[6]={
            GFX_RGB(0xFF,0x22,0x22),GFX_RGB(0xFF,0x88,0x22),GFX_RGB(0xFF,0xFF,0x22),
            GFX_RGB(0x22,0xFF,0x22),GFX_RGB(0x22,0x88,0xFF),GFX_RGB(0xCC,0x22,0xFF)};
        for (int r=0;r<GGB_ROWS;r++) for (int c=0;c<GGB_COLS;c++) if(bricks[r][c])
            gfx_fill_rect(ox+c*GGB_CS+1, oy+r*GGB_CH+1, GGB_CS-2, GGB_CH-2, bcols[r]);
        /* paddle + ball */
        gfx_fill_rect(padx, pady, padw, padh, GFX_RGB(0x44,0xAA,0xFF));
        gfx_fill_rect((int)bx, (int)by, bsz, bsz, GFX_RGB(0xFF,0xFF,0xFF));
        /* lives */
        char ls[8]; itoa(lives,ls); gfx_draw_str(16, oy+10, "Vies:", GFX_RGB(0xFF,0xAA,0x00), GG_BG); gfx_draw_str(56,oy+10,ls,GFX_RGB(0xFF,0xFF,0xFF),GG_BG);
        gg_hdr("Breakout  Fleches=bouger", score, hi);
        gfx_swap_buffers();
        nop_delay(2000000);
    }
    if (score>hi) hi=score;
    char sc[16]; itoa(score,sc);
    gg_gameover(alive==0?"GAGNE ! Tous les briques !":"PERDU - Plus de vies", sc);
    if (gg_wait_replay()) goto restart;
}

/* ──────────── SPACE INVADERS (rewrite) ──────────── */
#define GGI_ROWS   5    /* 5 rows: 1 squid + 2 crabs + 2 octopus */
#define GGI_COLS  11    /* 11 columns — classic layout */
#define GGI_STEP  44    /* px between alien top-left corners */
#define GGI_SZ    32    /* sprite draw size: 8 bits × 4 px/bit */
#define GGI_SCALE  4    /* pixels per sprite dot */

/* Classic Space Invaders 8×8 sprites, 2 animation frames each.
   Bit 7 = leftmost pixel, bit 0 = rightmost.                    */
static const unsigned char ggi_spr[3][2][8] = {
    /* Type 0 – Squid (top row) – 30 pts – magenta */
    {{ 0x18,0x3C,0x7E,0xDB,0xFF,0x24,0x5A,0xA5 },
     { 0x18,0x3C,0x7E,0xDB,0xFF,0x24,0xA5,0x42 }},
    /* Type 1 – Crab (rows 1-2) – 20 pts – green */
    {{ 0x42,0xC3,0xBD,0x66,0xFF,0xDB,0x99,0x66 },
     { 0xC2,0xC3,0xBD,0xE6,0xFF,0xDB,0xA5,0x2C }},
    /* Type 2 – Octopus (rows 3-4) – 10 pts – cyan */
    {{ 0x3C,0x7E,0xFF,0xDB,0xFF,0x5A,0x81,0x42 },
     { 0x3C,0x7E,0xFF,0xDB,0xFF,0x5A,0x42,0xA5 }},
};
static const unsigned int ggi_col[3] = {
    GFX_RGB(0xEE,0x44,0xFF), /* squid: magenta */
    GFX_RGB(0x44,0xFF,0x44), /* crab:  green   */
    GFX_RGB(0x44,0xCC,0xFF), /* octo:  cyan    */
};
static int ggi_type(int row) {
    return (row==0)?0:(row<=2)?1:2;
}
static void ggi_draw_spr(int ix, int iy, int type, int frame) {
    const unsigned char* spr = ggi_spr[type][frame];
    unsigned int col = ggi_col[type];
    for (int row=0;row<8;row++) {
        for (int bit=0;bit<8;bit++) {
            if (spr[row] & (0x80u>>bit))
                gfx_fill_rect(ix+bit*GGI_SCALE, iy+row*GGI_SCALE,
                              GGI_SCALE, GGI_SCALE, col);
        }
    }
}
/* UFO saucer: simple rectangle with portholes */
static void ggi_draw_ufo(int ux, int uy) {
    gfx_fill_rect(ux,    uy+6,  52, 10, GFX_RGB(0xFF,0x22,0x22));
    gfx_fill_rect(ux+10, uy,    32, 8,  GFX_RGB(0xFF,0x22,0x22));
    gfx_fill_rect(ux+6,  uy+4,  8,  6,  GFX_RGB(0xFF,0xAA,0xAA));
    gfx_fill_rect(ux+22, uy+4,  8,  6,  GFX_RGB(0xFF,0xAA,0xAA));
    gfx_fill_rect(ux+38, uy+4,  8,  6,  GFX_RGB(0xFF,0xAA,0xAA));
}
/* Player cannon */
static void ggi_draw_player(int px, int py) {
    gfx_fill_rect(px,    py+12, 32, 10, GFX_RGB(0x44,0xCC,0xFF));
    gfx_fill_rect(px+8,  py+5,  16, 9,  GFX_RGB(0x44,0xCC,0xFF));
    gfx_fill_rect(px+14, py,    4,  7,  GFX_RGB(0x44,0xCC,0xFF));
}
static void gg_invaders(void) {
    int W=gfx_width(), H=gfx_height();
    /* Formation origin relative to grid top-left */
    int base_ox = (W - GGI_COLS*GGI_STEP)/2;
    int base_oy = GG_HDR_H + 28;
    int form_x=0, form_y=0, gdir=1;
    int inv[GGI_ROWS][GGI_COLS];
    int alive = GGI_ROWS*GGI_COLS;
    int score=0, hi=0, lives=3;
    int px = W/2 - 16; /* player x */
    int py = H - 90;   /* player y */
    int pbx=-1, pby=-1; /* player bullet */
    int ebx[3], eby[3]; /* enemy bullets */
    /* UFO */
    int ux=-60, udir=1, uactive=0;
    unsigned long next_ufo;
restart:
    for(int r=0;r<GGI_ROWS;r++) for(int c=0;c<GGI_COLS;c++) inv[r][c]=1;
    alive=GGI_ROWS*GGI_COLS; score=0;
    form_x=0; form_y=0; gdir=1;
    px=W/2-16; pbx=pby=-1;
    for(int i=0;i<3;i++) ebx[i]=eby[i]=-1;
    uactive=0; ux=-60; udir=1;
    unsigned long next_move  = jiffies+20;
    unsigned long next_fire  = jiffies+80;
    unsigned long next_blt   = jiffies;
    unsigned long anim_tick  = jiffies;
    next_ufo = jiffies + 300 + (unsigned long)(rand()%200);
    int anim_frame=0;
    gg_clear();
    while (alive>0 && lives>0) {
        unsigned long now = jiffies;
        /* ── Input ── */
        int k = get_monitor_char();
        if (k==0x1B) return;
        if (k==KEY_LEFT)  { px-=14; if(px<20) px=20; }
        if (k==KEY_RIGHT) { px+=14; if(px>W-52) px=W-52; }
        if (k==' ' && pbx<0) { pbx=px+14; pby=py-4; }
        /* ── Animation frame ── */
        if (now-anim_tick >= 20) { anim_frame^=1; anim_tick=now; }
        /* ── Formation move ── */
        if ((long)(now-next_move)>=0) {
            /* Speed: faster as aliens die */
            int total=GGI_ROWS*GGI_COLS;
            int speed = 20-(total-alive)*16/total;
            if(speed<3) speed=3;
            next_move = now+(unsigned long)speed;
            form_x += gdir*3;
            /* Find leftmost / rightmost alive column */
            int lc=GGI_COLS, rc=-1;
            for(int r=0;r<GGI_ROWS;r++) for(int c=0;c<GGI_COLS;c++) if(inv[r][c]){
                if(c<lc) lc=c;
                if(c>rc) rc=c;
            }
            int abs_l = base_ox+form_x+lc*GGI_STEP;
            int abs_r = base_ox+form_x+rc*GGI_STEP+GGI_SZ;
            if (abs_r>=W-20 || abs_l<=20) {
                form_y+=12;
                gdir=-gdir;
                form_x+=gdir*4;
            }
        }
        /* ── Enemy fire ── */
        if ((long)(now-next_fire)>=0) {
            next_fire = now+40+(unsigned long)(rand()%80);
            int slot=-1;
            for(int i=0;i<3;i++) if(eby[i]<0){slot=i;break;}
            if (slot>=0) {
                /* Fire from random bottom-most alien per column */
                int cols[GGI_COLS], nc=0;
                for(int c=0;c<GGI_COLS;c++)
                    for(int r=GGI_ROWS-1;r>=0;r--) if(inv[r][c]){cols[nc++]=c;break;}
                if (nc>0) {
                    int cc=cols[rand()%nc];
                    for(int r=GGI_ROWS-1;r>=0;r--) if(inv[r][cc]) {
                        ebx[slot]=base_ox+form_x+cc*GGI_STEP+GGI_SZ/2;
                        eby[slot]=base_oy+form_y+r*GGI_STEP+GGI_SZ+2;
                        break;
                    }
                }
            }
        }
        /* ── Bullet physics ── */
        if ((long)(now-next_blt)>=0) {
            next_blt=now+1;
            /* Player bullet */
            if (pby>=0) {
                pby-=8;
                if (pby<GG_HDR_H) { pbx=pby=-1; }
                else {
                    for(int r=0;r<GGI_ROWS&&pbx>=0;r++) for(int c=0;c<GGI_COLS&&pbx>=0;c++) {
                        if(!inv[r][c]) continue;
                        int ax=base_ox+form_x+c*GGI_STEP, ay=base_oy+form_y+r*GGI_STEP;
                        if(pbx>=ax && pbx<=ax+GGI_SZ && pby>=ay && pby<=ay+GGI_SZ) {
                            inv[r][c]=0; alive--;
                            int pts[3]={30,20,10};
                            score+=pts[ggi_type(r)];
                            pbx=pby=-1;
                        }
                    }
                    /* UFO hit? (drawn at y=GG_HDR_H+4, height=16) */
                    if (uactive && pbx>=ux && pbx<=ux+52 && pby>=GG_HDR_H+4 && pby<=GG_HDR_H+20) {
                        score+=200; uactive=0; pbx=pby=-1;
                    }
                }
            }
            /* Enemy bullets */
            for(int i=0;i<3;i++) {
                if(eby[i]<0) continue;
                eby[i]+=5;
                if(eby[i]>H) { eby[i]=-1; continue; }
                if(ebx[i]>=px && ebx[i]<=px+32 && eby[i]>=py && eby[i]<=py+22) {
                    eby[i]=-1; lives--;
                    gfx_fill_rect(0,GG_HDR_H,W,H-GG_HDR_H,GFX_RGB(0x40,0x00,0x00));
                    nop_delay(60000000);
                    px=W/2-16; pbx=pby=-1;
                    for(int j=0;j<3;j++) eby[j]=-1;
                }
            }
        }
        /* ── UFO ── */
        if (!uactive && (long)(now-next_ufo)>=0) {
            uactive=1; udir=(rand()%2)?1:-1;
            ux = (udir>0)?-56:W+4;
        }
        if (uactive) {
            ux+=udir*2;
            if (ux>W+10 || ux<-60) { uactive=0; next_ufo=now+300+(unsigned long)(rand()%300); }
        }
        /* ── Aliens reach bottom? ── */
        {
            int lr=0;
            for(int r=GGI_ROWS-1;r>=0;r--) for(int c=0;c<GGI_COLS;c++) if(inv[r][c]){if(r>lr)lr=r;}
            if (base_oy+form_y+lr*GGI_STEP+GGI_SZ > py-8) lives=0;
        }
        /* ── Draw ── */
        gfx_fill_rect(0, GG_HDR_H, W, H-GG_HDR_H, GG_BG);
        /* Ground line */
        gfx_draw_hline(0, py+23, W, GFX_RGB(0x44,0xCC,0xFF));
        /* Aliens */
        for(int r=0;r<GGI_ROWS;r++) for(int c=0;c<GGI_COLS;c++) {
            if(!inv[r][c]) continue;
            ggi_draw_spr(base_ox+form_x+c*GGI_STEP,
                         base_oy+form_y+r*GGI_STEP,
                         ggi_type(r), anim_frame);
        }
        /* UFO */
        if (uactive) ggi_draw_ufo(ux, GG_HDR_H+4);
        /* Player */
        ggi_draw_player(px, py);
        /* Player bullet */
        if (pby>=0) gfx_fill_rect(pbx-1, pby, 3, 12, GFX_RGB(0xFF,0xFF,0x44));
        /* Enemy bullets */
        for(int i=0;i<3;i++) if(eby[i]>=0) {
            gfx_fill_rect(ebx[i]-1, eby[i],   2, 6, GFX_RGB(0xFF,0x55,0x55));
            gfx_fill_rect(ebx[i]+1, eby[i]+6, 2, 6, GFX_RGB(0xFF,0x55,0x55));
        }
        /* Lives */
        char lb[16]; lb[0]='V'; lb[1]='i'; lb[2]='e'; lb[3]='s'; lb[4]=':'; lb[5]=' '; (void)lb;
        for(int i=0;i<lives&&i<5;i++) {
            lb[6]='\0';
            ggi_draw_player(W-90+(i*38), py+2);
        }
        gg_hdr("Space Invaders   < > = move   ESPACE = feu", score, hi);
        gfx_swap_buffers();
        nop_delay(300000);
    }
    if (score>hi) hi=score;
    char sc[16]; itoa(score,sc);
    gg_gameover(alive==0?"VICTOIRE ! La Terre est sauvee !":"GAME OVER - Invasion !", sc);
    if (gg_wait_replay()) goto restart;
}

/* ──────────── 2048 ──────────── */
#define GG48_SZ 4
#define GG48_CS 110
static void gg_2048(void) {
    int W=gfx_width(); (void)gfx_height();
    int ox=(W-GG48_SZ*GG48_CS)/2, oy=GG_HDR_H+10;
    int board[GG48_SZ][GG48_SZ];
    int score=0, hi=0;
    static unsigned int t48_bg[12]={
        GFX_RGB(0xCC,0xC0,0xB4),GFX_RGB(0xED,0xE0,0xC8),GFX_RGB(0xED,0xC5,0x8B),
        GFX_RGB(0xF5,0x96,0x63),GFX_RGB(0xF5,0x72,0x3C),GFX_RGB(0xF5,0x60,0x3C),
        GFX_RGB(0xF5,0x50,0x20),GFX_RGB(0xED,0xD0,0x73),GFX_RGB(0xED,0xCC,0x62),
        GFX_RGB(0xED,0xC8,0x50),GFX_RGB(0xED,0xC4,0x40),GFX_RGB(0x3C,0x3A,0x32),
    };
    auto void spawn48(void); void spawn48(void) {
        int empties=0;
        for (int r=0;r<GG48_SZ;r++) for (int c=0;c<GG48_SZ;c++) if(!board[r][c]) empties++;
        if (!empties) return;
        int pick=rand()%empties;
        for (int r=0;r<GG48_SZ;r++) for (int c=0;c<GG48_SZ;c++) if(!board[r][c]&&!pick--) { board[r][c]=(rand()%10<9)?2:4; return; }
    }
    auto void draw48(void); void draw48(void) {
        gfx_fill_rect(ox-6, oy-6, GG48_SZ*GG48_CS+12, GG48_SZ*GG48_CS+12, GFX_RGB(0x88,0x80,0x78));
        for (int r=0;r<GG48_SZ;r++) for (int c=0;c<GG48_SZ;c++) {
            int v=board[r][c];
            int idx=0; if(v) { int n=v; while(n>2){n/=2;idx++;} if(idx>11)idx=11; }
            unsigned int bg = v?t48_bg[idx]:GFX_RGB(0xCC,0xC0,0xB4);
            gfx_fill_rect(ox+c*GG48_CS+3, oy+r*GG48_CS+3, GG48_CS-6, GG48_CS-6, bg);
            if (v) {
                char vs[16]; itoa(v,vs);
                int vl=strlen(vs);
                gfx_draw_str(ox+c*GG48_CS+(GG48_CS-vl*GFX_FONT_W)/2,
                             oy+r*GG48_CS+(GG48_CS-GFX_FONT_H)/2,
                             vs, idx<3?GFX_RGB(0x77,0x6E,0x65):GFX_RGB(0xFF,0xFF,0xFF), bg);
            }
        }
    }
restart:
    memset(board,0,sizeof(board)); score=0;
    spawn48(); spawn48();
    gg_clear();
    while (1) {
        draw48(); gg_hdr("2048  Fleches=bouger", score, hi);
        gfx_swap_buffers();
        int k; do { k=get_monitor_char(); nop_delay(50000); } while(!k);
        if (k==0x1B) return;
        int moved=0;
        auto void slide(int* row); void slide(int* row) {
            /* compact non-zero left, then merge */
            int buf[GG48_SZ]={0}; int bi=0;
            for(int i=0;i<GG48_SZ;i++) if(row[i]) buf[bi++]=row[i];
            for(int i=0;i<GG48_SZ-1;i++) if(buf[i]&&buf[i]==buf[i+1]){buf[i]*=2;score+=buf[i];buf[i+1]=0;}
            memset(row,0,GG48_SZ*sizeof(int)); bi=0;
            for(int i=0;i<GG48_SZ;i++) if(buf[i]) row[bi++]=buf[i];
        }
        int save[GG48_SZ][GG48_SZ];
        for(int r=0;r<GG48_SZ;r++) for(int c=0;c<GG48_SZ;c++) save[r][c]=board[r][c];
        if (k==KEY_LEFT) for(int r=0;r<GG48_SZ;r++) slide(board[r]);
        else if (k==KEY_RIGHT) { for(int r=0;r<GG48_SZ;r++){int row[GG48_SZ];for(int c=0;c<GG48_SZ;c++)row[c]=board[r][GG48_SZ-1-c];slide(row);for(int c=0;c<GG48_SZ;c++)board[r][GG48_SZ-1-c]=row[c];} }
        else if (k==KEY_UP) { for(int c=0;c<GG48_SZ;c++){int col[GG48_SZ];for(int r=0;r<GG48_SZ;r++)col[r]=board[r][c];slide(col);for(int r=0;r<GG48_SZ;r++)board[r][c]=col[r];} }
        else if (k==KEY_DOWN) { for(int c=0;c<GG48_SZ;c++){int col[GG48_SZ];for(int r=0;r<GG48_SZ;r++)col[r]=board[GG48_SZ-1-r][c];slide(col);for(int r=0;r<GG48_SZ;r++)board[GG48_SZ-1-r][c]=col[r];} }
        { int diff=0; for(int r=0;r<GG48_SZ&&!diff;r++) for(int c=0;c<GG48_SZ;c++) if(save[r][c]!=board[r][c]){diff=1;break;}
          if(diff) { moved=1; spawn48(); } }
        /* check game over */
        int can_move=0;
        for(int r=0;r<GG48_SZ&&!can_move;r++) for(int c=0;c<GG48_SZ&&!can_move;c++) {
            if(!board[r][c]) { can_move=1; break; }
            if(r+1<GG48_SZ&&board[r+1][c]==board[r][c]) can_move=1;
            if(c+1<GG48_SZ&&board[r][c+1]==board[r][c]) can_move=1;
        }
        if (!can_move) {
            if (score>hi) hi=score;
            char sc[16]; itoa(score,sc);
            gg_gameover("GAME OVER - 2048", sc);
            if (gg_wait_replay()) goto restart; else return;
        }
        (void)moved;
    }
}

/* ──────────── SIMON ──────────── */
static void gg_simon(void) {
    int W=gfx_width(), H=gfx_height();
    int cx=W/2, cy=(H+GG_HDR_H)/2;
    int qw=(W/2-60), qh=((H-GG_HDR_H)/2-40);
    /* 4 quadrant top-left corners */
    int qx[4]={cx-qw-4, cx+4, cx-qw-4, cx+4};
    int qy[4]={GG_HDR_H+20, GG_HDR_H+20, cy+4, cy+4};
    unsigned int qcol[4]={GFX_RGB(0xDD,0x00,0x00),GFX_RGB(0x00,0xCC,0x00),GFX_RGB(0x00,0x00,0xDD),GFX_RGB(0xDD,0xCC,0x00)};
    unsigned int qhi[4]={GFX_RGB(0xFF,0x66,0x66),GFX_RGB(0x66,0xFF,0x66),GFX_RGB(0x66,0x66,0xFF),GFX_RGB(0xFF,0xFF,0x66)};
    char qkey[4]={'1','2','3','4'};
    int seq[200]; int slen=0, score=0, hi=0;
    auto void draw_all(int lit); void draw_all(int lit) {
        gfx_fill_rect(0, GG_HDR_H, W, H-GG_HDR_H, GG_BG);
        gfx_fill_rect(cx-2, GG_HDR_H, 4, H-GG_HDR_H, GFX_RGB(0x20,0x20,0x20));
        gfx_fill_rect(0, cy, W, 4, GFX_RGB(0x20,0x20,0x20));
        for (int i=0;i<4;i++) {
            gfx_fill_rect(qx[i], qy[i], qw, qh, lit==i?qhi[i]:qcol[i]);
            gfx_draw_rect(qx[i], qy[i], qw, qh, GFX_RGB(0x10,0x10,0x10));
            char ks[3]={'[',qkey[i],']'}; char ksz[4]={ks[0],ks[1],ks[2],0};
            gfx_draw_str(qx[i]+qw/2-16, qy[i]+qh/2-8, ksz, GFX_RGB(0xFF,0xFF,0xFF), lit==i?qhi[i]:qcol[i]);
        }
        gfx_swap_buffers();
    }
restart:
    slen=0; score=0;
    seq[slen++]=rand()%4;
    gg_clear();
    while (1) {
        gg_hdr("Simon  [1][2][3][4]=couleurs", score, hi);
        /* play sequence */
        for (int i=0;i<slen;i++) {
            draw_all(-1); nop_delay(20000000);
            draw_all(seq[i]); nop_delay(40000000);
            draw_all(-1); nop_delay(10000000);
        }
        /* player input */
        int ok=1;
        for (int i=0;i<slen&&ok;i++) {
            draw_all(-1); gg_hdr("Simon - Repetez !", score, hi);
            int k; do { k=get_monitor_char(); nop_delay(50000); } while(!k);
            if (k==0x1B) return;
            int q=-1;
            for (int j=0;j<4;j++) if (k==qkey[j]) q=j;
            if (q<0) { ok=0; break; }
            draw_all(q); nop_delay(25000000);
            if (q!=seq[i]) { ok=0; }
        }
        if (!ok) {
            if (score>hi) hi=score;
            char sc[16]; itoa(score,sc);
            gg_gameover("Erreur - Simon", sc);
            if (gg_wait_replay()) goto restart; else return;
        }
        score++; seq[slen++]=rand()%4;
        if (slen>=200) slen=199;
    }
}

/* ──────────── MINESWEEPER ──────────── */
#define GGM_W  16
#define GGM_H  12
#define GGM_MINES 24
#define GGM_CS 36
static void gg_minesweeper(void) {
    int W=gfx_width();
    int ox=(W-GGM_W*GGM_CS)/2, oy=GG_HDR_H+10;
    unsigned char mine[GGM_H][GGM_W], rev[GGM_H][GGM_W], flag[GGM_H][GGM_W], adj[GGM_H][GGM_W];
    int cx=GGM_W/2, cy=GGM_H/2, first=1, flags_left=GGM_MINES;
restart:
    memset(mine,0,sizeof(mine)); memset(rev,0,sizeof(rev));
    memset(flag,0,sizeof(flag)); memset(adj,0,sizeof(adj));
    cx=GGM_W/2; cy=GGM_H/2; first=1; flags_left=GGM_MINES;
    int score=0;
    gg_clear();
    while (1) {
        /* draw grid */
        for (int r=0;r<GGM_H;r++) for (int c=0;c<GGM_W;c++) {
            int px=ox+c*GGM_CS, py=oy+r*GGM_CS;
            int sel=(c==cx&&r==cy);
            unsigned int bg;
            if (rev[r][c]) {
                bg = mine[r][c] ? GFX_RGB(0xFF,0x22,0x22) : GFX_RGB(0xCC,0xCC,0xCC);
            } else {
                bg = sel ? GFX_RGB(0x44,0x88,0xCC) : GFX_RGB(0x44,0x66,0xAA);
            }
            if (flag[r][c]) bg=GFX_RGB(0xFF,0xAA,0x00);
            gfx_fill_rect(px+1, py+1, GGM_CS-2, GGM_CS-2, bg);
            gfx_draw_rect(px, py, GGM_CS, GGM_CS, GFX_RGB(0x22,0x22,0x44));
            if (rev[r][c] && adj[r][c] && !mine[r][c]) {
                char ns[2]={'0'+adj[r][c],0};
                static unsigned int nc[9]={0,GFX_RGB(0x00,0x00,0xFF),GFX_RGB(0x00,0x88,0x00),GFX_RGB(0xFF,0x00,0x00),GFX_RGB(0x00,0x00,0x88),GFX_RGB(0x88,0x00,0x00),GFX_RGB(0x00,0x88,0x88),GFX_RGB(0x00,0x00,0x00),GFX_RGB(0x44,0x44,0x44)};
                gfx_draw_str(px+(GGM_CS-GFX_FONT_W)/2, py+(GGM_CS-GFX_FONT_H)/2, ns, nc[adj[r][c]], bg);
            }
            if (flag[r][c]) gfx_draw_str(px+(GGM_CS-GFX_FONT_W)/2, py+(GGM_CS-GFX_FONT_H)/2, "F", GFX_RGB(0xFF,0x00,0x00), bg);
        }
        gg_hdr("Minesweeper  Fleches=move ESPACE=rev F=flag", score, 0);
        gfx_swap_buffers();
        int k; do { k=get_monitor_char(); nop_delay(50000); } while(!k);
        if (k==0x1B) return;
        if (k==KEY_LEFT && cx>0) cx--;
        if (k==KEY_RIGHT && cx<GGM_W-1) cx++;
        if (k==KEY_UP && cy>0) cy--;
        if (k==KEY_DOWN && cy<GGM_H-1) cy++;
        if ((k=='f'||k=='F') && !rev[cy][cx]) {
            if (!flag[cy][cx] && flags_left>0) { flag[cy][cx]=1; flags_left--; }
            else if (flag[cy][cx]) { flag[cy][cx]=0; flags_left++; }
        }
        if (k==' ' && !flag[cy][cx]) {
            if (first) {
                first=0;
                /* place mines avoiding (cx,cy) */
                int placed=0;
                while (placed<GGM_MINES) {
                    int mr=rand()%GGM_H, mc=rand()%GGM_W;
                    if (!mine[mr][mc]&&!(mr==cy&&mc==cx)) { mine[mr][mc]=1; placed++; }
                }
                /* compute adjacency */
                for (int r=0;r<GGM_H;r++) for (int c=0;c<GGM_W;c++) {
                    int n=0;
                    for (int dr=-1;dr<=1;dr++) for (int dc=-1;dc<=1;dc++) {
                        int nr=r+dr, nc2=c+dc;
                        if (nr>=0&&nr<GGM_H&&nc2>=0&&nc2<GGM_W&&mine[nr][nc2]) n++;
                    }
                    adj[r][c]=(unsigned char)n;
                }
            }
            if (mine[cy][cx]) {
                /* reveal all mines */
                for (int r=0;r<GGM_H;r++) for (int c=0;c<GGM_W;c++) if(mine[r][c]) rev[r][c]=1;
                gg_gameover("BOOM ! Mine touche !", "");
                if (gg_wait_replay()) goto restart; else return;
            }
            /* flood fill reveal */
            rev[cy][cx]=1;
            if (adj[cy][cx]==0) {
                int q[GGM_W*GGM_H][2]; int qh=0, qt=0;
                q[qt][0]=cx; q[qt][1]=cy; qt++;
                while (qh<qt) {
                    int qx2=q[qh][0], qy2=q[qh][1]; qh++;
                    for (int dr=-1;dr<=1;dr++) for (int dc=-1;dc<=1;dc++) {
                        int nr=qy2+dr, nc2=qx2+dc;
                        if (nr>=0&&nr<GGM_H&&nc2>=0&&nc2<GGM_W&&!rev[nr][nc2]&&!mine[nr][nc2]) {
                            rev[nr][nc2]=1;
                            if (adj[nr][nc2]==0&&qt<GGM_W*GGM_H-1) { q[qt][0]=nc2; q[qt][1]=nr; qt++; }
                        }
                    }
                }
            }
            /* check win */
            int unrevealed=0;
            for (int r=0;r<GGM_H;r++) for (int c=0;c<GGM_W;c++) if(!rev[r][c]&&!mine[r][c]) unrevealed++;
            if (unrevealed==0) { score=flags_left*10; gg_gameover("GAGNE ! Demineur !",""); if(gg_wait_replay()) goto restart; else return; }
        }
    }
}

/* ──────────── MATRIX RAIN ──────────── */
static void gg_matrix(void) {
    int W=gfx_width(), H=gfx_height();
    int cols=W/GFX_FONT_W;
    int rows=(H-GG_HDR_H)/GFX_FONT_H;
    static int head[100], len[100], wait[100], spd[100];
    if (cols>100) cols=100;
    for (int i=0;i<cols;i++) {
        head[i]=-(rand()%(rows+5));
        len[i]=4+rand()%14; spd[i]=1+rand()%3; wait[i]=rand()%spd[i];
    }
    gfx_fill_rect(0, GG_HDR_H, W, H-GG_HDR_H, GFX_RGB(0,0,0));
    gg_hdr("Matrix Rain  [ESC] Menu", 0, 0);
    while (1) {
        int k=get_monitor_char();
        if (k==0x1B) return;
        for (int c=0;c<cols;c++) {
            if (wait[c]>0) { wait[c]--; continue; }
            wait[c]=spd[c];
            head[c]++;
            /* clear tail */
            int tail=head[c]-len[c];
            if (tail>=0&&tail<rows) {
                gfx_fill_rect(c*GFX_FONT_W, GG_HDR_H+tail*GFX_FONT_H, GFX_FONT_W, GFX_FONT_H, GFX_RGB(0,0,0));
            }
            /* draw head */
            if (head[c]>=0&&head[c]<rows) {
                char ch=(char)(0x21+rand()%94);
                gfx_draw_char(c*GFX_FONT_W, GG_HDR_H+head[c]*GFX_FONT_H, (unsigned char)ch,
                              GFX_RGB(0xFF,0xFF,0xFF), GFX_RGB(0,0,0));
                /* previous bright */
                if (head[c]-1>=0) {
                    char pch=(char)(0x21+rand()%94);
                    gfx_draw_char(c*GFX_FONT_W, GG_HDR_H+(head[c]-1)*GFX_FONT_H, (unsigned char)pch,
                                  GFX_RGB(0x00,0xCC,0x00), GFX_RGB(0,0,0));
                }
            }
            /* dim older chars */
            for (int r=head[c]-len[c]+1;r<head[c]-1&&r>=0&&r<rows;r++) {
                char pc2=(char)(0x21+rand()%94);
                int brightness=60+(r-(head[c]-len[c]))*20; if(brightness>140)brightness=140;
                gfx_draw_char(c*GFX_FONT_W, GG_HDR_H+r*GFX_FONT_H, (unsigned char)pc2,
                              GFX_RGB(0,(unsigned char)brightness,0), GFX_RGB(0,0,0));
            }
            if (head[c]>=rows+len[c]) { head[c]=-(1+rand()%8); len[c]=4+rand()%14; spd[c]=1+rand()%3; }
        }
        gfx_swap_buffers();
        nop_delay(500000);
    }
}

/* ──────────── CLOCK ──────────── */
static void gg_clock(void) {
    int W=gfx_width(), H=gfx_height();
    gg_clear();
    gg_hdr("Horloge  [ESC] Menu", 0, 0);
    static const unsigned char seg7[10][7]={
        {1,1,1,0,1,1,1},{0,0,1,0,0,1,0},{1,0,1,1,1,0,1},{1,0,1,1,0,1,1},
        {0,1,1,1,0,1,0},{1,1,0,1,0,1,1},{1,1,0,1,1,1,1},{1,0,1,0,0,1,0},
        {1,1,1,1,1,1,1},{1,1,1,1,0,1,1}
    };
    auto void seg_draw(int x, int y, int d, unsigned int col); void seg_draw(int x, int y, int d, unsigned int col) {
        int sw=36, sh=6, th=36;
        if (seg7[d][0]) gfx_fill_rect(x+4,y,sw,sh,col);             /* top */
        if (seg7[d][1]) gfx_fill_rect(x,y+4,sh,th,col);             /* top-left */
        if (seg7[d][2]) gfx_fill_rect(x+4+sw,y+4,sh,th,col);        /* top-right */
        if (seg7[d][3]) gfx_fill_rect(x+4,y+th+sh,sw,sh,col);       /* middle */
        if (seg7[d][4]) gfx_fill_rect(x,y+th+sh+4,sh,th,col);       /* bot-left */
        if (seg7[d][5]) gfx_fill_rect(x+4+sw,y+th+sh+4,sh,th,col);  /* bot-right */
        if (seg7[d][6]) gfx_fill_rect(x+4,y+2*th+2*sh,sw,sh,col);   /* bottom */
    }
    unsigned long prev=0;
    unsigned int segs_col=GFX_RGB(0x00,0xFF,0x88);
    while (1) {
        int k=get_monitor_char();
        if (k==0x1B) return;
        /* Use jiffies for time display */
        unsigned long j=jiffies;
        if (j==prev) { nop_delay(500000); continue; }
        prev=j;
        unsigned long secs=j/100, mins=secs/60, hrs=mins/60;
        int h=hrs%24, m=mins%60, s=secs%60;
        gfx_fill_rect(0, GG_HDR_H, W, H-GG_HDR_H, GFX_RGB(0x00,0x04,0x00));
        int dw=50, dh=100; /* digit bounding box */
        int start_x=(W-6*dw-2*20)/2, start_y=(H-GG_HDR_H-dh)/2+GG_HDR_H;
        int digits[6]={h/10,h%10,m/10,m%10,s/10,s%10};
        for (int i=0;i<6;i++) {
            int dx=start_x+i*dw; if (i>=2) dx+=20; if (i>=4) dx+=20;
            gfx_fill_rect(dx, start_y, dw-2, dh, GFX_RGB(0x00,0x08,0x00));
            seg_draw(dx+2, start_y+4, digits[i], segs_col);
            /* colon */
            if (i==1||i==3) {
                int cx2=dx+dw+4;
                gfx_fill_rect(cx2, start_y+dh/3, 6, 6, segs_col);
                gfx_fill_rect(cx2, start_y+2*dh/3, 6, 6, segs_col);
            }
        }
        /* subtitle */
        char ts[20]; ts[0]=h/10+'0';ts[1]=h%10+'0';ts[2]=':';ts[3]=m/10+'0';ts[4]=m%10+'0';ts[5]=':';ts[6]=s/10+'0';ts[7]=s%10+'0';ts[8]=0;
        gfx_draw_str(W/2-4*GFX_FONT_W, start_y+dh+16, ts, GFX_RGB(0x44,0x88,0x44), GFX_RGB(0x00,0x04,0x00));
        gg_hdr("Horloge  [ESC] Menu", 0, 0);
        gfx_swap_buffers();
        nop_delay(1000000);
    }
}

/* ──────────── GAMES LAUNCHER ──────────── */
static void gg_draw_lobby(void) {
    int W=gfx_width(), H=gfx_height();
    gfx_gradient_rect(0, GG_HDR_H, W, H-GG_HDR_H, GFX_RGB(0x04,0x04,0x14), GFX_RGB(0x08,0x08,0x28), 1);
    /* title */
    gfx_draw_str(W/2-10*GFX_FONT_W, GG_HDR_H+16, "~~ ARCADE GREGOS ~~",
                 GFX_RGB(0x88,0xCC,0xFF), GFX_RGB(0x04,0x04,0x14));
    gfx_draw_hline(W/2-140, GG_HDR_H+36, 280, GFX_RGB(0x22,0x44,0x88));
    /* 3×4 game grid */
    int bw=220, bh=96, gap=18;
    int cols=3;
    int gx=(W-cols*bw-(cols-1)*gap)/2;
    int gy=GG_HDR_H+52;
    static const char* gnames[10]={"Snake","Tetris","Pong","Invaders","Breakout","2048","Minesweeper","Simon","Matrix","Horloge"};
    static const char* gsubs[10]={"Z S =direction","Pieces+rotation","vs IA  7 pts","Fleches+Espace","Fleches=move","Fleches=slide","Fleches+Espace","Repetez!","Pluie digitale","Affiche heure"};
    static unsigned int gbg[10]={
        GFX_RGB(0x04,0x18,0x04),GFX_RGB(0x0A,0x04,0x18),GFX_RGB(0x18,0x04,0x04),
        GFX_RGB(0x04,0x10,0x18),GFX_RGB(0x18,0x10,0x00),GFX_RGB(0x12,0x0A,0x00),
        GFX_RGB(0x04,0x08,0x14),GFX_RGB(0x14,0x00,0x14),GFX_RGB(0x00,0x10,0x00),
        GFX_RGB(0x00,0x0C,0x10)
    };
    static unsigned int ghl[10]={
        GFX_RGB(0x22,0xCC,0x22),GFX_RGB(0x66,0x44,0xFF),GFX_RGB(0xFF,0x44,0x44),
        GFX_RGB(0x22,0x88,0xFF),GFX_RGB(0xFF,0xAA,0x22),GFX_RGB(0xFF,0xCC,0x44),
        GFX_RGB(0x44,0x88,0xFF),GFX_RGB(0xFF,0x44,0xFF),GFX_RGB(0x22,0xDD,0x22),
        GFX_RGB(0x22,0xDD,0xAA)
    };
    for (int i=0;i<10;i++) {
        int col=i%cols, row=i/cols;
        int bx=gx+col*(bw+gap), by=gy+row*(bh+gap);
        char kn[3]={'0'+(i+1)%10,0,0};
        cg_btn(bx, by, bw, bh, gbg[i], ghl[i], kn, gnames[i], gsubs[i]);
    }
    /* bottom hint */
    gfx_fill_rect(0, H-44, W, 44, GFX_RGB(0x04,0x04,0x14));
    gfx_draw_hline(0, H-44, W, GFX_RGB(0x22,0x44,0x88));
    gfx_draw_str(W/2-22*GFX_FONT_W, H-30, "Appuyez sur 1-9/0 pour lancer un jeu  [ESC]=Bureau",
                 GFX_RGB(0x55,0x88,0xAA), GFX_RGB(0x04,0x04,0x14));
}

static void __attribute__((unused)) start_games_gui(void) {
    if (!gfx_active()) return;
    g_in_launcher=1;
    int W=gfx_width(), H=gfx_height();
    while (1) {
        gfx_fill_rect(0,0,W,H,GG_BG);
        gfx_fill_rect(0,0,W,GG_HDR_H,GFX_RGB(0x08,0x08,0x20));
        gfx_draw_hline(0,GG_HDR_H-1,W,GG_BORDER);
        gfx_draw_str(12,10,"Arcade GregOS",GFX_RGB(0x88,0xCC,0xFF),GFX_RGB(0x08,0x08,0x20));
        gfx_draw_str(W-12-10*GFX_FONT_W,10,"[ESC] Bureau",GFX_RGB(0x55,0x77,0xAA),GFX_RGB(0x08,0x08,0x20));
        gg_draw_lobby();
        gfx_swap_buffers();
        int k; do { k=get_monitor_char(); nop_delay(80000); } while(!k);
        if (k==0x1B) { g_in_launcher=0; draw_interface(); return; }
        switch (k) {
            case '1': gg_snake();       break;
            case '2': gg_tetris();      break;
            case '3': gg_pong();        break;
            case '4': gg_invaders();    break;
            case '5': gg_breakout();    break;
            case '6': gg_2048();        break;
            case '7': gg_minesweeper(); break;
            case '8': gg_simon();       break;
            case '9': gg_matrix();      break;
            case '0': gg_clock();       break;
        }
        /* After game returns, loop back and redraw launcher */
        gfx_fill_rect(0,0,W,H,GG_BG);
    }
}

/* ═══════════════════ FILE EXPLORER ═════════════════════════════════ */
static void __attribute__((unused)) start_fileexplorer(void) {
    open_file_manager();
}

static void cmd_more(const char* fname) {
    const char* src = 0;
    char filebuf[4096];

    if (pipe_stdin && !fname[0]) {
        src = pipe_stdin;
    } else {
        int fi = find_file(fname);
        if (fi < 0) fi = find_file_path(fname);
        if (fi < 0) { term_print("more: fichier introuvable: "); term_print(fname); term_putc('\n'); return; }
        strcpy(filebuf, file_system[fi].content);
        src = filebuf;
    }

    int lines_per_page = VGA_HEIGHT - HEADER_HEIGHT - 3;
    int line = 0, col = 0, page = 1;
    int total = strlen(src);

    for (int i = 0; i <= total; i++) {
        char c = src[i];
        if (c == '\n' || c == '\0') {
            term_putc('\n');
            line++; col = 0;
            if (line >= lines_per_page || c == '\0') {
                if (c == '\0') break;
                term_set_color(0x00, 0x07);
                term_print("  -- Plus -- page ");
                term_print_int(page);
                term_print("  (ESPACE=suite  q=quitter)  ");
                term_set_color(0x0F, 0x00);
                while (1) {
                    int k = get_monitor_char();
                    if (!k) { nop_delay(100000); continue; }
                    if (k == 'q' || k == 'Q' || k == 0x1B) {
                        term_putc('\n'); return;
                    }
                    if (k == ' ' || k == '\r' || k == '\n') break;
                }
                term_move_cursor(0, VGA_HEIGHT - HEADER_HEIGHT - 2 + HEADER_HEIGHT);
                term_set_color(0x00, 0x00);
                for (int x = 0; x < VGA_WIDTH; x++) term_putc(' ');
                term_move_cursor(0, VGA_HEIGHT - HEADER_HEIGHT - 2 + HEADER_HEIGHT);
                line = 0; page++;
            }
        } else {
            term_putc(c); col++;
        }
    }
}

static void cmd_passwd(void) {
    term_set_color(0x0E, 0x00); term_print("Changer le mot de passe GregOS\n");
    term_set_color(0x07, 0x00);

    char cur[32], np1[32], np2[32];
    int i;

    term_print("Mot de passe actuel: ");
    for (i = 0; i < 31; ) {
        int c = get_monitor_char(); if (!c) { nop_delay(100000); continue; }
        if (c == '\n') break;
        if (c == '\b' && i > 0) { i--; term_putc('\b'); }
        else if (c >= 32 && c < 127) { cur[i++] = (char)c; term_putc('\xf9'); }
    }
    cur[i] = '\0'; term_putc('\n');

    if (strcmp(cur, gregos_passwd) != 0) {
        beep_fail();
        term_set_color(0x0C, 0x00); term_print("Mot de passe incorrect.\n");
        term_set_color(0x0F, 0x00); return;
    }

    term_print("Nouveau mot de passe: ");
    for (i = 0; i < 31; ) {
        int c = get_monitor_char(); if (!c) { nop_delay(100000); continue; }
        if (c == '\n') break;
        if (c == '\b' && i > 0) { i--; term_putc('\b'); }
        else if (c >= 32 && c < 127) { np1[i++] = (char)c; term_putc('\xf9'); }
    }
    np1[i] = '\0'; term_putc('\n');

    term_print("Confirmer: ");
    for (i = 0; i < 31; ) {
        int c = get_monitor_char(); if (!c) { nop_delay(100000); continue; }
        if (c == '\n') break;
        if (c == '\b' && i > 0) { i--; term_putc('\b'); }
        else if (c >= 32 && c < 127) { np2[i++] = (char)c; term_putc('\xf9'); }
    }
    np2[i] = '\0'; term_putc('\n');

    if (strcmp(np1, np2) != 0) {
        beep_fail();
        term_set_color(0x0C, 0x00); term_print("Les mots de passe ne correspondent pas.\n");
        term_set_color(0x0F, 0x00); return;
    }
    if (np1[0] == '\0') {
        term_set_color(0x0C, 0x00); term_print("Mot de passe vide refuse.\n");
        term_set_color(0x0F, 0x00); return;
    }
    strncpy(gregos_passwd, np1, 31); gregos_passwd[31] = '\0';
    beep_ok();
    term_set_color(0x0A, 0x00); term_print("Mot de passe mis a jour.\n");
    term_set_color(0x0F, 0x00);
}


static void cmd_cal(void) {
    const char* months[] = {"Janvier","Fevrier","Mars","Avril","Mai","Juin",
                            "Juillet","Aout","Septembre","Octobre","Novembre","Decembre"};
    const int days[]     = {31,28,31,30,31,30,31,31,30,31,30,31};
    int ts = (int)(jiffies / 100);
    int sec  = ts % 60; (void)sec;
    int min  = (ts / 60) % 60; (void)min;
    int hour = (ts / 3600) % 24; (void)hour;
    int day_of_year = (ts / 86400) % 365;
    int month = 0, dom = day_of_year;
    for (int m = 0; m < 12; m++) {
        if (dom < days[m]) { month = m; dom++; break; }
        dom -= days[m];
    }
    int year = 2025;
    int dow_jan1 = 2;
    int d = dow_jan1;
    for (int m = 0; m < month; m++) {
        d = (d + days[m]) % 7;
    }
    int first_dow = d;

    term_set_color(0x0E, 0x00);
    int mlen = strlen(months[month]);
    int pad  = (20 - mlen) / 2;
    for (int i = 0; i < pad; i++) term_putc(' ');
    term_print(months[month]); term_putc(' ');
    term_print_int(year); term_putc('\n');
    term_set_color(0x0B, 0x00);
    term_print("Lu Ma Me Je Ve Sa Di\n");
    term_set_color(0x07, 0x00);
    for (int i = 0; i < first_dow; i++) term_print("   ");
    for (int d2 = 1; d2 <= days[month]; d2++) {
        int col = (first_dow + d2 - 1) % 7;
        if (d2 == dom) term_set_color(0x0A, 0x00);
        if (d2 < 10) term_putc(' ');
        term_print_int(d2);
        term_set_color(0x07, 0x00);
        if (col == 6) term_putc('\n'); else term_putc(' ');
    }
    term_putc('\n');
    term_set_color(0x0F, 0x00);
}

static void cmd_ping(const char* host) {
    if (!host || !host[0]) { term_print("Usage: ping <host>\n"); return; }
    if (!net_ready()) {
        term_set_color(0x0C, 0x00);
        term_print("ping: reseau indisponible (carte RTL8139 absente)\n");
        term_set_color(0x0F, 0x00); return;
    }
    term_set_color(0x07, 0x00);
    term_print("Resolution DNS de "); term_print(host); term_print(" ...\n");
    unsigned int ip = net_dns_resolve(host);
    if (!ip) {
        term_set_color(0x0C, 0x00);
        term_print("ping: nom introuvable: "); term_print(host); term_putc('\n');
        term_set_color(0x0F, 0x00); return;
    }
    char ips[16]; net_format_ip(ip, ips);
    term_set_color(0x0A, 0x00);
    term_print("PING "); term_print(host); term_print(" ("); term_print(ips);
    term_print(") 32 octets de donnees\n");
    term_set_color(0x07, 0x00);
    int ok = 0, sent = 0;
    long total_rtt = 0;
    for (int i = 0; i < 4; i++) {
        sent++;
        int rtt = net_ping(ip, 1500);
        if (rtt < 0) {
            term_set_color(0x0C, 0x00);
            term_print("Delai depasse pour icmp_seq="); term_print_int(i+1); term_putc('\n');
            term_set_color(0x07, 0x00);
        } else {
            ok++; total_rtt += rtt;
            term_print("32 octets de "); term_print(ips);
            term_print(": icmp_seq="); term_print_int(i+1);
            term_print(" ttl=64 temps="); term_print_int(rtt);
            term_print(" ms\n");
        }
    }
    term_set_color(0x0E, 0x00);
    term_print("--- "); term_print(host); term_print(" statistiques ping ---\n");
    term_print_int(sent); term_print(" transmis, "); term_print_int(ok);
    term_print(" recus, "); term_print_int(sent ? (sent-ok)*100/sent : 0);
    term_print("% perte");
    if (ok) { term_print(", rtt moy "); term_print_int((int)(total_rtt/ok)); term_print(" ms"); }
    term_putc('\n');
    term_set_color(0x0F, 0x00);
}

static void cmd_ifconfig(void) {
    if (!net_ready()) {
        term_set_color(0x0C, 0x00);
        term_print("eth0: carte RTL8139 non detectee.\n");
        term_set_color(0x08, 0x00);
        term_print("Lance QEMU avec: -netdev user,id=n0 -device rtl8139,netdev=n0\n");
        term_print("(make run l'ajoute automatiquement)\n");
        term_set_color(0x0F, 0x00); return;
    }
    unsigned char mac[6]; net_get_mac(mac);
    char ip[16], gw[16], dns[16];
    net_format_ip(net_local_ip(), ip);
    net_format_ip(net_gateway_ip(), gw);
    net_format_ip(net_dns_ip(), dns);
    term_set_color(0x0A, 0x00);
    term_print("eth0: "); term_set_color(0x07, 0x00);
    term_print("flags=4163<UP,BROADCAST,RUNNING,MULTICAST>  mtu 1500  [RTL8139]\n");
    term_print("      inet "); term_print(ip);
    term_print("  netmask 255.255.255.0  gateway "); term_print(gw); term_putc('\n');
    term_print("      dns "); term_print(dns); term_print("  ether ");
    const char* hx = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        term_putc(hx[mac[i]>>4]); term_putc(hx[mac[i]&15]);
        if (i < 5) term_putc(':');
    }
    term_putc('\n');
    term_print("      RX packets "); term_print_int((int)net_rx_packets());
    term_print("  bytes "); term_print_int((int)net_rx_bytes());
    term_print("  TX packets "); term_print_int((int)net_tx_packets());
    term_print("  bytes "); term_print_int((int)net_tx_bytes()); term_print("\n\n");
    term_set_color(0x0A, 0x00);
    term_print("lo:   "); term_set_color(0x07, 0x00);
    term_print("flags=73<UP,LOOPBACK,RUNNING>  mtu 65536\n");
    term_print("      inet 127.0.0.1  netmask 255.0.0.0\n");
    term_set_color(0x0F, 0x00);
}

static void cmd_watch(int interval, char* cmd) {
    if (interval <= 0 || !cmd || !cmd[0]) {
        term_print("Usage: watch <secs> <commande>\n"); return;
    }
    term_set_color(0x08, 0x00);
    term_print("watch: "); term_print(cmd);
    term_print(" (ESC pour quitter)\n");
    term_set_color(0x0F, 0x00);
    while (1) {
        execute_command(cmd);
        unsigned long ticks = (unsigned long)interval * 100000000UL;
        unsigned long t = 0;
        int quit = 0;
        while (t < ticks) {
            int c = get_monitor_char();
            if (c == 0x1B) { quit = 1; break; }
            for (int d = 0; d < 100000; d++) __asm__ volatile("nop");
            t += 100000;
        }
        if (quit) break;
    }
}

static void cmd_sudo(char* rest) {
    if (!rest || !rest[0]) { term_print("Usage: sudo <commande>\n"); return; }
    term_print("[sudo] mot de passe pour root: ");
    char pw[32]; int i = 0;
    while (i < 31) {
        int c = get_monitor_char(); if (!c) { nop_delay(100000); continue; }
        if (c == '\n') break;
        if (c == '\b' && i > 0) { i--; term_putc('\b'); }
        else if (c >= 32 && c < 127) { pw[i++] = (char)c; term_putc('\xf9'); }
    }
    pw[i] = '\0'; term_putc('\n');
    if (strcmp(pw, gregos_passwd) != 0) {
        beep_fail();
        term_set_color(0x0C, 0x00);
        term_print("sudo: echec d'authentification\n");
        term_set_color(0x0F, 0x00);
        return;
    }
    beep_ok();
    term_set_color(0x0A, 0x00); term_print("Acces root: "); term_set_color(0x0F, 0x00);
    execute_command(rest);
}

static void cmd_tee(char* fname) {
    if (!pipe_stdin) { term_print("tee: necessite une pipe\n"); return; }
    if (!fname || !fname[0]) { term_print("Usage: ... | tee <fichier>\n"); return; }
    int fi = find_file(fname);
    if (fi == -1) {
        int sl = next_free_slot();
        if (sl == -1) { term_print("Disk full.\n"); return; }
        strncpy(file_system[sl].name, fname, FILENAME_SIZE);
        file_system[sl].type = TYPE_FILE; file_system[sl].exists = 1;
        file_system[sl].parent_id = current_dir_id; file_system[sl].id = sl + 1;
        fi = sl;
    }
    int len = strlen(pipe_stdin);
    if (len >= FILE_CONTENT_SIZE) len = FILE_CONTENT_SIZE - 1;
    strncpy(file_system[fi].content, pipe_stdin, len + 1);  /* n-1 bytes → copy all len */
    file_system[fi].content[len] = '\0';
    file_system[fi].size = len;
    term_print(pipe_stdin);
    beep_tick();
}

static void cmd_ln(const char* src, const char* dst) {
    if (!src || !src[0] || !dst || !dst[0]) { term_print("Usage: ln <src> <dst>\n"); return; }
    int si = find_file(src);
    if (si == -1) si = find_file_path(src);
    if (si == -1) { term_print("ln: source introuvable: "); term_print(src); term_putc('\n'); return; }
    int sl = next_free_slot();
    if (sl == -1) { term_print("Disk full.\n"); return; }
    strncpy(file_system[sl].name, dst, FILENAME_SIZE);
    file_system[sl].type = file_system[si].type;
    file_system[sl].exists = 1;
    file_system[sl].parent_id = current_dir_id;
    file_system[sl].id = sl + 1;
    strncpy(file_system[sl].content, file_system[si].content, FILE_CONTENT_SIZE);
    file_system[sl].size = file_system[si].size;
    beep_tick();
    term_print("Lien cree: "); term_print(dst); term_putc('\n');
}

static int strstr_simple(const char* hay, const char* needle) {
    int hl = strlen(hay), nl = strlen(needle);
    for (int i = 0; i <= hl - nl; i++) {
        int match = 1;
        for (int j = 0; j < nl; j++) if (hay[i+j] != needle[j]) { match = 0; break; }
        if (match) return 1;
    }
    return 0;
}

static void cmd_file(const char* fname) {
    if (!fname || !fname[0]) { term_print("Usage: file <nom>\n"); return; }
    int fi = find_file(fname);
    if (fi < 0) fi = find_file_path(fname);
    if (fi < 0) { term_print("file: introuvable: "); term_print(fname); term_putc('\n'); return; }
    term_set_color(0x0E, 0x00); term_print(fname); term_set_color(0x07, 0x00); term_print(": ");
    if (file_system[fi].type == TYPE_DIR) {
        term_set_color(0x0B, 0x00); term_print("directory\n");
    } else if (file_system[fi].size == 0) {
        term_print("empty\n");
    } else {
        const char* c = file_system[fi].content;
        int is_script = (c[0]=='#' || startswith(c,"echo") || startswith(c,"sh") || startswith(c,"cat"));
        int has_null = 0;
        for (int i = 0; i < file_system[fi].size && i < 64; i++) if (c[i] == 0) has_null = 1;
        if (has_null) {
            term_set_color(0x0C, 0x00); term_print("binary data\n");
        } else if (is_script) {
            term_set_color(0x0A, 0x00); term_print("shell script, ASCII text\n");
        } else {
            term_set_color(0x0F, 0x00); term_print("ASCII text\n");
        }
    }
    term_set_color(0x0F, 0x00);
}

static void cmd_du(const char* path) {
    int total = 0;
    int dir_id = current_dir_id;
    if (path && path[0]) {
        int fi = find_file_path(path);
        if (fi < 0) fi = find_file(path);
        if (fi >= 0 && file_system[fi].type == TYPE_DIR) dir_id = file_system[fi].id;
    }
    for (int i = 0; i < MAX_FILES; i++) {
        if (!file_system[i].exists) continue;
        if (file_system[i].type == TYPE_FILE &&
            (file_system[i].parent_id == dir_id || dir_id == current_dir_id)) {
            int kb = (file_system[i].size + 1023) / 1024;
            if (kb == 0) kb = 1;
            term_print_int(kb); term_putc('\t'); term_print(file_system[i].name); term_putc('\n');
            total += kb;
        }
    }
    term_set_color(0x0E, 0x00); term_print_int(total); term_print("\ttotal\n");
    term_set_color(0x0F, 0x00);
}

static void cmd_lspci(void) {
    term_set_color(0x0B, 0x00);
    term_print("00:00.0 "); term_set_color(0x07, 0x00); term_print("Host bridge: Intel Corporation 440FX - 82441FX PMC [Natoma]\n");
    term_set_color(0x0B, 0x00);
    term_print("00:01.0 "); term_set_color(0x07, 0x00); term_print("ISA bridge: Intel Corporation 82371SB PIIX3 ISA\n");
    term_set_color(0x0B, 0x00);
    term_print("00:01.1 "); term_set_color(0x07, 0x00); term_print("IDE interface: Intel Corporation 82371SB PIIX3 IDE\n");
    term_set_color(0x0B, 0x00);
    term_print("00:02.0 "); term_set_color(0x07, 0x00); term_print("VGA compatible controller: Cirrus Logic GD 5446\n");
    term_set_color(0x0B, 0x00);
    term_print("00:03.0 "); term_set_color(0x07, 0x00); term_print("Ethernet controller: Intel Corporation 82540EM Gigabit Ethernet\n");
    term_set_color(0x0B, 0x00);
    term_print("00:04.0 "); term_set_color(0x07, 0x00); term_print("Unclassified device: Red Hat QEMU Virtual Machine\n");
    term_set_color(0x0F, 0x00);
}

static void cmd_dmesg(void) {
    unsigned long t = jiffies / 100;
    const char* entries[] = {
        "GregOS kernel starting...",
        "CPU: i386 Protected Mode, 256MB RAM",
        "IDT: 8259A PIC remapped IRQ0-15 to 0x20-0x2F",
        "PIT: channel 0 at 100 Hz -> jiffies counter",
        "VGA: text mode 80x25 @ 0xB8000",
        "KB:  PS/2 AZERTY driver loaded, IRQ1 ring buffer",
        "FS:  in-memory filesystem mounted (64 slots, 4096B/file)",
        "HEAP: free-list allocator 32MB (kfree active)",
        "SND: PC speaker via PIT ch2 (0x42/0x43/0x61)",
        "NET: eth0 up 192.168.1.42/24",
        "LOGIN: gregsh started",
        0
    };
    for (int i = 0; entries[i]; i++) {
        term_set_color(0x08, 0x00);
        term_putc('['); term_print_int((int)(t + i)); term_print("."); term_print_int(i * 137 % 1000);
        term_print("] ");
        term_set_color(0x0A, 0x00);
        term_print(entries[i]); term_putc('\n');
    }
    term_set_color(0x0F, 0x00);
}

static void cmd_id(void) {
    term_print("uid=0("); term_print(current_user); term_print(") gid=0(");
    term_print(current_user); term_print(") groups=0("); term_print(current_user);
    term_print("),1(daemon),4(adm),27(sudo),1000(greg)\n");
}

static void cmd_hostname(const char* arg) {
    if (!arg || !arg[0]) {
        term_print(gregos_hostname); term_putc('\n');
    } else {
        strncpy(gregos_hostname, arg, 63); gregos_hostname[63] = '\0';
        beep_tick();
        term_print("Hostname set to: "); term_print(gregos_hostname); term_putc('\n');
    }
}

static void cmd_mount(void) {
    term_set_color(0x0E, 0x00); term_print("sysfs"); term_set_color(0x07,0x00);
    term_print(" on /sys type sysfs (rw,nosuid,nodev,noexec,relatime)\n");
    term_set_color(0x0E, 0x00); term_print("proc");  term_set_color(0x07,0x00);
    term_print(" on /proc type proc (rw,nosuid,nodev,noexec,relatime)\n");
    term_set_color(0x0E, 0x00); term_print("gregos-fs"); term_set_color(0x07,0x00);
    term_print(" on / type gregos-ramfs (rw,relatime,size=64k)\n");
    term_set_color(0x0E, 0x00); term_print("tmpfs"); term_set_color(0x07,0x00);
    term_print(" on /tmp type tmpfs (rw,nosuid,nodev)\n");
    term_set_color(0x0F, 0x00);
}

static void cmd_crontab(const char* arg) {
    if (arg && startswith(arg, "-e")) {
        term_print("EDITOR=nano: ouverture du crontab...\n");
        term_print("(cron non implementé, sessions temporaires)\n");
        return;
    }
    term_set_color(0x08, 0x00);
    term_print("# Crontab de "); term_print(current_user); term_putc('\n');
    term_print("# m  h  dom  mon  dow  commande\n");
    term_set_color(0x07, 0x00);
    term_print("  0  *   *    *    *   fortune\n");
    term_print(" 30  6   *    *    1   echo 'Bonne semaine!'\n");
    term_set_color(0x0F, 0x00);
}

static void cmd_type(const char* name) {
    if (!name || !name[0]) { term_print("Usage: type <commande>\n"); return; }
    for (int i = 0; cmd_list[i]; i++) {
        if (strcmp(cmd_list[i], name) == 0) {
            term_print(name); term_print(" is a shell builtin\n"); return;
        }
    }
    for (int i = 0; i < alias_count; i++) {
        if (strcmp(alias_names[i], name) == 0) {
            term_print(name); term_print(" is aliased to '");
            term_print(alias_cmds[i]); term_print("'\n"); return;
        }
    }
    term_print(name); term_print(": not found\n");
}

/* Shared real HTTP fetch used by wget/curl. If print_body, dump body to tty;
   if save_name != NULL, store the body into the VFS. */
extern int https_get(const char* url, char** out_body, int* out_len, int max_len,
                      char* final_url, char* content_type);

static void http_fetch_cli(const char* url, int print_body, const char* save_name) {
    if (!url || !url[0]) { term_print("Usage: <cmd> <url>\n"); return; }
    if (!net_ready()) {
        term_set_color(0x0C, 0x00);
        term_print("reseau indisponible (carte RTL8139 absente).\n");
        term_set_color(0x08, 0x00);
        term_print("Relance avec 'make run' (ajoute -device rtl8139).\n");
        term_set_color(0x0F, 0x00); return;
    }
    char full[512];
    if (!(url[0]=='h'&&url[1]=='t'&&url[2]=='t'&&url[3]=='p')) {
        int p = 0; const char* pfx = "http://";
        for (int i = 0; pfx[i]; i++) full[p++] = pfx[i];
        for (int i = 0; url[i] && p < 510; i++) full[p++] = url[i];
        full[p] = '\0';
    } else { strncpy(full, url, 511); full[511] = '\0'; }

    term_set_color(0x07, 0x00);
    term_print("Connexion a "); term_print(full); term_print(" ...\n");

    char* body = 0; int blen = 0;
    char final_url[512]; char ctype[64];
    int is_https = (full[0]=='h'&&full[1]=='t'&&full[2]=='t'&&full[3]=='p'&&full[4]=='s');
    int status = is_https
               ? https_get(full, &body, &blen, 262144, final_url, ctype)
               : http_get (full, &body, &blen, 262144, final_url, ctype);

    if (status == -2) {
        term_set_color(0x0C, 0x00);
        term_print("Echec de la poignee de main TLS.\n");
        term_set_color(0x0F, 0x00); return;
    }
    if (status == -1) {
        term_set_color(0x0C, 0x00);
        term_print("URL non supportee ou malformee.\n");
        term_set_color(0x0F, 0x00); return;
    }
    if (status == 0 || !body) {
        term_set_color(0x0C, 0x00);
        term_print("Echec: pas de reponse (hote injoignable ou DNS echoue).\n");
        term_set_color(0x0F, 0x00);
        if (body) kfree(body);
        return;
    }
    term_set_color(status < 400 ? 0x0A : 0x0E, 0x00);
    term_print("HTTP "); term_print_int(status);
    term_print("  "); term_print(ctype[0] ? ctype : "?");
    term_print("  "); term_print_int(blen); term_print(" octets\n");
    term_set_color(0x0F, 0x00);

    if (save_name && save_name[0]) {
        int id = vfs_create_file(save_name, 0);
        if (id >= 0) { vfs_write_file(id, body, blen > 4095 ? 4095 : blen);
            term_set_color(0x0A,0x00);
            term_print("Enregistre dans "); term_print(save_name); term_putc('\n');
            term_set_color(0x0F,0x00);
        } else term_print("(impossible de creer le fichier)\n");
    }
    if (print_body) {
        int shown = blen > 3000 ? 3000 : blen;
        for (int i = 0; i < shown; i++) term_putc(body[i]);
        if (blen > shown) { term_set_color(0x08,0x00);
            term_print("\n... ["); term_print_int(blen - shown);
            term_print(" octets tronques]\n"); term_set_color(0x0F,0x00); }
        else term_putc('\n');
    }
    kfree(body);
}

static void cmd_wget(const char* url) {
    /* Save to a local file named after the last path segment (or index.html). */
    char name[64]; int n = 0;
    const char* base = url ? url : "";
    for (const char* p = base; *p; p++) if (*p == '/') base = p + 1;
    for (const char* p = base; *p && *p!='?' && n < 63; p++) name[n++] = *p;
    name[n] = '\0';
    if (n == 0) strncpy(name, "index.html", 63);
    http_fetch_cli(url, 0, name);
}

static void cmd_curl(const char* url) {
    http_fetch_cli(url, 1, 0);
}

static void cmd_traceroute(const char* host) {
    if (!host || !host[0]) { term_print("Usage: traceroute <host>\n"); return; }
    term_set_color(0x0E, 0x00);
    term_print("traceroute vers "); term_print(host); term_print(", 30 sauts max\n");
    term_set_color(0x07, 0x00);
    unsigned int rng = (unsigned int)(jiffies * 1664525u + 1013904223u);
    const char* gws[] = {"192.168.1.1","10.0.0.1","172.16.0.1","8.8.4.4","1.1.1.1"};
    for (int hop = 1; hop <= 5; hop++) {
        rng = rng * 1664525u + 1013904223u;
        int rtt = 2 + (int)(rng % 30) * hop;
        term_print_int(hop); term_putc(' ');
        if (hop < 4) { term_print(gws[hop-1]); }
        else { term_print(host); }
        term_print("  "); term_print_int(rtt); term_print(" ms\n");
        for (int d = 0; d < 60000000; d++) __asm__ volatile("nop");
    }
    term_set_color(0x0F, 0x00);
}

static void cmd_nslookup(const char* host) {
    if (!host || !host[0]) { term_print("Usage: nslookup <host>\n"); return; }
    if (!net_ready()) {
        term_set_color(0x0C, 0x00);
        term_print("nslookup: reseau indisponible (carte RTL8139 absente)\n");
        term_set_color(0x0F, 0x00); return;
    }
    char dns[16]; net_format_ip(net_dns_ip(), dns);
    term_print("Server:\t\t"); term_print(dns);
    term_print("\nAddress:\t"); term_print(dns); term_print("#53\n\n");
    unsigned int ip = net_dns_resolve(host);
    if (!ip) {
        term_set_color(0x0C, 0x00);
        term_print("** serveur ne peut pas trouver "); term_print(host);
        term_print(": NXDOMAIN\n");
        term_set_color(0x0F, 0x00); return;
    }
    char ips[16]; net_format_ip(ip, ips);
    term_print("Non-authoritative answer:\n");
    term_print("Name:\t"); term_print(host); term_putc('\n');
    term_print("Address: "); term_print(ips); term_putc('\n');
}

static void cmd_nmap(const char* host) {
    if (!host || !host[0]) { term_print("Usage: nmap <host>\n"); return; }
    term_set_color(0x0E, 0x00);
    term_print("Starting Nmap 7.80 on "); term_print(host); term_putc('\n');
    term_set_color(0x07, 0x00);
    for (int i = 0; i < 120000000; i++) __asm__ volatile("nop");
    term_print("Nmap scan report for "); term_print(host); term_putc('\n');
    term_print("Host is up (0.012s latency).\n\n");
    term_set_color(0x0B, 0x00);
    term_print("PORT     STATE  SERVICE\n");
    term_set_color(0x0A, 0x00);
    term_print("22/tcp   open   ssh\n");
    term_print("80/tcp   open   http\n");
    term_print("443/tcp  open   https\n");
    term_set_color(0x07, 0x00);
    term_print("3306/tcp closed mysql\n");
    term_print("8080/tcp closed http-proxy\n\n");
    term_set_color(0x0E, 0x00);
    term_print("Nmap done: 1 IP address (1 host up) scanned\n");
    term_set_color(0x0F, 0x00);
}

static void cmd_ssh(const char* host) {
    if (!host || !host[0]) { term_print("Usage: ssh <host>\n"); return; }
    term_set_color(0x0E, 0x00);
    term_print("ssh: connexion vers "); term_print(current_user); term_putc('@'); term_print(host); term_putc('\n');
    term_set_color(0x07, 0x00);
    for (int i = 0; i < 100000000; i++) __asm__ volatile("nop");
    term_print("The authenticity of host '"); term_print(host); term_print("' can't be established.\n");
    term_print("RSA key fingerprint is SHA256:GregOS/DRAKKAR+2025+i386.\n");
    term_print("Are you sure you want to continue? (yes/no) ");
    int c = 0; while (!c) { c = get_monitor_char(); nop_delay(100000); }
    term_putc('\n');
    term_set_color(0x0C, 0x00);
    term_print("ssh: connect to host "); term_print(host);
    term_print(" port 22: Network unreachable\n");
    term_set_color(0x0F, 0x00);
}

static const char* pkg_installed[] = {
    "gregsh-2.0", "gregos-kernel-6.19", "nano-editor-1.0",
    "libgreg-0.9", "vga-driver-2.1", "ps2-keyboard-1.3",
    "pc-speaker-1.0", "gregos-games-1.4", "casino-suite-1.2",
    "music-engine-1.1", 0
};
static const char* pkg_available[] = {
    "gcc-greg-13.0", "python-greg-3.11", "nginx-greg-1.24",
    "sqlite-greg-3.41", "vim-greg-9.0", "htop-greg-3.2",
    "neovim-greg-0.9", "tmux-greg-3.3", "zsh-greg-5.9",
    "git-greg-2.40", 0
};

static void cmd_pkg(const char* arg) {
    if (!arg || !arg[0] || strcmp(arg, "list") == 0) {
        term_set_color(0x0E, 0x00); term_print("Paquets installes:\n"); term_set_color(0x07, 0x00);
        for (int i = 0; pkg_installed[i]; i++) {
            term_print("  [I] "); term_set_color(0x0A, 0x00);
            term_print(pkg_installed[i]); term_set_color(0x07, 0x00); term_putc('\n');
        }
        return;
    }
    if (startswith(arg, "search ") || strcmp(arg, "available") == 0) {
        const char* q = startswith(arg, "search ") ? arg + 7 : "";
        term_set_color(0x0E, 0x00); term_print("Paquets disponibles:\n"); term_set_color(0x07, 0x00);
        for (int i = 0; pkg_available[i]; i++) {
            if (!q[0] || strstr_simple(pkg_available[i], q))
                term_print("  [ ] "), term_print(pkg_available[i]), term_putc('\n');
        }
        return;
    }
    if (startswith(arg, "install ")) {
        const char* name = arg + 8;
        term_print("Recherche "); term_print(name); term_print("...\n");
        for (int i = 0; i < 120000000; i++) __asm__ volatile("nop");
        term_set_color(0x0A, 0x00);
        term_print("Telechargement: "); term_print(name); term_print(" [##########] 100%\n");
        term_print("Installation reussie: "); term_print(name); term_putc('\n');
        beep_ok(); term_set_color(0x0F, 0x00);
        return;
    }
    if (startswith(arg, "remove ")) {
        const char* name = arg + 7;
        term_set_color(0x0C, 0x00);
        term_print("Suppression: "); term_print(name); term_print("... OK\n");
        beep_tick(); term_set_color(0x0F, 0x00);
        return;
    }
    term_print("Usage: pkg list | pkg search <q> | pkg install <n> | pkg remove <n>\n");
}

static void cmd_fdisk(const char* arg) {
    (void)arg;
    term_set_color(0x0E, 0x00); term_print("Disk /dev/sda: 20 GiB, 21474836480 bytes\n");
    term_set_color(0x07, 0x00);
    term_print("Unites: secteurs de 512 octets\n");
    term_print("Taille des secteurs: 512 / 512 octets\n\n");
    term_set_color(0x0B, 0x00);
    term_print("Periph.    Debut      Fin  Secteurs Taille Type\n");
    term_set_color(0x07, 0x00);
    term_print("/dev/sda1   2048  2099199   2097152    1G  EFI System\n");
    term_print("/dev/sda2 2099200 41943039  39843840   19G  Linux filesystem\n\n");
    term_set_color(0x08, 0x00);
    term_print("(Partition table simulee - pas de disque ATA configure)\n");
    term_set_color(0x0F, 0x00);
}

static void cmd_expr(const char* expr) {
    if (!expr || !expr[0]) { term_print("Usage: expr <expression>\n"); return; }
    long a = 0, b = 0; char op = 0;
    const char* p = expr;
    while (*p == ' ') p++;
    int neg = 0; if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') a = a * 10 + (*p++ - '0');
    if (neg) a = -a;
    while (*p == ' ') p++;
    op = *p++;
    while (*p == ' ') p++;
    neg = 0; if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') b = b * 10 + (*p++ - '0');
    if (neg) b = -b;
    long res = 0; int ok = 1;
    if      (op == '+') res = a + b;
    else if (op == '-') res = a - b;
    else if (op == '*' || op == 'x') res = a * b;
    else if (op == '/') { if (b == 0) { term_print("expr: division par zero\n"); return; } res = a / b; }
    else if (op == '%') { if (b == 0) { term_print("expr: modulo zero\n"); return; } res = a % b; }
    else ok = 0;
    if (ok) term_print_int((int)res);
    else term_print("expr: operateur inconnu");
    term_putc('\n');
}

static void cmd_useradd(const char* name) {
    if (!name || !name[0]) { term_print("Usage: useradd <nom>\n"); return; }
    term_set_color(0x0A, 0x00);
    term_print("Utilisateur cree: "); term_print(name);
    term_print(" (uid=1001, home=/home/"); term_print(name); term_print(")\n");
    beep_tick(); term_set_color(0x0F, 0x00);
}

static void cmd_su(const char* user) {
    const char* u = (user && user[0]) ? user : "root";
    term_print("Mot de passe de "); term_print(u); term_print(": ");
    char pw[32]; int i = 0;
    while (i < 31) {
        int c = get_monitor_char(); if (!c) { nop_delay(100000); continue; }
        if (c == '\n') break;
        if (c == '\b' && i > 0) { i--; term_putc('\b'); }
        else if (c >= 32 && c < 127) { pw[i++] = (char)c; term_putc('\xf9'); }
    }
    pw[i] = '\0'; term_putc('\n');
    if (strcmp(pw, gregos_passwd) == 0) {
        strncpy(current_user, u, 31); current_user[31] = '\0';
        beep_ok();
        term_set_color(0x0A, 0x00); term_print("Session ouverte en tant que "); term_print(current_user); term_putc('\n');
        term_set_color(0x0F, 0x00);
    } else {
        beep_fail();
        term_set_color(0x0C, 0x00); term_print("su: echec d'authentification\n");
        term_set_color(0x0F, 0x00);
    }
}

static void cmd_strace(char* cmd) {
    if (!cmd || !cmd[0]) { term_print("Usage: strace <commande>\n"); return; }
    term_set_color(0x08, 0x00);
    const char* calls[] = {
        "execve(\"/bin/gregsh\", [\"%s\"], envp)", "brk(NULL) = 0x10000",
        "access(\"/etc/ld.so.preload\", R_OK) = -1 ENOENT",
        "openat(AT_FDCWD, \"/etc/ld.so.cache\", O_RDONLY) = 3",
        "mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE) = 0x20000",
        "fstat(3, {st_mode=S_IFREG, st_size=1024}) = 0",
        "read(3, \"\\x7fELF\", 4096) = 4096",
        "write(1, \"\", 0) = 0",
        "+++ exited with 0 +++",
        0
    };
    for (int i = 0; calls[i]; i++) {
        if (i == 0) {
            int sl = strlen(calls[0]) + strlen(cmd) + 2;
            char fmt[128]; int fi = 0, ci = 0;
            while (calls[0][ci] && fi < 126) {
                if (calls[0][ci] == '%' && calls[0][ci+1] == 's') {
                    int j = 0; while (cmd[j] && fi < 120) fmt[fi++] = cmd[j++];
                    ci += 2;
                } else { fmt[fi++] = calls[0][ci++]; }
            }
            fmt[fi] = '\0'; (void)sl;
            term_print(fmt); term_putc('\n');
        } else {
            term_print(calls[i]); term_putc('\n');
        }
        for (int d = 0; d < 20000000; d++) __asm__ volatile("nop");
    }
    term_set_color(0x07, 0x00);
    execute_command(cmd);
    term_set_color(0x0F, 0x00);
}

static void cmd_kill(const char* arg) {
    if (!arg || !arg[0]) { term_print("Usage: kill <pid>\n"); return; }
    int pid = 0;
    const char* p = arg;
    if (startswith(p, "-9 ") || startswith(p, "-SIGKILL ")) {
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }
    while (*p >= '0' && *p <= '9') pid = pid * 10 + (*p++ - '0');
    if (pid == 0) { term_print("kill: pas de PID valide\n"); return; }
    if (pid == 1) { term_print("kill: operation non autorisee (PID 1 = init)\n"); return; }
    term_set_color(0x0A, 0x00);
    term_print("Signal envoye au processus "); term_print_int(pid);
    term_print(" (terminated)\n");
    beep_tick(); term_set_color(0x0F, 0x00);
}

static void cmd_jobs(void) {
    term_set_color(0x08, 0x00);
    term_print("[GregOS: pas de scheduler preemptif - aucun job en arriere-plan]\n");
    term_set_color(0x0F, 0x00);
}

static void cmd_printenv(const char* key) {
    if (key && key[0]) {
        for (int i = 0; i < env_count; i++) {
            if (strcmp(env_keys[i], key) == 0) {
                term_print(env_vals[i]); term_putc('\n'); return;
            }
        }
        term_set_color(0x08, 0x00); term_print("(non defini)\n"); term_set_color(0x0F, 0x00);
        return;
    }
    for (int i = 0; i < env_count; i++) {
        term_print(env_keys[i]); term_putc('='); term_print(env_vals[i]); term_putc('\n');
    }
}

static void cmd_source(const char* fname) {
    if (!fname || !fname[0]) { term_print("Usage: source <fichier>\n"); return; }
    int fi = find_file(fname);
    if (fi < 0) fi = find_file_path(fname);
    if (fi < 0) { term_print("source: fichier introuvable: "); term_print(fname); term_putc('\n'); return; }
    run_script(file_system[fi].content);
}

static void cmd_true_cmd(void)  { last_exit_code = 0; }
static void cmd_false_cmd(void) { last_exit_code = 1; }

static void cmd_mouse(void) {
    term_set_color(0x0B, 0x00); term_print("Mouse status\n");
    term_set_color(0x0F, 0x00);
    term_print("  position: x="); term_print_int(ps2mouse_gui_x());
    term_print(" y="); term_print_int(ps2mouse_gui_y()); term_putc('\n');
    term_print("  buttons : ");
    int _mb = ps2mouse_buttons();
    if (_mb & 0x01) term_print("[L] "); else term_print("[ ] ");
    if (_mb & 0x02) term_print("[R] "); else term_print("[ ] ");
    if (_mb & 0x04) term_print("[M]");  else term_print("[ ]");
    term_putc('\n');
}

static void cmd_tasks(void) {
    term_set_color(0x0B, 0x00); term_print("ID  STATE    NAME\n");
    term_set_color(0x07, 0x00); term_print("--  -------  ----\n");
    term_set_color(0x0A, 0x00); term_print(" 0  running  main\n");
    term_set_color(0x0F, 0x00); term_print(" 1  ready    test_thread\n");
}

#define ATA_DATA    0x1F0
#define ATA_FEAT    0x1F1
#define ATA_SECCNT  0x1F2
#define ATA_LBA0    0x1F3
#define ATA_LBA1    0x1F4
#define ATA_LBA2    0x1F5
#define ATA_DRIVE   0x1F6
#define ATA_CMD     0x1F7
#define ATA_STATUS  0x1F7

static int ata_wait_ready(void) {
    int t = 0x200000;
    while (t-- > 0) {
        unsigned char s = port_byte_in(ATA_STATUS);
        if (!(s & 0x80) && (s & 0x40)) return 1;
        if (s & 0x01) return 0;
    }
    return 0;
}

static int ata_wait_drq(void) {
    int t = 0x200000;
    while (t-- > 0) {
        unsigned char s = port_byte_in(ATA_STATUS);
        if (s & 0x08) return 1;
        if (s & 0x01) return 0;
    }
    return 0;
}

static int ata_identify(unsigned short out[256]) {
    port_byte_out(ATA_DRIVE, 0xA0);
    for (int i = 0; i < 4; i++) port_byte_out(ATA_FEAT + i, 0);
    port_byte_out(ATA_CMD, 0xEC);
    unsigned char status = port_byte_in(ATA_STATUS);
    if (status == 0) return 0;
    if (!ata_wait_ready()) return 0;
    if (!ata_wait_drq())   return 0;
    for (int i = 0; i < 256; i++)
        out[i] = port_word_in(ATA_DATA);
    return 1;
}

static int ata_read_sector(unsigned int lba, unsigned short buf[256]) {
    port_byte_out(ATA_DRIVE,  0xE0 | ((lba >> 24) & 0x0F));
    port_byte_out(ATA_SECCNT, 1);
    port_byte_out(ATA_LBA0,   lba & 0xFF);
    port_byte_out(ATA_LBA1,   (lba >> 8) & 0xFF);
    port_byte_out(ATA_LBA2,   (lba >> 16) & 0xFF);
    port_byte_out(ATA_CMD,    0x20);
    if (!ata_wait_drq()) return 0;
    for (int i = 0; i < 256; i++)
        buf[i] = port_word_in(ATA_DATA);
    return 1;
}

static void ata_str(unsigned short* id, int start, int len, char* out) {
    for (int i = 0; i < len/2; i++) {
        unsigned short w = id[start + i];
        out[i*2]   = (char)(w >> 8);
        out[i*2+1] = (char)(w & 0xFF);
    }
    out[len] = '\0';
    int end = len - 1;
    while (end >= 0 && out[end] == ' ') { out[end] = '\0'; end--; }
}

static void cmd_ata(const char* arg) {
    static unsigned short id[256];
    if (!arg || *arg == '\0' || strcmp(arg, "identify") == 0) {
        term_set_color(0x0B, 0x00); term_print("ATA IDENTIFY:\n");
        if (!ata_identify(id)) {
            term_set_color(0x0C, 0x00); term_print("  No ATA drive detected.\n");
            return;
        }
        char model[42], serial[22], fw[10];
        ata_str(id, 27, 40, model);
        ata_str(id, 10, 20, serial);
        ata_str(id, 23,  8, fw);
        unsigned int sectors = (unsigned int)id[60] | ((unsigned int)id[61] << 16);
        term_set_color(0x0F, 0x00);
        term_print("  Model  : "); term_print(model);  term_putc('\n');
        term_print("  Serial : "); term_print(serial); term_putc('\n');
        term_print("  FW     : "); term_print(fw);     term_putc('\n');
        term_print("  Sectors: "); term_print_int((int)sectors); term_putc('\n');
        unsigned int mb = sectors / 2048;
        term_print("  Size   : "); term_print_int((int)mb); term_print(" MB\n");
        return;
    }
    if (startswith(arg, "read ")) {
        unsigned int lba = 0;
        const char* p = arg + 5;
        while (*p >= '0' && *p <= '9') { lba = lba * 10 + (unsigned int)(*p - '0'); p++; }
        static unsigned short sec[256];
        term_set_color(0x0B, 0x00);
        term_print("ATA read LBA "); term_print_int((int)lba); term_putc('\n');
        if (!ata_read_sector(lba, sec)) {
            term_set_color(0x0C, 0x00); term_print("  Read failed.\n");
            return;
        }
        unsigned char* bytes = (unsigned char*)sec;
        term_set_color(0x0F, 0x00);
        for (int row = 0; row < 8; row++) {
            char hx[3]; hx[2] = '\0';
            term_print_int(row * 64); term_print(": ");
            for (int col = 0; col < 16; col++) {
                unsigned char b = bytes[row * 16 + col];
                hx[0] = "0123456789ABCDEF"[b >> 4];
                hx[1] = "0123456789ABCDEF"[b & 0xF];
                term_print(hx); term_putc(' ');
            }
            term_putc('\n');
        }
        return;
    }
    term_print("Usage: ata [identify|read <lba>]\n");
}

static void __attribute__((unused)) cmd_sleep2(int sec, int ms) {
    (void)ms;
    if (sec <= 0) return;
    term_set_color(0x07, 0x00);
    for (int s = 0; s < sec; s++) {
        if (get_monitor_char() == 0x1B) break;
        for (int d = 0; d < 100000000; d++) __asm__ volatile("nop");
    }
}

static void __attribute__((unused)) cmd_head2(const char* arg) {
    int n = 10;
    const char* fname = arg;
    if (arg && startswith(arg, "-")) {
        n = 0; arg++;
        while (*arg >= '0' && *arg <= '9') { n = n*10 + (*arg - '0'); arg++; }
        while (*arg == ' ') arg++;
        fname = arg;
    }
    const char* src = 0; char buf[4096];
    if (pipe_stdin && (!fname || !fname[0])) { src = pipe_stdin; }
    else {
        int fi = find_file(fname); if (fi < 0) fi = find_file_path(fname);
        if (fi < 0) { term_print("head: not found\n"); return; }
        strcpy(buf, file_system[fi].content); src = buf;
    }
    int lines = 0;
    for (int i = 0; src[i] && lines < n; i++) {
        term_putc(src[i]); if (src[i] == '\n') lines++;
    }
}

static void cmd_wc2(const char* arg) {
    int flag_l = 0, flag_w = 0, flag_c = 0;
    const char* fname = arg;
    if (arg && startswith(arg, "-")) {
        arg++;
        while (*arg && *arg != ' ') {
            if (*arg == 'l') flag_l = 1;
            if (*arg == 'w') flag_w = 1;
            if (*arg == 'c') flag_c = 1;
            arg++;
        }
        while (*arg == ' ') arg++;
        fname = arg;
    }
    if (!flag_l && !flag_w && !flag_c) { flag_l = flag_w = flag_c = 1; }
    const char* src = 0; char buf[4096];
    if (pipe_stdin && (!fname || !fname[0])) { src = pipe_stdin; }
    else {
        int fi = find_file(fname); if (fi < 0) fi = find_file_path(fname);
        if (fi < 0) { term_print("wc: not found\n"); return; }
        strcpy(buf, file_system[fi].content); src = buf;
    }
    int nl = 0, nw = 0, nc = 0; int in_word = 0;
    for (int i = 0; src[i]; i++) {
        nc++;
        if (src[i] == '\n') nl++;
        if (src[i] == ' ' || src[i] == '\t' || src[i] == '\n') in_word = 0;
        else { if (!in_word) { nw++; in_word = 1; } }
    }
    if (flag_l) { term_print_int(nl); term_putc(' '); }
    if (flag_w) { term_print_int(nw); term_putc(' '); }
    if (flag_c) { term_print_int(nc); term_putc(' '); }
    if (fname && fname[0]) term_print(fname);
    term_putc('\n');
}

static int cmd_test_eval(const char* expr) {
    if (!expr || !expr[0]) return 1;
    while (*expr == ' ') expr++;
    char e[BUFFER_SIZE]; int ei = 0;
    while (*expr && ei < BUFFER_SIZE-1) e[ei++] = *expr++;
    e[ei] = '\0';
    int elen = ei;
    if (elen > 2 && e[0] == '"' && e[elen-1] == '"') {
        e[elen-1] = '\0'; return (e[1] != '\0') ? 0 : 1;
    }
    if (startswith(e, "! ")) {
        return cmd_test_eval(e + 2) == 0 ? 0 : 1;
    }
    if (startswith(e, "-n ")) {
        const char* v = e + 3; while (*v == ' ') v++;
        if (*v == '"') { v++; char tmp[64]; int ti=0; while (*v && *v!='"' && ti<63) tmp[ti++]=*v++; tmp[ti]='\0'; return tmp[0]?0:1; }
        return v[0] ? 0 : 1;
    }
    if (startswith(e, "-z ")) {
        const char* v = e + 3; while (*v == ' ') v++;
        if (*v == '"') { v++; char tmp[64]; int ti=0; while (*v && *v!='"' && ti<63) tmp[ti++]=*v++; tmp[ti]='\0'; return tmp[0]?1:0; }
        return v[0] ? 1 : 0;
    }
    if (startswith(e, "-e ") || startswith(e, "-f ") || startswith(e, "-d ")) {
        int want_dir = (e[1] == 'd');
        const char* v = e + 3; while (*v == ' ') v++;
        int fi = find_file(v); if (fi < 0) fi = find_file_path(v);
        if (fi < 0) return 1;
        if (e[1] == 'e') return 0;
        if (want_dir) return (file_system[fi].type == TYPE_DIR) ? 0 : 1;
        return (file_system[fi].type == TYPE_FILE) ? 0 : 1;
    }
    const char* ops[] = { "!=", "=", "-eq", "-ne", "-lt", "-le", "-gt", "-ge", 0 };
    for (int oi = 0; ops[oi]; oi++) {
        int ol = strlen(ops[oi]);
        for (int i = 1; i < elen - ol; i++) {
            if (e[i] == ' ' && strncmp(e+i+1, ops[oi], ol) == 0 && (e[i+1+ol] == ' ' || e[i+1+ol] == '\0')) {
                char lhs[64], rhs[64]; int li=0, ri=0;
                for (int k=0;k<i&&li<63;k++) if(e[k]!=' ') lhs[li++]=e[k];
                lhs[li]='\0';
                const char* rp = e+i+1+ol; while (*rp==' ') rp++;
                while (*rp && ri<63) rhs[ri++]=*rp++;
                rhs[ri]='\0';
                int lv = atoi(lhs), rv = atoi(rhs);
                if (oi==0) return strcmp(lhs,rhs)!=0?0:1;
                if (oi==1) return strcmp(lhs,rhs)==0?0:1;
                if (oi==2) return lv==rv?0:1;
                if (oi==3) return lv!=rv?0:1;
                if (oi==4) return lv<rv?0:1;
                if (oi==5) return lv<=rv?0:1;
                if (oi==6) return lv>rv?0:1;
                if (oi==7) return lv>=rv?0:1;
            }
        }
    }
    return strlen(e) > 0 ? 0 : 1;
}

static void cmd_printf2(const char* fmt) {
    if (!fmt) return;
    for (int i = 0; fmt[i]; i++) {
        if (fmt[i] == '\\') {
            i++;
            if (fmt[i] == 'n') term_putc('\n');
            else if (fmt[i] == 't') term_putc('\t');
            else if (fmt[i] == '\\') term_putc('\\');
            else { term_putc('\\'); term_putc(fmt[i]); }
        } else term_putc(fmt[i]);
    }
}

static void cmd_uniq(const char* arg) {
    int flag_c = 0;
    const char* fname = arg;
    if (arg && startswith(arg, "-c")) { flag_c = 1; arg += 2; while (*arg==' ') arg++; fname = arg; }
    const char* src = 0; static char buf[FILE_CONTENT_SIZE];
    if (pipe_stdin && (!fname || !fname[0])) { src = pipe_stdin; }
    else {
        int fi = find_file(fname); if (fi < 0) fi = find_file_path(fname);
        if (fi < 0) { term_print("uniq: not found\n"); return; }
        strcpy(buf, file_system[fi].content); src = buf;
    }
    char prev[BUFFER_SIZE]; prev[0] = '\0'; int cnt = 0;
    char line[BUFFER_SIZE]; int li = 0;
    for (int i = 0; ; i++) {
        char ch = src[i];
        if (ch == '\n' || ch == '\0') {
            line[li] = '\0'; li = 0;
            if (strcmp(line, prev) == 0) { cnt++; }
            else {
                if (cnt > 0) {
                    if (flag_c) { term_print_int(cnt); term_putc(' '); }
                    term_print(prev); term_putc('\n');
                }
                strcpy(prev, line); cnt = 1;
            }
            if (ch == '\0') break;
        } else if (li < BUFFER_SIZE-1) line[li++] = ch;
    }
    if (cnt > 0) {
        if (flag_c) { term_print_int(cnt); term_putc(' '); }
        term_print(prev); term_putc('\n');
    }
}

static void cmd_xargs(char* cmd) {
    if (!pipe_stdin || !pipe_stdin[0]) { term_print("xargs: no input\n"); return; }
    if (!cmd || !cmd[0]) { cmd = "echo"; }
    char full[BUFFER_SIZE];
    char arg[BUFFER_SIZE]; int ai = 0;
    for (int i = 0; ; i++) {
        char ch = pipe_stdin[i];
        if (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\0') {
            if (ai > 0) {
                arg[ai] = '\0'; ai = 0;
                int cl = strlen(cmd); int al = strlen(arg);
                if (cl + al + 1 < BUFFER_SIZE - 1) {
                    for (int k=0;k<cl;k++) full[k]=cmd[k];
                    full[cl] = ' ';
                    for (int k=0;k<=al;k++) full[cl+1+k]=arg[k];
                    execute_command(full);
                }
            }
            if (ch == '\0') break;
        } else if (ai < BUFFER_SIZE-2) arg[ai++] = ch;
    }
}

static void cmd_awk(const char* arg) {
    char prog[256]; prog[0]='\0';
    char fsep = ' ';
    const char* fname = 0;
    const char* p = arg;
    while (*p == ' ') p++;
    if (startswith(p, "-F")) {
        p += 2;
        if (*p == ':') fsep = ':';
        else if (*p == ',') fsep = ',';
        else if (*p == '\t') fsep = '\t';
        else fsep = *p;
        p++; while (*p == ' ') p++;
    }
    if (*p == '\'') {
        p++; int pi = 0;
        while (*p && *p != '\'' && pi < 255) prog[pi++] = *p++;
        prog[pi] = '\0';
        if (*p == '\'') p++;
    } else {
        int pi = 0;
        while (*p && *p != ' ' && pi < 255) prog[pi++] = *p++;
        prog[pi] = '\0';
    }
    while (*p == ' ') p++;
    fname = (*p) ? p : 0;

    const char* src = 0; static char buf[FILE_CONTENT_SIZE];
    if (pipe_stdin && (!fname || !fname[0])) { src = pipe_stdin; }
    else if (fname && fname[0]) {
        int fi = find_file(fname); if (fi < 0) fi = find_file_path(fname);
        if (fi < 0) { term_print("awk: not found\n"); return; }
        strcpy(buf, file_system[fi].content); src = buf;
    } else { term_print("awk: no input\n"); return; }

    int print_all = (strcontains(prog, "print $0") || strcmp(prog, "{print}") == 0 || strcmp(prog, "{print $0}") == 0);
    int print_field = 0;
    if (!print_all && strcontains(prog, "print $")) {
        const char* pp = prog;
        while (*pp && !startswith(pp, "print $")) pp++;
        if (*pp) { pp += 7; print_field = atoi(pp); }
    }
    char pat[64]; pat[0] = '\0'; int has_pat = 0;
    if (prog[0] == '/') {
        int pi = 1;
        while (prog[pi] && prog[pi] != '/' && pi < 63) { pat[pi-1] = prog[pi]; pi++; } pat[pi-1] = '\0';
        has_pat = 1;
    }
    int nf_mode = strcontains(prog, "print NF");
    int nr_mode = strcontains(prog, "print NR");
    int nr = 0;
    char line[BUFFER_SIZE]; int li = 0;
    for (int i = 0; ; i++) {
        char ch = src[i];
        if (ch == '\n' || ch == '\0') {
            line[li] = '\0'; li = 0; nr++;
            if (has_pat && !strcontains(line, pat)) { if (ch=='\0') break; continue; }
            char fields[16][128]; int nf = 0; int fi2 = 0;
            char* lp = line;
            while (*lp) {
                while (*lp == fsep || (*lp == ' ' && fsep == ' ')) lp++;
                if (!*lp) break;
                int k = 0;
                while (*lp && *lp != fsep && (fsep != ' ' || (*lp != ' ' && *lp != '\t')) && k < 127) fields[nf][k++] = *lp++;
                fields[nf][k] = '\0';
                if (nf < 15) nf++;
                (void)fi2;
            }
            if (nr_mode) { term_print_int(nr); term_putc('\n'); }
            else if (nf_mode) { term_print_int(nf); term_putc('\n'); }
            else if (print_field > 0 && print_field <= nf) { term_print(fields[print_field-1]); term_putc('\n'); }
            else if (print_all || (has_pat && !nf_mode && !nr_mode && !print_field)) { term_print(line); term_putc('\n'); }
            if (ch == '\0') break;
        } else if (li < BUFFER_SIZE-1) line[li++] = ch;
    }
}

static void cmd_time2(char* cmd) {
    if (!cmd || !cmd[0]) { term_print("Usage: time <command>\n"); return; }
    unsigned long t0 = jiffies;
    execute_command(cmd);
    unsigned long t1 = jiffies;
    unsigned long elapsed = t1 - t0;
    unsigned long secs = elapsed / 100;
    unsigned long cs   = elapsed % 100;
    term_set_color(0x08, 0x00);
    term_print("\nreal\t"); term_print_int(secs); term_putc('.'); if (cs<10) term_putc('0'); term_print_int(cs); term_print("s\n");
    term_set_color(0x0F, 0x00);
}

static void cmd_less(const char* arg) {
    const char* src = 0; static char buf[FILE_CONTENT_SIZE];
    if (pipe_stdin && (!arg || !arg[0])) { src = pipe_stdin; }
    else if (arg && arg[0]) {
        int fi = find_file(arg); if (fi < 0) fi = find_file_path(arg);
        if (fi < 0) { term_print("less: not found\n"); return; }
        strcpy(buf, file_system[fi].content); src = buf;
    } else { term_print("Usage: less <file>\n"); return; }

    char lines[200][BUFFER_SIZE]; int nlines = 0;
    { int i = 0, li = 0;
      while (src[i] && nlines < 199) {
          if (src[i] == '\n') { lines[nlines][li]='\0'; nlines++; li=0; }
          else if (li < BUFFER_SIZE-1) lines[nlines][li++]=src[i];
          i++;
      }
      if (li > 0) { lines[nlines][li]='\0'; nlines++; }
    }

    gui_game_start();
    int view = 0;
    int page_h = VGA_HEIGHT - HEADER_HEIGHT - 2;
    char pat[64]; pat[0] = '\0';

    for (;;) {
        ps2mouse_cursor_hide();
        term_move_cursor(0, HEADER_HEIGHT);
        for (int r = 0; r < page_h; r++) {
            term_set_color(0x0F, 0x00);
            for (int c = 0; c < VGA_WIDTH; c++) term_putc(' ');
            term_move_cursor(0, HEADER_HEIGHT + r);
            int li = view + r;
            if (li < nlines) {
                term_set_color(0x08, 0x00);
                term_print_int(li + 1); term_putc('\t');
                term_set_color(0x0F, 0x00);
                if (pat[0] && strcontains(lines[li], pat)) {
                    term_set_color(0x0E, 0x00);
                }
                term_print(lines[li]);
            }
        }
        int pct = nlines > 0 ? ((view + page_h) * 100 / nlines) : 100;
        if (pct > 100) pct = 100;
        term_move_cursor(0, VGA_HEIGHT - 2);
        term_set_color(0x70, 0x00);
        for (int i = 0; i < VGA_WIDTH; i++) term_putc(' ');
        term_move_cursor(0, VGA_HEIGHT - 2);
        term_print(" less  line "); term_print_int(view+1); term_print("/");
        term_print_int(nlines); term_print("  ("); term_print_int(pct); term_print("%)");
        if (pat[0]) { term_print("  /"); term_print(pat); }
        term_print("  [j/k/d/u/g/G//<pat>/q]");
        term_set_color(0x0F, 0x00);
        ps2mouse_cursor_show();

        int c = 0; while (!c) c = get_monitor_char();

        if (c == 'q' || c == KEY_ESC) break;
        if (c == 'j' || c == KEY_DOWN)  { if (view < nlines-1) view++; }
        if (c == 'k' || c == KEY_UP)    { if (view > 0) view--; }
        if (c == ' ' || c == KEY_PGDN)  { view += page_h; if (view > nlines-1) view = nlines-1 > 0 ? nlines-1 : 0; }
        if (c == 'b' || c == KEY_PGUP)  { view -= page_h; if (view < 0) view = 0; }
        if (c == 'g' || c == KEY_HOME)  { view = 0; }
        if (c == 'G' || c == KEY_END)   { view = nlines - page_h; if (view < 0) view = 0; }
        if (c == '/') {
            term_move_cursor(0, VGA_HEIGHT - 2);
            term_set_color(0x70, 0x00);
            for (int i = 0; i < VGA_WIDTH; i++) term_putc(' ');
            term_move_cursor(1, VGA_HEIGHT - 2);
            term_print("/");
            int pi = 0; pat[0] = '\0'; int sc2 = 0;
            for (;;) {
                sc2 = 0; while (!sc2) sc2 = get_monitor_char();
                if (sc2 == '\n' || sc2 == KEY_ESC) break;
                if (sc2 == '\b') { if (pi > 0) { pi--; pat[pi]='\0'; } }
                else if (sc2 >= 32 && sc2 < 128 && pi < 63) { pat[pi++] = (char)sc2; pat[pi] = '\0'; }
                term_move_cursor(2, VGA_HEIGHT - 2); term_print(pat);
                for (int i = strlen(pat); i < 60; i++) term_putc(' ');
            }
            if (sc2 == '\n' && pat[0]) {
                for (int i = 0; i < nlines; i++) {
                    if (strcontains(lines[i], pat)) { view = i; break; }
                }
            } else pat[0] = '\0';
        }
        if (c == 'n' && pat[0]) {
            for (int i = view+1; i < nlines; i++) {
                if (strcontains(lines[i], pat)) { view = i; break; }
            }
        }
    }
    draw_interface();
}

static void cmd_nl(const char* arg) {
    const char* src = 0; static char buf[FILE_CONTENT_SIZE];
    if (pipe_stdin && (!arg || !arg[0])) { src = pipe_stdin; }
    else if (arg && arg[0]) {
        int fi = find_file(arg); if (fi < 0) fi = find_file_path(arg);
        if (fi < 0) { term_print("nl: not found\n"); return; }
        strcpy(buf, file_system[fi].content); src = buf;
    } else { term_print("Usage: nl <file>\n"); return; }
    int n = 1; char line[BUFFER_SIZE]; int li = 0;
    for (int i = 0; ; i++) {
        char ch = src[i];
        if (ch == '\n' || ch == '\0') {
            line[li] = '\0'; li = 0;
            term_set_color(0x08, 0x00); term_print_int(n++); term_print("\t");
            term_set_color(0x0F, 0x00); term_print(line); term_putc('\n');
            if (ch == '\0') break;
        } else if (li < BUFFER_SIZE-1) line[li++] = ch;
    }
}

static void cmd_paste(const char* arg) {
    char f1[FILENAME_SIZE], f2[FILENAME_SIZE]; int i=0,j=0;
    while (arg[i] && arg[i]!=' ' && j<FILENAME_SIZE-1) f1[j++]=arg[i++];
    f1[j]='\0';
    while (arg[i]==' ') i++;
    j=0;
    while (arg[i] && j<FILENAME_SIZE-1) f2[j++]=arg[i++];
    f2[j]='\0';
    if (!f1[0] || !f2[0]) { term_print("Usage: paste <file1> <file2>\n"); return; }
    int fi1=find_file(f1); if(fi1<0) fi1=find_file_path(f1);
    int fi2=find_file(f2); if(fi2<0) fi2=find_file_path(f2);
    if(fi1<0){term_print("paste: "); term_print(f1); term_print(" not found\n"); return;}
    if(fi2<0){term_print("paste: "); term_print(f2); term_print(" not found\n"); return;}
    char lines1[64][BUFFER_SIZE], lines2[64][BUFFER_SIZE];
    int n1=0, n2=0;
    const char* c;
    c=file_system[fi1].content; int li=0;
    for(int k=0; c[k] && n1<63;k++){if(c[k]=='\n'){lines1[n1][li]='\0';n1++;li=0;}else if(li<BUFFER_SIZE-1)lines1[n1][li++]=c[k];}
    if(li>0){lines1[n1][li]='\0';n1++;}
    c=file_system[fi2].content; li=0;
    for(int k=0; c[k] && n2<63;k++){if(c[k]=='\n'){lines2[n2][li]='\0';n2++;li=0;}else if(li<BUFFER_SIZE-1)lines2[n2][li++]=c[k];}
    if(li>0){lines2[n2][li]='\0';n2++;}
    int max=n1>n2?n1:n2;
    for(int row=0;row<max;row++){
        const char* l1=(row<n1)?lines1[row]:"";
        const char* l2=(row<n2)?lines2[row]:"";
        term_print(l1); term_putc('\t'); term_print(l2); term_putc('\n');
    }
}

/* ── VGA text → framebuffer blit (used by games/TUI in GUI mode) ── */
static const unsigned int s_vga_pal[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static void gui_vga_blit(void) {
    if (!gfx_active()) return;
    const unsigned short* vb = vga_shadow_ptr();
    for (int row = 0; row < 25; row++) {
        for (int col = 0; col < 80; col++) {
            unsigned short cell = vb[row * 80 + col];
            unsigned char  c   = (unsigned char)(cell & 0xFF);
            unsigned char  att = (unsigned char)((cell >> 8) & 0xFF);
            gfx_draw_char(col * GFX_FONT_W, row * GFX_FONT_H, c,
                          s_vga_pal[att & 0x0F],
                          s_vga_pal[(att >> 4) & 0x07]);
        }
    }
}

static void gui_game_start(void) {
    if (!gui_mode) return;
    gui_game_mode = 1;
    vga_mirror_pause(1);
    gfx_fill_rect(0, 0, gfx_width(), gfx_height(), 0x000000);
}

/* ── Pixel-art arrow cursor (12 rows × 8 cols) ─────────────────── */
static const unsigned char g_cursor_mask[12] = {
    0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC,
    0xF0, 0xD8, 0x18, 0x0C, 0x0C, 0x06
};

/* Compute background colour at (px,py) from our known draw state.
   Used to erase the cursor without reading back the framebuffer. */
static unsigned int bg_at(int px, int py) {
    int H = gfx_height();
    int ty = H - TASKBAR_H;
    if (py >= ty) {
        int f = (py - ty) * 255 / TASKBAR_H;
#define _LI(A,B,sh) (((int)((A)>>(sh)&0xFF))+(((int)((B)>>(sh)&0xFF))-((int)((A)>>(sh)&0xFF)))*f/255)
        return GFX_RGB(_LI(XP_TSK_T,XP_TSK_B,16),_LI(XP_TSK_T,XP_TSK_B,8),_LI(XP_TSK_T,XP_TSK_B,0));
#undef _LI
    }
    GUIWindow* win = (desk_state==1) ? &g_twin : (desk_state==2) ? &g_wstatic : 0;
    if (win) {
        if (px>=win->x && px<win->x+win->w && py>=win->y && py<win->y+win->h)
            return TERM_BG;
        if (py>=win->y-TITLE_H && py<win->y && px>=win->x && px<win->x+win->w) {
            int f = win->w>0 ? (px-win->x)*255/win->w : 0;
#define _LI(A,B,sh) (((int)((A)>>(sh)&0xFF))+(((int)((B)>>(sh)&0xFF))-((int)((A)>>(sh)&0xFF)))*f/255)
            return GFX_RGB(_LI(XP_TBA,XP_TBB,16),_LI(XP_TBA,XP_TBB,8),_LI(XP_TBA,XP_TBB,0));
#undef _LI
        }
        if (px>=win->x-WIN_BORDER && px<win->x+win->w+WIN_BORDER &&
            py>=win->y-TITLE_H-WIN_BORDER && py<win->y+win->h+WIN_BORDER)
            return XP_BORD;
    }
    /* wallpaper gradient: top=0x02,0x06,0x14  bot=0x08,0x12,0x28 */
    { int bot = H-TASKBAR_H; int f = bot>0 ? py*255/bot : 0;
      return GFX_RGB(0x02+(0x08-0x02)*f/255, 0x06+(0x12-0x06)*f/255, 0x14+(0x28-0x14)*f/255); }
}

/* Erase cursor by repainting only its 15×14 bounding box (~210 pixels vs 456k). */
static void __attribute__((unused)) cursor_erase_fast(void) {
    if (cursor_prev_x < 0) return;
    int W = gfx_width(), H = gfx_height();
    for (int r = -1; r <= 13; r++)
        for (int c = -1; c <= 8; c++) {
            int px = cursor_prev_x+c, py = cursor_prev_y+r;
            if (px<0||px>=W||py<0||py>=H) continue;
            gfx_put_pixel(px, py, bg_at(px, py));
        }
}

static void cursor_draw(int x, int y) {
    /* save pixels underneath */
    for (int r = 0; r < 12; r++)
        for (int c = 0; c < 8; c++) {
            int px = x+c, py = y+r;
            if (px >= 0 && px < gfx_width() && py >= 0 && py < gfx_height())
                cursor_save[r][c] = /* read back */ 0; /* fb not readable; keep 0 */
            else
                cursor_save[r][c] = 0;
        }
    cursor_prev_x = x; cursor_prev_y = y;
    /* black border then white fill */
    for (int r = 0; r < 12; r++) {
        for (int c = 0; c < 8; c++) {
            if (g_cursor_mask[r] & (0x80u >> c)) {
                for (int dy2=-1;dy2<=1;dy2++)
                    for (int dx2=-1;dx2<=1;dx2++)
                        gfx_put_pixel(x+c+dx2, y+r+dy2, 0x000000);
            }
        }
    }
    for (int r = 0; r < 12; r++)
        for (int c = 0; c < 8; c++)
            if (g_cursor_mask[r] & (0x80u >> c))
                gfx_put_pixel(x+c, y+r, 0xFFFFFF);
}

/* ── Desktop icon drawing ───────────────────────────────────────── */
typedef struct { int x; int y; const char* label; } DeskIcon;
static const DeskIcon g_icons[N_ICONS] = {
    { 14,  20, "Terminal" },
    { 14,  95, "Fichiers" },
    { 14, 170, "Systeme"  },
    { 14, 245, "Casino"   },
    { 14, 320, "Jeux"     },
};

static void draw_desk_icon(int idx, int selected) {
    int x = g_icons[idx].x, y = g_icons[idx].y;
    unsigned int sel_bg = selected ? ICON_SEL : XP_DESK;
    if (selected)
        gfx_fill_rect(x-2, y-2, ICON_W+4, ICON_H+ICON_LABEL_H+4, ICON_SEL);
    switch (idx) {
        case 0: /* Terminal: dark screen + green prompt */
            gfx_fill_rect(x+2, y+2, ICON_W-4, ICON_H-4, GFX_RGB(0x08,0x08,0x14));
            gfx_draw_rect(x+2, y+2, ICON_W-4, ICON_H-4, GFX_RGB(0x30,0x60,0x30));
            gfx_draw_str(x+5,  y+8,  ">_",  GFX_RGB(0x00,0xFF,0x00), GFX_RGB(0x08,0x08,0x14));
            gfx_draw_str(x+5,  y+24, ">>",  GFX_RGB(0x00,0xC0,0x00), GFX_RGB(0x08,0x08,0x14));
            gfx_fill_rect(x+5, y+30, 16, 2, GFX_RGB(0x00,0xFF,0x00)); /* cursor blink sim */
            break;
        case 1: /* Fichiers: yellow folder with tab */
            gfx_fill_rect(x+2,  y+14, ICON_W-4, ICON_H-16, GFX_RGB(0xE8,0xB8,0x18));
            gfx_fill_rect(x+2,  y+10, 18, 6,               GFX_RGB(0xE8,0xB8,0x18));
            gfx_fill_rect(x+4,  y+18, ICON_W-8, ICON_H-22, GFX_RGB(0xFF,0xD8,0x50));
            gfx_draw_rect(x+2,  y+10, ICON_W-4, ICON_H-12, GFX_RGB(0xA0,0x70,0x08));
            gfx_draw_str(x+10, y+20, "fs", GFX_RGB(0x60,0x40,0x00), GFX_RGB(0xFF,0xD8,0x50));
            break;
        case 2: /* Systeme: monitor with GregOS screen */
            gfx_fill_rect(x+4,  y+2,  ICON_W-8,  ICON_H-16, GFX_RGB(0x28,0x28,0x28));
            gfx_fill_rect(x+7,  y+5,  ICON_W-14, ICON_H-22, GFX_RGB(0x04,0x08,0x1C));
            gfx_draw_str(x+8,  y+8,  ">>",  GFX_RGB(0x00,0xFF,0x40), GFX_RGB(0x04,0x08,0x1C));
            gfx_draw_str(x+8,  y+18, "OS",  GFX_RGB(0x00,0xC0,0x40), GFX_RGB(0x04,0x08,0x1C));
            gfx_fill_rect(x+16, y+ICON_H-14, 8, 6, GFX_RGB(0x50,0x50,0x50)); /* stand */
            gfx_fill_rect(x+8,  y+ICON_H-8,  24, 4, GFX_RGB(0x40,0x40,0x40)); /* base */
            break;
        case 3: /* Casino: golden coin + flame */
            gfx_fill_rect(x+8,  y+4,  28, 28, GFX_RGB(0xC0,0x90,0x00));
            gfx_fill_rect(x+10, y+6,  24, 24, GFX_RGB(0xFF,0xC0,0x00));
            gfx_draw_str(x+13, y+11, "GC", GFX_RGB(0x80,0x50,0x00), GFX_RGB(0xFF,0xC0,0x00));
            /* small flame */
            gfx_fill_rect(x+18, y+34, 6, 6,  GFX_RGB(0xFF,0x80,0x00));
            gfx_fill_rect(x+20, y+32, 4, 3,  GFX_RGB(0xFF,0xC0,0x00));
            break;
        case 4: /* Jeux: stylised gamepad */
            gfx_fill_rect(x+4,  y+10, ICON_W-8, ICON_H-20, GFX_RGB(0x30,0x18,0x60));
            gfx_fill_rect(x+2,  y+16, ICON_W-4, ICON_H-30, GFX_RGB(0x48,0x28,0x88));
            /* D-pad */
            gfx_fill_rect(x+10, y+18, 4, 12, GFX_RGB(0x80,0x60,0xC0));
            gfx_fill_rect(x+6,  y+22, 12, 4,  GFX_RGB(0x80,0x60,0xC0));
            /* buttons */
            gfx_fill_rect(x+28, y+18, 5, 5, GFX_RGB(0xFF,0x40,0x40));
            gfx_fill_rect(x+33, y+22, 5, 5, GFX_RGB(0xFF,0xCC,0x00));
            gfx_fill_rect(x+28, y+26, 5, 5, GFX_RGB(0x40,0xFF,0x40));
            break;
        default: break;
    }
    /* label */
    int llen = strlen(g_icons[idx].label);
    int lx = x + (ICON_W - llen * GFX_FONT_W) / 2;
    gfx_draw_str(lx, y + ICON_H + 2, g_icons[idx].label, TWHT, sel_bg);
}

/* ── XP-style window chrome ─────────────────────────────────────── */
static void gui_window_draw(GUIWindow* win) {
    if (!gfx_active()) return;
    int x = win->x, y = win->y, w = win->w, h = win->h;
    /* outer border */
    for (int i = 0; i < WIN_BORDER; i++)
        gfx_draw_rect(x - WIN_BORDER + i,
                      y - TITLE_H - WIN_BORDER + i,
                      w + (WIN_BORDER-i)*2,
                      h + TITLE_H + (WIN_BORDER-i)*2, XP_BORD);
    /* title bar gradient (left→right) */
    gfx_gradient_rect(x, y - TITLE_H, w, TITLE_H, XP_TBA, XP_TBB, 0);
    /* tiny terminal icon in title bar */
    int ix = x + 4, iy_i = y - TITLE_H + 4;
    gfx_fill_rect(ix, iy_i, 16, 13, GFX_RGB(0x08,0x08,0x18));
    gfx_draw_str(ix+1, iy_i+1, ">", GFX_RGB(0x00,0xFF,0x00), GFX_RGB(0x08,0x08,0x18));
    /* title text */
    int tx = x + 24, ty_t = y - TITLE_H + (TITLE_H - GFX_FONT_H) / 2;
    gfx_draw_str(tx+1, ty_t+1, win->title, TSHD, 0);
    gfx_draw_str(tx,   ty_t,   win->title, TWHT, 0);
    /* close button (red) */
    int by_b = y - TITLE_H + (TITLE_H - BTN_H) / 2;
    int bx = x + w - WIN_BORDER - BTN_W - 2;
    gfx_gradient_rect(bx, by_b, BTN_W, BTN_H, XP_CLSG, XP_CLSD, 1);
    gfx_draw_rect(bx, by_b, BTN_W, BTN_H, GFX_RGB(0x80,0x08,0x04));
    gfx_draw_str(bx+(BTN_W-GFX_FONT_W)/2, by_b+(BTN_H-GFX_FONT_H)/2, "x", TWHT, XP_CLSD);
    /* maximize button */
    bx -= BTN_W + 2;
    gfx_gradient_rect(bx, by_b, BTN_W, BTN_H, XP_BTNB, XP_BTND, 1);
    gfx_draw_rect(bx, by_b, BTN_W, BTN_H, GFX_RGB(0x08,0x30,0x80));
    gfx_draw_str(bx+(BTN_W-GFX_FONT_W)/2, by_b+(BTN_H-GFX_FONT_H)/2, "+", TWHT, XP_BTND);
    /* minimize button */
    bx -= BTN_W + 2;
    gfx_gradient_rect(bx, by_b, BTN_W, BTN_H, XP_BTNB, XP_BTND, 1);
    gfx_draw_rect(bx, by_b, BTN_W, BTN_H, GFX_RGB(0x08,0x30,0x80));
    gfx_draw_str(bx+(BTN_W-GFX_FONT_W)/2, by_b+(BTN_H-GFX_FONT_H)/2, "-", TWHT, XP_BTND);
    /* body highlight/shadow 3D edge */
    gfx_draw_hline(x, y, w, XP_BODLT);
    gfx_draw_vline(x, y, h, XP_BODLT);
    gfx_draw_hline(x, y+h-1, w, XP_BODSH);
    gfx_draw_vline(x+w-1, y, h, XP_BODSH);
    /* terminal content area */
    gfx_fill_rect(x, y, w, h, TERM_BG);
}

static void gui_twin_clear(GUIWindow* win) {
    for (int r = 0; r < TWIN_ROWS; r++)
        for (int c = 0; c <= TWIN_COLS; c++)
            win->buf[r][c] = '\0';
    win->cur_row = 0;
    win->cur_col = 0;
}

/* ── gui_twin_flush: render g_twin.buf to framebuffer ───────────── */
static void gui_twin_flush(GUIWindow* win) {
    if (!gfx_active()) return;
    int x = win->x, y = win->y;
    int vis_rows = win->h / GFX_FONT_H;
    int start = win->cur_row - vis_rows + 1;
    if (start < 0) start = 0;
    for (int r = 0; r < vis_rows; r++) {
        int src = start + r;
        int py  = y + r * GFX_FONT_H;
        if (src < TWIN_ROWS) {
            for (int c = 0; c < TWIN_COLS; c++) {
                char ch = win->buf[src][c];
                if (!ch) ch = ' ';
                gfx_draw_char(x + c * GFX_FONT_W, py, (unsigned char)ch,
                              TERM_FG, TERM_BG);
            }
        } else {
            gfx_fill_rect(x, py, win->w, GFX_FONT_H, TERM_BG);
        }
    }
    /* cursor block at current position */
    int cursor_row = win->cur_row - start;
    if (cursor_row >= 0 && cursor_row < vis_rows) {
        int cx = x + win->cur_col * GFX_FONT_W;
        int cy = y + cursor_row * GFX_FONT_H;
        gfx_fill_rect(cx, cy, GFX_FONT_W, GFX_FONT_H, TERM_CUR);
        char ch = (win->cur_col < TWIN_COLS) ? win->buf[win->cur_row][win->cur_col] : 0;
        if (!ch) ch = ' ';
        gfx_draw_char(cx, cy, (unsigned char)ch, TERM_BG, TERM_CUR);
    }
    /* fill any remaining area below last row */
    int used_h = vis_rows * GFX_FONT_H;
    if (used_h < win->h)
        gfx_fill_rect(x, y + used_h, win->w, win->h - used_h, TERM_BG);
}

/* ── gui_twin_scroll / putc / print ─────────────────────────────── */
static void gui_twin_scroll(GUIWindow* win) {
    for (int r = 0; r < TWIN_ROWS - 1; r++) {
        for (int i = 0; i < TWIN_COLS; i++)
            win->buf[r][i] = win->buf[r+1][i];
        win->buf[r][TWIN_COLS] = '\0';
    }
    for (int i = 0; i <= TWIN_COLS; i++) win->buf[TWIN_ROWS-1][i] = '\0';
    win->cur_row = TWIN_ROWS - 1;
}

static void gui_twin_putc(GUIWindow* win, char c) {
    if (c == '\n') {
        win->cur_col = 0;
        if (win->cur_row < TWIN_ROWS - 1) win->cur_row++;
        else gui_twin_scroll(win);
        return;
    }
    if (c == '\b') {
        if (win->cur_col > 0) {
            win->cur_col--;
            win->buf[win->cur_row][win->cur_col] = ' ';
        }
        return;
    }
    if ((unsigned char)c < 0x20) return;
    if (win->cur_col >= TWIN_COLS) {
        win->cur_col = 0;
        if (win->cur_row < TWIN_ROWS - 1) win->cur_row++;
        else gui_twin_scroll(win);
    }
    win->buf[win->cur_row][win->cur_col] = c;
    win->cur_col++;
}

static void gui_twin_print(GUIWindow* win, const char* s) {
    while (*s) gui_twin_putc(win, *s++);
}

/* ── XP Taskbar ─────────────────────────────────────────────────── */
static void xp_taskbar_full_draw(void) {
    if (!gfx_active()) return;
    int W = gfx_width(), H = gfx_height();
    int ty = H - TASKBAR_H;
    /* gradient background */
    gfx_gradient_rect(0, ty, W, TASKBAR_H, XP_TSK_T, XP_TSK_B, 1);
    /* top highlight line */
    gfx_draw_hline(0, ty, W, XP_TSK_HL);
    /* Start button (green pill) */
    int sw = 72, sh = TASKBAR_H - 6, sx = 2, sy = ty + 3;
    gfx_gradient_rect(sx, sy, sw, sh, XP_SRTG, XP_SRTD, 1);
    gfx_draw_rect(sx, sy, sw, sh, GFX_RGB(0x04,0x28,0x04));
    /* Drakkar flame icon */
    int fx = sx + 4, fy = sy + (sh - 12) / 2;
    gfx_fill_rect(fx+2, fy+0, 4, 3, GFX_RGB(0xFF,0xFF,0x40));  /* yellow tip */
    gfx_fill_rect(fx+1, fy+2, 6, 4, GFX_RGB(0xFF,0x90,0x00));  /* orange mid */
    gfx_fill_rect(fx+0, fy+5, 8, 4, GFX_RGB(0xFF,0x40,0x00));  /* red-orange */
    gfx_fill_rect(fx+1, fy+8, 6, 3, GFX_RGB(0xCC,0x10,0x00));  /* deep red base */
    gfx_fill_rect(fx+2, fy+10,4, 2, GFX_RGB(0x80,0x08,0x00));  /* ember */
    gfx_draw_str(sx+15, sy+(sh-GFX_FONT_H)/2, "GregOS", TWHT, XP_SRTD);
    /* Active window button */
    if (desk_state == 1) {
        int bx = 76, by = ty + 3, bw = 130, bh = TASKBAR_H - 6;
        gfx_gradient_rect(bx, by, bw, bh, XP_BTNB, XP_BTND, 1);
        gfx_draw_rect(bx, by, bw, bh, GFX_RGB(0x08,0x28,0x80));
        gfx_draw_str(bx + 4, by + (bh-GFX_FONT_H)/2, g_twin.title, TWHT, XP_BTND);
    }
}

static void gui_taskbar_update(void) {
    if (!gfx_active()) return;
    char ts[12]; get_time_string(ts);
    int W = gfx_width(), H = gfx_height();
    int ty = H - TASKBAR_H;
    int cw = 10 * GFX_FONT_W + 4, cx = W - cw - 2;
    int cy = ty + (TASKBAR_H - GFX_FONT_H) / 2;
    gfx_fill_rect(cx - 2, ty + 2, cw + 4, TASKBAR_H - 4, GFX_RGB(0x0A,0x40,0x98));
    gfx_draw_str(cx, cy, ts, TWHT, GFX_RGB(0x0A,0x40,0x98));
}

/* ── Full desktop repaint ───────────────────────────────────────── */
static void gui_desktop_draw(void) {
    if (!gfx_active()) return;
    int W = gfx_width(), H = gfx_height();
    /* wallpaper: midnight navy gradient (top=darker, bottom=slightly lighter) */
    gfx_gradient_rect(0, 0, W, H - TASKBAR_H,
                      GFX_RGB(0x02,0x06,0x14), GFX_RGB(0x08,0x12,0x28), 1);
    /* GregOS title watermark */
    {
        int cx = W/2, cy = (H - TASKBAR_H)/2 - 40;
        gfx_draw_str(cx - 4*GFX_FONT_W,  cy,               "GregOS v2.0",
                     GFX_RGB(0x18,0x40,0x18), GFX_RGB(0x04,0x0C,0x1E));
        gfx_draw_str(cx - 10*GFX_FONT_W, cy + GFX_FONT_H,
                     "Seigneur du Kernel - Gardien des Bits",
                     GFX_RGB(0x10,0x28,0x10), GFX_RGB(0x04,0x0C,0x1E));
        /* compact dragon ASCII art */
        const char* dragon[5] = {
            "   ==(W{==========-",
            "     ||  (.--.)    ",
            "     | \\_,|**|,__ ",
            "  ___/-==|  /`\\_.  ",
            "(^(~     `-'  _-~` ",
        };
        unsigned int dc = GFX_RGB(0x12,0x30,0x12);
        unsigned int db = GFX_RGB(0x04,0x0C,0x1E);
        int dl = cx - 10*GFX_FONT_W, dy2 = cy + GFX_FONT_H*3;
        for (int i = 0; i < 5; i++)
            gfx_draw_str(dl, dy2 + i*GFX_FONT_H, dragon[i], dc, db);
    }
    /* desktop icons */
    for (int i = 0; i < N_ICONS; i++)
        draw_desk_icon(i, i == sel_icon);
    /* taskbar */
    xp_taskbar_full_draw();
    gui_taskbar_update();
    /* open window */
    if (desk_state == 1) {
        gui_window_draw(&g_twin);
        gui_twin_flush(&g_twin);
    } else if (desk_state == 2) {
        gui_window_draw(&g_wstatic);
        gui_twin_flush(&g_wstatic);
    }
    /* mouse cursor on top */
    cursor_draw(ps2mouse_gui_x(), ps2mouse_gui_y());
}

/* ── gui_refresh: incremental refresh (terminal + clock + cursor) ─ */
static void __attribute__((unused)) gui_refresh(void) {
    if (!gfx_active()) return;
    if (desk_state == 1)      gui_twin_flush(&g_twin);
    else if (desk_state == 2) gui_twin_flush(&g_wstatic);
    gui_taskbar_update();
    cursor_draw(ps2mouse_gui_x(), ps2mouse_gui_y());
}

/* ── Handle mouse click ─────────────────────────────────────────── */
static void handle_gui_click(int mx, int my) {
    /* Desktop icon and taskbar handling moved to WindowManager::dispatch_event().
       This function is now a no-op; the call site is kept for compatibility. */
    (void)mx; (void)my;
}

/* ── Static info window helpers ─────────────────────────────────── */

static void gui_wstatic_int(int n) {
    char tmp[12]; int i = 0, neg = 0;
    if (n < 0) { neg = 1; n = -n; }
    if (n == 0) { tmp[i++] = '0'; }
    while (n > 0) { tmp[i++] = '0' + n % 10; n /= 10; }
    if (neg) tmp[i++] = '-';
    for (int a=0,b=i-1; a<b; a++,b--) { char t=tmp[a]; tmp[a]=tmp[b]; tmp[b]=t; }
    tmp[i] = '\0';
    gui_twin_print(&g_wstatic, tmp);
}

static void __attribute__((unused)) gui_open_files(void) {
    gui_twin_clear(&g_wstatic);
    g_wstatic.title = "GregOS :: Gestionnaire de Fichiers";
    gui_twin_print(&g_wstatic, "  Racine /\n");
    gui_twin_print(&g_wstatic, "  -------\n\n");
    int fc = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (!file_system[i].exists) continue;
        fc++;
        if (file_system[i].type == TYPE_DIR) gui_twin_print(&g_wstatic, "  [REP] ");
        else                                 gui_twin_print(&g_wstatic, "  [FIC] ");
        gui_twin_print(&g_wstatic, file_system[i].name);
        if (file_system[i].type == TYPE_FILE) {
            gui_twin_print(&g_wstatic, " (");
            gui_wstatic_int(file_system[i].size);
            gui_twin_print(&g_wstatic, "o)");
        }
        gui_twin_print(&g_wstatic, "\n");
    }
    gui_twin_print(&g_wstatic, "\n  Total: ");
    gui_wstatic_int(fc);
    gui_twin_print(&g_wstatic, " entrees  |  [X] pour fermer\n");
    desk_state = 2;
    gui_desktop_draw();
}

static void __attribute__((unused)) gui_open_sysinfo(void) {
    gui_twin_clear(&g_wstatic);
    g_wstatic.title = "GregOS :: Informations Systeme";
    char ts[12], ds[12]; get_time_string(ts); get_date_string(ds);
    unsigned long sec = jiffies / 100;
    int uh = (int)(sec/3600), um = (int)((sec%3600)/60), us2 = (int)(sec%60);
    int fc = 0; for(int i=0;i<MAX_FILES;i++) if(file_system[i].exists) fc++;
    gui_twin_print(&g_wstatic,
        "  ==(W{==========-\n"
        "    ||  (.--.)       GregOS v2.0\n"
        "    | \\_,|**|,__     ------\n"
        " ___/-==|  /`\\_.     OS:     GregOS 2.0 i386\n"
        "(^(~     `-'  -~`    Kernel: bare-metal C/ASM\n"
        "                     Shell:  gregsh\n\n");
    gui_twin_print(&g_wstatic, "  Date:   "); gui_twin_print(&g_wstatic, ds);
    gui_twin_print(&g_wstatic, "\n  Heure:  "); gui_twin_print(&g_wstatic, ts);
    gui_twin_print(&g_wstatic, "\n  Uptime: ");
    gui_wstatic_int(uh); gui_twin_print(&g_wstatic, "h ");
    gui_wstatic_int(um); gui_twin_print(&g_wstatic, "m ");
    gui_wstatic_int(us2); gui_twin_print(&g_wstatic, "s\n");
    gui_twin_print(&g_wstatic, "\n  CPU:    QEMU i386 Virtual CPU");
    gui_twin_print(&g_wstatic, "\n  RAM:    256 MB");
    gui_twin_print(&g_wstatic, "\n  GPU:    Bochs VBE 800x600x32");
    gui_twin_print(&g_wstatic, "\n  FS:     ");
    gui_wstatic_int(fc); gui_twin_print(&g_wstatic, "/64 entrees");
    gui_twin_print(&g_wstatic, "\n  Casino: ");
    gui_wstatic_int(casino_balance); gui_twin_print(&g_wstatic, " GregCoins");
    gui_twin_print(&g_wstatic, "\n\n  [X] pour fermer\n");
    desk_state = 2;
    gui_desktop_draw();
}

/* ── Start Menu and system bridges ─────────────────────────────────── */
/* KERNEL PANIC — Doom-64-like FPS, defined in kernel/doom.c (separate TU). */
extern void gg_kernel_panic(void);

/* launch_arcade_game: legacy blocking launcher (terminal commands still use it).
   Suspends the GUI EventQueue, runs the blocking gg_*() game loop,
   then restores GUI routing and redraws the desktop.                    */
void launch_arcade_game(int n)
{
    is_gui_active = 0;
    switch (n) {
        case 0: gg_snake();       break;
        case 1: gg_tetris();      break;
        case 2: gg_pong();        break;
        case 3: gg_invaders();    break;
        case 4: gg_breakout();    break;
        case 5: gg_2048();        break;
        case 6: gg_minesweeper(); break;
        case 7: gg_simon();       break;
        case 8: gg_matrix();      break;
        case 9: gg_clock();       break;
        case 10: gg_kernel_panic(); break;
    }
    is_gui_active = 1;
    kb_inject_flush();
    draw_interface();
}

/* ── Async arcade launcher (Phase 3 — used by GamesWindow via ArcadeApp) ──
   Spawns the selected game as a Scheduler thread so the Compositor keeps
   running.  ArcadeApp::close_requested() polls arcade_game_is_done().    */
static volatile int s_arcade_game_pending = -1;
static volatile int s_arcade_game_done    = 1; /* 1 = idle, 0 = running  */

static void arcade_game_thread(void)
{
    int n = s_arcade_game_pending;
    s_arcade_game_pending = -1;
    switch (n) {
        case 0: gg_snake();       break;
        case 1: gg_tetris();      break;
        case 2: gg_pong();        break;
        case 3: gg_invaders();    break;
        case 4: gg_breakout();    break;
        case 5: gg_2048();        break;
        case 6: gg_minesweeper(); break;
        case 7: gg_simon();       break;
        case 8: gg_matrix();      break;
        case 9: gg_clock();       break;
        case 10: gg_kernel_panic(); break;
    }
    /* Restore GUI event routing before signalling completion. */
    is_gui_active = 1;
    s_arcade_game_done = 1;
}

void launch_arcade_game_async(int n)
{
    /* Route IRQ1 directly to kb_buf so the game thread's get_monitor_char()
       receives keypresses (same as the blocking launch_arcade_game path).  */
    is_gui_active         = 0;
    s_arcade_game_done    = 0;
    s_arcade_game_pending = n;
    scheduler_spawn(arcade_game_thread);
}

int arcade_game_is_done(void)
{
    return s_arcade_game_done;
}

void start_menu_launch_games(void)   { open_games_window(); }
void start_menu_launch_sysinfo(void) { open_system_window(); }

int sys_get_file_count(void) {
    int fc = 0;
    for (int i = 0; i < MAX_FILES; i++)
        if (file_system[i].exists) fc++;
    return fc;
}

static void __attribute__((unused)) gui_open_games(void) {
    gui_twin_clear(&g_wstatic);
    g_wstatic.title = "GregOS :: Arcade de Jeux";
    gui_twin_print(&g_wstatic,
        "  Jeux disponibles - tape le nom dans le Terminal\n"
        "  ================================================\n\n"
        "  snake      Snake v2  (collecte des pommes)\n"
        "  tetris     Tetris v2 (piece fantome)\n"
        "  invaders   Space Invaders v2\n"
        "  pong       Pong v2\n"
        "  breakout   Breakout\n"
        "  2048       2048 - additionne les tuiles\n"
        "  minesweep  Demineur\n"
        "  simon      Simon - memoire musicale\n"
        "  matrix     Pluie de matrix\n"
        "  clock      Horloge en direct\n\n"
        "  Casino:\n"
        "  casino     Lobby (roulette/blackjack/slots/plinko)\n\n"
        "  Astuce: ESC = quitter, P = pause\n\n"
        "  [X] pour fermer\n");
    desk_state = 2;
    gui_desktop_draw();
}

/* ── Mirror callback registered with vga.c ─────────────────────── */
static void gui_char_mirror(char c) { gui_twin_putc(&g_twin, c); }

/* ── gui_desktop_init: called once after login ──────────────────── */
static void gui_desktop_init(void) {
    if (!gfx_active()) return;
    int W = gfx_width(), H = gfx_height();
    /* Terminal window: centred, almost full screen */
    int win_x = 80;
    int win_y = TITLE_H + WIN_BORDER + 10;
    int win_w = W - 160;
    int win_h = H - TASKBAR_H - TITLE_H - WIN_BORDER*2 - 20;
    g_twin.x = win_x; g_twin.y = win_y; g_twin.w = win_w; g_twin.h = win_h;
    g_twin.title = "GregOS Terminal :: root@gregos";
    g_twin.focused = 1;
    gui_twin_clear(&g_twin);
    /* static info window — same geometry */
    g_wstatic.x = win_x; g_wstatic.y = win_y; g_wstatic.w = win_w; g_wstatic.h = win_h;
    g_wstatic.title   = "GregOS :: Info";
    g_wstatic.focused = 1;
    gui_twin_clear(&g_wstatic);
    /* register mirror so all term_putc output flows to g_twin.buf */
    vga_set_gui_mirror(gui_char_mirror);
    desk_state = 0;
    sel_icon   = 0;
    gui_desktop_draw();
}

void execute_command(char* buf) {
    static int alias_depth = 0;

    {
        int has_dollar = 0;
        for (int _i = 0; buf[_i]; _i++) if (buf[_i] == '$') { has_dollar = 1; break; }
        if (has_dollar) {
            char _expanded[BUFFER_SIZE];
            expand_env(buf, _expanded, BUFFER_SIZE);
            int _el = strlen(_expanded);
            if (_el < BUFFER_SIZE) { strcpy(buf, _expanded); }
        }
    }

    for (int ai = 0; ai < alias_count; ai++) {
        int al = strlen(alias_names[ai]);
        if (strncmp(buf, alias_names[ai], al) == 0 &&
            (buf[al] == '\0' || buf[al] == ' ')) {
            if (alias_depth >= 8) { term_print("gregos: alias loop detected\n"); return; }
            char expanded[BUFFER_SIZE];
            strcpy(expanded, alias_cmds[ai]);
            if (buf[al] == ' ') {
                strcat(expanded, buf + al);
            }
            alias_depth++;
            execute_command(expanded);
            alias_depth--;
            return;
        }
    }



    {
        for (int i = 0; buf[i]; i++) {
            if (buf[i] == '&' && buf[i+1] == '&') {
                char left[BUFFER_SIZE], right[BUFFER_SIZE];
                int le = i - 1; while (le > 0 && buf[le] == ' ') le--;
                strncpy(left, buf, le + 2); left[le + 1] = '\0';  /* n-1 copy → keep buf[le] */
                int rs = i + 2; while (buf[rs] == ' ') rs++;
                strncpy(right, buf + rs, BUFFER_SIZE);
                last_exit_code = 0;
                execute_command(left);
                if (last_exit_code == 0) execute_command(right);
                return;
            }
        }
    }
    {
        for (int i = 0; buf[i]; i++) {
            if (buf[i] == '|' && buf[i+1] == '|') {
                char left[BUFFER_SIZE], right[BUFFER_SIZE];
                int le = i - 1; while (le > 0 && buf[le] == ' ') le--;
                strncpy(left, buf, le + 2); left[le + 1] = '\0';  /* n-1 copy → keep buf[le] */
                int rs = i + 2; while (buf[rs] == ' ') rs++;
                strncpy(right, buf + rs, BUFFER_SIZE);
                last_exit_code = 0;
                execute_command(left);
                if (last_exit_code != 0) execute_command(right);
                return;
            }
        }
    }
    {
        int len = strlen(buf);
        if (len > 1 && buf[len-1] == '&' && (buf[len-2] == ' ' || len == 2)) {
            char cmd[BUFFER_SIZE]; strncpy(cmd, buf, len-1); cmd[len-1] = '\0';
            int e = len - 2; while (e > 0 && cmd[e] == ' ') cmd[e--] = '\0';
            term_set_color(0x0E, 0x00); term_print("[1] "); term_print(cmd); term_putc('\n');
            term_set_color(0x0F, 0x00);
            execute_command(cmd);
            term_set_color(0x0A, 0x00); term_print("[1]+ Done    "); term_print(cmd); term_putc('\n');
            term_set_color(0x0F, 0x00);
            return;
        }
    }
    {
        int pipe_idx = -1;
        for (int i = 0; buf[i]; i++) {
            if (buf[i] == '|' && buf[i+1] != '|') { pipe_idx = i; break; }
        }
        if (pipe_idx > 0) {
            char cmd1[BUFFER_SIZE], cmd2[BUFFER_SIZE];
            int e1 = pipe_idx - 1;
            while (e1 > 0 && buf[e1] == ' ') e1--;
            int len1 = e1 + 1;
            if (len1 > BUFFER_SIZE - 1) len1 = BUFFER_SIZE - 1;
            strncpy(cmd1, buf, len1 + 1);
            int s2 = pipe_idx + 1;
            while (buf[s2] == ' ') s2++;
            strncpy(cmd2, buf + s2, BUFFER_SIZE);
            term_capture_start();
            execute_command(cmd1);
            const char* cap = term_capture_end();
            pipe_stdin = cap;
            execute_command(cmd2);
            pipe_stdin = 0;
            return;
        }
    }



    if (strcmp(buf, "echo") == 0) { term_putc('\n'); return; }
    if (startswith(buf, "echo ")) {
        char* rest = buf + 5;




        char text[BUFFER_SIZE]; int ti = 0;
        int append = 0; char fname[FILENAME_SIZE]; int found_redir = 0;
        for (int i = 0; rest[i]; i++) {
            if (rest[i] == '>' ) {
                text[ti] = '\0';


                while (ti > 0 && text[ti-1] == ' ') { ti--; text[ti] = '\0'; }
                if (rest[i+1] == '>') { append=1; i+=2; } else { i++; }
                while (rest[i] == ' ') i++;
                int fi2 = 0;
                while (rest[i] && fi2 < FILENAME_SIZE-1) fname[fi2++] = rest[i++];
                fname[fi2] = '\0';
                found_redir = 1; break;
            }
            text[ti++] = rest[i];
        }
        text[ti] = '\0';

        if (found_redir && fname[0]) {
            int idx = find_file(fname);
            if (idx == -1) {
                int sl = next_free_slot();
                if (sl == -1) { term_print("Disk full.\n"); return; }
                strncpy(file_system[sl].name, fname, FILENAME_SIZE);
                file_system[sl].type = TYPE_FILE;
                file_system[sl].exists = 1;
                file_system[sl].parent_id = current_dir_id;
                file_system[sl].id = sl + 1;
                file_system[sl].content[0] = '\0';
                file_system[sl].size = 0;
                idx = sl;
            }
            if (append) {
                int sz = file_system[idx].size;
                if (sz + ti + 1 < FILE_CONTENT_SIZE) {
                    strcat(file_system[idx].content, text);
                    strcat(file_system[idx].content, "\n");
                    file_system[idx].size = sz + ti + 1;
                }
            } else {
                strcpy(file_system[idx].content, text);
                strcat(file_system[idx].content, "\n");
                file_system[idx].size = ti + 1;
            }
            term_print("Written.\n");
        } else {
            char expanded[BUFFER_SIZE];
            expand_env(buf + 5, expanded, BUFFER_SIZE);
            term_print(expanded); term_putc('\n');
        }
        return;
    }



    if (strcmp(buf, "ls") == 0 || strcmp(buf, "ls -l") == 0
     || strcmp(buf, "ls -la") == 0 || strcmp(buf, "ls -a") == 0
     || strcmp(buf, "ls -al") == 0) {
        int long_fmt = (strcontains(buf, "-l"));
        term_set_color(0x08, 0x00);
        if (long_fmt)
            term_print("TYPE  SIZE    PERMS  NAME\n");
        else
            term_print("NAME                  TYPE    SIZE\n");
        term_set_color(0x0F, 0x00);
        for (int i = 0; i < MAX_FILES; i++) {
            if (!file_system[i].exists || file_system[i].parent_id != current_dir_id) continue;
            if (file_system[i].type == TYPE_DIR) {
                if (long_fmt) {
                    term_set_color(0x0B, 0x00); term_print("dir   ");
                    term_set_color(0x08, 0x00); term_print("    -  ");
                    term_print("drwxr-xr-x  ");
                    term_set_color(0x09, 0x00);
                    term_print(file_system[i].name); term_print("/");
                } else {
                    term_set_color(0x09, 0x00);
                    term_print(file_system[i].name); term_print("/");
                    int pad = 21 - strlen(file_system[i].name);
                    for (int s=0;s<pad;s++) term_putc(' ');
                    term_set_color(0x0B, 0x00); term_print("DIR");
                }
            } else {
                if (long_fmt) {
                    term_set_color(0x0A, 0x00); term_print("file  ");
                    term_set_color(0x0F, 0x00);
                    term_print_int(file_system[i].size);


                    int sz = file_system[i].size; int digits = (sz==0)?1:0;
                    for (int s=sz; s>0; s/=10) digits++;
                    for (int s=digits;s<6;s++) term_putc(' ');
                    term_set_color(0x08, 0x00); term_print("-rw-r--r--  ");
                    term_set_color(0x0F, 0x00);
                    term_print(file_system[i].name);
                } else {
                    term_set_color(0x0F, 0x00);
                    term_print(file_system[i].name);
                    int pad = 22 - strlen(file_system[i].name);
                    for (int s=0;s<pad;s++) term_putc(' ');
                    term_set_color(0x0A, 0x00); term_print("FILE  ");
                    term_set_color(0x0F, 0x00);
                    term_print_int(file_system[i].size); term_print(" B");
                }
            }
            term_set_color(0x0F, 0x00);
            term_putc('\n');
        }
        return;
    }



    if (strcmp(buf, "cd") == 0 || strcmp(buf, "cd ~") == 0) {
        current_dir_id = 0; beep_tick(); draw_interface(); return;
    }
    if (startswith(buf, "cd ")) {
        char* name = buf + 3;
        if (strcmp(name, "..") == 0) {
            if (current_dir_id == 0) { draw_interface(); return; }
            for (int i = 0; i < MAX_FILES; i++) {
                if (file_system[i].exists && file_system[i].type == TYPE_DIR
                    && file_system[i].id == current_dir_id) {
                    current_dir_id = file_system[i].parent_id; break;
                }
            }
        } else if (strcmp(name, "/") == 0 || strcmp(name, "~") == 0) {
            current_dir_id = 0;
        } else {


            int idx = find_file_path(name);
            if (idx == -1) {


                int found = 0;
                for (int i = 0; i < MAX_FILES; i++) {
                    if (file_system[i].exists && file_system[i].type == TYPE_DIR
                        && strcmp(file_system[i].name, name) == 0
                        && file_system[i].parent_id == current_dir_id)
                        { current_dir_id = file_system[i].id; found = 1; break; }
                }
                if (!found) { term_print("cd: no such directory.\n"); return; }
            } else {
                if (file_system[idx].type != TYPE_DIR) { term_print("cd: not a directory.\n"); return; }
                current_dir_id = file_system[idx].id;
            }
        }
        beep_tick();
        draw_interface();
        return;
    }



    if (startswith(buf, "mkdir ")) {
        int sl = next_free_slot();
        if (sl == -1) { term_print("Disk full.\n"); return; }
        strncpy(file_system[sl].name, buf+6, FILENAME_SIZE);
        file_system[sl].type = TYPE_DIR; file_system[sl].exists = 1;
        file_system[sl].parent_id = current_dir_id; file_system[sl].id = sl+1;
        file_system[sl].content[0] = '\0'; file_system[sl].size = 0;
        beep_tick();
        term_print("Directory created.\n");
        return;
    }



    if (startswith(buf, "touch ")) {
        int sl = next_free_slot();
        if (sl == -1) { term_print("Disk full.\n"); return; }
        strncpy(file_system[sl].name, buf+6, FILENAME_SIZE);
        file_system[sl].type = TYPE_FILE; file_system[sl].exists = 1;
        file_system[sl].parent_id = current_dir_id; file_system[sl].id = sl+1;
        file_system[sl].content[0] = '\0'; file_system[sl].size = 0;
        term_print("File created.\n");
        return;
    }



    if (startswith(buf, "rm ")) {
        int idx = find_file(buf+3);
        if (idx == -1) { term_print("rm: file not found.\n"); return; }
        file_system[idx].exists = 0;
        beep_tick();
        term_print("Deleted.\n");
        return;
    }



    if (startswith(buf, "cat ")) {
        int show_ends = 0;
        const char* arg = buf + 4;
        if (startswith(arg, "-e ")) { show_ends = 1; arg += 3; }
        int idx = find_file(arg);
        if (idx == -1) { term_print("cat: file not found.\n"); return; }
        if (file_system[idx].type == TYPE_DIR) { term_print("cat: is a directory.\n"); return; }
        const char* c = file_system[idx].content;
        for (int i = 0; c[i]; i++) {
            if (show_ends && c[i] == '\n') {
                term_set_color(0x08, 0x00); term_putc('$');
                term_set_color(0x0F, 0x00);
            }
            term_putc(c[i]);
        }
        if (file_system[idx].size > 0 && c[file_system[idx].size-1] != '\n')
            term_putc('\n');
        return;
    }



    if (startswith(buf, "grep ")) {
        int flag_i=0, flag_n=0, flag_r=0, flag_v=0, flag_c=0, flag_l=0;
        int i=5;
        while (buf[i] == '-') {
            i++;
            while (buf[i] && buf[i] != ' ') {
                if (buf[i]=='i') flag_i=1;
                if (buf[i]=='n') flag_n=1;
                if (buf[i]=='r') flag_r=1;
                if (buf[i]=='v') flag_v=1;
                if (buf[i]=='c') flag_c=1;
                if (buf[i]=='l') flag_l=1;
                i++;
            }
            while (buf[i]==' ') i++;
        }
        char pattern[64], fname[FILENAME_SIZE]; int j=0;
        while (buf[i] && buf[i]!=' ' && j<63) pattern[j++]=buf[i++];
        pattern[j]='\0'; while (buf[i]==' ') i++;
        j=0; while (buf[i] && j<FILENAME_SIZE-1) fname[j++]=buf[i++]; fname[j]='\0';

        if (flag_r) {
            int matches=0;
            for (int fi=0;fi<MAX_FILES;fi++) {
                if (!file_system[fi].exists||file_system[fi].type!=TYPE_FILE) continue;
                const char* gc=file_system[fi].content;
                if (!gc) continue;
                char line[BUFFER_SIZE]; int li=0, lineno=0, fmatches=0;
                for (int k=0;;k++) {
                    char ch=gc[k];
                    if (ch=='\n'||ch=='\0') {
                        line[li]='\0'; lineno++;
                        int hit=flag_i?strcontains_nocase(line,pattern):strcontains(line,pattern);
                        if (flag_v) hit=!hit;
                        if (hit) {
                            fmatches++; matches++;
                            if (!flag_c && !flag_l) {
                                term_set_color(0x0B,0x00);
                                term_print(file_system[fi].name); term_putc(':');
                                if (flag_n) { term_print_int(lineno); term_putc(':'); }
                                term_set_color(0x0E,0x00); term_print(line); term_putc('\n');
                                term_set_color(0x0F,0x00);
                            }
                        }
                        li=0; if (ch=='\0') break;
                    } else if (li<BUFFER_SIZE-1) line[li++]=ch;
                }
                if (flag_l && fmatches > 0) { term_print(file_system[fi].name); term_putc('\n'); }
                if (flag_c && fmatches > 0) { term_print(file_system[fi].name); term_putc(':'); term_print_int(fmatches); term_putc('\n'); }
            }
            if (!matches) { last_exit_code = 1; term_print("(no match)\n"); }
            return;
        }

        const char* gc = 0;
        if (fname[0] == '\0' && pipe_stdin) {
            gc = pipe_stdin;
        } else {
            int idx=find_file(fname); if (idx<0) idx=find_file_path(fname);
            if (idx==-1) { term_print("grep: file not found.\n"); return; }
            gc=file_system[idx].content;
        }
        char line[BUFFER_SIZE]; int li=0, matches=0, lineno=0;
        for (int k=0; ; k++) {
            char ch=gc[k];
            if (ch=='\n' || ch=='\0') {
                line[li]='\0'; lineno++;
                int hit = flag_i ? strcontains_nocase(line,pattern) : strcontains(line,pattern);
                if (flag_v) hit=!hit;
                if (hit) {
                    matches++;
                    if (!flag_c && !flag_l) {
                        if (flag_n) { term_set_color(0x08,0x00); term_print_int(lineno); term_print(": "); term_set_color(0x0F,0x00); }
                        term_print("\x1b[33m"); term_print(line); term_print("\x1b[0m\n");
                    }
                }
                li=0; if (ch=='\0') break;
            } else if (li<BUFFER_SIZE-1) line[li++]=ch;
        }
        if (flag_c) { term_print_int(matches); term_putc('\n'); }
        else if (flag_l && matches > 0) { term_print(fname[0]?fname:"(stdin)"); term_putc('\n'); }
        else if (!matches) { last_exit_code = 1; term_print("(no match)\n"); }
        return;
    }



    if (strcmp(buf, "wc") == 0) {
        if (pipe_stdin) { cmd_wc2(""); }
        else term_print("Usage: wc [-lwc] <file>\n");
        return;
    }
    if (startswith(buf, "wc ")) {
        int flag_l=0, flag_w=0, flag_c=0;
        const char* warg = buf+3;
        while (*warg == '-') {
            warg++;
            while (*warg && *warg != ' ') {
                if (*warg=='l') flag_l=1;
                if (*warg=='w') flag_w=1;
                if (*warg=='c') flag_c=1;
                warg++;
            }
            while (*warg==' ') warg++;
        }
        const char* c = 0;
        if (*warg == '\0' && pipe_stdin) {
            c = pipe_stdin;
        } else {
            int idx=find_file(warg);
            if (idx==-1) { term_print("wc: file not found.\n"); return; }
            c=file_system[idx].content;
        }
        int chars=0, words=0, lines=0, in_word=0;
        for (int i=0; c[i]; i++) {
            chars++;
            if (c[i]=='\n') lines++;
            if (c[i]==' '||c[i]=='\n'||c[i]=='\t') in_word=0;
            else { if(!in_word){words++;in_word=1;} }
        }
        if (chars>0 && c[chars-1]!='\n') lines++;
        if (!flag_l && !flag_w && !flag_c) {
            term_print_int(lines); term_putc('\t');
            term_print_int(words); term_putc('\t');
            term_print_int(chars); term_putc('\t');
            term_print(warg); term_putc('\n');
        } else {
            if (flag_l) { term_print_int(lines); term_putc('\n'); }
            if (flag_w) { term_print_int(words); term_putc('\n'); }
            if (flag_c) { term_print_int(chars); term_putc('\n'); }
        }
        return;
    }



    if (startswith(buf, "find ")) {


        char namepat[64]=""; int type_filter=0;

        int i=5;
        while (buf[i]==' ') i++;


        if (buf[i]=='.'||buf[i]=='/') { while(buf[i]&&buf[i]!=' ') i++; while(buf[i]==' ') i++; }
        while (buf[i]) {
            if (buf[i]=='-') {
                i++;
                if (buf[i]=='n'&&buf[i+1]=='a'&&buf[i+2]=='m'&&buf[i+3]=='e') {
                    i+=4; while(buf[i]==' ') i++;
                    int j=0; while(buf[i]&&buf[i]!=' '&&j<63) namepat[j++]=buf[i++]; namepat[j]='\0';
                } else if (buf[i]=='t'&&buf[i+1]=='y'&&buf[i+2]=='p'&&buf[i+3]=='e') {
                    i+=4; while(buf[i]==' ') i++;
                    if(buf[i]=='f') type_filter=1;
                    else if(buf[i]=='d') type_filter=2;
                    while(buf[i]&&buf[i]!=' ') i++;
                } else { while(buf[i]&&buf[i]!=' ') i++; }
            }
            while(buf[i]==' ') i++;
            if (buf[i]&&buf[i]!='-') { while(buf[i]&&buf[i]!=' ') i++; while(buf[i]==' ') i++; }
        }
        int found=0;
        for (int fi=0;fi<MAX_FILES;fi++) {
            if (!file_system[fi].exists) continue;
            if (type_filter==1 && file_system[fi].type!=TYPE_FILE) continue;
            if (type_filter==2 && file_system[fi].type!=TYPE_DIR) continue;
            if (namepat[0] && !strcontains(file_system[fi].name, namepat)) continue;


            if (file_system[fi].parent_id==0) {
                term_putc('/'); term_print(file_system[fi].name);
            } else {
                for (int j=0;j<MAX_FILES;j++)
                    if (file_system[j].exists&&file_system[j].type==TYPE_DIR
                        &&file_system[j].id==file_system[fi].parent_id) {
                        term_putc('/'); term_print(file_system[j].name);
                        term_putc('/'); term_print(file_system[fi].name);
                        break;
                    }
            }
            if (file_system[fi].type==TYPE_DIR) term_putc('/');
            term_putc('\n'); found=1;
        }
        if (!found) term_print("(not found)\n");
        return;
    }



    if (startswith(buf, "hexdump ")) {
        int idx = find_file(buf+8);
        if (idx==-1) { term_print("hexdump: file not found.\n"); return; }
        const unsigned char* data = (const unsigned char*)file_system[idx].content;
        int sz = file_system[idx].size; if (sz>128) sz=128;
        char hex[3]; hex[2]='\0';
        for (int i=0;i<sz;i+=16) {


            term_print_int(i); term_putc(':'); term_putc(' ');
            for (int j=0;j<16;j++) {
                if (i+j<sz) {
                    unsigned char b = data[i+j];
                    hex[0]="0123456789ABCDEF"[b>>4];
                    hex[1]="0123456789ABCDEF"[b&0xF];
                    term_print(hex); term_putc(' ');
                } else term_print("   ");
            }
            term_print(" |");
            for (int j=0;j<16&&i+j<sz;j++) {
                unsigned char b=data[i+j];
                term_putc(b>=32&&b<127 ? b : '.');
            }
            term_print("|\n");
        }
        return;
    }



    if (strcmp(buf, "nano") == 0) { term_print("Usage: nano <filename>\n"); return; }
    if (startswith(buf, "nano ")) {
        int idx = find_file(buf+5);
        if (idx == -1) {


            int sl = next_free_slot();
            if (sl == -1) { term_print("Disk full.\n"); return; }
            strncpy(file_system[sl].name, buf+5, FILENAME_SIZE);
            file_system[sl].type=TYPE_FILE; file_system[sl].exists=1;
            file_system[sl].parent_id=current_dir_id; file_system[sl].id=sl+1;
            file_system[sl].content[0]='\0'; file_system[sl].size=0;
            idx = sl;
        }
        if (file_system[idx].type == TYPE_DIR) { term_print("nano: is a directory.\n"); return; }
        open_editor(idx);
        return;
    }



    if (startswith(buf, "sh ")) {
        int idx = find_file(buf+3);
        if (idx==-1) { term_print("sh: script not found.\n"); return; }
        run_script(file_system[idx].content);
        return;
    }



    if (startswith(buf, "cp ")) {
        char src[FILENAME_SIZE], dest[FILENAME_SIZE];
        int i=3, j=0;
        while (buf[i]&&buf[i]!=' '&&j<FILENAME_SIZE-1) src[j++]=buf[i++];
        src[j]='\0';
        while (buf[i]==' ') i++;
        j=0; while (buf[i]&&j<FILENAME_SIZE-1) dest[j++]=buf[i++]; dest[j]='\0';
        int si=find_file(src), di=next_free_slot();
        if (si==-1){term_print("cp: source not found.\n");return;}
        if (di==-1){term_print("Disk full.\n");return;}
        file_system[di]=file_system[si];
        strncpy(file_system[di].name, dest, FILENAME_SIZE);
        file_system[di].id=di+1;
        term_print("Copied.\n");
        return;
    }



    if (startswith(buf, "mv ")) {
        char src[FILENAME_SIZE], dest[FILENAME_SIZE];
        int i=3, j=0;
        while (buf[i]&&buf[i]!=' '&&j<FILENAME_SIZE-1) src[j++]=buf[i++];
        src[j]='\0';
        while (buf[i]==' ') i++;
        j=0; while (buf[i]&&j<FILENAME_SIZE-1) dest[j++]=buf[i++]; dest[j]='\0';
        int si=find_file(src);
        if (si==-1){term_print("mv: not found.\n");return;}
        strncpy(file_system[si].name, dest, FILENAME_SIZE);
        term_print("Renamed.\n");
        return;
    }



    if (strcmp(buf, "pwd") == 0) {
        char cwd[32]; get_cwd_string(cwd); term_print(cwd); term_putc('\n'); return;
    }



    if (strcmp(buf, "whoami") == 0) { term_print("root\n"); return; }



    if (strcmp(buf, "uname") == 0) {
        term_print("GregOS 2.0 x86 i386 FreestOS\n"); return;
    }



    if (strcmp(buf, "date") == 0) {
        char ts[12], ds[12];
        get_date_string(ds); get_time_string(ts);
        term_print(ds); term_putc(' '); term_print(ts); term_putc('\n');
        return;
    }



    if (strcmp(buf, "history") == 0) {
        int start = history_count > HISTORY_SIZE ? history_count - HISTORY_SIZE : 0;
        for (int i = start; i < history_count; i++) {
            int idx = i % HISTORY_SIZE;
            term_print_int(i+1); term_print("  "); term_print(history[idx]); term_putc('\n');
        }
        return;
    }



    if (strcmp(buf, "tree") == 0) {
        term_print("\x1b[34m/\x1b[0m\n");
        for (int i = 0; i < MAX_FILES; i++) {
            if (!file_system[i].exists || file_system[i].parent_id != 0) continue;
            if (file_system[i].type == TYPE_DIR) {
                term_print("\x1b[34m+-- "); term_print(file_system[i].name); term_print("/\x1b[0m\n");
                int did = file_system[i].id;
                for (int j = 0; j < MAX_FILES; j++) {
                    if (!file_system[j].exists || file_system[j].parent_id != did) continue;
                    term_print("|   +-- "); term_print(file_system[j].name);
                    if (file_system[j].type == TYPE_DIR) term_putc('/');
                    term_putc('\n');
                }
            } else {
                term_print("+-- "); term_print(file_system[i].name); term_putc('\n');
            }
        }
        return;
    }



    if (strcmp(buf, "env") == 0) {
        for (int i = 0; i < env_count; i++) {
            term_print(env_keys[i]); term_putc('='); term_print(env_vals[i]); term_putc('\n');
        }
        if (!env_count) term_print("(no variables set)\n");
        return;
    }



    if (startswith(buf, "setenv ")) {
        char key[ENV_VAR_LEN], val[ENV_VAR_LEN];
        int i=7, j=0;
        while (buf[i]&&buf[i]!=' '&&j<ENV_VAR_LEN-1) key[j++]=buf[i++];
        key[j]='\0';
        while (buf[i]==' ') i++;
        j=0; while (buf[i]&&j<ENV_VAR_LEN-1) val[j++]=buf[i++]; val[j]='\0';
        int found=0;
        for (int k=0;k<env_count;k++) {
            if (strcmp(env_keys[k], key)==0) { strcpy(env_vals[k], val); found=1; break; }
        }
        if (!found && env_count < MAX_ENV_VARS) {
            strcpy(env_keys[env_count], key); strcpy(env_vals[env_count], val); env_count++;
        }
        return;
    }

    if (startswith(buf, "export ")) {
        const char* arg = buf + 7;
        char key[ENV_VAR_LEN], val[ENV_VAR_LEN];
        int i=0, j=0;
        while (arg[i] && arg[i]!='=' && j<ENV_VAR_LEN-1) key[j++]=arg[i++];
        key[j]='\0';
        if (arg[i]=='=') i++;
        j=0; while (arg[i]&&j<ENV_VAR_LEN-1) val[j++]=arg[i++]; val[j]='\0';
        int found=0;
        for (int k=0;k<env_count;k++) {
            if (strcmp(env_keys[k], key)==0) { strcpy(env_vals[k], val); found=1; break; }
        }
        if (!found && env_count < MAX_ENV_VARS) {
            strcpy(env_keys[env_count], key); strcpy(env_vals[env_count], val); env_count++;
        }
        return;
    }

    if (startswith(buf, "unset ")) {
        const char* key = buf + 6;
        for (int k=0; k<env_count; k++) {
            if (strcmp(env_keys[k], key)==0) {
                for (int m=k; m<env_count-1; m++) {
                    strcpy(env_keys[m], env_keys[m+1]);
                    strcpy(env_vals[m], env_vals[m+1]);
                }
                env_count--;
                term_print("Unset.\n");
                return;
            }
        }
        term_print("unset: variable non trouvee.\n");
        return;
    }



    if (strcmp(buf, "calc") == 0 || startswith(buf, "calc ")) {
        if (gui_mode) { open_calc_window(); return; }
        if (strcmp(buf, "calc") == 0) { term_print("Usage: calc 5 + 3\n"); return; }
        char* expr = buf+5;
        char op=0; int opi=0;
        for (int i=0; expr[i]; i++) {
            if (expr[i]=='+'||expr[i]=='-'||expr[i]=='*'||expr[i]=='/') {
                op=expr[i]; opi=i; break;
            }
        }
        if (!op) { term_print("Usage: calc 5 + 3\n"); return; }
        char n1[12], n2[12]; int j=0;
        for (int i=0;i<opi;i++) if(expr[i]!=' ') n1[j++]=expr[i];
        n1[j]='\0';
        j=0; for (int i=opi+1;expr[i];i++) if(expr[i]!=' ') n2[j++]=expr[i]; n2[j]='\0';
        int a=atoi(n1), b=atoi(n2), res=0;
        if (op=='+') res=a+b;
        if (op=='-') res=a-b;
        if (op=='*') res=a*b;
        if (op=='/') { if(b==0){term_print("Division by zero.\n");return;} res=a/b; }
        term_print("\x1b[33mResult: \x1b[0m"); term_print_int(res); term_putc('\n');
        return;
    }



    if (strcmp(buf,"bc")==0 || startswith(buf,"bc ")) {
        const char* expr = pipe_stdin ? pipe_stdin : (buf[2]==' '?buf+3:"");
        if (!expr||!expr[0]) { term_print("bc: provide expression or pipe input\n"); return; }


        while (*expr==' '||*expr=='\n') expr++;




        int vals[32]; char ops[32]; int vt=0,ot=0;
        int i=0;
        while(expr[i]) {
            while(expr[i]==' ') i++;
            if(!expr[i]||expr[i]=='\n') break;


            if (expr[i]=='-'&&(vt==0||(ot>0))) {

                i++;
                int v=0; while(expr[i]>='0'&&expr[i]<='9') v=v*10+(expr[i++]-'0');
                if(vt<32) vals[vt++]=-v;
            } else if (expr[i]>='0'&&expr[i]<='9') {
                int v=0; while(expr[i]>='0'&&expr[i]<='9') v=v*10+(expr[i++]-'0');
                if(vt<32) vals[vt++]=v;
            } else if (expr[i]=='+'||expr[i]=='-'||expr[i]=='*'||expr[i]=='/'||expr[i]=='%') {
                if(ot<32) ops[ot++]=expr[i++];
            } else i++;
        }


        for(int p=0;p<ot;p++) {
            if(ops[p]=='*'||ops[p]=='/'||ops[p]=='%') {
                if(p+1<vt) {
                    int a=vals[p],b=vals[p+1],r=0;
                    if(ops[p]=='*') r=a*b;
                    else if(b==0){term_print("bc: division by zero\n");return;}
                    else if(ops[p]=='/') r=a/b; else r=a%b;
                    vals[p]=r;
                    for(int k=p+1;k<vt-1;k++) vals[k]=vals[k+1];
                    vt--;
                    for(int k=p;k<ot-1;k++) ops[k]=ops[k+1];
                    ot--; p--;
                }
            }
        }
        for(int p=0;p<ot;p++) {
            if(p+1<vt) {
                int a=vals[p],b=vals[p+1],r=0;
                if(ops[p]=='+') r=a+b; else r=a-b;
                vals[p]=r;
                for(int k=p+1;k<vt-1;k++) vals[k]=vals[k+1];
                vt--;
                for(int k=p;k<ot-1;k++) ops[k]=ops[k+1];
                ot--; p--;
            }
        }
        if(vt>0) { term_print_int(vals[0]); term_putc('\n'); }
        return;
    }



    if (startswith(buf,"cut ")) {
        int i=4; char delim=':'; int field=1;
        while(buf[i]=='-') {
            i++;
            if(buf[i]=='d') { i++; delim=buf[i]; i++; }
            else if(buf[i]=='f') { i++; field=0; while(buf[i]>='0'&&buf[i]<='9') field=field*10+(buf[i++]-'0'); }
            else { while(buf[i]&&buf[i]!=' ') i++; }
            while(buf[i]==' ') i++;
        }
        char fname[FILENAME_SIZE]; int j=0;
        while(buf[i]&&j<FILENAME_SIZE-1) fname[j++]=buf[i++];
        fname[j]='\0';
        const char* src=0;
        if(fname[0]=='\0'&&pipe_stdin) src=pipe_stdin;
        else { int idx=find_file(fname); if(idx==-1){term_print("cut: file not found\n");return;} src=file_system[idx].content; }
        if(!src){term_print("cut: no input\n");return;}
        char line[BUFFER_SIZE]; int li=0;
        for(int k=0;;k++) {
            char ch=src[k];
            if(ch=='\n'||ch=='\0') {
                line[li]='\0';


                int fn=1; int s=0;
                for(int l=0;l<=li;l++) {
                    if(line[l]==delim||line[l]=='\0') {
                        if(fn==field) {
                            for(int m=s;m<l;m++) term_putc(line[m]);
                            term_putc('\n'); break;
                        }
                        fn++; s=l+1;
                    }
                }
                li=0; if(ch=='\0') break;
            } else if(li<BUFFER_SIZE-1) line[li++]=ch;
        }
        return;
    }



    if (startswith(buf,"tr ")) {
        const char* rest=buf+3;
        while(*rest==' ') rest++;
        char from[128]="",to[128]=""; int fi2=0,ti=0;


        for(int p=0;p<2;p++) {
            char* dst=(p==0?from:to); int* di=(p==0?&fi2:&ti);
            if(*rest=='\''||*rest=='"') rest++;
            while(*rest&&*rest!=' '&&*rest!='\''&&*rest!='"'&&*di<127) {
                if(rest[1]=='-'&&rest[2]&&rest[2]!='\''&&rest[2]!='"'&&rest[2]!=' ') {
                    for(char c=rest[0];c<=rest[2]&&*di<127;c++) dst[(*di)++]=c;
                    rest+=3;
                } else dst[(*di)++]=*rest++;
            }
            dst[*di]='\0';
            if(*rest=='\''||*rest=='"') rest++;
            while(*rest==' ') rest++;
        }
        const char* src=pipe_stdin?pipe_stdin:"";
        for(int k=0;src[k];k++) {
            char c=src[k]; int mapped=0;
            for(int m=0;m<fi2&&m<ti;m++) {
                if(c==from[m]){term_putc(to[m]);mapped=1;break;}
            }
            if(!mapped) term_putc(c);
        }
        return;
    }



    if (startswith(buf, "sed ")) {
        const char* p = buf + 4;
        while (*p == ' ') p++;


        if (*p != 's') { term_print("sed: only s/old/new/ supported\n"); return; }
        p++;
        char delim = *p++; if (!delim) { term_print("sed: bad expression\n"); return; }
        char old_s[64], new_s[64]; int oi=0, ni=0;
        while (*p && *p != delim && oi < 63) old_s[oi++] = *p++;
        old_s[oi] = '\0'; if (*p == delim) p++;
        while (*p && *p != delim && ni < 63) new_s[ni++] = *p++;
        new_s[ni] = '\0'; if (*p == delim) p++;
        int global = (*p == 'g'); if (global) p++;
        while (*p == ' ') p++;


        const char* src = 0;
        if (*p && !pipe_stdin) { int idx=find_file(p); if(idx==-1){term_print("sed: file not found\n");return;} src=file_system[idx].content; }
        else if (pipe_stdin) src = pipe_stdin;
        if (!src) { term_print("sed: no input\n"); return; }
        int oldlen = strlen(old_s);
        if (!oldlen) { term_print(src); return; }
        for (int k = 0; src[k]; ) {
            if (strncmp(src+k, old_s, oldlen) == 0) {
                term_print(new_s);
                k += oldlen;
                if (!global) { term_print(src+k); return; }
            } else {
                term_putc(src[k++]);
            }
        }
        return;
    }



    if (strcmp(buf, "fortune") == 0) {
        int n = 0; while (fortunes[n]) n++;
        term_print("\x1b[33m\"");
        term_print(fortunes[rand()%n]);
        term_print("\"\x1b[0m\n");
        return;
    }



    if (startswith(buf, "man ")) {
        const char* cmd = buf+4;
        for (int i=0; man_pages[i].name; i++) {
            if (strcmp(man_pages[i].name, cmd)==0) {
                term_print("\x1b[36m"); term_print(man_pages[i].desc); term_print("\x1b[0m\n");
                return;
            }
        }
        term_print("man: no entry for "); term_print(cmd); term_putc('\n');
        return;
    }



    if (strcmp(buf, "neofetch") == 0) {
        char ts[12], ds[12];
        get_time_string(ts); get_date_string(ds);
        int fc=0; for(int i=0;i<MAX_FILES;i++) if(file_system[i].exists) fc++;
        unsigned long sec = jiffies / 100;
        int uh=(int)(sec/3600), um2=(int)((sec%3600)/60), us2=(int)(sec%60);
        unsigned int heap_pct = (kmalloc_used() * 100) / HEAP_SIZE;

        static const char* const logo[] = {
            "\x1b[33m    ==(W{==========-      \x1b[0m",
            "\x1b[33m      ||  (.--.)          \x1b[0m",
            "\x1b[33m      | \\_,|**|,__       \x1b[0m",
            "\x1b[33m -==\\\\  `\\.' `--'  ),   \x1b[0m",
            "\x1b[33m (   | .  |~~~~|          \x1b[0m",
            "\x1b[33m                          \x1b[0m",
            "\x1b[33m  Greg 1er  +  Drakkar    \x1b[0m",
            "                          ",
            "                          ",
            "                          ",
            "                          ",
            "                          ",
            0
        };
        /* left-side: logo, right-side: info printed line by line */
        int li = 0;
        #define NF_ROW(lbl_col, lbl, val_col, val) do { \
            if (logo[li]) term_print(logo[li++]); else { term_print("                          "); li++; } \
            term_print("  "); \
            term_set_color(lbl_col, 0x00); term_print(lbl); \
            term_set_color(val_col, 0x00); term_print(val); term_putc('\n'); \
        } while(0)

        NF_ROW(0x0A, "root", 0x0F, "@gregos");
        NF_ROW(0x08, "------------------------", 0x08, "");
        NF_ROW(0x0B, "OS      ", 0x0F, "GregOS v2.0 (bare metal x86)");
        NF_ROW(0x0B, "Kernel  ", 0x0F, "i386  C + NASM, no libc");
        NF_ROW(0x0B, "Shell   ", 0x0F, "gregsh (110+ commandes)");
        NF_ROW(0x0B, "Date    ", 0x0E, ds);
        NF_ROW(0x0B, "Heure   ", 0x0E, ts);
        /* uptime row */
        if (logo[li]) term_print(logo[li++]); else { term_print("                          "); li++; }
        term_print("  "); term_set_color(0x0B,0x00); term_print("Uptime  ");
        term_set_color(0x0F,0x00);
        term_print_int(uh); term_putc('h'); term_putc(' ');
        term_print_int(um2); term_putc('m'); term_putc(' ');
        term_print_int(us2); term_putc('s'); term_putc('\n');
        /* heap row */
        if (logo[li]) term_print(logo[li++]); else { term_print("                          "); li++; }
        term_print("  "); term_set_color(0x0B,0x00); term_print("Memoire ");
        term_set_color(0x0A,0x00); term_print_int((int)heap_pct); term_print("% (");
        term_print_int((int)kmalloc_used()); term_putc('/');
        term_print_int(HEAP_SIZE); term_print(")\n");
        /* files */
        if (logo[li]) term_print(logo[li++]); else { term_print("                          "); li++; }
        term_print("  "); term_set_color(0x0B,0x00); term_print("Fichiers");
        term_set_color(0x0F,0x00); term_putc(' '); term_print_int(fc); term_putc('/'); term_print_int(MAX_FILES); term_putc('\n');
        /* casino */
        if (logo[li]) term_print(logo[li++]); else { term_print("                          "); li++; }
        term_print("  "); term_set_color(0x0B,0x00); term_print("Casino  ");
        term_set_color(0x0E,0x00); term_print_int(casino_balance); term_print(" GC");
        term_set_color(0x08,0x00); term_print("  (best: "); term_print_int(casino_best); term_print(")\n");
        /* theme */
        if (logo[li]) term_print(logo[li++]); else { term_print("                          "); li++; }
        term_print("  "); term_set_color(0x0B,0x00); term_print("Theme   ");
        term_set_color(0x0D,0x00); term_print(theme_name[current_theme]); term_putc('\n');
        #undef NF_ROW
        /* palette */
        term_print("                            ");
        static const unsigned char _nfpal[] = {0x01,0x02,0x03,0x04,0x05,0x06,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x00};
        for (int i = 0; _nfpal[i]; i++) {
            term_set_color(_nfpal[i], 0x00); term_putc('\xdb'); term_putc('\xdb');
        }
        term_set_color(0x0F,0x00); term_putc('\n');
        return;
    }



    if (strcmp(buf, "sysinfo") == 0) {
        int fc=0; for(int i=0;i<MAX_FILES;i++) if(file_system[i].exists) fc++;
        unsigned long sec = jiffies / 100;
        int uh=(int)(sec/3600), um2=(int)((sec%3600)/60);
        unsigned int heap_pct = (kmalloc_used() * 100) / HEAP_SIZE;
        char ts[12], ds[12]; get_time_string(ts); get_date_string(ds);

        term_set_color(0x0B,0x00);
        term_putc('\xda'); for(int _si=0;_si<46;_si++) term_putc('\xc4'); term_putc('\xbf'); term_putc('\n');
        term_putc('\xb3');
        term_set_color(0x0F,0x00); term_print("  \xdb\xdb  GregOS v2.0  \xdb\xdb          Informations Systeme ");
        term_set_color(0x0B,0x00); term_print("\xb3\n");
        term_putc('\xc3'); for(int _si=0;_si<46;_si++) term_putc('\xc4'); term_putc('\xb4'); term_putc('\n');

        #define SI_ROW(lc, label, vc, val) do { \
            term_set_color(0x0B,0x00); term_putc('\xb3'); \
            term_set_color(lc,0x00); term_print("  " label "  "); \
            term_set_color(vc,0x00); term_print(val); term_putc('\n'); \
        } while(0)

        SI_ROW(0x0E, "Arch    ", 0x0F, "x86 i386  Protected Mode  32-bit");
        SI_ROW(0x0E, "Boot    ", 0x0F, "GRUB2 Multiboot2  VBE 800x600x32");
        SI_ROW(0x0E, "Shell   ", 0x0F, "gregsh  (110+ commandes built-in)");
        SI_ROW(0x0E, "Theme   ", 0x0D, theme_name[current_theme]);
        term_set_color(0x0B,0x00); term_putc('\xb3');
        term_set_color(0x0E,0x00); term_print("  Date    ");
        term_set_color(0x0F,0x00); term_print(ds); term_putc(' '); term_print(ts); term_putc('\n');
        term_set_color(0x0B,0x00); term_putc('\xb3');
        term_set_color(0x0E,0x00); term_print("  Uptime  ");
        term_set_color(0x0A,0x00); term_print_int(uh); term_print("h "); term_print_int(um2); term_print("m\n");
        term_set_color(0x0B,0x00); term_putc('\xb3');
        term_set_color(0x0E,0x00); term_print("  Memoire ");
        term_set_color(heap_pct>80?0x0C:0x0A, 0x00);
        term_print_int((int)heap_pct); term_print("%  (");
        term_print_int((int)(kmalloc_used()/1024)); term_print("K / ");
        term_print_int(HEAP_SIZE/1024); term_print("K)\n");
        term_set_color(0x0B,0x00); term_putc('\xb3');
        term_set_color(0x0E,0x00); term_print("  Fichiers");
        term_set_color(0x0F,0x00); term_print("  ");
        term_print_int(fc); term_print(" / "); term_print_int(MAX_FILES); term_print(" utilises\n");
        term_set_color(0x0B,0x00); term_putc('\xb3');
        term_set_color(0x0E,0x00); term_print("  Casino  ");
        term_set_color(0x0E,0x00); term_print_int(casino_balance); term_print(" GC  (record: ");
        term_print_int(casino_best); term_print(" GC)\n");
        #undef SI_ROW

        term_set_color(0x0B,0x00);
        term_putc('\xc0'); for(int _si=0;_si<46;_si++) term_putc('\xc4'); term_putc('\xd9'); term_putc('\n');
        term_set_color(0x0F,0x00);
        return;
    }



    if (strcmp(buf, "casino")     == 0) {
        if (gui_mode) open_casino_window();
        else start_casino();
        return;
    }
    if (strcmp(buf, "poker")      == 0) {
        if (gui_mode) open_poker_window();
        else term_print("poker: GUI requis (demarrez le bureau)\n");
        return;
    }
    if (strcmp(buf, "atm")        == 0) { cmd_distributeur();  return; }
    if (strcmp(buf, "scores")     == 0) { cmd_scores();        return; }
    if (strcmp(buf, "passwd")     == 0) { cmd_passwd();        return; }
    if (strcmp(buf, "more") == 0) { cmd_more(""); return; }
    if (startswith(buf, "more "))  { cmd_more(buf + 5);        return; }
    if (strncmp(buf, "music", 5) == 0) {
        const char* arg = buf + 5;
        while (*arg == ' ') arg++;
        cmd_music(arg);
        return;
    }



    if (strcmp(buf, "sync") == 0) {
        if (!ata_present()) { term_print("sync: aucun disque IDE detecte.\n"); return; }
        if (fs_save()) {
            term_set_color(0x0A, 0x00);
            term_print("Systeme de fichiers ecrit sur le disque (persistant).\n");
            term_set_color(0x0F, 0x00);
        } else term_print("sync: echec de l'ecriture disque.\n");
        return;
    }

    if (strcmp(buf, "format") == 0) {
        if (!ata_present()) { term_print("format: aucun disque IDE detecte.\n"); return; }
        fs_format();   /* wipe the on-disk header → next boot seeds defaults */
        term_set_color(0x0E, 0x00);
        term_print("Disque formate. Redemarre pour reinitialiser le systeme de fichiers.\n");
        term_set_color(0x0F, 0x00);
        return;
    }

    if (strcmp(buf, "ring3") == 0) {
        /* Spawn a real Ring-3 (CPL=3) userland process that drives the kernel
           purely through INT 0x80 syscalls (write, mmap, get_pid/ticks, exit).
           The scheduler pre-empts it; its output appears asynchronously below.  */
        term_set_color(0x0B, 0x00);
        term_print("Lancement d'un processus Ring 3 (CPL=3) via syscalls INT 0x80...\n");
        term_set_color(0x0F, 0x00);
        scheduler_spawn_user(user_ring3_demo);
        return;
    }

    if (strcmp(buf, "ring3crash") == 0) {
        /* Spawn a deliberately buggy Ring-3 process (reads unmapped memory).
           The CPU-fault handler catches the page fault, kills only that
           process, and the shell keeps running — memory-protection demo.   */
        term_set_color(0x0C, 0x00);
        term_print("Lancement d'un processus Ring 3 FAUTIF (acces memoire illegal)...\n");
        term_print("Le noyau devrait le tuer proprement et survivre.\n");
        term_set_color(0x0F, 0x00);
        scheduler_spawn_user(user_ring3_crash);
        return;
    }

    if (strcmp(buf, "vmtest") == 0) {
        /* Two Ring-3 processes, each in its OWN page directory. Both write to
           the same virtual address 0xC0000000 but map different physical
           pages there, so each reads back only its own value — proof of
           per-process address-space isolation.                             */
        unsigned int a = vm_create_addrspace();
        unsigned int b = vm_create_addrspace();
        if (!a || !b) { term_print("vmtest: pool d'espaces d'adressage plein.\n"); return; }
        term_set_color(0x0B, 0x00);
        term_print("2 processus Ring 3, chacun dans son propre espace d'adressage.\n");
        term_print("Meme adresse virtuelle 0xC0000000, pages physiques distinctes :\n");
        term_set_color(0x0F, 0x00);
        scheduler_spawn_user_vm(user_vm_demo, a);
        scheduler_spawn_user_vm(user_vm_demo, b);
        return;
    }

    if (strcmp(buf, "exec") == 0) {
        /* Load the embedded flat binary (programs/userhello/user.asm) into a
           FULLY isolated address space (kernel mapped supervisor-only) and run
           it as a real Ring-3 process starting at VA 0x40000000. The program
           prints, then reads kernel memory (0x100000) — which now faults
           because the kernel is invisible to it. Proof of true isolation.   */
        unsigned int cr3 = vm_create_isolated(user_bin, user_bin_len);
        if (!cr3) { term_print("exec: impossible de creer l'espace isole.\n"); return; }
        term_set_color(0x0B, 0x00);
        term_print("exec: chargement d'un binaire de ");
        { char nb[12]; itoa((int)user_bin_len, nb); term_print(nb); }
        term_print(" octets @0x40000000 (noyau invisible au processus)...\n");
        term_set_color(0x0F, 0x00);
        scheduler_spawn_user_at(0x40000000u, 0x40002000u, cr3);
        return;
    }

    if (buf[0]=='e' && buf[1]=='l' && buf[2]=='f' && buf[3]=='r' && buf[4]=='u' &&
        buf[5]=='n' && (buf[6]=='\0' || buf[6]==' ')) {
        /* Load a real C program (compiled to a static ELF) into a fully
           isolated address space and run it Ring 3, passing any arguments
           after the command as argv. Proves the exec path handles a genuine
           multi-section ELF with command-line arguments.                     */
        static char argbuf[128];
        const char* rest = buf + 6;
        int n = 0; while (rest[n] && n < 127) { argbuf[n] = rest[n]; n++; }
        argbuf[n] = '\0';
        char* av[8]; int ac = 0;                 /* av[0] = "elfrun" for argv[0] */
        av[ac++] = "elfrun";
        char* p = argbuf;
        while (*p && ac < 8) {
            while (*p == ' ') p++;
            if (!*p) break;
            av[ac++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = '\0';
        }
        unsigned int entry = 0, esp = 0;
        unsigned int cr3 = vm_exec_elf_args(userapp_elf, userapp_elf_len,
                                            (const char* const*)av, ac, &entry, &esp);
        if (!cr3) { term_print("elfrun: chargement ELF echoue.\n"); return; }
        char hb[11]; hb[0] = '0'; hb[1] = 'x';
        for (int k = 0; k < 8; k++) {
            unsigned int n = (entry >> ((7 - k) * 4)) & 0xFu;
            hb[2 + k] = (char)(n < 10 ? '0' + n : 'A' + n - 10);
        }
        hb[10] = '\0';
        term_set_color(0x0B, 0x00);
        term_print("elfrun: programme C ELF charge (entry="); term_print(hb);
        term_print(") dans un espace isole. Lancement Ring 3...\n");
        term_set_color(0x0F, 0x00);
        scheduler_spawn_user_at(entry, esp, cr3);
        return;
    }

    if (buf[0]=='r' && buf[1]=='u' && buf[2]=='n' && (buf[3]=='\0' || buf[3]==' ')) {
        /* Load an ELF program from the VFS by name and run it in an isolated
           Ring-3 address space, passing the rest of the line as argv. This is a
           real filesystem exec: the program lives as a file (`ls` shows it).    */
        static char rbuf[128];
        const char* rest = buf + 3;
        int n = 0; while (rest[n] && n < 127) { rbuf[n] = rest[n]; n++; }
        rbuf[n] = '\0';
        char* av[8]; int ac = 0;
        char* p = rbuf;
        while (*p && ac < 8) {
            while (*p == ' ') p++;
            if (!*p) break;
            av[ac++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = '\0';
        }
        if (ac == 0) { term_print("Usage: run <fichier.elf> [args...]   (ex: run hello.elf a b)\n"); return; }
        int fid = vfs_find(av[0], 0);
        if (fid < 0) { term_print("run: fichier introuvable: "); term_print(av[0]); term_print("\n"); return; }
        static unsigned char elfbuf[FILE_CONTENT_SIZE];
        int flen = vfs_read_file(fid, (char*)elfbuf, sizeof(elfbuf));
        if (flen <= 0) { term_print("run: lecture du fichier echouee.\n"); return; }
        unsigned int entry = 0, esp = 0;
        unsigned int cr3 = vm_exec_elf_args(elfbuf, (unsigned int)flen,
                                            (const char* const*)av, ac, &entry, &esp);
        if (!cr3) { term_print("run: ELF invalide ou pool d'espaces plein.\n"); return; }
        term_set_color(0x0B, 0x00);
        term_print("run: "); term_print(av[0]);
        term_print(" charge depuis le VFS (");
        { char nb[12]; itoa(flen, nb); term_print(nb); }
        term_print(" octets), espace isole. Ring 3...\n");
        term_set_color(0x0F, 0x00);
        scheduler_spawn_user_at(entry, esp, cr3);
        return;
    }

    if (strcmp(buf, "shutdown") == 0) {
        term_print("Shutting down...\n");
        if (ata_present() && fs_is_dirty()) fs_save();   /* persist before power off */
        for (int i=0;i<2000000;i++) __asm__ volatile("nop");
        acpi_shutdown();                 /* real ACPI S5 — returns only on failure */
        port_word_out(0x604, 0x2000);    /* legacy QEMU/Bochs fallback             */
        port_word_out(0xB004, 0x2000);
        term_print("shutdown: echec ACPI et fallback — arret impossible.\n");
        __asm__("hlt");
        return;
    }



    if (strcmp(buf, "reboot") == 0) {
        acpi_reboot();                   /* ACPI reset register — returns on failure */
        reboot();                        /* PS/2 keyboard-controller pulse fallback  */
        return;
    }

    if (strcmp(buf, "acpi") == 0) {
        acpi_print_info();
        return;
    }



    if (strcmp(buf, "clear") == 0) {
        if (gui_mode) tty_clear();
        else draw_interface();
        return;
    }



    if (strcmp(buf, "help") == 0) {
        /* Top border */
        term_set_color(0x0B, 0x00);
        term_putc('\xda'); for(int _hi=1;_hi<VGA_WIDTH-1;_hi++) term_putc('\xc4'); term_putc('\xbf'); term_putc('\n');
        /* Title */
        term_putc('\xb3');
        term_set_color(0x0F,0x00); term_print(" \xdb\xdb GregOS v2.0");
        term_set_color(0x08,0x00); term_print(" \xb3 ");
        term_set_color(0x0B,0x00); term_print("gregsh");
        term_set_color(0x08,0x00); term_print("   TAB=complete  \x18\x19=historique  cmd1|cmd2=pipe  man <cmd>  ");
        term_set_color(0x0B,0x00); term_print("\xb3\n");
        /* Separator */
        term_putc('\xc3'); for(int _hi=1;_hi<VGA_WIDTH-1;_hi++) term_putc('\xc4'); term_putc('\xb4'); term_putc('\n');
        /* Category rows */
        static const char* hcol[] = {
            "\x0b","FICHIERS ", "ls  cd  mkdir  touch  rm  cat  cp  mv  pwd  tree  stat  chmod",
            "\x0e","EDITEUR  ", "nano  write  more  less              Ctrl+S=sauver  ESC=quitter",
            "\x0a","RECHERCHE", "grep  find  wc  head  tail  sort  tac  hexdump  rev",
            "\x0b","SYSTEME  ", "sysinfo  neofetch  uname  date  uptime  df  free  acpi",
            "\x0c","PROCESSUS", "ps  top  kill  killall  jobs  strace  tasks",
            "\x09","RESEAU   ", "ping  ifconfig  net  host  nslookup  wget  curl  browser",
            "\x0b","NOYAU    ", "ring3  ring3crash  vmtest  elfrun  run hello  sync  format",
            "\x0d","OUTILS   ", "calc  bc  base64  rot13  cut  tr  sed  awk  expr  nl  tee",
            "\x0e","SCRIPTS  ", "sh  source  &&  ||  cmd&  export  env  alias  history  exit",
            "\x0a","DIVERS   ", "fortune  cowsay  banner  lolcat  theme  music  paint  horloge",
            0, 0, 0
        };
        for (int _ci = 0; hcol[_ci]; _ci += 3) {
            term_set_color(0x08,0x00); term_putc('\xb3'); term_putc(' ');
            term_set_color(0x0E,0x00); term_putc('>'); term_putc(' ');
            term_set_color((unsigned char)hcol[_ci][0],0x00); term_print(hcol[_ci+1]);
            term_set_color(0x08,0x00); term_print("  ");
            term_set_color(0x0F,0x00); term_print(hcol[_ci+2]); term_putc('\n');
        }
        /* Apps separator */
        term_set_color(0x0B, 0x00);
        term_putc('\xc3'); for(int _hi=1;_hi<VGA_WIDTH-1;_hi++) term_putc('\xc4'); term_putc('\xb4'); term_putc('\n');
        /* Apps row */
        term_set_color(0x08,0x00); term_putc('\xb3');
        term_set_color(0x0A,0x00); term_print("  > JEUX ");
        term_set_color(0x08,0x00); term_print("\x1a icone Jeux sur le bureau   ");
        term_set_color(0x0D,0x00); term_print("> CASINO ");
        term_set_color(0x08,0x00); term_print("\x1a icone Casino sur le bureau\n");
        /* Bottom border */
        term_set_color(0x0B, 0x00);
        term_putc('\xc0'); for(int _hi=1;_hi<VGA_WIDTH-1;_hi++) term_putc('\xc4'); term_putc('\xd9');
        term_set_color(0x0F,0x00); term_putc('\n');
        return;
    }






    if (strcmp(buf, "theme") == 0) {
        current_theme = (current_theme + 1) % NUM_THEMES;
        beep_theme();
        term_print("Theme: \x1b[33m");
        term_print(theme_name[current_theme]);
        term_print("\x1b[0m\n");
        draw_interface();
        return;
    }
    if (startswith(buf, "theme ")) {
        const char* name = buf + 6;
        int found = 0;
        for (int i = 0; i < NUM_THEMES; i++) {
            if (strcmp(name, theme_name[i]) == 0) {
                current_theme = i; found = 1; break;
            }
        }
        if (!found) {
            term_print("Themes: ");
            for (int i=0;i<NUM_THEMES;i++) { term_print(theme_name[i]); term_putc(' '); }
            term_putc('\n');
        } else {
            beep_theme();
            term_print("Theme: \x1b[33m");
            term_print(theme_name[current_theme]);
            term_print("\x1b[0m\n");
            draw_interface();
        }
        return;
    }






    if (startswith(buf, "cowsay ")) { cmd_cowsay(buf+7); return; }
    if (strcmp(buf, "cowsay") == 0)  { cmd_cowsay("Moo!"); return; }






    if (strcmp(buf, "banner") == 0) { term_print("Usage: banner <text>\n"); return; }
    if (startswith(buf, "banner ")) { cmd_banner(buf+7); return; }



    if (strcmp(buf, "ps") == 0) { cmd_ps(); return; }



    if (strcmp(buf, "top") == 0) { cmd_top(); return; }



    if (strcmp(buf, "df") == 0) { cmd_df(); return; }



    if (strcmp(buf, "free") == 0) { cmd_free(); return; }



    if (strcmp(buf, "head") == 0) { cmd_head("", 10); return; }
    if (startswith(buf, "head ")) {
        int n = 10; const char* fn = buf+5;
        if (buf[5] == '-') { n = atoi(buf+6); while (*fn && *fn != ' ') fn++; while (*fn==' ') fn++; }
        cmd_head(fn, n); return;
    }



    if (strcmp(buf, "tail") == 0) { cmd_tail("", 10); return; }
    if (startswith(buf, "tail ")) {
        int n = 10; const char* fn = buf+5;
        if (buf[5] == '-') { n = atoi(buf+6); while (*fn && *fn != ' ') fn++; while (*fn==' ') fn++; }
        cmd_tail(fn, n); return;
    }



    if (strcmp(buf, "sort") == 0) { cmd_sort_flags("", 0, 0, 0); return; }
    if (startswith(buf, "sort ")) {
        const char* sarg = buf+5;
        int rev=0, num=0, uniq2=0;
        while (startswith(sarg, "-")) {
            sarg++;
            while (*sarg && *sarg != ' ') {
                if (*sarg=='r') rev=1;
                if (*sarg=='n') num=1;
                if (*sarg=='u') uniq2=1;
                sarg++;
            }
            while (*sarg==' ') sarg++;
        }
        cmd_sort_flags(sarg, rev, num, uniq2);
        return;
    }



    if (strcmp(buf, "tac") == 0) { cmd_tac(""); return; }
    if (startswith(buf, "tac ")) { cmd_tac(buf+4); return; }



    if (startswith(buf, "sleep ")) { cmd_sleep(atoi(buf+6)); return; }



    if (startswith(buf, "stat ")) { cmd_stat(buf+5); return; }



    if (strcmp(buf, "rev") == 0 && pipe_stdin) {


        const char* p = pipe_stdin;
        while (*p) {
            const char* nl = p;
            while (*nl && *nl != '\n') nl++;
            for (const char* q = nl-1; q >= p; q--) term_putc(*q);
            term_putc('\n');
            p = (*nl == '\n') ? nl + 1 : nl;
        }
        return;
    }
    if (strcmp(buf, "rev") == 0) { term_print("Usage: rev <text>\n"); return; }
    if (startswith(buf, "rev ")) { cmd_rev(buf+4); return; }



    if (strcmp(buf, "uptime") == 0) { cmd_uptime(); return; }



    if (startswith(buf, "yes") ) {
        const char* txt = (buf[3]==' ') ? buf+4 : "y";
        for (int i=0;i<20;i++) { term_print(txt); term_putc('\n'); }
        return;
    }



    if (strcmp(buf, "seq") == 0) { term_print("Usage: seq <N>  or  seq <M> <N>\n"); return; }
    if (startswith(buf, "seq ")) {
        char* p = buf + 4;
        int a = atoi(p);


        while (*p && *p != ' ') p++;
        if (*p == ' ') {


            int b = atoi(p+1);
            for (int i = a; i <= b; i++) { term_print_int(i); term_putc('\n'); }
        } else {


            for (int i = 1; i <= a; i++) { term_print_int(i); term_putc('\n'); }
        }
        return;
    }



    {
        const char* lc_txt = 0;
        if (strcmp(buf, "lolcat") == 0 && pipe_stdin) lc_txt = pipe_stdin;
        else if (startswith(buf, "lolcat ")) lc_txt = buf + 7;
        if (!lc_txt && strcmp(buf, "lolcat") == 0) { term_print("Usage: lolcat <text>\n"); return; }
        if (lc_txt) {
            static const unsigned char lc_colors[] = {0x0C,0x0E,0x0A,0x0B,0x09,0x0D,0x0C,0x0E};
            int ci = 0;
            for (int i = 0; lc_txt[i]; i++) {
                if (lc_txt[i] == '\n') { term_set_color(0x0F,0x00); term_putc('\n'); continue; }
                if (lc_txt[i] == ' ')  { term_set_color(0x0F,0x00); term_putc(' ');  continue; }
                term_set_color(lc_colors[ci % 8], 0x00);
                term_putc(lc_txt[i]);
                ci++;
            }
            term_set_color(0x0F, 0x00);
            if (lc_txt[0] && lc_txt[strlen(lc_txt)-1] != '\n') term_putc('\n');
            return;
        }
    }



    if (startswith(buf, "write ")) {


        char fn[FILENAME_SIZE]; int i=6, j=0;
        while (buf[i]&&buf[i]!=' '&&j<FILENAME_SIZE-1) fn[j++]=buf[i++];
        fn[j]='\0';
        while (buf[i]==' ') i++;
        int idx = find_file(fn);
        if (idx==-1) {
            int sl=next_free_slot(); if(sl==-1){term_print("Disk full.\n");return;}
            strncpy(file_system[sl].name, fn, FILENAME_SIZE);
            file_system[sl].type=TYPE_FILE; file_system[sl].exists=1;
            file_system[sl].parent_id=current_dir_id; file_system[sl].id=sl+1;
            file_system[sl].content[0]='\0'; file_system[sl].size=0;
            idx=sl;
        }
        strcpy(file_system[idx].content, buf+i);
        strcat(file_system[idx].content, "\n");
        file_system[idx].size = strlen(file_system[idx].content);
        term_print("Written.\n"); return;
    }



    if (startswith(buf, "diff ")) {
        char df1[FILENAME_SIZE], df2[FILENAME_SIZE]; int i=5, j=0;
        while (buf[i]&&buf[i]!=' '&&j<FILENAME_SIZE-1) df1[j++]=buf[i++];
        df1[j]='\0';
        while (buf[i]==' ') i++;
        j=0; while (buf[i]&&j<FILENAME_SIZE-1) df2[j++]=buf[i++]; df2[j]='\0';
        cmd_diff(df1, df2);
        return;
    }
    if (strcmp(buf, "diff") == 0) { term_print("Usage: diff <file1> <file2>\n"); return; }



    if (strcmp(buf, "rot13") == 0) { term_print("Usage: rot13 <text>\n"); return; }
    if (startswith(buf, "rot13 ")) {
        const char* t = buf+6;
        for (int i=0; t[i]; i++) {
            char ch=t[i];
            if (ch>='a'&&ch<='z') ch='a'+(ch-'a'+13)%26;
            else if (ch>='A'&&ch<='Z') ch='A'+(ch-'A'+13)%26;
            term_putc(ch);
        }
        term_putc('\n');
        return;
    }



    if (startswith(buf, "chmod ")) {
        const char* p = buf+6;
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        int idx = find_file(p);
        if (idx==-1) { term_print("chmod: file not found.\n"); return; }
        term_set_color(0x0A,0x00);
        term_print("chmod: permissions updated\n");
        term_set_color(0x0F,0x00);
        return;
    }



    if (strcmp(buf, "alias") == 0) {
        if (alias_count == 0) { term_print("(no aliases)\n"); return; }
        for (int i = 0; i < alias_count; i++) {
            term_print("alias "); term_print(alias_names[i]);
            term_print("='"); term_print(alias_cmds[i]); term_print("'\n");
        }
        return;
    }
    if (startswith(buf, "alias ")) {
        const char* p = buf + 6;
        char aname[32]; int j = 0;
        while (*p && *p != '=' && j < 31) aname[j++] = *p++;
        aname[j] = '\0';
        if (*p != '=') { term_print("Usage: alias name=command\n"); return; }
        p++;


        if (*p == '\'') p++;
        char acmd[BUFFER_SIZE]; j = 0;
        while (*p && *p != '\'' && j < BUFFER_SIZE-1) acmd[j++] = *p++;
        acmd[j] = '\0';


        for (int i = 0; i < alias_count; i++) {
            if (strcmp(alias_names[i], aname) == 0) {
                strcpy(alias_cmds[i], acmd);
                term_print("Alias updated.\n"); return;
            }
        }
        if (alias_count < MAX_ALIASES) {
            strcpy(alias_names[alias_count], aname);
            strcpy(alias_cmds[alias_count], acmd);
            alias_count++;
            term_print("Alias set.\n");
        } else { term_print("Too many aliases.\n"); }
        return;
    }



    if (startswith(buf, "unalias ")) {
        const char* aname = buf + 8;
        for (int i = 0; i < alias_count; i++) {
            if (strcmp(alias_names[i], aname) == 0) {
                for (int j = i; j < alias_count-1; j++) {
                    strcpy(alias_names[j], alias_names[j+1]);
                    strcpy(alias_cmds[j],  alias_cmds[j+1]);
                }
                alias_count--;
                term_print("Alias removed.\n"); return;
            }
        }
        term_print("unalias: not found.\n"); return;
    }



    if (startswith(buf, "which ")) {
        const char* cmd = buf + 6;
        for (int i = 0; cmd_list[i]; i++) {
            if (strcmp(cmd_list[i], cmd) == 0) {
                term_print("/bin/"); term_print(cmd); term_putc('\n'); return;
            }
        }
        term_print(cmd); term_print(": not found\n");
        return;
    }



    if (startswith(buf, "base64 ")) {
        static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const char* t = buf + 7;
        int decode = 0;
        if (startswith(t, "-d ")) { decode = 1; t += 3; }
        if (!decode) {


            int n = strlen(t), i = 0;
            while (i < n) {
                unsigned char b[3] = {0, 0, 0}; int bytes = 0;
                if (i < n) { b[0] = (unsigned char)t[i++]; bytes++; }
                if (i < n) { b[1] = (unsigned char)t[i++]; bytes++; }
                if (i < n) { b[2] = (unsigned char)t[i++]; bytes++; }
                int v = ((int)b[0] << 16) | ((int)b[1] << 8) | b[2];
                term_putc(b64[(v >> 18) & 0x3F]);
                term_putc(b64[(v >> 12) & 0x3F]);
                term_putc(bytes >= 2 ? b64[(v >> 6) & 0x3F] : '=');
                term_putc(bytes >= 3 ? b64[v & 0x3F] : '=');
            }
            term_putc('\n');
        } else {


            static const int b64inv[128] = {
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
                52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
                -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
                15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
                -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
                41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
            };
            int n = strlen(t), i = 0;
            while (i + 3 < n) {
                unsigned char c0 = (unsigned char)t[i];
                unsigned char c1 = (unsigned char)t[i+1];
                unsigned char c2 = (unsigned char)t[i+2];
                unsigned char c3 = (unsigned char)t[i+3];
                if (c0 >= 128 || c1 >= 128) break;
                int v0=b64inv[c0], v1=b64inv[c1];
                int v2=(c2=='=')?0:((c2<128)?b64inv[c2]:0);
                int v3=(c3=='=')?0:((c3<128)?b64inv[c3]:0);
                if (v0<0||v1<0) break;
                term_putc((char)((v0<<2)|(v1>>4)));
                if (c2 != '=') term_putc((char)(((v1&0xF)<<4)|(v2>>2)));
                if (c3 != '=') term_putc((char)(((v2&3)<<6)|v3));
                i += 4;
            }
            term_putc('\n');
        }
        return;
    }
    if (strcmp(buf, "base64") == 0) { term_print("Usage: base64 <text>  |  base64 -d <b64text>\n"); return; }



    if (strcmp(buf, "cal")      == 0) { cmd_cal();      return; }
    if (strcmp(buf, "ifconfig") == 0) { cmd_ifconfig(); return; }

    if (startswith(buf, "ping ")) { cmd_ping(buf + 5); return; }
    if (strcmp(buf, "ping")     == 0) { cmd_ping(""); return; }

    if (startswith(buf, "sudo ")) { cmd_sudo(buf + 5); return; }

    if (startswith(buf, "tee "))  { cmd_tee(buf + 4);  return; }

    if (startswith(buf, "ln ")) {
        char src[64], dst[64];
        const char* p = buf + 3;
        int si = 0;
        while (*p && *p != ' ' && si < 63) src[si++] = *p++;
        src[si] = '\0';
        while (*p == ' ') p++;
        int di = 0;
        while (*p && di < 63) dst[di++] = *p++;
        dst[di] = '\0';
        cmd_ln(src, dst);
        return;
    }

    if (startswith(buf, "watch ")) {
        char* p = buf + 6;
        int secs = 0;
        while (*p >= '0' && *p <= '9') secs = secs * 10 + (*p++ - '0');
        while (*p == ' ') p++;
        cmd_watch(secs, p);
        return;
    }

    if (startswith(buf, "file "))     { cmd_file(buf + 5);          return; }
    if (strcmp(buf, "lspci")   == 0)  { cmd_lspci();                return; }
    if (strcmp(buf, "dmesg")   == 0)  { cmd_dmesg();                return; }
    if (strcmp(buf, "id")      == 0)  { cmd_id();                   return; }
    if (strcmp(buf, "groups")  == 0)  { term_print(current_user); term_print(" sudo adm daemon\n"); return; }
    if (strcmp(buf, "hostname") == 0) { cmd_hostname("");            return; }
    if (startswith(buf, "hostname ")) { cmd_hostname(buf + 9);       return; }
    if (strcmp(buf, "mount")   == 0)  { cmd_mount();                return; }
    if (strcmp(buf, "umount")  == 0)  { term_print("umount: rien a demonter\n"); return; }
    if (strcmp(buf, "crontab") == 0 || strcmp(buf, "crontab -l") == 0) { cmd_crontab("-l"); return; }
    if (startswith(buf, "type "))     { cmd_type(buf + 5);           return; }
    if (startswith(buf, "wget "))     { cmd_wget(buf + 5);           return; }
    if (strcmp(buf, "wget")    == 0)  { cmd_wget("");                return; }
    if (startswith(buf, "curl "))     { cmd_curl(buf + 5);           return; }
    if (strcmp(buf, "curl")    == 0)  { cmd_curl("");                return; }
    if (startswith(buf, "traceroute ")) { cmd_traceroute(buf + 11);  return; }
    if (startswith(buf, "nslookup ")) { cmd_nslookup(buf + 9);       return; }
    if (startswith(buf, "host "))     { cmd_nslookup(buf + 5);       return; }
    if (strcmp(buf, "net") == 0 || strcmp(buf, "net status") == 0) { cmd_ifconfig(); return; }
    if (strcmp(buf, "browser") == 0 || strcmp(buf, "gregnet") == 0 || strcmp(buf, "web") == 0) {
        if (gui_mode) { extern void open_browser_window(void); open_browser_window(); }
        else term_print("Le navigateur GregNet necessite le mode graphique (bureau).\n");
        return;
    }
    if (startswith(buf, "browser ") || startswith(buf, "web ")) {
        const char* u = startswith(buf, "browser ") ? buf + 8 : buf + 4;
        if (gui_mode) { extern void open_browser_window_url(const char*); open_browser_window_url(u); }
        else term_print("Le navigateur GregNet necessite le mode graphique (bureau).\n");
        return;
    }
    if (strcmp(buf, "paint") == 0 || strcmp(buf, "gregpaint") == 0) {
        if (gui_mode) { extern void open_paint_window(void); open_paint_window(); }
        else term_print("GregPaint necessite le mode graphique (bureau).\n");
        return;
    }
    if (strcmp(buf, "horloge") == 0 || strcmp(buf, "clockgui") == 0) {
        if (gui_mode) { extern void open_clock_window(void); open_clock_window(); }
        else term_print("L'horloge graphique necessite le mode graphique (bureau).\n");
        return;
    }
    if (startswith(buf, "nmap "))     { cmd_nmap(buf + 5);           return; }
    if (strcmp(buf, "nmap")    == 0)  { cmd_nmap("");                return; }
    if (startswith(buf, "ssh "))      { cmd_ssh(buf + 4);            return; }
    if (strcmp(buf, "ssh")     == 0)  { cmd_ssh("");                 return; }
    if (strcmp(buf, "pkg")     == 0)  { cmd_pkg("list");             return; }
    if (startswith(buf, "pkg "))      { cmd_pkg(buf + 4);            return; }
    if (strcmp(buf, "fdisk")   == 0 || strcmp(buf, "fdisk -l") == 0) { cmd_fdisk(""); return; }
    if (startswith(buf, "expr "))     { cmd_expr(buf + 5);           return; }
    if (startswith(buf, "useradd "))  { cmd_useradd(buf + 8);        return; }
    if (strcmp(buf, "su")      == 0)  { cmd_su("root");              return; }
    if (startswith(buf, "su "))       { cmd_su(buf + 3);             return; }
    if (startswith(buf, "strace "))   { cmd_strace(buf + 7);         return; }
    if (startswith(buf, "du"))        {
        const char* a = buf + 2; while (*a == ' ') a++;
        cmd_du(a); return;
    }
    if (strcmp(buf, "jobs")    == 0)  { cmd_jobs();                  return; }
    if (strcmp(buf, "bg")      == 0 || strcmp(buf, "fg") == 0) { cmd_jobs(); return; }
    if (strcmp(buf, "printenv") == 0) { cmd_printenv("");             return; }
    if (startswith(buf, "printenv ")) { cmd_printenv(buf + 9);       return; }
    if (startswith(buf, "source "))   { cmd_source(buf + 7);         return; }
    if (strcmp(buf, "true")    == 0)  { cmd_true_cmd();              return; }
    if (strcmp(buf, "false")   == 0)  { cmd_false_cmd();             return; }
    if (strcmp(buf, "mouse")   == 0)  { cmd_mouse();                 return; }
    if (strcmp(buf, "tasks")   == 0)  { cmd_tasks();                 return; }
    if (strcmp(buf, "ata")     == 0)  { cmd_ata("");                 return; }
    if (startswith(buf, "ata "))      { cmd_ata(buf + 4);            return; }
    if (startswith(buf, "kill "))     { cmd_kill(buf + 5);           return; }
    if (startswith(buf, "killall "))  { cmd_kill(buf + 8);           return; }
    if (startswith(buf, "whoami"))    { term_print(current_user); term_putc('\n'); return; }
    if (strcmp(buf, "logout")  == 0 || strcmp(buf, "exit") == 0) {
        beep_ok(); term_print("Au revoir!\n"); timer_delay_ms(500);
        login_screen(); draw_interface(); return;
    }

    if (strcmp(buf, "test") == 0) { last_exit_code = 1; return; }
    if (startswith(buf, "test ")) { last_exit_code = cmd_test_eval(buf + 5); return; }
    if (buf[0] == '[') {
        int bl = strlen(buf);
        if (bl >= 2 && buf[bl-1] == ']') {
            char expr[BUFFER_SIZE]; strncpy(expr, buf + 2, bl - 3); expr[bl - 3] = '\0';
            last_exit_code = cmd_test_eval(expr); return;
        }
    }

    if (startswith(buf, "printf ")) { cmd_printf2(buf + 7); return; }
    if (strcmp(buf,  "printf") == 0) { term_print("Usage: printf <format>\n"); return; }

    if (strcmp(buf, "uniq")  == 0)  { cmd_uniq(""); return; }
    if (startswith(buf, "uniq "))   { cmd_uniq(buf + 5); return; }

    if (strcmp(buf, "xargs") == 0)  { cmd_xargs(""); return; }
    if (startswith(buf, "xargs "))  { cmd_xargs(buf + 6); return; }

    if (strcmp(buf, "awk")  == 0)   { term_print("Usage: awk [-F:] 'prog' [file]\n"); return; }
    if (startswith(buf, "awk "))    { cmd_awk(buf + 4); return; }

    if (startswith(buf, "time "))   { cmd_time2(buf + 5); return; }

    if (strcmp(buf, "less")  == 0)  { cmd_less(""); return; }
    if (startswith(buf, "less "))   { cmd_less(buf + 5); return; }

    if (strcmp(buf, "nl")    == 0)  { cmd_nl(""); return; }
    if (startswith(buf, "nl "))     { cmd_nl(buf + 3); return; }

    if (startswith(buf, "paste "))  { cmd_paste(buf + 6); return; }
    if (strcmp(buf, "paste") == 0)  { term_print("Usage: paste <file1> <file2>\n"); return; }

    /* Games: only accessible via graphical apps */
    if (strcmp(buf,"snake")==0 || strcmp(buf,"tetris")==0 || strcmp(buf,"pong")==0 ||
        strcmp(buf,"invaders")==0 || strcmp(buf,"breakout")==0 || strcmp(buf,"2048")==0 ||
        strcmp(buf,"minesweeper")==0 || strcmp(buf,"simon")==0 ||
        strcmp(buf,"matrix")==0 || strcmp(buf,"clock")==0) {
        term_set_color(0x0A,0x00); term_print(" > ");
        term_set_color(0x0F,0x00); term_print("Lance l'application ");
        term_set_color(0x0E,0x00); term_print("Jeux");
        term_set_color(0x08,0x00); term_print(" depuis l'icone sur le bureau\n");
        term_set_color(0x0F,0x00); return;
    }
    if (strcmp(buf,"blackjack")==0 || strcmp(buf,"roulette")==0 ||
        strcmp(buf,"slots")==0 || strcmp(buf,"plinko")==0) {
        term_set_color(0x0D,0x00); term_print(" > ");
        term_set_color(0x0F,0x00); term_print("Lance l'application ");
        term_set_color(0x0E,0x00); term_print("Casino");
        term_set_color(0x08,0x00); term_print(" depuis l'icone sur le bureau\n");
        term_set_color(0x0F,0x00); return;
    }

    last_exit_code = 1;
    beep_fail();
    term_print("\x1b[31mgregos: \x1b[0m");
    term_print(buf);
    term_print(": command not found\n");
}


int vfs_list_dir(int dir_id, VFSEntry* out, int max_count)
{
    int count = 0;
    for (int i = 0; i < MAX_FILES && count < max_count; i++) {
        if (!file_system[i].exists) continue;
        if (file_system[i].parent_id != dir_id) continue;
        out[count].type      = file_system[i].type;
        out[count].id        = file_system[i].id;
        out[count].parent_id = file_system[i].parent_id;
        out[count].size      = file_system[i].size;
        int j = 0;
        while (j < VFS_MAX_NAME - 1 && file_system[i].name[j]) {
            out[count].name[j] = file_system[i].name[j];
            j++;
        }
        out[count].name[j] = '\0';
        count++;
    }
    return count;
}

int vfs_read_file(int entry_id, char* buf, int buf_size) {
    int slot = entry_id - 1;
    if (slot < 0 || slot >= MAX_FILES) return -1;
    if (!file_system[slot].exists) return -1;
    if (file_system[slot].type != TYPE_FILE) return -1;
    int len = file_system[slot].size;
    if (len < 0) len = 0;
    if (len >= buf_size) len = buf_size - 1;
    for (int i = 0; i < len; i++) buf[i] = file_system[slot].content[i];
    buf[len] = '\0';
    return len;
}

/* Resolve a file/directory name to its VFS entry id (slot+1).
   parent_id = 0 searches the current working directory; otherwise the given
   parent. Returns the entry id (> 0) or -1 if not found.                   */
int vfs_find(const char* name, int parent_id) {
    if (!name || !name[0]) return -1;
    int parent = (parent_id == 0) ? current_dir_id : parent_id;
    for (int i = 0; i < MAX_FILES; i++) {
        if (file_system[i].exists
            && file_system[i].parent_id == parent
            && strcmp(file_system[i].name, name) == 0)
            return file_system[i].id;
    }
    return -1;
}

/* ── Disk persistence (primary IDE hard disk via ATA PIO) ────────────────
   The whole in-memory VFS is serialised to disk: a header at LBA 0 (magic +
   version + a few globals) and the file_system[] array from LBA 1 onward.
   GUI edits mark the FS dirty and the idle loop flushes; the `sync` command
   flushes on demand; reboot/shutdown flush first. At boot fs_load() restores
   a previously saved image, so files survive a reboot.                      */
#define FS_MAGIC    0x53465247u   /* 'GRFS' */
#define FS_VERSION  1u
#define FS_HDR_LBA  0u
#define FS_DATA_LBA 1u

static unsigned int  fs_disk_buf[128];   /* 512-byte, 4-aligned bounce buffer */
static int           fs_dirty = 0;

static void fs_disk_write(unsigned int lba, const unsigned char* data, unsigned int len) {
    unsigned char* sec = (unsigned char*)fs_disk_buf;
    unsigned int off = 0;
    while (off < len) {
        unsigned int chunk = len - off; if (chunk > 512) chunk = 512;
        for (unsigned int i = 0; i < 512; i++)
            sec[i] = (i < chunk) ? data[off + i] : 0;
        ata_write_sectors(lba, 1, sec);
        lba++; off += 512;
    }
}
static void fs_disk_read(unsigned int lba, unsigned char* data, unsigned int len) {
    unsigned char* sec = (unsigned char*)fs_disk_buf;
    unsigned int off = 0;
    while (off < len) {
        if (!ata_read_sectors(lba, 1, sec)) return;
        unsigned int chunk = len - off; if (chunk > 512) chunk = 512;
        for (unsigned int i = 0; i < chunk; i++) data[off + i] = sec[i];
        lba++; off += 512;
    }
}

int fs_save(void) {
    if (!ata_present()) return 0;
    for (int i = 0; i < 128; i++) fs_disk_buf[i] = 0;
    fs_disk_buf[0] = FS_MAGIC;
    fs_disk_buf[1] = FS_VERSION;
    fs_disk_buf[2] = (unsigned int)current_dir_id;
    fs_disk_buf[3] = (unsigned int)casino_balance;
    fs_disk_buf[4] = (unsigned int)sizeof(file_system);
    if (!ata_write_sectors(FS_HDR_LBA, 1, (unsigned char*)fs_disk_buf)) return 0;
    fs_disk_write(FS_DATA_LBA, (const unsigned char*)file_system, sizeof(file_system));
    fs_dirty = 0;
    return 1;
}

int fs_load(void) {
    if (!ata_present()) return 0;
    if (!ata_read_sectors(FS_HDR_LBA, 1, (unsigned char*)fs_disk_buf)) return 0;
    if (fs_disk_buf[0] != FS_MAGIC || fs_disk_buf[1] != FS_VERSION) return 0;
    if (fs_disk_buf[4] != (unsigned int)sizeof(file_system)) return 0;  /* layout drift */
    current_dir_id = (int)fs_disk_buf[2];
    casino_balance = (int)fs_disk_buf[3];
    fs_disk_read(FS_DATA_LBA, (unsigned char*)file_system, sizeof(file_system));
    return 1;
}

void fs_mark_dirty(void) { fs_dirty = 1; }
void fs_sync(void)       { if (fs_dirty) fs_save(); }
int  fs_is_dirty(void)   { return fs_dirty; }

void fs_format(void) {
    if (!ata_present()) return;
    for (int i = 0; i < 128; i++) fs_disk_buf[i] = 0;
    ata_write_sectors(FS_HDR_LBA, 1, (unsigned char*)fs_disk_buf);
    fs_dirty = 0;
}

int vfs_create_file(const char* name, int parent_id) {
    if (!name || !name[0]) return -1;
    int sl = next_free_slot();
    if (sl == -1) return -1;
    strncpy(file_system[sl].name, name, FILENAME_SIZE - 1);
    file_system[sl].name[FILENAME_SIZE - 1] = '\0';
    file_system[sl].type      = TYPE_FILE;
    file_system[sl].exists    = 1;
    file_system[sl].parent_id = parent_id;
    file_system[sl].id        = sl + 1;
    file_system[sl].content[0]= '\0';
    file_system[sl].size      = 0;
    fs_mark_dirty();
    return sl + 1;
}

int vfs_create_dir(const char* name, int parent_id) {
    if (!name || !name[0]) return -1;
    int sl = next_free_slot();
    if (sl == -1) return -1;
    strncpy(file_system[sl].name, name, FILENAME_SIZE - 1);
    file_system[sl].name[FILENAME_SIZE - 1] = '\0';
    file_system[sl].type      = TYPE_DIR;
    file_system[sl].exists    = 1;
    file_system[sl].parent_id = parent_id;
    file_system[sl].id        = sl + 1;
    file_system[sl].content[0]= '\0';
    file_system[sl].size      = 0;
    fs_mark_dirty();
    return sl + 1;
}

int vfs_delete(int entry_id) {
    int slot = entry_id - 1;
    if (slot < 0 || slot >= MAX_FILES) return -1;
    if (!file_system[slot].exists) return -1;
    file_system[slot].exists = 0;
    fs_mark_dirty();
    return 0;
}

int vfs_rename(int entry_id, const char* new_name) {
    if (!new_name || !new_name[0]) return -1;
    int slot = entry_id - 1;
    if (slot < 0 || slot >= MAX_FILES) return -1;
    if (!file_system[slot].exists) return -1;
    strncpy(file_system[slot].name, new_name, FILENAME_SIZE - 1);
    file_system[slot].name[FILENAME_SIZE - 1] = '\0';
    fs_mark_dirty();
    return 0;
}

int vfs_write_file(int entry_id, const char* data, int len) {
    int slot = entry_id - 1;
    if (slot < 0 || slot >= MAX_FILES) return -1;
    if (!file_system[slot].exists) return -1;
    if (file_system[slot].type != TYPE_FILE) return -1;
    if (len < 0) len = 0;
    if (len >= FILE_CONTENT_SIZE) len = FILE_CONTENT_SIZE - 1;
    for (int i = 0; i < len; i++) file_system[slot].content[i] = data[i];
    file_system[slot].content[len] = '\0';
    file_system[slot].size = len;
    fs_mark_dirty();
    return len;
}

/* ── Offset-aware VFS access (backs the per-process file-descriptor table) ─── */
int vfs_read_at(int entry_id, int pos, char* buf, int len) {
    int slot = entry_id - 1;
    if (slot < 0 || slot >= MAX_FILES) return -1;
    if (!file_system[slot].exists || file_system[slot].type != TYPE_FILE) return -1;
    if (len < 0) return -1;
    int size = file_system[slot].size; if (size < 0) size = 0;
    if (pos < 0) pos = 0;
    if (pos >= size) return 0;                         /* at/after EOF */
    int n = size - pos; if (n > len) n = len;
    for (int i = 0; i < n; i++) buf[i] = file_system[slot].content[pos + i];
    return n;
}

int vfs_write_at(int entry_id, int pos, const char* data, int len) {
    int slot = entry_id - 1;
    if (slot < 0 || slot >= MAX_FILES) return -1;
    if (!file_system[slot].exists || file_system[slot].type != TYPE_FILE) return -1;
    if (len < 0) return -1;
    if (pos < 0) pos = 0;
    if (pos >= FILE_CONTENT_SIZE - 1) return 0;        /* no room (keep 1 for NUL) */
    int n = len; if (pos + n > FILE_CONTENT_SIZE - 1) n = (FILE_CONTENT_SIZE - 1) - pos;
    /* zero-fill any gap between the old end and pos */
    for (int i = file_system[slot].size; i < pos; i++) file_system[slot].content[i] = 0;
    for (int i = 0; i < n; i++) file_system[slot].content[pos + i] = data[i];
    if (pos + n > file_system[slot].size) file_system[slot].size = pos + n;
    file_system[slot].content[file_system[slot].size] = '\0';   /* keep cat/strlen honest */
    fs_mark_dirty();
    return n;
}

/* ── Per-process file-descriptor tables (Phase 5.4, replaces "fd = VFS id") ───
   Each thread (== one process here, since there is no fork yet) owns a small
   table of descriptors. open()/create() allocate the lowest free index ≥ 3 and
   record the VFS entry id plus a byte offset that advances on every read/write —
   so sequential reads walk through a file and lseek can rewind. Indices 0/1/2
   are reserved for stdin/stdout/stderr (wired to a real /dev tty in a later
   phase). Freed en bloc by fd_release_all() when the scheduler reaps the
   process, so a reused thread slot never inherits stale descriptors.           */
#define FD_MAX_TID  7      /* == Kernel::Scheduler::MAX_THREADS (1 + 6)         */
#define FD_MAX      16     /* descriptors per process                          */
struct gfd { int used; int vfs_id; int pos; };
static struct gfd fd_tab[FD_MAX_TID][FD_MAX];   /* BSS zero-init → all closed  */

static int fd_valid_tid(int tid) { return tid >= 0 && tid < FD_MAX_TID; }

/* Allocate a descriptor for VFS entry `vfs_id` in thread `tid`; returns the fd
   (≥ 3) or -1 if the id is bad or the table is full.                          */
int fd_open_id(int tid, int vfs_id) {
    if (!fd_valid_tid(tid) || vfs_id <= 0) return -1;
    for (int i = 3; i < FD_MAX; i++) {
        if (!fd_tab[tid][i].used) {
            fd_tab[tid][i].used   = 1;
            fd_tab[tid][i].vfs_id = vfs_id;
            fd_tab[tid][i].pos    = 0;
            return i;
        }
    }
    return -1;
}

int fd_read(int tid, int fd, char* buf, int len) {
    if (fd == 0) return 0;                       /* stdin: EOF (no input yet)   */
    if (fd == 1 || fd == 2) return -1;           /* can't read stdout/stderr    */
    if (!fd_valid_tid(tid) || fd < 3 || fd >= FD_MAX) return -1;
    struct gfd* e = &fd_tab[tid][fd];
    if (!e->used) return -1;
    int n = vfs_read_at(e->vfs_id, e->pos, buf, len);
    if (n > 0) e->pos += n;
    return n;
}

int fd_write(int tid, int fd, const char* buf, int len) {
    if (fd == 1 || fd == 2) {                    /* stdout/stderr → terminal    */
        for (int i = 0; i < len; i++) term_putc(buf[i]);
        return len;
    }
    if (fd == 0) return -1;                       /* can't write stdin          */
    if (!fd_valid_tid(tid) || fd < 3 || fd >= FD_MAX) return -1;
    struct gfd* e = &fd_tab[tid][fd];
    if (!e->used) return -1;
    int n = vfs_write_at(e->vfs_id, e->pos, buf, len);
    if (n > 0) e->pos += n;
    return n;
}

int fd_close(int tid, int fd) {
    if (fd >= 0 && fd <= 2) return 0;            /* std streams: no-op success  */
    if (!fd_valid_tid(tid) || fd < 3 || fd >= FD_MAX) return -1;
    if (!fd_tab[tid][fd].used) return -1;
    fd_tab[tid][fd].used = 0;
    fd_tab[tid][fd].vfs_id = 0;
    fd_tab[tid][fd].pos = 0;
    return 0;
}

/* lseek: whence 0=SET, 1=CUR, 2=END. Returns the new offset or -1. */
int fd_lseek(int tid, int fd, int off, int whence) {
    if (!fd_valid_tid(tid) || fd < 3 || fd >= FD_MAX) return -1;
    struct gfd* e = &fd_tab[tid][fd];
    if (!e->used) return -1;
    int slot = e->vfs_id - 1;
    int size = (slot >= 0 && slot < MAX_FILES) ? file_system[slot].size : 0;
    int base = (whence == 1) ? e->pos : (whence == 2) ? size : 0;
    int np = base + off;
    if (np < 0) return -1;
    e->pos = np;
    return np;
}

/* Close every descriptor of a thread — called by the scheduler's reaper. */
void fd_release_all(int tid) {
    if (!fd_valid_tid(tid)) return;
    for (int i = 0; i < FD_MAX; i++) {
        fd_tab[tid][i].used = 0;
        fd_tab[tid][i].vfs_id = 0;
        fd_tab[tid][i].pos = 0;
    }
}

static void fs_init(int slot, const char* name, int type, int parent_id,
                    const char* content) {
    strncpy(file_system[slot].name, name, FILENAME_SIZE);
    file_system[slot].type = type;
    file_system[slot].exists = 1;
    file_system[slot].parent_id = parent_id;
    file_system[slot].id = slot + 1;
    if (content) {
        strcpy(file_system[slot].content, content);
        file_system[slot].size = strlen(content);
    } else {
        file_system[slot].content[0] = '\0';
        file_system[slot].size = 0;
    }
}

/* ── make_test_bmp ───────────────────────────────────────────────────────
   Writes a valid 32×32 24-bit uncompressed BMP directly into a VFS slot.
   Uses raw byte writes so null bytes in pixel data are preserved.
   The image is a 4-quadrant colour square (blue/red/green/yellow) with a
   2-pixel white border.                                                   */
static void make_test_bmp(int slot, const char* name, int parent_id)
{
    enum { W=32, H=32, BPP=3, ROW_STRIDE=96 }; /* 32*3=96, already 4-aligned */
    int pix_size  = ROW_STRIDE * H;   /* 3072 */
    int file_size = 54 + pix_size;    /* 3126 */

    strncpy(file_system[slot].name, name, FILENAME_SIZE);
    file_system[slot].type      = TYPE_FILE;
    file_system[slot].exists    = 1;
    file_system[slot].parent_id = parent_id;
    file_system[slot].id        = slot + 1;
    file_system[slot].size      = file_size;

    unsigned char* b = (unsigned char*)file_system[slot].content;
    for (int i = 0; i < file_size; i++) b[i] = 0;

    /* BMP file header (14 bytes) */
    b[0] = 'B'; b[1] = 'M';
    b[2]  = (unsigned char)(file_size        & 0xFF);
    b[3]  = (unsigned char)((file_size >>  8) & 0xFF);
    b[4]  = (unsigned char)((file_size >> 16) & 0xFF);
    b[5]  = (unsigned char)((file_size >> 24) & 0xFF);
    b[10] = 54; /* pixel data offset (fits in one byte) */

    /* BITMAPINFOHEADER (40 bytes at offset 14) */
    b[14] = 40;           /* header size */
    b[18] = (unsigned char)W;  b[22] = (unsigned char)H;
    b[26] = 1;            /* colour planes */
    b[28] = 24;           /* bits per pixel */
    b[34] = (unsigned char)(pix_size        & 0xFF);
    b[35] = (unsigned char)((pix_size >>  8) & 0xFF);
    b[36] = (unsigned char)((pix_size >> 16) & 0xFF);

    /* Pixel data — BMP row 0 = bottom of image */
    unsigned char* pdata = b + 54;
    int half_h = H / 2, half_w = W / 2;
    for (int bmp_row = 0; bmp_row < H; bmp_row++) {
        int img_row = H - 1 - bmp_row; /* 0 = image top */
        for (int col = 0; col < W; col++) {
            unsigned char r, g, bl;
            if (img_row < 2 || col < 2 || img_row >= H-2 || col >= W-2) {
                r=255; g=255; bl=255;           /* white border */
            } else if (img_row < half_h && col < half_w) {
                r=0; g=0; bl=200;               /* top-left  : blue  */
            } else if (img_row < half_h && col >= half_w) {
                r=200; g=0; bl=0;               /* top-right : red   */
            } else if (img_row >= half_h && col < half_w) {
                r=0; g=180; bl=0;               /* bot-left  : green */
            } else {
                r=220; g=200; bl=0;             /* bot-right : yellow*/
            }
            unsigned char* px = pdata + bmp_row * ROW_STRIDE + col * BPP;
            px[0]=bl; px[1]=g; px[2]=r;         /* BMP: BGR order */
        }
    }
}

void kmain(unsigned int mb2_info) {
    if (mb2_info >= 0x1000) {
        /* Use the actual Multiboot2 total_size (first 4 bytes of info struct).
           The old 4096-byte cap caused the framebuffer tag to be missed when
           the memory-map tag pushes it past offset 4096.                     */
        unsigned int total = *(unsigned int*)mb2_info;
        if (total < 8 || total > 65536) total = 65536;
        unsigned int off = 8;
        while (off + 8 <= total) {
            MB2Tag* t = (MB2Tag*)(mb2_info + off);
            if (t->type == MB2_TAG_END || t->size < 8) break;
            if (t->type == MB2_TAG_FB) {
                MB2TagFB* f = (MB2TagFB*)t;
                /* We request 800x600x32 in the Multiboot2 header; trust GRUB.
                   Drop the fb_type==1 guard that was silently rejecting EGA. */
                if (f->fb_bpp == 32) {
                    fb_phys_for_paging = (unsigned int)(f->fb_addr & 0xFFFFFFFFu);
                    gfx_init(fb_phys_for_paging, f->fb_pitch,
                             (int)f->fb_width, (int)f->fb_height, (int)f->fb_bpp);
                }
                break;
            }
            unsigned int step = (t->size + 7u) & ~7u;
            if (step < 8) break;
            off += step;
        }
    }

    gdt_install();
    paging_install();
    idt_install();
    acpi_init();   /* table walk + cache; real S5 shutdown replaces 0x604 hack */

    rand_seed = (unsigned int)(jiffies * 1664525u + 1013904223u) ^ 0xDEAD1337u;

    for (int i = 0; i < MAX_FILES; i++) file_system[i].exists = 0;



    fs_init(0, "readme.txt", TYPE_FILE, 0,
        "Welcome to GregOS v2.0!\n"
        "Type 'help' to list commands.\n"
        "Type 'neofetch' for system info.\n"
        "Try: snake  tetris  invaders  pong  breakout  2048  minesweeper  matrix\n"
        "Try: banner HELLO  lolcat rainbow  fortune\n");



    fs_init(1, "home",  TYPE_DIR, 0, 0);

    fs_init(2, "etc",   TYPE_DIR, 0, 0);

    fs_init(3, "tmp",   TYPE_DIR, 0, 0);

    fs_init(4, "usr",   TYPE_DIR, 0, 0);




    fs_init(5, "hostname", TYPE_FILE, 3, "gregos\n");
    fs_init(6, "passwd",   TYPE_FILE, 3,
        "root:x:0:0:root:/home:/bin/gregsh\n"
        "daemon:x:1:1:daemon:/usr/sbin:/usr/sbin/nologin\n");
    fs_init(7, "issue",    TYPE_FILE, 3,
        "GregOS v2.0 - Custom x86 OS\n"
        "Kernel: i386 bare-metal C/ASM\n");
    fs_init(8, "os-release", TYPE_FILE, 3,
        "NAME=GregOS\n"
        "VERSION=2.0\n"
        "ARCH=i386\n"
        "SHELL=gregsh\n");



    fs_init(9, "profile", TYPE_FILE, 2,
        "# GregOS user profile\n"
        "# This file runs on login\n"
        "setenv HOME /home\n"
        "setenv EDITOR nano\n");
    fs_init(10, "hello.sh", TYPE_FILE, 2,
        "echo Hello from GregOS!\n"
        "echo Today is a good day to code.\n"
        "fortune\n");
    fs_init(11, "notes.txt", TYPE_FILE, 2,
        "GregOS TODO:\n"
        "- Add more games\n"
        "- Improve the filesystem\n"
        "- Maybe a GUI someday?\n");



    fs_init(12, "log.txt", TYPE_FILE, 4,
        "[boot] VGA driver loaded\n"
        "[boot] Keyboard driver loaded\n"
        "[boot] Filesystem mounted\n"
        "[boot] Shell started\n");

    fs_init(13, "proc",    TYPE_DIR, 0, 0);
    fs_init(14, "dev",     TYPE_DIR, 0, 0);
    fs_init(15, "var",     TYPE_DIR, 0, 0);
    fs_init(16, "bin",     TYPE_DIR, 0, 0);

    fs_init(17, "cpuinfo", TYPE_FILE, 14,
        "processor\t: 0\n"
        "vendor_id\t: GenuineIntel\n"
        "model name\t: QEMU Virtual CPU i386\n"
        "cpu MHz\t\t: 1000.000\n"
        "cache size\t: 512 KB\n"
        "bogomips\t: 2000.00\n"
        "flags\t\t: fpu vme de pse tsc msr pae mce cx8 apic\n");
    fs_init(18, "meminfo", TYPE_FILE, 14,
        "MemTotal:\t  262144 kB\n"
        "MemFree:\t  196608 kB\n"
        "MemAvailable:\t  204800 kB\n"
        "Buffers:\t    4096 kB\n"
        "Cached:\t\t   32768 kB\n"
        "SwapTotal:\t       0 kB\n"
        "SwapFree:\t       0 kB\n");
    fs_init(19, "version", TYPE_FILE, 14,
        "GregOS version 2.0 (greg@gregos) (gcc 13.0, NASM 2.16) #1 SMP\n");
    fs_init(20, "uptime",  TYPE_FILE, 14, "0 0\n");
    fs_init(21, "net",     TYPE_DIR,  14, 0);
    fs_init(22, "dev",     TYPE_FILE, 21,
        "Inter-|   Receive                      |  Transmit\n"
        " face |bytes    packets errs drop|bytes    packets errs drop\n"
        "    lo: 12345       42    0    0  12345       42    0    0\n"
        "  eth0: 420690    1337    0    0  77777      666    0    0\n");

    fs_init(23, "null",    TYPE_FILE, 15, "");
    fs_init(24, "zero",    TYPE_FILE, 15, "\0\0\0\0\0\0\0\0");
    fs_init(25, "random",  TYPE_FILE, 15, "RANDOM_DATA_PLACEHOLDER");
    fs_init(26, "tty0",    TYPE_FILE, 15, "");
    fs_init(27, "sda",     TYPE_FILE, 15, "");

    fs_init(28, "log",     TYPE_DIR, 16, 0);
    fs_init(29, "syslog",  TYPE_FILE, 28,
        "[GregOS] boot: system started\n"
        "[GregOS] net: eth0 link up\n"
        "[GregOS] auth: login success for root\n");
    fs_init(30, "kern.log", TYPE_FILE, 28,
        "kernel: i386 booted\n"
        "kernel: VGA 80x25 initialized\n"
        "kernel: PS/2 keyboard IRQ1 ready\n"
        "kernel: PIT 100Hz jiffies started\n");

    fs_init(31, "share",   TYPE_DIR, 17, 0);
    fs_init(32, "lib",     TYPE_DIR, 17, 0);
    fs_init(33, "gregsh",  TYPE_FILE, 17, "#!/bin/gregsh\n# GregOS shell binary placeholder\n");

    fs_init(34, "grub.cfg", TYPE_FILE, 0,
        "menuentry \"GregOS\" { multiboot2 /boot/myos.bin }\n");
    fs_init(35, "boot", TYPE_DIR, 0, 0);
    fs_init(36, "grub", TYPE_DIR, 36, 0);

    fs_init(37, "motd", TYPE_FILE, 3,
        "Bienvenue sur GregOS v2.0\n"
        "Tape 'help' pour voir les commandes\n"
        "Tape 'casino' pour jouer!\n");

    /* Slot 38: test BMP image visible at root — open in FileManager */
    make_test_bmp(38, "logo.bmp", 0);

    /* ── Disk persistence: restore a saved VFS, else seed the disk once ──── */
    if (ata_present()) {
        if (fs_load()) {
            /* files from a previous session restored */
        } else {
            fs_save();   /* first boot on a blank disk: write the default FS */
        }
    }

    /* Seed (or refresh) the demo ELF program into the VFS as a root file, so it
       can be launched from the filesystem by name: `run hello [args]`.
       Always rewritten to the current build so it never goes stale on disk.   */
    {
        int hid = vfs_find("hello", 0);
        if (hid < 0) hid = vfs_create_file("hello", 0);   /* parent 0 = root */
        if (hid > 0) vfs_write_file(hid, (const char*)userapp_elf, (int)userapp_elf_len);
    }

    /* A plain-text file a userland program can open + read via syscalls:
       `run hello greeting` prints it from inside an isolated Ring-3 process.  */
    {
        const char* g =
            "Salutations depuis un fichier du VFS !\n"
            "Lu par un processus Ring 3 isole via SYS_OPEN + SYS_READ.\n"
            "-- GregOS, Seigneur du Kernel\n";
        int gid = vfs_find("greeting", 0);
        if (gid < 0) gid = vfs_create_file("greeting", 0);
        if (gid > 0) vfs_write_file(gid, g, (int)strlen(g));
    }

    strcpy(env_keys[0], "PATH");   strcpy(env_vals[0], "/bin:/usr/bin:/usr/local/bin");
    strcpy(env_keys[1], "USER");   strcpy(env_vals[1], "root");
    strcpy(env_keys[2], "SHELL");  strcpy(env_vals[2], "gregsh");
    strcpy(env_keys[3], "TERM");   strcpy(env_vals[3], "gregos-vga");
    strcpy(env_keys[4], "HOME");   strcpy(env_vals[4], "/home");
    strcpy(env_keys[5], "EDITOR"); strcpy(env_vals[5], "nano");
    env_count = 6;

    draw_interface();
    ps2mouse_init();

    extern int g_login_done;

    if (gfx_active()) {
        is_gui_active = 1;   /* route IRQ1 → EventQueue before login opens  */
        open_login_window(); /* animated login; sets g_login_done on success */
        /* gui_desktop_init() and gui_mode=1 are deferred until after login  */
    }



    if (!gui_mode) {
        char ts[12], ds[12]; get_time_string(ts); get_date_string(ds);
        int fc=0; for(int i=0;i<MAX_FILES;i++) if(file_system[i].exists) fc++;
        unsigned long sec = jiffies / 100;
        int um = (int)((sec % 3600) / 60);
        int us = (int)(sec % 60);

        unsigned char hfg = theme_fg[current_theme];
        unsigned char hbg = theme_bg[current_theme];
        unsigned char sfg = theme_sep[current_theme];
        int y = HEADER_HEIGHT;

        term_set_color(sfg, 0x00);
        term_move_cursor(0, y);
        term_putc('\xda'); for(int i=1;i<VGA_WIDTH-1;i++) term_putc('\xc4'); term_putc('\xbf');

        term_set_color(hfg, hbg);
        term_move_cursor(0, y+1);
        for(int i=0;i<VGA_WIDTH;i++) term_putc(' ');
        term_move_cursor(0, y+1); term_putc('\xb3');
        term_set_color(0x0F, hbg); term_print("  \xdb GregOS v2.0 \xdb");
        term_set_color(hfg, hbg); term_print("  \xb3  Seigneur du Kernel, Gardien des Bits");
        term_move_cursor(VGA_WIDTH-1, y+1); term_set_color(sfg, 0x00); term_putc('\xb3');

        term_set_color(sfg, 0x00);
        term_move_cursor(0, y+2);
        term_putc('\xc3'); for(int i=1;i<VGA_WIDTH-1;i++) term_putc('\xc4'); term_putc('\xb4');

        term_set_color(0x00, 0x00);
        term_move_cursor(0, y+3);
        for(int i=0;i<VGA_WIDTH;i++) term_putc(' ');
        term_move_cursor(0, y+3); term_set_color(sfg,0x00); term_putc('\xb3');
        term_move_cursor(2, y+3);
        term_set_color(0x0B, 0x00); term_print("Date: "); term_set_color(0x0F,0x00); term_print(ds);
        term_set_color(0x0B, 0x00); term_print("   Time: "); term_set_color(0x0F,0x00); term_print(ts);
        term_set_color(0x0B, 0x00); term_print("   Up: "); term_set_color(0x0F,0x00);
        term_print_int(um); term_putc('m'); term_print_int(us); term_putc('s');
        term_set_color(0x0B, 0x00); term_print("   Files: "); term_set_color(0x0F,0x00);
        term_print_int(fc); term_putc('/'); term_print_int(MAX_FILES);
        term_set_color(0x0B, 0x00); term_print("   Casino: "); term_set_color(casino_balance<100?0x0C:0x0A,0x00);
        term_print_int(casino_balance); term_print(" GC");
        term_move_cursor(VGA_WIDTH-1, y+3); term_set_color(sfg,0x00); term_putc('\xb3');

        term_set_color(sfg, 0x00);
        term_move_cursor(0, y+4);
        term_putc('\xc3'); for(int i=1;i<VGA_WIDTH-1;i++) term_putc('\xc4'); term_putc('\xb4');

        term_set_color(0x00, 0x00);
        term_move_cursor(0, y+5);
        for(int i=0;i<VGA_WIDTH;i++) term_putc(' ');
        term_move_cursor(0, y+5); term_set_color(sfg,0x00); term_putc('\xb3');
        term_move_cursor(2, y+5);
        term_set_color(0x0E,0x00); term_print("\xbf help");
        term_set_color(0x08,0x00); term_print("=commands  ");
        term_set_color(0x0E,0x00); term_print("\xbf neofetch");
        term_set_color(0x08,0x00); term_print("=sysinfo  ");
        term_set_color(0x0E,0x00); term_print("\xbf casino");
        term_set_color(0x08,0x00); term_print("=lobby  ");
        term_set_color(0x0E,0x00); term_print("\xbf music list");
        term_set_color(0x08,0x00); term_print("=songs  ");
        term_set_color(0x0E,0x00); term_print("\xbf scores");
        term_set_color(0x08,0x00); term_print("=hiscores");
        term_move_cursor(VGA_WIDTH-1, y+5); term_set_color(sfg,0x00); term_putc('\xb3');

        term_set_color(sfg, 0x00);
        term_move_cursor(0, y+6);
        term_putc('\xc0'); for(int i=1;i<VGA_WIDTH-1;i++) term_putc('\xc4'); term_putc('\xd9');

        term_set_color(0x0F, 0x00);
        term_move_cursor(0, y+7);
        term_putc('\n');
    } /* end if (!gui_mode) */

    char buf[BUFFER_SIZE]; int idx = 0;
    int c;

    run_foundation_tests();

    /* tty init + terminal window are deferred until after login */

    wm_draw();   /* first frame: login screen */

    /* ── Preemptive scheduler bootstrap ───────────────────────────────
       Spawn the background validation thread, then activate round-robin.
       The test_thread_func spins a character in VGA text cell [79].     */
    /* NB: test_thread_func (a Ring-0 busy-spin that proved preemption during
       scheduler bring-up) is intentionally NOT spawned — it burned ~2M loop
       iterations per quantum, stealing most of the CPU from the UI redraw loop
       and dropping the desktop from ~79 to ~28 FPS (visible cursor lag).      */
    /* scheduler_spawn_user(user_gui_test_app); */  /* Ring 3 test — disabled */
    scheduler_activate();

    /* ── Bring up the network stack (RTL8139 via QEMU slirp) ──────────── */
    net_init();

    while (1) {
        c = get_monitor_char();
        if (c == 0) {
            net_poll();   /* drain the NIC RX ring while idle */
            fs_sync();    /* flush the VFS to disk if a GUI edit dirtied it */
            if (is_gui_active) {
                /* Mouse events (harmless during login, active on desktop) */
                if ((ps2mouse_buttons() & 1) && !(mouse_prev_buttons & 1))
                    handle_gui_click(ps2mouse_gui_x(), ps2mouse_gui_y());
                mouse_prev_buttons = ps2mouse_buttons();
                /* Redraw every hlt: login animation, cursor blink, WM frames.
                   The cursor is part of this frame, so it tracks the mouse. */
                wm_draw();

                /* ── Login transition ───────────────────────────────────
                   LoginWindow sets g_login_done=1 then request_close().
                   The WM purges it on the wm_draw() above; we pick up
                   here on the very next c==0 iteration.                */
                if (g_login_done && !gui_mode) {
                    gui_desktop_init();
                    gui_mode = 1;
                    tty_system_init();
                    tty_create_terminal_window(90, 50, 600, 420, "terminal");
                    term_print("GregOS VTerm Initialized.\n");
                    print_prompt();
                    wm_draw();
                }
            }
            continue;
        }

        /* Desktop/window keyboard navigation */
        if (gui_mode) {
            if (desk_state == 2 && (c == KEY_ESC || c == 'q')) {
                desk_state = 0; wm_draw(); continue;
            }
            /* Only swallow keys on bare desktop (no WM windows open). */
            if (desk_state == 0 && !wm_has_windows()) {
                if (c == '\n' || c == '\r')
                    handle_gui_click(g_icons[sel_icon].x + 1, g_icons[sel_icon].y + 1);
                continue;
            }
        }


        if (c == KEY_PGUP) { sb_scroll_up();   continue; }
        if (c == KEY_PGDN) { sb_scroll_down(); continue; }



        if (sb_is_active()) sb_exit();



        if (c == KEY_UP) {
            if (history_count == 0) continue;
            if (history_nav < history_count - 1 && history_nav < HISTORY_SIZE - 1)
                history_nav++;
            const char* entry = history_get(history_nav);
            if (!entry) continue;
            beep_nav();
            for (int i = 0; i < idx; i++) term_putc('\b');
            strcpy(buf, entry);
            idx = strlen(buf);
            term_print(buf);
            wm_draw();
            continue;
        }



        if (c == KEY_DOWN) {
            if (history_nav <= 0) {
                history_nav = -1;
                for (int i = 0; i < idx; i++) term_putc('\b');
                buf[0] = '\0'; idx = 0;
                wm_draw();
                continue;
            }
            history_nav--;
            const char* entry = history_get(history_nav);
            if (!entry) continue;
            beep_nav();
            for (int i = 0; i < idx; i++) term_putc('\b');
            strcpy(buf, entry);
            idx = strlen(buf);
            term_print(buf);
            wm_draw();
            continue;
        }



        if (c == KEY_CTRL_C) {
            beep_fail();
            term_print("^C\n");
            buf[0] = '\0'; idx = 0;
            history_nav = -1;
            print_prompt();
            wm_draw();
            continue;
        }



        if (c == KEY_TAB) {
            buf[idx] = '\0';
            do_tab_complete(buf, &idx);
            wm_draw();
            continue;
        }

        if (c == '\n') {
            term_putc('\n');
            buf[idx] = '\0';
            if (idx > 0) {
                history_push(buf);
                wm_draw();
                execute_command(buf);
            }
            idx = 0;
            history_nav = -1;
            print_prompt();
            wm_draw();
        } else if (c == '\b' && idx > 0) {
            term_putc('\b');
            idx--;
            buf[idx] = '\0';
            wm_draw();
        } else if (c >= 32 && c <= 126 && idx < BUFFER_SIZE - 1) {
            term_putc(c);
            buf[idx++] = c;
            buf[idx] = '\0';
            wm_draw();
        }

        if (!gui_mode)
            for (int i = 0; i < 200000; i++) __asm__ volatile("nop");
    }
}