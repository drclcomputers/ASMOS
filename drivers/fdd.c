#include "drivers/fdd.h"
#include "lib/core.h"
#include "lib/memory.h"
#include "lib/time.h"

#define s_dma_buf ((uint8_t *)0x7000)
#define s_present ((bool *)0x7200)
#define s_motor_on ((bool *)0x7204)
#define s_current_track ((int *)0x7208)

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
    outb(FDD_CCR, 0x00); /* 500 kbps: 1.44 MB */
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

    for (int attempt = 0; attempt < 3; attempt++) {
        if (!fdc_send_byte(FDD_CMD_SEEK) || !fdc_send_byte(drive_id & 1) ||
            !fdc_send_byte(track))
            continue;
        uint8_t st0, cyl;
        if (fdc_wait_seek(drive_id, &st0, &cyl) && !(st0 & 0xC0) &&
            cyl == track) {
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
static void lba_to_chs(uint32_t lba, uint8_t *cyl, uint8_t *head,
                       uint8_t *sector) {
    *cyl = (uint8_t)(lba / (FDD_HEADS * FDD_SECTORS));
    *head = (uint8_t)((lba / FDD_SECTORS) % FDD_HEADS);
    *sector = (uint8_t)((lba % FDD_SECTORS) + 1); /* 1-based */
}

/* ── read/write sector ──────────────────────────────────────────────────── */
bool fdd_read_sector(uint8_t drive_id, uint32_t lba, void *buf) {
    if (drive_id > 1 || !s_present[drive_id])
        return false;
    if (lba >= FDD_SECTORS_TOTAL)
        return false;

    uint8_t cyl, head, sector;
    lba_to_chs(lba, &cyl, &head, &sector);

    fdd_motor_start(drive_id);
    if (!fdd_seek(drive_id, cyl)) {
        fdd_motor_off(drive_id);
        return false;
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        dma_setup_read(s_dma_buf, FDD_SECTOR_SIZE);

        fdc_send_byte(FDD_CMD_READ);
        fdc_send_byte((head << 2) | (drive_id & 1));
        fdc_send_byte(cyl);
        fdc_send_byte(head);
        fdc_send_byte(sector);
        fdc_send_byte(2); /* sector size: 512 = 2^(2+7) */
        fdc_send_byte(FDD_SECTORS);
        fdc_send_byte(0x1B);
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
    if (lba >= FDD_SECTORS_TOTAL)
        return false;

    uint8_t cyl, head, sector;
    lba_to_chs(lba, &cyl, &head, &sector);

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
        fdc_send_byte(FDD_SECTORS);
        fdc_send_byte(0x1B);
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
    /* the drive answers a recalibrate with or without a disk in it */
    if (!fdd_recalibrate(drive_id)) {
        s_present[drive_id] = false;
        fdd_motor_off(drive_id);
        return false;
    }
    fdd_motor_off(drive_id);
    return true;
}
