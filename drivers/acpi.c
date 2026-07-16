/* drivers/acpi.c — ACPI table discovery + real S5 shutdown / reset reboot.
   Freestanding C, no libc. See include/acpi.h for the contract.

   Boot-time flow (acpi_init):
     1. Find the RSDP: scan the EBDA's first KB, then the BIOS area
        0xE0000-0xFFFFF, for the 16-byte-aligned "RSD PTR " signature with a
        valid 20-byte checksum.
     2. Walk the RSDT (validating signature + full checksum) to the FADT
        ("FACP"), pulling PM1a/PM1b control ports, the SMI enable handshake,
        and the ACPI 2.0 reset register.
     3. Parse the DSDT's AML for the \_S5 package to get SLP_TYPa/SLP_TYPb.
     4. If SCI_EN is clear, perform the SMI_CMD/ACPI_ENABLE handshake.
   Every table pointer may live near the top of low RAM (QEMU: ~255 MB with
   -m 256M), above the 0-48 MB boot identity map — paging_map_4mb() (kernel.c)
   extends the kernel page directory before each dereference.

   All results are cached in statics so acpi_shutdown()/acpi_reboot() do pure
   port I/O — no table access at runtime, valid under any CR3.               */

#include "../include/acpi.h"
#include "../include/ports.h"
#include "../include/vga.h"

extern void paging_map_4mb(unsigned int phys);
extern void itoa(int n, char* buf);                  /* kernel.c */

/* ── Cached results ─────────────────────────────────────────────────────── */

static int          s_ok         = 0;   /* FADT + S5 parsed, shutdown ready */
static unsigned int s_rsdp_addr  = 0;
static unsigned int s_rsdt_addr  = 0;
static int          s_table_cnt  = 0;
static char         s_table_sig[16][5];             /* first 16 signatures  */
static unsigned int s_pm1a_cnt   = 0;               /* PM1a control port    */
static unsigned int s_pm1b_cnt   = 0;
static unsigned short s_slp_typa = 0;               /* S5 sleep type values */
static unsigned short s_slp_typb = 0;
static unsigned int s_smi_cmd    = 0;
static unsigned char s_acpi_enable = 0;
static unsigned char s_reset_ok  = 0;               /* reset register valid */
static unsigned char s_reset_space = 0;             /* 1 = I/O port         */
static unsigned int s_reset_addr = 0;
static unsigned char s_reset_val = 0;

#define SLP_EN   (1u << 13)
#define SCI_EN   1u

/* ── Helpers ────────────────────────────────────────────────────────────── */

static int sig_match(const unsigned char* p, const char* sig) {
    for (int i = 0; sig[i]; i++) if (p[i] != (unsigned char)sig[i]) return 0;
    return 1;
}

static int checksum_ok(const unsigned char* p, unsigned int len) {
    unsigned char sum = 0;
    for (unsigned int i = 0; i < len; i++) sum = (unsigned char)(sum + p[i]);
    return sum == 0;
}

