/* drivers/ata.c's CD-ROM part as the kernel has it (the simulator has its
 * own CD drive): against a model of an ATAPI drive on the I/O ports, with
 * the test disc fixtures/cd.iso. Built and run by run.sh.
 *
 * The model: nothing on the primary cable (a floating bus reads FFh), the
 * CD drive as secondary master (170h/376h), no slave. It answers IDENTIFY
 * with an abort and the ATAPI signature, IDENTIFY PACKET DEVICE, and the
 * packets TEST UNIT READY, REQUEST SENSE, READ (10), READ CAPACITY and
 * START STOP UNIT; data comes in pieces of at most 8 KB (several DRQ
 * phases a command), and BSY stays up for a few status reads after each
 * command. */
#include "drivers/ata.h"
#include "drivers/cdrom.h"
#include "drivers/pci.h"
#include "lib/core.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "sys.h"

bool load_file(const char *path, uint8_t *dst, uint32_t max, uint32_t *got) {
    int fd = sys3(SYS_open, (int)path, 0, 0);
    if (fd < 0)
        return false;
    uint32_t n = 0;
    int r;
    while (n < max &&
           (r = sys3(SYS_read, fd, (int)(dst + n), (int)(max - n))) > 0)
        n += (uint32_t)r;
    sys3(SYS_close, fd, 0, 0);
    *got = n;
    return true;
}

static uint8_t *disc;
static uint32_t disc_sectors;
static bool inserted;

/* the drive */
static uint8_t r_err, r_cnt, r_lo, r_mid, r_hi, r_drv, r_stat = 0x00;
static int busy; /* status reads still BSY */
static uint8_t data[40000];
static uint32_t d_len, d_pos, piece_end;
static uint8_t pkt[12];
static int pkt_n = -1; /* -1: not taking a packet */
static uint8_t sense_key, sense_asc;
static bool attention;
static uint32_t limit;
static bool audio_disc;
static uint32_t play_from, play_to;
static uint8_t play_status = 0x15; /* none */
static int reads_started;

static void set_sense(uint8_t k, uint8_t a) { sense_key = k, sense_asc = a; }
static void fail(uint8_t k, uint8_t a) {
    set_sense(k, a);
    r_stat = 0x41; /* DRDY ERR */
    r_err = (uint8_t)(k << 4);
    busy = 2;
}
static void give(uint32_t len) { /* data ready to be read */
    d_len = len;
    d_pos = 0;
    uint32_t n = len - d_pos;
    if (n > limit)
        n = limit;
    if (n > 8192)
        n = 8192;
    piece_end = n;
    r_mid = (uint8_t)n, r_hi = (uint8_t)(n >> 8);
    r_stat = 0x58; /* DRDY DSC DRQ */
    busy = 3;
}
static void done(void) {
    r_stat = 0x50;
    busy = 2;
}

