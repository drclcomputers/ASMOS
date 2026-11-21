#include "drivers/fdd.h"
#include "lib/alloc.h"
#include "lib/core.h"
#include "lib/memory.h"
#include "lib/time.h"

#define s_dma_buf ((uint8_t *)0x7000)
/* a whole track by DMA (36 x 512: a 2.88 MB track), below 64 KB so it
 * can't cross a DMA page; free once stage 2 (0x7E00) has run */
#define s_track_dma ((uint8_t *)0x8000)
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
static uint8_t s_motors;       /* FDD_DOR_MOTOR_A/B bits that are on */
static uint32_t s_last_use[2]; /* pit_ticks of each drive's last access */
static int s_media[2];
static uint8_t s_spt[2], s_heads[2], s_tracks[2], s_dstep[2];
static int8_t s_wp[2] = {-1, -1}; /* write protected: -1 unknown */

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
        /* a 1.2 MB drive also does 720 KB, 80 tracks of double density
         * (DOS: FORMAT /T:80 /N:9); last, so that a 40-track disk is
         * tried first (its track 0 looks the same) */
        if (fdd_drive_kind(drive_id) == FDD_MEDIA_1200K)
            out[n++] = FDD_MEDIA_720K;
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

static void tc_drop(uint8_t d, bool discard);

void fdd_forget_media(uint8_t drive_id) {
    if (drive_id < 2) {
        tc_drop(drive_id, false); /* (eject already wrote them) */
        s_media[drive_id] = FDD_MEDIA_NONE;
        s_wp[drive_id] = -1;
    }
}

