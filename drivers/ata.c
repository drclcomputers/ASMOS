#include "drivers/ata.h"
#include "drivers/cdrom.h"
#include "drivers/pci.h"
#include "lib/io.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"

typedef struct {
    uint16_t base; /* command block (0x1F0) */
    uint16_t ctrl; /* control block (0x3F6): alt status / device control */
} ata_channel_t;

static ata_channel_t s_chan[2] = {{0x1F0, 0x3F6}, {0x170, 0x376}};
static ata_disk_t s_disks[ATA_MAX_DISKS]; /* index = channel*2 + slave */
static int8_t s_unit[2] = {-1, -1};       /* logical unit -> s_disks index */
static char s_desc[48] = "legacy ports";

/* ── low level ─────────────────────────────────────────────────────────── */

static inline uint8_t status(const ata_channel_t *c) {
    return inb(c->base + ATA_REG_STATUS);
}

/* ~400 ns: four reads of the alternate status register */
static void delay400(const ata_channel_t *c) {
    for (int i = 0; i < 4; i++)
        inb(c->ctrl);
}

/* Wait for BSY to clear. Generous limit: old drives spin up slowly. */
static bool wait_not_busy(const ata_channel_t *c, uint32_t limit) {
    while (limit--) {
        uint8_t s = status(c);
        if (s == 0xFF)
            return false; /* floating bus: nothing there */
        if (!(s & ATA_SR_BSY))
            return true;
    }
    return false;
}

static bool wait_drq(const ata_channel_t *c) {
    uint32_t limit = 0x200000;
    while (limit--) {
        uint8_t s = status(c);
        if (s & ATA_SR_BSY)
            continue; /* other bits undefined while BSY */
        if (s & (ATA_SR_ERR | ATA_SR_DF))
            return false;
        if (s & ATA_SR_DRQ)
            return true;
    }
    return false;
}

static bool wait_ready(const ata_channel_t *c) {
    if (!wait_not_busy(c, 0x200000))
        return false;
    return !(status(c) & (ATA_SR_ERR | ATA_SR_DF));
}

/* Program drive/head + address registers for one sector. */
static bool set_address(const ata_disk_t *d, const ata_channel_t *c,
                        uint32_t lba) {
    if (lba >= d->sectors)
        return false;
    uint8_t sel = d->slave ? 0x10 : 0x00;
    if (d->lba) {
        outb(c->base + ATA_REG_DRIVE, 0xE0 | sel | ((lba >> 24) & 0x0F));
        delay400(c);
        if (!wait_not_busy(c, 0x200000))
            return false;
        outb(c->base + ATA_REG_SECCOUNT, 1);
        outb(c->base + ATA_REG_LBA_LO, lba & 0xFF);
        outb(c->base + ATA_REG_LBA_MID, (lba >> 8) & 0xFF);
        outb(c->base + ATA_REG_LBA_HI, (lba >> 16) & 0xFF);
    } else {
        uint32_t track = lba / d->spt;
        uint8_t sector = (uint8_t)(lba % d->spt + 1);
        uint8_t head = (uint8_t)(track % d->heads);
        uint16_t cyl = (uint16_t)(track / d->heads);
        outb(c->base + ATA_REG_DRIVE, 0xA0 | sel | (head & 0x0F));
        delay400(c);
        if (!wait_not_busy(c, 0x200000))
            return false;
        outb(c->base + ATA_REG_SECCOUNT, 1);
        outb(c->base + ATA_REG_LBA_LO, sector);
        outb(c->base + ATA_REG_LBA_MID, cyl & 0xFF);
        outb(c->base + ATA_REG_LBA_HI, cyl >> 8);
    }
    return true;
}

static const ata_disk_t *disk_of(uint8_t unit) {
    if (unit > 1 || s_unit[unit] < 0)
        return NULL;
    return &s_disks[(int)s_unit[unit]];
}

/* ── detection ─────────────────────────────────────────────────────────── */

/* PCI IDE controller in native mode: use its BARs. Compatibility mode (the
 * norm on 386-Pentium boards, and ISA cards) keeps the legacy ports. */
