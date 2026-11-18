/* Sim: an emulated floppy controller (NEC 765 / 82077 subset), DMA channel
 * 2 and drives A: and B:, so the real drivers/fdd.c runs against it.
 *
 * Drive: g_fdc_drive_type = CMOS type (1 360K, 2 1.2M, 3 720K, 4 1.44M,
 * 5 2.88M). Disk: g_fdc_disk_kb (0 = none) with its geometry; it holds
 * its data in g_fdc_img. What a real one does, and the driver must cope
 * with:
 *   - a seek takes a few SENSE INTERRUPT polls (0x80 until then);
 *   - after a reset, four pending "ready changed" interrupts;
 *   - reading needs the disk's data rate (CCR), the head on a track the
 *     disk has, and the C/H/R of an existing sector; a 40-track disk in an
 *     80-track drive is only at even head positions (double stepping);
 *   - FORMAT TRACK rewrites a track: the disk takes the new layout. */
#include "lib/core.h"
#include "lib/memory.h"
#include "lib/string.h"

int g_fdc_drive_type = 4;
int g_fdc_disk_kb = 1440;
bool g_fdc_wp;
uint8_t g_fdc_img[2880 * 512];
/* drive B: (type 0 = none) */
int g_fdcb_drive_type = 0;
int g_fdcb_disk_kb = 0;
uint8_t g_fdcb_img[2880 * 512];
int g_fdc_spinups;     /* motor off -> on, either drive */
int g_fdc_motor_fails; /* reads/writes with that drive's motor off */
int g_fdc_reads, g_fdc_writes, g_fdc_formats, g_fdc_seeks, g_fdc_errors;
int g_fdc_max_pcn;
extern unsigned char g_cmos[128];

/* sim_low stands in for the fixed low addresses (0x7000..) fdd.c uses */
__attribute__((aligned(65536))) uint8_t sim_low[0x6000]; /* 0x7000..0xD000 */
int g_fdc_ms; /* time the drive would take: seeks, turns of the disk */
static bool s_chg[2] = {true, true}; /* the disk change line, per drive */

typedef struct {
    int kb, spt, heads, tracks;
    int density; /* 0 DD, 1 HD, 2 ED */
} geo_t;
static const geo_t GEOS[] = {
    {160, 8, 1, 40, 0},   {180, 9, 1, 40, 0},   {320, 8, 2, 40, 0},
    {360, 9, 2, 40, 0},   {720, 9, 2, 80, 0},   {1200, 15, 2, 80, 1},
    {1440, 18, 2, 80, 1}, {2880, 36, 2, 80, 2},
};
static geo_t s_disks[2];
static int s_u; /* the drive of the command being run */
#define s_disk s_disks[s_u]
#define g_fdc_drive_type (*(s_u ? &g_fdcb_drive_type : &g_fdc_drive_type))
#define g_fdc_disk_kb (*(s_u ? &g_fdcb_disk_kb : &g_fdc_disk_kb))
#define g_fdc_img (s_u ? g_fdcb_img : g_fdc_img)

static const geo_t *geo_of(int kb) {
    for (unsigned i = 0; i < sizeof(GEOS) / sizeof(GEOS[0]); i++)
        if (GEOS[i].kb == kb)
            return &GEOS[i];
    return NULL;
}

void fdc_insert_unit(int u, int kb) {
    s_chg[u] = true; /* the door opened */
    s_u = u;
    const geo_t *g = geo_of(kb);
    g_fdc_disk_kb = g ? kb : 0;
    if (g)
        s_disk = *g;
    s_u = 0;
}
void fdc_insert(int kb) { fdc_insert_unit(0, kb); }

static int drive_tracks(void) { return g_fdc_drive_type == 1 ? 40 : 80; }
static bool drive_525(void) { return g_fdc_drive_type <= 2; }

