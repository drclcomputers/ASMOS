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
    // extension
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

typedef bool (*dir_cb)(dir_entry_t *entry, uint32_t lba, int idx, void *user);

static bool root_dir_iterate(dir_cb cb, void *user) {
    uint32_t root_sectors = (fs.bpb.root_entry_count * 32 + 511) / 512;

    for (uint32_t s = 0; s < root_sectors; s++) {
        uint32_t lba = fs.root_lba + s;
        if (!ata_read_sector(lba, sector_buf)) return false;

        dir_entry_t *entries = (dir_entry_t *)sector_buf;
        for (int i = 0; i < 16; i++) {
            if (entries[i].name[0] == DIR_ENTRY_END) return true;
            if (entries[i].name[0] == DIR_ENTRY_FREE) continue;
            if (entries[i].attr & ATTR_VOLUME_ID)     continue;

            if (!cb(&entries[i], lba, i, user)) return false;
        }
    }
    return true;
}

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

bool fat16_find(const char *name83, dir_entry_t *out) {
    find_ctx ctx = { name83, out, false };
    root_dir_iterate(find_cb, &ctx);
    return ctx.found;
}

typedef struct { dir_entry_t *buf; int max; int count; } list_ctx;

static bool list_cb(dir_entry_t *e, uint32_t lba, int idx, void *user) {
    (void)lba; (void)idx;
    list_ctx *ctx = (list_ctx *)user;
    if (ctx->count < ctx->max)
        ctx->buf[ctx->count++] = *e;
    return true;
}

bool fat16_list(dir_entry_t *buf, int max, int *count) {
    list_ctx ctx = { buf, max, 0 };
    bool ok = root_dir_iterate(list_cb, &ctx);
    *count = ctx.count;
    return ok;
}

bool fat16_open(const char *name83, fat16_file_t *f) {
    dir_entry_t entry;
    if (!fat16_find(name83, &entry)) return false;

    memset(f, 0, sizeof(fat16_file_t));
    f->entry       = entry;
    f->cur_cluster = entry.cluster_lo;
    f->open        = true;

    uint32_t root_sectors = (fs.bpb.root_entry_count * 32 + 511) / 512;
    for (uint32_t s = 0; s < root_sectors; s++) {
        uint32_t lba = fs.root_lba + s;
        if (!ata_read_sector(lba, sector_buf)) return false;
        dir_entry_t *entries = (dir_entry_t *)sector_buf;
        for (int i = 0; i < 16; i++) {
            if (entries[i].name[0] == DIR_ENTRY_END) goto done_scan;
            if (name83_eq(entries[i].name, name83)) {
                f->dir_entry_lba = lba;
                f->dir_entry_idx = i;
                goto done_scan;
            }
        }
    }
    done_scan:
    return true;
}

bool fat16_create(const char *name83, fat16_file_t *f) {
    dir_entry_t existing;
    if (fat16_find(name83, &existing)) return false;

    uint16_t cluster = fat_alloc();
    if (cluster == 0) return false;

    uint32_t root_sectors = (fs.bpb.root_entry_count * 32 + 511) / 512;
    for (uint32_t s = 0; s < root_sectors; s++) {
        uint32_t lba = fs.root_lba + s;
        if (!ata_read_sector(lba, sector_buf)) return false;
        dir_entry_t *entries = (dir_entry_t *)sector_buf;

        for (int i = 0; i < 16; i++) {
            uint8_t first = entries[i].name[0];
            if (first == DIR_ENTRY_FREE || first == DIR_ENTRY_END) {
                memset(&entries[i], 0, sizeof(dir_entry_t));
                memcpy(entries[i].name, name83, 11);
                entries[i].attr       = ATTR_ARCHIVE;
                entries[i].cluster_lo = cluster;
                entries[i].file_size  = 0;

                if (first == DIR_ENTRY_END && i + 1 < 16)
                    entries[i + 1].name[0] = DIR_ENTRY_END;

                if (!ata_write_sector(lba, sector_buf)) return false;

                memset(f, 0, sizeof(fat16_file_t));
                f->entry           = entries[i];
                f->dir_entry_lba   = lba;
                f->dir_entry_idx   = i;
                f->cur_cluster     = cluster;
                f->open            = true;
                return true;
            }
        }
    }
    return false;
}

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

bool fat16_delete(const char *name83) {
    uint32_t root_sectors = (fs.bpb.root_entry_count * 32 + 511) / 512;

    for (uint32_t s = 0; s < root_sectors; s++) {
        uint32_t lba = fs.root_lba + s;
        if (!ata_read_sector(lba, sector_buf)) return false;
        dir_entry_t *entries = (dir_entry_t *)sector_buf;

        for (int i = 0; i < 16; i++) {
            if (entries[i].name[0] == DIR_ENTRY_END) return false;
            if (entries[i].name[0] == DIR_ENTRY_FREE) continue;
            if (name83_eq(entries[i].name, name83)) {
                fat_free_chain(entries[i].cluster_lo);
                entries[i].name[0] = DIR_ENTRY_FREE;
                return ata_write_sector(lba, sector_buf);
            }
        }
    }
    return false;
}
