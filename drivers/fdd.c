#include "drivers/fdd.h"
#include "lib/core.h"
#include "lib/memory.h"
#include "lib/time.h"

#define s_dma_buf ((uint8_t *)0x7000)
#define s_present ((bool *)0x7200)
#define s_motor_on ((bool *)0x7204)
#define s_current_track ((int *)0x7208)

/* name, KB, sectors/track, heads, tracks, rate, gap r/w, gap format,
 * then the FAT12 DOS puts on it: media byte, sectors/cluster, FAT
 * sectors, root entries (the IBM/DOS standard values) */
const fdd_format_t FDD_FORMATS[FDD_MEDIA_COUNT] = {
    [FDD_MEDIA_NONE] = {"none", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    [FDD_MEDIA_160K] = {"160 KB", 160, 8, 1, 40, 2, 0x2A, 0x50, 0xFE, 1, 1, 64},
    [FDD_MEDIA_180K] = {"180 KB", 180, 9, 1, 40, 2, 0x2A, 0x50, 0xFC, 1, 2, 64},
    [FDD_MEDIA_320K] = {"320 KB", 320, 8, 2, 40, 2, 0x2A, 0x50, 0xFF, 2, 1,
                        112},
    [FDD_MEDIA_360K] = {"360 KB", 360, 9, 2, 40, 2, 0x2A, 0x50, 0xFD, 2, 2,
                        112},
    [FDD_MEDIA_1200K] = {"1.2 MB", 1200, 15, 2, 80, 0, 0x1B, 0x54, 0xF9, 1, 7,
                         224},
    [FDD_MEDIA_720K] = {"720 KB", 720, 9, 2, 80, 2, 0x2A, 0x50, 0xF9, 2, 3,
                        112},
    [FDD_MEDIA_1440K] = {"1.44 MB", 1440, 18, 2, 80, 0, 0x1B, 0x6C, 0xF0, 1, 9,
                         224},
    [FDD_MEDIA_2880K] = {"2.88 MB", 2880, 36, 2, 80, 3, 0x1B, 0x53, 0xF0, 2, 9,
                         240},
};

/* the disk in each drive: its format and geometry (spt/heads from the
 * boot sector, which may differ from the table for odd disks); dstep: a
 * 40-track disk in an 80-track drive (two steps a track, 300 kbps) */
static int s_media[2];
static uint8_t s_spt[2], s_heads[2], s_tracks[2], s_dstep[2];

/* Timeouts: pit_ticks counts milliseconds. Each loop also gives up after
 * a number of ISA reads (~1 us each), so they still end if the timer is
 * not running (interrupts off). */
typedef struct {
    uint32_t end, left;
} fdd_deadline_t;

static fdd_deadline_t fdd_deadline(uint32_t ms) {
    return (fdd_deadline_t){pit_ticks + ms, ms * 3000u + 1000u};
}

static bool fdd_expired(fdd_deadline_t *d) {
    return !d->left-- || (int32_t)(pit_ticks - d->end) >= 0;
}

static void fdd_wait_ms(uint32_t ms) {
    fdd_deadline_t d = fdd_deadline(ms);
    while (!fdd_expired(&d))
        (void)inb(FDD_MSR); /* ~1 us ISA read */
}

static bool fdd_cmos_present(uint8_t drive_id) {
    outb(0x70, 0x10 | 0x80);
    uint8_t drives = inb(0x71);
    outb(0x70, 0x80);
    uint8_t type = (drive_id == 0) ? (drives >> 4) : (drives & 0x0F);
    return type != 0;
}

static uint8_t fdd_cmos_type(uint8_t drive_id) {
    outb(0x70, 0x10 | 0x80);
    uint8_t drives = inb(0x71);
    outb(0x70, 0x80);
    return (drive_id == 0) ? (drives >> 4) : (drives & 0x0F);
}

int fdd_drive_kind(uint8_t drive_id) {
    switch (fdd_cmos_type(drive_id)) {
    case 1:
        return FDD_MEDIA_360K;
    case 2:
        return FDD_MEDIA_1200K;
    case 3:
        return FDD_MEDIA_720K;
    case 4:
        return FDD_MEDIA_1440K;
    case 5:
    case 6:
        return FDD_MEDIA_2880K;
    default:
        return FDD_MEDIA_NONE;
    }
}

/* what the drive reads and formats, its own format first */
int fdd_drive_formats(uint8_t drive_id, int out[FDD_MAX_FORMATS]) {
    int n = 0;
    switch (fdd_drive_kind(drive_id)) {
    case FDD_MEDIA_2880K:
        out[n++] = FDD_MEDIA_2880K; /* fall through */
    case FDD_MEDIA_1440K:
        out[n++] = FDD_MEDIA_1440K; /* fall through */
    case FDD_MEDIA_720K:
        out[n++] = FDD_MEDIA_720K;
        break;
    case FDD_MEDIA_1200K: /* and the 40-track ones, double-stepped */
        out[n++] = FDD_MEDIA_1200K;
        /* fall through */
    case FDD_MEDIA_360K:
        out[n++] = FDD_MEDIA_360K;
        out[n++] = FDD_MEDIA_320K;
        out[n++] = FDD_MEDIA_180K;
        out[n++] = FDD_MEDIA_160K;
        break;
    }
    return n;
}

static void use_media(uint8_t d, int media, int spt, int heads) {
    const fdd_format_t *f = &FDD_FORMATS[media];
    s_media[d] = media;
    s_spt[d] = (uint8_t)(spt ? spt : f->spt);
    s_heads[d] = (uint8_t)(heads ? heads : f->heads);
    s_tracks[d] = f->tracks;
    s_dstep[d] = f->tracks == 40 && fdd_drive_kind(d) == FDD_MEDIA_1200K;
}

int fdd_media(uint8_t drive_id) { return drive_id < 2 ? s_media[drive_id] : 0; }

void fdd_forget_media(uint8_t drive_id) {
    if (drive_id < 2)
        s_media[drive_id] = FDD_MEDIA_NONE;
}

void fdd_set_media(uint8_t drive_id, int media) {
    if (drive_id < 2 && media > 0 && media < FDD_MEDIA_COUNT) {
        use_media(drive_id, media, 0, 0);
        s_current_track[drive_id] = -1; /* new rate: recalibrate */
    }
}

uint32_t fdd_total_sectors(uint8_t d) {
    return d < 2 ? (uint32_t)s_spt[d] * s_heads[d] * s_tracks[d] : 0;
}

static bool fdc_wait_rqm(void) {
    fdd_deadline_t d = fdd_deadline(500);
    while (!fdd_expired(&d))
        if (inb(FDD_MSR) & FDD_MSR_RQM)
            return true;
    return false;
}

static bool fdc_send_byte(uint8_t b) {
    if (!fdc_wait_rqm())
        return false;
    if (inb(FDD_MSR) & FDD_MSR_DIO)
        return false;
    outb(FDD_DATA, b);
    return true;
}

static int fdc_recv_byte(void) {
    if (!fdc_wait_rqm())
        return -1;
    if (!(inb(FDD_MSR) & FDD_MSR_DIO))
        return -1;
    return inb(FDD_DATA);
}

/* SENSE INTERRUPT STATUS. With nothing pending the controller answers a
 * single byte, 0x80 ("invalid command"), and no cylinder byte. */
static bool fdc_sense(uint8_t *st0, uint8_t *cyl) {
    if (!fdc_send_byte(FDD_CMD_SENSE))
        return false;
    int a = fdc_recv_byte();
    if (a < 0 || a == 0x80)
        return false;
    int c = fdc_recv_byte();
    *st0 = (uint8_t)a;
    *cyl = (uint8_t)(c < 0 ? 0 : c);
    return true;
}

/* Reset the controller. It then raises one interrupt per drive (ST0 =
 * 0xC0..0xC3) that must be sensed away, or the next seek reads those. */
static void fdc_reset(void) {
    outb(FDD_DOR, 0x00);
    fdd_wait_ms(2);
    outb(FDD_DOR, FDD_DOR_RESET | FDD_DOR_IRQ_DMA);
    fdd_wait_ms(10);
    uint8_t st0, cyl;
    for (int i = 0; i < 4; i++)
        fdc_sense(&st0, &cyl);
    outb(FDD_CCR, 0x00); /* 500 kbps until a disk says otherwise */
    /* SRT=8 ms, HUT=240 ms, HLT=2 ms, DMA mode */
    fdc_send_byte(FDD_CMD_SPECIFY);
    fdc_send_byte(0x8F);
    fdc_send_byte(0x02);
    for (int d = 0; d < 2; d++) {
        s_motor_on[d] = false;
        s_current_track[d] = -1;
    }
}

/* A seek or recalibrate only starts the head moving: the command phase
 * ends at once and the controller interrupts when the head arrives. Poll
 * SENSE INTERRUPT until this drive reports "seek end". (QEMU and Bochs
 * finish seeks instantly, so sensing straight away worked there; 86Box
 * and real drives take milliseconds per step.) */
static bool fdc_wait_seek(uint8_t drive_id, uint8_t *st0, uint8_t *cyl) {
    fdd_deadline_t d = fdd_deadline(3000);
    while (!fdd_expired(&d)) {
        if (fdc_sense(st0, cyl) && (*st0 & 3) == (drive_id & 1) &&
            (*st0 & 0x20))
            return true;
        fdd_wait_ms(1);
    }
    return false;
}

/* ── motor control ──────────────────────────────────────────────────────── */
static void fdd_motor_start(uint8_t drive_id) {
    if (s_motor_on[drive_id])
        return;
    uint8_t dor = FDD_DOR_RESET | FDD_DOR_IRQ_DMA;
    dor |= (drive_id == 0) ? (FDD_DOR_SEL_A | FDD_DOR_MOTOR_A)
                           : (FDD_DOR_SEL_B | FDD_DOR_MOTOR_B);
    outb(FDD_DOR, dor);
    s_motor_on[drive_id] = true;
    fdd_wait_ms(500); /* spin-up */
}

void fdd_motor_off(uint8_t drive_id) {
    if (!s_motor_on[drive_id])
        return;
    uint8_t dor = FDD_DOR_RESET | FDD_DOR_IRQ_DMA;
    dor |= (drive_id == 0) ? FDD_DOR_SEL_A : FDD_DOR_SEL_B;
    outb(FDD_DOR, dor);
    s_motor_on[drive_id] = false;
}

/* ── recalibrate (seek to track 0) ─────────────────────────────────────── */
static bool fdd_recalibrate(uint8_t drive_id) {
    fdd_motor_start(drive_id);
    /* one recalibrate steps at most 77 times: an 80-track drive may need
     * two (the first then ends with "equipment check") */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!fdc_send_byte(FDD_CMD_RECAL) || !fdc_send_byte(drive_id & 1))
            continue;
        uint8_t st0, cyl;
        if (fdc_wait_seek(drive_id, &st0, &cyl) && !(st0 & 0xC0) && cyl == 0) {
            s_current_track[drive_id] = 0;
            return true;
        }
    }
    return false;
}

