#include "fs/fat16.h"
#include "fs/ata.h"
#include "fs/fdd.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"

fat16_fs_t fs;
fat16_fs_t g_drives[DRIVE_COUNT];
static uint8_t s_current_drive = DRIVE_HDA;

fat16_dir_context_t dir_context = {
    .current_cluster = 0, .path = "/", .drive_id = DRIVE_HDA};

static uint8_t sector_buf[512];

const char *g_protected_paths[PROTECTED_PATH_COUNT] = {
    "DESKTOP", "/DESKTOP", "FDD0", "/FDD0", "FDD1", "/FDD1", "HDB", "/HDB"};

fat16_mount_point_t g_mount_points[] = {
    {"FDD0", DRIVE_FDD0}, {"FDD1", DRIVE_FDD1}, {"HDB", DRIVE_HDB}, {NULL, 0}};

bool path_is_protected(const char *name_or_path) {
    if (!name_or_path)
        return false;
    for (int i = 0; i < PROTECTED_PATH_COUNT; i++) {
        if (strcasecmp(name_or_path, g_protected_paths[i]) == 0)
            return true;
    }
    if (strcasecmp(name_or_path, "/DESKTOP") == 0)
        return true;
    if (strcasecmp(name_or_path, "/DESKTOP/") == 0)
        return true;
    return false;
}

bool fat16_is_mount_point(const char *name, uint8_t *drive_id_out) {
    for (int i = 0; g_mount_points[i].name != NULL; i++) {
        if (strcasecmp(name, g_mount_points[i].name) == 0) {
            if (fat16_drive_mounted(g_mount_points[i].drive_id)) {
                *drive_id_out = g_mount_points[i].drive_id;
                return true;
            }
        }
    }
    return false;
}

/* ── low-level sector I/O (drive-aware) ───────────────────────────────── */

static bool drive_read_sector(uint8_t drive_id, uint32_t lba, void *buf) {
    switch (drive_id) {
    case DRIVE_HDA:
    case DRIVE_HDB:
        return ata_read_sector(drive_id, lba, buf);
    case DRIVE_FDD0:
        return fdd_read_sector(0, lba, buf);
    case DRIVE_FDD1:
        return fdd_read_sector(1, lba, buf);
    default:
        return false;
    }
}

static bool drive_write_sector(uint8_t drive_id, uint32_t lba,
                               const void *buf) {
    switch (drive_id) {
    case DRIVE_HDA:
    case DRIVE_HDB:
        return ata_write_sector(drive_id, lba, buf);
    case DRIVE_FDD0:
        return fdd_write_sector(0, lba, buf);
    case DRIVE_FDD1:
        return fdd_write_sector(1, lba, buf);
    default:
        return false;
    }
}

static inline bool rd(uint32_t lba, void *buf) {
    return drive_read_sector(fs.drive_id, lba, buf);
}
static inline bool wr(uint32_t lba, const void *buf) {
    return drive_write_sector(fs.drive_id, lba, buf);
}

/* ── internal helpers ─────────────────────────────────────────────────── */

static bool cluster_valid(uint16_t cluster) {
    if (cluster < 2)
        return false;
    if (cluster >= (uint16_t)(fs.cluster_count + 2))
        return false;

    uint16_t reserved =
        (fs.fat_type == FAT_TYPE_FAT12) ? FAT12_RESERVED : FAT16_RESERVED;
    if (cluster >= reserved)
        return false;
    return true;
}

static uint32_t cluster_to_lba(uint16_t cluster) {
    if (!cluster_valid(cluster))
        return 0xFFFFFFFF;
    return fs.data_lba + (cluster - 2) * fs.bpb.sectors_per_cluster;
}

static uint16_t fat_get(uint16_t cluster) {
    if (cluster < 2 || cluster >= (uint16_t)(fs.cluster_count + 2))
        return FAT16_BAD;

    if (fs.fat_type == FAT_TYPE_FAT12) {
        uint32_t bit_offset = cluster * 12;
        uint32_t byte_offset = bit_offset / 8;
        uint32_t fat_sector = fs.fat_lba + byte_offset / 512;
        uint32_t off_in_sec = byte_offset % 512;

        uint8_t buf0[512], buf1[512];
        if (!rd(fat_sector, buf0))
            return FAT16_BAD;

        uint16_t val;
        if (off_in_sec == 511) {
            if (!rd(fat_sector + 1, buf1))
                return FAT16_BAD;
            val = buf0[511] | ((uint16_t)buf1[0] << 8);
        } else {
            val = buf0[off_in_sec] | ((uint16_t)buf0[off_in_sec + 1] << 8);
        }

        if (cluster & 1)
            val >>= 4;
        else
            val &= 0x0FFF;

        if (val >= FAT12_RESERVED)
            return FAT16_EOC;
        if (val == FAT12_BAD)
            return FAT16_BAD;
        return val;
    }

    uint32_t fat_sector = fs.fat_lba + (cluster / 256);
    uint16_t fat_offset = (cluster % 256) * 2;
    uint8_t local_buf[512];
    if (!rd(fat_sector, local_buf))
        return FAT16_BAD;
    return *(uint16_t *)(local_buf + fat_offset);
}

static bool fat_set(uint16_t cluster, uint16_t value) {
    if (cluster < 2 || cluster >= (uint16_t)(fs.cluster_count + 2))
        return false;

    if (fs.fat_type == FAT_TYPE_FAT12) {
        uint16_t val12 = value;
        if (value == FAT16_FREE)
            val12 = FAT12_FREE;
        else if (value >= FAT16_EOC)
            val12 = 0xFFF;
        else if (value == FAT16_BAD)
            val12 = FAT12_BAD;

        uint32_t bit_offset = cluster * 12;
        uint32_t byte_offset = bit_offset / 8;
        uint32_t fat_sector = fs.fat_lba + byte_offset / 512;
        uint32_t off_in_sec = byte_offset % 512;

        for (int copy = 0; copy < fs.bpb.fat_count; copy++) {
            uint32_t lba0 =
                fs.fat_lba + copy * fs.bpb.sectors_per_fat + byte_offset / 512;

            uint8_t buf0[512], buf1[512];
            bool straddle = (off_in_sec == 511);

            if (!rd(lba0, buf0))
                return false;
            if (straddle && !rd(lba0 + 1, buf1))
                return false;

            uint8_t *lo = straddle ? &buf0[511] : &buf0[off_in_sec];
            uint8_t *hi = straddle ? &buf1[0] : &buf0[off_in_sec + 1];

            if (cluster & 1) {
                *lo = (*lo & 0x0F) | ((val12 << 4) & 0xF0);
                *hi = (val12 >> 4) & 0xFF;
            } else {
                *lo = val12 & 0xFF;
                *hi = (*hi & 0xF0) | ((val12 >> 8) & 0x0F);
            }

            if (!wr(lba0, buf0))
                return false;
            if (straddle && !wr(lba0 + 1, buf1))
                return false;
            (void)fat_sector;
        }
        return true;
    }

    uint32_t fat_sector = cluster / 256;
    uint16_t fat_offset = (cluster % 256) * 2;
    uint8_t local_buf[512];
    for (int copy = 0; copy < fs.bpb.fat_count; copy++) {
        uint32_t lba = fs.fat_lba + copy * fs.bpb.sectors_per_fat + fat_sector;
        if (!rd(lba, local_buf))
            return false;
        *(uint16_t *)(local_buf + fat_offset) = value;
        if (!wr(lba, local_buf))
            return false;
    }
    return true;
}

