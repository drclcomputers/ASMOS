#include "fs/fat16.h"
#include "fs/ata.h"
#include "lib/mem.h"
#include "lib/string.h"

fat16_fs_t fs;

static uint8_t sector_buf[512];

static uint32_t cluster_to_lba(uint16_t cluster) {
    return fs.data_lba + (cluster - 2) * fs.bpb.sectors_per_cluster;
}

static uint16_t fat_get(uint16_t cluster) {
    uint32_t fat_sector = fs.fat_lba + (cluster / 256);
    uint16_t fat_offset = (cluster % 256) * 2;

    if (!ata_read_sector(fat_sector, sector_buf)) return FAT16_BAD;
    return *(uint16_t *)(sector_buf + fat_offset);
}

static bool fat_set(uint16_t cluster, uint16_t value) {
    uint32_t fat_sector = cluster / 256;
    uint16_t fat_offset = (cluster % 256) * 2;

    for (int copy = 0; copy < fs.bpb.fat_count; copy++) {
        uint32_t lba = fs.fat_lba
                     + copy * fs.bpb.sectors_per_fat
                     + fat_sector;
        if (!ata_read_sector(lba, sector_buf)) return false;
        *(uint16_t *)(sector_buf + fat_offset) = value;
        if (!ata_write_sector(lba, sector_buf)) return false;
    }
    return true;
}