/* ── seek ───────────────────────────────────────────────────────────────── */
static bool fdd_seek(uint8_t drive_id, uint8_t track) {
    if (s_current_track[drive_id] == (int)track)
        return true;
    if (s_current_track[drive_id] < 0 && !fdd_recalibrate(drive_id))
        return false;

    uint8_t phys = (uint8_t)(track << s_dstep[drive_id]); /* double step */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!fdc_send_byte(FDD_CMD_SEEK) || !fdc_send_byte(drive_id & 1) ||
            !fdc_send_byte(phys))
            continue;
        uint8_t st0, cyl;
        if (fdc_wait_seek(drive_id, &st0, &cyl) && !(st0 & 0xC0) &&
            cyl == phys) {
            s_current_track[drive_id] = track;
            fdd_wait_ms(15); /* head settle */
            return true;
        }
    }
    s_current_track[drive_id] = -1;
    return false;
}

/* After a read/write command: wait for the result phase. With no disk in
 * the drive it never comes; the controller is then stuck mid-command and
 * only a reset frees it. */
static bool fdc_wait_result(void) {
    fdd_deadline_t d = fdd_deadline(2000);
    while (!fdd_expired(&d))
        if ((inb(FDD_MSR) & (FDD_MSR_RQM | FDD_MSR_DIO)) ==
            (FDD_MSR_RQM | FDD_MSR_DIO))
            return true;
    return false;
}

