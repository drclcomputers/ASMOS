#include "fs/fdd.h"
#include "lib/core.h"
#include "lib/memory.h"
#include "lib/time.h"

static uint8_t s_dma_buf[FDD_SECTOR_SIZE] __attribute__((aligned(512)));

static bool s_present[2] = {false, false};
static bool s_motor_on[2] = {false, false};
static int s_current_track[2] = {-1, -1};

static bool fdc_wait_rqm(void) {
    uint32_t timeout = 0x40000;
    while (--timeout) {
        if (inb(FDD_MSR) & FDD_MSR_RQM)
            return true;
    }
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

static void fdc_sense_interrupt(uint8_t *st0, uint8_t *cyl) {
    fdc_send_byte(FDD_CMD_SENSE);
    *st0 = (uint8_t)fdc_recv_byte();
    *cyl = (uint8_t)fdc_recv_byte();
}

/* ── IRQ wait (polled — we don't use IRQ handler for FDD) ─────────────── */

static bool fdc_wait_interrupt(void) {
    /* Poll MSR for completion: wait until BUSY clears */
    uint32_t timeout = 0x200000;
    while (--timeout) {
        uint8_t msr = inb(FDD_MSR);
        if (!(msr & FDD_MSR_BUSY))
            return true;
        __asm__ volatile("nop");
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
    sleep_ms(300);
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
    for (int attempt = 0; attempt < 3; attempt++) {
        fdc_send_byte(FDD_CMD_RECAL);
        fdc_send_byte(drive_id & 1);
        fdc_wait_interrupt();

        uint8_t st0, cyl;
        fdc_sense_interrupt(&st0, &cyl);

        if ((st0 & 0x20) && cyl == 0) {
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

    for (int attempt = 0; attempt < 3; attempt++) {
        fdc_send_byte(FDD_CMD_SEEK);
        fdc_send_byte((drive_id & 1));
        fdc_send_byte(track);
        fdc_wait_interrupt();

        uint8_t st0, cyl;
        fdc_sense_interrupt(&st0, &cyl);

        if (cyl == track) {
            s_current_track[drive_id] = track;
            sleep_ms(15);
            return true;
        }
    }
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
    outb(DMA_MODE, 0x56);
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
    outb(DMA_MODE, 0x5A);
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

        uint32_t timeout = 0x400000;
        while (--timeout) {
            uint8_t msr = inb(FDD_MSR);
            if ((msr & (FDD_MSR_RQM | FDD_MSR_DIO)) ==
                (FDD_MSR_RQM | FDD_MSR_DIO))
                break;
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

        uint32_t timeout = 0x400000;
        while (--timeout) {
            uint8_t msr = inb(FDD_MSR);
            if ((msr & (FDD_MSR_RQM | FDD_MSR_DIO)) ==
                (FDD_MSR_RQM | FDD_MSR_DIO))
                break;
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

    /* Reset FDC */
    outb(FDD_DOR, 0x00);
    sleep_ms(5);
    outb(FDD_DOR, FDD_DOR_RESET | FDD_DOR_IRQ_DMA);
    sleep_ms(5);

    /* Specify: SRT=8ms, HLT=15ms, HUT=240ms, no DMA=0 */
    fdc_send_byte(FDD_CMD_SPECIFY);
    fdc_send_byte(0x8F); /* SRT/HUT */
    fdc_send_byte(0x02); /* HLT, DMA mode */

    /* Set data rate: 500 Kbps for 1.44 MB */
    outb(FDD_CCR, 0x00);

    s_present[drive_id] = true;
    s_current_track[drive_id] = -1;
    s_motor_on[drive_id] = false;

    if (!fdd_recalibrate(drive_id)) {
        s_present[drive_id] = false;
        fdd_motor_off(drive_id);
        return false;
    }

    fdd_motor_off(drive_id);
    s_present[drive_id] = true;
    return true;
}
