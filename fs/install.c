// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "fs/install.h"
#include "config/runtime_config.h"
#include "drivers/ata.h"
#include "fs/bootcode.h"
#include "fs/fs.h"
#include "fs/zip.h"
#include "lib/cpu.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"

#define PART_START 63
#define RESERVED 16 /* boot sector + stage 2 (STAGE2_MAX in bootdefs.inc) */
#define ROOT_ENTRIES 512
#define MAX_CLUSTERS 65524u
#define MIN_SECTORS 8400u /* 4.2 MB: 4085 clusters of 512 bytes, and more */

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

// the FAT16 volume inst_erase makes

typedef struct {
    uint32_t sectors;
    uint8_t spc;
    uint16_t spf;
} layout_t;

static uint16_t fat_sectors(uint32_t total, uint8_t spc) {
    uint32_t root = ROOT_ENTRIES * 32 / 512, spf = 1;
    for (;;) {
        uint32_t clusters = (total - RESERVED - 2 * spf - root) / spc;
        uint32_t need = ((clusters + 2) * 2 + 511) / 512;
        if (need <= spf)
            return (uint16_t)spf;
        spf = need;
    }
}

static uint32_t clusters_of(uint32_t total, uint8_t spc, uint16_t spf) {
    return (total - RESERVED - 2u * spf - ROOT_ENTRIES * 32 / 512) / spc;
}

/* cluster sizes as MS-DOS FORMAT picks them; at most 2 GB */
static bool plan(uint32_t disk, layout_t *l) {
    if (disk < PART_START + MIN_SECTORS)
        return false;
    uint32_t total = disk - PART_START;
    if (total > MAX_CLUSTERS * 64u + 2048u)
        total = MAX_CLUSTERS * 64u + 2048u;
    uint32_t mb = total / 2048;
    uint8_t spc = mb <= 16     ? 2
                  : mb <= 128  ? 4
                  : mb <= 256  ? 8
                  : mb <= 512  ? 16
                  : mb <= 1024 ? 32
                               : 64;
    uint16_t spf = fat_sectors(total, spc);
    if (clusters_of(total, spc, spf) < 4085) { /* small: 512-byte clusters */
        spc = 1;
        spf = fat_sectors(total, spc);
    }
    while (clusters_of(total, spc, spf) > MAX_CLUSTERS) { /* the 2 GB edge */
        total -= spc;
        spf = fat_sectors(total, spc);
    }
    l->sectors = total;
    l->spc = spc;
    l->spf = spf;
    return true;
}

// probe

void inst_probe(uint8_t unit, inst_disk_t *d) {
    memset(d, 0, sizeof(*d));
    const ata_disk_t *a = ata_unit(unit);
    if (!a || !a->present) {
        d->kind = INST_NONE;
        d->why_not = "no disk";
        return;
    }
    snprintf(d->model, sizeof(d->model), "%s", a->model);
    d->sectors = a->sectors;
    layout_t l;
    if (plan(a->sectors, &l))
        d->new_sectors = l.sectors;
    static uint8_t b[512];
    if (!ata_read_sector(unit, 0, b)) {
        d->kind = INST_OTHER;
        d->why_not = "it can't be read";
        return;
    }
    uint32_t vol = 0, len = 0;
    if (fs_bpb_valid(b)) { /* one volume from sector 0 (the old layout) */
        const bpb_t *p = (const bpb_t *)b;
        len = p->total_sectors_16 ? p->total_sectors_16 : p->total_sectors_32;
    } else if (b[510] == 0x55 && b[511] == 0xAA) {
        d->partitioned = true;
        bool any = false;
        for (int i = 0; i < 4; i++)
            any |= b[0x1BE + i * 16 + 4] != 0;
        if (!fs_pick_partition(b, 0, &vol, &len)) {
            d->kind = any ? INST_OTHER : INST_EMPTY;
            d->why_not =
                any ? "its partitions are not FAT16" : "it has no partitions";
            return;
        }
        if (!ata_read_sector(unit, vol, b) || !fs_bpb_valid(b)) {
            d->kind = INST_OTHER;
            d->why_not = "its partition is not formatted";
            return;
        }
    } else {
        d->kind = INST_EMPTY;
        d->why_not = "it is empty";
        return;
    }
    const bpb_t *p = (const bpb_t *)b;
    d->vol_lba = vol;
    d->vol_sectors = len;
    d->kind = memcmp(b + 3, "ASMOS", 5) == 0 ? INST_ASMOS : INST_FAT;
    if (p->boot_sig == 0x29) {
        memcpy(d->label, p->volume_label, 11);
        for (int i = 10; i >= 0 && d->label[i] == ' '; i--)
            d->label[i] = 0;
    }
    if (p->reserved_sectors < 1 + BOOT_STAGE2_SECTORS)
        d->why_not = "its volume has no room for the ASMOS loader "
                     "(1 reserved sector: made by DOS FORMAT?)";
    else
        d->can_keep = true;
}

