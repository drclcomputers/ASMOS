#ifndef FS_H
#define FS_H

#include "lib/core.h"

typedef struct __attribute__((packed)) {
    uint8_t jmp[3];
    uint8_t oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t media_type;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t head_count;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint8_t drive_number;
    uint8_t reserved;
    uint8_t boot_sig;
    uint32_t volume_id;
    uint8_t volume_label[11];
    uint8_t fs_type[8];
} bpb_t;

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYSTEM 0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE 0x20

#define DIR_ENTRY_FREE 0xE5
#define DIR_ENTRY_END 0x00

typedef struct __attribute__((packed)) {
    uint8_t name[8];
    uint8_t ext[3];
    uint8_t attr;
    uint8_t reserved;
    uint8_t create_time_ms;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t cluster_hi;
    uint16_t modify_time;
    uint16_t modify_date;
    uint16_t cluster_lo;
    uint32_t file_size;
} dir_entry_t;

#define FAT16_FREE 0x0000
#define FAT16_RESERVED 0xFFF0
#define FAT16_BAD 0xFFF7
#define FAT16_EOC 0xFFF8

#define FAT12_FREE 0x000
#define FAT12_RESERVED 0xFF0
#define FAT12_BAD 0xFF7
#define FAT12_EOC 0xFF8

#define DRIVE_HDA 0
#define DRIVE_HDB 1
#define DRIVE_FDD0 2
#define DRIVE_FDD1 3
#define DRIVE_COUNT 4

typedef enum {
    FAT_TYPE_FAT12 = 12,
    FAT_TYPE_FAT16 = 16,
} fat_type_t;

typedef struct {
    bpb_t bpb;
    uint32_t fat_lba;
    uint32_t root_lba;
    uint32_t data_lba;
    uint32_t cluster_count;
    bool mounted;
    uint8_t drive_id;
    char label[12];
    fat_type_t fat_type;
} fat_vol_t;

typedef struct {
    dir_entry_t entry;
    uint32_t dir_entry_lba;
    int dir_entry_idx;
    uint16_t cur_cluster;
    uint32_t cur_offset;
    bool open;
    uint16_t parent_cluster;
    uint16_t dir_cluster;
    uint8_t drive_id;
} fat_file_t;

typedef struct {
    uint8_t drive_id;
    uint16_t current_cluster;
    char path[256];
} fat_dir_ctx_t;

typedef struct {
    const char *path;
    uint8_t drive_id;
} vfs_mount_t;

extern const char *g_drive_paths[DRIVE_COUNT];

#define VFS_MOUNT_COUNT 3
extern vfs_mount_t g_vfs_mounts[VFS_MOUNT_COUNT];

#define PROTECTED_PATH_COUNT 5
extern const char *g_protected_paths[PROTECTED_PATH_COUNT];

extern fat_vol_t g_drives[DRIVE_COUNT];
extern fat_dir_ctx_t dir_context;

bool path_is_protected(const char *name_or_path);

bool fs_mount(void);
bool fs_mount_drive(uint8_t drive_id);
bool fs_select_drive(uint8_t drive_id);
uint8_t fs_current_drive(void);
const char *fs_drive_label(uint8_t drive_id);
bool fs_drive_mounted(uint8_t drive_id);
bool fs_resolve_dir(const char *path, uint8_t *out_drive, uint16_t *out_cluster);
bool fs_get_usage(uint32_t *total_bytes, uint32_t *used_bytes);
bool fs_get_usage_drive(uint8_t drive_id, uint32_t *total_bytes, uint32_t *used_bytes);

bool fs_read_sector(uint8_t drive_id, uint32_t lba, void *buf);
bool fs_write_sector(uint8_t drive_id, uint32_t lba, const void *buf);

void fs_make_83(const char *filename, char *out83);
bool fs_resolve(const char *path, uint8_t *out_drive,
                uint16_t *out_parent_cluster, char out_name83[12]);
bool fs_chdir(const char *path);
bool fs_pwd(char *buf, int buflen);

bool fs_mkdir(const char *path);
bool fs_find_in_dir(uint8_t drive_id, uint16_t dir_cluster, const char *name83,
                    dir_entry_t *out);
bool fs_find(const char *path, dir_entry_t *out);
bool fs_list_dir(uint8_t drive_id, uint16_t dir_cluster, dir_entry_t *buf,
                 int max, int *count);
bool fs_is_dir_empty(uint8_t drive_id, uint16_t cluster);
void fs_wipe_cluster(uint8_t drive_id, uint16_t cluster);

bool fs_open(const char *path, fat_file_t *f);
bool fs_create(const char *path, fat_file_t *f);
int fs_read(fat_file_t *f, void *buf, int len);
int fs_write(fat_file_t *f, const void *buf, int len);
bool fs_seek(fat_file_t *f, uint32_t offset);
int fs_tell(fat_file_t *f);
bool fs_close(fat_file_t *f);

bool fs_rename(const char *path, const char *new_name);
bool fs_delete(const char *path);
bool fs_rmdir(const char *path);
bool fs_rm_rf(const char *path);

bool fs_copy_file(const char *src, const char *dst);
bool fs_move_file(const char *src, const char *dst);
bool fs_copy_dir(const char *src, const char *dst);
bool fs_move_dir(const char *src, const char *dst);

bool fs_set_hidden(const char *path, bool hidden);
bool fs_is_hidden(const char *path);

typedef fat_vol_t fs_fs_t;
typedef fat_file_t fs_file_t;

static inline uint16_t vol_reserved(const fat_vol_t *v) {
    return (v->fat_type == FAT_TYPE_FAT12) ? FAT12_RESERVED : FAT16_RESERVED;
}
static inline uint16_t vol_eoc(const fat_vol_t *v) {
    return (v->fat_type == FAT_TYPE_FAT12) ? FAT12_EOC : FAT16_EOC;
}
static inline uint16_t vol_bad(const fat_vol_t *v) {
    return (v->fat_type == FAT_TYPE_FAT12) ? FAT12_BAD : FAT16_BAD;
}
static inline bool vol_is_eoc(const fat_vol_t *v, uint16_t c) {
    return c >= vol_reserved(v);
}
static inline bool vol_cluster_active(const fat_vol_t *v, uint16_t c) {
    return c >= 2 && c < vol_reserved(v);
}

#endif
