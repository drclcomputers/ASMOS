#include "fs/fs.h"
#include "fs/ata.h"
#include "fs/fat_io.h"
#include "fs/fdd.h"
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
    "DESKTOP", "/DESKTOP", "HDB", "FDD0", "FDD1",
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

bool fs_read_sector(uint8_t drive_id, uint32_t lba, void *buf) {
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

bool fs_write_sector(uint8_t drive_id, uint32_t lba, const void *buf) {
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

typedef bool (*dir_cb)(uint8_t drive_id, dir_entry_t *entry, uint32_t lba,
                       int idx, void *user);

static bool dir_iterate(uint8_t drive_id, uint16_t dir_cluster, dir_cb cb,
                        void *user) {
    fat_vol_t *v = vol(drive_id);
    uint32_t lba;
    uint8_t sector_buf[512];

    if (dir_cluster == 0) {
        uint32_t root_sectors = (v->bpb.root_entry_count * 32 + 511) / 512;
        for (uint32_t s = 0; s < root_sectors; s++) {
            lba = v->root_lba + s;
            if (!fs_read_sector(drive_id, lba, sector_buf))
                return false;
            dir_entry_t *entries = (dir_entry_t *)sector_buf;
            if (entries[0].name[0] == DIR_ENTRY_END)
                return true;
            for (int i = 0; i < 16; i++) {
                if (entries[i].name[0] == DIR_ENTRY_END)
                    return true;
                if (entries[i].name[0] == DIR_ENTRY_FREE)
                    continue;
                if (entries[i].attr & ATTR_VOLUME_ID)
                    continue;
                if (!cb(drive_id, &entries[i], lba, i, user))
                    return false;
            }
        }
    } else {
        uint16_t cluster = dir_cluster;
        int safety = 0;
        while (vol_cluster_active(v, cluster) && safety++ < v->cluster_count) {
            uint32_t start_lba = fat_cluster_to_lba(drive_id, cluster);
            for (uint32_t s = 0; s < v->bpb.sectors_per_cluster; s++) {
                lba = start_lba + s;
                if (!fs_read_sector(drive_id, lba, sector_buf))
                    return false;
                dir_entry_t *entries = (dir_entry_t *)sector_buf;
                for (int i = 0; i < 16; i++) {
                    if (entries[i].name[0] == DIR_ENTRY_END)
                        return true;
                    if (entries[i].name[0] == DIR_ENTRY_FREE)
                        continue;
                    if (entries[i].attr & ATTR_VOLUME_ID)
                        continue;
                    if (!cb(drive_id, &entries[i], lba, i, user))
                        return false;
                }
            }
            cluster = fat_get(drive_id, cluster);
        }
    }
    return true;
}

typedef bool (*cluster_cb)(uint8_t drive_id, dir_entry_t *e, uint16_t cluster,
                           int idx, void *user);

static bool cluster_iterate(uint8_t drive_id, uint16_t cluster, cluster_cb cb,
                            void *user) {
    fat_vol_t *v = vol(drive_id);
    uint8_t sector_buf[512];
    int safety = 0;
    while (vol_cluster_active(v, cluster) && safety++ < v->cluster_count) {
        uint32_t start_lba = fat_cluster_to_lba(drive_id, cluster);
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
                if (entries[i].attr & ATTR_VOLUME_ID)
                    continue;
                if (!cb(drive_id, &entries[i], cluster, i, user))
                    return false;
            }
        }
        cluster = fat_get(drive_id, cluster);
    }
    return true;
}

