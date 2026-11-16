#include "fs/fs.h"
#include "drivers/ata.h"
#include "drivers/fdd.h"
#include "fs/fat_io.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"

fat_vol_t g_drives[DRIVE_COUNT];
fat_dir_ctx_t dir_context = {DRIVE_HDA, 0, "/"};

static uint8_t s_current_drive = DRIVE_HDA;

vfs_mount_t g_vfs_mounts[VFS_MOUNT_COUNT] = {
    {"/HDB", DRIVE_HDB},
    {"/FDD0", DRIVE_FDD0},
    {"/FDD1", DRIVE_FDD1},
};

const char *g_protected_paths[PROTECTED_PATH_COUNT] = {
    "DESKTOP", "/DESKTOP", "HDB", "FDD0", "FDD1", "TRASH", "/TRASH",
};

const char *g_drive_paths[DRIVE_COUNT] = {
    [DRIVE_HDA] = "/HDA",
    [DRIVE_HDB] = "/HDB",
    [DRIVE_FDD0] = "/FDD0",
    [DRIVE_FDD1] = "/FDD1",
};

bool path_is_protected(const char *name_or_path) {
    if (!name_or_path)
        return false;
    for (int i = 0; i < PROTECTED_PATH_COUNT; i++)
        if (strcasecmp(name_or_path, g_protected_paths[i]) == 0)
            return true;
    for (int i = 0; i < VFS_MOUNT_COUNT; i++) {
        if (strcasecmp(name_or_path, g_vfs_mounts[i].path) == 0)
            return true;
        if (strcasecmp(name_or_path, g_vfs_mounts[i].path + 1) == 0)
            return true;
    }
    return false;
}

/* Floppies: a small write-through cache of recent sectors. The FAT and the
 * folders are read again and again (each cluster of a file looks up the
 * FAT), and a floppy read costs a seek and up to a turn of the disk.
 * Emptied when a floppy is mounted or unmounted. */
#define FCACHE_N 16
static struct {
    uint32_t lba, used;
    uint8_t drive; /* DRIVE_FDD0/1; 0 = empty slot */
    uint8_t data[512];
} s_fcache[FCACHE_N];
static uint32_t s_fcache_clock;

static int fcache_find(uint8_t drive_id, uint32_t lba) {
    for (int i = 0; i < FCACHE_N; i++)
        if (s_fcache[i].drive == drive_id && s_fcache[i].lba == lba)
            return i;
    return -1;
}

static void fcache_put(uint8_t drive_id, uint32_t lba, const void *buf) {
    int k = fcache_find(drive_id, lba);
    if (k < 0) { /* the empty or least recently used slot */
        k = 0;
        for (int i = 1; i < FCACHE_N && s_fcache[k].drive; i++)
            if (!s_fcache[i].drive || s_fcache[i].used < s_fcache[k].used)
                k = i;
    }
    s_fcache[k].drive = drive_id;
    s_fcache[k].lba = lba;
    s_fcache[k].used = ++s_fcache_clock;
    memcpy(s_fcache[k].data, buf, 512);
}

static void fcache_drop(uint8_t drive_id) {
    for (int i = 0; i < FCACHE_N; i++)
        if (s_fcache[i].drive == drive_id)
            s_fcache[i].drive = 0;
}

bool fs_read_sector(uint8_t drive_id, uint32_t lba, void *buf) {
    switch (drive_id) {
    case DRIVE_HDA:
    case DRIVE_HDB:
        return ata_read_sector(drive_id, lba, buf);
    case DRIVE_FDD0:
    case DRIVE_FDD1: {
        int k = fcache_find(drive_id, lba);
        if (k >= 0) {
            memcpy(buf, s_fcache[k].data, 512);
            s_fcache[k].used = ++s_fcache_clock;
            return true;
        }
        if (!fdd_read_sector(drive_id == DRIVE_FDD0 ? 0 : 1, lba, buf))
            return false;
        fcache_put(drive_id, lba, buf);
        return true;
    }
    default:
        return false;
    }
}

bool fs_write_sector(uint8_t drive_id, uint32_t lba, const void *buf) {
    switch (drive_id) {
    case DRIVE_HDA:
    case DRIVE_HDB:
        return ata_write_sector(drive_id, lba, buf);
    case DRIVE_FDD0:
    case DRIVE_FDD1: {
        bool ok = fdd_write_sector(drive_id == DRIVE_FDD0 ? 0 : 1, lba, buf);
        if (ok)
            fcache_put(drive_id, lba, buf);
        else { /* what's on the disk is unknown now */
            int k = fcache_find(drive_id, lba);
            if (k >= 0)
                s_fcache[k].drive = 0;
        }
        return ok;
    }
    default:
        return false;
    }
}

static fat_vol_t *vol(uint8_t drive_id) { return &g_drives[drive_id]; }

static uint16_t now_fat_time(void) { return time_from_rtc(); }
static uint16_t now_fat_date(void) { return date_from_rtc(); }

static bool name83_eq(const uint8_t *entry_name, const char *name83) {
    for (int i = 0; i < 11; i++)
        if (entry_name[i] != (uint8_t)name83[i])
            return false;
    return true;
}

static void zero_cluster(uint8_t drive_id, uint16_t cluster) {
    fat_vol_t *v = vol(drive_id);
    if (!vol_cluster_active(v, cluster))
        return;
    uint32_t lba = fat_cluster_to_lba(drive_id, cluster);
    uint8_t zero[512];
    memset(zero, 0, 512);
    for (uint32_t s = 0; s < v->bpb.sectors_per_cluster; s++)
        fs_write_sector(drive_id, lba + s, zero);
}

/* ── directory walking, with long file names (Windows 95 and later) ──────
 * Before its 8.3 entry, a file with a long name has entries with attr 0x0F
 * holding the name in 13-character pieces, last piece first, each with a
 * checksum of the 8.3 name. DOS skips them and sees only the 8.3 name (the
 * "alias", like LONGFI~1.TXT); we read them, find files by either name, and
 * write them for names that don't fit 8.3. Names are kept up to FS_NAME_MAX
 * characters: a longer one on a disk from elsewhere shows as its alias.
 * Characters outside ASCII show as '_'. */
#define ATTR_LFN 0x0F
#define LFN_MAX 20 /* 20 x 13 = 260 characters: the longest name on disk */
#define CASE_LOWER_BASE 0x08 /* Windows NT: the 8.3 name is lower case */
#define CASE_LOWER_EXT 0x10

static bool is_lfn(const dir_entry_t *e) {
    return (e->attr & 0x3F) == ATTR_LFN && e->name[0] != DIR_ENTRY_FREE;
}

static uint8_t lfn_checksum(const uint8_t name11[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name11[i]);
    return sum;
}

/* where the 13 UCS-2 characters sit in a long-name entry */
static const uint8_t k_lfn_off[13] = {1,  3,  5,  7,  9,  14, 16,
                                      18, 20, 22, 24, 28, 30};

typedef struct {
    char name[FS_NAME_SZ];
    uint8_t sum;
    uint8_t expect; /* the next piece expected (counts down to 1); 0: none */
    bool done;      /* all pieces seen, down to piece 1 */
    bool too_long;
} lfn_acc;

static void lfn_reset(lfn_acc *a) {
    a->expect = 0;
    a->done = false;
}

static void lfn_feed(lfn_acc *a, const dir_entry_t *e) {
    const uint8_t *r = (const uint8_t *)e;
    uint8_t ord = r[0];
    if (ord & 0x40) { /* the last piece: comes first */
        int n = ord & 0x3F;
        if (n == 0 || n > LFN_MAX) {
            lfn_reset(a);
            return;
        }
        memset(a->name, 0, sizeof(a->name));
        a->sum = r[13];
        a->expect = (uint8_t)n;
        a->too_long = false;
        a->done = false;
    } else if (!a->expect || ord != a->expect || r[13] != a->sum) {
        lfn_reset(a); /* out of order: not a name */
        return;
    }
    int base = (a->expect - 1) * 13;
    for (int k = 0; k < 13; k++) {
        uint16_t c = (uint16_t)(r[k_lfn_off[k]] | (r[k_lfn_off[k] + 1] << 8));
        if (c == 0x0000 || c == 0xFFFF)
            break;
        int pos = base + k;
        if (pos >= FS_NAME_MAX) {
            a->too_long = true;
            continue;
        }
        a->name[pos] = (c >= 0x20 && c < 0x7F) ? (char)c : '_';
    }
    a->expect--;
    a->done = a->expect == 0;
}

/* the long name of the 8.3 entry e, if the pieces before it were its own */
static const char *lfn_take(lfn_acc *a, const dir_entry_t *e) {
    bool ok = a->done && !a->too_long && a->name[0] &&
              a->sum == lfn_checksum((const uint8_t *)e);
    lfn_reset(a);
    return ok ? a->name : NULL;
}

/* the 8.3 name as text: "README.TXT" (lower case if Windows NT said so) */
void fs_entry_name(const dir_entry_t *e, char out[13]) {
    int j = 0;
    bool lb = e->reserved & CASE_LOWER_BASE, le = e->reserved & CASE_LOWER_EXT;
    for (int i = 0; i < 8 && e->name[i] != ' '; i++) {
        char c = (char)(i == 0 && e->name[0] == 0x05 ? 0xE5 : e->name[i]);
        out[j++] = lb ? (char)tolower(c) : c;
    }
    if (e->ext[0] != ' ') {
        out[j++] = '.';
        for (int i = 0; i < 3 && e->ext[i] != ' '; i++)
            out[j++] = le ? (char)tolower(e->ext[i]) : (char)e->ext[i];
    }
    out[j] = '\0';
}

/* Called for every file and folder (not '.'-skipping; not the volume label)
 * with its long name, or NULL. Return false to stop. */
typedef bool (*dir_cb)(uint8_t drive_id, dir_entry_t *entry, const char *lname,
                       uint32_t lba, int idx, void *user);