// erase: partition table + empty FAT16

static void chs(uint32_t lba, uint32_t heads, uint8_t *out) {
    uint32_t c = lba / (heads * 63);
    if (c > 1023) { /* past what CHS reaches: the usual 1023/max/63 */
        out[0] = (uint8_t)(heads - 1);
        out[1] = 0xFF;
        out[2] = 0xFF;
        return;
    }
    out[0] = (uint8_t)((lba / 63) % heads);
    out[1] = (uint8_t)((lba % 63 + 1) | ((c >> 2) & 0xC0));
    out[2] = (uint8_t)c;
}

static bool wr(uint8_t unit, uint32_t lba, const uint8_t *b) {
    return ata_write_sector(unit, lba, b);
}

const char *inst_erase(uint8_t unit, inst_progress_fn cb, void *user) {
    return inst_erase_label(unit, NULL, cb, user);
}

const char *inst_erase_label(uint8_t unit, const char *label,
                             inst_progress_fn cb, void *user) {
    char lab[11]; /* upper case, space padded */
    int ln = 0;
    for (; label && label[ln] && ln < 11; ln++)
        lab[ln] = label[ln] >= 'a' && label[ln] <= 'z' ? (char)(label[ln] - 32)
                                                       : label[ln];
    bool named = ln > 0;
    for (int i = ln; i < 11; i++)
        lab[i] = ' ';
    const ata_disk_t *a = ata_unit(unit);
    layout_t l;
    if (!a || !a->present)
        return "there is no such disk";
    if (!plan(a->sectors, &l))
        return "the disk is too small (ASMOS needs 5 MB)";
    fs_unmount(unit);
    static uint8_t b[512];
    uint32_t heads = a->sectors > 1024u * 16 * 63 ? 255 : 16;

    /* the MBR: boot code, a serial number, one active FAT16 partition */
    if (cb)
        cb(user, 0, "Making the partition");
    memcpy(b, boot_mbr, 512);
    put32(b + 0x1B8, time_seconds() ^ (a->sectors << 7));
    memset(b + 0x1BE, 0, 64);
    uint8_t *e = b + 0x1BE;
    e[0] = 0x80;
    chs(PART_START, heads, e + 1);
    e[4] = l.sectors >= 65536 ? PART_TYPE_FAT16 : PART_TYPE_FAT16_SMALL;
    chs(PART_START + l.sectors - 1, heads, e + 5);
    put32(e + 8, PART_START);
    put32(e + 12, l.sectors);
    b[510] = 0x55;
    b[511] = 0xAA;
    if (!wr(unit, 0, b))
        return "could not write to the disk";
    memset(b, 0, 512);
    for (uint32_t s = 1; s < PART_START; s++) /* nothing old in the gap */
        if (!wr(unit, s, b))
            return "could not write to the disk";

    /* the volume: boot sector with the BPB (its code: inst_write_boot) */
    if (cb)
        cb(user, 5, "Formatting");
    memcpy(b, boot_vbr, 512);
    memcpy(b + 3, "ASMOS1.0", 8);
    put16(b + 0x0B, 512);
    b[0x0D] = l.spc;
    put16(b + 0x0E, RESERVED);
    b[0x10] = 2;
    put16(b + 0x11, ROOT_ENTRIES);
    put16(b + 0x13, l.sectors < 65536 ? l.sectors : 0);
    b[0x15] = 0xF8;
    put16(b + 0x16, l.spf);
    put16(b + 0x18, 63);
    put16(b + 0x1A, heads);
    put32(b + 0x1C, PART_START);
    put32(b + 0x20, l.sectors >= 65536 ? l.sectors : 0);
    b[0x24] = 0x80;
    b[0x25] = 0;
    b[0x26] = 0x29;
    put32(b + 0x27, time_seconds() * 2654435761u);
    memcpy(b + 0x2B, named ? lab : "ASMOS      ", 11);
    memcpy(b + 0x36, "FAT16   ", 8);
    if (!wr(unit, PART_START, b))
        return "could not write to the disk";

    /* reserved sectors, both FATs, the root: zero; each FAT starts with the
     * media byte and an end mark */
    uint32_t n = RESERVED - 1 + 2u * l.spf + ROOT_ENTRIES * 32 / 512;
    uint32_t lba = PART_START + 1;
    for (uint32_t i = 0; i < n; i++, lba++) {
        memset(b, 0, 512);
        uint32_t f = lba - (PART_START + RESERVED);
        if (lba >= PART_START + RESERVED && f % l.spf == 0 && f < 2u * l.spf) {
            b[0] = 0xF8;
            b[1] = b[2] = b[3] = 0xFF;
        }
        if (named && f == 2u * l.spf) { /* the root's first entry */
            memcpy(b, lab, 11);
            b[11] = 0x08; /* the volume label */
        }
        if (!wr(unit, lba, b))
            return "could not write to the disk";
        if (cb && (i & 31) == 0)
            cb(user, 5 + (int)(i * 20 / n), "Formatting");
    }
    return NULL;
}

