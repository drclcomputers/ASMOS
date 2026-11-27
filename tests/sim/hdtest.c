/* Tests the kernel's IDE hard disk driver (drivers/ata.c) on a model drive
 * (the primary master): several sectors per command, READ/WRITE MULTIPLE
 * and its data blocks, a drive without it, one whose multiple mode is
 * broken, CHS addressing, one flush per write. */
#include "drivers/ata.h"
#include "drivers/pci.h"
#include "lib/core.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "sys.h"

#define SPT 17
#define HEADS 4
#define NSEC (SPT * HEADS * 120) /* 8160 sectors, 4 MB */

static uint8_t disk[NSEC * 512];

/* the drive as it is made */
static int most_multi;    /* IDENTIFY word 47: 0 = no READ/WRITE MULTIPLE */
static bool has_lba;      /* else CHS only */
static bool multi_broken; /* READ/WRITE MULTIPLE abort */
static bool refuse_set;   /* SET MULTIPLE MODE aborts */

/* its registers */
static uint8_t r_err, r_cnt, r_lo, r_mid, r_hi, r_drv, r_stat = 0x50;
static int busy;
static int cur_multi;

/* a transfer */
static bool writing;
static uint8_t *dp;           /* where the data goes / comes from */
static uint32_t left;         /* sectors still to move */
static uint32_t per;          /* sectors per data request */
static uint32_t pos, blk_end; /* bytes into this transfer, this block's end */
static uint8_t idbuf[512];
static bool id_phase;

/* what the driver did */
static int cmds, multi_cmds, plain_cmds, flushes, blocks;

static bool master(void) { return !(r_drv & 0x10); }

static void abort_cmd(uint8_t err) {
    r_err = err;
    r_stat = 0x51; /* DRDY DSC ERR */
    busy = 2;
}
static void done(void) {
    r_stat = 0x50;
    busy = 2;
}
static void next_block(void) {
    uint32_t n = left < per ? left : per;
    blk_end = pos + n * 512;
    r_stat = 0x58; /* DRDY DSC DRQ */
    blocks++;
    busy = 2;
}

static uint32_t address(void) {
    if (r_drv & 0x40)
        return (uint32_t)(r_drv & 0x0F) << 24 | (uint32_t)r_hi << 16 |
               (uint32_t)r_mid << 8 | r_lo;
    uint32_t cyl = r_mid | (uint32_t)r_hi << 8;
    return (cyl * HEADS + (r_drv & 0x0F)) * SPT + r_lo - 1;
}

static void command(uint8_t v) {
    r_err = 0;
    if (v == 0xEC) { /* IDENTIFY */
        uint16_t *id = (uint16_t *)idbuf;
        memset(idbuf, 0, 512);
        id[1] = NSEC / (SPT * HEADS);
        id[3] = HEADS;
        id[6] = SPT;
        id[47] = (uint16_t)(0x8000 | most_multi);
        id[49] = has_lba ? 1 << 9 : 0;
        id[60] = has_lba ? NSEC & 0xFFFF : 0;
        id[61] = has_lba ? NSEC >> 16 : 0;
        const char *m = "TEST DISK                               ";
        for (int i = 0; i < 20; i++)
            id[27 + i] = (uint16_t)(m[2 * i] << 8 | m[2 * i + 1]);
        id_phase = true;
        pos = 0;
        blk_end = 512;
        r_stat = 0x58;
        busy = 2;
        return;
    }
    if (v == 0xC6) { /* SET MULTIPLE MODE */
        int n = r_cnt;
        if (refuse_set || !most_multi || n > most_multi || (n & (n - 1))) {
            abort_cmd(4);
            return;
        }
        cur_multi = n;
        done();
        return;
    }
    if (v == 0xE7) { /* FLUSH CACHE */
        flushes++;
        done();
        return;
    }
    bool multi = v == 0xC4 || v == 0xC5;
    if (v != 0x20 && v != 0x30 && !multi) {
        abort_cmd(4);
        return;
    }
    cmds++;
    if (multi && (multi_broken || !cur_multi)) {
        abort_cmd(4);
        return;
    }
    multi_cmds += multi;
    plain_cmds += !multi;
    uint32_t lba = address(), n = r_cnt ? r_cnt : 256;
    if (lba + n > NSEC) {
        abort_cmd(0x10); /* IDNF: no such sector */
        return;
    }
    writing = v == 0x30 || v == 0xC5;
    dp = disk + lba * 512;
    left = n;
    per = multi ? (uint32_t)cur_multi : 1;
    pos = 0;
    id_phase = false;
    next_block();
}

/* a block moved: the next, or done */
static void block_moved(void) {
    uint32_t n = left < per ? left : per;
    left -= n;
    if (left)
        next_block();
    else
        done();
}