/* 1: go on; 0: the end of the folder; -1: stopped or a read failed */
static int scan_sector(uint8_t drive_id, uint32_t lba, lfn_acc *a, dir_cb cb,
                       void *user) {
    uint8_t buf[512];
    if (!fs_read_sector(drive_id, lba, buf))
        return -1;
    dir_entry_t *e = (dir_entry_t *)buf;
    for (int i = 0; i < 16; i++) {
        if (e[i].name[0] == DIR_ENTRY_END)
            return 0;
        if (e[i].name[0] == DIR_ENTRY_FREE) {
            lfn_reset(a);
            continue;
        }
        if (is_lfn(&e[i])) {
            lfn_feed(a, &e[i]);
            continue;
        }
        if (e[i].attr & ATTR_VOLUME_ID) {
            lfn_reset(a);
            continue;
        }
        const char *ln = lfn_take(a, &e[i]);
        if (!cb(drive_id, &e[i], ln, lba, i, user))
            return -1;
    }
    return 1;
}

static bool dir_iterate(uint8_t drive_id, uint16_t dir_cluster, dir_cb cb,
                        void *user) {
    fat_vol_t *v = vol(drive_id);
    lfn_acc a;
    lfn_reset(&a);
    if (dir_cluster == 0) {
        uint32_t root_sectors = (v->bpb.root_entry_count * 32 + 511) / 512;
        for (uint32_t s = 0; s < root_sectors; s++) {
            int r = scan_sector(drive_id, v->root_lba + s, &a, cb, user);
            if (r <= 0)
                return r == 0;
        }
        return true;
    }
    uint16_t cluster = dir_cluster;
    int safety = 0;
    while (vol_cluster_active(v, cluster) && safety++ < (int)v->cluster_count) {
        uint32_t start_lba = fat_cluster_to_lba(drive_id, cluster);
        for (uint32_t s = 0; s < v->bpb.sectors_per_cluster; s++) {
            int r = scan_sector(drive_id, start_lba + s, &a, cb, user);
            if (r <= 0)
                return r == 0;
        }
        cluster = fat_get(drive_id, cluster);
    }
    return true;
}

/* the n-th sector of a directory (cluster 0: the root); false past its end */
static bool dir_sector(uint8_t drive_id, uint16_t dir_cluster, uint32_t n,
                       uint32_t *lba) {
    fat_vol_t *v = vol(drive_id);
    if (dir_cluster == 0) {
        uint32_t root_sectors = (v->bpb.root_entry_count * 32 + 511) / 512;
        if (n >= root_sectors)
            return false;
        *lba = v->root_lba + n;
        return true;
    }
    uint16_t c = dir_cluster;
    uint32_t spc = v->bpb.sectors_per_cluster;
    for (uint32_t k = n / spc; k > 0; k--) {
        c = fat_get(drive_id, c);
        if (!vol_cluster_active(v, c))
            return false;
    }
    if (!vol_cluster_active(v, c))
        return false;
    *lba = fat_cluster_to_lba(drive_id, c) + n % spc;
    return true;
}

/* Free a file's long-name entries: only those right before it, with its
 * checksum, are touched, never another file's. */
static void lfn_clear(uint8_t drive_id, uint16_t dir_cluster,
                      const char name11[11]) {
    struct {
        uint32_t lba;
        uint8_t idx, sum;
    } run[LFN_MAX];
    int nrun = 0;
    uint8_t buf[512];
    uint32_t lba;
    for (uint32_t n = 0; dir_sector(drive_id, dir_cluster, n, &lba); n++) {
        if (!fs_read_sector(drive_id, lba, buf))
            return;
        dir_entry_t *e = (dir_entry_t *)buf;
        for (int i = 0; i < 16; i++) {
            if (e[i].name[0] == DIR_ENTRY_END)
                return;
            if (e[i].name[0] == DIR_ENTRY_FREE) {
                nrun = 0;
                continue;
            }
            if ((e[i].attr & 0x3F) == ATTR_LFN) {
                if (nrun < LFN_MAX) {
                    run[nrun].lba = lba;
                    run[nrun].idx = (uint8_t)i;
                    run[nrun].sum = ((uint8_t *)&e[i])[13];
                    nrun++;
                }
                continue;
            }
            if (!name83_eq(e[i].name, name11)) {
                nrun = 0;
                continue;
            }
            /* the file: free the long-name entries that belong to it */
            uint8_t sum = lfn_checksum((const uint8_t *)&e[i]); /* name+ext */
            uint8_t sb[512];
            uint32_t have = 0xFFFFFFFF;
            for (int k = 0; k < nrun; k++) {
                if (run[k].sum != sum)
                    continue;
                if (run[k].lba != have) {
                    if (have != 0xFFFFFFFF)
                        fs_write_sector(drive_id, have, sb);
                    if (!fs_read_sector(drive_id, run[k].lba, sb))
                        return;
                    have = run[k].lba;
                }
                sb[run[k].idx * 32] = DIR_ENTRY_FREE;
            }
            if (have != 0xFFFFFFFF)
                fs_write_sector(drive_id, have, sb);
            return;
        }
    }
}

/* ── names ───────────────────────────────────────────────────────────── */

/* a long name may hold anything but these (and control characters) */
bool fs_long_char_ok(char c) {
    if ((unsigned char)c < 0x20 || (unsigned char)c >= 0x7F)
        return false;
    return !strchr("\\/:*?\"<>|", c);
}

/* Copy name without leading spaces and trailing spaces and dots (Windows
 * drops those); false if it's empty or too long for FS_NAME_MAX. */
static bool name_trim(const char *in, char out[FS_NAME_SZ]) {
    while (*in == ' ')
        in++;
    if (strcmp(in, ".") == 0 || strcmp(in, "..") == 0) {
        strcpy(out, in);
        return true;
    }
    int n = (int)strlen(in);
    while (n > 0 && (in[n - 1] == ' ' || in[n - 1] == '.'))
        n--;
    if (n == 0 || n > FS_NAME_MAX)
        return false;
    memcpy(out, in, n);
    out[n] = '\0';
    return true;
}

/* fits 8.3 as it is (any case): no long name needed, except for case */
bool fs_name_is_83(const char *n) {
    if (strcmp(n, ".") == 0 || strcmp(n, "..") == 0)
        return true;
    const char *dot = strchr(n, '.');
    int bl = dot ? (int)(dot - n) : (int)strlen(n);
    int el = dot ? (int)strlen(dot + 1) : 0;
    if (bl < 1 || bl > 8 || el > 3 || (dot && el == 0))
        return false;
    for (int i = 0; i < bl; i++)
        if (!fs_name_char_ok(n[i]))
            return false;
    for (int i = 0; i < el; i++)
        if (!fs_name_char_ok(dot[1 + i]))
            return false;
    return true;
}

/* 0: all upper (or no letters), 1: all lower, 2: mixed */
static int part_case(const char *s, int len) {
    bool up = false, lo = false;
    for (int i = 0; i < len; i++) {
        if (s[i] >= 'A' && s[i] <= 'Z')
            up = true;
        if (s[i] >= 'a' && s[i] <= 'z')
            lo = true;
    }
    return up && lo ? 2 : lo ? 1 : 0;
}

/* an 8.3 name keeps all-lower parts in its case flags (like Windows NT);
 * only mixed case ("ReadMe.txt") needs a long name */
static bool needs_lfn(const char *n, uint8_t *case_flags) {
    *case_flags = 0;
    if (!fs_name_is_83(n))
        return true;
    if (n[0] == '.')
        return false;
    const char *dot = strchr(n, '.');
    int bl = dot ? (int)(dot - n) : (int)strlen(n);
    int cb = part_case(n, bl),
        ce = dot ? part_case(dot + 1, (int)strlen(dot + 1)) : 0;
    if (cb == 2 || ce == 2)
        return true;
    if (cb == 1)
        *case_flags |= CASE_LOWER_BASE;
    if (ce == 1)
        *case_flags |= CASE_LOWER_EXT;
    return false;
}

typedef struct {
    const char *name; /* trimmed */
    char n83[12];
    bool is83;
    dir_entry_t *out;
    char *lname_out; /* FS_NAME_SZ, may be NULL */
    uint32_t lba;
    int idx;
    bool found;
} lookup_ctx;

static bool lookup_cb(uint8_t drive_id, dir_entry_t *e, const char *lname,
                      uint32_t lba, int idx, void *user) {
    (void)drive_id;
    lookup_ctx *c = user;
    if ((lname && strcasecmp(lname, c->name) == 0) ||
        (c->is83 && name83_eq(e->name, c->n83))) {
        if (c->out)
            *c->out = *e;
        if (c->lname_out) {
            if (lname)
                strcpy(c->lname_out, lname);
            else
                fs_entry_name(e, c->lname_out);
        }
        c->lba = lba;
        c->idx = idx;
        c->found = true;
        return false;
    }
    return true;
}

/* find a file or folder by its long or its 8.3 name (any case) */
static bool lookup(uint8_t drive_id, uint16_t dir_cluster, const char *name,
                   dir_entry_t *out, char *lname_out, uint32_t *lba, int *idx) {
    char t[FS_NAME_SZ];
    if (!name_trim(name, t))
        return false;
    lookup_ctx c = {t, {0}, fs_name_is_83(t), out, lname_out, 0, 0, false};
    if (c.is83)
        fs_make_83(t, c.n83);
    dir_iterate(drive_id, dir_cluster, lookup_cb, &c);
    if (c.found) {
        if (lba)
            *lba = c.lba;
        if (idx)
            *idx = c.idx;
    }
    return c.found;
}

bool fs_find_name(uint8_t drive_id, uint16_t dir_cluster, const char *name,
                  dir_entry_t *out) {
    dir_entry_t tmp;
    return lookup(drive_id, dir_cluster, name, out ? out : &tmp, NULL, NULL,
                  NULL);
}

/* The 8.3 alias of a long name, as Windows makes it: upper case, without
 * spaces and inner dots, other unfit characters as '_', the base cut to fit
 * "~N" for the first N not taken: "My Long File.txt" -> MYLONG~1.TXT */
