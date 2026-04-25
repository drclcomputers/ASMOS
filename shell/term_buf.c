#include "shell/term_buf.h"
#include "fs/fs.h"
#include "lib/memory.h"
#include "lib/string.h"

typedef struct {
    char lines[TERM_BUF_LINES][TERM_BUF_LINE_W];
    int head, count;
} term_ring_t;

static term_ring_t s_ring = {.head = 0, .count = 0};

void term_buf_push(const char *line) {
    if (!line)
        return;
    int slot;
    if (s_ring.count < TERM_BUF_LINES) {
        slot = (s_ring.head + s_ring.count) % TERM_BUF_LINES;
        s_ring.count++;
    } else {
        slot = s_ring.head;
        s_ring.head = (s_ring.head + 1) % TERM_BUF_LINES;
    }
    strncpy(s_ring.lines[slot], line, TERM_BUF_LINE_W - 1);

    s_ring.lines[slot][TERM_BUF_LINE_W - 1] = '\0';
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

int term_buf_count(void) { return s_ring.count; }
const char *term_buf_get(int i) {
    if (i < 0 || i >= s_ring.count)
        return NULL;
    return s_ring.lines[(s_ring.head + i) % TERM_BUF_LINES];
}
void term_buf_clear(void) {
    s_ring.head = 0;
    s_ring.count = 0;
}

int term_buf_read_all(char *dst, int max) {
    if (!dst || max <= 0)
        return 0;
    int w = 0;
    for (int i = 0; i < s_ring.count && w < max - 1; i++) {
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
    int w = 0;
    while (*cursor < s_ring.count && w < max - 1) {
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
    char lb[TERM_BUF_LINE_W + 1];
    bool ok = true;
    for (int i = 0; i < s_ring.count; i++) {
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