// boot code

const char *inst_write_boot(uint8_t unit, int video_mode) {
    static uint8_t b[512], v[512];
    if (!ata_read_sector(unit, 0, b))
        return "could not read the disk";
    uint32_t vol = 0, len;
    bool parted = !fs_bpb_valid(b);
    if (parted) {
        if (!fs_pick_partition(b, 0, &vol, &len))
            return "the disk has no FAT16 partition";
        /* our MBR code, its partition table; the volume the active one */
        uint8_t m[512];
        memcpy(m, boot_mbr, 0x1B8);
        memcpy(m + 0x1B8, b + 0x1B8, 512 - 0x1B8);
        for (int i = 0; i < 4; i++) {
            uint8_t *e = m + 0x1BE + i * 16;
            e[0] = (e[4] && le32(e + 8) == vol) ? 0x80 : 0x00;
        }
        if (!wr(unit, 0, m))
            return "could not write to the disk";
    }
    if (!ata_read_sector(unit, vol, v) || !fs_bpb_valid(v))
        return "the volume's boot sector is damaged";
    if (((const bpb_t *)v)->reserved_sectors < 1 + BOOT_STAGE2_SECTORS)
        return "the volume has no room for the ASMOS loader";

    /* stage 2 in the reserved sectors; the screen mode in its byte 4 */
    if (video_mode < 0) {
        video_mode = 1;
        if (ata_read_sector(unit, vol + 1, b) && b[2] == 'V' && b[3] == 'M')
            video_mode = b[4] <= 3 ? b[4] : 1;
    }
    for (uint32_t i = 0; i < BOOT_STAGE2_SECTORS; i++) {
        memcpy(b, boot_stage2 + i * 512, 512);
        if (i == 0)
            b[4] = (uint8_t)video_mode;
        if (!wr(unit, vol + 1 + i, b))
            return "could not write to the disk";
    }
    /* our boot sector code around the volume's own BPB */
    memcpy(b, boot_vbr, 512);
    memcpy(b + 0x0B, v + 0x0B, 0x3E - 0x0B);
    memcpy(b + 3, "ASMOS1.0", 8);
    put32(b + 0x1C, vol); /* hidden sectors: where it really is */
    b[0x24] = 0x80;
    if (!wr(unit, vol, b))
        return "could not write to the disk";
    return NULL;
}