void fdd_set_media(uint8_t drive_id, int media) {
    if (drive_id < 2 && media > 0 && media < FDD_MEDIA_COUNT) {
        tc_drop(drive_id, true); /* formatting: the old tracks are gone */
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
    s_motors = 0; /* the reset stopped both motors */
    for (int d = 0; d < 2; d++) {
        s_motor_on[d] = false;
        s_current_track[d] = -1;
    }
}

/* Seek/recalibrate completes asynchronously: poll SENSE INTERRUPT until
 * "seek end". QEMU and Bochs finish instantly; real drives and 86Box take
 * milliseconds per step. */
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

// motor control
/*
 * The DOR selects a drive and controls both motors, so preserve the other
 * drive's motor bit (otherwise using B: can stop A:). Keep motors on between
 * accesses; fdd_poll() stops them after 2 seconds idle.
 */
#define MOTOR_IDLE_MS 2000

static uint8_t motor_bit(uint8_t d) {
    return d ? FDD_DOR_MOTOR_B : FDD_DOR_MOTOR_A;
}

static void write_dor(uint8_t sel) {
    outb(FDD_DOR, (uint8_t)(FDD_DOR_RESET | FDD_DOR_IRQ_DMA | s_motors |
                            (sel ? FDD_DOR_SEL_B : FDD_DOR_SEL_A)));
}

static void fdd_motor_start(uint8_t drive_id) {
    s_last_use[drive_id] = pit_ticks;
    if (s_motor_on[drive_id] && (s_motors & motor_bit(drive_id))) {
        write_dor(drive_id); /* just select it */
        return;
    }
    s_motors |= motor_bit(drive_id);
    write_dor(drive_id);
    s_motor_on[drive_id] = true;
    fdd_wait_ms(500); /* spin-up */
}

void fdd_motor_off(uint8_t drive_id) {
    if (drive_id > 1 || !s_motor_on[drive_id])
        return;
    s_motors &= (uint8_t)~motor_bit(drive_id);
    s_motor_on[drive_id] = false;
    write_dor(drive_id);
}

bool fdd_motor_running(uint8_t drive_id) {
    return drive_id <= 1 && s_motor_on[drive_id];
}

// recalibrate (seek to track 0)
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

// seek
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

// DMA setup
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

// LBA to CHS
static void lba_to_chs(uint8_t d, uint32_t lba, uint8_t *cyl, uint8_t *head,
                       uint8_t *sector) {
    *cyl = (uint8_t)(lba / (s_heads[d] * s_spt[d]));
    *head = (uint8_t)((lba / s_spt[d]) % s_heads[d]);
    *sector = (uint8_t)((lba % s_spt[d]) + 1); /* 1-based */
}

/* the controller's data rate for this disk (shared by both drives); a
 * double-density disk in a 1.2 MB drive spins at 360 rpm: 300 kbps */
/* double density (250 kbps at 300 rpm) is 300 kbps in a 1.2 MB drive,
 * which turns at 360 rpm: the 40-track disks and 720 KB */
static void set_rate(uint8_t d) {
    int r = FDD_FORMATS[s_media[d]].rate;
    if (r == 2 && fdd_drive_kind(d) == FDD_MEDIA_1200K)
        r = 1;
    outb(FDD_CCR, (uint8_t)r);
}

// transfers
/*
 * n sectors from (cyl, head, sector) on, through the DMA buffer: a whole
 * track in one command takes one turn of the disk; sector by sector, each
 * one waits most of a turn for the next to come round. */
static bool xfer(uint8_t d, uint8_t cyl, uint8_t head, uint8_t sector, int n,
                 bool write, int tries) {
    const fdd_format_t *fm = &FDD_FORMATS[s_media[d]];
    uint8_t *dma = n > 1 ? s_track_dma : s_dma_buf;
    set_rate(d);
    fdd_motor_start(d);
    if (!fdd_seek(d, cyl)) {
        fdd_motor_off(d);
        return false;
    }
    for (int attempt = 0; attempt < tries; attempt++) {
        if (write)
            dma_setup_write(dma, (uint16_t)(n * FDD_SECTOR_SIZE));
        else
            dma_setup_read(dma, (uint16_t)(n * FDD_SECTOR_SIZE));
        fdc_send_byte(write ? FDD_CMD_WRITE : FDD_CMD_READ);
        fdc_send_byte((uint8_t)((head << 2) | (d & 1)));
        fdc_send_byte(cyl);
        fdc_send_byte(head);
        fdc_send_byte(sector);
        fdc_send_byte(2); /* sector size: 512 = 2^(2+7) */
        fdc_send_byte((uint8_t)(sector + n - 1)); /* the last one (EOT) */
        fdc_send_byte(fm->gap_rw);
        fdc_send_byte(0xFF);

        if (!fdc_wait_result()) { /* no disk: don't try again */
            fdc_reset();          /* frees the stuck controller */
            return false;         /* motor is off after a reset */
        }
        uint8_t st0 = (uint8_t)fdc_recv_byte();
        uint8_t st1 = (uint8_t)fdc_recv_byte();
        uint8_t st2 = (uint8_t)fdc_recv_byte();
        for (int k = 0; k < 4; k++)
            (void)fdc_recv_byte(); /* C H R N */
        if ((st0 & 0xC0) == 0 && st1 == 0 && st2 == 0) {
            s_last_use[d] = pit_ticks; /* the motor stays on a bit */
            return true;
        }
        if (st1 & 0x02) /* write protected: trying again won't help */
            break;
        fdd_recalibrate(d);
        fdd_seek(d, cyl);
    }
    fdd_motor_off(d);
    return false;
}

static bool read_raw(uint8_t drive_id, uint32_t lba, void *buf, int tries) {
    uint8_t cyl, head, sector;
    lba_to_chs(drive_id, lba, &cyl, &head, &sector);
    if (!xfer(drive_id, cyl, head, sector, 1, false, tries))
        return false;
    memcpy(buf, s_dma_buf, FDD_SECTOR_SIZE);
    return true;
}

// the track cache
/*
 * Cache several tracks, one side per line. Reads load a whole track on a
 * miss; writes stay dirty until eviction, 1/2 s idle (fdd_poll), or
 * fdd_flush (eject/shutdown). Lines are allocated on first use to fit the
 * disk's track size. */
#define TC_LINES 4
#define TC_V 1 /* sector valid */
#define TC_D 2 /* sector dirty */
#define FLUSH_AFTER_MS 500

typedef struct {
    int8_t drive; /* -1: free */
    uint8_t cyl, head;
    uint32_t used;
    uint8_t flag[36];
    uint8_t *data;
} tc_line_t;

static tc_line_t s_tc[TC_LINES]; /* drive -1: free (fdd_init) */
static bool s_tc_init;

static void tc_init(void) {
    if (s_tc_init)
        return;
    for (int i = 0; i < TC_LINES; i++)
        s_tc[i].drive = -1;
    s_tc_init = true;
}
static int s_tc_spt; /* sectors each line holds */
static uint32_t s_tc_clock;
static uint32_t s_last_write[2];
static bool s_changed[2];       /* the disk was changed (fdd_take_change) */
static bool s_no_changeline[2]; /* this drive has none (it stays on) */

static tc_line_t *tc_find(uint8_t d, uint8_t cyl, uint8_t head) {
    tc_init();
    for (int i = 0; i < TC_LINES; i++)
        if (s_tc[i].drive == d && s_tc[i].cyl == cyl && s_tc[i].head == head)
            return &s_tc[i];
    return NULL;
}

/* the dirty sectors of a line to the disk: runs of them, one command each */
static bool tc_flush_line(tc_line_t *l) {
    if (l->drive < 0)
        return true;
    uint8_t d = (uint8_t)l->drive;
    bool ok = true;
    for (int s = 0; s < s_tc_spt;) {
        if (!(l->flag[s] & TC_D)) {
            s++;
            continue;
        }
        int e = s;
        while (e < s_tc_spt && (l->flag[e] & TC_D))
            e++;
        int n = e - s;
        memcpy(n > 1 ? s_track_dma : s_dma_buf, l->data + s * FDD_SECTOR_SIZE,
               (uint32_t)n * FDD_SECTOR_SIZE);
        bool w = xfer(d, l->cyl, l->head, (uint8_t)(s + 1), n, true, 3);
        if (!w && n > 1) /* one bad sector: the others one at a time */
            for (int k = s; k < e; k++) {
                memcpy(s_dma_buf, l->data + k * FDD_SECTOR_SIZE,
                       FDD_SECTOR_SIZE);
                if (!xfer(d, l->cyl, l->head, (uint8_t)(k + 1), 1, true, 3))
                    ok = false;
            }
        else if (!w)
            ok = false;
        for (int k = s; k < e; k++)
            l->flag[k] &= (uint8_t)~TC_D;
        s = e;
    }
    return ok;
}

/* the drive's lines go (written first unless `discard`) */
static void tc_drop(uint8_t d, bool discard) {
    tc_init();
    for (int i = 0; i < TC_LINES; i++)
        if (s_tc[i].drive == d) {
            if (!discard)
                tc_flush_line(&s_tc[i]);
            s_tc[i].drive = -1;
        }
}

/* a line for (d, cyl, head): the free or least recently used one */
static tc_line_t *tc_take(uint8_t d, uint8_t cyl, uint8_t head) {
    tc_init();
    int spt = s_spt[d];
    if (spt > s_tc_spt) { /* a disk with longer tracks: bigger lines */
        for (int i = 0; i < TC_LINES; i++) {
            if (s_tc[i].drive >= 0)
                tc_flush_line(&s_tc[i]);
            s_tc[i].drive = -1;
            if (s_tc[i].data)
                kfree(s_tc[i].data);
            s_tc[i].data = NULL;
        }
        s_tc_spt = spt;
    }
    tc_line_t *l = NULL;
    for (int i = 0; i < TC_LINES; i++)
        if (!l || s_tc[i].drive < 0 ||
            (l->drive >= 0 && s_tc[i].used < l->used))
            l = &s_tc[i];
    if (l->drive >= 0 && !tc_flush_line(l))
        return NULL;
    if (!l->data &&
        !(l->data = (uint8_t *)kmalloc((uint32_t)s_tc_spt * FDD_SECTOR_SIZE)))
        return NULL;
    l->drive = (int8_t)d;
    l->cyl = cyl;
    l->head = head;
    memset(l->flag, 0, sizeof(l->flag));
    return l;
}

bool fdd_flush(uint8_t d) {
    if (d > 1)
        return false;
    tc_init();
    bool ok = true;
    for (int i = 0; i < TC_LINES; i++)
        if (s_tc[i].drive == d && !tc_flush_line(&s_tc[i]))
            ok = false;
    return ok;
}

static bool tc_dirty(uint8_t d) { /* writes still waiting */
    tc_init();
    bool dirty = false;
    for (int i = 0; i < TC_LINES; i++)
        if (s_tc[i].drive == d)
            for (int k = 0; k < s_tc_spt; k++)
                dirty |= (s_tc[i].flag[k] & TC_D) != 0;
    return dirty;
}

/* main loop: write what has waited 1/2 s, stop motors idle for 2 s */
void fdd_poll(void) {
    for (uint8_t d = 0; d < 2; d++)
        if (tc_dirty(d) &&
            (int32_t)(pit_ticks - s_last_write[d]) >= FLUSH_AFTER_MS)
            fdd_flush(d);
    for (uint8_t d = 0; d < 2; d++)
        if (s_motor_on[d] &&
            (int32_t)(pit_ticks - s_last_use[d]) >= MOTOR_IDLE_MS)
            fdd_motor_off(d);
}

// the disk change line
/*
 * Door opening raises the change line; stepping with a disk lowers it.
 * On change, discard cached tracks and geometry and tell the file system to
 * remount. Drives without a change line are not polled again after reads.
 */
static bool disk_changed(uint8_t d) {
    if (s_no_changeline[d])
        return false;
    write_dor(d); /* the line is the selected drive's */
    if (!(inb(FDD_DIR) & 0x80))
        return false;
    tc_drop(d, true); /* another disk: what we kept isn't its */
    s_media[d] = FDD_MEDIA_NONE;
    s_wp[d] = -1;
    s_changed[d] = true;
    /* lower it: step away and back (with a disk in the drive) */
    s_current_track[d] = -1;
    if (fdd_recalibrate(d) && fdd_seek(d, 1)) {
        s_current_track[d] = 1;
        fdd_seek(d, 0);
    }
    write_dor(d);
    if ((inb(FDD_DIR) & 0x80) && fdd_detect(d))
        s_no_changeline[d] = true; /* a disk reads, the line stays: none */
    return true;
}

/* Only reads the change line: the head does not move, so it is silent
 * (lowering the line takes a step, done when the next disk is read). */
bool fdd_door_opened(uint8_t d, bool *lost) {
    *lost = false;
    if (d > 1 || !s_present[d] || s_no_changeline[d])
        return false;
    write_dor(d); /* the line is the selected drive's */
    if (!(inb(FDD_DIR) & 0x80))
        return false;
    *lost = tc_dirty(d);
    tc_drop(d, true); /* that disk is gone */
    s_media[d] = FDD_MEDIA_NONE;
    s_wp[d] = -1;
    return true;
}

bool fdd_take_change(uint8_t d) {
    if (d > 1 || !s_changed[d])
        return false;
    s_changed[d] = false;
    return true;
}

/* the drive's own answer (SENSE DRIVE STATUS, ST3 bit 6) */
static bool write_protected(uint8_t d) {
    if (s_wp[d] < 0) {
        fdc_send_byte(0x04);
        fdc_send_byte(d & 1);
        int st3 = fdc_recv_byte();
        s_wp[d] = st3 >= 0 && (st3 & 0x40) ? 1 : 0;
    }
    return s_wp[d] == 1;
}

/* Is there a disk? The change line, lowered first if it is up (a step: the
 * drive clicks once). An unformatted disk counts as one. */
int fdd_disk_in(uint8_t d) {
    if (d > 1 || !s_present[d])
        return 0;
    if (s_no_changeline[d])
        return -1;
    disk_changed(d); /* (up: drops what was kept, steps the head) */
    if (s_no_changeline[d])
        return 1; /* it just read one */
    write_dor(d);
    return (inb(FDD_DIR) & 0x80) ? 0 : 1;
}

bool fdd_write_protected(uint8_t d) {
    return d < 2 && s_present[d] && write_protected(d);
}

// read/write sector
static bool ready(uint8_t d, uint32_t lba) {
    if (d > 1 || !s_present[d])
        return false;
    if (disk_changed(d))
        return false; /* the file system mounts the new disk */
    if (!s_media[d] && !fdd_detect(d))
        return false;
    return lba < fdd_total_sectors(d);
}

bool fdd_read_sector(uint8_t drive_id, uint32_t lba, void *buf) {
    if (!ready(drive_id, lba))
        return false;
    uint8_t cyl, head, sector;
    lba_to_chs(drive_id, lba, &cyl, &head, &sector);
    int i = sector - 1, spt = s_spt[drive_id];
    tc_line_t *l = tc_find(drive_id, cyl, head);
    if (!l || !(l->flag[i] & TC_V)) {
        if (!l && !(l = tc_take(drive_id, cyl, head)))
            return read_raw(drive_id, lba, buf, 3); /* no memory: direct */
        if (xfer(drive_id, cyl, head, 1, spt, false, 2)) { /* the track */
            for (int k = 0; k < spt; k++)
                if (!(l->flag[k] & TC_D)) {
                    memcpy(l->data + k * FDD_SECTOR_SIZE,
                           s_track_dma + k * FDD_SECTOR_SIZE, FDD_SECTOR_SIZE);
                    l->flag[k] |= TC_V;
                }
        } else { /* a bad sector somewhere: just this one */
            if (!read_raw(drive_id, lba, l->data + i * FDD_SECTOR_SIZE, 3))
                return false;
            l->flag[i] |= TC_V;
        }
    }
    l->used = ++s_tc_clock;
    memcpy(buf, l->data + i * FDD_SECTOR_SIZE, FDD_SECTOR_SIZE);
    return true;
}

bool fdd_write_sector(uint8_t drive_id, uint32_t lba, const void *buf) {
    if (!ready(drive_id, lba))
        return false;
    fdd_motor_start(drive_id);
    if (write_protected(drive_id))
        return false;
    uint8_t cyl, head, sector;
    lba_to_chs(drive_id, lba, &cyl, &head, &sector);
    int i = sector - 1;
    tc_line_t *l = tc_find(drive_id, cyl, head);
    if (!l && !(l = tc_take(drive_id, cyl, head))) { /* no memory: direct */
        memcpy(s_dma_buf, buf, FDD_SECTOR_SIZE);
        return xfer(drive_id, cyl, head, sector, 1, true, 3);
    }
    memcpy(l->data + i * FDD_SECTOR_SIZE, buf, FDD_SECTOR_SIZE);
    l->flag[i] |= TC_V | TC_D;
    l->used = ++s_tc_clock;
    s_last_write[drive_id] = pit_ticks;
    return true;
}

// what disk is in the drive
/*
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
        if (bpb) {
            /* the tracks from its size: 40 and 80 share track 0 (a 720 KB
             * disk reads as 360 KB at first in a 1.2 MB drive) */
            uint32_t total = (uint32_t)(sec[19] | sec[20] << 8);
            if (!total)
                total = (uint32_t)sec[32] | (uint32_t)sec[33] << 8 |
                        (uint32_t)sec[34] << 16 | (uint32_t)sec[35] << 24;
            uint32_t tr = total / (spt * heads);
            if ((tr == 40 || tr == 80) && total == tr * spt * heads) {
                found = media_for(spt, heads, (int)tr);
                bool here = false; /* a size this drive takes */
                for (int k = 0; k < n; k++)
                    here |= cand[k] == found;
                if (found && !here) { /* (80 tracks in a 40-track drive) */
                    s_media[d] = FDD_MEDIA_NONE;
                    fdd_motor_off(d);
                    return FDD_MEDIA_NONE;
                }
            }
            if (!found)
                found = media_for(spt, heads, FDD_FORMATS[m].tracks);
        } else if (is40 && read_raw(d, 1, sec, 1)) /* C0 H0 S2: the FAT */
            found = fdd_media_from_byte(sec[0]);
        if (!found)
            found = m;
        use_media(d, found, bpb ? spt : 0, bpb ? heads : 0);
        if (found != m)
            s_current_track[d] = -1; /* (other stepping: recalibrate) */
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

// init / detect
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