static bool make_alias(uint8_t drive_id, uint16_t dir_cluster, const char *n,
                       char out83[12]) {
    char base[FS_NAME_SZ], ext[4];
    int bl = 0, el = 0;
    while (*n == '.')
        n++;
    const char *dot = strrchr(n, '.');
    for (const char *p = n; *p && p != dot; p++) {
        char c = *p;
        if (c == ' ' || c == '.')
            continue;
        c = (char)toupper(c);
        base[bl++] = fs_name_char_ok(c) ? c : '_';
    }
    if (dot)
        for (const char *p = dot + 1; *p && el < 3; p++) {
            char c = *p;
            if (c == ' ' || c == '.')
                continue;
            c = (char)toupper(c);
            ext[el++] = fs_name_char_ok(c) ? c : '_';
        }
    if (bl == 0)
        base[bl++] = '_';
    for (int k = 1; k < 100000; k++) {
        char tail[8];
        int tl = snprintf(tail, sizeof(tail), "~%d", k);
        int keep = bl < 8 - tl ? bl : 8 - tl;
        memset(out83, ' ', 11);
        out83[11] = '\0';
        memcpy(out83, base, keep);
        memcpy(out83 + keep, tail, tl);
        memcpy(out83 + 8, ext, el);
        dir_entry_t tmp;
        if (!fs_find_in_dir(drive_id, dir_cluster, out83, &tmp))
            return true;
    }
    return false;
}

/* ── adding entries ──────────────────────────────────────────────────── */

/* Put e (and, if lname, its long-name pieces before it) into a run of free
 * slots, growing the folder when it's full (not the root: it has a fixed
 * size). Gives where the 8.3 entry went. */
static bool dir_put(uint8_t drive_id, uint16_t dir_cluster, const char *lname,
                    const dir_entry_t *e, uint32_t *out_lba, int *out_idx) {
    fat_vol_t *v = vol(drive_id);
    int len = lname ? (int)strlen(lname) : 0;
    int nl = (len + 12) / 13, total = nl + 1;
    uint8_t buf[512];
    uint32_t lba, start = 0;
    int run = 0;
    bool end = false; /* past the folder's end marker: all free */

    for (int pass = 0; pass < 2 && run < total; pass++) {
        run = 0;
        end = false;
        for (uint32_t n = 0;
             run < total && dir_sector(drive_id, dir_cluster, n, &lba); n++) {
            if (!fs_read_sector(drive_id, lba, buf))
                return false;
            dir_entry_t *d = (dir_entry_t *)buf;
            for (int i = 0; i < 16 && run < total; i++) {
                if (d[i].name[0] == DIR_ENTRY_END)
                    end = true;
                if (end || d[i].name[0] == DIR_ENTRY_FREE) {
                    if (run++ == 0)
                        start = n * 16 + i;
                } else {
                    run = 0;
                }
            }
        }
        if (run >= total)
            break;
        if (dir_cluster == 0)
            return false; /* the root is full */
        /* add a cluster to the folder; the run may begin at the old end */
        uint16_t last = dir_cluster, nx;
        while (vol_cluster_active(v, nx = fat_get(drive_id, last)))
            last = nx;
        uint16_t nc = fat_alloc(drive_id);
        if (nc == 0)
            return false;
        zero_cluster(drive_id, nc);
        fat_set(drive_id, last, nc);
    }
    if (run < total)
        return false;

    uint8_t sum = lfn_checksum((const uint8_t *)e);
    uint32_t have = 0xFFFFFFFF;
    for (int k = 0; k <= total; k++) {
        uint32_t slot = start + k;
        if (!dir_sector(drive_id, dir_cluster, slot / 16, &lba))
            break; /* the folder ends right after the run */
        if (lba != have) {
            if (have != 0xFFFFFFFF && !fs_write_sector(drive_id, have, buf))
                return false;
            if (!fs_read_sector(drive_id, lba, buf))
                return false;
            have = lba;
        }
        uint8_t *r = buf + (slot % 16) * 32;
        if (k == total) { /* after the run: keep the end marker */
            if (end)
                r[0] = DIR_ENTRY_END;
            break;
        }
        if (k < nl) {
            int ord = nl - k; /* the last piece first */
            memset(r, 0, 32);
            r[0] = (uint8_t)(ord | (k == 0 ? 0x40 : 0));
            r[11] = ATTR_LFN;
            r[13] = sum;
            for (int j = 0; j < 13; j++) {
                int pos = (ord - 1) * 13 + j;
                uint16_t c = pos < len    ? (uint8_t)lname[pos]
                             : pos == len ? 0x0000
                                          : 0xFFFF;
                r[k_lfn_off[j]] = (uint8_t)c;
                r[k_lfn_off[j] + 1] = (uint8_t)(c >> 8);
            }
        } else {
            memcpy(r, e, 32);
            *out_lba = lba;
            *out_idx = (int)(slot % 16);
        }
    }
    if (have != 0xFFFFFFFF && !fs_write_sector(drive_id, have, buf))
        return false;
    return true;
}

/* Name e for name (8.3 if it fits, else a long name and its alias) and put
 * it in the folder; false if the name is taken or bad. */
static bool make_entry(uint8_t drive_id, uint16_t dir_cluster, const char *name,
                       dir_entry_t *e, uint32_t *lba, int *idx) {
    char t[FS_NAME_SZ];
    if (!name_trim(name, t) || strcmp(t, ".") == 0 || strcmp(t, "..") == 0)
        return false;
    for (int i = 0; t[i]; i++)
        if (!fs_long_char_ok(t[i]))
            return false;
    dir_entry_t tmp;
    if (lookup(drive_id, dir_cluster, t, &tmp, NULL, NULL, NULL))
        return false;
    uint8_t cf;
    bool lfn = needs_lfn(t, &cf);
    char n83[12];
    if (lfn) {
        if (!make_alias(drive_id, dir_cluster, t, n83))
            return false;
    } else {
        fs_make_83(t, n83);
    }
    memcpy(e->name, n83, 11);
    e->reserved =
        (uint8_t)((e->reserved & ~(CASE_LOWER_BASE | CASE_LOWER_EXT)) | cf);
    return dir_put(drive_id, dir_cluster, lfn ? t : NULL, e, lba, idx);
}

static void stamp_new(dir_entry_t *e, uint8_t attr, uint16_t cluster) {
    memset(e, 0, sizeof(*e));
    e->attr = attr;
    e->cluster_lo = cluster;
    e->create_time = now_fat_time();
    e->create_date = now_fat_date();
    e->modify_time = e->create_time;
    e->modify_date = e->create_date;
}

/* mark the 8.3 entry at lba/idx free (after lfn_clear for its long name) */
static bool free_slot(uint8_t drive_id, uint32_t lba, int idx) {
    uint8_t buf[512];
    if (!fs_read_sector(drive_id, lba, buf))
        return false;
    ((dir_entry_t *)buf)[idx].name[0] = DIR_ENTRY_FREE;
    return fs_write_sector(drive_id, lba, buf);
}

/* '.'-less walk for copying folders */
typedef struct {
    dir_cb cb;
    void *user;
} skipdot_ctx;

static bool skipdot_cb(uint8_t drive_id, dir_entry_t *e, const char *lname,
                       uint32_t lba, int idx, void *user) {
    skipdot_ctx *c = user;
    if (e->name[0] == '.')
        return true;
    return c->cb(drive_id, e, lname, lba, idx, c->user);
}

typedef struct {
    dir_entry_t *buf;
    char (*names)[FS_NAME_SZ];
    int max;
    int count;
} list_ctx;

static bool list_cb(uint8_t drive_id, dir_entry_t *e, const char *lname,
                    uint32_t lba, int idx, void *user) {
    (void)drive_id;
    (void)lba;
    (void)idx;
    list_ctx *ctx = user;
    if (ctx->count < ctx->max) {
        if (ctx->buf)
            ctx->buf[ctx->count] = *e;
        if (ctx->names) {
            if (lname)
                strcpy(ctx->names[ctx->count], lname);
            else
                fs_entry_name(e, ctx->names[ctx->count]);
        }
        ctx->count++;
    }
    return true;
}

/* exact 8.3 match, with where the entry sits */
typedef struct {
    const char *name83;
    dir_entry_t *out;
    uint32_t lba;
    int idx;
    bool found;
} find_ctx;

static bool find_cb(uint8_t drive_id, dir_entry_t *e, const char *lname,
                    uint32_t lba, int idx, void *user) {
    (void)drive_id;
    (void)lname;
    find_ctx *ctx = user;
    if (name83_eq(e->name, ctx->name83)) {
        if (ctx->out)
            *ctx->out = *e;
        ctx->lba = lba;
        ctx->idx = idx;
        ctx->found = true;
        return false;
    }
    return true;
}

static bool find83(uint8_t drive_id, uint16_t dir_cluster, const char *name83,
                   dir_entry_t *out, uint32_t *lba, int *idx) {
    find_ctx ctx = {name83, out, 0, 0, false};
    dir_iterate(drive_id, dir_cluster, find_cb, &ctx);
    if (ctx.found) {
        if (lba)
            *lba = ctx.lba;
        if (idx)
            *idx = ctx.idx;
    }
    return ctx.found;
}

typedef struct {
    const char *name;
    bool done;
    int action; /* 0: delete a file, 2: a folder (its clusters go too) */
} modify_ctx;

static bool modify_cb(uint8_t drive_id, dir_entry_t *e, const char *lname,
                      uint32_t lba, int idx, void *user) {
    (void)lname;
    modify_ctx *ctx = user;
    if (name83_eq(e->name, ctx->name)) {
        uint8_t sector_buf[512];
        if (!fs_read_sector(drive_id, lba, sector_buf))
            return false;
        dir_entry_t *entries = (dir_entry_t *)sector_buf;
        fat_free_chain(drive_id, entries[idx].cluster_lo);
        entries[idx].name[0] = DIR_ENTRY_FREE;
        fs_write_sector(drive_id, lba, sector_buf);
        ctx->done = true;
        return false;
    }
    return true;
}

