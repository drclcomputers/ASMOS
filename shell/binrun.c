#include "shell/binrun.h"

#include "fs/fs.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "os/scheduler.h"

#define BINRUN_MAX_SIZE 65536

static term_context_t *s_active_ctx = NULL;

static void bsc_print(const char *str) {
    if (!str || !s_active_ctx)
        return;
    term_ctx_print(s_active_ctx, str);
    term_buf_push_text(str);
}

static void bsc_putchar(char c) {
    if (!s_active_ctx)
        return;
    if (s_active_ctx->putchar)
        s_active_ctx->putchar(s_active_ctx, c);
}

static int bsc_readline(char *buf, int maxchars) {
    if (!buf || maxchars <= 0 || !s_active_ctx)
        return 0;
    if (s_active_ctx->readline)
        return s_active_ctx->readline(s_active_ctx, buf, maxchars);
    return 0;
}

static int bsc_getchar(void) {
    if (!s_active_ctx)
        return 0;
    if (s_active_ctx->getchar)
        return s_active_ctx->getchar(s_active_ctx);
    return 0;
}

static void bsc_itoa(int val, char *buf) {
    if (!buf)
        return;
    int_to_str(val, buf);
}

static const bin_syscall_t s_table = {
    .sc_print = bsc_print,
    .sc_putchar = bsc_putchar,
    .sc_readline = bsc_readline,
    .sc_getchar = bsc_getchar,
    .sc_itoa = bsc_itoa,
};

const bin_syscall_t *binrun_make_table(term_context_t *ctx) {
    s_active_ctx = ctx;
    return &s_table;
}

typedef struct {
    uint8_t *buf;
    uint32_t size;
    const bin_syscall_t *sc;
    term_context_t *ctx;
    int task_slot;
} bin_task_ctx_t;

static void bin_task_entry(void *arg) {
    bin_task_ctx_t *ctx = (bin_task_ctx_t *)arg;

    s_active_ctx = ctx->ctx;

    typedef void (*bin_fn_t)(const bin_syscall_t *);
    bin_fn_t entry = (bin_fn_t)(void *)ctx->buf;

    entry(ctx->sc);

    kfree(ctx->buf);
    ctx->buf = NULL;
    s_active_ctx = NULL;

    kfree(ctx);

    scheduler_exit_current();
}

static void do_run(term_context_t *tctx, const char *path, char *out,
                   size_t max) {
    fat_file_t f;
    if (!fs_open(path, &f)) {
        sprintf(out, "run: cannot open '%s'\n", path);
        return;
    }
    uint32_t file_size = f.entry.file_size;
    fs_close(&f);

    if (file_size == 0) {
        sprintf(out, "run: '%s' is empty\n", path);
        return;
    }
    if (file_size > BINRUN_MAX_SIZE) {
        sprintf(out, "run: '%s' too large (%u B)\n", path, file_size);
        return;
    }

    uint8_t *buf = (uint8_t *)kmalloc(file_size);
    if (!buf) {
        sprintf(out, "run: out of memory\n");
        return;
    }

    if (!fs_open(path, &f)) {
        kfree(buf);
        sprintf(out, "run: re-open failed\n");
        return;
    }
    int got = fs_read(&f, buf, (int)file_size);
    fs_close(&f);

    if (got != (int)file_size) {
        kfree(buf);
        sprintf(out, "run: read error (%d of %u)\n", got, file_size);
        return;
    }

    bin_task_ctx_t *bctx = (bin_task_ctx_t *)kmalloc(sizeof(bin_task_ctx_t));
    if (!bctx) {
        kfree(buf);
        sprintf(out, "run: out of memory\n");
        return;
    }
    bctx->buf = buf;
    bctx->size = file_size;
    bctx->sc = binrun_make_table(tctx);
    bctx->ctx = tctx;
    bctx->task_slot = -1;

    int slot = scheduler_add_task(bin_task_entry, bctx);
    if (slot < 0) {
        kfree(buf);
        kfree(bctx);
        sprintf(out, "run: no free task slots\n");
        return;
    }
    bctx->task_slot = slot;

    sprintf(out, "run: launched '%s' (%u B) as task %d\n", path, file_size,
            slot);
}

void cmd_run(term_context_t *ctx, const char *path, char *out, size_t max) {
    if (!path || path[0] == '\0') {
        sprintf(out, "Usage: run <file.bin>\n"
                     "  Binary calling convention (cdecl):\n"
                     "    void binary(const bin_syscall_t *sc)\n"
                     "  Get base addr first:\n"
                     "    call .here\n"
                     "    .here: pop ebp\n"
                     "    sub  ebp, (.here - $$)\n"
                     "  Then use [ebp+label] for all data refs.\n"
                     "  SC offsets: PRINT=0 PUTCHAR=4 READLINE=8 GETCHAR=12 "
                     "ITOA=16\n\n");
        return;
    }
    do_run(ctx, path, out, max);
}

void cmd_run_cli(const char *path, char *out, size_t max) {
    cmd_run(NULL, path, out, max);
}