unsigned char inb(unsigned short p) {
    if ((p >= 0x170 && p <= 0x177) || p == 0x376)
        return 0xFF; /* no secondary channel */
    if (!master() && p != 0x1F6)
        return 0x00; /* no slave */
    switch (p) {
    case 0x1F1:
        return r_err;
    case 0x1F2:
        return r_cnt;
    case 0x1F3:
        return r_lo;
    case 0x1F4:
        return 0; /* (a disk's signature: not ATAPI) */
    case 0x1F5:
        return 0;
    case 0x1F6:
        return r_drv;
    case 0x1F7:
    case 0x3F6:
        if (busy > 0) {
            busy--;
            return 0x80;
        }
        return r_stat;
    }
    return 0xFF;
}

unsigned short inw(unsigned short p) {
    if (p != 0x1F0 || !master() || !(r_stat & 0x08) || busy)
        return 0xFFFF;
    if (id_phase) {
        uint16_t w = (uint16_t)(idbuf[pos] | idbuf[pos + 1] << 8);
        pos += 2;
        if (pos >= 512)
            done();
        return w;
    }
    if (writing)
        return 0xFFFF;
    uint16_t w = (uint16_t)(dp[pos] | dp[pos + 1] << 8);
    pos += 2;
    if (pos >= blk_end)
        block_moved();
    return w;
}

void outw(unsigned short p, unsigned short v) {
    if (p != 0x1F0 || !master() || !writing || !(r_stat & 0x08) || busy)
        return;
    dp[pos] = (uint8_t)v;
    dp[pos + 1] = (uint8_t)(v >> 8);
    pos += 2;
    if (pos >= blk_end)
        block_moved();
}

void insw(unsigned short p, void *buf, uint32_t n) {
    uint16_t *w = buf;
    while (n--)
        *w++ = inw(p);
}
void outsw(unsigned short p, const void *buf, uint32_t n) {
    const uint16_t *w = buf;
    while (n--)
        outw(p, *w++);
}

void outb(unsigned short p, unsigned char v) {
    if (p == 0x1F6) {
        r_drv = v;
        return;
    }
    if (!master())
        return;
    switch (p) {
    case 0x1F2:
        r_cnt = v;
        break;
    case 0x1F3:
        r_lo = v;
        break;
    case 0x1F4:
        r_mid = v;
        break;
    case 0x1F5:
        r_hi = v;
        break;
    case 0x3F6:
        if (v & 4) { /* SRST */
            r_stat = 0x50;
            busy = 2;
        }
        break;
    case 0x1F7:
        command(v);
        break;
    }
}

static uint32_t clock;
uint32_t time_millis(void) { return clock++ / 64; }
int pci_mechanism(void) { return 0; }
const pci_dev_t *pci_find_class(uint8_t a, uint8_t b, int n) {
    (void)a, (void)b, (void)n;
    return NULL;
}
uint16_t pci_bar_io(const pci_dev_t *d, int i) {
    (void)d, (void)i;
    return 0;
}
void pci_enable(const pci_dev_t *d, bool a, bool b, bool c) {
    (void)d, (void)a, (void)b, (void)c;
}
void *kmalloc(uint32_t n) {
    (void)n;
    return NULL;
}

static void say(const char *s) { sys3(SYS_write, 1, (int)s, (int)strlen(s)); }
static int fails;
static void T(const char *what, bool ok) {
    char b[100];
    snprintf(b, sizeof(b), "  %-50s %s\n", what, ok ? "ok" : "FAIL");
    say(b);
    fails += !ok;
}

static uint8_t pat(uint32_t i, int seed) {
    return (uint8_t)(i * 13 + (i >> 9) * 7 + seed);
}
static void fill(int seed) {
    for (uint32_t i = 0; i < sizeof(disk); i++)
        disk[i] = pat(i, seed);
}
static bool same(const uint8_t *b, uint32_t lba, uint32_t n, int seed) {
    for (uint32_t i = 0; i < n * 512; i++)
        if (b[i] != pat(lba * 512 + i, seed))
            return false;
    return true;
}

/* a drive: how it is made; then the driver finds it */
static void drive(int most, bool lba, bool broken, bool refuse) {
    most_multi = most;
    has_lba = lba;
    multi_broken = broken;
    refuse_set = refuse;
    cur_multi = 0;
    r_stat = 0x50;
    busy = 0;
    ata_init(0x80);
    cmds = multi_cmds = plain_cmds = flushes = blocks = 0;
}

