#include "fs/fat_io.h"
#include "fs/fs.h"
#include "lib/memory.h"

static fat_vol_t *vol(uint8_t drive_id) { return &g_drives[drive_id]; }

bool fat_cluster_valid(uint8_t drive_id, uint16_t cluster) {
    fat_vol_t *v = vol(drive_id);
    if (cluster < 2)
        return false;
    if (cluster >= (uint16_t)(v->cluster_count + 2))
        return false;
    return cluster < vol_reserved(v);
}

uint32_t fat_cluster_to_lba(uint8_t drive_id, uint16_t cluster) {
    if (!fat_cluster_valid(drive_id, cluster))
        return 0xFFFFFFFF;
    fat_vol_t *v = vol(drive_id);
    return v->data_lba + (uint32_t)(cluster - 2) * v->bpb.sectors_per_cluster;
}

uint16_t fat_get(uint8_t drive_id, uint16_t cluster) {
    fat_vol_t *v = vol(drive_id);
    if (cluster < 2 || cluster >= (uint16_t)(v->cluster_count + 2))
        return vol_bad(v);

    if (v->fat_type == FAT_TYPE_FAT12) {
        uint32_t bit_offset = cluster * 12;
        uint32_t byte_offset = bit_offset / 8;
        uint32_t fat_sector = v->fat_lba + byte_offset / 512;
        uint32_t off_in_sec = byte_offset % 512;

        uint8_t buf0[512], buf1[512];
        if (!fs_read_sector(drive_id, fat_sector, buf0))
            return vol_bad(v);

        uint16_t val;
        if (off_in_sec == 511) {
            if (!fs_read_sector(drive_id, fat_sector + 1, buf1))
                return vol_bad(v);
            val = buf0[511] | ((uint16_t)buf1[0] << 8);
        } else {
            val = buf0[off_in_sec] | ((uint16_t)buf0[off_in_sec + 1] << 8);
        }

        val = (cluster & 1) ? (val >> 4) : (val & 0x0FFF);

        if (val >= FAT12_RESERVED)
            return FAT16_EOC;
        if (val == FAT12_BAD)
            return FAT16_BAD;
        return val;
    }

    uint32_t fat_sector = v->fat_lba + (cluster / 256);
    uint16_t fat_offset = (cluster % 256) * 2;
    uint8_t local_buf[512];
    if (!fs_read_sector(drive_id, fat_sector, local_buf))
        return FAT16_BAD;
    return *(uint16_t *)(local_buf + fat_offset);
}

bool fat_set(uint8_t drive_id, uint16_t cluster, uint16_t value) {
    fat_vol_t *v = vol(drive_id);
    if (cluster < 2 || cluster >= (uint16_t)(v->cluster_count + 2))
        return false;

    if (v->fat_type == FAT_TYPE_FAT12) {
        uint16_t val12;
        if (value == FAT16_FREE)
            val12 = FAT12_FREE;
        else if (value >= FAT16_EOC)
            val12 = 0xFFF;
        else if (value == FAT16_BAD)
            val12 = FAT12_BAD;
        else
            val12 = value;

        uint32_t bit_offset = cluster * 12;
        uint32_t byte_offset = bit_offset / 8;
        uint32_t off_in_sec = byte_offset % 512;

        for (int copy = 0; copy < v->bpb.fat_count; copy++) {
            uint32_t lba0 =
                v->fat_lba + copy * v->bpb.sectors_per_fat + byte_offset / 512;
            uint8_t buf0[512], buf1[512];
            bool straddle = (off_in_sec == 511);

            if (!fs_read_sector(drive_id, lba0, buf0))
                return false;
            if (straddle && !fs_read_sector(drive_id, lba0 + 1, buf1))
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

            if (!fs_write_sector(drive_id, lba0, buf0))
                return false;
            if (straddle && !fs_write_sector(drive_id, lba0 + 1, buf1))
                return false;
        }
        return true;
    }

    uint32_t fat_sector = cluster / 256;
    uint16_t fat_offset = (cluster % 256) * 2;
    uint8_t local_buf[512];
    for (int copy = 0; copy < v->bpb.fat_count; copy++) {
        uint32_t lba = v->fat_lba + copy * v->bpb.sectors_per_fat + fat_sector;
        if (!fs_read_sector(drive_id, lba, local_buf))
            return false;
        *(uint16_t *)(local_buf + fat_offset) = value;
        if (!fs_write_sector(drive_id, lba, local_buf))
            return false;
    }
    return true;
}

uint16_t fat_alloc(uint8_t drive_id) {
    fat_vol_t *v = vol(drive_id);
    for (uint16_t c = 2; c < (uint16_t)(v->cluster_count + 2); c++) {
        if (fat_get(drive_id, c) == FAT16_FREE) {
            if (!fat_set(drive_id, c, FAT16_EOC))
                return 0;
            return c;
        }
    }
    return 0;
}

void fat_free_chain(uint8_t drive_id, uint16_t cluster) {
    fat_vol_t *v = vol(drive_id);
    while (vol_cluster_active(v, cluster)) {
        uint16_t next = fat_get(drive_id, cluster);
        fat_set(drive_id, cluster, FAT16_FREE);
        cluster = next;
    }
}
