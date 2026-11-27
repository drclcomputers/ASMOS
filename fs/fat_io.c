// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "fs/fat_io.h"
#include "fs/fs.h"
#include "lib/memory.h"

static fat_vol_t *vol(uint8_t drive_id) { return &g_drives[drive_id]; }

// FAT sector cache
/*
 * Cache frequently used FAT sectors to avoid repeated reads during file
 * operations. fat_cache_wrote() tracks writes; mount/unmount clears the
 * cache because formatting and Setup may write FATs directly.
 *
 * Between fat_batch_begin and fat_batch_end (one fs_write) the FAT's
 * changes stay here, and each sector changed is written once at the end,
 * not once per cluster: after the file's data, which is the safe order.
 */
#define FC_N 6
static struct {
    bool valid;
    bool dirty; /* changed in a batch, not written yet */
    uint8_t drive;
    uint32_t lba;
    uint32_t used;
    uint8_t data[512];
} s_fc[FC_N];
static uint32_t s_fc_clock;
static uint16_t s_next_free[DRIVE_COUNT]; /* where fat_alloc looks first */
static int s_batch = -1; /* the drive whose FAT writes wait, -1 none */

/* a cache slot for lba: its own, else an empty one, else the one used
 * longest ago (written out first if it waits to be) */
static int fc_slot(uint8_t drive_id, uint32_t lba, bool *hit) {
    int victim = -1;
    for (int i = 0; i < FC_N; i++) {
        if (s_fc[i].valid && s_fc[i].drive == drive_id && s_fc[i].lba == lba) {
            *hit = true;
            return i;
        }
        if (!s_fc[i].valid) {
            if (victim < 0 || s_fc[victim].valid)
                victim = i;
        } else if (victim < 0 ||
                   (s_fc[victim].valid && s_fc[i].used < s_fc[victim].used)) {
            victim = i;
        }
    }
    *hit = false;
    if (s_fc[victim].valid && s_fc[victim].dirty) {
        s_fc[victim].dirty = false;
        fs_write_sector(s_fc[victim].drive, s_fc[victim].lba,
                        s_fc[victim].data);
    }
    return victim;
}

static bool fat_read(uint8_t drive_id, uint32_t lba, uint8_t *out) {
    bool hit;
    int i = fc_slot(drive_id, lba, &hit);
    if (!hit) {
        if (!fs_read_sector(drive_id, lba, out))
            return false;
        s_fc[i].valid = true;
        s_fc[i].dirty = false;
        s_fc[i].drive = drive_id;
        s_fc[i].lba = lba;
        memcpy(s_fc[i].data, out, 512);
    }
    s_fc[i].used = ++s_fc_clock;
    memcpy(out, s_fc[i].data, 512);
    return true;
}

/* a FAT sector changed: on the disk now, or (in a batch) at its end */
static bool fat_write(uint8_t drive_id, uint32_t lba, const uint8_t *data) {
    if (s_batch != drive_id)
        return fs_write_sector(drive_id, lba, data);
    bool hit;
    int i = fc_slot(drive_id, lba, &hit);
    s_fc[i].valid = true;
    s_fc[i].dirty = true;
    s_fc[i].drive = drive_id;
    s_fc[i].lba = lba;
    s_fc[i].used = ++s_fc_clock;
    memcpy(s_fc[i].data, data, 512);
    return true;
}

void fat_batch_begin(uint8_t drive_id) {
    fat_batch_end();
    s_batch = drive_id;
}

bool fat_batch_end(void) {
    bool ok = true;
    s_batch = -1;
    for (int i = 0; i < FC_N; i++)
        if (s_fc[i].valid && s_fc[i].dirty) {
            s_fc[i].dirty = false;
            ok =
                fs_write_sector(s_fc[i].drive, s_fc[i].lba, s_fc[i].data) && ok;
        }
    return ok;
}

void fat_cache_wrote(uint8_t drive_id, uint32_t lba, const void *data) {
    for (int i = 0; i < FC_N; i++)
        if (s_fc[i].valid && s_fc[i].drive == drive_id && s_fc[i].lba == lba)
            memcpy(s_fc[i].data, data, 512);
}

