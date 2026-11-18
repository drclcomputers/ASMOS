#include "fs/iso9660.h"
#include "drivers/cdrom.h"
#include "lib/memory.h"
#include "lib/string.h"

/* a few sectors kept: directories are read again and again */
#define CACHE 4
static struct {
    uint32_t lba;
    bool valid;
    uint8_t data[CD_SECTOR];
} s_cache[CACHE];
static int s_next_slot;

static const uint8_t *sector(uint32_t lba) {
    for (int i = 0; i < CACHE; i++)
        if (s_cache[i].valid && s_cache[i].lba == lba)
            return s_cache[i].data;
    int k = s_next_slot;
    s_next_slot = (s_next_slot + 1) % CACHE;
    s_cache[k].valid = false;
    if (!cd_read(lba, 1, s_cache[k].data))
        return NULL;
    s_cache[k].lba = lba;
    s_cache[k].valid = true;
    return s_cache[k].data;
}

/* folders seen: their number is the index + 2 (0 = the root) */
#define DIRS 512
static struct {
    uint32_t lba, size;
} s_dirs[DIRS];
static int s_ndirs;
static uint32_t s_root_lba, s_root_size, s_total;
static bool s_joliet;

static uint32_t le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static uint16_t dir_id(uint32_t lba, uint32_t size) {
    if (lba == s_root_lba)
        return 0;
    for (int i = 0; i < s_ndirs; i++)
        if (s_dirs[i].lba == lba)
            return (uint16_t)(i + 2);
    int i = s_ndirs < DIRS ? s_ndirs++ : DIRS - 1; /* full: the last slot */
    s_dirs[i].lba = lba;
    s_dirs[i].size = size;
    return (uint16_t)(i + 2);
}

static bool dir_extent(uint16_t id, uint32_t *lba, uint32_t *size) {
    if (id == 0) {
        *lba = s_root_lba;
        *size = s_root_size;
        return true;
    }
    if (id < 2 || id - 2 >= s_ndirs)
        return false;
    *lba = s_dirs[id - 2].lba;
    *size = s_dirs[id - 2].size;
    return true;
}

static void cache_drop(void) {
    for (int i = 0; i < CACHE; i++)
        s_cache[i].valid = false;
}

void iso_unmount(void) {
    cache_drop();
    s_ndirs = 0;
}

bool iso_mount(fat_vol_t *v) {
    iso_unmount();
    const uint8_t *root = NULL;
    char label[33] = "";
    s_joliet = false;
    /* the volume descriptors, from sector 16: primary (1), Joliet (2 with
     * an escape sequence %/@ %/C %/E), the end (255) */
    for (uint32_t lba = 16; lba < 64; lba++) {
        const uint8_t *d = sector(lba);
        if (!d || memcmp(d + 1, "CD001", 5) != 0)
            return false;
        if (d[0] == 255)
            break;
        if (d[0] == 1 && !root) {
            if ((d[128] | d[129] << 8) != CD_SECTOR)
                return false;
            s_total = le32(d + 80);
            s_root_lba = le32(d + 156 + 2);
            s_root_size = le32(d + 156 + 10);
            root = d;
            memcpy(label, d + 40, 32);
        } else if (d[0] == 2 && d[88] == 0x25 && d[89] == 0x2F &&
                   (d[90] == 0x40 || d[90] == 0x43 || d[90] == 0x45)) {
            s_joliet = true; /* its own tree, with the long names */
            s_root_lba = le32(d + 156 + 2);
            s_root_size = le32(d + 156 + 10);
            for (int i = 0; i < 16; i++) { /* the label, UCS-2 */
                uint8_t ch = d[40 + 2 * i + 1];
                label[i] = d[40 + 2 * i] ? '_' : (char)ch;
            }
            label[16] = 0;
        }
    }
    if (!root && !s_joliet)
        return false;
    cache_drop(); /* (the descriptor sectors are no use now) */
    int n = (int)strlen(label);
    while (n && (label[n - 1] == ' ' || !label[n - 1]))
        label[--n] = 0;
    memset(&v->bpb, 0, sizeof(v->bpb));
    v->bpb.bytes_per_sector = 512; /* (FAT-shaped callers: unused) */
    v->bpb.sectors_per_cluster = 4;
    snprintf(v->label, sizeof(v->label), "%s", n ? label : "CD");
    v->mounted = true;
    return true;
}

uint32_t iso_size_bytes(void) { return s_total * CD_SECTOR; }

/* a FAT date/time from the record's 7 bytes (years since 1900, ...) */
static void stamp(dir_entry_t *e, const uint8_t *t) {
    int y = t[0] + 1900;
    if (y < 1980)
        y = 1980;
    e->modify_date = e->create_date =
        (uint16_t)(((y - 1980) << 9) | (t[1] << 5) | t[2]);
    e->modify_time = e->create_time =
        (uint16_t)((t[3] << 11) | (t[4] << 5) | (t[5] / 2));
}

static bool ok83(const char *s) {
    if (!fs_name_is_83(s))
        return false;
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z')
            return false;
    return true;
}

/* the disc's name: Joliet (UCS-2, big end first) or plain; ";1" and a
 * last '.' off */