static uint16_t fat_alloc(void) {
    for (uint16_t c = 2; c < (uint16_t)(fs.cluster_count + 2); c++) {
        if (fat_get(c) == FAT16_FREE) {
            if (!fat_set(c, FAT16_EOC))
                return 0;
            return c;
        }
    }
    return 0;
}

static void fat_free_chain(uint16_t cluster) {
    while (cluster >= 2 && cluster < FAT16_RESERVED) {
        uint16_t next = fat_get(cluster);
        fat_set(cluster, FAT16_FREE);
        cluster = next;
    }
}

static uint16_t now_fat_time(void) { return time_from_rtc(); }
static uint16_t now_fat_date(void) { return date_from_rtc(); }

static bool name83_eq(const uint8_t *entry_name, const char *name83) {
    for (int i = 0; i < 11; i++)
        if (entry_name[i] != (uint8_t)name83[i])
            return false;
    return true;
}

typedef bool (*dir_cb)(dir_entry_t *entry, uint32_t lba, int idx, void *user);

static bool dir_iterate(uint16_t dir_cluster, dir_cb cb, void *user) {
    uint32_t lba;
    if (dir_cluster == 0) {
        uint32_t root_sectors = (fs.bpb.root_entry_count * 32 + 511) / 512;
        for (uint32_t s = 0; s < root_sectors; s++) {
            lba = fs.root_lba + s;
            if (!rd(lba, sector_buf))
                return false;
            dir_entry_t *entries = (dir_entry_t *)sector_buf;
            for (int i = 0; i < 16; i++) {
                if (entries[i].name[0] == DIR_ENTRY_END)
                    return true;
                if (entries[i].name[0] == DIR_ENTRY_FREE)
                    continue;
                if (entries[i].attr & ATTR_VOLUME_ID)
                    continue;
                if (!cb(&entries[i], lba, i, user))
                    return false;
            }
        }
    } else {
        uint16_t cluster = dir_cluster;
        while (cluster >= 2 && cluster < FAT16_RESERVED) {
            uint32_t start_lba = cluster_to_lba(cluster);
            for (uint32_t s = 0; s < fs.bpb.sectors_per_cluster; s++) {
                lba = start_lba + s;
                if (!rd(lba, sector_buf))
                    return false;
                dir_entry_t *entries = (dir_entry_t *)sector_buf;
                for (int i = 0; i < 16; i++) {
                    if (entries[i].name[0] == DIR_ENTRY_END)
                        return true;
                    if (entries[i].name[0] == DIR_ENTRY_FREE)
                        continue;
                    if (entries[i].attr & ATTR_VOLUME_ID)
                        continue;
                    if (!cb(&entries[i], lba, i, user))
                        return false;
                }
            }
            cluster = fat_get(cluster);
        }
    }
    return true;
}

typedef bool (*cluster_cb)(dir_entry_t *e, uint16_t cluster, int idx,
                           void *user);
static bool cluster_iterate(uint16_t cluster, cluster_cb cb, void *user) {
    while (cluster >= 2 && cluster < FAT16_RESERVED) {
        uint32_t start_lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < fs.bpb.sectors_per_cluster; s++) {
            if (!rd(start_lba + s, sector_buf))
                return false;
            dir_entry_t *entries = (dir_entry_t *)sector_buf;
            for (int i = 0; i < 16; i++) {
                uint8_t first = entries[i].name[0];
                if (first == DIR_ENTRY_END)
                    return true;
                if (first == DIR_ENTRY_FREE)
                    continue;
                if (entries[i].attr & ATTR_VOLUME_ID)
                    continue;
                if (entries[i].name[0] == '.')
                    continue;
                if (!cb(&entries[i], cluster, i, user))
                    return false;
            }
        }
        cluster = fat_get(cluster);
    }
    return true;
}

static bool alloc_dir_entry(uint16_t dir_cluster, uint32_t *out_lba,
                            int *out_idx) {
    uint32_t lba;
    if (dir_cluster == 0) {
        uint32_t root_sectors = (fs.bpb.root_entry_count * 32 + 511) / 512;
        for (uint32_t s = 0; s < root_sectors; s++) {
            lba = fs.root_lba + s;
            if (!rd(lba, sector_buf))
                return false;
            dir_entry_t *entries = (dir_entry_t *)sector_buf;
            for (int i = 0; i < 16; i++) {
                if (entries[i].name[0] == DIR_ENTRY_FREE ||
                    entries[i].name[0] == DIR_ENTRY_END) {
                    *out_lba = lba;
                    *out_idx = i;
                    return true;
                }
            }
        }
    } else {
        uint16_t cluster = dir_cluster, prev = 0;
        while (cluster >= 2 && cluster < FAT16_RESERVED) {
            uint32_t start_lba = cluster_to_lba(cluster);
            for (uint32_t s = 0; s < fs.bpb.sectors_per_cluster; s++) {
                lba = start_lba + s;
                if (!rd(lba, sector_buf))
                    return false;
                dir_entry_t *entries = (dir_entry_t *)sector_buf;
                for (int i = 0; i < 16; i++) {
                    if (entries[i].name[0] == DIR_ENTRY_FREE ||
                        entries[i].name[0] == DIR_ENTRY_END) {
                        *out_lba = lba;
                        *out_idx = i;
                        return true;
                    }
                }
            }
            prev = cluster;
            cluster = fat_get(cluster);
        }
        uint16_t new_cluster = fat_alloc();
        if (new_cluster == 0)
            return false;
        if (prev)
            fat_set(prev, new_cluster);
        uint32_t new_lba = cluster_to_lba(new_cluster);
        uint8_t zero[512];
        memset(zero, 0, 512);
        for (uint32_t s = 0; s < fs.bpb.sectors_per_cluster; s++)
            wr(new_lba + s, zero);
        *out_lba = new_lba;
        *out_idx = 0;
        return true;
    }
    return false;
}