/* ── DMA setup ──────────────────────────────────────────────────────────── */
static void dma_setup_read(void *buf, uint16_t count) {
    uint32_t addr = (uint32_t)buf;
    uint8_t page = (addr >> 16) & 0xFF;
    uint16_t offset = (uint16_t)(addr & 0xFFFF);

    /* Mask channel 2 */
    outb(DMA_MASK, 0x06);
    outb(DMA_FLIP_FLOP, 0x00);
    /* Address */
    outb(DMA2_ADDR, offset & 0xFF);
    outb(DMA2_ADDR, (offset >> 8) & 0xFF);
    /* Page */
    outb(DMA2_PAGE, page);
    /* Count (bytes - 1) */
    outb(DMA_FLIP_FLOP, 0x00);
    outb(DMA2_COUNT, (count - 1) & 0xFF);
    outb(DMA2_COUNT, ((count - 1) >> 8) & 0xFF);
    /* Mode: single, increment, no auto, read (FDC writes to memory) */
    outb(DMA_MODE, 0x46);
    /* Unmask channel 2 */
    outb(DMA_UNMASK, 0x02);
}

static void dma_setup_write(const void *buf, uint16_t count) {
    uint32_t addr = (uint32_t)buf;
    uint8_t page = (addr >> 16) & 0xFF;
    uint16_t offset = (uint16_t)(addr & 0xFFFF);

    outb(DMA_MASK, 0x06);
    outb(DMA_FLIP_FLOP, 0x00);
    outb(DMA2_ADDR, offset & 0xFF);
    outb(DMA2_ADDR, (offset >> 8) & 0xFF);
    outb(DMA2_PAGE, page);
    outb(DMA_FLIP_FLOP, 0x00);
    outb(DMA2_COUNT, (count - 1) & 0xFF);
    outb(DMA2_COUNT, ((count - 1) >> 8) & 0xFF);
    /* Mode: single, increment, no auto, write (FDC reads from memory) */
    outb(DMA_MODE, 0x4A);
    outb(DMA_UNMASK, 0x02);
}