static void rec_name(const uint8_t *r, char *out) {
    int len = r[32], k = 0;
    const uint8_t *n = r + 33;
    if (s_joliet) {
        for (int i = 0; i + 1 < len && k < FS_NAME_MAX; i += 2) {
            uint16_t ch = (uint16_t)(n[i] << 8 | n[i + 1]);
            if (ch == ';')
                break;
            out[k++] = ch < 0x20 || ch > 0x7E || ch == '/' ? '_' : (char)ch;
        }
    } else {
        for (int i = 0; i < len && k < FS_NAME_MAX; i++) {
            if (n[i] == ';')
                break;
            out[k++] = (char)n[i];
        }
    }
    out[k] = 0;
    if (k > 1 && out[k - 1] == '.')
        out[k - 1] = 0;
}

/* NAME~N.EXT for a name that isn't 8.3 (n: its place in the folder) */
static void alias(const char *name, int n, uint8_t out11[11]) {
    memset(out11, ' ', 11);
    char num[8];
    int nl = snprintf(num, sizeof(num), "~%d", n);
    const char *dot = strrchr(name, '.');
    int k = 0;
    for (const char *p = name; *p && p != dot && k < 8 - nl; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 32);
        if (fs_name_char_ok(c) && c != '.' && c != ' ')
            out11[k++] = (uint8_t)c;
    }
    memcpy(out11 + k, num, (size_t)nl);
    if (dot)
        for (int i = 0, j = 8; dot[1 + i] && j < 11; i++) {
            char c = dot[1 + i];
            if (c >= 'a' && c <= 'z')
                c = (char)(c - 32);
            if (fs_name_char_ok(c) && c != '.' && c != ' ')
                out11[j++] = (uint8_t)c;
        }
}

bool iso_iterate(uint16_t dir, iso_cb cb, void *user) {
    uint32_t lba, size;
    if (!dir_extent(dir, &lba, &size))
        return false;
    int ord = 0;
    for (uint32_t off = 0; off < size;) {
        const uint8_t *s = sector(lba + off / CD_SECTOR);
        if (!s)
            return false;
        uint32_t in = off % CD_SECTOR;
        const uint8_t *r = s + in;
        if (r[0] == 0 || in + r[0] > CD_SECTOR) { /* the rest is padding */
            off = (off / CD_SECTOR + 1) * CD_SECTOR;
            continue;
        }
        off += r[0];
        if (r[25] & 0x80) /* a piece of a multi-extent file: skip */
            continue;
        dir_entry_t e;
        memset(&e, 0, sizeof(e));
        memset(e.name, ' ', 11);
        uint32_t ext = le32(r + 2), len = le32(r + 10);
        bool is_dir = (r[25] & 0x02) != 0;
        e.attr = (uint8_t)(ATTR_READ_ONLY | (is_dir ? ATTR_DIRECTORY : 0) |
                           (r[25] & 0x01 ? ATTR_HIDDEN : 0));
        stamp(&e, r + 18);
        char lname[FS_NAME_SZ];
        const char *ln = NULL;
        if (r[32] == 1 && (r[33] == 0 || r[33] == 1)) { /* "." and ".." */
            e.name[0] = '.';
            if (r[33] == 1)
                e.name[1] = '.';
            uint16_t id = r[33] == 0 ? dir : dir_id(ext, len);
            e.cluster_lo = id;
        } else {
            ord++;
            rec_name(r, lname);
            if (!lname[0])
                continue;
            if (ok83(lname)) {
                char n83[12];
                fs_make_83(lname, n83);
                memcpy(e.name, n83, 8);
                memcpy(e.ext, n83 + 8, 3);
            } else {
                uint8_t a11[11];
                alias(lname, ord, a11);
                memcpy(e.name, a11, 8);
                memcpy(e.ext, a11 + 8, 3);
                ln = lname;
            }
            if (s_joliet && !ln)
                ln = lname; /* (as the disc spells it) */
            if (is_dir) {
                e.cluster_lo = dir_id(ext, len);
            } else {
                e.cluster_lo = (uint16_t)ext;
                e.cluster_hi = (uint16_t)(ext >> 16);
                e.file_size = len;
            }
        }
        if (!cb(&e, ln, user))
            return true;
    }
    return true;
}

int iso_read(fat_file_t *f, void *buf, int len) {
    uint32_t ext = (uint32_t)f->entry.cluster_hi << 16 | f->entry.cluster_lo;
    uint8_t *out = buf;
    int total = 0;
    while (len > 0 && f->cur_offset < f->entry.file_size) {
        uint32_t off = f->cur_offset;
        uint32_t in = off % CD_SECTOR;
        int n = CD_SECTOR - (int)in;
        if (n > len)
            n = len;
        if ((uint32_t)n > f->entry.file_size - off)
            n = (int)(f->entry.file_size - off);
        if (in == 0 && n == CD_SECTOR) { /* whole sectors: straight in */
            uint32_t k = (uint32_t)len / CD_SECTOR,
                     left = (f->entry.file_size - off) / CD_SECTOR;
            if (k > left)
                k = left;
            if (k > 16)
                k = 16;
            if (k >= 2) {
                if (!cd_read(ext + off / CD_SECTOR, k, out))
                    break;
                n = (int)(k * CD_SECTOR);
                out += n;
                total += n;
                len -= n;
                f->cur_offset += (uint32_t)n;
                continue;
            }
        }
        const uint8_t *s = sector(ext + off / CD_SECTOR);
        if (!s)
            break;
        memcpy(out, s + in, (size_t)n);
        out += n;
        total += n;
        len -= n;
        f->cur_offset += (uint32_t)n;
    }
    return total;
}