// files

typedef struct {
    inst_progress_fn cb;
    void *user;
} zprog_t;

static void zip_progress(void *user, const char *name, uint32_t done,
                         uint32_t total) {
    zprog_t *z = (zprog_t *)user;
    (void)name;
    if (z->cb)
        z->cb(z->user,
              30 + (int)(total ? (done / 16) * 70 / (total / 16 + 1) : 0),
              "Copying the system and the apps");
}

/* inst_copy_files_parts' filter, while it runs (NULL: everything) */
static zip_want_fn s_filter;
static void *s_filter_user;

const char *inst_copy_files(uint8_t drive, const char *kernel, const char *zip,
                            bool replace, inst_progress_fn cb, void *user) {
    if (!fs_drive_mounted(drive) && !fs_mount_drive(drive))
        return "the new volume does not mount";
    const char *root = g_drive_paths[drive];
    char sys[FS_PATH_SZ], dst[FS_PATH_SZ];
    snprintf(sys, sizeof(sys), "%s/SYSTEM", root);
    snprintf(dst, sizeof(dst), "%s/SYSTEM/KERNEL.SYS", root);
    dir_entry_t de;
    if (!fs_find(sys, &de) && !fs_mkdir(sys))
        return "could not make the SYSTEM folder";
    fs_set_attr(sys, ATTR_SYSTEM);

    /* the old kernel goes first (it is read-only) */
    if (fs_find(dst, &de)) {
        fs_set_attr(dst, ATTR_ARCHIVE);
        if (!fs_delete(dst))
            return "could not replace the old KERNEL.SYS";
    }
    const char *err = NULL;

    /* ASMOS.ZIP: the apps, and the kernel compressed (half the floppy to
     * read: it is the slow part) */
    if (zip) {
        if (cb)
            cb(user, 30, "Copying the system and the apps");
        zprog_t z = {cb, user};
        zip_stats_t st;
        err = zip_extract_some(zip, root, replace, s_filter, s_filter_user,
                               zip_progress, &z, &st);
        if (err)
            return err;
        if (st.bad)
            return "some files on the setup disk are damaged";
    }

    /* not in it (an older setup floppy, or SYS): the plain file */
    if (!fs_find(dst, &de)) {
        if (!kernel)
            return "there is no KERNEL.SYS on the setup disk";
        if (cb)
            cb(user, 30, "Copying the system");
        fat_file_t in, out;
        if (!fs_open(kernel, &in))
            return "KERNEL.SYS is not on the setup disk";
        uint32_t size = in.entry.file_size, done = 0;
        if (!fs_create(dst, &out)) {
            fs_close(&in);
            return "could not create KERNEL.SYS (disk full?)";
        }
        static uint8_t buf[4096];
        int n;
        while ((n = fs_read(&in, buf, sizeof(buf))) > 0) {
            if (fs_write(&out, buf, n) != n) {
                err = "could not write KERNEL.SYS (disk full?)";
                break;
            }
            done += (uint32_t)n;
            if (cb)
                cb(user, 30 + (int)(done / 1024 * 70 / (size / 1024 + 1)),
                   "Copying the system");
        }
        fs_close(&out);
        fs_close(&in);
        if (!err && done != size)
            err = "the setup disk could not be read (KERNEL.SYS)";
        if (err) {
            fs_delete(dst);
            return err;
        }
    }
    fs_set_attr(dst, ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM);

    if (cb)
        cb(user, 100, "Done");
    return NULL;
}