static void find_controller(void) {
    const pci_dev_t *ide = pci_find_class(0x01, 0x01, 0);
    if (!ide) {
        strcpy(s_desc, pci_mechanism() ? "legacy ports (no PCI IDE)"
                                       : "legacy ports (ISA)");
        return;
    }
    pci_enable(ide, true, false, false);
    int native = 0;
    for (int ch = 0; ch < 2; ch++) {
        if (!(ide->prog_if & (1 << (ch * 2))))
            continue; /* this channel is in compatibility mode */
        uint16_t b = pci_bar_io(ide, ch * 2), k = pci_bar_io(ide, ch * 2 + 1);
        if (b && k) {
            s_chan[ch].base = b;
            s_chan[ch].ctrl = (uint16_t)(k + 2);
            native++;
        }
    }
    snprintf(s_desc, sizeof(s_desc), "PCI %04X:%04X%s", ide->vendor,
             ide->device, native ? " native" : "");
}

static void reset_channel(const ata_channel_t *c) {
    if (status(c) == 0xFF)
        return;          /* nothing on this cable */
    outb(c->ctrl, 0x06); /* SRST + nIEN (we poll; keep IRQ 14/15 quiet) */
    for (int i = 0; i < 40; i++)
        inb(c->ctrl); /* >5 us */
    outb(c->ctrl, 0x02);
    wait_not_busy(c, 0x400000);
}