static unsigned int rd32(const unsigned char* p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

/* Map the pages backing [phys, phys+len) before touching them. */
static const unsigned char* table_at(unsigned int phys, unsigned int len) {
    if (!phys) return 0;
    paging_map_4mb(phys);
    if (len) paging_map_4mb(phys + len - 1u);
    return (const unsigned char*)phys;
}

/* ── RSDP scan ──────────────────────────────────────────────────────────── */

static unsigned int rsdp_scan_range(unsigned int start, unsigned int end) {
    for (unsigned int a = start & ~0xFu; a + 20u <= end; a += 16u) {
        const unsigned char* p = (const unsigned char*)a;
        if (sig_match(p, "RSD PTR ") && checksum_ok(p, 20u)) return a;
    }
    return 0;
}

/* Address of the BDA's EBDA-segment word, held in a volatile so GCC cannot
   constant-fold the low-memory dereference (silences -Warray-bounds).      */
static volatile unsigned int s_bda_ebda_ptr = 0x40Eu;

static unsigned int rsdp_find(void) {
    /* EBDA segment pointer lives at 0x40E in the BDA. */
    unsigned int ebda = ((unsigned int)(*(volatile unsigned short*)s_bda_ebda_ptr)) << 4;
    if (ebda >= 0x80000u && ebda < 0xA0000u) {
        unsigned int hit = rsdp_scan_range(ebda, ebda + 1024u);
        if (hit) return hit;
    }
    return rsdp_scan_range(0xE0000u, 0x100000u);
}

/* ── DSDT \_S5 AML parse ────────────────────────────────────────────────── */

static int parse_s5(const unsigned char* dsdt, unsigned int len) {
    if (len < 36u + 8u) return 0;
    /* Scan the AML body for NameOp "_S5_" then a PackageOp of sleep values.
       Layout: 08 '_' 'S' '5' '_' 12 <PkgLength> <NumElements>
               [0A] SLP_TYPa [0A] SLP_TYPb ...
       0x0A is BytePrefix; small constants 0/1 may appear raw (ZeroOp/OneOp). */
    for (unsigned int i = 36u; i + 8u < len; i++) {
        const unsigned char* p = dsdt + i;
        if (p[0] != '_' || p[1] != 'S' || p[2] != '5' || p[3] != '_') continue;
        if (!(i >= 1 && p[-1] == 0x08) &&
            !(i >= 2 && p[-2] == 0x08 && p[-1] == '\\')) continue;
        p += 4;
        if (*p != 0x12) continue;                    /* PackageOp            */
        p++;
        p += ((*p >> 6) & 3) + 1;                    /* skip PkgLength bytes */
        p++;                                         /* skip NumElements     */
        if (*p == 0x0A) p++;                         /* BytePrefix           */
        s_slp_typa = (unsigned short)(*p & 0x07u);
        p++;
        if (*p == 0x0A) p++;
        s_slp_typb = (unsigned short)(*p & 0x07u);
        return 1;
    }
    return 0;
}

/* ── FADT parse (offsets per the ACPI specification) ────────────────────── */

static int parse_fadt(const unsigned char* fadt, unsigned int len) {
    if (len < 116u) return 0;
    unsigned int dsdt_addr = rd32(fadt + 40);
    s_smi_cmd     = rd32(fadt + 48);
    s_acpi_enable = fadt[52];
    s_pm1a_cnt    = rd32(fadt + 64);
    s_pm1b_cnt    = rd32(fadt + 68);

    /* ACPI 2.0+ reset register: Generic Address Structure at 116, value 128. */
    if (len >= 129u) {
        unsigned char space = fadt[116];
        unsigned int  addr  = rd32(fadt + 120);      /* low half of u64      */
        if (space == 1u && addr) {                   /* System I/O space     */
            s_reset_space = space;
            s_reset_addr  = addr;
            s_reset_val   = fadt[128];
            s_reset_ok    = 1;
        }
    }

    if (!s_pm1a_cnt) return 0;

    const unsigned char* dsdt = table_at(dsdt_addr, 36u);
    if (!dsdt || !sig_match(dsdt, "DSDT")) return 0;
    unsigned int dlen = rd32(dsdt + 4);
    if (dlen < 36u || dlen > 0x200000u) return 0;    /* sanity: ≤ 2 MB       */
    table_at(dsdt_addr, dlen);                       /* map the full body    */
    return parse_s5(dsdt, dlen);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int acpi_init(void) {
    s_rsdp_addr = rsdp_find();
    if (!s_rsdp_addr) return 0;
    const unsigned char* rsdp = (const unsigned char*)s_rsdp_addr;

    s_rsdt_addr = rd32(rsdp + 16);
    const unsigned char* rsdt = table_at(s_rsdt_addr, 36u);
    if (!rsdt || !sig_match(rsdt, "RSDT")) return 0;
    unsigned int rlen = rd32(rsdt + 4);
    if (rlen < 36u || rlen > 0x10000u) return 0;
    rsdt = table_at(s_rsdt_addr, rlen);
    if (!checksum_ok(rsdt, rlen)) return 0;

    int n = (int)((rlen - 36u) / 4u);
    for (int i = 0; i < n; i++) {
        unsigned int taddr = rd32(rsdt + 36 + i * 4);
        const unsigned char* t = table_at(taddr, 36u);
        if (!t) continue;
        if (s_table_cnt < 16) {
            for (int j = 0; j < 4; j++) s_table_sig[s_table_cnt][j] = (char)t[j];
            s_table_sig[s_table_cnt][4] = '\0';
            s_table_cnt++;
        }
        if (sig_match(t, "FACP")) {
            unsigned int tlen = rd32(t + 4);
            if (tlen >= 116u && tlen <= 0x10000u) {
                t = table_at(taddr, tlen);
                if (checksum_ok(t, tlen) && parse_fadt(t, tlen))
                    s_ok = 1;
            }
        }
    }
    if (!s_ok) return 0;

    /* Hand the chipset to ACPI mode if the firmware left it in legacy mode. */
    if (!(port_word_in((unsigned short)s_pm1a_cnt) & SCI_EN)
        && s_smi_cmd && s_acpi_enable) {
        port_byte_out((unsigned short)s_smi_cmd, s_acpi_enable);
        for (volatile int w = 0; w < 3000000; w++) {
            if (port_word_in((unsigned short)s_pm1a_cnt) & SCI_EN) break;
        }
    }
    return 1;
}

int acpi_available(void) { return s_ok; }

int acpi_shutdown(void) {
    if (!s_ok) return -1;
    port_word_out((unsigned short)s_pm1a_cnt,
                  (unsigned short)((s_slp_typa << 10) | SLP_EN));
    if (s_pm1b_cnt)
        port_word_out((unsigned short)s_pm1b_cnt,
                      (unsigned short)((s_slp_typb << 10) | SLP_EN));
    /* Power-off is not instantaneous; give the chipset a moment. */
    for (volatile int w = 0; w < 10000000; w++) { }
    return -1;                                       /* still alive → failed */
}

int acpi_reboot(void) {
    if (!s_reset_ok) return -1;
    port_byte_out((unsigned short)s_reset_addr, s_reset_val);
    for (volatile int w = 0; w < 10000000; w++) { }
    return -1;
}

/* ── Diagnostic report (shell `acpi` command) ───────────────────────────── */

static void print_hex(unsigned int v) {
    char b[11];
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; i++) {
        unsigned int nib = (v >> ((7 - i) * 4)) & 0xFu;
        b[2 + i] = (char)(nib < 10u ? '0' + nib : 'A' + nib - 10u);
    }
    b[10] = '\0';
    term_print(b);
}

void acpi_print_info(void) {
    if (!s_rsdp_addr) {
        term_print("ACPI : aucune table RSDP trouvee (BIOS sans ACPI ?).\n");
        return;
    }
    term_print("ACPI : RSDP @ "); print_hex(s_rsdp_addr);
    term_print("   RSDT @ ");     print_hex(s_rsdt_addr);
    term_print("\nTables (");
    { char n[12]; itoa(s_table_cnt, n); term_print(n); }
    term_print(") :");
    for (int i = 0; i < s_table_cnt; i++) {
        term_print(" ");
        term_print(s_table_sig[i]);
    }
    term_print("\n");
    if (!s_ok) {
        term_print("FADT/_S5 introuvable : arret ACPI indisponible (fallback QEMU).\n");
        return;
    }
    term_print("PM1a_CNT="); print_hex(s_pm1a_cnt);
    if (s_pm1b_cnt) { term_print("  PM1b_CNT="); print_hex(s_pm1b_cnt); }
    term_print("  SLP_TYPa=");
    { char n[12]; itoa((int)s_slp_typa, n); term_print(n); }
    term_print("  SLP_TYPb=");
    { char n[12]; itoa((int)s_slp_typb, n); term_print(n); }
    term_print("\nSCI_EN=");
    term_print((port_word_in((unsigned short)s_pm1a_cnt) & SCI_EN) ? "1" : "0");
    if (s_reset_ok) {
        term_print("  ResetReg(I/O)="); print_hex(s_reset_addr);
        term_print(" val=");
        { char n[12]; itoa((int)s_reset_val, n); term_print(n); }
    } else {
        term_print("  ResetReg: absent (reboot via PS/2)");
    }
    term_print("\nArret S5 : PRET (commande shutdown).\n");
}
