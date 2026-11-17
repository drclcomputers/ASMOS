#ifndef ATA_H
#define ATA_H

#include "lib/core.h"

/*
 * IDE/ATA disks, PIO, polled.
 *
 * ata_init() finds the controllers (PCI IDE in native mode, else the
 * legacy 0x1F0/0x170 ports), IDENTIFYs all four positions and picks:
 *   unit 0 (HDA) = the disk we booted from (ASMOS boot sector; BIOS drive
 *                  number breaks ties),
 *   unit 1 (HDB) = the next hard disk found.
 * Drives without LBA (most pre-1994 disks) are driven in CHS mode using the
 * geometry they report.
 */

#define ATA_REG_DATA 0x00
#define ATA_REG_ERROR 0x01
#define ATA_REG_FEATURES 0x01
#define ATA_REG_SECCOUNT 0x02
#define ATA_REG_LBA_LO 0x03  /* sector number in CHS mode   */
#define ATA_REG_LBA_MID 0x04 /* cylinder low               */
#define ATA_REG_LBA_HI 0x05  /* cylinder high              */
#define ATA_REG_DRIVE 0x06
#define ATA_REG_STATUS 0x07
#define ATA_REG_COMMAND 0x07

#define ATA_SR_BSY 0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF 0x20
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01

#define ATA_CMD_READ_PIO 0x20
#define ATA_CMD_WRITE_PIO 0x30
#define ATA_CMD_FLUSH 0xE7
#define ATA_CMD_IDENTIFY 0xEC

#define ATA_MAX_DISKS 4

typedef struct {
    bool present;
    uint8_t channel; /* 0 primary, 1 secondary */
    uint8_t slave;   /* 0 master, 1 slave */
    bool lba;        /* false: CHS addressing */
    uint16_t cyls, heads, spt;
    uint32_t sectors; /* capacity */
    char model[41];
} ata_disk_t;

/* Probe controllers and disks; bios_boot_drive = DL at boot (0x80...).
 * Returns the number of hard disks found. */
int ata_init(uint8_t bios_boot_drive);
/* Disk behind logical unit 0 (HDA) / 1 (HDB), or NULL. */
const ata_disk_t *ata_unit(uint8_t unit);
/* Where the controllers are (for boot messages). */
const char *ata_controller_desc(void);

bool ata_read_sector(uint8_t unit, uint32_t lba, void *buf);
bool ata_write_sector(uint8_t unit, uint32_t lba, const void *buf);
bool ata_read_sectors(uint8_t unit, uint32_t lba, uint8_t count, void *buf);
bool ata_write_sectors(uint8_t unit, uint32_t lba, uint8_t count,
                       const void *buf);
/* Shut down / restart: every disk writes out its cache and, with
 * spin_down, stops (STANDBY IMMEDIATE: heads parked). Returns when they
 * are done (or after a few seconds a drive does not answer). */
void ata_park(bool spin_down);

#endif
