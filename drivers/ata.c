/* ── ATA/IDE PIO driver (LBA28, primary bus master) ───────────────────────
   Freestanding C. Ports:
     0x1F0  data (16-bit)
     0x1F1  error / features
     0x1F2  sector count
     0x1F3  LBA low
     0x1F4  LBA mid
     0x1F5  LBA high
     0x1F6  drive / head (0xE0 = LBA, master)
     0x1F7  status / command
     0x3F6  device control (alt status)                                     */

#include "../include/ata.h"
#include "../include/ports.h"

#define ATA_DATA    0x1F0
#define ATA_ERR     0x1F1
#define ATA_SECCNT  0x1F2
#define ATA_LBA0    0x1F3
#define ATA_LBA1    0x1F4
#define ATA_LBA2    0x1F5
#define ATA_DRIVE   0x1F6
#define ATA_STATUS  0x1F7
#define ATA_CMD     0x1F7
#define ATA_CTRL    0x3F6

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DRQ  0x08
#define ST_ERR  0x01

#define CMD_READ     0x20
#define CMD_WRITE    0x30
#define CMD_FLUSH    0xE7
#define CMD_IDENTIFY 0xEC

static int   s_present = -1;      /* -1 = not probed yet */
static unsigned int s_sectors = 0;

/* 400 ns settle: read alt-status 4× (each read ~100 ns on real HW). */
static void ata_delay400(void) {
    for (int i = 0; i < 4; i++) (void)port_byte_in(ATA_CTRL);
}

/* Poll until BSY clears (or timeout). Returns status, or 0xFF on timeout. */
static unsigned char ata_wait_ready(void) {
    for (int i = 0; i < 100000; i++) {
        unsigned char st = port_byte_in(ATA_STATUS);
        if (!(st & ST_BSY)) return st;
    }
    return 0xFF;
}

/* Poll until DRQ set and BSY clear. Returns 1 on ready, 0 on error/timeout. */
static int ata_wait_drq(void) {
    for (int i = 0; i < 100000; i++) {
        unsigned char st = port_byte_in(ATA_STATUS);
        if (st == 0xFF) return 0;              /* floating bus: no drive     */
        if (st & ST_ERR) return 0;
        if (!(st & ST_BSY) && (st & ST_DRQ)) return 1;
    }
    return 0;
}

int ata_present(void) {
    if (s_present >= 0) return s_present;

    /* Select master, issue IDENTIFY. */
    port_byte_out(ATA_DRIVE, 0xA0);
    ata_delay400();
    unsigned char st = port_byte_in(ATA_STATUS);
    if (st == 0xFF) { s_present = 0; return 0; }   /* no drive on the bus    */

    port_byte_out(ATA_SECCNT, 0);
    port_byte_out(ATA_LBA0, 0);
    port_byte_out(ATA_LBA1, 0);
    port_byte_out(ATA_LBA2, 0);
    port_byte_out(ATA_CMD, CMD_IDENTIFY);

    st = port_byte_in(ATA_STATUS);
    if (st == 0) { s_present = 0; return 0; }      /* drive does not exist    */

    if (ata_wait_ready() == 0xFF) { s_present = 0; return 0; }
    /* If LBA1/LBA2 nonzero after IDENTIFY, it's an ATAPI/SATA device, not ATA. */
    if (port_byte_in(ATA_LBA1) != 0 || port_byte_in(ATA_LBA2) != 0) {
        s_present = 0; return 0;
    }
    if (!ata_wait_drq()) { s_present = 0; return 0; }

    /* Read the 256-word IDENTIFY block; words 60-61 = LBA28 sector count. */
    unsigned short id[256];
    for (int i = 0; i < 256; i++) id[i] = port_word_in(ATA_DATA);
    s_sectors = (unsigned int)id[60] | ((unsigned int)id[61] << 16);

    s_present = 1;
    return 1;
}

unsigned int ata_sector_count(void) {
    if (s_present < 0) ata_present();
    return s_sectors;
}

int ata_read_sectors(unsigned int lba, unsigned char count, void* buf) {
    if (!ata_present() || count == 0) return 0;
    unsigned short* out = (unsigned short*)buf;

    if (ata_wait_ready() == 0xFF) return 0;
    port_byte_out(ATA_DRIVE, (unsigned char)(0xE0 | ((lba >> 24) & 0x0F)));
    port_byte_out(ATA_SECCNT, count);
    port_byte_out(ATA_LBA0, (unsigned char)(lba & 0xFF));
    port_byte_out(ATA_LBA1, (unsigned char)((lba >> 8) & 0xFF));
    port_byte_out(ATA_LBA2, (unsigned char)((lba >> 16) & 0xFF));
    port_byte_out(ATA_CMD, CMD_READ);

    for (int s = 0; s < count; s++) {
        if (!ata_wait_drq()) return 0;
        for (int i = 0; i < 256; i++) *out++ = port_word_in(ATA_DATA);
        ata_delay400();
    }
    return 1;
}

int ata_write_sectors(unsigned int lba, unsigned char count, const void* buf) {
    if (!ata_present() || count == 0) return 0;
    const unsigned short* in = (const unsigned short*)buf;

    if (ata_wait_ready() == 0xFF) return 0;
    port_byte_out(ATA_DRIVE, (unsigned char)(0xE0 | ((lba >> 24) & 0x0F)));
    port_byte_out(ATA_SECCNT, count);
    port_byte_out(ATA_LBA0, (unsigned char)(lba & 0xFF));
    port_byte_out(ATA_LBA1, (unsigned char)((lba >> 8) & 0xFF));
    port_byte_out(ATA_LBA2, (unsigned char)((lba >> 16) & 0xFF));
    port_byte_out(ATA_CMD, CMD_WRITE);

    for (int s = 0; s < count; s++) {
        if (!ata_wait_drq()) return 0;
        for (int i = 0; i < 256; i++) port_word_out(ATA_DATA, *in++);
        ata_delay400();
    }
    /* Flush the write cache to the backing store. */
    port_byte_out(ATA_CMD, CMD_FLUSH);
    ata_wait_ready();
    return 1;
}