/* the CCR a disk needs in this drive */
static int needed_rate(void) {
    if (s_disk.density == 2)
        return 3;
    if (s_disk.density == 1)
        return 0;
    return drive_525() && g_fdc_drive_type == 2 ? 1
                                                : 2; /* DD in a 1.2M: 300k */
}

/* ── controller state ── */
static uint8_t s_dor, s_ccr, s_pcns[2];
#define s_pcn s_pcns[s_u]
static uint8_t s_cmd[9], s_ncmd, s_need;
static uint8_t s_res[7], s_nres, s_rpos;
static int s_reset_ints;                 /* pending after a reset */
static int s_seek_int = -1, s_seek_wait; /* st0, polls left */
static int s_seek_u;
/* DMA channel 2 */
static uint16_t s_dma_addr, s_dma_count;
static uint8_t s_dma_page;
static bool s_ff;

static uint8_t *dma_ptr(void) {
    uint32_t full = ((uint32_t)(uintptr_t)sim_low & 0xFF000000u) |
                    (uint32_t)s_dma_page << 16 | s_dma_addr;
    return (uint8_t *)(uintptr_t)full;
}

static int params_for(uint8_t c) {
    switch (c & 0x1F) {
    case 0x03:
        return 3; /* SPECIFY */
    case 0x07:
        return 2; /* RECALIBRATE */
    case 0x0F:
        return 3; /* SEEK */
    case 0x08:
        return 1; /* SENSE INTERRUPT */
    case 0x06:
    case 0x05:
        return 9; /* READ / WRITE */
    case 0x04:
        return 2; /* SENSE DRIVE STATUS */
    case 0x0D:
        return 6; /* FORMAT */
    default:
        return 1;
    }
}

static void result7(uint8_t st0, uint8_t st1, uint8_t st2, uint8_t c, uint8_t h,
                    uint8_t r) {
    s_res[0] = st0, s_res[1] = st1, s_res[2] = st2;
    s_res[3] = c, s_res[4] = h, s_res[5] = r, s_res[6] = 2;
    s_nres = 7;
    s_rpos = 0;
    if (st0 & 0xC0)
        g_fdc_errors++;
}

/* the disk's cylinder under the head, or -1 */
static int cyl_under_head(void) {
    if (!g_fdc_disk_kb)
        return -1;
    if (s_disk.tracks == 40 && drive_tracks() == 80)
        return (s_pcn & 1) ? -1 : s_pcn / 2; /* double stepping */
    return s_pcn < s_disk.tracks ? s_pcn : -1;
}

static void do_rw(bool write) {
    uint8_t hd = s_cmd[1], c = s_cmd[2], h = s_cmd[3], r = s_cmd[4];
    uint8_t st0 = hd & 7;
    int cyl = cyl_under_head();
    if (!(s_dor & (0x10 << s_u))) { /* motor off: no index, no answer */
        g_fdc_motor_fails++;
        s_nres = 0;
        return;
    }
    if (!g_fdc_drive_type || !g_fdc_disk_kb) { /* no disk: never answers */
        s_nres = 0;
        return;
    }
    if (s_ccr != needed_rate() || cyl < 0 || (hd >> 2 & 1) >= s_disk.heads) {
        result7(st0 | 0x40, 0x01, 0, c, h, r); /* missing address mark */
        return;
    }
    if (c != cyl) {
        result7(st0 | 0x40, 0x04, 0x10, c, h, r); /* wrong cylinder */
        return;
    }
    if (r < 1 || r > s_disk.spt) {
        result7(st0 | 0x40, 0x04, 0, c, h, r); /* no data */
        return;
    }
    if (write && g_fdc_wp) {
        result7(st0 | 0x40, 0x02, 0, c, h, r);
        return;
    }
    uint32_t n = (uint32_t)s_dma_count + 1;
    uint8_t *mem = dma_ptr();
    g_fdc_ms += 100 + (int)(n / 512) * 200 /
                          s_disk.spt; /* half a turn, then the sectors */
    while (n >= 512 && r <= s_disk.spt) {
        uint32_t lba = ((uint32_t)c * s_disk.heads + h) * s_disk.spt + (r - 1);
        if (write)
            memcpy(g_fdc_img + lba * 512, mem, 512), g_fdc_writes++;
        else
            memcpy(mem, g_fdc_img + lba * 512, 512), g_fdc_reads++;
        mem += 512, n -= 512, r++;
    }
    result7(st0, 0, 0, c, h, r);
}

