#include "fs/ata.h"
#include "lib/core.h"
#include "lib/memory.h"

static bool ata_wait_drq(void) {
    uint32_t timeout = 0x100000;
    while (--timeout) {
        uint8_t status = inb(ATA_PRIMARY_BASE + ATA_REG_STATUS);
        if (status & ATA_SR_ERR)  return false;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) return true;
    }
    return false;
}

static bool ata_wait_ready(void) {
    uint32_t timeout = 0x100000;
    while (--timeout) {
        uint8_t status = inb(ATA_PRIMARY_BASE + ATA_REG_STATUS);
        if (status & ATA_SR_ERR)  return false;
        if (!(status & ATA_SR_BSY)) return true;
    }
    return false;
}

static void ata_select_lba28(uint32_t lba, uint8_t sector_count) {
    outb(ATA_PRIMARY_BASE + ATA_REG_DRIVE,
         0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_PRIMARY_BASE + ATA_REG_SECCOUNT, sector_count);
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA_LO,  (lba)       & 0xFF);
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA_MID, (lba >> 8)  & 0xFF);
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA_HI,  (lba >> 16) & 0xFF);
}

bool ata_read_sector(uint32_t lba, void *buf) {
    if (!ata_wait_ready()) return false;

    ata_select_lba28(lba, 1);
    outb(ATA_PRIMARY_BASE + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    if (!ata_wait_drq()) return false;

    uint16_t *p = (uint16_t *)buf;
    for (int i = 0; i < 256; i++)
        p[i] = inw(ATA_PRIMARY_BASE + ATA_REG_DATA);

    return true;
}

bool ata_write_sector(uint32_t lba, const void *buf) {
    if (!ata_wait_ready()) return false;

    ata_select_lba28(lba, 1);
    outb(ATA_PRIMARY_BASE + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    if (!ata_wait_drq()) return false;

    const uint16_t *p = (const uint16_t *)buf;
    for (int i = 0; i < 256; i++)
        outw(ATA_PRIMARY_BASE + ATA_REG_DATA, p[i]);

    outb(ATA_PRIMARY_BASE + ATA_REG_COMMAND, ATA_CMD_FLUSH);
    ata_wait_ready();

    return true;
}

bool ata_read_sectors(uint32_t lba, uint8_t count, void *buf) {
    uint8_t *p = (uint8_t *)buf;
    for (uint8_t i = 0; i < count; i++) {
        if (!ata_read_sector(lba + i, p)) return false;
        p += 512;
    }
    return true;
}

bool ata_write_sectors(uint32_t lba, uint8_t count, const void *buf) {
    const uint8_t *p = (const uint8_t *)buf;
    for (uint8_t i = 0; i < count; i++) {
        if (!ata_write_sector(lba + i, p)) return false;
        p += 512;
    }
    return true;
}