static void run_packet(void) {
    pkt_n = -1;
    uint8_t op = pkt[0];
    if (op == 0x03) { /* REQUEST SENSE */
        memset(data, 0, 18);
        data[0] = 0x70;
        data[2] = sense_key;
        data[7] = 10;
        data[12] = sense_asc;
        set_sense(0, 0);
        give(18);
        return;
    }
    if (!inserted) {
        fail(2, 0x3A); /* NOT READY, no medium */
        return;
    }
    if (attention) {
        attention = false;
        fail(6, 0x28); /* UNIT ATTENTION: the disc changed */
        return;
    }
    if (op == 0x00) { /* TEST UNIT READY */
        done();
        return;
    }
    if (op == 0x25) { /* READ CAPACITY */
        uint32_t last = disc_sectors - 1;
        data[0] = (uint8_t)(last >> 24), data[1] = (uint8_t)(last >> 16);
        data[2] = (uint8_t)(last >> 8), data[3] = (uint8_t)last;
        data[4] = 0, data[5] = 0, data[6] = 8, data[7] = 0;
        give(8);
        return;
    }
    if (op == 0x28) { /* READ (10) */
        uint32_t lba = (uint32_t)pkt[2] << 24 | (uint32_t)pkt[3] << 16 |
                       (uint32_t)pkt[4] << 8 | pkt[5];
        uint32_t n = (uint32_t)pkt[7] << 8 | pkt[8];
        if (lba + n > disc_sectors || n * 2048 > sizeof(data)) {
            fail(5, 0x21); /* ILLEGAL REQUEST: past the end */
            return;
        }
        memcpy(data, disc + lba * 2048, n * 2048);
        reads_started++;
        give(n * 2048);
        return;
    }
    if (op == 0x43) { /* READ TOC (LBA form): the data disc, or 2 audio
                       * tracks of 1000 and 2000 sectors */
        int nt = audio_disc ? 2 : 1;
        uint32_t lba[3] = {0, 1000, 3000};
        memset(data, 0, 4 + 8 * (nt + 1));
        data[1] = (uint8_t)(2 + 8 * (nt + 1));
        data[2] = 1, data[3] = (uint8_t)nt;
        for (int i = 0; i <= nt; i++) {
            uint8_t *d = data + 4 + 8 * i;
            d[1] = audio_disc ? 0x10 : 0x14; /* ADR 1; data: control 4 */
            d[2] = i == nt ? 0xAA : (uint8_t)(i + 1);
            uint32_t v = i == nt && !audio_disc ? disc_sectors : lba[i];
            d[4] = (uint8_t)(v >> 24), d[5] = (uint8_t)(v >> 16);
            d[6] = (uint8_t)(v >> 8), d[7] = (uint8_t)v;
        }
        uint32_t want = (uint32_t)pkt[7] << 8 | pkt[8], n = 4 + 8 * (nt + 1);
        give(n < want ? n : want);
        return;
    }
    if (op == 0x47) { /* PLAY AUDIO MSF */
        uint32_t a = (pkt[3] * 60u + pkt[4]) * 75u + pkt[5] - 150,
                 b = (pkt[6] * 60u + pkt[7]) * 75u + pkt[8] - 150;
        if (!audio_disc || a >= b || b > 3000) {
            fail(5, 0x64); /* ILLEGAL MODE FOR THIS TRACK */
            return;
        }
        play_from = a, play_to = b, play_status = 0x11;
        done();
        return;
    }
    if (op == 0x4B) { /* PAUSE / RESUME */
        if (play_status != 0x11 && play_status != 0x12) {
            fail(5, 0x2C); /* sequence error */
            return;
        }
        play_status = pkt[8] & 1 ? 0x11 : 0x12;
        done();
        return;
    }
    if (op == 0x4E) { /* STOP PLAY/SCAN */
        play_status = 0x15;
        done();
        return;
    }
    if (op == 0x42) { /* READ SUB-CHANNEL, current position */
        memset(data, 0, 16);
        data[1] = play_status;
        data[3] = 12;
        data[4] = 1;
        uint32_t at = play_from + 10; /* (it moved a little) */
        data[8] = (uint8_t)(at >> 24), data[9] = (uint8_t)(at >> 16);
        data[10] = (uint8_t)(at >> 8), data[11] = (uint8_t)at;
        give(16);
        return;
    }
    if (op == 0x1B) { /* START STOP UNIT */
        if (pkt[4] & 2)
            inserted = false;
        done();
        return;
    }
    fail(5, 0x20); /* invalid command */
}

static bool master(void) { return !(r_drv & 0x10); }

unsigned char inb(unsigned short p) {
    if (p >= 0x1F0 && p <= 0x1F7)
        return 0xFF;
    if (p == 0x3F6)
        return 0xFF;
    if (!master() && p != 0x176)
        return 0x00; /* no slave: nothing holds what is written */
    switch (p) {
    case 0x171:
        return r_err;
    case 0x172:
        return r_cnt;
    case 0x173:
        return r_lo;
    case 0x174:
        return r_mid;
    case 0x175:
        return r_hi;
    case 0x176:
        return r_drv;
    case 0x177:
    case 0x376:
        if (busy > 0) {
            busy--;
            return 0x80;
        }
        return r_stat;
    }
    return 0xFF;
}

unsigned short inw(unsigned short p) {
    if (p != 0x170 || !master() || !(r_stat & 0x08))
        return 0xFFFF;
    uint16_t w = (uint16_t)(data[d_pos] | data[d_pos + 1] << 8);
    d_pos += 2;
    if (d_pos >= piece_end) { /* this piece read */
        if (d_pos >= d_len) {
            done();
        } else {
            uint32_t n = d_len - d_pos;
            if (n > limit)
                n = limit;
            if (n > 8192)
                n = 8192;
            piece_end = d_pos + n;
            r_mid = (uint8_t)n, r_hi = (uint8_t)(n >> 8);
            busy = 2;
        }
    }
    return w;
}