typedef struct {
    const char *name;
    dir_entry_t *out;
    bool found;
} find_ctx;
static bool find_cb(dir_entry_t *e, uint32_t lba, int idx, void *user) {
    (void)lba;
    (void)idx;
    find_ctx *ctx = (find_ctx *)user;
    if (name83_eq(e->name, ctx->name)) {
        *ctx->out = *e;
        ctx->found = true;
        return false;
    }
    return true;
}

typedef struct {
    dir_entry_t *buf;
    int max;
    int count;
} list_ctx;
static bool list_cb(dir_entry_t *e, uint32_t lba, int idx, void *user) {
    (void)lba;
    (void)idx;
    list_ctx *ctx = (list_ctx *)user;
    if (ctx->count < ctx->max)
        ctx->buf[ctx->count++] = *e;
    return true;
}

typedef struct {
    const char *name;
    bool done;
    const char *new_name;
    int action;
} modify_ctx;
static bool modify_cb(dir_entry_t *e, uint32_t lba, int idx, void *user) {
    modify_ctx *ctx = (modify_ctx *)user;
    if (name83_eq(e->name, ctx->name)) {
        if (ctx->action == 0 || ctx->action == 2) {
            fat_free_chain(e->cluster_lo);
            e->name[0] = DIR_ENTRY_FREE;
        } else if (ctx->action == 1)
            memcpy(e->name, ctx->new_name, 11);
        wr(lba, sector_buf);
        ctx->done = true;
        return false;
    }
    return true;
}

typedef struct {
    const char *name83;
    uint32_t lba;
    int idx;
    bool found;
} locate_ctx;
static bool locate_cb(dir_entry_t *e, uint32_t lba, int idx, void *user) {
    locate_ctx *ctx = (locate_ctx *)user;
    if (name83_eq(e->name, ctx->name83)) {
        ctx->lba = lba;
        ctx->idx = idx;
        ctx->found = true;
        return false;
    }
    return true;
}

static bool _copy_open_file(fat16_file_t *src, const char *dest_name83) {
    fat16_file_t dst;
    if (!fat16_create(dest_name83, &dst))
        return false;
    uint8_t buf[512];
    int n;
    while ((n = fat16_read(src, buf, 512)) > 0) {
        if (fat16_write(&dst, buf, n) != n) {
            fat16_close(&dst);
            fat16_delete(dest_name83);
            return false;
        }
    }
    fat16_close(&dst);
    return true;
}

static bool _copy_dir_cluster(uint16_t src_cluster, const char *dest_name83);
static bool _copy_dir_cluster_drive(uint8_t src_drive, uint16_t src_cluster,
                                    uint8_t dst_drive, const char *dst_path);
static bool _copy_dir_entry_cb(dir_entry_t *e, uint16_t cluster, int idx,
                               void *user) {
    (void)cluster;
    (void)idx;
    (void)user;
    char child83[12];
    for (int i = 0; i < 11; i++)
        child83[i] = (char)e->name[i];
    child83[11] = '\0';
    if (e->attr & ATTR_DIRECTORY) {
        if (!fat16_mkdir(child83))
            return false;
        if (!_copy_dir_cluster(e->cluster_lo, child83))
            return false;
    } else {
        fat16_file_t src;
        if (!fat16_open(child83, &src))
            return false;
        bool ok = _copy_open_file(&src, child83);
        fat16_close(&src);
        if (!ok)
            return false;
    }
    return true;
}
static bool _copy_dir_entry_cb_drive(dir_entry_t *e, uint16_t cluster, int idx,
                                     void *user) {
    (void)cluster;
    (void)idx;
    struct copy_ctx {
        uint8_t src_drive;
        uint8_t dst_drive;
        const char *dst_path;
    } *ctx = (struct copy_ctx *)user;

    char child83[12];
    for (int i = 0; i < 11; i++)
        child83[i] = (char)e->name[i];
    child83[11] = '\0';

    if (e->attr & ATTR_DIRECTORY) {
        uint8_t saved = fat16_current_drive();
        fat16_select_drive(ctx->dst_drive);
        char full_child[270];
        snprintf(full_child, sizeof(full_child), "%s/%s", ctx->dst_path,
                 child83);
        if (!fat16_mkdir(child83)) {
            fat16_select_drive(saved);
            return false;
        }
        fat16_select_drive(saved);
        if (!_copy_dir_cluster_drive(ctx->src_drive, e->cluster_lo,
                                     ctx->dst_drive, full_child))
            return false;
    } else {
        uint8_t saved = fat16_current_drive();
        fat16_select_drive(ctx->src_drive);
        fat16_file_t src;
        if (!fat16_open(child83, &src)) {
            fat16_select_drive(saved);
            return false;
        }
        fat16_select_drive(ctx->dst_drive);
        bool ok = _copy_open_file(&src, child83);
        fat16_close(&src);
        fat16_select_drive(saved);
        if (!ok)
            return false;
    }
    return true;
}
static bool _copy_dir_cluster(uint16_t src_cluster, const char *dest_name83) {
    (void)dest_name83;
    return cluster_iterate(src_cluster, _copy_dir_entry_cb, NULL);
}
static bool _copy_dir_cluster_drive(uint8_t src_drive, uint16_t src_cluster,
                                    uint8_t dst_drive, const char *dst_path) {
    struct {
        uint8_t src_drive;
        uint8_t dst_drive;
        const char *dst_path;
    } ctx = {src_drive, dst_drive, dst_path};
    uint8_t saved = fat16_current_drive();
    fat16_select_drive(src_drive);
    bool ok = cluster_iterate(src_cluster, _copy_dir_entry_cb_drive, &ctx);
    fat16_select_drive(saved);
    return ok;
}

/* ── drive management ─────────────────────────────────────────────────── */

