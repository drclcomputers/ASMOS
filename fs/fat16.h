#ifndef FAT16_H
#define FAT16_H

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

#define DRIVE_HDA 0  /* Primary ATA master  (boot drive) */
#define DRIVE_HDB 1  /* Primary ATA slave */
#define DRIVE_FDD0 2 /* Floppy A: */
#define DRIVE_FDD1 3 /* Floppy B: */
#define DRIVE_COUNT 4

typedef struct {
    bpb_t bpb;
    uint32_t fat_lba;
    uint32_t root_lba;
    uint32_t data_lba;
    uint32_t cluster_count;
    bool mounted;
    uint8_t drive_id; /* physical drive */
    char label[12];   /* volume label */
} fat16_fs_t;

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
} fat16_file_t;

typedef struct {
    uint16_t current_cluster;
    char path[256];
    uint8_t drive_id;
} fat16_dir_context_t;

extern fat16_fs_t fs;                    /* current active volume   */
extern fat16_fs_t g_drives[DRIVE_COUNT]; /* all mounted volumes     */
extern fat16_dir_context_t dir_context;

#define PROTECTED_PATH_COUNT 2
extern const char *g_protected_paths[PROTECTED_PATH_COUNT];

bool path_is_protected(const char *name_or_path);

/* Drive management */
bool fat16_mount_drive(uint8_t drive_id);
bool fat16_select_drive(uint8_t drive_id);
uint8_t fat16_current_drive(void);
const char *fat16_drive_label(uint8_t drive_id);
bool fat16_drive_mounted(uint8_t drive_id);

/* init */
bool fat16_mount(void);
bool fat16_get_usage(uint32_t *total_bytes, uint32_t *used_bytes);

/* path */
void fat16_make_83(const char *filename, char *out83);
bool fat16_resolve(const char *path, uint16_t *out_cluster, char *out_name83);
bool fat16_chdir(const char *path);
bool fat16_pwd(char *buf, int buflen);

/* dirs */
bool fat16_mkdir(const char *path);
bool fat16_find_in_dir(uint16_t dir_cluster, const char *name83,
                       dir_entry_t *out);
bool fat16_find(const char *path, dir_entry_t *out);
bool fat16_list_dir(uint16_t dir_cluster, dir_entry_t *buf, int max,
                    int *count);
bool is_dir_empty(uint16_t cluster);
void fat16_wipe_cluster(uint16_t cluster);

/* files */
bool fat16_open(const char *path, fat16_file_t *f);
bool fat16_create(const char *path, fat16_file_t *f);
int fat16_read(fat16_file_t *f, void *buf, int len);
int fat16_write(fat16_file_t *f, const void *buf, int len);
bool fat16_seek(fat16_file_t *f, uint32_t offset);
int fat16_tell(fat16_file_t *f);
bool fat16_close(fat16_file_t *f);

/* file and dir */
bool fat16_rename(const char *path, const char *new_name);
bool fat16_delete(const char *path);
bool fat16_rmdir(const char *path);
bool fat16_rm_rf(const char *path);

bool fat16_copy_file(const char *src_path, const char *dest_path);
bool fat16_move_file(const char *src_path, const char *dest_path);

bool fat16_copy_dir(const char *src_path, const char *dest_path);
bool fat16_move_dir(const char *src_path, const char *dest_path);

/* hidden file helpers */
bool fat16_set_hidden(const char *path, bool hidden);
bool fat16_is_hidden(const char *path);

#endif
