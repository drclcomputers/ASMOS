/* Floppy formatting (fs/fdformat.h). */
#include "fs/fdformat.h"
#include "drivers/fdd.h"
#include "fs/fdd_ctrl.h"
#include "fs/fs.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"

/* boot code at 0x3E: "Not a system disk", wait for a key, INT 19h
 * (assembled from a few lines of NASM; the message is in it) */
static const uint8_t BOOT_CODE[] = {
    0xFA, 0x31, 0xC0, 0x8E, 0xD8, 0x8E, 0xD0, 0xBC, 0x00, 0x7C, 0xFB, 0xBE,
    0x60, 0x7C, 0xAC, 0x08, 0xC0, 0x74, 0x09, 0xB4, 0x0E, 0xBB, 0x07, 0x00,
    0xCD, 0x10, 0xEB, 0xF2, 0x30, 0xE4, 0xCD, 0x16, 0xCD, 0x19, 0x0D, 0x0A,
    'N',  'o',  't',  ' ',  'a',  ' ',  's',  'y',  's',  't',  'e',  'm',
    ' ',  'd',  'i',  's',  'k',  ' ',  '(',  'f',  'o',  'r',  'm',  'a',
    't',  't',  'e',  'd',  ' ',  'b',  'y',  ' ',  'A',  'S',  'M',  'O',
    'S',  ')',  '.',  0x0D, 0x0A, 'T',  'a',  'k',  'e',  ' ',  'i',  't',
    ' ',  'o',  'u',  't',  ' ',  'a',  'n',  'd',  ' ',  'p',  'r',  'e',
    's',  's',  ' ',  'a',  ' ',  'k',  'e',  'y',  '.',  0x0D, 0x0A, 0x00};

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

/* 11 characters, upper case, space padded; "NO NAME" if none */
static void label11(char out[11], const char *label) {
    int n = 0;
    if (label)
        for (; label[n] && n < 11; n++) {
            char c = label[n];
            out[n] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        }
    if (!n) {
        memcpy(out, "NO NAME    ", 11);
        return;
    }
    for (; n < 11; n++)
        out[n] = ' ';
}

bool fdformat_write_fat(int media, const char *label, uint32_t serial,
                        fdformat_write_fn wr, void *u) {
    if (media <= FDD_MEDIA_NONE || media >= FDD_MEDIA_COUNT)
        return false;
    const fdd_format_t *f = &FDD_FORMATS[media];
    uint16_t total = (uint16_t)(f->spt * f->heads * f->tracks);
    static uint8_t sec[512];
    char lab[11];
    label11(lab, label);

    /* boot sector with the DOS 4+ BPB */
    memset(sec, 0, sizeof(sec));
    sec[0] = 0xEB;
    sec[1] = 0x3C;
    sec[2] = 0x90;
    memcpy(sec + 3, "MSDOS5.0", 8); /* what DOS and Windows expect most */
    put16(sec + 11, 512);
    sec[13] = f->sec_per_clus;
    put16(sec + 14, 1); /* reserved: the boot sector */
    sec[16] = 2;        /* FATs */
    put16(sec + 17, f->root_entries);
    put16(sec + 19, total);
    sec[21] = f->media_byte;
    put16(sec + 22, f->fat_sectors);
    put16(sec + 24, f->spt);
    put16(sec + 26, f->heads);
    put32(sec + 28, 0); /* hidden sectors */
    put32(sec + 32, 0); /* large total: unused */
    sec[36] = 0x00;     /* drive number A: */
    sec[38] = 0x29;     /* extended boot signature */
    put32(sec + 39, serial);
    memcpy(sec + 43, lab, 11);
    memcpy(sec + 54, "FAT12   ", 8);
    memcpy(sec + 0x3E, BOOT_CODE, sizeof(BOOT_CODE));
    sec[510] = 0x55;
    sec[511] = 0xAA;
    if (!wr(0, sec, u))
        return false;

    /* two FATs: entry 0 = media byte, entry 1 = end of chain */
    uint32_t lba = 1;
    for (int fat = 0; fat < 2; fat++)
        for (int i = 0; i < f->fat_sectors; i++) {
            memset(sec, 0, sizeof(sec));
            if (i == 0) {
                sec[0] = f->media_byte;
                sec[1] = 0xFF;
                sec[2] = 0xFF;
            }
            if (!wr(lba++, sec, u))
                return false;
        }

    /* root directory: the volume label, then empty */
    int root_secs = f->root_entries * 32 / 512;
    for (int i = 0; i < root_secs; i++) {
        memset(sec, 0, sizeof(sec));
        if (i == 0 && label && label[0]) {
            memcpy(sec, lab, 11);
            sec[11] = 0x08; /* volume label */
            uint16_t dt = date_from_rtc(), tm = time_from_rtc();
            put16(sec + 22, tm);
            put16(sec + 24, dt);
        }
        if (!wr(lba++, sec, u))
            return false;
    }
    return true;
}