static void extract_volume_label(fat16_fs_t *vol) {
    int j = 0;
    for (int i = 0; i < 11; i++) {
        char c = (char)vol->bpb.volume_label[i];
        if (c == ' ' || c == '\0')
            break;
        vol->label[j++] = c;
    }
    vol->label[j] = '\0';
    if (j == 0) {
        uint8_t buf[512];
        if (rd(vol->root_lba, buf)) {
            dir_entry_t *entries = (dir_entry_t *)buf;
            for (int i = 0; i < 16; i++) {
                if (entries[i].name[0] == DIR_ENTRY_END)
                    break;
                if (entries[i].attr & ATTR_VOLUME_ID) {
                    int k = 0;
                    for (int m = 0; m < 8 && entries[i].name[m] != ' '; m++)
                        vol->label[k++] = entries[i].name[m];
                    for (int m = 0; m < 3 && entries[i].ext[m] != ' '; m++)
                        vol->label[k++] = entries[i].ext[m];
                    vol->label[k] = '\0';
                    break;
                }
            }
        }
    }
    if (vol->label[0] == '\0') {
        const char *names[] = {"HDA", "HDB", "FDD0", "FDD1"};
        strncpy(vol->label, names[vol->drive_id], 11);
    }
}

bool fat16_mount_drive(uint8_t drive_id) {
    if (drive_id >= DRIVE_COUNT)
        return false;
    fat16_fs_t *vol = &g_drives[drive_id];
    uint8_t saved_id = fs.drive_id;

    fs.drive_id = drive_id;

    uint8_t tmp_buf[512];
    if (!rd(0, tmp_buf)) {
        fs.drive_id = saved_id;
        vol->mounted = false;
        return false;
    }

    memcpy(&vol->bpb, tmp_buf, sizeof(bpb_t));

    if (vol->bpb.bytes_per_sector != 512) {
        fs.drive_id = saved_id;
        vol->mounted = false;
        return false;
    }

    vol->fat_lba = vol->bpb.reserved_sectors;
    vol->root_lba =
        vol->fat_lba + vol->bpb.fat_count * vol->bpb.sectors_per_fat;
    uint32_t root_sectors = (vol->bpb.root_entry_count * 32 + 511) / 512;
    vol->data_lba = vol->root_lba + root_sectors;
    uint32_t total = vol->bpb.total_sectors_16 ? vol->bpb.total_sectors_16
                                               : vol->bpb.total_sectors_32;
    vol->cluster_count = (total - vol->data_lba) / vol->bpb.sectors_per_cluster;
    vol->drive_id = drive_id;
    vol->mounted = true;

    if (vol->cluster_count < 4085)
        vol->fat_type = FAT_TYPE_FAT12;
    else
        vol->fat_type = FAT_TYPE_FAT16;

    extract_volume_label(vol);

    fs.drive_id = saved_id;
    return true;
}

bool fat16_select_drive(uint8_t drive_id) {
    if (drive_id >= DRIVE_COUNT)
        return false;
    if (!g_drives[drive_id].mounted)
        return false;
    s_current_drive = drive_id;
    fs = g_drives[drive_id];
    dir_context.current_cluster = 0;
    dir_context.drive_id = drive_id;
    strncpy(dir_context.path, "/", 255);
    return true;
}

uint8_t fat16_current_drive(void) { return s_current_drive; }

const char *fat16_drive_label(uint8_t drive_id) {
    if (drive_id >= DRIVE_COUNT || !g_drives[drive_id].mounted)
        return "";
    return g_drives[drive_id].label;
}

bool fat16_drive_mounted(uint8_t drive_id) {
    if (drive_id >= DRIVE_COUNT)
        return false;
    return g_drives[drive_id].mounted;
}

/* ── filesystem init ──────────────────────────────────────────────────── */

bool fat16_mount(void) {
    memset(g_drives, 0, sizeof(g_drives));
    fs.drive_id = DRIVE_HDA;

    if (!fat16_mount_drive(DRIVE_HDA))
        return false;
    fs = g_drives[DRIVE_HDA];
    s_current_drive = DRIVE_HDA;

    fat16_mount_drive(DRIVE_HDB);

    if (fdd_init(0)) {
        fat16_mount_drive(DRIVE_FDD0);
    }
    if (fdd_init(1)) {
        fat16_mount_drive(DRIVE_FDD1);
    }

    return true;
}

bool fat16_get_usage(uint32_t *total_bytes, uint32_t *used_bytes) {
    if (!fs.mounted)
        return false;
    *total_bytes = (fs.bpb.total_sectors_32 ? fs.bpb.total_sectors_32
                                            : fs.bpb.total_sectors_16) *
                   fs.bpb.bytes_per_sector;
    uint32_t used_clusters = 0;

    for (uint16_t c = 2; c < (uint16_t)(fs.cluster_count + 2); c++) {
        uint16_t v = fat_get(c);
        if (v != FAT16_FREE && v < FAT16_EOC && v != FAT16_BAD)
            used_clusters++;
        if (v >= FAT16_EOC)
            used_clusters++;
    }

    *used_bytes =
        used_clusters * fs.bpb.bytes_per_sector * fs.bpb.sectors_per_cluster;
    return true;
}