static void do_format(void) {
    uint8_t hd = s_cmd[1], sc = s_cmd[3], fill = s_cmd[5];
    uint8_t st0 = hd & 7;
    int head = hd >> 2 & 1;
    if (!g_fdc_disk_kb) {
        s_nres = 0;
        return;
    }
    if (g_fdc_wp) {
        result7(st0 | 0x40, 0x02, 0, 0, 0, 0);
        return;
    }
    const uint8_t *ids = dma_ptr();
    int cyl = ids[0];
    /* the disk takes the layout being written: density from the rate,
     * tracks from the stepping (in an 80-track drive: 80 until cylinder 1
     * comes at head position 2, double stepping; 300 kbps in a 1.2 MB
     * drive is both 360 KB and 720 KB) */
    if (s_pcn == 0 && head == 0) {
        s_disk.spt = sc;
        s_disk.heads = 1;
        s_disk.tracks = drive_tracks();
        s_disk.density = s_ccr == 3 ? 2 : s_ccr == 0 ? 1 : 0;
    }
    if (s_disk.tracks == 80 && drive_tracks() == 80 && cyl > 0 &&
        s_pcn == cyl * 2)
        s_disk.tracks = 40;
    if (head == 1)
        s_disk.heads = 2;
    if (cyl !=
        (s_disk.tracks == 40 && drive_tracks() == 80 ? s_pcn / 2 : s_pcn)) {
        result7(st0 | 0x40, 0x04, 0x10, 0, 0, 0);
        return;
    }
    for (int i = 0; i < sc; i++) {
        uint32_t lba = ((uint32_t)cyl * 2 + head) * sc + (ids[i * 4 + 2] - 1);
        (void)lba; /* the image is re-laid at the end (by heads) */
    }
    /* data of this track = filler (layout: heads known by the end; the
     * image is addressed with the final head count, so fill both) */
    for (int hh = head; hh <= head; hh++) {
        uint32_t base = ((uint32_t)cyl * 2 + hh) * sc;
        if ((base + sc) * 512 <= sizeof(g_fdc_img))
            memset(g_fdc_img + base * 512, fill, (uint32_t)sc * 512);
    }
    g_fdc_formats++;
    s_disk.kb = s_disk.spt * s_disk.heads * s_disk.tracks / 2;
    g_fdc_disk_kb = s_disk.kb;
    result7(st0, 0, 0, (uint8_t)cyl, (uint8_t)head, sc);
}