// the apps to choose (SETUP.INF)

static int field(const char **p, char *out, int sz) {
    int k = 0;
    while (**p && **p != '|' && **p != '\r' && **p != '\n') {
        if (k < sz - 1)
            out[k++] = **p;
        (*p)++;
    }
    out[k] = 0;
    if (**p == '|')
        (*p)++;
    return k;
}

int inst_read_parts(const char *inf, inst_part_t *parts, int max) {
    fat_file_t f;
    if (!fs_open(inf, &f))
        return 0;
    static char buf[4096];
    int n = fs_read(&f, buf, sizeof(buf) - 1);
    fs_close(&f);
    if (n <= 0)
        return 0;
    buf[n] = 0;
    int count = 0;
    for (const char *p = buf; *p && count < max;) {
        if (*p == ';' || *p == '\r' || *p == '\n') { /* (a note, a gap) */
            while (*p && *p != '\n')
                p++;
            if (*p)
                p++;
            continue;
        }
        inst_part_t *t = &parts[count];
        char size[12], group[12], kb[12];
        memset(t, 0, sizeof(*t));
        field(&p, t->app, sizeof(t->app));
        field(&p, t->name, sizeof(t->name));
        field(&p, size, sizeof(size));
        field(&p, group, sizeof(group));
        field(&p, t->path, sizeof(t->path));
        field(&p, kb, sizeof(kb));
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
        if (!t->app[0] || !t->path[0])
            continue;
        t->size = !strcmp(size, "min")      ? PART_MIN
                  : !strcmp(size, "medium") ? PART_MEDIUM
                                            : PART_FULL;
        t->group = !strcmp(group, "media")   ? PART_MEDIA
                   : !strcmp(group, "games") ? PART_GAMES
                   : !strcmp(group, "net")   ? PART_NET
                                             : PART_ANY;
        t->kb = (uint16_t)str_to_uint32(kb);
        count++;
    }
    return count;
}

static void part_file(uint8_t drive, const inst_part_t *p, char *out, int sz) {
    snprintf(out, sz, "%s/APPS/%s", g_drive_paths[drive], p->path);
}

bool inst_part_installed(uint8_t drive, const inst_part_t *p) {
    char path[FS_PATH_SZ];
    dir_entry_t de;
    part_file(drive, p, path, sizeof(path));
    return fs_find(path, &de);
}

/* which entries of ASMOS.ZIP: an app's by want[], the rest if all_else */
typedef struct {
    const inst_part_t *parts;
    int n;
    const bool *want;
    bool all_else;
} parts_filter_t;

static bool want_entry(void *user, const char *name) {
    const parts_filter_t *f = user;
    if (!strncasecmp(name, "APPS/", 5))
        for (int k = 0; k < f->n; k++)
            if (!strcasecmp(name + 5, f->parts[k].path))
                return f->want[k];
    return f->all_else;
}

const char *inst_copy_files_parts(uint8_t drive, const char *kernel,
                                  const char *zip, const inst_part_t *parts,
                                  int n, const bool *want, inst_progress_fn cb,
                                  void *user) {
    parts_filter_t f = {parts, n, want, true};
    s_filter = want_entry;
    s_filter_user = &f;
    const char *err = inst_copy_files(drive, kernel, zip, true, cb, user);
    s_filter = NULL;
    s_filter_user = NULL;
    return err;
}