/* ── path resolution ──────────────────────────────────────────────────── */
void fat16_make_83(const char *filename, char *out83) {
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

bool fat16_resolve(const char *path, uint16_t *out_parent_cluster,
                   char *out_name83) {
    if (!path || !*path)
        return false;
    uint16_t current = dir_context.current_cluster;
    const char *ptr = path;
    if (*ptr == '/') {
        current = 0;
        ptr++;
        if (!*ptr) {
            *out_parent_cluster = 0;
            memset(out_name83, ' ', 11);
            return true;
        }
    }
    char component[256];
    uint16_t parent = current;
    while (true) {
        int i = 0;
        while (*ptr && *ptr != '/' && i < 255)
            component[i++] = *ptr++;
        component[i] = '\0';
        while (*ptr == '/')
            ptr++;
        if (*ptr == '\0') {
            fat16_make_83(component, out_name83);
            *out_parent_cluster = parent;
            return true;
        }
        uint8_t mount_drive;
        if (fat16_is_mount_point(component, &mount_drive)) {
            uint8_t saved = s_current_drive;
            fat16_select_drive(mount_drive);
            parent = 0;
            continue;
        }
        char name83[12];
        fat16_make_83(component, name83);
        dir_entry_t entry;
        if (!fat16_find_in_dir(parent, name83, &entry))
            return false;
        if (!(entry.attr & ATTR_DIRECTORY))
            return false;
        parent = entry.cluster_lo;
    }
}

bool fat16_chdir(const char *path) {
    if (!path || !*path)
        return false;
    if (strcmp(path, "/") == 0) {
        dir_context.current_cluster = 0;
        strcpy(dir_context.path, "/");
        return true;
    }
    uint16_t target_parent;
    char target_name83[12];
    if (!fat16_resolve(path, &target_parent, target_name83))
        return false;
    dir_entry_t entry;
    if (!fat16_find_in_dir(target_parent, target_name83, &entry))
        return false;
    if (!(entry.attr & ATTR_DIRECTORY))
        return false;
    dir_context.current_cluster = entry.cluster_lo;
    char new_path[256];
    if (path[0] == '/')
        strncpy(new_path, path, 255);
    else {
        strcpy(new_path, dir_context.path);
        if (strcmp(new_path, "/") != 0)
            strcat(new_path, "/");
        strcat(new_path, path);
    }
    new_path[255] = '\0';
    char *comps[32];
    int comp_len[32];
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
        if (len == 1 && start[0] == '.') {
            continue;
        } else if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (top > 0)
                top--;
        } else if (top < 32) {
            comps[top] = start;
            comp_len[top] = len;
            top++;
        }
    }
    if (top == 0) {
        strcpy(dir_context.path, "/");
    } else {
        int out_idx = 0;
        for (int i = 0; i < top; i++) {
            dir_context.path[out_idx++] = '/';
            for (int j = 0; j < comp_len[i]; j++)
                dir_context.path[out_idx++] = comps[i][j];
        }
        dir_context.path[out_idx] = '\0';
    }
    return true;
}

bool fat16_pwd(char *buf, int buflen) {
    if (!buf || buflen < 2)
        return false;
    strncpy(buf, dir_context.path, buflen - 1);
    buf[buflen - 1] = '\0';
    return true;
}

bool fat16_find_in_dir(uint16_t dir_cluster, const char *name83,
                       dir_entry_t *out) {
    find_ctx ctx = {name83, out, false};
    dir_iterate(dir_cluster, find_cb, &ctx);
    return ctx.found;
}

bool fat16_find(const char *path, dir_entry_t *out) {
    uint16_t dir_cluster;
    char name83[12];
    if (!fat16_resolve(path, &dir_cluster, name83))
        return false;
    return fat16_find_in_dir(dir_cluster, name83, out);
}

bool fat16_list_dir(uint16_t dir_cluster, dir_entry_t *buf, int max,
                    int *count) {
    list_ctx ctx = {buf, max, 0};
    bool ok = dir_iterate(dir_cluster, list_cb, &ctx);
    *count = ctx.count;
    return ok;
}

bool is_dir_empty(uint16_t cluster) {
    if (cluster == 0)
        return false;
    uint16_t cur = cluster;
    int depth = 0;
    while (cur >= 2 && cur < FAT16_RESERVED) {
        uint32_t start_lba = cluster_to_lba(cur);
        for (uint32_t s = 0; s < fs.bpb.sectors_per_cluster; s++) {
            if (!rd(start_lba + s, sector_buf))
                return false;
            dir_entry_t *entries = (dir_entry_t *)sector_buf;
            for (int i = 0; i < 16; i++) {
                uint8_t first = entries[i].name[0];
                if (first == DIR_ENTRY_END)
                    return true;
                if (first == DIR_ENTRY_FREE)
                    continue;
                if (entries[i].name[0] == '.')
                    continue;
                return false;
            }
        }
        cur = fat_get(cur);
        depth++;
    }
    return true;
}

void fat16_wipe_cluster(uint16_t cluster) {
    if (cluster == 0 || cluster >= FAT16_RESERVED)
        return;
    uint16_t cur = cluster;
    while (cur >= 2 && cur < FAT16_RESERVED) {
        uint32_t start_lba = cluster_to_lba(cur);
        for (uint32_t s = 0; s < fs.bpb.sectors_per_cluster; s++) {
            uint8_t local_buf[512];
            if (!rd(start_lba + s, local_buf))
                return;
            dir_entry_t *entries = (dir_entry_t *)local_buf;
            for (int i = 0; i < 16; i++) {
                uint8_t first = entries[i].name[0];
                if (first == DIR_ENTRY_END) {
                    fat_free_chain(cluster);
                    return;
                }
                if (first == DIR_ENTRY_FREE)
                    continue;
                if (entries[i].name[0] == '.')
                    continue;
                if (entries[i].attr & ATTR_DIRECTORY)
                    fat16_wipe_cluster(entries[i].cluster_lo);
                else
                    fat_free_chain(entries[i].cluster_lo);
            }
        }
        cur = fat_get(cur);
    }
}

/* ── file operations ──────────────────────────────────────────────────── */
bool fat16_open(const char *path, fat16_file_t *f) {
    uint16_t dir_cluster;
    char name83[12];
    if (!fat16_resolve(path, &dir_cluster, name83))
        return false;
    dir_entry_t entry;
    if (!fat16_find_in_dir(dir_cluster, name83, &entry))
        return false;
    locate_ctx loc = {name83, 0, 0, false};
    dir_iterate(dir_cluster, locate_cb, &loc);
    if (!loc.found)
        return false;
    memset(f, 0, sizeof(fat16_file_t));
    f->entry = entry;
    f->cur_cluster = entry.cluster_lo;
    f->dir_cluster = dir_cluster;
    f->dir_entry_lba = loc.lba;
    f->dir_entry_idx = loc.idx;
    f->open = true;
    f->drive_id = fs.drive_id;
    return true;
}

bool fat16_create(const char *path, fat16_file_t *f) {
    uint16_t dir_cluster;
    char name83[12];
    if (!fat16_resolve(path, &dir_cluster, name83))
        return false;
    dir_entry_t existing;
    if (fat16_find_in_dir(dir_cluster, name83, &existing))
        return false;
    uint32_t lba;
    int idx;
    if (!alloc_dir_entry(dir_cluster, &lba, &idx))
        return false;
    uint16_t cluster = fat_alloc();
    if (cluster == 0)
        return false;
    if (!rd(lba, sector_buf))
        return false;
    dir_entry_t *entries = (dir_entry_t *)sector_buf;
    uint8_t first = entries[idx].name[0];
    memset(&entries[idx], 0, sizeof(dir_entry_t));
    memcpy(entries[idx].name, name83, 11);
    entries[idx].attr = ATTR_ARCHIVE;
    entries[idx].cluster_lo = cluster;
    entries[idx].file_size = 0;
    entries[idx].create_time = now_fat_time();
    entries[idx].create_date = now_fat_date();
    entries[idx].modify_time = entries[idx].create_time;
    entries[idx].modify_date = entries[idx].create_date;
    if (first == DIR_ENTRY_END && idx + 1 < 16)
        entries[idx + 1].name[0] = DIR_ENTRY_END;
    if (!wr(lba, sector_buf))
        return false;
    memset(f, 0, sizeof(fat16_file_t));
    f->entry = entries[idx];
    f->dir_entry_lba = lba;
    f->dir_entry_idx = idx;
    f->cur_cluster = cluster;
    f->dir_cluster = dir_cluster;
    f->open = true;
    f->drive_id = fs.drive_id;
    return true;
}