/* ── LBA to CHS ─────────────────────────────────────────────────────────── */
static void lba_to_chs(uint8_t d, uint32_t lba, uint8_t *cyl, uint8_t *head,
                       uint8_t *sector) {
    *cyl = (uint8_t)(lba / (s_heads[d] * s_spt[d]));
    *head = (uint8_t)((lba / s_spt[d]) % s_heads[d]);
    *sector = (uint8_t)((lba % s_spt[d]) + 1); /* 1-based */
}

/* the controller's data rate for this disk (shared by both drives); a
 * double-density disk in a 1.2 MB drive spins at 360 rpm: 300 kbps */
static void set_rate(uint8_t d) {
    outb(FDD_CCR, s_dstep[d] ? 1 : FDD_FORMATS[s_media[d]].rate);
}

/* ── read/write sector ──────────────────────────────────────────────────── */
static bool read_raw(uint8_t drive_id, uint32_t lba, void *buf, int tries);

bool fdd_read_sector(uint8_t drive_id, uint32_t lba, void *buf) {
    if (drive_id > 1 || !s_present[drive_id])
        return false;
    if (!s_media[drive_id] && !fdd_detect(drive_id))
        return false;
    if (lba >= fdd_total_sectors(drive_id))
        return false;
    return read_raw(drive_id, lba, buf, 3);
}

static bool read_raw(uint8_t drive_id, uint32_t lba, void *buf, int tries) {
    uint8_t cyl, head, sector;
    lba_to_chs(drive_id, lba, &cyl, &head, &sector);
    const fdd_format_t *fm = &FDD_FORMATS[s_media[drive_id]];
    set_rate(drive_id);

    fdd_motor_start(drive_id);
    if (!fdd_seek(drive_id, cyl)) {
        fdd_motor_off(drive_id);
        return false;
    }

    for (int attempt = 0; attempt < tries; attempt++) {
        dma_setup_read(s_dma_buf, FDD_SECTOR_SIZE);

        fdc_send_byte(FDD_CMD_READ);
        fdc_send_byte((head << 2) | (drive_id & 1));
        fdc_send_byte(cyl);
        fdc_send_byte(head);
        fdc_send_byte(sector);
        fdc_send_byte(2); /* sector size: 512 = 2^(2+7) */
        fdc_send_byte(s_spt[drive_id]);
        fdc_send_byte(fm->gap_rw);
        fdc_send_byte(0xFF);

        if (!fdc_wait_result()) { /* no disk: don't try 3 times */
            fdc_reset();          /* frees the stuck controller */
            return false;         /* motor is off after a reset */
        }

        uint8_t st0 = (uint8_t)fdc_recv_byte();
        uint8_t st1 = (uint8_t)fdc_recv_byte();
        uint8_t st2 = (uint8_t)fdc_recv_byte();
        (void)fdc_recv_byte(); /* C */
        (void)fdc_recv_byte(); /* H */
        (void)fdc_recv_byte(); /* R */
        (void)fdc_recv_byte(); /* N */

        if ((st0 & 0xC0) == 0 && st1 == 0 && st2 == 0) {
            memcpy(buf, s_dma_buf, FDD_SECTOR_SIZE);
            return true;
        }

        fdd_recalibrate(drive_id);
        fdd_seek(drive_id, cyl);
    }

    fdd_motor_off(drive_id);
    return false;
}