static void extract_volume_label(fat_vol_t *v) {
    int j = 0;
    for (int i = 0; i < 11; i++) {
        char c = (char)v->bpb.volume_label[i];
        if (c == ' ' || c == '\0')
            break;
        v->label[j++] = c;
    }
    v->label[j] = '\0';

    if (j == 0) {
        uint8_t buf[512];
        if (fs_read_sector(v->drive_id, v->root_lba, buf)) {
            dir_entry_t *entries = (dir_entry_t *)buf;
            for (int i = 0; i < 16; i++) {
                if (entries[i].name[0] == DIR_ENTRY_END)
                    break;
                if ((entries[i].attr & ATTR_VOLUME_ID) &&
                    (entries[i].attr & 0x3F) != 0x0F) { /* not a long name */
                    int k = 0;
                    for (int m = 0; m < 8 && entries[i].name[m] != ' '; m++)
                        v->label[k++] = entries[i].name[m];
                    for (int m = 0; m < 3 && entries[i].ext[m] != ' '; m++)
                        v->label[k++] = entries[i].ext[m];
                    v->label[k] = '\0';
                    break;
                }
            }
        }
    }

    if (v->label[0] == '\0') {
        const char *names[] = {"HDA", "HDB", "FDD0", "FDD1"};
        strncpy(v->label, names[v->drive_id], 11);
    }
}

/* any change to names on any disk (fs.h) */
uint32_t g_fs_rev;

bool fs_mount_drive(uint8_t drive_id) {
    g_fs_rev++;
    if (drive_id >= DRIVE_COUNT)
        return false;
    fcache_drop(drive_id); /* maybe another disk now */
    fat_vol_t *v = vol(drive_id);
    v->drive_id = drive_id;
    v->mounted = false;
    fs_usage_invalidate(drive_id);

    uint8_t tmp_buf[512];
    if (!fs_read_sector(drive_id, 0, tmp_buf))
        return false;
    memcpy(&v->bpb, tmp_buf, sizeof(bpb_t));
    if (v->bpb.bytes_per_sector != 512 || !v->bpb.sectors_per_cluster ||
        !v->bpb.fat_count) {
        /* DOS 1.x floppies have no BPB: the FAT's media byte says the
         * size, and the size gives the standard layout */
        if (drive_id != DRIVE_FDD0 && drive_id != DRIVE_FDD1)
            return false;
        uint8_t unit = drive_id == DRIVE_FDD0 ? 0 : 1;
        int m = fdd_media(unit);
        if (fs_read_sector(drive_id, 1, tmp_buf) &&
            fdd_media_from_byte(tmp_buf[0]))
            m = fdd_media_from_byte(tmp_buf[0]);
        if (m <= FDD_MEDIA_NONE || m > FDD_MEDIA_360K)
            return false; /* the later sizes always have a BPB */
        const fdd_format_t *f = &FDD_FORMATS[m];
        memset(&v->bpb, 0, sizeof(v->bpb));
        v->bpb.bytes_per_sector = 512;
        v->bpb.sectors_per_cluster = f->sec_per_clus;
        v->bpb.reserved_sectors = 1;
        v->bpb.fat_count = 2;
        v->bpb.root_entry_count = f->root_entries;
        v->bpb.total_sectors_16 = (uint16_t)(f->spt * f->heads * f->tracks);
        v->bpb.media_type = f->media_byte;
        v->bpb.sectors_per_fat = f->fat_sectors;
        v->bpb.sectors_per_track = f->spt;
        v->bpb.head_count = f->heads;
    }

    v->fat_lba = v->bpb.reserved_sectors;
    v->root_lba = v->fat_lba + v->bpb.fat_count * v->bpb.sectors_per_fat;
    uint32_t root_sectors = (v->bpb.root_entry_count * 32 + 511) / 512;
    v->data_lba = v->root_lba + root_sectors;

    uint32_t total = v->bpb.total_sectors_16 ? v->bpb.total_sectors_16
                                             : v->bpb.total_sectors_32;
    v->cluster_count = (total - v->data_lba) / v->bpb.sectors_per_cluster;
    v->fat_type = (v->cluster_count < 4085) ? FAT_TYPE_FAT12 : FAT_TYPE_FAT16;

    /* Clamp to what one FAT copy can address. The default image has 16110
     * data clusters but a 32-sector FAT (8192 entries): clusters past 8190
     * made fat_set write into FAT #2 and then the ROOT DIRECTORY, wiping
     * files once ~16 MB were used. */
    uint32_t fat_entries =
        (v->fat_type == FAT_TYPE_FAT12)
            ? (uint32_t)v->bpb.sectors_per_fat * 512u * 2u / 3u
            : (uint32_t)v->bpb.sectors_per_fat * 256u;
    if (fat_entries >= 2 && v->cluster_count > fat_entries - 2)
        v->cluster_count = fat_entries - 2;
    v->mounted = true;

    extract_volume_label(v);
    return true;
}

bool fs_select_drive(uint8_t drive_id) {
    if (drive_id >= DRIVE_COUNT || !g_drives[drive_id].mounted)
        return false;
    s_current_drive = drive_id;
    return true;
}

uint8_t fs_current_drive(void) { return s_current_drive; }

const char *fs_drive_label(uint8_t drive_id) {
    if (drive_id >= DRIVE_COUNT || !g_drives[drive_id].mounted)
        return "";
    return g_drives[drive_id].label;
}

bool fs_drive_mounted(uint8_t drive_id) {
    if (drive_id >= DRIVE_COUNT)
        return false;
    return g_drives[drive_id].mounted;
}

bool fs_mount(void) {
    memset(g_drives, 0, sizeof(g_drives));
    if (!fs_mount_drive(DRIVE_HDA))
        return false;
    s_current_drive = DRIVE_HDA;
    dir_context.drive_id = DRIVE_HDA;
    dir_context.current_cluster = 0;
    strcpy(dir_context.path, "/");
    fs_mount_drive(DRIVE_HDB);
    if (fdd_init(0))
        fs_mount_drive(DRIVE_FDD0);
    if (fdd_init(1))
        fs_mount_drive(DRIVE_FDD1);
    return true;
}

void fs_unmount(uint8_t drive_id) {
    g_fs_rev++;
    if (drive_id >= DRIVE_COUNT)
        return;
    fcache_drop(drive_id);
    g_drives[drive_id].mounted = false;
    fs_usage_invalidate(drive_id);
}
/* copy the next path component (up to '/'); one longer than any name is
 * cut so it can't match */
static const char *next_component(const char *p, char out[FS_NAME_SZ + 2]) {
    int i = 0;
    while (*p && *p != '/') {
        if (i < FS_NAME_SZ + 1)
            out[i++] = *p;
        p++;
    }
    out[i] = '\0';
    return p;
}

bool fs_resolve_dir(const char *path, uint8_t *out_drive,
                    uint16_t *out_cluster) {
    uint8_t drive;
    uint16_t cluster = 0;
    if (!path || !*path)
        return false;
    if (*path == '/') {
        drive = DRIVE_HDA;
        path++;
        if (!*path) {
            *out_drive = DRIVE_HDA;
            *out_cluster = 0;
            return true;
        }
    } else {
        drive = dir_context.drive_id;
        cluster = dir_context.current_cluster;
    }

    char component[FS_NAME_SZ + 2];
    while (*path) {
        while (*path == '/')
            path++;
        if (!*path)
            break;
        path = next_component(path, component);

        if (cluster == 0 && drive == DRIVE_HDA && strcmp(component, "HDA") == 0)
            continue;
        bool crossed = false;
        for (int m = 0; m < VFS_MOUNT_COUNT; m++) {
            const char *mp = g_vfs_mounts[m].path + 1;
            if (strcasecmp(component, mp) == 0) {
                uint8_t target = g_vfs_mounts[m].drive_id;
                if (!fs_drive_mounted(target))
                    return false;
                drive = target;
                cluster = 0;
                crossed = true;
                break;
            }
        }
        if (crossed)
            continue;

        dir_entry_t de;
        if (!lookup(drive, cluster, component, &de, NULL, NULL, NULL))
            return false;
        if (!(de.attr & ATTR_DIRECTORY))
            return false;
        cluster = de.cluster_lo;
    }
    *out_drive = drive;
    *out_cluster = cluster;
    return true;
}

bool fs_get_usage(uint32_t *total_bytes, uint32_t *used_bytes) {
    uint8_t d = dir_context.drive_id;
    fat_vol_t *v = vol(d);
    if (!v->mounted)
        return false;
    *total_bytes = (v->bpb.total_sectors_32 ? v->bpb.total_sectors_32
                                            : v->bpb.total_sectors_16) *
                   v->bpb.bytes_per_sector;
    uint32_t used = 0;
    for (uint16_t c = 2; c < (uint16_t)(v->cluster_count + 2); c++) {
        uint16_t val = fat_get(d, c);
        if (val != FAT16_FREE && val != FAT16_BAD)
            used++;
    }
    *used_bytes = used * v->bpb.bytes_per_sector * v->bpb.sectors_per_cluster;
    return true;
}

/* Used-cluster counts. Counting walks the whole FAT, so the result is
 * cached until the FAT changes (fat_set) or the drive is (un)mounted. */
static bool s_usage_valid[DRIVE_COUNT];
static uint32_t s_usage_used[DRIVE_COUNT];

void fs_usage_invalidate(uint8_t drive_id) {
    if (drive_id < DRIVE_COUNT)
        s_usage_valid[drive_id] = false;
}

