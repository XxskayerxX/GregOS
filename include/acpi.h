#ifndef ACPI_H
#define ACPI_H

/* ── ACPI driver (drivers/acpi.c) ───────────────────────────────────────────
   Boot-time table discovery + real S5 shutdown and reset-register reboot,
   replacing the QEMU-hardwired ports 0x604/0xB004 (roadmap L11).

   acpi_init() walks RSDP → RSDT → FADT → DSDT once at boot (identity-mapping
   high table regions via paging_map_4mb) and CACHES everything needed, so
   acpi_shutdown()/acpi_reboot() are pure port I/O afterwards — safe to call
   from any context, under any CR3.                                          */

#ifdef __cplusplus
extern "C" {
#endif

/* Parse the tables; call once at boot, after paging_install()/idt_install().
   Returns 1 if a usable FADT was found (S5 shutdown available), 0 otherwise. */
int acpi_init(void);

/* 1 if acpi_init() succeeded and S5 shutdown is available. */
int acpi_available(void);

/* Enter S5 (power off). On success this never returns. Returns -1 if ACPI is
   unavailable or the write had no effect (caller should fall back).         */
int acpi_shutdown(void);

/* Reboot via the FADT reset register (ACPI 2.0+). Returns -1 if unavailable
   or ineffective (caller should fall back to the PS/2 pulse).               */
int acpi_reboot(void);

/* Print the discovery report (tables found, ports, S5 values) to the tty. */
void acpi_print_info(void);

#ifdef __cplusplus
}
#endif

#endif /* ACPI_H */
