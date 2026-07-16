/* userapp/main.c — a real C userland program for GregOS.
 *
 * Compiled to a static ET_EXEC linked at 0x40000000 (see the Makefile rule),
 * freestanding and without any libc: it reaches the kernel ONLY through INT
 * 0x80 syscalls. The `elfrun` shell command loads it into an isolated Ring-3
 * address space (kernel mapped supervisor-only) and runs it. Unlike the tiny
 * hand-written user.asm, this is genuine compiled C — helper functions, loops,
 * .rodata strings — proving the exec path handles a real ELF, not just a blob. */

/* ── syscall numbers (mirror Kernel::SyscallNumber) ── */
#define SYS_EXIT       1u
#define SYS_WRITE      3u
#define SYS_GET_TICKS  5u
#define SYS_MMAP       7u
#define SYS_MUNMAP     8u
#define SYS_OPEN       9u
#define SYS_READ      10u
#define SYS_CLOSE     11u
#define SYS_GET_PID   12u
#define SYS_CREATE    13u
#define SYS_WRITE_FILE 14u
#define SYS_LSEEK     15u

static inline unsigned sc0(unsigned nr) {
    unsigned r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr) : "memory"); return r;
}
static inline unsigned sc1(unsigned nr, unsigned a) {
    unsigned r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "b"(a) : "memory"); return r;
}
static inline unsigned sc2(unsigned nr, unsigned a, unsigned b) {
    unsigned r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "b"(a), "c"(b) : "memory"); return r;
}
static inline unsigned sc3(unsigned nr, unsigned a, unsigned b, unsigned c) {
    unsigned r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "b"(a), "c"(b), "d"(c) : "memory"); return r;
}