void fat_cache_drop(uint8_t drive_id) {
    if (s_batch == drive_id)
        fat_batch_end();
    for (int i = 0; i < FC_N; i++)
        if (s_fc[i].drive == drive_id)
            s_fc[i].valid = false;
    if (drive_id < DRIVE_COUNT)
        s_next_free[drive_id] = 2;
}

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
        if (!fat_read(drive_id, fat_sector, buf0))
            return vol_bad(v);

        uint16_t val;
        if (off_in_sec == 511) {
            if (!fat_read(drive_id, fat_sector + 1, buf1))
                return vol_bad(v);
            val = buf0[511] | ((uint16_t)buf1[0] << 8);
        } else {
            val = buf0[off_in_sec] | ((uint16_t)buf0[off_in_sec + 1] << 8);
        }

        val = (cluster & 1) ? (val >> 4) : (val & 0x0FFF);

        if (val == FAT12_BAD) /* (0xFF7: before the end marks it is in) */
            return FAT16_BAD;
        if (val >= FAT12_RESERVED)
            return FAT16_EOC;
        return val;
    }

    uint32_t fat_sector = v->fat_lba + (cluster / 256);
    uint16_t fat_offset = (cluster % 256) * 2;
    uint8_t local_buf[512];
    if (!fat_read(drive_id, fat_sector, local_buf))
        return FAT16_BAD;
    return *(uint16_t *)(local_buf + fat_offset);
}

bool fat_set(uint8_t drive_id, uint16_t cluster, uint16_t value) {
    fat_vol_t *v = vol(drive_id);
    fs_usage_invalidate(drive_id);
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

            if (!fat_read(drive_id, lba0, buf0))
                return false;
            if (straddle && !fat_read(drive_id, lba0 + 1, buf1))
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

            if (!fat_write(drive_id, lba0, buf0))
                return false;
            if (straddle && !fat_write(drive_id, lba0 + 1, buf1))
                return false;
        }
        return true;
    }

    uint32_t fat_sector = cluster / 256;
    uint16_t fat_offset = (cluster % 256) * 2;
    uint8_t local_buf[512];
    for (int copy = 0; copy < v->bpb.fat_count; copy++) {
        uint32_t lba = v->fat_lba + copy * v->bpb.sectors_per_fat + fat_sector;
        if (!fat_read(drive_id, lba, local_buf))
            return false;
        *(uint16_t *)(local_buf + fat_offset) = value;
        if (!fat_write(drive_id, lba, local_buf))
            return false;
    }
    return true;
}

/* the first free cluster from the hint on (all clusters before it were in
 * use when it was set), then from the start: a file grows in one piece and
 * the FAT is not read from cluster 2 again for each cluster */
uint16_t fat_alloc(uint8_t drive_id) {
    fat_vol_t *v = vol(drive_id);
    uint32_t end = v->cluster_count + 2;
    uint32_t start = drive_id < DRIVE_COUNT ? s_next_free[drive_id] : 2;
    if (start < 2 || start >= end)
        start = 2;
    for (uint32_t k = 0; k < end - 2; k++) {
        uint32_t c = start + k;
        if (c >= end)
            c -= end - 2;
        if (fat_get(drive_id, (uint16_t)c) == FAT16_FREE) {
            if (!fat_set(drive_id, (uint16_t)c, FAT16_EOC))
                return 0;
            if (drive_id < DRIVE_COUNT)
                s_next_free[drive_id] = (uint16_t)(c + 1);
            return (uint16_t)c;
        }
    }
    return 0;
}

/* (the FAT's sectors written once each, at the end: a batch, unless one
 * is open already) */
void fat_free_chain(uint8_t drive_id, uint16_t cluster) {
    fat_vol_t *v = vol(drive_id);
    bool mine = s_batch < 0;
    if (mine)
        fat_batch_begin(drive_id);
    while (vol_cluster_active(v, cluster)) {
        uint16_t next = fat_get(drive_id, cluster);
        fat_set(drive_id, cluster, FAT16_FREE);
        if (drive_id < DRIVE_COUNT && cluster < s_next_free[drive_id])
            s_next_free[drive_id] = cluster; /* free again: look here first */
        cluster = next;
    }
    if (mine)
        fat_batch_end();
}