const char *inst_change_parts(uint8_t drive, const char *zip,
                              const inst_part_t *parts, int n, const bool *want,
                              inst_progress_fn cb, void *user) {
    if (!fs_drive_mounted(drive))
        return "the disk is not there";
    const char *root = g_drive_paths[drive];
    /* out: the apps not wanted (their group's folder too, once empty) */
    for (int k = 0; k < n; k++) {
        if (want[k] || !inst_part_installed(drive, &parts[k]))
            continue;
        char path[FS_PATH_SZ];
        part_file(drive, &parts[k], path, sizeof(path));
        if (cb)
            cb(user, 5, "Taking out apps");
        fs_set_attr(path, ATTR_ARCHIVE);
        if (!fs_delete(path))
            return "an app could not be taken out (is it running?)";
        char *slash = strrchr(path, '/');
        if (slash && slash - path > (int)strlen(root) + 5) {
            *slash = 0; /* (its folder: gone if nothing else is in it) */
            fs_rmdir(path);
        }
    }
    /* in: the wanted ones, from the setup floppy */
    bool any = false;
    for (int k = 0; k < n; k++)
        any |= want[k] && !inst_part_installed(drive, &parts[k]);
    if (!any) {
        if (cb)
            cb(user, 100, "Done");
        return NULL;
    }
    parts_filter_t f = {parts, n, want, false};
    zprog_t z = {cb, user};
    zip_stats_t st;
    if (cb)
        cb(user, 10, "Copying apps");
    const char *err = zip_extract_some(zip, root, true, want_entry, &f,
                                       zip_progress, &z, &st);
    if (err)
        return err;
    if (st.bad)
        return "some files on the setup disk are damaged";
    if (cb)
        cb(user, 100, "Done");
    return NULL;
}

// the settings

/* the drive's ASMOS.CFG (an update), else the defaults: a first boot's
 * (the kernel makes them light on a 386 when there is no ASMOS.CFG; Setup
 * makes the file, so it does that here, on the same machine) */
static void read_cfg(uint8_t drive, os_config_t *c, char *path, int sz) {
    snprintf(path, sz, "%s%s", g_drive_paths[drive], CFG_PATH);
    os_config_t mine = g_cfg;
    cfg_init_defaults();
    if (cpu_family() <= 3)
        cfg_set_low_end(true);
    *c = g_cfg;
    g_cfg = mine;
    fat_file_t f;
    if (fs_open(path, &f)) {
        os_config_t old = *c;
        int n = fs_read(&f, &old, sizeof(old));
        fs_close(&f);
        if (n >= (int)__builtin_offsetof(os_config_t, wall_custom) &&
            old.magic == CFG_MAGIC)
            *c = old;
    }
}

static const char *write_cfg(const char *path, os_config_t *c);

bool inst_read_network(uint8_t drive) {
    char path[FS_PATH_SZ];
    os_config_t c;
    read_cfg(drive, &c, path, sizeof(path));
    return c.networking_enabled != 0;
}

const char *inst_write_network(uint8_t drive, bool on) {
    char path[FS_PATH_SZ];
    os_config_t c;
    read_cfg(drive, &c, path, sizeof(path));
    c.networking_enabled = on;
    return write_cfg(path, &c);
}

const char *inst_write_settings(uint8_t drive, const inst_settings_t *set) {
    char path[FS_PATH_SZ];
    os_config_t c;
    read_cfg(drive, &c, path, sizeof(path));
    c.networking_enabled = set->network;
    c.sound_enabled = set->sound;
    c.play_bootchime = set->chime;
    c.printer_type = set->printer_type;
    c.printer_port = set->printer_port;
    return write_cfg(path, &c);
}

static const char *write_cfg(const char *path, os_config_t *cp) {
    os_config_t c = *cp;
    fat_file_t f;
    c.magic = CFG_MAGIC;
    c.version = CFG_VERSION;
    dir_entry_t de;
    if (fs_find(path, &de))
        fs_delete(path);
    if (!fs_create(path, &f))
        return "could not save the settings";
    int w = fs_write(&f, &c, sizeof(c));
    fs_close(&f);
    return w == (int)sizeof(c) ? NULL : "could not save the settings";
}