static int  slen(const char* s) { int n = 0; while (s[n]) n++; return n; }
static void puts_(const char* s) { sc3(SYS_WRITE, 1u, (unsigned)s, (unsigned)slen(s)); }
static void utoa_(unsigned v, char* b) {
    char t[12]; int i = 0;
    if (!v) { b[0] = '0'; b[1] = 0; return; }
    while (v && i < 11) { t[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0; while (i) b[j++] = t[--i]; b[j] = 0;
}
static void utohex_(unsigned v, char* b) {
    b[0] = '0'; b[1] = 'x';
    for (int k = 0; k < 8; k++) {
        unsigned n = (v >> ((7 - k) * 4)) & 0xFu;
        b[2 + k] = (char)(n < 10 ? '0' + n : 'A' + n - 10);
    }
    b[10] = 0;
}
static int is_prime(unsigned n) {
    if (n < 2) return 0;
    for (unsigned d = 2; d * d <= n; d++) if (n % d == 0) return 0;
    return 1;
}
/* Tiny string builder for composing a file's contents in a user buffer. */
static int sapp(char* d, int p, const char* s) { int i = 0; while (s[i]) d[p++] = s[i++]; d[p] = 0; return p; }
static int sappu(char* d, int p, unsigned v) { char t[12]; utoa_(v, t); return sapp(d, p, t); }

/* Naked entry: the kernel put argc at [esp] and argv[] just above it. Read them
   before any prologue can disturb esp, then call cmain(argc, argv) cdecl. */
__attribute__((naked, used)) void _start(void) {
    __asm__ volatile(
        "movl (%esp), %eax\n"   /* argc */
        "leal 4(%esp), %edx\n"  /* argv */
        "pushl %edx\n"
        "pushl %eax\n"
        "call cmain\n"
        "1: jmp 1b\n"           /* cmain calls SYS_EXIT and never returns */
    );
}

int cmain(int argc, char** argv) {
    char b[16];
    puts_("\n[Capp] Vrai programme C compile (ELF), isole en Ring 3 !\n");

    /* Command-line arguments handed in by the kernel on the user stack. */
    puts_("[Capp] argc="); utoa_((unsigned)argc, b); puts_(b); puts_("\n");
    for (int i = 0; i < argc; i++) {
        puts_("[Capp]   argv["); utoa_((unsigned)i, b); puts_(b); puts_("]=");
        puts_(argv[i]); puts_("\n");
    }

    unsigned pid = sc0(SYS_GET_PID);
    puts_("[Capp] pid=");   utoa_(pid,                b); puts_(b);
    puts_("  ticks=");      utoa_(sc0(SYS_GET_TICKS), b); puts_(b); puts_("\n");

    /* Real computation with a helper call + loops (exercises .text). */
    unsigned primes = 0;
    for (unsigned n = 2; n < 100; n++) if (is_prime(n)) primes++;
    puts_("[Capp] nombre de premiers < 100 = "); utoa_(primes, b); puts_(b); puts_("\n");

    /* Dynamic memory via SYS_MMAP. It now returns a user VA mapped into THIS
       process's own address space (0x50000000 region) — memory the isolated
       process can actually read and write.                                  */
    unsigned p = sc1(SYS_MMAP, 2048u);
    puts_("[Capp] SYS_MMAP 2048o -> VA="); utohex_(p, b); puts_(b); puts_("\n");
    if (p) {
        volatile unsigned char* m = (volatile unsigned char*)p;
        unsigned sum = 0;
        for (int i = 0; i < 2048; i++) { m[i] = (unsigned char)(i + pid); sum += m[i]; }
        sc1(SYS_MUNMAP, p);
        puts_("[Capp] ecrit+relu 2048o puis SYS_MUNMAP, checksum="); utoa_(sum, b); puts_(b); puts_("\n");
    }

    /* File I/O + the per-process file-descriptor table: SYS_OPEN returns a small
       process-local fd (>= 3, not a raw VFS id). Reading advances a per-fd byte
       offset, so successive reads walk through the file; SYS_LSEEK rewinds it.
       The kernel never hands us a pointer — we read into memory we own.        */
    if (argc >= 2) {
        static char chunk[64];
        puts_("[Capp] SYS_OPEN \""); puts_(argv[1]); puts_("\" ... ");
        int fd = (int)sc2(SYS_OPEN, (unsigned)argv[1], 0u);
        if (fd < 0) {
            puts_("introuvable.\n");
        } else {
            puts_("fd="); utoa_((unsigned)fd, b); puts_(b);
            puts_(" (petit entier par processus)\n");

            int n1 = (int)sc3(SYS_READ, (unsigned)fd, (unsigned)chunk, 24u);
            if (n1 > 0) { chunk[n1] = 0; puts_("[Capp] read #1 (24o)         : "); puts_(chunk); puts_("\n"); }
            int n2 = (int)sc3(SYS_READ, (unsigned)fd, (unsigned)chunk, 24u);
            if (n2 > 0) { chunk[n2] = 0; puts_("[Capp] read #2 (offset avance): "); puts_(chunk); puts_("\n"); }

            int np = (int)sc3(SYS_LSEEK, (unsigned)fd, 0u, 0u);   /* SEEK_SET → rewind */
            puts_("[Capp] SYS_LSEEK debut -> pos="); utoa_((unsigned)np, b); puts_(b); puts_("\n");
            int n3 = (int)sc3(SYS_READ, (unsigned)fd, (unsigned)chunk, 24u);
            if (n3 > 0) { chunk[n3] = 0; puts_("[Capp] read apres rewind     : "); puts_(chunk); puts_("\n"); }

            sc1(SYS_CLOSE, (unsigned)fd);
        }
    }

    /* The write path, mirror of the read path: an isolated Ring-3 process can
       now CREATE a file and WRITE to it through syscalls. We compose a small
       report in a buffer we own, SYS_CREATE the file named by argv[2],
       SYS_WRITE_FILE our bytes into it, then read it straight back to prove the
       round-trip landed in the kernel's VFS. `cat argv[2]` in the shell after
       the program exits shows the file the sandbox created.                    */
    if (argc >= 3) {
        static char outbuf[512];
        int p = 0;
        p = sapp(outbuf, p, "Rapport ecrit par un processus Ring 3 isole.\n");
        p = sapp(outbuf, p, "pid=");            p = sappu(outbuf, p, pid);
        p = sapp(outbuf, p, " ticks=");         p = sappu(outbuf, p, sc0(SYS_GET_TICKS));
        p = sapp(outbuf, p, " premiers<100=");  p = sappu(outbuf, p, primes);
        p = sapp(outbuf, p, "\n-- ecrit via SYS_CREATE + SYS_WRITE_FILE\n");

        puts_("[Capp] SYS_CREATE \""); puts_(argv[2]); puts_("\" ... ");
        int wfd = (int)sc2(SYS_CREATE, (unsigned)argv[2], 0u);
        if (wfd < 0) {
            puts_("echec.\n");
        } else {
            puts_("fd="); utoa_((unsigned)wfd, b); puts_(b);
            int wn = (int)sc3(SYS_WRITE_FILE, (unsigned)wfd, (unsigned)outbuf, (unsigned)p);
            puts_(", SYS_WRITE_FILE "); utoa_((unsigned)(wn < 0 ? 0 : wn), b); puts_(b);
            puts_(" octets ecrits.\n");

            /* Read it back through the read path to confirm it persisted. */
            static char backbuf[512];
            int rfd = (int)sc2(SYS_OPEN, (unsigned)argv[2], 0u);
            int rn = (rfd < 0) ? -1 : (int)sc3(SYS_READ, (unsigned)rfd, (unsigned)backbuf, sizeof(backbuf) - 1);
            if (rn >= 0) {
                if (rn > (int)sizeof(backbuf) - 1) rn = (int)sizeof(backbuf) - 1;
                backbuf[rn] = 0;
                puts_("[Capp] relecture ("); utoa_((unsigned)rn, b); puts_(b);
                puts_(" octets) :\n---8<---\n"); puts_(backbuf); puts_("--->8---\n");
            }
            sc1(SYS_CLOSE, (unsigned)rfd);
        }
    }

    /* Security probe: aim syscalls at a KERNEL address (0x00100000, the kernel
       image — supervisor-only in this isolated space). Every one must be REFUSED
       (return -1) by the kernel's user-pointer validation. Without that guard,
       SYS_READ here would overwrite kernel code, SYS_WRITE would leak kernel
       memory to the terminal, and SYS_GET_HEAP would scribble on the kernel.    */
    if (argc >= 4) {
        unsigned KADDR = 0x00100000u;
        puts_("[Capp] --- sonde securite : pointeurs noyau hostiles ---\n");

        int gfd = (int)sc2(SYS_OPEN, (unsigned)argv[1], 0u);   /* a valid fd */
        int r_read = (int)sc3(SYS_READ, (unsigned)gfd, KADDR, 16u);
        puts_("[Capp] SYS_READ  buf=0x00100000 -> ");
        puts_((unsigned)r_read == 0xFFFFFFFFu ? "REFUSE (-1) OK\n" : "ACCEPTE (FUITE!) \n");
        sc1(SYS_CLOSE, (unsigned)gfd);

        int r_write = (int)sc3(SYS_WRITE, 1u, KADDR, 16u);
        puts_("[Capp] SYS_WRITE buf=0x00100000 -> ");
        puts_((unsigned)r_write == 0xFFFFFFFFu ? "REFUSE (-1) OK\n" : "ACCEPTE (FUITE!) \n");

        int r_heap = (int)sc2(6u /*SYS_GET_HEAP*/, KADDR, 0u);
        puts_("[Capp] SYS_GET_HEAP out=0x00100000 -> ");
        puts_((unsigned)r_heap == 0xFFFFFFFFu ? "REFUSE (-1) OK\n" : "ACCEPTE (CORRUPTION!) \n");
    }

    puts_("[Capp] Termine proprement via SYS_EXIT.\n");
    sc1(SYS_EXIT, 0u);
    for (;;) { }
}
