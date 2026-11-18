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

/* Long file names (VFAT): names up to FS_NAME_MAX characters, paths up to
 * FS_PATH_MAX. Buffers: char name[FS_NAME_SZ], path[FS_PATH_SZ]. */
#define FS_NAME_MAX 63
#define FS_NAME_SZ (FS_NAME_MAX + 1)
#define FS_PATH_MAX 511
#define FS_PATH_SZ (FS_PATH_MAX + 1)

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
#define DRIVE_CD0 4 /* the CD-ROM: ISO 9660, read only (fs/iso9660.h) */
#define DRIVE_COUNT 5

typedef enum {
    FAT_TYPE_FAT12 = 12,
    FAT_TYPE_FAT16 = 16,
} fat_type_t;

typedef struct {
    bpb_t bpb;
    uint32_t base_lba; /* where the volume starts on the disk: a partition's
                        * first sector (0: a floppy, or a disk without one) */
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
    char path[FS_PATH_SZ];
} fat_dir_ctx_t;

typedef struct {
    const char *path;
    uint8_t drive_id;
} vfs_mount_t;

extern const char *g_drive_paths[DRIVE_COUNT];

#define VFS_MOUNT_COUNT 4
extern vfs_mount_t g_vfs_mounts[VFS_MOUNT_COUNT];

#define PROTECTED_PATH_COUNT 8
extern const char *g_protected_paths[PROTECTED_PATH_COUNT];

extern fat_vol_t g_drives[DRIVE_COUNT];
extern fat_dir_ctx_t dir_context;

bool path_is_protected(const char *name_or_path);

bool fs_mount(void);
void fs_unmount(uint8_t drive_id);
bool fs_mount_drive(uint8_t drive_id);
/* goes up whenever a file or folder is made, deleted or renamed, or a disk
 * is mounted: for views that list folders (the Apps menu) */
extern uint32_t g_fs_rev;
/* one count per drive, up on every mount and unmount: a window showing a
 * drive sees it was taken out, or put back with another disk */
extern uint8_t g_mount_gen[DRIVE_COUNT];
bool fs_select_drive(uint8_t drive_id);
uint8_t fs_current_drive(void);
const char *fs_drive_label(uint8_t drive_id);
bool fs_drive_mounted(uint8_t drive_id);
/* the CD drive: look whether a disc went in or out (a few times a second at
 * most); true if /CD0 came or went */
bool fs_cd_poll(void);
/* a floppy taken out: unmounted (twice a second; silent). true if one
 * went; *lost: a drive whose last writes never got to it, or DRIVE_COUNT */
bool fs_fdd_poll(uint8_t *lost);
bool fs_resolve_dir(const char *path, uint8_t *out_drive,
                    uint16_t *out_cluster);
bool fs_get_usage(uint32_t *total_bytes, uint32_t *used_bytes);
bool fs_get_usage_drive(uint8_t drive_id, uint32_t *total_bytes,
                        uint32_t *used_bytes);
/* drop the cached usage count (called whenever the FAT changes) */
void fs_usage_invalidate(uint8_t drive_id);

bool fs_read_sector(uint8_t drive_id, uint32_t lba, void *buf);
bool fs_write_sector(uint8_t drive_id, uint32_t lba, const void *buf);

void fs_make_83(const char *filename, char *out83);
bool fs_name_char_ok(char c);
/* Validate a user-typed name (long names allowed); on failure *err gets a
 * short message. */
bool fs_validate_name(const char *name, bool is_dir, const char **err);
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
/* the same, with each one's name to show (its long name, if it has one);
 * buf or names may be NULL */
bool fs_list_dir_names(uint8_t drive_id, uint16_t dir_cluster, dir_entry_t *buf,
                       char (*names)[FS_NAME_SZ], int max, int *count);
/* each file and folder (not . and ..) with the name to show; cb returns
 * false to stop. No arrays: any number of entries. */
typedef bool (*fs_walk_cb)(const dir_entry_t *e, const char *name, void *user);
bool fs_walk_dir(uint8_t drive_id, uint16_t dir_cluster, fs_walk_cb cb,
                 void *user);
/* find by long or 8.3 name, any case (fs_find_in_dir: exact 8.3 only) */
bool fs_find_name(uint8_t drive_id, uint16_t dir_cluster, const char *name,
                  dir_entry_t *out);
/* the name to show for the last part of path (its long name if any) */
bool fs_name_of(const char *path, char out[FS_NAME_SZ]);
/* an entry's 8.3 name as text, "README.TXT" */
void fs_entry_name(const dir_entry_t *e, char out[13]);
/* fits 8.3 as typed (any case) */
bool fs_name_is_83(const char *name);
/* may be in a long name: not a control character or \ / : * ? " < > | */
bool fs_long_char_ok(char c);
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
/* the entry's attribute byte (ATTR_*; the folder bit is kept as it was) */
bool fs_set_attr(const char *path, uint8_t attr);

/* ── disks and partitions (fs/fs.c, fs/install.c) ──────────────────────
 * A hard disk is either partitioned (an MBR in sector 0: the volume is a
 * FAT partition in its table) or one volume from sector 0 (a "superfloppy",
 * like the old 32 MB ASMOS image). fs_read_sector / fs_write_sector add
 * the volume's start (base_lba): sector 0 is always the volume's own boot
 * sector. */
#define PART_TYPE_FAT12 0x01
#define PART_TYPE_FAT16_SMALL 0x04 /* under 32 MB */
#define PART_TYPE_FAT16 0x06
#define PART_TYPE_FAT16_LBA 0x0E
bool fs_bpb_valid(const uint8_t *sector); /* a FAT12/16 boot sector */
bool fs_part_is_fat(uint8_t type);
/* the FAT partition to use in an MBR (sector 0): the one starting at
 * prefer_lba if there is one (the one booted from), else the active one,
 * else the first. false: no FAT partition (or no partition table). */
bool fs_pick_partition(const uint8_t *mbr, uint32_t prefer_lba, uint32_t *lba,
                       uint32_t *sectors);
/* kmain: the volume the loader started (bootdefs.inc 0x060C), so that
 * one is HDA's when a disk has several */
void fs_set_boot_volume(uint32_t lba);
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