void outb(unsigned short p, unsigned char v) {
    if (p == 0x176) {
        r_drv = v;
        return;
    }
    if (!master())
        return;
    switch (p) {
    case 0x171:
        break;
    case 0x172:
        r_cnt = v;
        break;
    case 0x173:
        r_lo = v;
        break;
    case 0x174:
        r_mid = v;
        break;
    case 0x175:
        r_hi = v;
        break;
    case 0x376:
        if (v & 4) { /* SRST: the signature */
            r_cnt = 1, r_lo = 1, r_mid = 0x14, r_hi = 0xEB, r_stat = 0;
            busy = 2;
        }
        break;
    case 0x177:
        if (v == 0xEC) { /* IDENTIFY: not for ATAPI */
            r_mid = 0x14, r_hi = 0xEB, r_err = 4, r_stat = 0x41;
        } else if (v == 0xA1) {
            uint16_t id[256];
            memset(id, 0, sizeof(id));
            id[0] = 0x8580; /* ATAPI, CD-ROM, 12-byte packets */
            const char *m = "TEST CDROM                              ";
            for (int i = 0; i < 20; i++)
                id[27 + i] = (uint16_t)(m[2 * i] << 8 | m[2 * i + 1]);
            memcpy(data, id, 512);
            limit = 512;
            give(512);
        } else if (v == 0xA0) {
            limit = (uint32_t)r_mid | (uint32_t)r_hi << 8;
            pkt_n = 0;
            r_stat = 0x58;
            busy = 1;
        } else {
            r_err = 4, r_stat = 0x41;
        }
        break;
    }
}

void outw(unsigned short p, unsigned short v) {
    if (p != 0x170 || pkt_n < 0)
        return;
    pkt[pkt_n++] = (uint8_t)v;
    pkt[pkt_n++] = (uint8_t)(v >> 8);
    if (pkt_n == 12)
        run_packet();
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

static void say(const char *s) { sys3(SYS_write, 1, (int)s, (int)strlen(s)); }
static int fails;
static void T(const char *what, bool ok) {
    char b[100];
    snprintf(b, sizeof(b), "  %-46s %s\n", what, ok ? "ok" : "FAIL");
    say(b);
    fails += !ok;
}

int sim_main(void) {
    disc = (uint8_t *)sys6(SYS_mmap2, 0x50000000, 8 << 20, 3, 0x32, -1, 0);
    uint32_t got = 0;
    if (!load_file("fixtures/cd.iso", disc, 8 << 20, &got)) {
        say("no fixtures/cd.iso\n");
        return 1;
    }
    disc_sectors = got / 2048;
    int hd = ata_init(0x80);
    T("no hard disks", hd == 0 && !ata_unit(0));
    T("the CD drive found", cd_present());
    T("its model", !strcmp(cd_model(), "TEST CDROM"));
    T("no disc: empty", cd_check() == CD_EMPTY);
    inserted = true;
    attention = true;
    T("a disc put in: changed", cd_check() == CD_CHANGED);
    T("then ready", cd_check() == CD_READY);
    uint32_t n = 0;
    T("its size", cd_capacity(&n) && n == disc_sectors);
    static uint8_t buf[16 * 2048];
    bool ok = true;
    for (uint32_t lba = 0; lba + 16 <= disc_sectors; lba += 13) {
        if (!cd_read(lba, 1, buf) || memcmp(buf, disc + lba * 2048, 2048))
            ok = false;
        if (!cd_read(lba, 16, buf) || memcmp(buf, disc + lba * 2048, 16 * 2048))
            ok = false;
    }
    T("reads (1 and 16 sectors, 8 KB pieces)", ok);
    T("past the end: refused", !cd_read(disc_sectors - 1, 2, buf));
    T("ready again after that", cd_check() == CD_READY);
    /* the TOC of the data disc: one data track */
    cd_toc_t toc;
    T("TOC: one data track", cd_read_toc(&toc) && toc.first == 1 &&
                                 toc.last == 1 && toc.t[1].data &&
                                 toc.t[2].lba == disc_sectors);
    T("play on a data disc: refused",
      !cd_play(0, 100) && cd_check() == CD_READY);
    /* an audio disc */
    audio_disc = true;
    T("TOC: 2 audio tracks", cd_read_toc(&toc) && toc.last == 2 &&
                                 !toc.t[1].data && toc.t[2].lba == 1000 &&
                                 toc.t[3].lba == 3000);
    T("play track 2 (MSF)",
      cd_play(1000, 3000) && play_from == 1000 && play_to == 3000);
    uint32_t at = 0;
    T("its position", cd_audio_status(&at) == CD_AUDIO_PLAYING && at == 1010);
    T("pause", cd_pause(true) && cd_audio_status(NULL) == CD_AUDIO_PAUSED);
    T("resume", cd_pause(false) && cd_audio_status(NULL) == CD_AUDIO_PLAYING);
    T("stop", cd_stop_audio() && cd_audio_status(NULL) == CD_AUDIO_NONE);
    T("past the end: refused", !cd_play(2000, 3100));
    audio_disc = false;
    T("eject", cd_eject() && cd_check() == CD_EMPTY);
    T("reading with no disc fails", !cd_read(16, 1, buf));
    say(fails ? "SOME FAILED\n" : "all ok\n");
    return fails;
}

void *kmalloc(uint32_t n) {
    (void)n;
    return NULL;
} /* (lib/string.c links it) */
