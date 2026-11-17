/*
 * ZIP archives (fs/zip.h): deflate to make them, inflate to read them.
 *
 * Deflate here: a 16 KB window (any decoder handles it: the format allows
 * up to 32 KB back), hash chains, lazy matching like zlib's, and for each
 * block the cheapest of stored / fixed / dynamic Huffman codes. A block
 * ends before the window slides, so its bytes are always there to be
 * stored as they are: a file that won't compress grows by only a few bytes
 * per 16 KB. Memory: about 100 KB while compressing, 34 KB extracting.
 */
#include "fs/zip.h"
#include "fs/fs.h"
#include "lib/alloc.h"
#include "lib/memory.h"
#include "lib/string.h"

/* ── CRC-32 ─────────────────────────────────────────────────────────── */

static uint32_t s_crc_tab[256];

uint32_t crc32_update(uint32_t crc, const void *p, uint32_t n) {
    if (!s_crc_tab[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            s_crc_tab[i] = c;
        }
    const uint8_t *b = (const uint8_t *)p;
    crc = ~crc;
    while (n--)
        crc = s_crc_tab[(crc ^ *b++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ── little helpers ─────────────────────────────────────────────────── */

static void put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}
static uint32_t get16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) {
    return get16(p) | get16(p + 2) << 16;
}

static void zpath_join(char *out, int sz, const char *dir, const char *name) {
    int n = (int)strlen(dir);
    if (n && dir[n - 1] == '/')
        snprintf(out, sz, "%s%s", dir, name);
    else
        snprintf(out, sz, "%s/%s", dir, name);
}

static const uint16_t LEN_BASE[29] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                                      1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                      4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t DIST_BASE[30] = {
    1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
    33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t DIST_EXTRA[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                       4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                       9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
/* order the code-length code lengths are sent in */
static const uint8_t CL_ORDER[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                     11, 4,  12, 3, 13, 2, 14, 1, 15};

/* ══ output: a buffered writer for the zip being made ══════════════════ */

typedef struct {
    fat_file_t f;
    uint8_t buf[2048];
    int n;
    uint32_t pos; /* bytes written so far, buffered ones included */
    bool err;
} zout_t;

static void zo_flush(zout_t *o) {
    if (o->n && !o->err && fs_write(&o->f, o->buf, o->n) != o->n)
        o->err = true;
    o->n = 0;
}

static void zo_put(zout_t *o, const void *p, uint32_t n) {
    const uint8_t *b = (const uint8_t *)p;
    o->pos += n;
    while (n) {
        uint32_t k = sizeof(o->buf) - (uint32_t)o->n;
        if (k > n)
            k = n;
        memcpy(o->buf + o->n, b, k);
        o->n += (int)k;
        b += k;
        n -= k;
        if (o->n == (int)sizeof(o->buf))
            zo_flush(o);
    }
}

static void zo_byte(zout_t *o, uint8_t v) {
    o->buf[o->n++] = v;
    o->pos++;
    if (o->n == (int)sizeof(o->buf))
        zo_flush(o);
}

/* ══ deflate ═════════════════════════════════════════════════════════════ */

#define WBITS 14
#define WSIZE (1u << WBITS)
#define WMASK (WSIZE - 1)
#define HBITS 13
#define HSIZE (1u << HBITS)
#define HMASK (HSIZE - 1)
#define MIN_MATCH 3
#define MAX_MATCH 258
#define MIN_LOOKAHEAD (MAX_MATCH + MIN_MATCH + 1)
#define MAX_DIST (WSIZE - MIN_LOOKAHEAD)
#define NSYMS 8192

typedef struct {
    uint8_t win[2 * WSIZE];
    uint16_t prev[WSIZE];
    uint16_t head[HSIZE];
    uint8_t sym_l[NSYMS];  /* literal, or match length - 3 */
    uint16_t sym_d[NSYMS]; /* 0 for a literal, else the distance */
    int nsym;
    uint32_t strstart, lookahead, match_start;
    int block_start;
    bool eof;
    /* what to read */
    fat_file_t *in;
    uint32_t crc, read, total_read_goal;
    /* settings */
    int chain, lazy, nice;
    /* bits out */
    zout_t *out;
    uint32_t bitbuf;
    int bitcnt;
    /* trees for the block */
    uint16_t lfreq[286], dfreq[30], cfreq[19];
    uint8_t llen[286], dlen[30], clen[19];
    uint16_t lcode[286], dcode[30], ccode[19];
    uint8_t len_code[256];  /* length - 3 -> 0..28 */
    uint8_t dist_code[512]; /* see d_code() */
    /* progress */
    zip_progress_fn cb;
    void *user;
    const char *name;
    uint32_t done_before, total_all;
} dfl_t;

static void put_bits(dfl_t *s, uint32_t v, int n) {
    s->bitbuf |= v << s->bitcnt;
    s->bitcnt += n;
    while (s->bitcnt >= 8) {
        zo_byte(s->out, (uint8_t)s->bitbuf);
        s->bitbuf >>= 8;
        s->bitcnt -= 8;
    }
}

static void align_bits(dfl_t *s) {
    if (s->bitcnt > 0)
        zo_byte(s->out, (uint8_t)s->bitbuf);
    s->bitbuf = 0;
    s->bitcnt = 0;
}

static int d_code(const dfl_t *s, uint32_t dist) { /* dist >= 1 */
    uint32_t d = dist - 1;
    return d < 256 ? s->dist_code[d] : s->dist_code[256 + (d >> 7)];
}

static void dfl_tables(dfl_t *s) {
    for (int c = 0; c < 29; c++) {
        int n = 1 << LEN_EXTRA[c];
        for (int k = 0; k < n && LEN_BASE[c] - 3 + k < 256; k++)
            s->len_code[LEN_BASE[c] - 3 + k] = (uint8_t)c;
    }
    s->len_code[255] = 28; /* 258 */
    for (int c = 0; c < 30; c++) {
        uint32_t lo = DIST_BASE[c] - 1, hi = lo + (1u << DIST_EXTRA[c]);
        for (uint32_t d = lo; d < hi; d++) {
            if (d < 256)
                s->dist_code[d] = (uint8_t)c;
            else
                s->dist_code[256 + (d >> 7)] = (uint8_t)c;
        }
    }
}

/* Huffman code lengths for freq[0..n), none longer than limit. Plain
 * Huffman; if too deep, halve the counts and try again. */
static void build_lengths(const uint16_t *freq_in, int n, int limit,
                          uint8_t *len) {
    static uint32_t freq[286];
    static int parent[2 * 286], node_w[2 * 286];
    static bool alive[2 * 286];
    int used = 0;
    for (int i = 0; i < n; i++) {
        freq[i] = freq_in[i];
        len[i] = 0;
        if (freq[i])
            used++;
    }
    /* at least two codes, so every decoder takes the tree */
    for (int i = 0; used < 2 && i < n; i++)
        if (!freq[i]) {
            freq[i] = 1;
            used++;
        }
    for (;;) {
        int nodes = 0;
        static int leaf_of[2 * 286];
        for (int i = 0; i < n; i++)
            if (freq[i]) {
                node_w[nodes] = (int)freq[i];
                leaf_of[nodes] = i;
                parent[nodes] = -1;
                alive[nodes] = true;
                nodes++;
            }
        int leaves = nodes, left = nodes;
        while (left > 1) {
            int a = -1, b = -1;
            for (int i = 0; i < nodes; i++) {
                if (!alive[i])
                    continue;
                if (a < 0 || node_w[i] < node_w[a]) {
                    b = a;
                    a = i;
                } else if (b < 0 || node_w[i] < node_w[b]) {
                    b = i;
                }
            }
            alive[a] = alive[b] = false;
            node_w[nodes] = node_w[a] + node_w[b];
            parent[nodes] = -1;
            alive[nodes] = true;
            parent[a] = parent[b] = nodes;
            nodes++;
            left--;
        }
        int maxd = 0;
        for (int i = 0; i < leaves; i++) {
            int d = 0;
            for (int p = i; parent[p] >= 0; p = parent[p])
                d++;
            len[leaf_of[i]] = (uint8_t)d;
            if (d > maxd)
                maxd = d;
        }
        if (maxd <= limit)
            return;
        for (int i = 0; i < n; i++)
            if (freq[i])
                freq[i] = (freq[i] + 1) / 2;
    }
}

/* canonical codes, bit-reversed (deflate sends them MSB first) */
static void make_codes(const uint8_t *len, int n, uint16_t *code) {
    uint16_t count[16] = {0}, next[16];
    for (int i = 0; i < n; i++)
        count[len[i]]++;
    count[0] = 0;
    uint16_t c = 0;
    for (int b = 1; b < 16; b++) {
        c = (uint16_t)((c + count[b - 1]) << 1);
        next[b] = c;
    }
    for (int i = 0; i < n; i++) {
        int l = len[i];
        if (!l)
            continue;
        uint16_t v = next[l]++, r = 0;
        for (int k = 0; k < l; k++)
            r = (uint16_t)(r << 1 | ((v >> k) & 1));
        code[i] = r;
    }
}

static void fixed_trees(uint8_t *ll, uint8_t *dl) {
    /* all 288: 286 and 287 are never sent, but they shift the 9-bit codes */
    for (int i = 0; i < 288; i++)
        ll[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    for (int i = 0; i < 30; i++)
        dl[i] = 5;
}

/* the run-length form of the code lengths; calls emit(sym, extra) or
 * counts when emit is NULL */
static int rle_lengths(dfl_t *s, const uint8_t *lens, int n, bool send) {
    int bits = 0;
    for (int i = 0; i < n;) {
        int l = lens[i], run = 1;
        while (i + run < n && lens[i + run] == l)
            run++;
        i += run;
        while (run > 0) {
            int sym, extra = 0, eb = 0, take;
            if (l == 0 && run >= 11) {
                take = run > 138 ? 138 : run;
                sym = 18, extra = take - 11, eb = 7;
            } else if (l == 0 && run >= 3) {
                take = run;
                sym = 17, extra = take - 3, eb = 3;
            } else if (l != 0 && run >= 4) {
                /* the length once, then repeats of it */
                if (send)
                    put_bits(s, s->ccode[l], s->clen[l]);
                else
                    s->cfreq[l]++;
                bits += s->clen[l];
                run--;
                take = run > 6 ? 6 : run;
                if (take < 3) {
                    continue; /* the rest go one by one */
                }
                sym = 16, extra = take - 3, eb = 2;
            } else {
                take = 1;
                sym = l;
            }
            if (send) {
                put_bits(s, s->ccode[sym], s->clen[sym]);
                if (eb)
                    put_bits(s, (uint32_t)extra, eb);
            } else {
                s->cfreq[sym]++;
            }
            bits += s->clen[sym] + eb;
            run -= take;
        }
    }
    return bits;
}

static uint32_t data_bits(dfl_t *s, const uint8_t *ll, const uint8_t *dl) {
    uint32_t b = ll[256];
    for (int i = 0; i < s->nsym; i++) {
        if (!s->sym_d[i]) {
            b += ll[s->sym_l[i]];
        } else {
            int lc = s->len_code[s->sym_l[i]], dc = d_code(s, s->sym_d[i]);
            b += ll[257 + lc] + LEN_EXTRA[lc] + dl[dc] + DIST_EXTRA[dc];
        }
    }
    return b;
}

static void send_data(dfl_t *s, const uint8_t *ll, const uint16_t *lc_,
                      const uint8_t *dl, const uint16_t *dc_) {
    for (int i = 0; i < s->nsym; i++) {
        if (!s->sym_d[i]) {
            put_bits(s, lc_[s->sym_l[i]], ll[s->sym_l[i]]);
        } else {
            int len3 = s->sym_l[i], lc = s->len_code[len3];
            put_bits(s, lc_[257 + lc], ll[257 + lc]);
            if (LEN_EXTRA[lc])
                put_bits(s, (uint32_t)(len3 + 3 - LEN_BASE[lc]), LEN_EXTRA[lc]);
            uint32_t d = s->sym_d[i];
            int dc = d_code(s, d);
            put_bits(s, dc_[dc], dl[dc]);
            if (DIST_EXTRA[dc])
                put_bits(s, d - DIST_BASE[dc], DIST_EXTRA[dc]);
        }
    }
    put_bits(s, lc_[256], ll[256]);
}

/* Send the symbols gathered since block_start, ending at byte `end` of
 * the window, in whichever form is smallest. */
static void flush_block(dfl_t *s, uint32_t end, bool last) {
    uint32_t raw = end - (uint32_t)s->block_start;
    memset(s->lfreq, 0, sizeof(s->lfreq));
    memset(s->dfreq, 0, sizeof(s->dfreq));
    for (int i = 0; i < s->nsym; i++) {
        if (!s->sym_d[i]) {
            s->lfreq[s->sym_l[i]]++;
        } else {
            s->lfreq[257 + s->len_code[s->sym_l[i]]]++;
            s->dfreq[d_code(s, s->sym_d[i])]++;
        }
    }
    s->lfreq[256] = 1;

    /* dynamic */
    build_lengths(s->lfreq, 286, 15, s->llen);
    build_lengths(s->dfreq, 30, 15, s->dlen);
    int hlit = 286, hdist = 30;
    while (hlit > 257 && !s->llen[hlit - 1])
        hlit--;
    while (hdist > 1 && !s->dlen[hdist - 1])
        hdist--;
    static uint8_t all[286 + 30];
    memcpy(all, s->llen, (uint32_t)hlit);
    memcpy(all + hlit, s->dlen, (uint32_t)hdist);
    memset(s->cfreq, 0, sizeof(s->cfreq));
    memset(s->clen, 0, sizeof(s->clen));
    rle_lengths(s, all, hlit + hdist, false); /* counts */
    build_lengths(s->cfreq, 19, 7, s->clen);
    int hclen = 19;
    while (hclen > 4 && !s->clen[CL_ORDER[hclen - 1]])
        hclen--;
    uint32_t dyn = 3 + 5 + 5 + 4 + 3 * (uint32_t)hclen +
                   (uint32_t)rle_lengths(s, all, hlit + hdist, false) +
                   data_bits(s, s->llen, s->dlen);
    /* (the second count pass only added to cfreq; harmless now) */

    static uint8_t fll[288], fdl[30];
    fixed_trees(fll, fdl);
    uint32_t fix = 3 + data_bits(s, fll, fdl);
    uint32_t sto = 3 + 7 + 32 + raw * 8;

    if (sto <= fix && sto <= dyn) {
        put_bits(s, last ? 1 : 0, 1);
        put_bits(s, 0, 2);
        align_bits(s);
        uint8_t h[4];
        put16(h, raw);
        put16(h + 2, ~raw & 0xFFFF);
        zo_put(s->out, h, 4);
        zo_put(s->out, s->win + s->block_start, raw);
    } else if (fix <= dyn) {
        static uint16_t flc[288], fdc[30];
        make_codes(fll, 288, flc);
        make_codes(fdl, 30, fdc);
        put_bits(s, last ? 1 : 0, 1);
        put_bits(s, 1, 2);
        send_data(s, fll, flc, fdl, fdc);
    } else {
        make_codes(s->llen, 286, s->lcode);
        make_codes(s->dlen, 30, s->dcode);
        make_codes(s->clen, 19, s->ccode);
        put_bits(s, last ? 1 : 0, 1);
        put_bits(s, 2, 2);
        put_bits(s, (uint32_t)(hlit - 257), 5);
        put_bits(s, (uint32_t)(hdist - 1), 5);
        put_bits(s, (uint32_t)(hclen - 4), 4);
        for (int i = 0; i < hclen; i++)
            put_bits(s, s->clen[CL_ORDER[i]], 3);
        rle_lengths(s, all, hlit + hdist, true);
        send_data(s, s->llen, s->lcode, s->dlen, s->dcode);
    }
    if (last)
        align_bits(s);
    s->nsym = 0;
    s->block_start = (int)end;
}

static uint32_t insert(dfl_t *s, uint32_t p) {
    uint32_t h = ((uint32_t)s->win[p] << 10 ^ (uint32_t)s->win[p + 1] << 5 ^
                  s->win[p + 2]) &
                 HMASK;
    uint32_t old = s->head[h];
    s->prev[p & WMASK] = (uint16_t)old;
    s->head[h] = (uint16_t)p;
    return old;
}

/* fill the window; slides it (after ending the block) when near the top */
static void fill(dfl_t *s, uint32_t pending) {
    if (s->strstart >= WSIZE + MAX_DIST) {
        flush_block(s, s->strstart - pending, false);
        memcpy(s->win, s->win + WSIZE, WSIZE);
        s->strstart -= WSIZE;
        s->match_start -= WSIZE;
        s->block_start -= (int)WSIZE;
        for (uint32_t i = 0; i < HSIZE; i++)
            s->head[i] = s->head[i] >= WSIZE ? s->head[i] - WSIZE : 0;
        for (uint32_t i = 0; i < WSIZE; i++)
            s->prev[i] = s->prev[i] >= WSIZE ? s->prev[i] - WSIZE : 0;
    }
    while (!s->eof && s->lookahead < MIN_LOOKAHEAD) {
        uint32_t at = s->strstart + s->lookahead;
        uint32_t room = 2 * WSIZE - at;
        if (room > 4096)
            room = 4096;
        if (room == 0)
            break;
        int n = fs_read(s->in, s->win + at, (int)room);
        if (n <= 0) {
            s->eof = true;
            break;
        }
        s->crc = crc32_update(s->crc, s->win + at, (uint32_t)n);
        s->read += (uint32_t)n;
        s->lookahead += (uint32_t)n;
        if (s->cb && (s->read & 0x3FFF) < (uint32_t)n)
            s->cb(s->user, s->name, s->done_before + s->read, s->total_all);
    }
}

static uint32_t longest_match(dfl_t *s, uint32_t cur, uint32_t prev_len) {
    uint32_t chain = (uint32_t)s->chain;
    uint32_t limit = s->strstart > MAX_DIST ? s->strstart - MAX_DIST : 0;
    uint32_t best = prev_len;
    uint32_t maxlen = s->lookahead < MAX_MATCH ? s->lookahead : MAX_MATCH;
    if (best >= maxlen)
        return best;
    const uint8_t *scan = s->win + s->strstart;
    do {
        const uint8_t *m = s->win + cur;
        if (m[best] != scan[best] || m[0] != scan[0] || m[1] != scan[1])
            continue;
        uint32_t len = 2;
        while (len < maxlen && m[len] == scan[len])
            len++;
        if (len > best) {
            best = len;
            s->match_start = cur;
            if (len >= (uint32_t)s->nice || len >= maxlen)
                break;
        }
    } while ((cur = s->prev[cur & WMASK]) > limit && --chain);
    return best;
}

static void tally(dfl_t *s, uint32_t dist, uint32_t lit_or_len3) {
    s->sym_l[s->nsym] = (uint8_t)lit_or_len3;
    s->sym_d[s->nsym] = (uint16_t)dist;
    s->nsym++;
}

/* compress the file open in s->in to s->out (one deflate stream) */
static void deflate_file(dfl_t *s) {
    memset(s->head, 0, sizeof(s->head));
    memset(s->prev, 0, sizeof(s->prev));
    s->strstart = 0;
    s->lookahead = 0;
    s->block_start = 0;
    s->match_start = 0;
    s->nsym = 0;
    s->eof = false;
    s->bitbuf = 0;
    s->bitcnt = 0;
    uint32_t match_len = MIN_MATCH - 1, prev_len, prev_match;
    bool avail = false;
    for (;;) {
        if (s->lookahead < MIN_LOOKAHEAD) {
            fill(s, avail ? 1 : 0);
            if (s->lookahead == 0)
                break;
        }
        uint32_t hh = 0;
        if (s->lookahead >= MIN_MATCH)
            hh = insert(s, s->strstart);
        prev_len = match_len;
        prev_match = s->match_start;
        match_len = MIN_MATCH - 1;
        if (hh && prev_len < (uint32_t)s->lazy &&
            s->strstart - hh <= MAX_DIST) {
            match_len = longest_match(s, hh, prev_len);
            if (match_len < MIN_MATCH ||
                (match_len == MIN_MATCH &&
                 s->strstart - s->match_start > 4096)) {
                match_len = MIN_MATCH - 1;
            }
        }
        if (prev_len >= MIN_MATCH && match_len <= prev_len) {
            uint32_t max_insert = s->strstart + s->lookahead - MIN_MATCH;
            tally(s, s->strstart - 1 - prev_match, prev_len - MIN_MATCH);
            s->lookahead -= prev_len - 1;
            prev_len -= 2;
            do {
                if (++s->strstart <= max_insert)
                    insert(s, s->strstart);
            } while (--prev_len);
            avail = false;
            match_len = MIN_MATCH - 1;
            s->strstart++;
            if (s->nsym == NSYMS)
                flush_block(s, s->strstart, false);
        } else if (avail) {
            tally(s, 0, s->win[s->strstart - 1]);
            if (s->nsym == NSYMS)
                flush_block(s, s->strstart, false);
            s->strstart++;
            s->lookahead--;
        } else {
            avail = true;
            s->strstart++;
            s->lookahead--;
        }
    }
    if (avail)
        tally(s, 0, s->win[s->strstart - 1]);
    flush_block(s, s->strstart, true);
}

/* ══ making a zip ════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t usize, csize, crc, off;
    uint16_t time, date, len;
    uint8_t dir, method;
    /* name follows, NUL-terminated */
} zent_t;

#define POOL_SZ (48 * 1024)

typedef struct {
    uint8_t *pool;
    uint32_t used;
    int count;
    bool full;
    const char *base;     /* the folder holding src */
    const char *skip;     /* the zip itself */
    char rel[FS_PATH_SZ]; /* folder being listed, relative, ends in '/' */
    bool hidden;
    uint32_t total;
} zlist_t;

static zent_t *add_ent(zlist_t *L, const char *rel, bool dir, uint32_t size,
                       uint16_t t, uint16_t d) {
    uint32_t n = (uint32_t)strlen(rel);
    uint32_t need = (sizeof(zent_t) + n + 1 + 3) & ~3u;
    if (L->used + need > POOL_SZ || L->count >= 65000 || n > 0xFFFF) {
        L->full = true;
        return NULL;
    }
    zent_t *e = (zent_t *)(L->pool + L->used);
    memset(e, 0, sizeof(*e));
    e->dir = dir;
    e->usize = size;
    e->time = t;
    e->date = d;
    e->len = (uint16_t)n;
    memcpy(e + 1, rel, n + 1);
    L->used += need;
    L->count++;
    if (!dir)
        L->total += size;
    return e;
}

static zent_t *next_ent(zent_t *e) {
    uint32_t need = (sizeof(zent_t) + e->len + 1 + 3) & ~3u;
    return (zent_t *)((uint8_t *)e + need);
}

static bool same_path(const char *a, const char *b) {
    /* "/HDA/X" and "/X" are the same place */
    if (!strncasecmp(a, "/HDA/", 5))
        a += 4;
    if (!strncasecmp(b, "/HDA/", 5))
        b += 4;
    return !strcasecmp(a, b);
}

static bool list_cb(const dir_entry_t *e, const char *name, void *user) {
    zlist_t *L = (zlist_t *)user;
    if (e->attr & ATTR_VOLUME_ID)
        return true;
    if (!L->hidden && (e->attr & ATTR_HIDDEN))
        return true;
    bool dir = (e->attr & ATTR_DIRECTORY) != 0;
    char rel[FS_PATH_SZ];
    snprintf(rel, sizeof(rel), "%s%s%s", L->rel, name, dir ? "/" : "");
    char full[FS_PATH_SZ];
    zpath_join(full, sizeof(full), L->base, rel);
    int fl = (int)strlen(full);
    if (dir && fl)
        full[fl - 1] = 0;
    if (L->skip && same_path(full, L->skip))
        return true;
    return add_ent(L, rel, dir, dir ? 0 : e->file_size, e->modify_time,
                   e->modify_date) != NULL;
}

static const char *write_entry(zout_t *o, dfl_t *s, zent_t *e, const char *full,
                               int level, uint32_t *done, uint32_t total,
                               zip_progress_fn cb, void *user) {
    const char *name = (const char *)(e + 1);
    e->off = o->pos;
    e->method = (e->dir || level == ZIP_STORE || !s || !e->usize) ? 0 : 8;
    uint8_t h[30];
    memset(h, 0, sizeof(h));
    put32(h, 0x04034b50);
    put16(h + 4, e->method ? 20 : 10);
    put16(h + 8, e->method);
    put16(h + 10, e->time);
    put16(h + 12, e->date);
    put16(h + 26, e->len);
    zo_put(o, h, 30);
    zo_put(o, name, e->len);
    if (e->dir)
        return NULL;
    fat_file_t f;
    if (!fs_open(full, &f))
        return "a file could not be opened";
    uint32_t start = o->pos;
    if (e->method == 8) {
        s->in = &f;
        s->out = o;
        s->crc = 0;
        s->read = 0;
        s->name = name;
        s->done_before = *done;
        s->total_all = total;
        deflate_file(s);
        e->crc = s->crc;
        e->usize = s->read;
    } else {
        static uint8_t buf[2048];
        uint32_t crc = 0, got = 0;
        int n;
        while ((n = fs_read(&f, buf, sizeof(buf))) > 0) {
            crc = crc32_update(crc, buf, (uint32_t)n);
            zo_put(o, buf, (uint32_t)n);
            got += (uint32_t)n;
            if (cb && (got & 0x3FFF) < (uint32_t)n)
                cb(user, name, *done + got, total);
        }
        e->crc = crc;
        e->usize = got;
    }
    fs_close(&f);
    e->csize = o->pos - start;
    *done += e->usize;
    /* sizes and CRC into the local header */
    zo_flush(o);
    if (o->err)
        return "could not write the archive (disk full?)";
    uint8_t p[12];
    put32(p, e->crc);
    put32(p + 4, e->csize);
    put32(p + 8, e->usize);
    if (!fs_seek(&o->f, e->off + 14) || fs_write(&o->f, p, 12) != 12 ||
        !fs_seek(&o->f, o->pos))
        return "could not write the archive";
    return NULL;
}

/* the entries, then the central directory and the end record */
static const char *write_archive(zout_t *o, dfl_t *s, zlist_t *L,
                                 const char *base, int level,
                                 zip_progress_fn cb, void *user,
                                 zip_stats_t *st) {
    const char *err = NULL;
    zent_t *e;
    uint32_t done = 0;
    e = (zent_t *)L->pool;
    for (int i = 0; i < L->count && !err; i++, e = next_ent(e)) {
        char full[FS_PATH_SZ];
        zpath_join(full, sizeof(full), base, (const char *)(e + 1));
        if (cb)
            cb(user, (const char *)(e + 1), done, L->total);
        err = write_entry(o, s, e, full, level, &done, L->total, cb, user);
        if (e->dir)
            st->dirs++;
        else
            st->files++;
        st->in_bytes += e->usize;
    }
    if (!err) {
        uint32_t cd = o->pos;
        e = (zent_t *)L->pool;
        for (int i = 0; i < L->count; i++, e = next_ent(e)) {
            uint8_t h[46];
            memset(h, 0, sizeof(h));
            put32(h, 0x02014b50);
            put16(h + 4, 20);
            put16(h + 6, e->method ? 20 : 10);
            put16(h + 10, e->method);
            put16(h + 12, e->time);
            put16(h + 14, e->date);
            put32(h + 16, e->crc);
            put32(h + 20, e->csize);
            put32(h + 24, e->usize);
            put16(h + 28, e->len);
            put32(h + 38, e->dir ? ATTR_DIRECTORY : ATTR_ARCHIVE);
            put32(h + 42, e->off);
            zo_put(o, h, 46);
            zo_put(o, e + 1, e->len);
        }
        uint8_t z[22];
        memset(z, 0, sizeof(z));
        put32(z, 0x06054b50);
        put16(z + 8, (uint32_t)L->count);
        put16(z + 10, (uint32_t)L->count);
        put32(z + 12, o->pos - cd);
        put32(z + 16, cd);
        zo_put(o, z, 22);
        zo_flush(o);
        if (o->err)
            err = "could not write the archive (disk full?)";
    }
    return err;
}

const char *zip_create(const char *src, const char *zip_path, int level,
                       bool hidden, zip_progress_fn cb, void *user,
                       zip_stats_t *st) {
    static zip_stats_t dummy;
    if (!st)
        st = &dummy;
    memset(st, 0, sizeof(*st));
    dir_entry_t de;
    if (!fs_find(src, &de))
        return "the item to compress was not found";
    if (fs_find(zip_path, &de))
        return "an item with that name already exists";
    char base[FS_PATH_SZ], top[FS_NAME_SZ];
    snprintf(base, sizeof(base), "%s", src);
    char *sl = strrchr(base, '/');
    if (!sl)
        return "bad path";
    if (sl == base)
        strcpy(base, "/");
    else
        *sl = 0;
    if (!fs_name_of(src, top))
        snprintf(top, sizeof(top), "%s", strrchr(src, '/') + 1);
    fs_find(src, &de);

    zlist_t L;
    memset(&L, 0, sizeof(L));
    L.pool = (uint8_t *)kmalloc(POOL_SZ);
    if (!L.pool)
        return "not enough memory";
    L.base = base;
    L.skip = zip_path;
    L.hidden = hidden;
    bool is_dir = (de.attr & ATTR_DIRECTORY) != 0;
    char rel[FS_PATH_SZ];
    snprintf(rel, sizeof(rel), "%s%s", top, is_dir ? "/" : "");
    add_ent(&L, rel, is_dir, is_dir ? 0 : de.file_size, de.modify_time,
            de.modify_date);
    /* folders: list each one found, breadth first */
    zent_t *e = (zent_t *)L.pool;
    for (int i = 0; i < L.count && !L.full; i++, e = next_ent(e)) {
        if (!e->dir)
            continue;
        snprintf(L.rel, sizeof(L.rel), "%s", (const char *)(e + 1));
        char full[FS_PATH_SZ];
        zpath_join(full, sizeof(full), base, L.rel);
        full[strlen(full) - 1] = 0;
        uint8_t drv;
        uint16_t cl;
        if (!fs_resolve_dir(full, &drv, &cl) ||
            !fs_walk_dir(drv, cl, list_cb, &L)) {
            if (!L.full) {
                kfree(L.pool);
                return "a folder could not be read";
            }
        }
    }
    if (L.full) {
        kfree(L.pool);
        return "too many files for one archive here";
    }

    zout_t *o = (zout_t *)kzalloc(sizeof(zout_t));
    dfl_t *s = level == ZIP_STORE ? NULL : (dfl_t *)kmalloc(sizeof(dfl_t));
    if (!o) {
        kfree(L.pool);
        kfree(s);
        return "not enough memory";
    }
    if (level != ZIP_STORE && !s)
        st->stored_only = true;
    if (s) {
        dfl_tables(s);
        s->cb = cb;
        s->user = user;
        s->chain = level == ZIP_FAST ? 16 : level == ZIP_NORMAL ? 128 : 1024;
        s->lazy = level == ZIP_FAST ? 4 : level == ZIP_NORMAL ? 16 : 258;
        s->nice = level == ZIP_FAST ? 16 : level == ZIP_NORMAL ? 128 : 258;
    }
    const char *err = NULL;
    if (!fs_create(zip_path, &o->f)) {
        err = "could not create the archive (bad name, disk full or "
              "write-protected?)";
    } else {
        err = write_archive(o, s, &L, base, level, cb, user, st);
        st->out_bytes = o->pos;
        fs_close(&o->f);
        if (err)
            fs_delete(zip_path);
    }
    kfree(L.pool);
    kfree(s);
    kfree(o);
    return err;
}

/* ══ inflate ═════════════════════════════════════════════════════════════ */

#define FAST_BITS 9
typedef struct {
    short count[16];
    short symbol[288];
    /* the next 9 bits -> symbol | code length << 12 (0: a longer code) */
    uint16_t fast[1 << FAST_BITS];
} huff_t;

typedef struct {
    fat_file_t *in;
    uint8_t ibuf[2048];
    int ipos, ilen;
    uint32_t remain; /* compressed bytes not yet read into ibuf */
    uint32_t bitbuf;
    int bitcnt;
    bool err;
    bool werr; /* the file being written: disk full */
    uint8_t win[32768];
    uint32_t wpos, flushed; /* bytes out, bytes written to the file */
    fat_file_t *out;
    uint32_t crc;
    huff_t lc, dc;
    /* progress */
    zip_progress_fn cb;
    void *user;
    const char *name;
    uint32_t done_before, total;
} inf_t;

static int in_byte(inf_t *s) {
    if (s->ipos == s->ilen) {
        uint32_t k = s->remain < sizeof(s->ibuf) ? s->remain : sizeof(s->ibuf);
        int n = k ? fs_read(s->in, s->ibuf, (int)k) : 0;
        if (n <= 0) {
            s->err = true;
            return 0;
        }
        s->remain -= (uint32_t)n;
        s->ilen = n;
        s->ipos = 0;
    }
    return s->ibuf[s->ipos++];
}

/* the next byte of input, or -1 at the end (no error: a look ahead) */
static int peek_byte(inf_t *s) {
    if (s->ipos == s->ilen) {
        if (!s->remain)
            return -1;
        uint32_t k = s->remain < sizeof(s->ibuf) ? s->remain : sizeof(s->ibuf);
        int n = fs_read(s->in, s->ibuf, (int)k);
        if (n <= 0)
            return -1;
        s->remain -= (uint32_t)n;
        s->ilen = n;
        s->ipos = 0;
    }
    return s->ibuf[s->ipos++];
}

static uint32_t bits(inf_t *s, int n) {
    while (s->bitcnt < n) {
        s->bitbuf |= (uint32_t)in_byte(s) << s->bitcnt;
        s->bitcnt += 8;
        if (s->err)
            return 0;
    }
    uint32_t v = s->bitbuf & ((1u << n) - 1);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

static void out_flush(inf_t *s) {
    uint32_t n = s->wpos - s->flushed;
    if (!n)
        return;
    const uint8_t *p = s->win + (s->flushed & 32767);
    s->crc = crc32_update(s->crc, p, n);
    if (s->out && fs_write(s->out, p, (int)n) != (int)n)
        s->err = s->werr = true;
    s->flushed = s->wpos;
    if (s->cb)
        s->cb(s->user, s->name, s->done_before + s->wpos, s->total);
}

static void out_byte(inf_t *s, uint8_t v) {
    s->win[s->wpos & 32767] = v;
    s->wpos++;
    if (!(s->wpos & 32767))
        out_flush(s);
}

/* A symbol: one table look-up for codes up to 9 bits (nearly all), else
 * bit by bit, puff-style. */
static int decode(inf_t *s, const huff_t *h) {
    while (s->bitcnt < FAST_BITS) {
        int c = peek_byte(s);
        if (c < 0)
            break;
        s->bitbuf |= (uint32_t)c << s->bitcnt;
        s->bitcnt += 8;
    }
    uint16_t e = h->fast[s->bitbuf & ((1u << FAST_BITS) - 1)];
    int l = e >> 12;
    if (l && l <= s->bitcnt) {
        s->bitbuf >>= l;
        s->bitcnt -= l;
        return e & 0x0FFF;
    }
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= (int)bits(s, 1);
        if (s->err)
            return -1;
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = true;
    return -1;
}

static int construct(huff_t *h, const uint8_t *len, int n) {
    for (int i = 0; i < 16; i++)
        h->count[i] = 0;
    for (int i = 0; i < n; i++)
        h->count[len[i]]++;
    if (h->count[0] == n)
        return 0;
    int left = 1;
    for (int i = 1; i < 16; i++) {
        left <<= 1;
        left -= h->count[i];
        if (left < 0)
            return left;
    }
    short offs[16];
    offs[1] = 0;
    for (int i = 1; i < 15; i++)
        offs[i + 1] = (short)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++)
        if (len[i])
            h->symbol[offs[len[i]]++] = (short)i;
    /* the fast table: each short code (bit-reversed: deflate sends them
     * high bit first) fills every slot that starts with it */
    memset(h->fast, 0, sizeof(h->fast));
    int code = 0, idx = 0;
    for (int l = 1; l < 16; l++) {
        for (int k = 0; k < h->count[l]; k++, code++) {
            int sym = h->symbol[idx++];
            if (l > FAST_BITS)
                continue;
            int rev = 0;
            for (int b = 0; b < l; b++)
                rev |= ((code >> b) & 1) << (l - 1 - b);
            for (int f = rev; f < (1 << FAST_BITS); f += 1 << l)
                h->fast[f] = (uint16_t)(sym | l << 12);
        }
        code <<= 1;
    }
    return left;
}

static bool codes(inf_t *s) {
    for (;;) {
        int sym = decode(s, &s->lc);
        if (sym < 0)
            return false;
        if (sym < 256) {
            out_byte(s, (uint8_t)sym);
        } else if (sym == 256) {
            return !s->err;
        } else {
            sym -= 257;
            if (sym >= 29)
                return false;
            uint32_t len = LEN_BASE[sym] + bits(s, LEN_EXTRA[sym]);
            int ds = decode(s, &s->dc);
            if (ds < 0 || ds >= 30)
                return false;
            uint32_t dist = DIST_BASE[ds] + bits(s, DIST_EXTRA[ds]);
            if (s->err || dist > s->wpos || dist > 32768)
                return false;
            while (len--)
                out_byte(s, s->win[(s->wpos - dist) & 32767]);
        }
        if (s->err)
            return false;
    }
}

static bool inflate_stream(inf_t *s) {
    static uint8_t lens[320];
    int last;
    do {
        last = (int)bits(s, 1);
        int type = (int)bits(s, 2);
        if (s->err)
            return false;
        if (type == 0) {
            /* to the byte boundary; whole bytes the decoder looked ahead
             * at are still in bitbuf, and come out of it first */
            bits(s, s->bitcnt & 7);
            uint32_t len = bits(s, 16);
            uint32_t nlen = bits(s, 16);
            if (s->err || len != (~nlen & 0xFFFF))
                return false;
            while (len--) {
                out_byte(s, (uint8_t)(s->bitcnt ? bits(s, 8) : in_byte(s)));
                if (s->err)
                    return false;
            }
        } else if (type == 1) {
            for (int i = 0; i < 288; i++)
                lens[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
            construct(&s->lc, lens, 288);
            for (int i = 0; i < 30; i++)
                lens[i] = 5;
            construct(&s->dc, lens, 30);
            if (!codes(s))
                return false;
        } else if (type == 2) {
            int nlen = (int)bits(s, 5) + 257, ndist = (int)bits(s, 5) + 1,
                ncode = (int)bits(s, 4) + 4;
            if (s->err || nlen > 286 || ndist > 30)
                return false;
            for (int i = 0; i < 19; i++)
                lens[CL_ORDER[i]] = i < ncode ? (uint8_t)bits(s, 3) : 0;
            if (construct(&s->lc, lens, 19) != 0)
                return false;
            int i = 0;
            while (i < nlen + ndist) {
                int sym = decode(s, &s->lc);
                if (sym < 0)
                    return false;
                if (sym < 16) {
                    lens[i++] = (uint8_t)sym;
                } else {
                    uint8_t l = 0;
                    int rep;
                    if (sym == 16) {
                        if (!i)
                            return false;
                        l = lens[i - 1];
                        rep = 3 + (int)bits(s, 2);
                    } else if (sym == 17) {
                        rep = 3 + (int)bits(s, 3);
                    } else {
                        rep = 11 + (int)bits(s, 7);
                    }
                    if (i + rep > nlen + ndist)
                        return false;
                    while (rep--)
                        lens[i++] = l;
                }
            }
            if (lens[256] == 0)
                return false;
            int e1 = construct(&s->lc, lens, nlen);
            if (e1 < 0 || (e1 > 0 && nlen - s->lc.count[0] != 1))
                return false;
            int e2 = construct(&s->dc, lens + nlen, ndist);
            if (e2 < 0 || (e2 > 0 && ndist - s->dc.count[0] != 1))
                return false;
            if (!codes(s))
                return false;
        } else {
            return false;
        }
    } while (!last);
    out_flush(s);
    return !s->err;
}

/* ══ taking a zip apart ══════════════════════════════════════════════════ */

bool zip_is_zip(const char *path) {
    fat_file_t f;
    if (!fs_open(path, &f))
        return false;
    uint8_t b[4] = {0};
    int n = fs_read(&f, b, 4);
    fs_close(&f);
    return n == 4 && b[0] == 'P' && b[1] == 'K' &&
           ((b[2] == 3 && b[3] == 4) || (b[2] == 5 && b[3] == 6));
}

/* one part of a name in the zip, made safe for FAT */
static void safe_part(const char *p, int n, char *out) {
    int o = 0;
    for (int i = 0; i < n && o < FS_NAME_MAX; i++) {
        char c = p[i];
        out[o++] = ((unsigned char)c < 32 || (unsigned char)c > 126 ||
                    !fs_long_char_ok(c))
                       ? '_'
                       : c;
    }
    while (o && (out[o - 1] == ' ' || out[o - 1] == '.'))
        o--; /* FAT drops these */
    out[o] = 0;
    if (!o)
        strcpy(out, "_");
}

static bool make_dirs(const char *path) {
    /* every folder along path (which is a folder) */
    char p[FS_PATH_SZ];
    snprintf(p, sizeof(p), "%s", path);
    int n = (int)strlen(p);
    for (int i = 1; i <= n; i++) {
        if (p[i] != '/' && p[i] != 0)
            continue;
        char c = p[i];
        p[i] = 0;
        dir_entry_t de;
        if (fs_find(p, &de)) {
            if (!(de.attr & ATTR_DIRECTORY))
                return false;
        } else {
            uint8_t drv;
            uint16_t cl;
            if (!fs_resolve_dir(p, &drv, &cl) && !fs_mkdir(p))
                return false;
        }
        p[i] = c;
    }
    return true;
}

/* every entry of the central directory cd, into dst_dir */
static const char *extract_entries(fat_file_t *zf, const uint8_t *cd,
                                   uint32_t cd_size, uint32_t count, inf_t *s,
                                   const char *dst_dir, bool overwrite,
                                   zip_progress_fn cb, void *user,
                                   zip_stats_t *st) {
    const char *err = NULL;
    /* the total, for progress */
    uint32_t total = 0, done = 0;
    for (uint32_t p = 0, i = 0; i < count && p + 46 <= cd_size; i++) {
        total += get32(cd + p + 24);
        p += 46 + get16(cd + p + 28) + get16(cd + p + 30) + get16(cd + p + 32);
    }
    if (!make_dirs(dst_dir))
        return "could not make the folder to extract into";
    uint32_t p = 0;
    for (uint32_t i = 0; i < count && !err; i++) {
        if (p + 46 > cd_size || get32(cd + p) != 0x02014b50) {
            err = "the archive's directory is damaged";
            break;
        }
        const uint8_t *h = cd + p;
        uint32_t flags = get16(h + 8), method = get16(h + 10);
        uint32_t crc = get32(h + 16), csize = get32(h + 20),
                 usize = get32(h + 24);
        uint32_t nlen = get16(h + 28), xlen = get16(h + 30),
                 clen = get16(h + 32);
        uint32_t ext = get32(h + 38), loff = get32(h + 42);
        uint16_t mtime = (uint16_t)get16(h + 12),
                 mdate = (uint16_t)get16(h + 14);
        const char *name = (const char *)(h + 46);
        p += 46 + nlen + xlen + clen;
        if (p > cd_size) {
            err = "the archive's directory is damaged";
            break;
        }
        /* the path, made safe */
        char out[FS_PATH_SZ];
        snprintf(out, sizeof(out), "%s", dst_dir);
        int ol = (int)strlen(out);
        if (ol && out[ol - 1] == '/')
            out[--ol] = 0;
        bool is_dir = nlen && (name[nlen - 1] == '/' || name[nlen - 1] == '\\');
        if (ext & ATTR_DIRECTORY && !usize)
            is_dir = true;
        bool too_long = false, any = false;
        char shown[FS_NAME_SZ] = "";
        for (uint32_t a = 0; a < nlen;) {
            uint32_t b = a;
            while (b < nlen && name[b] != '/' && name[b] != '\\')
                b++;
            int n = (int)(b - a);
            bool skip = n == 0 || (n == 1 && name[a] == '.') ||
                        (n == 2 && name[a] == '.' && name[a + 1] == '.') ||
                        (n == 2 && name[a + 1] == ':');
            if (!skip) {
                char part[FS_NAME_SZ];
                safe_part(name + a, n, part);
                int pl = (int)strlen(part);
                if (ol + 1 + pl >= FS_PATH_MAX) {
                    too_long = true;
                    break;
                }
                out[ol++] = '/';
                memcpy(out + ol, part, (uint32_t)pl + 1);
                ol += pl;
                strcpy(shown, part);
                any = true;
            }
            a = b + 1;
        }
        if (too_long || !any) {
            st->unsupported++;
            continue;
        }
        if (is_dir) {
            if (make_dirs(out))
                st->dirs++;
            else
                st->unsupported++;
            continue;
        }
        if ((flags & 1) || (method != 0 && method != 8) ||
            csize == 0xFFFFFFFF || usize == 0xFFFFFFFF) {
            st->unsupported++;
            done += usize;
            continue;
        }
        /* its folder */
        char *sl = strrchr(out, '/');
        *sl = 0;
        bool dir_ok = make_dirs(out);
        *sl = '/';
        if (!dir_ok) {
            st->unsupported++;
            continue;
        }
        dir_entry_t de;
        if (fs_find(out, &de)) {
            if ((de.attr & ATTR_DIRECTORY) || !overwrite) {
                st->skipped++;
                done += usize;
                continue;
            }
            if (!fs_delete(out)) {
                st->skipped++;
                done += usize;
                continue;
            }
        }
        /* the data: after the local header */
        uint8_t lh[30];
        if (!fs_seek(zf, loff) || fs_read(zf, lh, 30) != 30 ||
            get32(lh) != 0x04034b50 ||
            !fs_seek(zf, loff + 30 + get16(lh + 26) + get16(lh + 28))) {
            st->bad++;
            continue;
        }
        fat_file_t of;
        if (!fs_create(out, &of)) {
            err = "could not create a file (disk full or write-protected?)";
            break;
        }
        if (cb)
            cb(user, shown, done, total);
        s->in = zf;
        s->out = &of;
        s->ipos = s->ilen = 0;
        s->remain = csize;
        s->bitbuf = 0;
        s->bitcnt = 0;
        s->err = s->werr = false;
        s->wpos = s->flushed = 0;
        s->crc = 0;
        s->cb = cb;
        s->user = user;
        s->name = shown;
        s->done_before = done;
        s->total = total;
        bool ok;
        if (method == 0) {
            for (uint32_t k = 0; k < csize && !s->err; k++)
                out_byte(s, (uint8_t)in_byte(s));
            out_flush(s);
            ok = !s->err;
        } else {
            ok = inflate_stream(s);
        }
        ok = ok && s->wpos == usize && s->crc == crc;
        of.entry.modify_time = mtime; /* the time it has in the zip */
        of.entry.modify_date = mdate;
        fs_close(&of);
        done += usize;
        st->out_bytes += s->wpos;
        if (!ok) {
            fs_delete(out);
            if (s->werr) {
                err = "could not write a file (disk full?)";
                break;
            }
            st->bad++;
            continue;
        }
        st->files++;
    }
    return err;
}

const char *zip_extract(const char *zip_path, const char *dst_dir,
                        bool overwrite, zip_progress_fn cb, void *user,
                        zip_stats_t *st) {
    static zip_stats_t dummy;
    if (!st)
        st = &dummy;
    memset(st, 0, sizeof(*st));
    fat_file_t zf;
    if (!fs_open(zip_path, &zf))
        return "the archive could not be opened";
    uint32_t size = zf.entry.file_size;
    /* the end record: last 22 bytes, unless there is a comment */
    static uint8_t tail[1024 + 22];
    uint32_t eocd = 0xFFFFFFFF;
    uint8_t z[22];
    for (uint32_t back = 0; back < 65536 + 22 && eocd == 0xFFFFFFFF;
         back += 1024) {
        if (back >= size)
            break;
        uint32_t end = size - back;
        uint32_t start = end > sizeof(tail) ? end - sizeof(tail) : 0;
        uint32_t n = end - start;
        if (!fs_seek(&zf, start) || fs_read(&zf, tail, (int)n) != (int)n)
            break;
        for (int i = (int)n - 22; i >= 0; i--)
            if (tail[i] == 'P' && tail[i + 1] == 'K' && tail[i + 2] == 5 &&
                tail[i + 3] == 6) {
                eocd = start + (uint32_t)i;
                memcpy(z, tail + i, 22);
                break;
            }
    }
    if (eocd == 0xFFFFFFFF) {
        fs_close(&zf);
        return "this is not a ZIP archive (or it is damaged)";
    }
    uint32_t count = get16(z + 10), cd_size = get32(z + 12),
             cd_off = get32(z + 16);
    if (get16(z + 4) != 0 || cd_off == 0xFFFFFFFF || count == 0xFFFF ||
        cd_off + cd_size > eocd) {
        fs_close(&zf);
        return "this archive is split, ZIP64 or damaged: not supported";
    }
    uint8_t *cd = (uint8_t *)kmalloc(cd_size + 1);
    inf_t *s = (inf_t *)kzalloc(sizeof(inf_t));
    if (!cd || !s) {
        kfree(cd);
        kfree(s);
        fs_close(&zf);
        return "not enough memory";
    }
    const char *err = NULL;
    if (!fs_seek(&zf, cd_off) || fs_read(&zf, cd, (int)cd_size) != (int)cd_size)
        err = "the archive could not be read";
    else
        err = extract_entries(&zf, cd, cd_size, count, s, dst_dir, overwrite,
                              cb, user, st);
    st->in_bytes = size;
    fs_close(&zf);
    kfree(cd);
    kfree(s);
    return err;
}