static bool alloc_dir_entry(uint8_t drive_id, uint16_t dir_cluster,
                            uint32_t *out_lba, int *out_idx) {
    fat_vol_t *v = vol(drive_id);
    uint32_t lba;
    uint8_t sector_buf[512];

    if (dir_cluster == 0) {
        uint32_t root_sectors = (v->bpb.root_entry_count * 32 + 511) / 512;
        for (uint32_t s = 0; s < root_sectors; s++) {
            lba = v->root_lba + s;
            if (!fs_read_sector(drive_id, lba, sector_buf))
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
        return false;
    }

    uint16_t cluster = dir_cluster, prev = 0;
    while (vol_cluster_active(v, cluster)) {
        uint32_t start_lba = fat_cluster_to_lba(drive_id, cluster);
        for (uint32_t s = 0; s < v->bpb.sectors_per_cluster; s++) {
            lba = start_lba + s;
            if (!fs_read_sector(drive_id, lba, sector_buf))
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
        cluster = fat_get(drive_id, cluster);
    }

    uint16_t new_cluster = fat_alloc(drive_id);
    if (new_cluster == 0)
        return false;
    if (prev)
        fat_set(drive_id, prev, new_cluster);
    uint32_t new_lba = fat_cluster_to_lba(drive_id, new_cluster);
    uint8_t zero[512];
    memset(zero, 0, 512);
    for (uint32_t s = 0; s < v->bpb.sectors_per_cluster; s++)
        fs_write_sector(drive_id, new_lba + s, zero);
    *out_lba = new_lba;
    *out_idx = 0;
    return true;
}

typedef struct {
    const char *name;
    dir_entry_t *out;
    bool found;
} find_ctx;
typedef struct {
    dir_entry_t *buf;
    int max;
    int count;
} list_ctx;
typedef struct {
    const char *name;
    bool done;
    const char *new_name;
    int action;
} modify_ctx;
typedef struct {
    const char *name83;
    uint32_t lba;
    int idx;
    bool found;
} locate_ctx;

static bool find_cb(uint8_t drive_id, dir_entry_t *e, uint32_t lba, int idx,
                    void *user) {
    (void)drive_id;
    (void)lba;
    (void)idx;
    find_ctx *ctx = user;
    if (name83_eq(e->name, ctx->name)) {
        *ctx->out = *e;
        ctx->found = true;
        return false;
    }
    return true;
}
static bool list_cb(uint8_t drive_id, dir_entry_t *e, uint32_t lba, int idx,
                    void *user) {
    (void)drive_id;
    (void)lba;
    (void)idx;
    list_ctx *ctx = user;
    if (ctx->count < ctx->max)
        ctx->buf[ctx->count++] = *e;
    return true;
}
static bool modify_cb(uint8_t drive_id, dir_entry_t *e, uint32_t lba, int idx,
                      void *user) {
    modify_ctx *ctx = user;
    if (name83_eq(e->name, ctx->name)) {
        uint8_t sector_buf[512];
        if (!fs_read_sector(drive_id, lba, sector_buf))
            return false;
        dir_entry_t *entries = (dir_entry_t *)sector_buf;
        if (ctx->action == 0 || ctx->action == 2) {
            fat_free_chain(drive_id, entries[idx].cluster_lo);
            entries[idx].name[0] = DIR_ENTRY_FREE;
        } else if (ctx->action == 1) {
            memcpy(entries[idx].name, ctx->new_name, 11);
        }
        fs_write_sector(drive_id, lba, sector_buf);
        ctx->done = true;
        return false;
    }
    return true;
}
static bool locate_cb(uint8_t drive_id, dir_entry_t *e, uint32_t lba, int idx,
                      void *user) {
    (void)drive_id;
    locate_ctx *ctx = user;
    if (name83_eq(e->name, ctx->name83)) {
        ctx->lba = lba;
        ctx->idx = idx;
        ctx->found = true;
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
                if (entries[i].attr & ATTR_VOLUME_ID) {
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

bool fs_mount_drive(uint8_t drive_id) {
    if (drive_id >= DRIVE_COUNT)
        return false;
    fat_vol_t *v = vol(drive_id);
    v->drive_id = drive_id;
    v->mounted = false;

    uint8_t tmp_buf[512];
    if (!fs_read_sector(drive_id, 0, tmp_buf))
        return false;
    memcpy(&v->bpb, tmp_buf, sizeof(bpb_t));
    if (v->bpb.bytes_per_sector != 512)
        return false;

    v->fat_lba = v->bpb.reserved_sectors;
    v->root_lba = v->fat_lba + v->bpb.fat_count * v->bpb.sectors_per_fat;
    uint32_t root_sectors = (v->bpb.root_entry_count * 32 + 511) / 512;
    v->data_lba = v->root_lba + root_sectors;

    uint32_t total = v->bpb.total_sectors_16 ? v->bpb.total_sectors_16
                                             : v->bpb.total_sectors_32;
    v->cluster_count = (total - v->data_lba) / v->bpb.sectors_per_cluster;
    v->fat_type = (v->cluster_count < 4085) ? FAT_TYPE_FAT12 : FAT_TYPE_FAT16;
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
    strncpy(dir_context.path, "/", 255);
    fs_mount_drive(DRIVE_HDB);
    if (fdd_init(0))
        fs_mount_drive(DRIVE_FDD0);
    if (fdd_init(1))
        fs_mount_drive(DRIVE_FDD1);
    return true;
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

    char component[256];
    while (*path) {
        while (*path == '/')
            path++;
        if (!*path)
            break;
        int ci = 0;
        while (*path && *path != '/' && ci < 255)
            component[ci++] = *path++;
        component[ci] = '\0';

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

        char name83[12];
        fs_make_83(component, name83);
        dir_entry_t de;
        if (!fs_find_in_dir(drive, cluster, name83, &de))
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

bool fs_get_usage_drive(uint8_t drive_id, uint32_t *total_bytes,
                        uint32_t *used_bytes) {
    if (drive_id >= DRIVE_COUNT || !g_drives[drive_id].mounted)
        return false;
    fat_vol_t *v = &g_drives[drive_id];
    *total_bytes = (v->bpb.total_sectors_32 ? v->bpb.total_sectors_32
                                            : v->bpb.total_sectors_16) *
                   v->bpb.bytes_per_sector;
    uint32_t used = 0;
    for (uint16_t c = 2; c < (uint16_t)(v->cluster_count + 2); c++) {
        uint16_t val = fat_get(drive_id, c);
        if (val != FAT16_FREE && val != FAT16_BAD)
            used++;
    }
    *used_bytes = used * v->bpb.bytes_per_sector * v->bpb.sectors_per_cluster;
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

bool fs_resolve(const char *path, uint8_t *out_drive,
                uint16_t *out_parent_cluster, char out_name83[12]) {
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
            return true;
        }
    }

    char component[256];
    while (true) {
        int i = 0;
        while (*ptr && *ptr != '/' && i < 255)
            component[i++] = *ptr++;
        component[i] = '\0';
        while (*ptr == '/')
            ptr++;

        bool is_leaf = (*ptr == '\0');
        if (is_leaf) {
            fs_make_83(component, out_name83);
            *out_drive = cur_drive;
            *out_parent_cluster = cur_cluster;
            return true;
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

        char name83[12];
        fs_make_83(component, name83);
        dir_entry_t entry;
        if (!fs_find_in_dir(cur_drive, cur_cluster, name83, &entry))
            return false;
        if (!(entry.attr & ATTR_DIRECTORY))
            return false;
        cur_cluster = entry.cluster_lo;
    }
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

    dir_context.drive_id = target_drive;
    dir_context.current_cluster = entry.cluster_lo;

    char new_path[256];
    if (path[0] == '/') {
        strncpy(new_path, path, 255);
    } else {
        strncpy(new_path, dir_context.path, 255);
        if (strcmp(new_path, "/") != 0)
            strncat(new_path, "/", 255 - strlen(new_path));
        strncat(new_path, path, 255 - strlen(new_path));
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
        if (len == 1 && start[0] == '.')
            continue;
        else if (len == 2 && start[0] == '.' && start[1] == '.') {
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

bool fs_pwd(char *buf, int buflen) {
    if (!buf || buflen < 2)
        return false;
    strncpy(buf, dir_context.path, buflen - 1);
    buf[buflen - 1] = '\0';
    return true;
}

bool fs_find_in_dir(uint8_t drive_id, uint16_t dir_cluster, const char *name83,
                    dir_entry_t *out) {
    find_ctx ctx = {name83, out, false};
    dir_iterate(drive_id, dir_cluster, find_cb, &ctx);
    return ctx.found;
}

bool fs_find(const char *path, dir_entry_t *out) {
    uint8_t drive_id;
    uint16_t dir_cluster;
    char name83[12];
    if (!fs_resolve(path, &drive_id, &dir_cluster, name83))
        return false;
    return fs_find_in_dir(drive_id, dir_cluster, name83, out);
}

bool fs_list_dir(uint8_t drive_id, uint16_t dir_cluster, dir_entry_t *buf,
                 int max, int *count) {
    list_ctx ctx = {buf, max, 0};
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
                if (entries[i].attr & ATTR_DIRECTORY)
                    fs_wipe_cluster(drive_id, entries[i].cluster_lo);
                else
                    fat_free_chain(drive_id, entries[i].cluster_lo);
            }
        }
        cur = fat_get(drive_id, cur);
    }
}

bool fs_open(const char *path, fat_file_t *f) {
    uint8_t drive_id;
    uint16_t dir_cluster;
    char name83[12];
    if (!fs_resolve(path, &drive_id, &dir_cluster, name83))
        return false;
    dir_entry_t entry;
    if (!fs_find_in_dir(drive_id, dir_cluster, name83, &entry))
        return false;
    locate_ctx loc = {name83, 0, 0, false};
    dir_iterate(drive_id, dir_cluster, locate_cb, &loc);
    if (!loc.found)
        return false;
    memset(f, 0, sizeof(fat_file_t));
    f->entry = entry;
    f->cur_cluster = entry.cluster_lo;
    f->dir_cluster = dir_cluster;
    f->dir_entry_lba = loc.lba;
    f->dir_entry_idx = loc.idx;
    f->open = true;
    f->drive_id = drive_id;
    return true;
}

bool fs_create(const char *path, fat_file_t *f) {
    uint8_t drive_id;
    uint16_t dir_cluster;
    char name83[12];
    uint8_t sector_buf[512];

    if (!fs_resolve(path, &drive_id, &dir_cluster, name83))
        return false;
    dir_entry_t existing;
    if (fs_find_in_dir(drive_id, dir_cluster, name83, &existing))
        return false;

    uint32_t lba;
    int idx;
    if (!alloc_dir_entry(drive_id, dir_cluster, &lba, &idx))
        return false;

    uint16_t cluster = fat_alloc(drive_id);
    if (cluster == 0)
        return false;

    if (!fs_read_sector(drive_id, lba, sector_buf))
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
    if (!fs_write_sector(drive_id, lba, sector_buf))
        return false;

    memset(f, 0, sizeof(fat_file_t));
    f->entry = entries[idx];
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

static int done_write(fat_file_t *f, int total, fat_vol_t *v) {
    if (vol_cluster_active(v, f->cur_cluster))
        fat_set(f->drive_id, f->cur_cluster, FAT16_EOC);

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
        if(!fs_write_sector(f->drive_id, lba, write_sector_buf)) {
            return done_write(f, -1, v);
        }

        in += can_write;
        total += can_write;
        len -= can_write;
        f->cur_offset += can_write;
        if (f->cur_offset > f->entry.file_size)
            f->entry.file_size = f->cur_offset;
    }

    if (vol_cluster_active(v, f->cur_cluster))
        fat_set(f->drive_id, f->cur_cluster, FAT16_EOC);

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
    uint32_t clusters_to_skip = offset / cluster_bytes;
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

bool fs_mkdir(const char *path) {
    uint8_t drive_id;
    uint16_t dir_cluster;
    char name83[12];
    uint8_t sector_buf[512];
    if (!fs_resolve(path, &drive_id, &dir_cluster, name83))
        return false;
    dir_entry_t existing;
    if (fs_find_in_dir(drive_id, dir_cluster, name83, &existing))
        return false;

    uint32_t lba;
    int idx;
    if (!alloc_dir_entry(drive_id, dir_cluster, &lba, &idx))
        return false;

    uint16_t cluster = fat_alloc(drive_id);
    if (cluster == 0)
        return false;

    if (!fs_read_sector(drive_id, lba, sector_buf))
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
    if (first == DIR_ENTRY_END && idx + 1 < 16)
        entries[idx + 1].name[0] = DIR_ENTRY_END;
    if (!fs_write_sector(drive_id, lba, sector_buf))
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
    return fs_write_sector(drive_id, fat_cluster_to_lba(drive_id, cluster),
                           dir_block);
}

bool fs_rename(const char *path, const char *new_name) {
    if (path_is_protected(path))
        return false;
    uint8_t drive_id;
    uint16_t dir_cluster;
    char name83[12];
    if (!fs_resolve(path, &drive_id, &dir_cluster, name83))
        return false;

    char tmp_name[13];
    int ni = 0;
    for (int i = 0; i < 8 && name83[i] != ' '; i++)
        tmp_name[ni++] = name83[i];
    tmp_name[ni] = '\0';
    if (path_is_protected(tmp_name))
        return false;

    char new83[12];
    fs_make_83(new_name, new83);
    dir_entry_t tmp2;
    if (fs_find_in_dir(drive_id, dir_cluster, new83, &tmp2))
        return false;

    modify_ctx ctx = {name83, false, new83, 1};
    dir_iterate(drive_id, dir_cluster, modify_cb, &ctx);
    return ctx.done;
}

bool fs_delete(const char *path) {
    if (path_is_protected(path))
        return false;
    uint8_t drive_id;
    uint16_t dir_cluster;
    char name83[12];
    if (!fs_resolve(path, &drive_id, &dir_cluster, name83))
        return false;
    modify_ctx ctx = {name83, false, NULL, 0};
    dir_iterate(drive_id, dir_cluster, modify_cb, &ctx);
    return ctx.done;
}

bool fs_rmdir(const char *path) {
    if (path_is_protected(path))
        return false;
    uint8_t drive_id;
    uint16_t dir_cluster;
    char name83[12];
    if (!fs_resolve(path, &drive_id, &dir_cluster, name83))
        return false;
    dir_entry_t entry;
    if (!fs_find_in_dir(drive_id, dir_cluster, name83, &entry))
        return false;
    if (!(entry.attr & ATTR_DIRECTORY))
        return false;
    if (!fs_is_dir_empty(drive_id, entry.cluster_lo))
        return false;
    modify_ctx ctx = {name83, false, NULL, 2};
    dir_iterate(drive_id, dir_cluster, modify_cb, &ctx);
    return ctx.done;
}

bool fs_rm_rf(const char *path) {
    if (path_is_protected(path))
        return false;
    uint8_t drive_id;
    uint16_t dir_cluster;
    char name83[12];
    if (!fs_resolve(path, &drive_id, &dir_cluster, name83))
        return false;
    dir_entry_t entry;
    if (!fs_find_in_dir(drive_id, dir_cluster, name83, &entry))
        return false;
    if (!(entry.attr & ATTR_DIRECTORY))
        return false;
    fs_wipe_cluster(drive_id, entry.cluster_lo);
    modify_ctx ctx = {name83, false, NULL, 2};
    dir_iterate(drive_id, dir_cluster, modify_cb, &ctx);
    return ctx.done;
}

static bool _copy_open_file_to(fat_file_t *src, uint8_t dst_drive,
                               uint16_t dst_dir_cluster,
                               const char *dst_name83) {
    fs_seek(src, 0);

    uint32_t lba;
    int idx;
    if (!alloc_dir_entry(dst_drive, dst_dir_cluster, &lba, &idx))
        return false;

    uint16_t cluster = fat_alloc(dst_drive);
    if (cluster == 0)
        return false;

    uint8_t dbuf[512];
    if (!fs_read_sector(dst_drive, lba, dbuf)) {
        fat_set(dst_drive, cluster, FAT16_FREE);
        return false;
    }
    dir_entry_t *entries = (dir_entry_t *)dbuf;
    uint8_t first = entries[idx].name[0];
    memset(&entries[idx], 0, sizeof(dir_entry_t));
    memcpy(entries[idx].name, dst_name83, 11);
    entries[idx].attr = ATTR_ARCHIVE;
    entries[idx].cluster_lo = cluster;
    entries[idx].file_size = 0;
    entries[idx].create_time = now_fat_time();
    entries[idx].create_date = now_fat_date();
    entries[idx].modify_time = entries[idx].create_time;
    entries[idx].modify_date = entries[idx].create_date;
    if (first == DIR_ENTRY_END && idx + 1 < 16)
        entries[idx + 1].name[0] = DIR_ENTRY_END;
    if (!fs_write_sector(dst_drive, lba, dbuf)) {
        fat_set(dst_drive, cluster, FAT16_FREE);
        return false;
    }

    fat_file_t dst;
    memset(&dst, 0, sizeof(fat_file_t));
    dst.entry = entries[idx];
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

static bool _copy_dir_cluster(uint8_t src_drive, uint16_t src_cluster,
                              uint8_t dst_drive, uint16_t dst_dir_cluster);

typedef struct {
    uint8_t src_drive;
    uint8_t dst_drive;
    uint16_t dst_dir_cluster;
} copy_dir_ctx;

static bool _copy_entry_cb(uint8_t drive_id, dir_entry_t *e, uint16_t cluster,
                           int idx, void *user) {
    (void)cluster;
    (void)idx;
    copy_dir_ctx *ctx = user;
    char child83[12];
    for (int i = 0; i < 11; i++)
        child83[i] = (char)e->name[i];
    child83[11] = '\0';

    if (e->attr & ATTR_DIRECTORY) {
        uint8_t saved_drive = dir_context.drive_id;
        uint16_t saved_cluster = dir_context.current_cluster;
        dir_context.drive_id = ctx->dst_drive;
        dir_context.current_cluster = ctx->dst_dir_cluster;
        bool ok = fs_mkdir(child83);
        dir_context.drive_id = saved_drive;
        dir_context.current_cluster = saved_cluster;
        if (!ok)
            return false;

        dir_entry_t new_de;
        if (!fs_find_in_dir(ctx->dst_drive, ctx->dst_dir_cluster, child83,
                            &new_de))
            return false;

        return _copy_dir_cluster(ctx->src_drive, e->cluster_lo, ctx->dst_drive,
                                 new_de.cluster_lo);
    } else {
        fat_file_t src;
        uint8_t saved_drive = dir_context.drive_id;
        uint16_t saved_cluster = dir_context.current_cluster;
        dir_context.drive_id = ctx->src_drive;
        dir_context.current_cluster = e->cluster_lo;

        locate_ctx loc = {child83, 0, 0, false};
        dir_iterate(ctx->src_drive, cluster, locate_cb, &loc);
        dir_context.drive_id = saved_drive;
        dir_context.current_cluster = saved_cluster;
        if (!loc.found)
            return false;

        memset(&src, 0, sizeof(src));
        src.entry = *e;
        src.cur_cluster = e->cluster_lo;
        src.dir_cluster = cluster;
        src.dir_entry_lba = loc.lba;
        src.dir_entry_idx = loc.idx;
        src.open = true;
        src.drive_id = ctx->src_drive;

        bool ok = _copy_open_file_to(&src, ctx->dst_drive, ctx->dst_dir_cluster,
                                     child83);
        fs_close(&src);
        return ok;
    }
}

static bool _copy_dir_cluster(uint8_t src_drive, uint16_t src_cluster,
                              uint8_t dst_drive, uint16_t dst_dir_cluster) {
    copy_dir_ctx ctx = {src_drive, dst_drive, dst_dir_cluster};
    return cluster_iterate(src_drive, src_cluster, _copy_entry_cb, &ctx);
}

bool fs_copy_file(const char *src_path, const char *dst_path) {
    fat_file_t src;
    if (!fs_open(src_path, &src))
        return false;
    uint8_t dst_drive;
    uint16_t dst_dir;
    char dst83[12];
    if (!fs_resolve(dst_path, &dst_drive, &dst_dir, dst83)) {
        fs_close(&src);
        return false;
    }
    bool ok = _copy_open_file_to(&src, dst_drive, dst_dir, dst83);
    fs_close(&src);
    return ok;
}

bool fs_move_file(const char *src_path, const char *dst_path) {
    if (path_is_protected(src_path))
        return false;

    uint8_t src_drive, dst_drive;
    uint16_t src_dir, dst_dir;
    char src83[12], dst83[12];
    uint8_t sector_buf[512];

    if (!fs_resolve(src_path, &src_drive, &src_dir, src83))
        return false;
    if (!fs_resolve(dst_path, &dst_drive, &dst_dir, dst83))
        return false;

    dir_entry_t src_entry;
    if (!fs_find_in_dir(src_drive, src_dir, src83, &src_entry))
        return false;
    if (src_entry.attr & ATTR_DIRECTORY)
        return false;

    dir_entry_t collision;
    if (fs_find_in_dir(dst_drive, dst_dir, dst83, &collision))
        return false;

    if (src_drive == dst_drive && src_dir == dst_dir) {
        modify_ctx ctx = {src83, false, dst83, 1};
        dir_iterate(src_drive, src_dir, modify_cb, &ctx);
        return ctx.done;
    }

    if (src_drive == dst_drive) {
        uint32_t dst_lba;
        int dst_idx;
        if (!alloc_dir_entry(dst_drive, dst_dir, &dst_lba, &dst_idx))
            return false;
        if (!fs_read_sector(dst_drive, dst_lba, sector_buf))
            return false;
        dir_entry_t *dst_entries = (dir_entry_t *)sector_buf;
        uint8_t was_end = dst_entries[dst_idx].name[0];
        dst_entries[dst_idx] = src_entry;
        memcpy(dst_entries[dst_idx].name, dst83, 11);
        if (was_end == DIR_ENTRY_END && dst_idx + 1 < 16)
            dst_entries[dst_idx + 1].name[0] = DIR_ENTRY_END;
        if (!fs_write_sector(dst_drive, dst_lba, sector_buf))
            return false;

        locate_ctx loc = {src83, 0, 0, false};
        dir_iterate(src_drive, src_dir, locate_cb, &loc);
        if (!loc.found)
            return false;
        if (!fs_read_sector(src_drive, loc.lba, sector_buf))
            return false;
        ((dir_entry_t *)sector_buf)[loc.idx].name[0] = DIR_ENTRY_FREE;
        return fs_write_sector(src_drive, loc.lba, sector_buf);
    }

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

    dir_entry_t collision;
    if (fs_find(dst_path, &collision))
        return false;

    uint8_t dst_drive;
    uint16_t dst_dir;
    char dst83[12];
    if (!fs_resolve(dst_path, &dst_drive, &dst_dir, dst83))
        return false;

    if (!fs_mkdir(dst_path))
        return false;

    dir_entry_t new_de;
    if (!fs_find(dst_path, &new_de)) {
        fs_rm_rf(dst_path);
        return false;
    }

    uint8_t src_drive;
    uint16_t src_dir;
    char src83[12];
    fs_resolve(src_path, &src_drive, &src_dir, src83);

    if (!_copy_dir_cluster(src_drive, src_entry.cluster_lo, dst_drive,
                           new_de.cluster_lo)) {
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
    char src83[12], dst83[12];
    uint8_t sector_buf[512];

    if (!fs_resolve(src_path, &src_drive, &src_dir, src83))
        return false;
    if (!fs_resolve(dst_path, &dst_drive, &dst_dir, dst83))
        return false;

    dir_entry_t src_entry;
    if (!fs_find_in_dir(src_drive, src_dir, src83, &src_entry))
        return false;
    if (!(src_entry.attr & ATTR_DIRECTORY))
        return false;

    dir_entry_t collision;
    if (fs_find_in_dir(dst_drive, dst_dir, dst83, &collision))
        return false;

    if (src_drive == dst_drive) {
        if (src_dir == dst_dir) {
            modify_ctx ctx = {src83, false, dst83, 1};
            dir_iterate(src_drive, src_dir, modify_cb, &ctx);
            return ctx.done;
        }

        uint32_t dst_lba;
        int dst_idx;
        if (!alloc_dir_entry(dst_drive, dst_dir, &dst_lba, &dst_idx))
            return false;
        if (!fs_read_sector(dst_drive, dst_lba, sector_buf))
            return false;
        dir_entry_t *dst_entries = (dir_entry_t *)sector_buf;
        uint8_t was_end = dst_entries[dst_idx].name[0];
        dst_entries[dst_idx] = src_entry;
        memcpy(dst_entries[dst_idx].name, dst83, 11);
        if (was_end == DIR_ENTRY_END && dst_idx + 1 < 16)
            dst_entries[dst_idx + 1].name[0] = DIR_ENTRY_END;
        if (!fs_write_sector(dst_drive, dst_lba, sector_buf))
            return false;

        locate_ctx loc = {src83, 0, 0, false};
        dir_iterate(src_drive, src_dir, locate_cb, &loc);
        if (!loc.found)
            return false;
        if (!fs_read_sector(src_drive, loc.lba, sector_buf))
            return false;
        ((dir_entry_t *)sector_buf)[loc.idx].name[0] = DIR_ENTRY_FREE;
        if (!fs_write_sector(src_drive, loc.lba, sector_buf))
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
    char name83[12];
    uint8_t sector_buf[512];
    if (!fs_resolve(path, &drive_id, &dir_cluster, name83))
        return false;
    locate_ctx loc = {name83, 0, 0, false};
    dir_iterate(drive_id, dir_cluster, locate_cb, &loc);
    if (!loc.found)
        return false;
    if (!fs_read_sector(drive_id, loc.lba, sector_buf))
        return false;
    dir_entry_t *entries = (dir_entry_t *)sector_buf;
    if (hidden)
        entries[loc.idx].attr |= ATTR_HIDDEN;
    else
        entries[loc.idx].attr &= ~ATTR_HIDDEN;
    return fs_write_sector(drive_id, loc.lba, sector_buf);
}

bool fs_is_hidden(const char *path) {
    dir_entry_t de;
    if (!fs_find(path, &de))
        return false;
    return (de.attr & ATTR_HIDDEN) != 0;
}
