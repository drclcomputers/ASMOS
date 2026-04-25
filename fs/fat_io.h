#ifndef FAT_IO_H
#define FAT_IO_H

#include "fs/fs.h"

uint16_t fat_get(uint8_t drive_id, uint16_t cluster);
bool fat_set(uint8_t drive_id, uint16_t cluster, uint16_t value);
uint16_t fat_alloc(uint8_t drive_id);
void fat_free_chain(uint8_t drive_id, uint16_t cluster);

uint32_t fat_cluster_to_lba(uint8_t drive_id, uint16_t cluster);
bool fat_cluster_valid(uint8_t drive_id, uint16_t cluster);

#endif
