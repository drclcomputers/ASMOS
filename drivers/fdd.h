#ifndef FDD_H
#define FDD_H

#include "lib/core.h"

/* Floppy controller I/O ports */
#define FDD_DOR 0x3F2  /* Digital Output Register        */
#define FDD_MSR 0x3F4  /* Main Status Register (read)    */
#define FDD_DATA 0x3F5 /* Data FIFO                      */
#define FDD_DIR 0x3F7  /* Digital Input Register (read)  */
#define FDD_CCR 0x3F7  /* Config Control Register (write)*/

/* DOR bits */
#define FDD_DOR_RESET 0x04
#define FDD_DOR_IRQ_DMA 0x08
#define FDD_DOR_MOTOR_A 0x10
#define FDD_DOR_MOTOR_B 0x20
#define FDD_DOR_SEL_A 0x00
#define FDD_DOR_SEL_B 0x01

/* MSR bits */
#define FDD_MSR_RQM 0x80 /* Ready for data transfer */
#define FDD_MSR_DIO 0x40 /* Direction: 1=controller->CPU */
#define FDD_MSR_BUSY 0x10

/* FDC commands */
#define FDD_CMD_READ 0xE6    /* Read sector (with MT/MFM/SK flags) */
#define FDD_CMD_WRITE 0xC5   /* Write sector */
#define FDD_CMD_RECAL 0x07   /* Recalibrate */
#define FDD_CMD_SENSE 0x08   /* Sense interrupt */
#define FDD_CMD_SEEK 0x0F    /* Seek */
#define FDD_CMD_SPECIFY 0x03 /* Specify drive timings */
#define FDD_CMD_VERSION 0x10 /* Get FDC version */

#define FDD_CMD_FORMAT 0x4D /* Format track (MFM) */

#define FDD_SECTOR_SIZE 512

/* Disk formats. A drive reads its own kind and the lower ones of its
 * size: a 1.44 MB drive also reads 720K; a 1.2 MB drive reads 1.2M. */
enum {
    FDD_MEDIA_NONE = 0,
    FDD_MEDIA_160K,  /* 5.25" SS/DD: 40 tracks, 1 side, 8 sectors (DOS 1.0) */
    FDD_MEDIA_180K,  /* 5.25" SS/DD: 40 tracks, 1 side, 9 sectors (DOS 2.0) */
    FDD_MEDIA_320K,  /* 5.25" DS/DD: 40 tracks, 8 sectors (DOS 1.1) */
    FDD_MEDIA_360K,  /* 5.25" DS/DD: 40 tracks, 9 sectors (DOS 2.0) */
    FDD_MEDIA_1200K, /* 5.25" HD: 80 tracks, 15 sectors, 500 kbps */
    FDD_MEDIA_720K,  /* 3.5" DD: 80 tracks, 9 sectors, 250 kbps */
    FDD_MEDIA_1440K, /* 3.5" HD: 80 tracks, 18 sectors, 500 kbps */
    FDD_MEDIA_2880K, /* 3.5" ED: 80 tracks, 36 sectors, 1 Mbps */
    FDD_MEDIA_COUNT
};
/* The 40-track disks (160K-360K) work in a 360K drive at 250 kbps, and in
 * a 1.2 MB drive too: its head then steps twice a track, at 300 kbps
 * (the drive spins faster). Disks from DOS 1.x have no BPB: their size
 * comes from the media byte at the start of the FAT. */

typedef struct {
    const char *name; /* "1.44 MB" */
    uint16_t kb;      /* 1440 */
    uint8_t spt, heads, tracks;
    uint8_t rate;               /* CCR: 0 500k, 1 300k, 2 250k, 3 1M */
    uint8_t gap_rw, gap_format; /* GPL for read/write, for format */
    /* the FAT12 a DOS FORMAT puts on it */
    uint8_t media_byte, sec_per_clus, fat_sectors;
    uint16_t root_entries;
} fdd_format_t;

extern const fdd_format_t FDD_FORMATS[FDD_MEDIA_COUNT];

#define DMA_FLIP_FLOP 0x0C
#define DMA2_ADDR 0x04
#define DMA2_COUNT 0x05
#define DMA2_PAGE 0x81
#define DMA_MASK 0x0A
#define DMA_MODE 0x0B
#define DMA_UNMASK 0x0A

bool fdd_init(uint8_t drive_id);

/* CMOS drive type: FDD_MEDIA_* of the drive itself (its largest format) */
int fdd_drive_kind(uint8_t drive_id);
/* formats this drive reads and formats: out[] gets FDD_MEDIA_* */
#define FDD_MAX_FORMATS 6
int fdd_drive_formats(uint8_t drive_id, int out[FDD_MAX_FORMATS]);
/* the disk's format: found on the first read after an insert (from the
 * boot sector, trying each data rate the drive has). NONE = no disk / not
 * yet known. */
int fdd_media(uint8_t drive_id);
int fdd_detect(uint8_t drive_id); /* look now; returns FDD_MEDIA_* */
/* the 40-track size from a FAT media byte (FE, FC, FF, FD), else NONE */
int fdd_media_from_byte(uint8_t media_byte);
void fdd_forget_media(uint8_t drive_id);         /* the disk may have changed */
void fdd_set_media(uint8_t drive_id, int media); /* for formatting */
uint32_t fdd_total_sectors(uint8_t drive_id);
/* low-level format one track (current media), sectors 1..spt */
bool fdd_format_track(uint8_t drive_id, uint8_t cyl, uint8_t head);
bool fdd_read_sector(uint8_t drive_id, uint32_t lba, void *buf);
bool fdd_write_sector(uint8_t drive_id, uint32_t lba, const void *buf);
bool fdd_is_present(uint8_t drive_id);
void fdd_motor_off(uint8_t drive_id);
/* call often (main loop): motors stop 2 s after the last access */
void fdd_poll(void);

#endif