/* One read per FAT sector (fat_get would re-read a sector per cluster). */
static bool count_used_clusters(fat_vol_t *v, uint8_t d, uint32_t *out) {
    uint8_t buf[1024]; /* two sectors: a FAT12 entry may straddle them */
    uint32_t loaded = 0xFFFFFFFFu, used = 0;
    uint32_t fat_secs = v->bpb.sectors_per_fat;
    for (uint32_t c = 2; c < v->cluster_count + 2; c++) {
        bool fat12 = v->fat_type == FAT_TYPE_FAT12;
        uint32_t off = fat12 ? c * 3 / 2 : c * 2;
        uint32_t sec = off / 512;
        if (sec != loaded) {
            if (!fs_read_sector(d, v->fat_lba + sec, buf))
                return false;
            if (sec + 1 < fat_secs &&
                !fs_read_sector(d, v->fat_lba + sec + 1, buf + 512))
                return false;
            loaded = sec;
        }
        uint32_t o = off % 512;
        uint16_t val = buf[o] | ((uint16_t)buf[o + 1] << 8);
        if (fat12) {
            val = (c & 1) ? (val >> 4) : (val & 0x0FFF);
            if (val != 0 && val != FAT12_BAD)
                used++;
        } else if (val != FAT16_FREE && val != FAT16_BAD) {
            used++;
        }
    }
    *out = used;
    return true;
}