static uint16_t fat_alloc(void) {
    for (uint16_t c = 2; c < (uint16_t)(fs.cluster_count + 2); c++) {
        if (fat_get(c) == FAT16_FREE) {
            fat_set(c, FAT16_EOC);
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

bool fat16_mount(void) {
    if (!ata_read_sector(0, &fs.bpb)) return false;

    if (fs.bpb.bytes_per_sector != 512) return false;

    fs.fat_lba  = fs.bpb.reserved_sectors;
    fs.root_lba = fs.fat_lba
                + fs.bpb.fat_count * fs.bpb.sectors_per_fat;

    uint32_t root_sectors = (fs.bpb.root_entry_count * 32 + 511) / 512;
    fs.data_lba = fs.root_lba + root_sectors;

    uint32_t total = fs.bpb.total_sectors_16
                   ? fs.bpb.total_sectors_16
                   : fs.bpb.total_sectors_32;
    fs.cluster_count = (total - fs.data_lba) / fs.bpb.sectors_per_cluster;
    fs.mounted = true;
    return true;
}

void fat16_make_83(const char *filename, char *out83) {
    memset(out83, ' ', 11);
    out83[11] = '\0';

    int i = 0, j = 0;
    while (filename[i] && filename[i] != '.' && j < 8) {
        char c = filename[i++];
        out83[j++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    if (filename[i] == '.') i++;

    j = 8;
    while (filename[i] && j < 11) {
        char c = filename[i++];
        out83[j++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
}

static bool name83_eq(const uint8_t *a, const char *b) {
    for (int i = 0; i < 11; i++)
        if (a[i] != (uint8_t)b[i]) return false;
    return true;
}

// internal for copy/move
static bool _copy_open_file(fat16_file_t *src, const char *dest_name83) {
    fat16_file_t dst;
    if (!fat16_create(dest_name83, &dst)) return false;

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
typedef bool (*cluster_cb)(dir_entry_t *e, uint16_t cluster, int idx, void *user);
static bool cluster_iterate(uint16_t cluster, cluster_cb cb, void *user) {
    while (cluster >= 2 && cluster < FAT16_RESERVED) {
        uint32_t start_lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < fs.bpb.sectors_per_cluster; s++) {
            if (!ata_read_sector(start_lba + s, sector_buf)) return false;

            dir_entry_t *entries = (dir_entry_t *)sector_buf;
            for (int i = 0; i < 16; i++) {
                uint8_t first = entries[i].name[0];
                if (first == DIR_ENTRY_END) return true;
                if (first == DIR_ENTRY_FREE) continue;
                if (entries[i].attr & ATTR_VOLUME_ID) continue;
                if (entries[i].name[0] == '.') continue;

                if (!cb(&entries[i], cluster, i, user)) return false;
            }
        }
        cluster = fat_get(cluster);
    }
    return true;
}
static bool _copy_dir_cluster(uint16_t src_cluster, const char *dest_name83);
typedef struct {
    const char *dest_parent83;
} copy_dir_ctx;
static bool _copy_dir_entry_cb(dir_entry_t *e, uint16_t cluster, int idx, void *user) {
    (void)cluster; (void)idx;
    copy_dir_ctx *ctx = (copy_dir_ctx *)user;

    char child83[12];
    for (int i = 0; i < 11; i++) child83[i] = (char)e->name[i];
    child83[11] = '\0';

    if (e->attr & ATTR_DIRECTORY) {
        if (!fat16_mkdir(child83)) return false;
        if (!_copy_dir_cluster(e->cluster_lo, child83)) return false;
    } else {
        fat16_file_t src;
        if (!fat16_open(child83, &src)) return false;
        bool ok = _copy_open_file(&src, child83);
        fat16_close(&src);
        if (!ok) return false;
    }
    (void)ctx;
    return true;
}
static bool _copy_dir_cluster(uint16_t src_cluster, const char *dest_name83) {
    copy_dir_ctx ctx = { dest_name83 };
    return cluster_iterate(src_cluster, _copy_dir_entry_cb, &ctx);
}
typedef bool (*dir_cb)(dir_entry_t *entry, uint32_t lba, int idx, void *user);

typedef struct { const char *name; dir_entry_t *out; bool found; } find_ctx;

static bool find_cb(dir_entry_t *e, uint32_t lba, int idx, void *user) {
    (void)lba; (void)idx;
    find_ctx *ctx = (find_ctx *)user;
    if (name83_eq(e->name, ctx->name)) {
        *ctx->out = *e;
        ctx->found = true;
        return false;
    }
    return true;
}

typedef struct { dir_entry_t *buf; int max; int count; } list_ctx;

static bool list_cb(dir_entry_t *e, uint32_t lba, int idx, void *user) {
    (void)lba; (void)idx;
    list_ctx *ctx = (list_ctx *)user;
    if (ctx->count < ctx->max)
        ctx->buf[ctx->count++] = *e;
    return true;
}

bool fat16_get_usage(uint32_t *total_bytes, uint32_t *used_bytes) {
    if (!fs.mounted) return false;

    *total_bytes = (fs.bpb.total_sectors_32 ? fs.bpb.total_sectors_32 : fs.bpb.total_sectors_16)
                 * fs.bpb.bytes_per_sector;

    uint32_t used_clusters = 0;
    uint16_t fat_buf[256];

    uint32_t fat_sectors = fs.bpb.sectors_per_fat;
    for (uint32_t s = 0; s < fat_sectors; s++) {
        if (!ata_read_sector(fs.fat_lba + s, fat_buf)) return false;

        for (int i = 0; i < 256; i++) {
            if (fat_buf[i] != FAT16_FREE && fat_buf[i] != FAT16_RESERVED) {
                used_clusters++;
            }
        }
    }

    *used_bytes = used_clusters * fs.bpb.bytes_per_sector * fs.bpb.sectors_per_cluster;
    return true;
}


// files
int fat16_read(fat16_file_t *f, void *buf, int len) {
    if (!f->open) return -1;

    uint8_t *out     = (uint8_t *)buf;
    int      total   = 0;
    uint32_t filesize = f->entry.file_size;

    while (len > 0 && f->cur_offset < filesize) {
        if (f->cur_cluster < 2 || f->cur_cluster >= FAT16_RESERVED) break;

        uint32_t cluster_bytes =
            fs.bpb.sectors_per_cluster * 512;
        uint32_t offset_in_cluster = f->cur_offset % cluster_bytes;
        uint32_t sector_in_cluster = offset_in_cluster / 512;
        uint32_t offset_in_sector  = offset_in_cluster % 512;

        uint32_t lba = cluster_to_lba(f->cur_cluster) + sector_in_cluster;
        if (!ata_read_sector(lba, sector_buf)) break;

        int can_read = 512 - (int)offset_in_sector;
        int remaining_file = (int)(filesize - f->cur_offset);
        if (can_read > remaining_file) can_read = remaining_file;
        if (can_read > len)            can_read = len;

        memcpy(out, sector_buf + offset_in_sector, can_read);
        out            += can_read;
        total          += can_read;
        len            -= can_read;
        f->cur_offset  += can_read;

        if (f->cur_offset % cluster_bytes == 0) {
            uint16_t next = fat_get(f->cur_cluster);
            f->cur_cluster = (next >= FAT16_EOC) ? 0xFFFF : next;
        }
    }
    return total;
}

int fat16_write(fat16_file_t *f, const void *buf, int len) {
    if (!f->open) return -1;

    const uint8_t *in    = (const uint8_t *)buf;
    int            total = 0;

    while (len > 0) {
        if (f->cur_cluster < 2 || f->cur_cluster >= FAT16_RESERVED) {
            uint16_t prev = f->cur_cluster;
            f->cur_cluster = fat_alloc();
            if (f->cur_cluster == 0) break;

            if (prev >= 2 && prev < FAT16_RESERVED)
                fat_set(prev, f->cur_cluster);
            else
                f->entry.cluster_lo = f->cur_cluster;
        }

        uint32_t cluster_bytes     = fs.bpb.sectors_per_cluster * 512;
        uint32_t offset_in_cluster = f->cur_offset % cluster_bytes;
        uint32_t sector_in_cluster = offset_in_cluster / 512;
        uint32_t offset_in_sector  = offset_in_cluster % 512;

        uint32_t lba = cluster_to_lba(f->cur_cluster) + sector_in_cluster;

        if (offset_in_sector != 0 || len < 512) {
            if (!ata_read_sector(lba, sector_buf)) break;
        } else {
            memset(sector_buf, 0, 512);
        }

        int can_write = 512 - (int)offset_in_sector;
        if (can_write > len) can_write = len;

        memcpy(sector_buf + offset_in_sector, in, can_write);
        if (!ata_write_sector(lba, sector_buf)) break;

        in             += can_write;
        total          += can_write;
        len            -= can_write;
        f->cur_offset  += can_write;
        if (f->cur_offset > f->entry.file_size)
            f->entry.file_size = f->cur_offset;

        if (f->cur_offset % cluster_bytes == 0) {
            uint16_t next = fat_get(f->cur_cluster);
            if (next >= FAT16_EOC || next == FAT16_FREE)
                f->cur_cluster = 0xFFFF;
            else
                f->cur_cluster = next;
        }
    }
    return total;
}

bool fat16_close(fat16_file_t *f) {
    if (!f->open) return false;

    if (!ata_read_sector(f->dir_entry_lba, sector_buf)) return false;
    dir_entry_t *entries = (dir_entry_t *)sector_buf;
    entries[f->dir_entry_idx].file_size  = f->entry.file_size;
    entries[f->dir_entry_idx].cluster_lo = f->entry.cluster_lo;
    if (!ata_write_sector(f->dir_entry_lba, sector_buf)) return false;

    f->open = false;
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
        } else if (ctx->action == 1) {
            memcpy(e->name, ctx->new_name, 11);
        }
        ata_write_sector(lba, sector_buf);
        ctx->done = true;
        return false;
    }
    return true;
}

bool fat16_copy_file(const char *src_path, const char *dest_path) {
    fat16_file_t src;
    if (!fat16_open(src_path, &src)) return false;

    bool ok = _copy_open_file(&src, dest_path);
    fat16_close(&src);
    return ok;
}

// dirs
bool is_dir_empty(uint16_t cluster) {
    if (cluster == 0) return false;

    uint16_t curr = cluster;
    while (curr >= 2 && curr < FAT16_RESERVED) {
        uint32_t start_lba = cluster_to_lba(curr);
        for (uint32_t s = 0; s < fs.bpb.sectors_per_cluster; s++) {
            if (!ata_read_sector(start_lba + s, sector_buf)) return false;
            dir_entry_t *entries = (dir_entry_t *)sector_buf;

            for (int i = 0; i < 16; i++) {
                uint8_t first = entries[i].name[0];
                if (first == DIR_ENTRY_END) return true;
                if (first == DIR_ENTRY_FREE) continue;
                if (entries[i].name[0] == '.') continue;
                return false;
            }
        }
        curr = fat_get(curr);
    }
    return true;
}

void fat16_wipe_cluster(uint16_t cluster) {
    if (cluster == 0 || cluster >= FAT16_RESERVED) return;

    uint16_t curr = cluster;
    while (curr >= 2 && curr < FAT16_RESERVED) {
        uint32_t start_lba = cluster_to_lba(curr);
        for (uint32_t s = 0; s < fs.bpb.sectors_per_cluster; s++) {
            uint8_t local_buf[512];
            if (!ata_read_sector(start_lba + s, local_buf)) return;
            dir_entry_t *entries = (dir_entry_t *)local_buf;

            for (int i = 0; i < 16; i++) {
                uint8_t first = entries[i].name[0];
                if (first == DIR_ENTRY_END) {
                	fat_free_chain(cluster);
                 	return;
                }
                if (first == DIR_ENTRY_FREE) continue;
                if (entries[i].name[0] == '.') continue;

                if (entries[i].attr & ATTR_DIRECTORY) {
                    fat16_wipe_cluster(entries[i].cluster_lo);
                } else {
                    fat_free_chain(entries[i].cluster_lo);
                }
            }
        }
        curr = fat_get(curr);
    }
}

fat16_dir_context_t dir_context = {
    .current_cluster = 0,
    .path = "/"
};

static bool dir_iterate(uint16_t dir_cluster, dir_cb cb, void *user) {
    uint32_t lba;

    if (dir_cluster == 0) {
        uint32_t root_sectors = (fs.bpb.root_entry_count * 32 + 511) / 512;
        for (uint32_t s = 0; s < root_sectors; s++) {
            lba = fs.root_lba + s;
            if (!ata_read_sector(lba, sector_buf)) return false;

            dir_entry_t *entries = (dir_entry_t *)sector_buf;
            for (int i = 0; i < 16; i++) {
                if (entries[i].name[0] == DIR_ENTRY_END) return true;
                if (entries[i].name[0] == DIR_ENTRY_FREE) continue;
                if (entries[i].attr & ATTR_VOLUME_ID) continue;

                if (!cb(&entries[i], lba, i, user)) return false;
            }
        }
    } else {
        uint16_t cluster = dir_cluster;
        while (cluster >= 2 && cluster < FAT16_RESERVED) {
            uint32_t start_lba = cluster_to_lba(cluster);
            for (uint32_t s = 0; s < fs.bpb.sectors_per_cluster; s++) {
                lba = start_lba + s;
                if (!ata_read_sector(lba, sector_buf)) return false;

                dir_entry_t *entries = (dir_entry_t *)sector_buf;
                for (int i = 0; i < 16; i++) {
                    if (entries[i].name[0] == DIR_ENTRY_END) return true;
                    if (entries[i].name[0] == DIR_ENTRY_FREE) continue;
                    if (entries[i].attr & ATTR_VOLUME_ID) continue;

                    if (!cb(&entries[i], lba, i, user)) return false;
                }
            }
            cluster = fat_get(cluster);
        }
    }
    return true;
}

bool fat16_delete(const char *path) {
    uint16_t dir_cluster; char name83[12];
    if (!fat16_resolve(path, &dir_cluster, name83)) return false;

    modify_ctx ctx = { name83, false, NULL, 0 };
    dir_iterate(dir_cluster, modify_cb, &ctx);
    return ctx.done;
}

bool fat16_rename(const char *path, const char *new_name) {
    uint16_t dir_cluster; char name83[12];
    if (!fat16_resolve(path, &dir_cluster, name83)) return false;

    char new_name83[12];
    fat16_make_83(new_name, new_name83);

    dir_entry_t tmp;
    if (fat16_find_in_dir(dir_cluster, new_name83, &tmp)) return false;

    modify_ctx ctx = { name83, false, new_name83, 1 };
    dir_iterate(dir_cluster, modify_cb, &ctx);
    return ctx.done;
}

bool fat16_move_file(const char *src_path, const char *dest_path) {
    dir_entry_t tmp;
    if (fat16_find(dest_path, &tmp)) return false;
    return fat16_rename(src_path, dest_path);
}

bool fat16_rmdir(const char *path) {
    uint16_t dir_cluster; char name83[12];
    if (!fat16_resolve(path, &dir_cluster, name83)) return false;

    dir_entry_t entry;
    if (!fat16_find_in_dir(dir_cluster, name83, &entry)) return false;
    if (!(entry.attr & ATTR_DIRECTORY)) return false;
    if (!is_dir_empty(entry.cluster_lo)) return false;

    modify_ctx ctx = { name83, false, NULL, 2 };
    dir_iterate(dir_cluster, modify_cb, &ctx);
    return ctx.done;
}

bool fat16_rm_rf(const char *path) {
    uint16_t dir_cluster; char name83[12];
    if (!fat16_resolve(path, &dir_cluster, name83)) return false;

    dir_entry_t entry;
    if (!fat16_find_in_dir(dir_cluster, name83, &entry)) return false;
    if (!(entry.attr & ATTR_DIRECTORY)) return false;

    fat16_wipe_cluster(entry.cluster_lo);

    modify_ctx ctx = { name83, false, NULL, 2 };
    dir_iterate(dir_cluster, modify_cb, &ctx);
    return ctx.done;
}

bool fat16_copy_dir(const char *src_path, const char *dest_path) {
    dir_entry_t src_entry;
    if (!fat16_find(src_path, &src_entry)) return false;
    if (!(src_entry.attr & ATTR_DIRECTORY))  return false;

    dir_entry_t tmp;
    if (fat16_find(dest_path, &tmp)) return false;

    if (!fat16_mkdir(dest_path)) return false;

    if (!_copy_dir_cluster(src_entry.cluster_lo, dest_path)) {
        fat16_rm_rf(dest_path);
        return false;
    }
    return true;
}

bool fat16_move_dir(const char *src_path, const char *dest_path) {
    dir_entry_t entry;
    if (!fat16_find(src_path, &entry)) return false;
    if (!(entry.attr & ATTR_DIRECTORY))  return false;

    dir_entry_t tmp;
    if (fat16_find(dest_path, &tmp)) return false;

    return fat16_rename(src_path, dest_path);
}

bool fat16_find_in_dir(uint16_t dir_cluster, const char *name83, dir_entry_t *out) {
    find_ctx ctx = { name83, out, false };
    dir_iterate(dir_cluster, find_cb, &ctx);
    return ctx.found;
}

bool fat16_find(const char *path, dir_entry_t *out) {
    uint16_t dir_cluster;
    char name83[12];
    if (!fat16_resolve(path, &dir_cluster, name83)) return false;
    return fat16_find_in_dir(dir_cluster, name83, out);
}

bool fat16_list_dir(uint16_t dir_cluster, dir_entry_t *buf, int max, int *count) {
    list_ctx ctx = { buf, max, 0 };
    bool ok = dir_iterate(dir_cluster, list_cb, &ctx);
    *count = ctx.count;
    return ok;
}

static bool split_path(const char *path, char *component, int comp_size, const char **remaining) {
    if (!path || !*path || *path != '/') return false;

    path++;

    int i = 0;
    while (*path && *path != '/' && i < comp_size - 1) {
        component[i++] = *path++;
    }
    component[i] = '\0';

    if (*path == '/') {
        *remaining = path;
    } else {
        *remaining = NULL;
    }

    return i > 0;
}

bool fat16_resolve(const char *path, uint16_t *out_cluster, char *out_name83) {
    if (!path || !*path) return false;

    uint16_t current = dir_context.current_cluster;
    const char *remaining = path;
    char component[256];

    if (*path == '/') {
        current = 0;
    }

    while (split_path(remaining, component, 256, &remaining)) {
        if (!remaining) {
            fat16_make_83(component, out_name83);
            *out_cluster = current;
            return true;
        }

        char name83[12];
        fat16_make_83(component, name83);

        dir_entry_t entry;
        if (!fat16_find_in_dir(current, name83, &entry)) return false;
        if (!(entry.attr & ATTR_DIRECTORY)) return false;

        current = entry.cluster_lo;
    }

    return false;
}

bool fat16_chdir(const char *path) {
    if (!path || !*path) return false;

    uint16_t target_cluster;

    if (strcmp(path, "/") == 0) {
        target_cluster = 0;
    } else if (strcmp(path, "..") == 0) {
        if (dir_context.current_cluster == 0) return false;
        char name83[12];
        fat16_make_83("..", name83);

        dir_entry_t entry;
        if (!fat16_find_in_dir(dir_context.current_cluster, name83, &entry)) return false;
        target_cluster = entry.cluster_lo;
    } else if (*path == '/') {
        char final_name[12];
        if (!fat16_resolve(path, &target_cluster, final_name)) return false;

        dir_entry_t entry;
        if (!fat16_find_in_dir(target_cluster, final_name, &entry)) return false;
        if (!(entry.attr & ATTR_DIRECTORY)) return false;
        target_cluster = entry.cluster_lo;
    } else {
        char name83[12];
        fat16_make_83(path, name83);

        dir_entry_t entry;
        if (!fat16_find_in_dir(dir_context.current_cluster, name83, &entry)) return false;
        if (!(entry.attr & ATTR_DIRECTORY)) return false;
        target_cluster = entry.cluster_lo;
    }

    dir_context.current_cluster = target_cluster;
    strcpy(dir_context.path, path);
    return true;
}

bool fat16_pwd(char *buf, int buflen) {
    if (!buf || buflen < 2) return false;
    strncpy(buf, dir_context.path, buflen - 1);
    buf[buflen - 1] = '\0';
    return true;
}

bool fat16_open(const char *path, fat16_file_t *f) {
    uint16_t dir_cluster;
    char name83[12];

    if (!fat16_resolve(path, &dir_cluster, name83)) return false;

    dir_entry_t entry;
    if (!fat16_find_in_dir(dir_cluster, name83, &entry)) return false;

    memset(f, 0, sizeof(fat16_file_t));
    f->entry = entry;
    f->cur_cluster = entry.cluster_lo;
    f->dir_cluster = dir_cluster;
    f->open = true;

    uint32_t lba = (dir_cluster == 0) ? fs.root_lba : cluster_to_lba(dir_cluster);
    if (!ata_read_sector(lba, sector_buf)) return false;

    dir_entry_t *entries = (dir_entry_t *)sector_buf;
    for (int i = 0; i < 16; i++) {
        if (name83_eq(entries[i].name, name83)) {
            f->dir_entry_lba = lba;
            f->dir_entry_idx = i;
            break;
        }
    }

    return true;
}

static bool alloc_dir_entry(uint16_t dir_cluster, uint32_t *out_lba, int *out_idx) {
    uint32_t lba;
    if (dir_cluster == 0) {
        uint32_t root_sectors = (fs.bpb.root_entry_count * 32 + 511) / 512;
        for (uint32_t s = 0; s < root_sectors; s++) {
            lba = fs.root_lba + s;
            if (!ata_read_sector(lba, sector_buf)) return false;
            dir_entry_t *entries = (dir_entry_t *)sector_buf;
            for (int i = 0; i < 16; i++) {
                if (entries[i].name[0] == DIR_ENTRY_FREE || entries[i].name[0] == DIR_ENTRY_END) {
                    *out_lba = lba;
                    *out_idx = i;
                    return true;
                }
            }
        }
    } else {
        uint16_t cluster = dir_cluster;
        while (cluster >= 2 && cluster < FAT16_RESERVED) {
            uint32_t start_lba = cluster_to_lba(cluster);
            for (uint32_t s = 0; s < fs.bpb.sectors_per_cluster; s++) {
                lba = start_lba + s;
                if (!ata_read_sector(lba, sector_buf)) return false;
                dir_entry_t *entries = (dir_entry_t *)sector_buf;
                for (int i = 0; i < 16; i++) {
                    if (entries[i].name[0] == DIR_ENTRY_FREE || entries[i].name[0] == DIR_ENTRY_END) {
                        *out_lba = lba;
                        *out_idx = i;
                        return true;
                    }
                }
            }
            cluster = fat_get(cluster);
        }
    }
    return false;
}

bool fat16_create(const char *path, fat16_file_t *f) {
    uint16_t dir_cluster;
    char name83[12];

    if (!fat16_resolve(path, &dir_cluster, name83)) return false;

    dir_entry_t existing;
    if (fat16_find_in_dir(dir_cluster, name83, &existing)) return false;

    uint32_t lba; int idx;
    if (!alloc_dir_entry(dir_cluster, &lba, &idx)) return false;

    uint16_t cluster = fat_alloc();
    if (cluster == 0) return false;

    if (!ata_read_sector(lba, sector_buf)) return false;
    dir_entry_t *entries = (dir_entry_t *)sector_buf;

    uint8_t first = entries[idx].name[0];
    memset(&entries[idx], 0, sizeof(dir_entry_t));
    memcpy(entries[idx].name, name83, 11);
    entries[idx].attr = ATTR_ARCHIVE;
    entries[idx].cluster_lo = cluster;
    entries[idx].file_size = 0;

    if (first == DIR_ENTRY_END && idx + 1 < 16)
        entries[idx + 1].name[0] = DIR_ENTRY_END;

    if (!ata_write_sector(lba, sector_buf)) return false;

    memset(f, 0, sizeof(fat16_file_t));
    f->entry = entries[idx];
    f->dir_entry_lba = lba;
    f->dir_entry_idx = idx;
    f->cur_cluster = cluster;
    f->dir_cluster = dir_cluster;
    f->open = true;
    return true;
}

bool fat16_mkdir(const char *path) {
    uint16_t dir_cluster;
    char name83[12];

    if (!fat16_resolve(path, &dir_cluster, name83)) return false;

    dir_entry_t existing;
    if (fat16_find_in_dir(dir_cluster, name83, &existing)) return false;

    uint32_t lba; int idx;
    if (!alloc_dir_entry(dir_cluster, &lba, &idx)) return false;

    uint16_t cluster = fat_alloc();
    if (cluster == 0) return false;

    if (!ata_read_sector(lba, sector_buf)) return false;
    dir_entry_t *entries = (dir_entry_t *)sector_buf;

    uint8_t first = entries[idx].name[0];
    memset(&entries[idx], 0, sizeof(dir_entry_t));
    memcpy(entries[idx].name, name83, 11);
    entries[idx].attr = ATTR_DIRECTORY;
    entries[idx].cluster_lo = cluster;
    entries[idx].file_size = 0;

    if (first == DIR_ENTRY_END && idx + 1 < 16)
        entries[idx + 1].name[0] = DIR_ENTRY_END;

    if (!ata_write_sector(lba, sector_buf)) return false;

    uint8_t dir_block[512];
    memset(dir_block, 0, 512);
    dir_entry_t *dot_entries = (dir_entry_t *)dir_block;

    memcpy(dot_entries[0].name, ".          ", 11);
    dot_entries[0].attr = ATTR_DIRECTORY;
    dot_entries[0].cluster_lo = cluster;

    memcpy(dot_entries[1].name, "..         ", 11);
    dot_entries[1].attr = ATTR_DIRECTORY;
    dot_entries[1].cluster_lo = dir_cluster;

    dot_entries[2].name[0] = DIR_ENTRY_END;

    uint32_t new_dir_lba = cluster_to_lba(cluster);
    if (!ata_write_sector(new_dir_lba, dir_block)) return false;

    return true;
}
