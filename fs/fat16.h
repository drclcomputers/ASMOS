#ifndef FAT16_H
#define FAT16_H

#include "lib/types.h"


typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];            // jump instruction (0xEB, xx, 0x90)
    uint8_t  oem[8];            // OEM name, e.g. "MYOS    "
    uint16_t bytes_per_sector;  // almost always 512
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;  // sectors before FAT (includes boot sector)
    uint8_t  fat_count;         // number of FAT copies (2)
    uint16_t root_entry_count;  // max root dir entries (512 for FAT16)
    uint16_t total_sectors_16;  // total sectors if < 65536, else 0
    uint8_t  media_type;        // 0xF8 = fixed disk
    uint16_t sectors_per_fat;   // sectors occupied by one FAT
    uint16_t sectors_per_track; // for CHS geometry
    uint16_t head_count;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;  // used when total_sectors_16 == 0

    uint8_t  drive_number;      // 0x80 = first hard disk
    uint8_t  reserved;
    uint8_t  boot_sig;          // 0x29 = extended boot record present
    uint32_t volume_id;
    uint8_t  volume_label[11];  // e.g. "MYOS       "
    uint8_t  fs_type[8];        // "FAT16   "
} bpb_t;


#define ATTR_READ_ONLY  0x01
#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define ATTR_VOLUME_ID  0x08
#define ATTR_DIRECTORY  0x10
#define ATTR_ARCHIVE    0x20

#define DIR_ENTRY_FREE      0xE5    // first byte of name = deleted entry
#define DIR_ENTRY_END       0x00    // first byte of name = no more entries

typedef struct __attribute__((packed)) {
    uint8_t  name[8];
    uint8_t  ext[3];
    uint8_t  attr;
    uint8_t  reserved;
    uint8_t  create_time_ms;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t cluster_hi;
    uint16_t modify_time;
    uint16_t modify_date;
    uint16_t cluster_lo;
    uint32_t file_size;
} dir_entry_t;

#define FAT16_FREE      0x0000
#define FAT16_RESERVED  0xFFF0
#define FAT16_BAD       0xFFF7
#define FAT16_EOC       0xFFF8  // end of chain (FFF8-FFFF all mean EOC)


typedef struct {
    bpb_t    bpb;
    uint32_t fat_lba;           // LBA of first FAT
    uint32_t root_lba;          // LBA of root directory
    uint32_t data_lba;          // LBA of first data cluster
    uint32_t cluster_count;
    bool     mounted;
} fat16_fs_t;

extern fat16_fs_t fs;


typedef struct {
    dir_entry_t entry;
    uint32_t    dir_entry_lba;  // LBA of the sector containing this entry
    int         dir_entry_idx;  // index within that sector (0-15)
    uint16_t    cur_cluster;
    uint32_t    cur_offset;     // byte offset within file
    bool        open;
} fat16_file_t;

bool fat16_mount(void);

bool fat16_find(const char *name83, dir_entry_t *out);
bool fat16_list(dir_entry_t *buf, int max, int *count);

bool fat16_open(const char *name83, fat16_file_t *f);
bool fat16_create(const char *name83, fat16_file_t *f);
int  fat16_read(fat16_file_t *f, void *buf, int len);
int  fat16_write(fat16_file_t *f, const void *buf, int len);
bool fat16_close(fat16_file_t *f);
bool fat16_delete(const char *name83);

void fat16_make_83(const char *filename, char *out83);

#endif
