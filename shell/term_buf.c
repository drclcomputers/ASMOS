#include "shell/term_buf.h"
#include "fs/fs.h"
#include "lib/memory.h"
#include "lib/string.h"

typedef struct {
    char lines[TERM_BUF_LINES][TERM_BUF_LINE_W];
    int head, count;
} term_ring_t;

static term_ring_t *s_ring = NULL;

static term_ring_t *ring(void) {
    if (!s_ring) {
        s_ring = (term_ring_t *)kzalloc(sizeof(term_ring_t));
        /* kzalloc zeroes the struct, so head=0 count=0 already set */
    }
    return s_ring;
}

void term_buf_push(const char *line) {
    if (!line)
        return;
    term_ring_t *r = ring();
    if (!r)
        return;
    int slot;
    if (r->count < TERM_BUF_LINES) {
        slot = (r->head + r->count) % TERM_BUF_LINES;
        r->count++;
    } else {
        slot = r->head;
        r->head = (r->head + 1) % TERM_BUF_LINES;
    }
    strncpy(r->lines[slot], line, TERM_BUF_LINE_W - 1);
    r->lines[slot][TERM_BUF_LINE_W - 1] = '\0';
}

void term_buf_push_text(const char *text) {
    if (!text)
        return;
    char line[TERM_BUF_LINE_W];
    int li = 0;
    for (const char *p = text;; p++) {
        if (*p == '\n' || *p == '\0') {
            line[li] = '\0';
            if (li > 0)
                term_buf_push(line);
            li = 0;
            if (*p == '\0')
                break;
        } else if (li < TERM_BUF_LINE_W - 1)
            line[li++] = *p;
    }
}

int term_buf_count(void) {
    term_ring_t *r = ring();
    return r ? r->count : 0;
}

const char *term_buf_get(int i) {
    term_ring_t *r = ring();
    if (!r || i < 0 || i >= r->count)
        return NULL;
    return r->lines[(r->head + i) % TERM_BUF_LINES];
}

void term_buf_clear(void) {
    term_ring_t *r = ring();
    if (!r)
        return;
    r->head = 0;
    r->count = 0;
}

void term_buf_free(void) {
    kfree(s_ring);
    s_ring = NULL;
}

int term_buf_read_all(char *dst, int max) {
    if (!dst || max <= 0)
        return 0;
    int count = term_buf_count();
    int w = 0;
    for (int i = 0; i < count && w < max - 1; i++) {
        const char *l = term_buf_get(i);
        if (!l)
            continue;
        int len = (int)strlen(l), sp = max - 1 - w;
        if (len > sp)
            len = sp;
        memcpy(dst + w, l, len);
        w += len;
        if (w < max - 1)
            dst[w++] = '\n';
    }
    dst[w] = '\0';
    return w;
}

int term_buf_read_new(char *dst, int max, int *cursor) {
    if (!dst || max <= 0 || !cursor)
        return 0;
    int count = term_buf_count();
    int w = 0;
    while (*cursor < count && w < max - 1) {
        const char *l = term_buf_get(*cursor);
        if (l) {
            int len = (int)strlen(l), sp = max - 1 - w;
            if (len > sp)
                len = sp;
            memcpy(dst + w, l, len);
            w += len;
            if (w < max - 1)
                dst[w++] = '\n';
        }
        (*cursor)++;
    }
    dst[w] = '\0';
    return w;
}

bool term_buf_save(const char *path) {
    if (!path)
        return false;
    dir_entry_t de;
    if (fs_find(path, &de))
        fs_delete(path);
    fat_file_t f;
    if (!fs_create(path, &f))
        return false;
    int count = term_buf_count();
    char lb[TERM_BUF_LINE_W + 1];
    bool ok = true;
    for (int i = 0; i < count; i++) {
        const char *l = term_buf_get(i);
        if (!l)
            continue;
        int len = (int)strlen(l);
        strncpy(lb, l, TERM_BUF_LINE_W);
        lb[len] = '\n';
        if (fs_write(&f, lb, len + 1) != len + 1) {
            ok = false;
            break;
        }
    }
    fs_close(&f);
    return ok;
}

void term_ctx_printf(term_context_t *ctx, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    va_end(ap);
    term_ctx_print(ctx, buf);
}