static void copy_model(char *dst, const uint16_t *id) {
    for (int i = 0; i < 20; i++) {
        dst[i * 2] = (char)(id[27 + i] >> 8);
        dst[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    dst[40] = 0;
    for (int i = 39; i >= 0 && (dst[i] == ' ' || !dst[i]); i--)
        dst[i] = 0;
}

static bool identify(int ch, int slave, ata_disk_t *d) {
    const ata_channel_t *c = &s_chan[ch];
    outb(c->base + ATA_REG_DRIVE, slave ? 0xB0 : 0xA0);
    delay400(c);
    if (status(c) == 0xFF)
        return false;
    /* registers must hold what we write, or there is no drive */
    outb(c->base + ATA_REG_SECCOUNT, 0x55);
    outb(c->base + ATA_REG_LBA_LO, 0xAA);
    if (inb(c->base + ATA_REG_SECCOUNT) != 0x55 ||
        inb(c->base + ATA_REG_LBA_LO) != 0xAA)
        return false;
    outb(c->base + ATA_REG_SECCOUNT, 0);
    outb(c->base + ATA_REG_LBA_LO, 0);
    outb(c->base + ATA_REG_LBA_MID, 0);
    outb(c->base + ATA_REG_LBA_HI, 0);
    outb(c->base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    delay400(c);
    if (status(c) == 0 || !wait_not_busy(c, 0x400000))
        return false;
    /* ATAPI / SATA signatures: not a hard disk we can use (a CD drive is
     * looked at by atapi_identify) */
    if (inb(c->base + ATA_REG_LBA_MID) || inb(c->base + ATA_REG_LBA_HI))
        return false;
    if (!wait_drq(c))
        return false;
    uint16_t id[256];
    for (int i = 0; i < 256; i++)
        id[i] = inw(c->base + ATA_REG_DATA);

    memset(d, 0, sizeof(*d));
    d->channel = (uint8_t)ch;
    d->slave = (uint8_t)slave;
    d->cyls = id[1];
    d->heads = id[3];
    d->spt = id[6];
    if ((id[53] & 1) && id[54] && id[55] && id[56]) { /* current geometry */
        d->cyls = id[54];
        d->heads = id[55];
        d->spt = id[56];
    }
    d->lba = (id[49] & (1 << 9)) != 0;
    uint32_t lba_sectors = id[60] | ((uint32_t)id[61] << 16);
    if (d->lba && lba_sectors)
        d->sectors = lba_sectors;
    else {
        d->lba = false;
        d->sectors = (uint32_t)d->cyls * d->heads * d->spt;
    }
    if (!d->lba && (!d->heads || !d->spt || d->heads > 16))
        return false;
    copy_model(d->model, id);
    d->present = d->sectors != 0;
    return d->present;
}

/* An ASMOS boot sector (OEM "ASMOS") in sector 0 (a disk without
 * partitions, like the old 32 MB image), or in the first sector of the
 * active partition (what Setup and tools/mkdisk.py make). */
static bool is_asmos_disk(int idx) {
    uint8_t buf[512];
    int8_t saved = s_unit[0];
    s_unit[0] = (int8_t)idx;
    bool ok =
        ata_read_sector(0, 0, buf) && buf[510] == 0x55 && buf[511] == 0xAA;
    if (ok && memcmp(buf + 3, "ASMOS", 5) != 0) {
        uint32_t start = 0;
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = buf + 0x1BE + i * 16;
            if (e[0] == 0x80)
                start = (uint32_t)e[8] | (uint32_t)e[9] << 8 |
                        (uint32_t)e[10] << 16 | (uint32_t)e[11] << 24;
        }
        ok = start && ata_read_sector(0, start, buf) && buf[510] == 0x55 &&
             buf[511] == 0xAA && memcmp(buf + 3, "ASMOS", 5) == 0;
    }
    s_unit[0] = saved;
    return ok;
}

static bool atapi_identify(int ch, int slave);

int ata_init(uint8_t bios_boot_drive) {
    find_controller();
    for (int ch = 0; ch < 2; ch++)
        reset_channel(&s_chan[ch]);
    int found = 0;
    for (int i = 0; i < ATA_MAX_DISKS; i++)
        if (identify(i / 2, i % 2, &s_disks[i]))
            found++;
        else {
            s_disks[i].present = false;
            atapi_identify(i / 2, i % 2); /* a CD drive? */
        }

    /* HDA = the disk we booted from: one carrying our boot sector. If
     * several do, the BIOS drive number picks (0x80 = first disk the BIOS
     * enumerated = first one we find, normally). */
    int order[ATA_MAX_DISKS], n = 0, first_asmos = -1;
    for (int i = 0; i < ATA_MAX_DISKS; i++)
        if (s_disks[i].present)
            order[n++] = i;
    int want = bios_boot_drive >= 0x80 ? bios_boot_drive - 0x80 : -1;
    for (int k = 0; k < n; k++) {
        if (!is_asmos_disk(order[k]))
            continue;
        if (first_asmos < 0)
            first_asmos = order[k];
        if (k == want) {
            first_asmos = order[k];
            break;
        }
    }
    s_unit[0] = (int8_t)(first_asmos >= 0 ? first_asmos : n ? order[0] : -1);
    for (int i = 0; i < ATA_MAX_DISKS; i++)
        if (s_disks[i].present && i != s_unit[0]) {
            s_unit[1] = (int8_t)i;
            break;
        }
    return found;
}

const ata_disk_t *ata_unit(uint8_t unit) { return disk_of(unit); }
const char *ata_controller_desc(void) { return s_desc; }

/* ── I/O ───────────────────────────────────────────────────────────────── */

bool ata_read_sector(uint8_t unit, uint32_t lba, void *buf) {
    const ata_disk_t *d = disk_of(unit);
    if (!d)
        return false;
    const ata_channel_t *c = &s_chan[d->channel];
    if (!set_address(d, c, lba))
        return false;
    outb(c->base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);
    delay400(c);
    if (!wait_drq(c))
        return false;
    uint16_t *p = (uint16_t *)buf;
    for (int i = 0; i < 256; i++)
        p[i] = inw(c->base + ATA_REG_DATA);
    return true;
}

bool ata_write_sector(uint8_t unit, uint32_t lba, const void *buf) {
    const ata_disk_t *d = disk_of(unit);
    if (!d)
        return false;
    const ata_channel_t *c = &s_chan[d->channel];
    if (!set_address(d, c, lba))
        return false;
    outb(c->base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);
    delay400(c);
    if (!wait_drq(c))
        return false;
    const uint16_t *p = (const uint16_t *)buf;
    for (int i = 0; i < 256; i++)
        outw(c->base + ATA_REG_DATA, p[i]);
    if (!wait_ready(c)) /* the write must finish before FLUSH */
        return false;
    /* FLUSH CACHE is ATA-4+; old drives abort it, which is harmless (they
     * have no write cache), so its error status is ignored. */
    outb(c->base + ATA_REG_COMMAND, ATA_CMD_FLUSH);
    delay400(c);
    wait_not_busy(c, 0x400000);
    return true;
}

bool ata_read_sectors(uint8_t unit, uint32_t lba, uint8_t count, void *buf) {
    uint8_t *p = (uint8_t *)buf;
    for (uint8_t i = 0; i < count; i++, p += 512)
        if (!ata_read_sector(unit, lba + i, p))
            return false;
    return true;
}

bool ata_write_sectors(uint8_t unit, uint32_t lba, uint8_t count,
                       const void *buf) {
    const uint8_t *p = (const uint8_t *)buf;
    for (uint8_t i = 0; i < count; i++, p += 512)
        if (!ata_write_sector(unit, lba + i, p))
            return false;
    return true;
}

/* ── shut down ─────────────────────────────────────────────────────────── */

/* BSY clear within ms milliseconds (the timer runs: spinning down takes
 * seconds on old drives) */
static bool wait_idle_ms(const ata_channel_t *c, uint32_t ms) {
    uint32_t t0 = time_millis();
    while (time_millis() - t0 < ms) {
        uint8_t s = status(c);
        if (s == 0xFF)
            return false;
        if (!(s & ATA_SR_BSY))
            return true;
    }
    return false;
}

static bool disk_cmd(const ata_disk_t *d, uint8_t cmd, uint32_t ms) {
    const ata_channel_t *c = &s_chan[d->channel];
    outb(c->base + ATA_REG_DRIVE, 0xA0 | (d->slave ? 0x10 : 0x00));
    delay400(c);
    if (!wait_idle_ms(c, 2000))
        return false;
    outb(c->base + ATA_REG_COMMAND, cmd);
    delay400(c);
    if (!wait_idle_ms(c, ms))
        return false;
    return !(status(c) & ATA_SR_ERR);
}

void ata_park(bool spin_down) {
    for (int i = 0; i < ATA_MAX_DISKS; i++) {
        const ata_disk_t *d = &s_disks[i];
        if (!d->present)
            continue;
        /* FLUSH CACHE: old drives abort it (no write cache): fine */
        disk_cmd(d, ATA_CMD_FLUSH, 5000);
        if (spin_down && !disk_cmd(d, 0xE0, 10000)) /* STANDBY IMMEDIATE */
            disk_cmd(d, 0x94, 10000); /* the pre-ATA-2 code for it */
    }
}

/* ── the CD-ROM drive (ATAPI; drivers/cdrom.h) ─────────────────────────────
 * Commands go as 12-byte SCSI packets: PACKET (A0h), then the packet as 6
 * words, then the data in pieces whose size the drive puts in the cylinder
 * registers. All polled; the waits are timed (a disc spins up for
 * seconds). */

static struct {
    bool present;
    uint8_t channel, slave;
    char model[41];
} s_cd;

/* BSY clear within ms (other bits are valid then) */
static bool cd_wait(const ata_channel_t *c, uint32_t ms, uint8_t *st) {
    uint32_t t0 = time_millis();
    do {
        uint8_t s = status(c);
        if (s == 0xFF)
            return false;
        if (!(s & ATA_SR_BSY)) {
            *st = s;
            return true;
        }
    } while (time_millis() - t0 < ms);
    return false;
}

static bool atapi_identify(int ch, int slave) {
    if (s_cd.present)
        return false; /* the first one found */
    const ata_channel_t *c = &s_chan[ch];
    outb(c->base + ATA_REG_DRIVE, slave ? 0xB0 : 0xA0);
    delay400(c);
    /* after the failed IDENTIFY (or a reset) an ATAPI drive shows
     * 14h EBh in the cylinder registers */
    if (inb(c->base + ATA_REG_LBA_MID) != 0x14 ||
        inb(c->base + ATA_REG_LBA_HI) != 0xEB)
        return false;
    outb(c->base + ATA_REG_COMMAND, 0xA1); /* IDENTIFY PACKET DEVICE */
    delay400(c);
    uint8_t st;
    if (!cd_wait(c, 2000, &st) || !(st & ATA_SR_DRQ))
        return false;
    uint16_t id[256];
    for (int i = 0; i < 256; i++)
        id[i] = inw(c->base + ATA_REG_DATA);
    if (((id[0] >> 8) & 0x1F) != 5) /* device type 5: CD/DVD */
        return false;
    s_cd.present = true;
    s_cd.channel = (uint8_t)ch;
    s_cd.slave = (uint8_t)slave;
    copy_model(s_cd.model, id);
    return true;
}

bool cd_present(void) { return s_cd.present; }
const char *cd_model(void) { return s_cd.model; }

/* one packet command; data in to buf (up to len bytes, the rest dropped).
 * Returns the bytes read, or -1 (the drive said no: see REQUEST SENSE) */
static int cd_packet(const uint8_t pkt[12], void *buf, uint32_t len,
                     uint32_t ms) {
    if (!s_cd.present)
        return -1;
    const ata_channel_t *c = &s_chan[s_cd.channel];
    uint8_t st;
    outb(c->base + ATA_REG_DRIVE, s_cd.slave ? 0xB0 : 0xA0);
    delay400(c);
    if (!cd_wait(c, 1000, &st))
        return -1;
    uint32_t limit = len > 0xF800 ? 0xF800 : (len ? len : 2);
    outb(c->base + ATA_REG_FEATURES, 0); /* PIO */
    outb(c->base + ATA_REG_LBA_MID, (uint8_t)(limit & 0xFF));
    outb(c->base + ATA_REG_LBA_HI, (uint8_t)(limit >> 8));
    outb(c->base + ATA_REG_COMMAND, 0xA0); /* PACKET */
    delay400(c);
    if (!cd_wait(c, 1000, &st) || (st & ATA_SR_ERR) || !(st & ATA_SR_DRQ))
        return -1;
    for (int i = 0; i < 6; i++)
        outw(c->base + ATA_REG_DATA,
             (uint16_t)(pkt[2 * i] | (uint16_t)pkt[2 * i + 1] << 8));
    uint8_t *out = buf;
    uint32_t got = 0;
    while (true) {
        delay400(c);
        if (!cd_wait(c, ms, &st))
            return -1;
        if (st & ATA_SR_ERR)
            return -1;
        if (!(st & ATA_SR_DRQ))
            break; /* done */
        uint32_t n = inb(c->base + ATA_REG_LBA_MID) |
                     (uint32_t)inb(c->base + ATA_REG_LBA_HI) << 8;
        if (!n)
            return -1;
        for (uint32_t i = 0; i < (n + 1) / 2; i++) {
            uint16_t w = inw(c->base + ATA_REG_DATA);
            if (got < len)
                out[got] = (uint8_t)w;
            if (got + 1 < len)
                out[got + 1] = (uint8_t)(w >> 8);
            got += 2;
        }
    }
    return (int)(got < len ? got : len);
}

static bool cd_sense(uint8_t *key, uint8_t *asc) {
    uint8_t pkt[12] = {0x03, 0, 0, 0, 18, 0}, b[18];
    if (cd_packet(pkt, b, sizeof(b), 1000) < 14)
        return false;
    *key = b[2] & 0x0F;
    *asc = b[12];
    return true;
}

cd_state_t cd_check(void) {
    if (!s_cd.present)
        return CD_NONE;
    uint8_t pkt[12] = {0}; /* TEST UNIT READY */
    if (cd_packet(pkt, NULL, 0, 1000) >= 0)
        return CD_READY;
    uint8_t key = 0, asc = 0;
    if (!cd_sense(&key, &asc))
        return CD_EMPTY;
    if (key == 6) /* UNIT ATTENTION: a disc went in, or was swapped */
        return CD_CHANGED;
    if (key == 2 && asc == 0x04) /* NOT READY, becoming ready */
        return CD_SPINNING;
    return CD_EMPTY; /* 3Ah: no medium */
}

bool cd_read(uint32_t lba, uint32_t count, void *buf) {
    if (!count || count > 16)
        return false;
    for (int tries = 0; tries < 3; tries++) {
        uint8_t pkt[12] = {0x28,
                           0, /* READ (10) */
                           (uint8_t)(lba >> 24),
                           (uint8_t)(lba >> 16),
                           (uint8_t)(lba >> 8),
                           (uint8_t)lba,
                           0,
                           (uint8_t)(count >> 8),
                           (uint8_t)count,
                           0};
        if (cd_packet(pkt, buf, count * CD_SECTOR, 10000) ==
            (int)(count * CD_SECTOR))
            return true;
        uint8_t key, asc;
        cd_sense(&key, &asc); /* clears it */
    }
    return false;
}

bool cd_capacity(uint32_t *sectors) {
    uint8_t pkt[12] = {0x25}, b[8]; /* READ CAPACITY */
    if (cd_packet(pkt, b, 8, 5000) != 8)
        return false;
    *sectors = ((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 |
                (uint32_t)b[2] << 8 | b[3]) +
               1;
    return true;
}

bool cd_eject(void) {
    uint8_t pkt[12] = {0x1B, 0, 0, 0, 0x02}; /* START STOP UNIT: eject */
    return cd_packet(pkt, NULL, 0, 5000) >= 0;
}