static void execute(void) {
    uint8_t c = s_cmd[0] & 0x1F;
    s_nres = s_rpos = 0;
    s_u = c == 0x08 || c == 0x03 ? s_u : s_cmd[1] & 1;
    switch (c) {
    case 0x03:
        break;
    case 0x07:
        g_fdc_ms += s_pcn * 3 + 15;
        if (g_fdc_disk_kb)
            s_chg[s_u] = false; /* a step with a disk in: the line drops */
        s_pcn = 0;
        s_seek_u = s_u;
        s_seek_int = 0x20 | (s_cmd[1] & 3);
        s_seek_wait = 3;
        g_fdc_seeks++;
        break;
    case 0x0F: {
        int max = drive_tracks() - 1;
        g_fdc_ms +=
            (s_cmd[2] > s_pcn ? s_cmd[2] - s_pcn : s_pcn - s_cmd[2]) * 3 + 15;
        if (g_fdc_disk_kb && s_cmd[2] != s_pcn)
            s_chg[s_u] = false;
        s_pcn = s_cmd[2] > max ? (uint8_t)max : s_cmd[2];
        if (s_pcn > g_fdc_max_pcn)
            g_fdc_max_pcn = s_pcn;
        s_seek_u = s_u;
        s_seek_int = 0x20 | (s_cmd[1] & 3);
        s_seek_wait = 3;
        g_fdc_seeks++;
        break;
    }
    case 0x08:
        if (s_reset_ints) {
            s_res[0] = (uint8_t)(0xC0 | (4 - s_reset_ints));
            s_res[1] = s_pcn;
            s_reset_ints--;
            s_nres = 2;
        } else if (s_seek_int >= 0 && --s_seek_wait <= 0) {
            s_res[0] = (uint8_t)s_seek_int;
            s_res[1] = s_pcns[s_seek_u];
            s_seek_int = -1;
            s_nres = 2;
        } else {
            s_res[0] = 0x80; /* nothing pending */
            s_nres = 1;
        }
        break;
    case 0x04: /* ST3: write protect, track 0, ready, the drive */
        s_res[0] = (uint8_t)((g_fdc_wp ? 0x40 : 0) | 0x20 |
                             (s_pcn == 0 ? 0x10 : 0) | (s_cmd[1] & 7));
        s_nres = 1;
        break;
    case 0x06:
        do_rw(false);
        break;
    case 0x05:
        do_rw(true);
        break;
    case 0x0D:
        do_format();
        break;
    default:
        s_res[0] = 0x80;
        s_nres = 1;
    }
    s_ncmd = 0;
}

/* port hooks, called from the sim's inb/outb */
bool fdc_port_out(uint16_t p, uint8_t v) {
    switch (p) {
    case 0x3F2:
        if (!(s_dor & 0x04) && (v & 0x04)) { /* out of reset */
            s_reset_ints = 4;
            s_seek_int = -1;
            s_ncmd = s_nres = 0;
        }
        for (int u = 0; u < 2; u++)
            if ((v & (0x10 << u)) && !(s_dor & (0x10 << u)))
                g_fdc_spinups++;
        s_dor = v;
        return true;
    case 0x3F5:
        if (s_nres > s_rpos)
            return true; /* results pending: ignored */
        if (!s_ncmd)
            s_need = (uint8_t)params_for(v);
        s_cmd[s_ncmd++] = v;
        if (s_ncmd >= s_need)
            execute();
        return true;
    case 0x3F7:
        s_ccr = v & 3;
        return true;
    case 0x0C:
        s_ff = false;
        return true;
    case 0x04:
        s_dma_addr = s_ff ? (uint16_t)((s_dma_addr & 0xFF) | v << 8)
                          : (uint16_t)((s_dma_addr & 0xFF00) | v);
        s_ff = !s_ff;
        return true;
    case 0x05:
        s_dma_count = s_ff ? (uint16_t)((s_dma_count & 0xFF) | v << 8)
                           : (uint16_t)((s_dma_count & 0xFF00) | v);
        s_ff = !s_ff;
        return true;
    case 0x81:
        s_dma_page = v;
        return true;
    case 0x0A:
    case 0x0B:
        return true;
    }
    return false;
}

bool fdc_port_in(uint16_t p, uint8_t *v) {
    switch (p) {
    case 0x3F4:
        *v = (uint8_t)(0x80 | (s_nres > s_rpos ? 0x50 : s_ncmd ? 0x10 : 0));
        return true;
    case 0x3F5:
        *v = s_nres > s_rpos ? s_res[s_rpos++] : 0;
        if (s_rpos >= s_nres)
            s_nres = s_rpos = 0;
        return true;
    case 0x3F7: /* DIR: bit 7 the selected drive's change line */
        *v = s_chg[s_dor & 1] ? 0x80 : 0;
        return true;
    }
    return false;
}

void fdc_setup(void) {
    s_u = 0;
    g_cmos[0x10] = (uint8_t)(g_fdc_drive_type << 4 | (g_fdcb_drive_type & 15));
}
