/* elf.cpp — ELF32 loader for GregOS.
   Freestanding: no libc, no exceptions.

   Built-in ELF table: binaries embedded as C arrays take priority over VFS.

   elf_load(name):
     1. Search built-in table, then VFS (root then /bin/).
     2. Validate ELF32 magic, EM_386.
     3. ET_EXEC — load PT_LOAD segments to physical p_paddr; entry = e_entry.
     4. ET_DYN  — allocate heap buffer for span, load PT_LOAD relative to base;
                  entry = load_base + e_entry.
     5. Guard: reject entry < 1 MB (would corrupt kernel).
     6. Return real entry point on success, 0 on any failure.

   elf_execute(entry_point):
     Calls entry as int(void), logs return value, returns it.           */

#include "../include/elf.h"
#include "../include/vfs.h"
#include "../include/tty.h"
#include "../include/blackjack_elf_data.h"

extern "C" void* kmalloc(unsigned int size);

/* ── Tiny freestanding helpers ─────────────────────────────────────────── */

static void elf_putc(char c)  { tty_putc(c); }

static void elf_puts(const char* s) {
    while (*s) elf_putc(*s++);
}

static void elf_puthex(unsigned int v) {
    elf_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        unsigned int n = (v >> shift) & 0xFu;
        elf_putc((char)(n < 10 ? '0' + n : 'A' + n - 10));
    }
}

static void elf_put_int(int v) {
    if (v < 0) { elf_putc('-'); v = -v; }
    char tmp[12]; int ti = 0;
    if (v == 0) { elf_putc('0'); return; }
    while (v > 0) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
    for (int k = ti - 1; k >= 0; --k) elf_putc(tmp[k]);
}

static int elf_streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void elf_memcpy(unsigned char* dst, const unsigned char* src, unsigned int n) {
    for (unsigned int i = 0; i < n; i++) dst[i] = src[i];
}

static void elf_memzero(unsigned char* dst, unsigned int n) {
    for (unsigned int i = 0; i < n; i++) dst[i] = 0;
}

/* ── Built-in ELF table ────────────────────────────────────────────────── */

struct BuiltinElf {
    const char*          name;
    const unsigned char* data;
    unsigned int         size;
};

static const BuiltinElf s_builtins[] = {
    { "blackjack.elf", blackjack_elf, blackjack_elf_len },
    { 0, 0, 0 }
};

static const BuiltinElf* builtin_find(const char* name) {
    for (const BuiltinElf* e = s_builtins; e->name; ++e)
        if (elf_streq(e->name, name)) return e;
    return 0;
}

/* ── VFS lookup: root then /bin/ ───────────────────────────────────────── */

static int elf_vfs_find(const char* name, int* size_out) {
    *size_out = 0;
    VFSEntry entries[64];

    int n = vfs_list_dir(0, entries, 64);
    for (int i = 0; i < n; i++) {
        if (entries[i].type == VFS_TYPE_FILE && elf_streq(entries[i].name, name)) {
            *size_out = entries[i].size;
            return entries[i].id;
        }
    }

    for (int i = 0; i < n; i++) {
        if (entries[i].type == VFS_TYPE_DIR && elf_streq(entries[i].name, "bin")) {
            VFSEntry bin[32];
            int m = vfs_list_dir(entries[i].id, bin, 32);
            for (int j = 0; j < m; j++) {
                if (bin[j].type == VFS_TYPE_FILE && elf_streq(bin[j].name, name)) {
                    *size_out = bin[j].size;
                    return bin[j].id;
                }
            }
            break;
        }
    }
    return -1;
}

/* ── Core loader: validate + load segments from a byte buffer ─────────── */
/* Returns real entry point (already rebased for PIE), 0 on failure.       */

static const unsigned int MAP_LIMIT = 48u * 1024u * 1024u;  /* PSE coverage */
static const unsigned int KBASE     = 0x100000u;             /* 1 MB */