bool fdd_write_sector(uint8_t drive_id, uint32_t lba, const void *buf) {
    if (drive_id > 1 || !s_present[drive_id])
        return false;
    if (!s_media[drive_id] && !fdd_detect(drive_id))
        return false;
    if (lba >= fdd_total_sectors(drive_id))
        return false;

    uint8_t cyl, head, sector;
    lba_to_chs(drive_id, lba, &cyl, &head, &sector);
    const fdd_format_t *fm = &FDD_FORMATS[s_media[drive_id]];
    set_rate(drive_id);

    fdd_motor_start(drive_id);
    if (!fdd_seek(drive_id, cyl)) {
        fdd_motor_off(drive_id);
        return false;
    }

    memcpy(s_dma_buf, buf, FDD_SECTOR_SIZE);

    for (int attempt = 0; attempt < 3; attempt++) {
        dma_setup_write(s_dma_buf, FDD_SECTOR_SIZE);

        fdc_send_byte(FDD_CMD_WRITE);
        fdc_send_byte((head << 2) | (drive_id & 1));
        fdc_send_byte(cyl);
        fdc_send_byte(head);
        fdc_send_byte(sector);
        fdc_send_byte(2);
        fdc_send_byte(s_spt[drive_id]);
        fdc_send_byte(fm->gap_rw);
        fdc_send_byte(0xFF);

        if (!fdc_wait_result()) { /* no disk: don't try 3 times */
            fdc_reset();          /* frees the stuck controller */
            return false;         /* motor is off after a reset */
        }

        uint8_t st0 = (uint8_t)fdc_recv_byte();
        uint8_t st1 = (uint8_t)fdc_recv_byte();
        uint8_t st2 = (uint8_t)fdc_recv_byte();
        (void)fdc_recv_byte();
        (void)fdc_recv_byte();
        (void)fdc_recv_byte();
        (void)fdc_recv_byte();

        if ((st0 & 0xC0) == 0 && st1 == 0 && st2 == 0)
            return true;

        fdd_recalibrate(drive_id);
        fdd_seek(drive_id, cyl);
    }

    fdd_motor_off(drive_id);
    return false;
}

/* ── what disk is in the drive ─────────────────────────────────────────
 * Each format the drive can hold is tried at its data rate: reading the
 * boot sector only works at the right one. Its BPB then gives the real
 * sectors per track and heads. */
/* the format with this geometry, among the 40- or 80-track ones */
static int media_for(int spt, int heads, int tracks) {
    for (int m = 1; m < FDD_MEDIA_COUNT; m++)
        if (FDD_FORMATS[m].spt == spt && FDD_FORMATS[m].heads == heads &&
            FDD_FORMATS[m].tracks == tracks)
            return m;
    return FDD_MEDIA_NONE;
}

int fdd_media_from_byte(uint8_t media_byte) {
    switch (media_byte) { /* the DOS 1.x disks: no BPB */
    case 0xFE:
        return FDD_MEDIA_160K;
    case 0xFC:
        return FDD_MEDIA_180K;
    case 0xFF:
        return FDD_MEDIA_320K;
    case 0xFD:
        return FDD_MEDIA_360K;
    default:
        return FDD_MEDIA_NONE;
    }
}