int fat16_read(fat16_file_t *f, void *buf, int len) {
    if (!f->open)
        return -1;
    uint8_t local_buf[512];
    uint8_t *out = (uint8_t *)buf;
    int total = 0;
    uint32_t filesize = f->entry.file_size;
    while (len > 0 && f->cur_offset < filesize) {
        if (f->cur_cluster < 2 || f->cur_cluster >= FAT16_RESERVED)
            break;
        uint32_t cluster_bytes = fs.bpb.sectors_per_cluster * 512;
        uint32_t offset_in_cluster = f->cur_offset % cluster_bytes;
        uint32_t sector_in_cluster = offset_in_cluster / 512;
        uint32_t offset_in_sector = offset_in_cluster % 512;
        uint32_t lba = cluster_to_lba(f->cur_cluster);
        if (lba == 0xFFFFFFFF)
            break;
        lba += sector_in_cluster;
        if (!rd(lba, local_buf))
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
        if (f->cur_offset % cluster_bytes == 0) {
            uint16_t next = fat_get(f->cur_cluster);
            f->cur_cluster = (next >= FAT16_EOC) ? 0xFFFF : next;
        }
    }
    return total;
}

int fat16_write(fat16_file_t *f, const void *buf, int len) {
    if (!f->open)
        return -1;
    const uint8_t *in = (const uint8_t *)buf;
    int total = 0;
    while (len > 0) {
        if (f->cur_cluster < 2 || f->cur_cluster >= FAT16_RESERVED) {
            uint16_t new_cluster = fat_alloc();
            if (new_cluster == 0)
                break;
            if (f->cur_offset > 0) {
                uint16_t temp_cluster = f->entry.cluster_lo;
                while (temp_cluster >= 2 && temp_cluster < FAT16_RESERVED) {
                    uint16_t next = fat_get(temp_cluster);
                    if (next == 0 || next >= FAT16_EOC) {
                        fat_set(temp_cluster, new_cluster);
                        break;
                    }
                    temp_cluster = next;
                }
            } else {
                f->entry.cluster_lo = new_cluster;
            }
            f->cur_cluster = new_cluster;
        }
        uint32_t cluster_bytes = fs.bpb.sectors_per_cluster * 512;
        uint32_t offset_in_cluster = f->cur_offset % cluster_bytes;
        uint32_t sector_in_cluster = offset_in_cluster / 512;
        uint32_t offset_in_sector = offset_in_cluster % 512;
        uint32_t lba = cluster_to_lba(f->cur_cluster) + sector_in_cluster;
        if (offset_in_sector != 0 || len < 512) {
            if (!rd(lba, sector_buf))
                break;
        } else
            memset(sector_buf, 0, 512);
        int can_write = 512 - (int)offset_in_sector;
        if (can_write > len)
            can_write = len;
        memcpy(sector_buf + offset_in_sector, in, can_write);
        if (!wr(lba, sector_buf))
            break;
        in += can_write;
        total += can_write;
        len -= can_write;
        f->cur_offset += can_write;
        if (f->cur_offset > f->entry.file_size)
            f->entry.file_size = f->cur_offset;
        if (f->cur_offset % cluster_bytes == 0 && len > 0)
            f->cur_cluster = 0;
    }
    if (f->cur_cluster >= 2 && f->cur_cluster < FAT16_RESERVED)
        fat_set(f->cur_cluster, FAT16_EOC);
    f->entry.modify_time = now_fat_time();
    f->entry.modify_date = now_fat_date();
    return total;
}

bool fat16_seek(fat16_file_t *f, uint32_t offset) {
    if (!f->open)
        return false;
    if (offset > f->entry.file_size)
        return false;
    f->cur_offset = offset;
    f->cur_cluster = f->entry.cluster_lo;
    uint32_t cluster_bytes = fs.bpb.sectors_per_cluster * 512;
    uint32_t clusters_to_skip = offset / cluster_bytes;
    for (uint32_t i = 0; i < clusters_to_skip; i++) {
        if (f->cur_cluster < 2 || f->cur_cluster >= FAT16_RESERVED)
            return false;
        f->cur_cluster = fat_get(f->cur_cluster);
    }
    return true;
}

int fat16_tell(fat16_file_t *f) {
    if (!f || !f->open)
        return -1;
    return (int)f->cur_offset;
}

bool fat16_close(fat16_file_t *f) {
    if (!f->open)
        return false;
    if (!rd(f->dir_entry_lba, sector_buf))
        return false;
    dir_entry_t *entries = (dir_entry_t *)sector_buf;
    entries[f->dir_entry_idx].file_size = f->entry.file_size;
    entries[f->dir_entry_idx].cluster_lo = f->entry.cluster_lo;
    entries[f->dir_entry_idx].modify_time = f->entry.modify_time;
    entries[f->dir_entry_idx].modify_date = f->entry.modify_date;
    if (!wr(f->dir_entry_lba, sector_buf))
        return false;
    f->open = false;
    return true;
}

bool fat16_rename(const char *path, const char *new_name) {
    uint16_t dir_cluster;
    char name83[12];
    if (!fat16_resolve(path, &dir_cluster, name83))
        return false;
    char tmp_name[13];
    int ni = 0;
    for (int i = 0; i < 8 && name83[i] != ' '; i++)
        tmp_name[ni++] = name83[i];
    tmp_name[ni] = '\0';
    if (path_is_protected(tmp_name) || path_is_protected(path))
        return false;
    char new83[12];
    fat16_make_83(new_name, new83);
    dir_entry_t tmp2;
    if (fat16_find_in_dir(dir_cluster, new83, &tmp2))
        return false;
    modify_ctx ctx = {name83, false, new83, 1};
    dir_iterate(dir_cluster, modify_cb, &ctx);
    return ctx.done;
}