static unsigned int elf_load_buf(const unsigned char* buf, unsigned int fsize,
                                 const char* name)
{
    if (fsize < sizeof(Elf32_Ehdr)) {
        elf_puts("[ELF] Fichier trop petit\n");
        return 0;
    }

    const Elf32_Ehdr* eh = (const Elf32_Ehdr*)buf;

    /* Magic */
    if (eh->e_ident[0] != ELF_MAGIC0 || eh->e_ident[1] != ELF_MAGIC1 ||
        eh->e_ident[2] != ELF_MAGIC2 || eh->e_ident[3] != ELF_MAGIC3) {
        elf_puts("[ELF] Magic invalide\n");
        return 0;
    }
    if (eh->e_ident[4] != ELFCLASS32) {
        elf_puts("[ELF] Classe non supportee (ELF64?)\n");
        return 0;
    }
    if (eh->e_machine != EM_386) {
        elf_puts("[ELF] Architecture non supportee (i386 requis)\n");
        return 0;
    }
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) {
        elf_puts("[ELF] Type non supporte (ET_EXEC ou ET_DYN requis)\n");
        return 0;
    }
    if (eh->e_phnum == 0 || eh->e_phentsize < (unsigned short)sizeof(Elf32_Phdr)) {
        elf_puts("[ELF] Program headers absents\n");
        return 0;
    }

    unsigned int load_base = 0;

    if (eh->e_type == ET_DYN) {
        /* PIE: compute span across all PT_LOAD segments */
        unsigned int span = 0;
        for (unsigned short pi = 0; pi < eh->e_phnum; pi++) {
            const Elf32_Phdr* ph =
                (const Elf32_Phdr*)(buf + eh->e_phoff + (unsigned)pi * eh->e_phentsize);
            if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
            unsigned int end = ph->p_vaddr + ph->p_memsz;
            if (end > span) span = end;
        }
        if (span == 0) {
            elf_puts("[ELF] Aucun segment PT_LOAD\n");
            return 0;
        }
        load_base = (unsigned int)kmalloc(span);
        if (!load_base) {
            elf_puts("[ELF] kmalloc echoue\n");
            return 0;
        }
        elf_memzero((unsigned char*)load_base, span);
    }

    /* Load PT_LOAD segments */
    for (unsigned short pi = 0; pi < eh->e_phnum; pi++) {
        const Elf32_Phdr* ph =
            (const Elf32_Phdr*)(buf + eh->e_phoff + (unsigned)pi * eh->e_phentsize);

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        if (ph->p_offset + ph->p_filesz > fsize) {
            elf_puts("[ELF] Segment depasse la taille du fichier\n");
            return 0;
        }

        unsigned char* dst;
        if (eh->e_type == ET_DYN) {
            dst = (unsigned char*)load_base + ph->p_vaddr;
        } else {
            /* ET_EXEC: bounds check physical address */
            if (ph->p_paddr + ph->p_memsz > MAP_LIMIT) {
                elf_puts("[ELF] Segment hors zone mappee (>48 Mo)\n");
                return 0;
            }
            dst = (unsigned char*)ph->p_paddr;
        }

        const unsigned char* src = buf + ph->p_offset;
        elf_memcpy(dst, src, ph->p_filesz);

        if (ph->p_memsz > ph->p_filesz)
            elf_memzero(dst + ph->p_filesz, ph->p_memsz - ph->p_filesz);
    }

    unsigned int real_entry = (eh->e_type == ET_DYN)
                              ? load_base + eh->e_entry
                              : eh->e_entry;

    /* Safety: never hand control to an address inside kernel image region */
    if (real_entry < KBASE) {
        elf_puts("[ELF] Adresse d'entree invalide: ");
        elf_puthex(real_entry);
        elf_puts("\n");
        return 0;
    }

    elf_puts("[ELF] Charge: ");
    elf_puts(name);
    elf_puts("  entry=");
    elf_puthex(real_entry);
    elf_puts("\n");

    return real_entry;
}

/* ── elf_load ─────────────────────────────────────────────────────────── */

extern "C" unsigned int elf_load(const char* name)
{
    /* 1. Check built-in table first */
    const BuiltinElf* bi = builtin_find(name);
    if (bi) {
        elf_puts("[ELF] Source: builtin\n");
        return elf_load_buf(bi->data, bi->size, name);
    }

    /* 2. Fall back to VFS (text-only; useful only for small ET_EXEC blobs) */
    int fsize = 0;
    int fid   = elf_vfs_find(name, &fsize);

    if (fid < 0) {
        elf_puts("[ELF] Fichier introuvable: ");
        elf_puts(name);
        elf_puts("\n");
        return 0;
    }

    /* +1 so vfs_read_file can null-terminate without overrun */
    unsigned char* buf = (unsigned char*)kmalloc((unsigned int)fsize + 1);
    if (!buf) {
        elf_puts("[ELF] kmalloc echoue\n");
        return 0;
    }

    int got = vfs_read_file(fid, (char*)buf, fsize);
    if (got < (int)sizeof(Elf32_Ehdr)) {
        elf_puts("[ELF] Lecture incomplete\n");
        return 0;
    }

    return elf_load_buf(buf, (unsigned int)got, name);
}

/* ── elf_execute ──────────────────────────────────────────────────────── */

extern "C" int elf_execute(unsigned int entry_point)
{
    if (!entry_point) return -1;
    typedef int (*entry_fn_t)(void);
    entry_fn_t fn;
    __builtin_memcpy(&fn, &entry_point, sizeof(fn));
    int ret = fn();
    elf_puts("[ELF] Execution terminee. Code de retour : ");
    elf_put_int(ret);
    elf_puts("\n");
    return ret;
}
