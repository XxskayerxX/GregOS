#ifndef ATA_H
#define ATA_H

/* ── Minimal ATA/IDE PIO driver (LBA28, primary bus, master drive) ────────
   Targets the primary IDE channel (I/O base 0x1F0), master drive — this is
   where QEMU attaches `-drive ...,if=ide,index=0`. CD-ROM boot media sits on
   a different channel and is untouched. All transfers are 512-byte sectors
   via 16-bit PIO. Bounded busy-waits so a missing disk can't hang the boot. */

#ifdef __cplusplus
extern "C" {
#endif

/* Probe the primary master. Returns 1 if a hard disk answered IDENTIFY. */
int ata_present(void);

/* Total addressable sectors reported by IDENTIFY (0 if absent/unknown). */
unsigned int ata_sector_count(void);

/* Read/write `count` sectors starting at LBA into/from buf (count*512 bytes).
   Returns 1 on success, 0 on error/timeout. count must be 1..255.          */
int ata_read_sectors (unsigned int lba, unsigned char count, void* buf);
int ata_write_sectors(unsigned int lba, unsigned char count, const void* buf);

#ifdef __cplusplus
}
#endif

#endif /* ATA_H */