/* ── the real drive ───────────────────────────────────────────────── */

static bool wr_drive(uint32_t lba, const void *sector, void *u) {
    return fdd_write_sector(*(uint8_t *)u, lba, sector);
}

bool fdformat_begin(fdformat_t *f, uint8_t drive_id, int media,
                    const char *label, bool quick) {
    memset(f, 0, sizeof(*f));
    f->drive_id = drive_id;
    f->unit = drive_id == DRIVE_FDD0 ? 0 : 1;
    f->media = media;
    f->quick = quick;
    if (label)
        strncpy(f->label, label, 11);
    if (drive_id != DRIVE_FDD0 && drive_id != DRIVE_FDD1) {
        f->err = "Not a floppy drive.";
        return false;
    }
    if (!fdd_is_present(f->unit) && !fdd_init(f->unit)) {
        f->err = "There is no such floppy drive.";
        return false;
    }
    int ok[FDD_MAX_FORMATS], n = fdd_drive_formats(f->unit, ok), i;
    for (i = 0; i < n && ok[i] != media; i++) {
    }
    if (i == n) {
        f->err = "This drive cannot format that size.";
        return false;
    }
    if (fs_drive_mounted(drive_id))
        fdd_eject(drive_id); /* the old file system is about to go */
    fdd_set_media(f->unit, media);
    const fdd_format_t *fm = &FDD_FORMATS[media];
    f->tracks = quick ? 0 : fm->tracks * fm->heads;
    return true;
}

int fdformat_step(fdformat_t *f) {
    if (f->err)
        return -1;
    if (f->done)
        return 100;
    const fdd_format_t *fm = &FDD_FORMATS[f->media];
    if (f->track < f->tracks) {
        int cyl = f->track / fm->heads, head = f->track % fm->heads;
        if (!fdd_format_track(f->unit, (uint8_t)cyl, (uint8_t)head)) {
            f->err = f->track == 0 ? "Could not format: no disk, or it is "
                                     "write-protected."
                                   : "A track could not be formatted: the "
                                     "disk may be damaged.";
            fdd_motor_off(f->unit);
            return -1;
        }
        f->track++;
        return f->track * 95 / f->tracks;
    }
    /* the file system, then use the disk */
    uint32_t serial = pit_ticks * 2654435761u ^
                      (uint32_t)date_from_rtc() << 16 ^ time_from_rtc();
    if (!fdformat_write_fat(f->media, f->label, serial, wr_drive, &f->unit) ||
        !fdd_flush(f->unit)) { /* (the writes wait in the track cache) */
        f->err = f->quick ? "Could not write to the disk (no disk, not "
                            "formatted, or write-protected)."
                          : "Could not write the file system.";
        fdd_motor_off(f->unit);
        return -1;
    }
    fdd_motor_off(f->unit);
    fdd_forget_media(f->unit);
    fdd_insert(f->drive_id);
    f->done = true;
    return 100;
}