bool fat16_mkdir(const char *path) {
    uint16_t dir_cluster;
    char name83[12];
    if (!fat16_resolve(path, &dir_cluster, name83))
        return false;
    dir_entry_t existing;
    if (fat16_find_in_dir(dir_cluster, name83, &existing))
        return false;
    uint32_t lba;
    int idx;
    if (!alloc_dir_entry(dir_cluster, &lba, &idx))
        return false;
    uint16_t cluster = fat_alloc();
    if (cluster == 0)
        return false;
    if (!rd(lba, sector_buf))
        return false;
    dir_entry_t *entries = (dir_entry_t *)sector_buf;
    uint8_t first = entries[idx].name[0];
    memset(&entries[idx], 0, sizeof(dir_entry_t));
    memcpy(entries[idx].name, name83, 11);
    entries[idx].attr = ATTR_DIRECTORY;
    entries[idx].create_time = now_fat_time();
    entries[idx].create_date = now_fat_date();
    entries[idx].modify_time = entries[idx].create_time;
    entries[idx].modify_date = entries[idx].create_date;
    entries[idx].cluster_lo = cluster;
    entries[idx].file_size = 0;
    if (first == DIR_ENTRY_END && idx + 1 < 16)
        entries[idx + 1].name[0] = DIR_ENTRY_END;
    if (!wr(lba, sector_buf))
        return false;
    uint8_t dir_block[512];
    memset(dir_block, 0, 512);
    dir_entry_t *dot = (dir_entry_t *)dir_block;
    memcpy(dot[0].name, ".          ", 11);
    dot[0].attr = ATTR_DIRECTORY;
    dot[0].cluster_lo = cluster;
    memcpy(dot[1].name, "..         ", 11);
    dot[1].attr = ATTR_DIRECTORY;
    dot[1].cluster_lo = dir_cluster;
    dot[2].name[0] = DIR_ENTRY_END;
    uint32_t new_dir_lba = cluster_to_lba(cluster);
    return wr(new_dir_lba, dir_block);
}

bool fat16_delete(const char *path) {
    if (path_is_protected(path))
        return false;
    uint16_t dir_cluster;
    char name83[12];
    if (!fat16_resolve(path, &dir_cluster, name83))
        return false;
    modify_ctx ctx = {name83, false, NULL, 0};
    dir_iterate(dir_cluster, modify_cb, &ctx);
    return ctx.done;
}

bool fat16_rmdir(const char *path) {
    if (path_is_protected(path))
        return false;
    uint16_t dir_cluster;
    char name83[12];
    if (!fat16_resolve(path, &dir_cluster, name83))
        return false;
    dir_entry_t entry;
    if (!fat16_find_in_dir(dir_cluster, name83, &entry))
        return false;
    if (!(entry.attr & ATTR_DIRECTORY))
        return false;
    if (!is_dir_empty(entry.cluster_lo))
        return false;
    modify_ctx ctx = {name83, false, NULL, 2};
    dir_iterate(dir_cluster, modify_cb, &ctx);
    return ctx.done;
}

bool fat16_rm_rf(const char *path) {
    if (path_is_protected(path))
        return false;
    uint16_t dir_cluster;
    char name83[12];
    if (!fat16_resolve(path, &dir_cluster, name83))
        return false;
    dir_entry_t entry;
    if (!fat16_find_in_dir(dir_cluster, name83, &entry))
        return false;
    if (!(entry.attr & ATTR_DIRECTORY))
        return false;
    fat16_wipe_cluster(entry.cluster_lo);
    modify_ctx ctx = {name83, false, NULL, 2};
    dir_iterate(dir_cluster, modify_cb, &ctx);
    return ctx.done;
}

bool fat16_copy_file(const char *src_path, const char *dest_path) {
    fat16_file_t src;
    if (!fat16_open(src_path, &src))
        return false;
    bool ok = _copy_open_file(&src, dest_path);
    fat16_close(&src);
    return ok;
}

bool fat16_copy_dir(const char *src_path, const char *dest_path) {
    if (path_is_protected(src_path))
        return false;
    dir_entry_t src_entry;
    if (!fat16_find(src_path, &src_entry))
        return false;
    if (!(src_entry.attr & ATTR_DIRECTORY))
        return false;
    dir_entry_t tmp;
    if (fat16_find(dest_path, &tmp))
        return false;
    if (!fat16_mkdir(dest_path))
        return false;
    if (!_copy_dir_cluster(src_entry.cluster_lo, dest_path)) {
        fat16_rm_rf(dest_path);
        return false;
    }
    return true;
}

bool fat16_move_file(const char *src_path, const char *dest_path) {
    if (path_is_protected(src_path))
        return false;
    uint16_t src_dir, dst_dir;
    char src83[12], dst83[12];
    if (!fat16_resolve(src_path, &src_dir, src83))
        return false;
    if (!fat16_resolve(dest_path, &dst_dir, dst83))
        return false;
    dir_entry_t src_entry;
    if (!fat16_find_in_dir(src_dir, src83, &src_entry))
        return false;
    if (src_entry.attr & ATTR_DIRECTORY)
        return false;
    dir_entry_t collision;
    if (fat16_find_in_dir(dst_dir, dst83, &collision))
        return false;
    if (src_dir == dst_dir) {
        modify_ctx ctx = {src83, false, dst83, 1};
        dir_iterate(src_dir, modify_cb, &ctx);
        return ctx.done;
    }
    uint32_t dst_lba;
    int dst_idx;
    if (!alloc_dir_entry(dst_dir, &dst_lba, &dst_idx))
        return false;
    if (!rd(dst_lba, sector_buf))
        return false;
    dir_entry_t *dst_entries = (dir_entry_t *)sector_buf;
    uint8_t was_end = dst_entries[dst_idx].name[0];
    dst_entries[dst_idx] = src_entry;
    memcpy(dst_entries[dst_idx].name, dst83, 11);
    if (was_end == DIR_ENTRY_END && dst_idx + 1 < 16)
        dst_entries[dst_idx + 1].name[0] = DIR_ENTRY_END;
    if (!wr(dst_lba, sector_buf))
        return false;
    locate_ctx loc = {src83, 0, 0, false};
    dir_iterate(src_dir, locate_cb, &loc);
    if (!loc.found)
        return false;
    if (!rd(loc.lba, sector_buf))
        return false;
    dir_entry_t *src_entries = (dir_entry_t *)sector_buf;
    src_entries[loc.idx].name[0] = DIR_ENTRY_FREE;
    return wr(loc.lba, sector_buf);
}

