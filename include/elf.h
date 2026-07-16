#ifndef ELF_H
#define ELF_H

/* ELF32 structures — freestanding i386 bare-metal.
   No libc dependency; all fields use explicit widths via GCC primitives. */

/* ── Magic / identification ────────────────────────────────────────────── */
#define ELF_MAGIC0   0x7Fu
#define ELF_MAGIC1   'E'
#define ELF_MAGIC2   'L'
#define ELF_MAGIC3   'F'

#define ELFCLASS32   1u    /* 32-bit objects    */
#define ELFDATA2LSB  1u    /* Little-endian     */

/* ── Object types ───────────────────────────────────────────────────────── */
#define ET_EXEC      2u    /* Executable file          */
#define ET_DYN       3u    /* Position-independent (PIE) */

/* ── Machine types ──────────────────────────────────────────────────────── */
#define EM_386       3u    /* Intel 80386       */

/* ── Segment types ──────────────────────────────────────────────────────── */
#define PT_LOAD      1u    /* Loadable segment  */

/* ── Segment flags ──────────────────────────────────────────────────────── */
#define PF_X         0x1u  /* Execute           */
#define PF_W         0x2u  /* Write             */
#define PF_R         0x4u  /* Read              */

/* ── ELF32 file header (52 bytes) ───────────────────────────────────────── */
typedef struct {
    unsigned char  e_ident[16]; /* Magic, class, data, ABI, padding     */
    unsigned short e_type;      /* Object type (ET_EXEC …)              */
    unsigned short e_machine;   /* Machine type (EM_386)                */
    unsigned int   e_version;   /* Object version (always 1)            */
    unsigned int   e_entry;     /* Virtual entry-point address          */
    unsigned int   e_phoff;     /* Program-header table file offset     */
    unsigned int   e_shoff;     /* Section-header table file offset     */
    unsigned int   e_flags;     /* Processor-specific flags             */
    unsigned short e_ehsize;    /* ELF header size in bytes (52)        */
    unsigned short e_phentsize; /* Program header entry size (32)       */
    unsigned short e_phnum;     /* Number of program headers            */
    unsigned short e_shentsize; /* Section header entry size            */
    unsigned short e_shnum;     /* Number of section headers            */
    unsigned short e_shstrndx;  /* Section name string-table index      */
} Elf32_Ehdr;

/* ── ELF32 program header (32 bytes) ────────────────────────────────────── */
typedef struct {
    unsigned int p_type;    /* Segment type (PT_LOAD …)              */
    unsigned int p_offset;  /* Segment offset in file                */
    unsigned int p_vaddr;   /* Virtual address in memory             */
    unsigned int p_paddr;   /* Physical address (same for flat maps) */
    unsigned int p_filesz;  /* Size of segment in file               */
    unsigned int p_memsz;   /* Size of segment in memory (≥ filesz)  */
    unsigned int p_flags;   /* Segment permission flags (PF_*)       */
    unsigned int p_align;   /* Required alignment                    */
} Elf32_Phdr;

/* ── Public loader API (C-linkage so kernel.c can call if needed) ───────── */
#ifdef __cplusplus
extern "C" {
#endif

/* Load a named ELF32 binary from the in-memory VFS.
   Searches root and /bin/.
   - Validates ELF magic, machine (i386), type (ET_EXEC).
   - Copies each PT_LOAD segment to its physical address (p_paddr),
     zeros the BSS difference (p_memsz – p_filesz).
   - Logs diagnostics to the active tty via tty_putc.
   Returns the entry-point address on success, 0 on any failure. */
unsigned int elf_load(const char* name);

/* Call an ELF entry point in kernel mode (flat identity-mapped ring 0).
   Safe only when the binary was linked within the PSE-mapped range (0–48 MB).
   No-op if entry_point == 0. */
int elf_execute(unsigned int entry_point);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ELF_H */