int fdd_detect(uint8_t d) {
    if (d > 1 || !s_present[d])
        return FDD_MEDIA_NONE;
    int cand[FDD_MAX_FORMATS], n = fdd_drive_formats(d, cand);
    static uint8_t sec[FDD_SECTOR_SIZE];
    bool tried40 = false;
    for (int i = 0; i < n; i++) {
        int m = cand[i];
        bool is40 = FDD_FORMATS[m].tracks == 40;
        if (is40 && tried40)
            continue; /* one try covers every 40-track size */
        tried40 |= is40;
        use_media(d, m, 0, 0);       /* the rate (and stepping) of this kind */
        s_current_track[d] = -1;     /* recalibrate at this rate */
        if (!read_raw(d, 0, sec, 1)) /* C0 H0 S1: there on every size */
            continue;
        uint16_t bps = (uint16_t)(sec[11] | sec[12] << 8);
        uint16_t spt = (uint16_t)(sec[24] | sec[25] << 8);
        uint16_t heads = (uint16_t)(sec[26] | sec[27] << 8);
        bool bpb =
            bps == 512 && spt >= 8 && spt <= 36 && heads >= 1 && heads <= 2;
        int found = FDD_MEDIA_NONE;
        if (bpb)
            found = media_for(spt, heads, FDD_FORMATS[m].tracks);
        else if (is40 && read_raw(d, 1, sec, 1)) /* C0 H0 S2: the FAT */
            found = fdd_media_from_byte(sec[0]);
        if (!found)
            found = m;
        use_media(d, found, bpb ? spt : 0, bpb ? heads : 0);
        return found;
    }
    s_media[d] = FDD_MEDIA_NONE;
    fdd_motor_off(d);
    return FDD_MEDIA_NONE;
}

/* FORMAT TRACK: the controller writes the sector headers we hand it by
 * DMA (C, H, R, N for each sector) and fills the data with 0xF6. */
bool fdd_format_track(uint8_t d, uint8_t cyl, uint8_t head) {
    if (d > 1 || !s_present[d] || !s_media[d])
        return false;
    const fdd_format_t *fm = &FDD_FORMATS[s_media[d]];
    set_rate(d);
    fdd_motor_start(d);
    if (!fdd_seek(d, cyl))
        return false;
    uint8_t *ids = s_dma_buf;
    for (int i = 0; i < s_spt[d]; i++) {
        ids[i * 4 + 0] = cyl;
        ids[i * 4 + 1] = head;
        ids[i * 4 + 2] = (uint8_t)(i + 1);
        ids[i * 4 + 3] = 2; /* 512 bytes */
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        dma_setup_write(ids, (uint16_t)(s_spt[d] * 4));
        fdc_send_byte(FDD_CMD_FORMAT);
        fdc_send_byte((uint8_t)((head << 2) | (d & 1)));
        fdc_send_byte(2);        /* N: 512 bytes */
        fdc_send_byte(s_spt[d]); /* sectors on the track */
        fdc_send_byte(fm->gap_format);
        fdc_send_byte(0xF6); /* filler */
        if (!fdc_wait_result()) {
            fdc_reset();
            return false;
        }
        uint8_t st0 = (uint8_t)fdc_recv_byte();
        uint8_t st1 = (uint8_t)fdc_recv_byte();
        uint8_t st2 = (uint8_t)fdc_recv_byte();
        for (int k = 0; k < 4; k++)
            (void)fdc_recv_byte();
        if ((st0 & 0xC0) == 0 && !st1 && !st2)
            return true;
        if (st1 & 0x02) /* write protected */
            return false;
        fdd_recalibrate(d);
        fdd_seek(d, cyl);
    }
    return false;
}

/* ── init / detect ──────────────────────────────────────────────────────── */
bool fdd_is_present(uint8_t drive_id) {
    if (drive_id > 1)
        return false;
    return s_present[drive_id];
}

bool fdd_init(uint8_t drive_id) {
    if (drive_id > 1)
        return false;
    s_present[drive_id] = false;
    if (!fdd_cmos_present(drive_id))
        return false;

    fdc_reset();
    s_present[drive_id] = true;
    s_media[drive_id] = FDD_MEDIA_NONE; /* found on the first read */
    /* the drive answers a recalibrate with or without a disk in it */
    if (!fdd_recalibrate(drive_id)) {
        s_present[drive_id] = false;
        fdd_motor_off(drive_id);
        return false;
    }
    fdd_motor_off(drive_id);
    return true;
}