bool fat16_move_dir(const char *src_path, const char *dest_path) {
    if (path_is_protected(src_path))
        return false;
    uint16_t src_dir, dst_dir;
    char src83[12], dst83[12];
    if (!fat16_resolve(src_path, &src_dir, src83))
        return false;
    if (!fat16_resolve(dest_path, &dst_dir, dst83))
        return false;
    dir_entry_t src_entry;
    if (!fat16_find_in_dir(src_dir, src83, &src_entry))
        return false;
    if (!(src_entry.attr & ATTR_DIRECTORY))
        return false;
    dir_entry_t collision;
    if (fat16_find_in_dir(dst_dir, dst83, &collision))
        return false;
    if (src_dir == dst_dir) {
        modify_ctx ctx = {src83, false, dst83, 1};
        dir_iterate(src_dir, modify_cb, &ctx);
        return ctx.done;
    }
    uint32_t dst_lba;
    int dst_idx;
    if (!alloc_dir_entry(dst_dir, &dst_lba, &dst_idx))
        return false;
    if (!rd(dst_lba, sector_buf))
        return false;
    dir_entry_t *dst_entries = (dir_entry_t *)sector_buf;
    uint8_t was_end = dst_entries[dst_idx].name[0];
    dst_entries[dst_idx] = src_entry;
    memcpy(dst_entries[dst_idx].name, dst83, 11);
    if (was_end == DIR_ENTRY_END && dst_idx + 1 < 16)
        dst_entries[dst_idx + 1].name[0] = DIR_ENTRY_END;
    if (!wr(dst_lba, sector_buf))
        return false;
    locate_ctx loc = {src83, 0, 0, false};
    dir_iterate(src_dir, locate_cb, &loc);
    if (!loc.found)
        return false;
    if (!rd(loc.lba, sector_buf))
        return false;
    dir_entry_t *src_entries = (dir_entry_t *)sector_buf;
    src_entries[loc.idx].name[0] = DIR_ENTRY_FREE;
    if (!wr(loc.lba, sector_buf))
        return false;
    uint32_t dotdot_lba = cluster_to_lba(src_entry.cluster_lo);
    if (!rd(dotdot_lba, sector_buf))
        return true;
    dir_entry_t *dot_entries = (dir_entry_t *)sector_buf;
    for (int i = 0; i < 16; i++) {
        if (dot_entries[i].name[0] == '.' && dot_entries[i].name[1] == '.') {
            dot_entries[i].cluster_lo = dst_dir;
            break;
        }
    }
    wr(dotdot_lba, sector_buf);
    return true;
}

bool fat16_copy_file_drive(uint8_t src_drive, const char *src_path,
                           uint8_t dst_drive, const char *dst_path) {
    uint8_t saved_drive = fat16_current_drive();

    fat16_select_drive(src_drive);
    fat16_file_t src;
    if (!fat16_open(src_path, &src)) {
        fat16_select_drive(saved_drive);
        return false;
    }

    fat16_select_drive(dst_drive);
    fat16_file_t dst;
    if (!fat16_create(dst_path, &dst)) {
        fat16_select_drive(src_drive);
        fat16_close(&src);
        fat16_select_drive(saved_drive);
        return false;
    }

    uint8_t buf[512];
    int n;
    while ((n = fat16_read(&src, buf, sizeof(buf))) > 0) {
        if (fat16_write(&dst, buf, n) != n) {
            fat16_close(&dst);
            fat16_delete(dst_path);
            fat16_select_drive(src_drive);
            fat16_close(&src);
            fat16_select_drive(saved_drive);
            return false;
        }
    }

    fat16_close(&src);
    fat16_close(&dst);
    fat16_select_drive(saved_drive);
    return true;
}

bool fat16_move_file_drive(uint8_t src_drive, const char *src_path,
                           uint8_t dst_drive, const char *dst_path) {
    if (!fat16_copy_file_drive(src_drive, src_path, dst_drive, dst_path))
        return false;
    fat16_select_drive(src_drive);
    return fat16_delete(src_path);
}

bool fat16_copy_dir_drive(uint8_t src_drive, const char *src_path,
                          uint8_t dst_drive, const char *dst_path) {
    uint8_t saved_drive = fat16_current_drive();

    fat16_select_drive(src_drive);
    dir_entry_t src_entry;
    if (!fat16_find(src_path, &src_entry) ||
        !(src_entry.attr & ATTR_DIRECTORY)) {
        fat16_select_drive(saved_drive);
        return false;
    }

    fat16_select_drive(dst_drive);
    dir_entry_t collision;
    if (fat16_find(dst_path, &collision)) {
        fat16_select_drive(saved_drive);
        return false;
    }
    if (!fat16_mkdir(dst_path)) {
        fat16_select_drive(saved_drive);
        return false;
    }

    if (!_copy_dir_cluster_drive(src_drive, src_entry.cluster_lo, dst_drive,
                                 dst_path)) {
        fat16_select_drive(dst_drive);
        fat16_rm_rf(dst_path);
        fat16_select_drive(saved_drive);
        return false;
    }

    fat16_select_drive(saved_drive);
    return true;
}

bool fat16_move_dir_drive(uint8_t src_drive, const char *src_path,
                          uint8_t dst_drive, const char *dst_path) {
    if (!fat16_copy_dir_drive(src_drive, src_path, dst_drive, dst_path))
        return false;
    fat16_select_drive(src_drive);
    return fat16_rm_rf(src_path);
}

/* ── hidden file helpers ──────────────────────────────────────────────── */
bool fat16_set_hidden(const char *path, bool hidden) {
    uint16_t dir_cluster;
    char name83[12];
    if (!fat16_resolve(path, &dir_cluster, name83))
        return false;
    locate_ctx loc = {name83, 0, 0, false};
    dir_iterate(dir_cluster, locate_cb, &loc);
    if (!loc.found)
        return false;
    if (!rd(loc.lba, sector_buf))
        return false;
    dir_entry_t *entries = (dir_entry_t *)sector_buf;
    if (hidden)
        entries[loc.idx].attr |= ATTR_HIDDEN;
    else
        entries[loc.idx].attr &= ~ATTR_HIDDEN;
    return wr(loc.lba, sector_buf);
}

bool fat16_is_hidden(const char *path) {
    dir_entry_t de;
    if (!fat16_find(path, &de))
        return false;
    return (de.attr & ATTR_HIDDEN) != 0;
}