static void counts(const char *what) {
    char b[120];
    snprintf(b, sizeof(b),
             "    %s: %d commands (%d multiple), %d data blocks, %d flushes\n",
             what, cmds, multi_cmds, blocks, flushes);
    say(b);
    cmds = multi_cmds = plain_cmds = flushes = blocks = 0;
}

int sim_main(void) {
    static uint8_t buf[255 * 512];

    /* a drive with READ/WRITE MULTIPLE, 16 sectors a block */
    fill(1);
    drive(16, true, false, false);
    const ata_disk_t *d = ata_unit(0);
    T("found the disk", d && d->present && d->sectors == NSEC);
    T("multiple mode: 16 sectors a block", d && d->multi == 16);
    cmds = multi_cmds = blocks = 0;
    T("read 40 sectors", ata_read_sectors(0, 100, 40, buf));
    T("... the right bytes", same(buf, 100, 40, 1));
    T("... one READ MULTIPLE, 3 blocks (16+16+8)",
      cmds == 1 && multi_cmds == 1 && blocks == 3);
    counts("40 sectors read");
    T("read 1 sector", ata_read_sector(0, 7, buf) && same(buf, 7, 1, 1));
    counts("1 sector read");
    T("read 255 sectors: 2 commands (128 + 127)",
      ata_read_sectors(0, 1000, 255, buf) && same(buf, 1000, 255, 1) &&
          cmds == 2);
    counts("255 sectors read");
    for (uint32_t i = 0; i < 40 * 512; i++)
        buf[i] = pat((3000 + 0) * 512 + i, 2);
    T("write 40 sectors", ata_write_sectors(0, 3000, 40, buf));
    T("... one WRITE MULTIPLE, one flush",
      cmds == 1 && multi_cmds == 1 && flushes == 1);
    T("... on the disk", same(disk + 3000 * 512, 3000, 40, 2) &&
                             same(disk + 3040 * 512, 3040, 1, 1) &&
                             same(disk + 2999 * 512, 2999, 1, 1));
    counts("40 sectors written");
    T("past the end: refused", !ata_read_sectors(0, NSEC - 10, 20, buf));
    T("... multiple mode still on (the sectors failed)", d->multi == 16);
    T("the last sectors",
      ata_read_sectors(0, NSEC - 10, 10, buf) && same(buf, NSEC - 10, 10, 1));

    /* the most it takes: 8; 32 (16 is used) */
    drive(8, true, false, false);
    T("a drive taking 8: 8 a block", ata_unit(0)->multi == 8);
    drive(32, true, false, false);
    T("a drive taking 32: 16 a block", ata_unit(0)->multi == 16);

    /* no READ/WRITE MULTIPLE: READ SECTORS with a count, a block each */
    fill(3);
    drive(0, true, false, false);
    T("no multiple mode", ata_unit(0)->multi == 0);
    T("read 40 sectors",
      ata_read_sectors(0, 500, 40, buf) && same(buf, 500, 40, 3));
    T("... still one command (40 blocks)",
      cmds == 1 && plain_cmds == 1 && blocks == 40);
    counts("40 sectors read, no multiple");
    T("write 20: one command, one flush",
      ata_write_sectors(0, 600, 20, disk + 600 * 512) && cmds == 1 &&
          flushes == 1);

    /* SET MULTIPLE MODE refused: not used */
    drive(16, true, false, true);
    T("SET MULTIPLE refused: plain commands",
      ata_unit(0)->multi == 0 && ata_read_sectors(0, 9, 30, buf) &&
          same(buf, 9, 30, 3));

    /* multiple mode set, but READ/WRITE MULTIPLE fail: the plain ones */
    drive(16, true, true, false);
    T("broken multiple mode: still read",
      ata_read_sectors(0, 300, 33, buf) && same(buf, 300, 33, 3));
    T("... and not used again", ata_unit(0)->multi == 0 &&
                                    ata_read_sectors(0, 1, 2, buf) &&
                                    multi_cmds == 0 && plain_cmds == 2);

    /* CHS only: a run over the end of a track and a head */
    fill(4);
    drive(16, false, false, false);
    d = ata_unit(0);
    T("a CHS disk", d && !d->lba && d->spt == SPT && d->heads == HEADS);
    uint32_t from = SPT * HEADS - 5; /* the last head's track, then on */
    T("read over a cylinder's end",
      ata_read_sectors(0, from, 30, buf) && same(buf, from, 30, 4));
    for (uint32_t i = 0; i < 30 * 512; i++)
        buf[i] = pat(from * 512 + i, 5);
    T("write there", ata_write_sectors(0, from, 30, buf) &&
                         same(disk + from * 512, from, 30, 5) &&
                         same(disk + (from + 30) * 512, from + 30, 1, 4));

    say(fails ? "SOME FAILED\n" : "all ok\n");
    return fails;
}