bool fs_get_usage_drive(uint8_t drive_id, uint32_t *total_bytes,
                        uint32_t *used_bytes) {
    if (drive_id >= DRIVE_COUNT || !g_drives[drive_id].mounted)
        return false;
    fat_vol_t *v = &g_drives[drive_id];
    *total_bytes = (v->bpb.total_sectors_32 ? v->bpb.total_sectors_32
                                            : v->bpb.total_sectors_16) *
                   v->bpb.bytes_per_sector;
    if (!s_usage_valid[drive_id]) {
        if (!count_used_clusters(v, drive_id, &s_usage_used[drive_id]))
            return false;
        s_usage_valid[drive_id] = true;
    }
    *used_bytes = s_usage_used[drive_id] * v->bpb.bytes_per_sector *
                  v->bpb.sectors_per_cluster;
    return true;
}
void fs_make_83(const char *filename, char *out83) {
    memset(out83, ' ', 11);
    out83[11] = '\0';
    int i = 0, j = 0;
    while (filename[i] == ' ')
        i++;
    if (filename[i] == '.') {
        out83[0] = '.';
        if (filename[i + 1] == '.') {
            out83[1] = '.';
            return;
        }
        if (filename[i + 1] == '\0' || filename[i + 1] == ' ')
            return;
        i++;
    }
    while (filename[i] && filename[i] != '.' && j < 8) {
        char c = filename[i++];
        if (c == '"' || c == '*' || c == '/' || c == ':' || c == '<' ||
            c == '>' || c == '?' || c == '\\' || c == '|')
            continue;
        out83[j++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    if (filename[i] == '.')
        i++;
    j = 8;
    while (filename[i] && j < 11) {
        char c = filename[i++];
        if (c == '"' || c == '*' || c == '/' || c == ':' || c == '<' ||
            c == '>' || c == '?' || c == '\\' || c == '|')
            continue;
        out83[j++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
}

/* Walk path to its last part. out_name83 is that file's 8.3 name if it
 * exists, else the one it would get (its alias, for a long name); leaf is
 * the last part as typed (trimmed). */
static bool resolve_ex(const char *path, uint8_t *out_drive,
                       uint16_t *out_parent_cluster, char out_name83[12],
                       char leaf[FS_NAME_SZ], bool *exists) {
    if (!path || !*path)
        return false;

    uint8_t cur_drive = dir_context.drive_id;
    uint16_t cur_cluster = dir_context.current_cluster;
    const char *ptr = path;

    if (*ptr == '/') {
        cur_drive = DRIVE_HDA;
        cur_cluster = 0;
        ptr++;
        if (!*ptr) {
            *out_drive = DRIVE_HDA;
            *out_parent_cluster = 0;
            memset(out_name83, ' ', 11);
            out_name83[11] = '\0';
            leaf[0] = '\0';
            *exists = true;
            return true;
        }
    }

    char component[FS_NAME_SZ + 2];
    while (true) {
        ptr = next_component(ptr, component);
        while (*ptr == '/')
            ptr++;

        bool is_leaf = (*ptr == '\0');
        if (is_leaf) {
            if (!name_trim(component, leaf))
                return false;
            *out_drive = cur_drive;
            *out_parent_cluster = cur_cluster;
            dir_entry_t e;
            if (lookup(cur_drive, cur_cluster, leaf, &e, NULL, NULL, NULL)) {
                memcpy(out_name83, e.name, 11);
                out_name83[11] = '\0';
                *exists = true;
                return true;
            }
            *exists = false;
            uint8_t cf;
            if (!needs_lfn(leaf, &cf)) {
                fs_make_83(leaf, out_name83);
                return true;
            }
            return make_alias(cur_drive, cur_cluster, leaf, out_name83);
        }

        // ── Handle drive‑prefix components ─────────────────────
        if (cur_drive == DRIVE_HDA && cur_cluster == 0) {
            if (strcmp(component, "HDA") == 0)
                continue;

            bool crossed = false;
            for (int m = 0; m < VFS_MOUNT_COUNT; m++) {
                const char *mp_name = g_vfs_mounts[m].path + 1;
                if (strcasecmp(component, mp_name) == 0) {
                    uint8_t target = g_vfs_mounts[m].drive_id;
                    if (!fs_drive_mounted(target))
                        return false;
                    cur_drive = target;
                    cur_cluster = 0;
                    crossed = true;
                    break;
                }
            }
            if (crossed)
                continue;
        }

        dir_entry_t entry;
        if (!lookup(cur_drive, cur_cluster, component, &entry, NULL, NULL,
                    NULL))
            return false;
        if (!(entry.attr & ATTR_DIRECTORY))
            return false;
        cur_cluster = entry.cluster_lo;
    }
}

bool fs_resolve(const char *path, uint8_t *out_drive,
                uint16_t *out_parent_cluster, char out_name83[12]) {
    char leaf[FS_NAME_SZ];
    bool ex;
    return resolve_ex(path, out_drive, out_parent_cluster, out_name83, leaf,
                      &ex);
}

bool fs_chdir(const char *path) {
    if (!path || !*path)
        return false;
    if (strcmp(path, "/") == 0) {
        dir_context.drive_id = DRIVE_HDA;
        dir_context.current_cluster = 0;
        strcpy(dir_context.path, "/");
        return true;
    }

    uint8_t target_drive;
    uint16_t target_parent;
    char target_name83[12];
    if (!fs_resolve(path, &target_drive, &target_parent, target_name83))
        return false;

    dir_entry_t entry;
    if (!fs_find_in_dir(target_drive, target_parent, target_name83, &entry))
        return false;
    if (!(entry.attr & ATTR_DIRECTORY))
        return false;

    char new_path[FS_PATH_SZ];
    if (path[0] == '/') {
        strncpy(new_path, path, FS_PATH_MAX);
    } else {
        strncpy(new_path, dir_context.path, FS_PATH_MAX);
        new_path[FS_PATH_MAX] = '\0';
        if (strcmp(new_path, "/") != 0)
            strncat(new_path, "/", FS_PATH_MAX - strlen(new_path));
        strncat(new_path, path, FS_PATH_MAX - strlen(new_path));
    }
    new_path[FS_PATH_MAX] = '\0';

    char *comps[64];
    int comp_len[64];
    int top = 0;
    char *p = new_path;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        char *start = p;
        int len = 0;
        while (*p && *p != '/') {
            p++;
            len++;
        }
        if (len == 1 && start[0] == '.')
            continue;
        else if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (top > 0)
                top--;
        } else if (top < 64) {
            comps[top] = start;
            comp_len[top] = len;
            top++;
        }
    }

    /* the path that went in is checked; only now change the context */
    dir_context.drive_id = target_drive;
    dir_context.current_cluster = entry.cluster_lo;
    if (top == 0) {
        strcpy(dir_context.path, "/");
    } else {
        int out_idx = 0;
        for (int i = 0; i < top; i++) {
            if (out_idx + 1 + comp_len[i] > FS_PATH_MAX)
                break;
            dir_context.path[out_idx++] = '/';
            for (int j = 0; j < comp_len[i]; j++)
                dir_context.path[out_idx++] = comps[i][j];
        }
        dir_context.path[out_idx] = '\0';
    }
    return true;
}

bool fs_pwd(char *buf, int buflen) {
    if (!buf || buflen < 2)
        return false;
    strncpy(buf, dir_context.path, buflen - 1);
    buf[buflen - 1] = '\0';
    return true;
}

bool fs_find_in_dir(uint8_t drive_id, uint16_t dir_cluster, const char *name83,
                    dir_entry_t *out) {
    dir_entry_t tmp;
    return find83(drive_id, dir_cluster, name83, out ? out : &tmp, NULL, NULL);
}

bool fs_find(const char *path, dir_entry_t *out) {
    uint8_t drive_id;
    uint16_t dir_cluster;
    char name83[12], leaf[FS_NAME_SZ];
    bool ex;
    if (!resolve_ex(path, &drive_id, &dir_cluster, name83, leaf, &ex) || !ex)
        return false;
    return fs_find_in_dir(drive_id, dir_cluster, name83, out);
}

bool fs_name_of(const char *path, char out[FS_NAME_SZ]) {
    uint8_t drive_id;
    uint16_t dir_cluster;
    char name83[12], leaf[FS_NAME_SZ];
    bool ex;
    if (!resolve_ex(path, &drive_id, &dir_cluster, name83, leaf, &ex) || !ex)
        return false;
    if (!leaf[0]) {
        strcpy(out, "/");
        return true;
    }
    return lookup(drive_id, dir_cluster, leaf, NULL, out, NULL, NULL);
}

bool fs_list_dir(uint8_t drive_id, uint16_t dir_cluster, dir_entry_t *buf,
                 int max, int *count) {
    list_ctx ctx = {buf, NULL, max, 0};
    bool ok = dir_iterate(drive_id, dir_cluster, list_cb, &ctx);
    *count = ctx.count;
    return ok;
}

typedef struct {
    fs_walk_cb cb;
    void *user;
} walk_ctx;

static bool walk_cb(uint8_t drive_id, dir_entry_t *e, const char *lname,
                    uint32_t lba, int idx, void *user) {
    (void)drive_id;
    (void)lba;
    (void)idx;
    walk_ctx *w = user;
    if (e->name[0] == '.')
        return true;
    char nm[FS_NAME_SZ];
    if (lname)
        strcpy(nm, lname);
    else
        fs_entry_name(e, nm);
    return w->cb(e, nm, w->user);
}

bool fs_walk_dir(uint8_t drive_id, uint16_t dir_cluster, fs_walk_cb cb,
                 void *user) {
    walk_ctx w = {cb, user};
    return cb && dir_iterate(drive_id, dir_cluster, walk_cb, &w);
}

bool fs_list_dir_names(uint8_t drive_id, uint16_t dir_cluster, dir_entry_t *buf,
                       char (*names)[FS_NAME_SZ], int max, int *count) {
    list_ctx ctx = {buf, names, max, 0};
    bool ok = dir_iterate(drive_id, dir_cluster, list_cb, &ctx);
    *count = ctx.count;
    return ok;
}

bool fs_is_dir_empty(uint8_t drive_id, uint16_t cluster) {
    if (cluster == 0)
        return false;
    fat_vol_t *v = vol(drive_id);
    uint16_t cur = cluster;
    uint8_t sector_buf[512];
    int safety = 0;
    while (vol_cluster_active(v, cur) && safety++ < 65536) {
        uint32_t start_lba = fat_cluster_to_lba(drive_id, cur);
        for (uint32_t s = 0; s < v->bpb.sectors_per_cluster; s++) {
            if (!fs_read_sector(drive_id, start_lba + s, sector_buf))
                return false;
            dir_entry_t *entries = (dir_entry_t *)sector_buf;
            for (int i = 0; i < 16; i++) {
                uint8_t first = entries[i].name[0];
                if (first == DIR_ENTRY_END)
                    return true;
                if (first == DIR_ENTRY_FREE || first == '.')
                    continue;
                if ((entries[i].attr & 0x3F) == ATTR_LFN)
                    continue; /* a stray long name: no file */
                return false;
            }
        }
        cur = fat_get(drive_id, cur);
    }
    return true;
}

void fs_wipe_cluster(uint8_t drive_id, uint16_t cluster) {
    fat_vol_t *v = vol(drive_id);
    if (!vol_cluster_active(v, cluster))
        return;
    uint16_t cur = cluster;
    int safety = 0;
    while (vol_cluster_active(v, cur) && safety++ < 65536) {
        uint32_t start_lba = fat_cluster_to_lba(drive_id, cur);
        for (uint32_t s = 0; s < v->bpb.sectors_per_cluster; s++) {
            uint8_t local_buf[512];
            if (!fs_read_sector(drive_id, start_lba + s, local_buf)) {
                fat_set(drive_id, cur, FAT16_BAD);
                return;
            }
            dir_entry_t *entries = (dir_entry_t *)local_buf;
            for (int i = 0; i < 16; i++) {
                uint8_t first = entries[i].name[0];
                if (first == DIR_ENTRY_END) {
                    fat_free_chain(drive_id, cluster);
                    return;
                }
                if (first == DIR_ENTRY_FREE || first == '.')
                    continue;
                if ((entries[i].attr & 0x3F) == ATTR_LFN)
                    continue;
                if (entries[i].attr & ATTR_DIRECTORY)
                    fs_wipe_cluster(drive_id, entries[i].cluster_lo);
                else
                    fat_free_chain(drive_id, entries[i].cluster_lo);
            }
        }
        cur = fat_get(drive_id, cur);
    }
    fat_free_chain(drive_id, cluster);
}

bool fs_open(const char *path, fat_file_t *f) {
    uint8_t drive_id;
    uint16_t dir_cluster;
    char name83[12], leaf[FS_NAME_SZ];
    bool ex;
    if (!resolve_ex(path, &drive_id, &dir_cluster, name83, leaf, &ex) || !ex)
        return false;
    dir_entry_t entry;
    uint32_t lba;
    int idx;
    if (!find83(drive_id, dir_cluster, name83, &entry, &lba, &idx))
        return false;
    memset(f, 0, sizeof(fat_file_t));
    f->entry = entry;
    f->cur_cluster = entry.cluster_lo;
    f->dir_cluster = dir_cluster;
    f->dir_entry_lba = lba;
    f->dir_entry_idx = idx;
    f->open = true;
    f->drive_id = drive_id;
    return true;
}

bool fs_create(const char *path, fat_file_t *f) {
    g_fs_rev++;
    uint8_t drive_id;
    uint16_t dir_cluster;
    char name83[12], leaf[FS_NAME_SZ];
    bool ex;
    if (!resolve_ex(path, &drive_id, &dir_cluster, name83, leaf, &ex) || ex)
        return false;

    uint16_t cluster = fat_alloc(drive_id);
    if (cluster == 0)
        return false;
    dir_entry_t e;
    stamp_new(&e, ATTR_ARCHIVE, cluster);
    uint32_t lba;
    int idx;
    if (!make_entry(drive_id, dir_cluster, leaf, &e, &lba, &idx)) {
        fat_free_chain(drive_id, cluster);
        return false;
    }

    memset(f, 0, sizeof(fat_file_t));
    f->entry = e;
    f->dir_entry_lba = lba;
    f->dir_entry_idx = idx;
    f->cur_cluster = cluster;
    f->dir_cluster = dir_cluster;
    f->open = true;
    f->drive_id = drive_id;
    return true;
}

int fs_read(fat_file_t *f, void *buf, int len) {
    if (!f->open)
        return -1;
    fat_vol_t *v = vol(f->drive_id);
    uint8_t *out = (uint8_t *)buf;
    int total = 0;
    uint32_t filesize = f->entry.file_size;
    uint32_t cluster_bytes = v->bpb.sectors_per_cluster * 512;

    while (len > 0 && f->cur_offset < filesize) {
        if (f->cur_offset > 0 && f->cur_offset % cluster_bytes == 0) {
            uint16_t next = fat_get(f->drive_id, f->cur_cluster);
            if (vol_is_eoc(v, next) || next == 0)
                break;
            f->cur_cluster = next;
        }

        if (!vol_cluster_active(v, f->cur_cluster))
            break;

        uint32_t offset_in_cluster = f->cur_offset % cluster_bytes;
        uint32_t sector_in_cluster = offset_in_cluster / 512;
        uint32_t offset_in_sector = offset_in_cluster % 512;
        uint32_t lba = fat_cluster_to_lba(f->drive_id, f->cur_cluster);
        if (lba == 0xFFFFFFFF)
            break;
        lba += sector_in_cluster;

        uint8_t local_buf[512];
        if (!fs_read_sector(f->drive_id, lba, local_buf))
            break;

        int can_read = 512 - (int)offset_in_sector;
        int remaining = (int)(filesize - f->cur_offset);
        if (can_read > remaining)
            can_read = remaining;
        if (can_read > len)
            can_read = len;

        memcpy(out, local_buf + offset_in_sector, can_read);
        out += can_read;
        total += can_read;
        len -= can_read;
        f->cur_offset += can_read;
    }
    return total;
}

/* No EOC rewrite here: fat_alloc() already marks every new cluster EOC.
 * Forcing EOC at the current cluster truncated (and leaked) the rest of the
 * chain whenever an existing file was overwritten from the start. */
static int done_write(fat_file_t *f, int total, fat_vol_t *v) {
    (void)v;
    if (total >= 0) {
        f->entry.modify_time = now_fat_time();
        f->entry.modify_date = now_fat_date();
    }
    return total;
}

int fs_write(fat_file_t *f, const void *buf, int len) {
    if (!f->open)
        return -1;
    fat_vol_t *v = vol(f->drive_id);
    const uint8_t *in = (const uint8_t *)buf;
    int total = 0;
    uint8_t write_sector_buf[512];
    uint32_t cluster_bytes = v->bpb.sectors_per_cluster * 512;

    while (len > 0) {
        if (f->cur_offset > 0 && f->cur_offset % cluster_bytes == 0) {
            uint16_t next = fat_get(f->drive_id, f->cur_cluster);
            if (vol_is_eoc(v, next) || next == 0) {
                uint16_t new_cluster = fat_alloc(f->drive_id);
                if (new_cluster == 0) {
                    return done_write(f, -1, v);
                }
                zero_cluster(f->drive_id, new_cluster);
                fat_set(f->drive_id, f->cur_cluster, new_cluster);
                f->cur_cluster = new_cluster;
            } else {
                f->cur_cluster = next;
            }
        } else if (f->cur_offset == 0 &&
                   !vol_cluster_active(v, f->cur_cluster)) {
            uint16_t new_cluster = fat_alloc(f->drive_id);
            if (new_cluster == 0) {
                return done_write(f, -1, v);
            }
            zero_cluster(f->drive_id, new_cluster);
            f->entry.cluster_lo = new_cluster;
            f->cur_cluster = new_cluster;
        }

        if (!vol_cluster_active(v, f->cur_cluster))
            break;

        uint32_t offset_in_cluster = f->cur_offset % cluster_bytes;
        uint32_t sector_in_cluster = offset_in_cluster / 512;
        uint32_t offset_in_sector = offset_in_cluster % 512;
        uint32_t lba =
            fat_cluster_to_lba(f->drive_id, f->cur_cluster) + sector_in_cluster;

        if (offset_in_sector != 0 || len < 512) {
            if (!fs_read_sector(f->drive_id, lba, write_sector_buf))
                break;
        } else {
            memset(write_sector_buf, 0, 512);
        }

        int can_write = 512 - (int)offset_in_sector;
        if (can_write > len)
            can_write = len;

        memcpy(write_sector_buf + offset_in_sector, in, can_write);
        if (!fs_write_sector(f->drive_id, lba, write_sector_buf)) {
            return done_write(f, -1, v);
        }

        in += can_write;
        total += can_write;
        len -= can_write;
        f->cur_offset += can_write;
        if (f->cur_offset > f->entry.file_size)
            f->entry.file_size = f->cur_offset;
    }

    f->entry.modify_time = now_fat_time();
    f->entry.modify_date = now_fat_date();
    return total;
}

bool fs_seek(fat_file_t *f, uint32_t offset) {
    if (!f->open || offset > f->entry.file_size)
        return false;
    fat_vol_t *v = vol(f->drive_id);
    f->cur_offset = offset;
    f->cur_cluster = f->entry.cluster_lo;
    uint32_t cluster_bytes = v->bpb.sectors_per_cluster * 512;
    /* read/write keep cur_cluster on the PREVIOUS cluster at an exact
     * boundary and advance on the next access; seek the same way, or the
     * next read/write skips a whole cluster. */
    uint32_t clusters_to_skip = offset ? (offset - 1) / cluster_bytes : 0;
    for (uint32_t i = 0; i < clusters_to_skip; i++) {
        if (!vol_cluster_active(v, f->cur_cluster))
            return false;
        f->cur_cluster = fat_get(f->drive_id, f->cur_cluster);
    }
    return true;
}

int fs_tell(fat_file_t *f) {
    if (!f || !f->open)
        return -1;
    return (int)f->cur_offset;
}

bool fs_close(fat_file_t *f) {
    if (!f->open)
        return false;
    uint8_t close_buf[512];
    if (!fs_read_sector(f->drive_id, f->dir_entry_lba, close_buf))
        return false;
    dir_entry_t *entries = (dir_entry_t *)close_buf;
    entries[f->dir_entry_idx].file_size = f->entry.file_size;
    entries[f->dir_entry_idx].cluster_lo = f->entry.cluster_lo;
    entries[f->dir_entry_idx].modify_time = f->entry.modify_time;
    entries[f->dir_entry_idx].modify_date = f->entry.modify_date;
    if (!fs_write_sector(f->drive_id, f->dir_entry_lba, close_buf))
        return false;
    f->open = false;
    return true;
}
/* a new, empty folder called name in dir_cluster */
static bool mkdir_in(uint8_t drive_id, uint16_t dir_cluster, const char *name,
                     uint16_t *out_cluster) {
    uint16_t cluster = fat_alloc(drive_id);
    if (cluster == 0)
        return false;
    zero_cluster(drive_id, cluster);
    dir_entry_t e;
    stamp_new(&e, ATTR_DIRECTORY, cluster);

    uint8_t dir_block[512];
    memset(dir_block, 0, 512);
    dir_entry_t *dot = (dir_entry_t *)dir_block;
    dot[0] = e;
    memcpy(dot[0].name, ".          ", 11);
    dot[1] = e;
    memcpy(dot[1].name, "..         ", 11);
    dot[1].cluster_lo = dir_cluster;
    if (!fs_write_sector(drive_id, fat_cluster_to_lba(drive_id, cluster),
                         dir_block)) {
        fat_free_chain(drive_id, cluster);
        return false;
    }

    uint32_t lba;
    int idx;
    if (!make_entry(drive_id, dir_cluster, name, &e, &lba, &idx)) {
        fat_free_chain(drive_id, cluster);
        return false;
    }
    if (out_cluster)
        *out_cluster = cluster;
    return true;
}

bool fs_mkdir(const char *path) {
    g_fs_rev++;
    uint8_t drive_id;
    uint16_t dir_cluster;
    char name83[12], leaf[FS_NAME_SZ];
    bool ex;
    if (!resolve_ex(path, &drive_id, &dir_cluster, name83, leaf, &ex) || ex)
        return false;
    return mkdir_in(drive_id, dir_cluster, leaf, NULL);
}

/* Rename the entry name83 in a folder to new_name. The 8.3 entry stays put
 * when the new name fits 8.3; a long name needs free slots before it, so
 * the entry moves (to a new run of slots) then. */
static bool rename_in(uint8_t drive_id, uint16_t dir_cluster,
                      const char *name83, const char *new_name) {
    char t[FS_NAME_SZ];
    if (!name_trim(new_name, t) || strcmp(t, ".") == 0 || strcmp(t, "..") == 0)
        return false;
    for (int i = 0; t[i]; i++)
        if (!fs_long_char_ok(t[i]))
            return false;

    dir_entry_t e;
    uint32_t lba, olba;
    int idx, oidx;
    if (!find83(drive_id, dir_cluster, name83, &e, &lba, &idx))
        return false;
    dir_entry_t other;
    if (lookup(drive_id, dir_cluster, t, &other, NULL, &olba, &oidx) &&
        !(olba == lba && oidx == idx))
        return false; /* another file has that name */

    g_fs_rev++;
    uint8_t cf;
    bool lfn = needs_lfn(t, &cf);
    lfn_clear(drive_id, dir_cluster, name83); /* the old long name goes */

    if (!lfn) {
        uint8_t buf[512];
        if (!fs_read_sector(drive_id, lba, buf))
            return false;
        dir_entry_t *d = &((dir_entry_t *)buf)[idx];
        fs_make_83(t, (char *)d->name); /* writes 11 + a 0 into attr... */
        d->attr = e.attr;               /* ...so put attr back */
        d->reserved =
            (uint8_t)((d->reserved & ~(CASE_LOWER_BASE | CASE_LOWER_EXT)) | cf);
        return fs_write_sector(drive_id, lba, buf);
    }

    /* free the old slot first so the new run may use it; if there's no
     * room, put the entry back (without its long name) */
    if (!free_slot(drive_id, lba, idx))
        return false;
    dir_entry_t ne = e;
    uint32_t nlba;
    int nidx;
    if (make_entry(drive_id, dir_cluster, t, &ne, &nlba, &nidx))
        return true;
    uint8_t buf[512];
    if (fs_read_sector(drive_id, lba, buf)) {
        ((dir_entry_t *)buf)[idx] = e;
        fs_write_sector(drive_id, lba, buf);
    }
    return false;
}

bool fs_rename(const char *path, const char *new_name) {
    g_fs_rev++;
    if (path_is_protected(path))
        return false;
    uint8_t drive_id;
    uint16_t dir_cluster;
    char name83[12], leaf[FS_NAME_SZ];
    bool ex;
    if (!resolve_ex(path, &drive_id, &dir_cluster, name83, leaf, &ex) || !ex)
        return false;

    char tmp_name[13];
    int ni = 0;
    for (int i = 0; i < 8 && name83[i] != ' '; i++)
        tmp_name[ni++] = name83[i];
    tmp_name[ni] = '\0';
    if (path_is_protected(tmp_name))
        return false;
    return rename_in(drive_id, dir_cluster, name83, new_name);
}

bool fs_delete(const char *path) {
    g_fs_rev++;
    if (path_is_protected(path))
        return false;
    uint8_t drive_id;
    uint16_t dir_cluster;
    char name83[12], leaf[FS_NAME_SZ];
    bool ex;
    if (!resolve_ex(path, &drive_id, &dir_cluster, name83, leaf, &ex) || !ex)
        return false;
    lfn_clear(drive_id, dir_cluster, name83);
    modify_ctx ctx = {name83, false, 0};
    dir_iterate(drive_id, dir_cluster, modify_cb, &ctx);
    return ctx.done;
}

bool fs_rmdir(const char *path) {
    g_fs_rev++;
    if (path_is_protected(path))
        return false;
    uint8_t drive_id;
    uint16_t dir_cluster;
    char name83[12], leaf[FS_NAME_SZ];
    bool ex;
    if (!resolve_ex(path, &drive_id, &dir_cluster, name83, leaf, &ex) || !ex)
        return false;
    dir_entry_t entry;
    if (!fs_find_in_dir(drive_id, dir_cluster, name83, &entry))
        return false;
    if (!(entry.attr & ATTR_DIRECTORY))
        return false;
    if (!fs_is_dir_empty(drive_id, entry.cluster_lo))
        return false;
    lfn_clear(drive_id, dir_cluster, name83);
    modify_ctx ctx = {name83, false, 2};
    dir_iterate(drive_id, dir_cluster, modify_cb, &ctx);
    return ctx.done;
}

bool fs_rm_rf(const char *path) {
    g_fs_rev++;
    if (path_is_protected(path))
        return false;
    uint8_t drive_id;
    uint16_t dir_cluster;
    char name83[12], leaf[FS_NAME_SZ];
    bool ex;
    if (!resolve_ex(path, &drive_id, &dir_cluster, name83, leaf, &ex) || !ex)
        return false;
    dir_entry_t entry;
    if (!fs_find_in_dir(drive_id, dir_cluster, name83, &entry))
        return false;
    if (!(entry.attr & ATTR_DIRECTORY))
        return false;
    /* frees the folder's own clusters too: the slot then just goes */
    fs_wipe_cluster(drive_id, entry.cluster_lo);
    lfn_clear(drive_id, dir_cluster, name83);
    uint32_t lba;
    int idx;
    if (!find83(drive_id, dir_cluster, name83, NULL, &lba, &idx))
        return false;
    return free_slot(drive_id, lba, idx);
}

/* a new file called name in dst_dir_cluster, with src's contents */
static bool copy_file_to(fat_file_t *src, uint8_t dst_drive,
                         uint16_t dst_dir_cluster, const char *name) {
    fs_seek(src, 0);
    uint16_t cluster = fat_alloc(dst_drive);
    if (cluster == 0)
        return false;
    dir_entry_t e;
    stamp_new(&e, ATTR_ARCHIVE, cluster);
    uint32_t lba;
    int idx;
    if (!make_entry(dst_drive, dst_dir_cluster, name, &e, &lba, &idx)) {
        fat_free_chain(dst_drive, cluster);
        return false;
    }

    fat_file_t dst;
    memset(&dst, 0, sizeof(fat_file_t));
    dst.entry = e;
    dst.dir_entry_lba = lba;
    dst.dir_entry_idx = idx;
    dst.cur_cluster = cluster;
    dst.dir_cluster = dst_dir_cluster;
    dst.open = true;
    dst.drive_id = dst_drive;

    uint8_t buf[512];
    int n;
    while ((n = fs_read(src, buf, sizeof(buf))) > 0) {
        if (fs_write(&dst, buf, n) != n) {
            fs_close(&dst);
            return false;
        }
    }
    fs_close(&dst);
    return true;
}

static bool copy_dir_cluster(uint8_t src_drive, uint16_t src_cluster,
                             uint8_t dst_drive, uint16_t dst_dir_cluster);

typedef struct {
    uint8_t src_drive;
    uint8_t dst_drive;
    uint16_t dst_dir_cluster;
    bool ok;
} copy_dir_ctx;

static bool copy_entry_cb(uint8_t drive_id, dir_entry_t *e, const char *lname,
                          uint32_t lba, int idx, void *user) {
    copy_dir_ctx *ctx = user;
    char name[FS_NAME_SZ];
    if (lname)
        strcpy(name, lname);
    else
        fs_entry_name(e, name);

    if (e->attr & ATTR_DIRECTORY) {
        uint16_t nc;
        if (!mkdir_in(ctx->dst_drive, ctx->dst_dir_cluster, name, &nc) ||
            !copy_dir_cluster(ctx->src_drive, e->cluster_lo, ctx->dst_drive,
                              nc)) {
            ctx->ok = false;
            return false;
        }
        return true;
    }
    fat_file_t src;
    memset(&src, 0, sizeof(src));
    src.entry = *e;
    src.cur_cluster = e->cluster_lo;
    src.dir_entry_lba = lba;
    src.dir_entry_idx = idx;
    src.open = true;
    src.drive_id = drive_id;
    if (!copy_file_to(&src, ctx->dst_drive, ctx->dst_dir_cluster, name)) {
        ctx->ok = false;
        return false;
    }
    return true;
}

static bool copy_dir_cluster(uint8_t src_drive, uint16_t src_cluster,
                             uint8_t dst_drive, uint16_t dst_dir_cluster) {
    copy_dir_ctx ctx = {src_drive, dst_drive, dst_dir_cluster, true};
    skipdot_ctx sk = {copy_entry_cb, &ctx};
    bool ok = dir_iterate(src_drive, src_cluster, skipdot_cb, &sk);
    return ok && ctx.ok;
}

bool fs_copy_file(const char *src_path, const char *dst_path) {
    fat_file_t src;
    if (!fs_open(src_path, &src))
        return false;
    uint8_t dst_drive;
    uint16_t dst_dir;
    char dst83[12], leaf[FS_NAME_SZ];
    bool ex;
    bool ok = resolve_ex(dst_path, &dst_drive, &dst_dir, dst83, leaf, &ex) &&
              !ex && copy_file_to(&src, dst_drive, dst_dir, leaf);
    src.open = false; /* read only: nothing to write back */
    return ok;
}

/* the same-disk part of a move: the entry goes to the other folder under
 * its new name; the data stays where it is */
static bool move_entry(uint8_t drive, uint16_t src_dir, const char *src83,
                       uint16_t dst_dir, const char *dst_name,
                       dir_entry_t *moved) {
    dir_entry_t e;
    uint32_t lba, nlba;
    int idx, nidx;
    if (!find83(drive, src_dir, src83, &e, &lba, &idx))
        return false;
    dir_entry_t ne = e;
    if (!make_entry(drive, dst_dir, dst_name, &ne, &nlba, &nidx))
        return false;
    lfn_clear(drive, src_dir, src83);
    if (!free_slot(drive, lba, idx))
        return false;
    if (moved)
        *moved = ne;
    return true;
}

bool fs_move_file(const char *src_path, const char *dst_path) {
    if (path_is_protected(src_path))
        return false;

    uint8_t src_drive, dst_drive;
    uint16_t src_dir, dst_dir;
    char src83[12], dst83[12], sleaf[FS_NAME_SZ], dleaf[FS_NAME_SZ];
    bool sex, dex;

    if (!resolve_ex(src_path, &src_drive, &src_dir, src83, sleaf, &sex) || !sex)
        return false;
    if (!resolve_ex(dst_path, &dst_drive, &dst_dir, dst83, dleaf, &dex))
        return false;

    dir_entry_t src_entry;
    if (!fs_find_in_dir(src_drive, src_dir, src83, &src_entry))
        return false;
    if (src_entry.attr & ATTR_DIRECTORY)
        return false;

    if (src_drive == dst_drive && src_dir == dst_dir)
        return rename_in(src_drive, src_dir, src83, dleaf);
    if (dex)
        return false;
    g_fs_rev++;
    if (src_drive == dst_drive)
        return move_entry(src_drive, src_dir, src83, dst_dir, dleaf, NULL);

    if (!fs_copy_file(src_path, dst_path))
        return false;
    return fs_delete(src_path);
}

bool fs_copy_dir(const char *src_path, const char *dst_path) {
    if (path_is_protected(src_path))
        return false;

    dir_entry_t src_entry;
    if (!fs_find(src_path, &src_entry))
        return false;
    if (!(src_entry.attr & ATTR_DIRECTORY))
        return false;

    uint8_t src_drive, dst_drive;
    uint16_t src_dir, dst_dir;
    char src83[12], dst83[12], sleaf[FS_NAME_SZ], dleaf[FS_NAME_SZ];
    bool sex, dex;
    if (!resolve_ex(dst_path, &dst_drive, &dst_dir, dst83, dleaf, &dex) || dex)
        return false;
    if (!resolve_ex(src_path, &src_drive, &src_dir, src83, sleaf, &sex))
        return false;

    g_fs_rev++;
    uint16_t nc;
    if (!mkdir_in(dst_drive, dst_dir, dleaf, &nc))
        return false;
    if (!copy_dir_cluster(src_drive, src_entry.cluster_lo, dst_drive, nc)) {
        fs_rm_rf(dst_path);
        return false;
    }
    return true;
}

bool fs_move_dir(const char *src_path, const char *dst_path) {
    if (path_is_protected(src_path))
        return false;

    uint8_t src_drive, dst_drive;
    uint16_t src_dir, dst_dir;
    char src83[12], dst83[12], sleaf[FS_NAME_SZ], dleaf[FS_NAME_SZ];
    bool sex, dex;
    uint8_t sector_buf[512];

    if (!resolve_ex(src_path, &src_drive, &src_dir, src83, sleaf, &sex) || !sex)
        return false;
    if (!resolve_ex(dst_path, &dst_drive, &dst_dir, dst83, dleaf, &dex))
        return false;

    dir_entry_t src_entry;
    if (!fs_find_in_dir(src_drive, src_dir, src83, &src_entry))
        return false;
    if (!(src_entry.attr & ATTR_DIRECTORY))
        return false;

    if (src_drive == dst_drive && src_dir == dst_dir)
        return rename_in(src_drive, src_dir, src83, dleaf);
    if (dex)
        return false;

    if (src_drive == dst_drive) {
        /* not into itself or a folder inside it */
        uint16_t c = dst_dir;
        for (int guard = 0; c != 0 && guard < 256; guard++) {
            if (c == src_entry.cluster_lo)
                return false;
            dir_entry_t up;
            if (!fs_find_in_dir(src_drive, c, "..         ", &up))
                break;
            c = up.cluster_lo;
        }
        g_fs_rev++;
        if (!move_entry(src_drive, src_dir, src83, dst_dir, dleaf, NULL))
            return false;

        uint32_t dotdot_lba =
            fat_cluster_to_lba(src_drive, src_entry.cluster_lo);
        if (!fs_read_sector(src_drive, dotdot_lba, sector_buf))
            return true;
        dir_entry_t *dot_entries = (dir_entry_t *)sector_buf;
        for (int i = 0; i < 16; i++) {
            if (dot_entries[i].name[0] == '.' &&
                dot_entries[i].name[1] == '.') {
                dot_entries[i].cluster_lo = dst_dir;
                break;
            }
        }
        fs_write_sector(src_drive, dotdot_lba, sector_buf);
        return true;
    }

    if (!fs_copy_dir(src_path, dst_path))
        return false;
    return fs_rm_rf(src_path);
}

bool fs_set_hidden(const char *path, bool hidden) {
    uint8_t drive_id;
    uint16_t dir_cluster;
    char name83[12], leaf[FS_NAME_SZ];
    bool ex;
    uint8_t sector_buf[512];
    if (!resolve_ex(path, &drive_id, &dir_cluster, name83, leaf, &ex) || !ex)
        return false;
    uint32_t lba;
    int idx;
    if (!find83(drive_id, dir_cluster, name83, NULL, &lba, &idx))
        return false;
    if (!fs_read_sector(drive_id, lba, sector_buf))
        return false;
    dir_entry_t *entries = (dir_entry_t *)sector_buf;
    if (hidden)
        entries[idx].attr |= ATTR_HIDDEN;
    else
        entries[idx].attr &= ~ATTR_HIDDEN;
    return fs_write_sector(drive_id, lba, sector_buf);
}

bool fs_is_hidden(const char *path) {
    dir_entry_t de;
    if (!fs_find(path, &de))
        return false;
    return (de.attr & ATTR_HIDDEN) != 0;
}

/* ── name checks (used by the desktop, file dialogs and the shell) ───── */
bool fs_name_char_ok(char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9'))
        return true;
    const char *ok = "!#$%&'()-@^_`{}~";
    for (int i = 0; ok[i]; i++)
        if (c == ok[i])
            return true;
    return false;
}

/* a name typed by the user: up to FS_NAME_MAX characters, spaces and more
 * than one dot allowed, not \ / : * ? " < > | */
bool fs_validate_name(const char *n, bool is_dir, const char **err) {
    (void)is_dir;
    char t[FS_NAME_SZ];
    if (!n || !n[0]) {
        *err = "Name cannot be empty.";
        return false;
    }
    int len = (int)strlen(n);
    if (len > FS_NAME_MAX) {
        *err = "Name too long (max 63).";
        return false;
    }
    if (!name_trim(n, t)) {
        *err = "Name cannot be empty.";
        return false;
    }
    if (strcmp(t, ".") == 0 || strcmp(t, "..") == 0) {
        *err = "Name not allowed.";
        return false;
    }
    for (int i = 0; n[i]; i++)
        if (!fs_long_char_ok(n[i])) {
            *err = "No \\ / : * ? \" < > |";
            return false;
        }
    return true;
}
