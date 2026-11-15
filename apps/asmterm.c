#include "os/api.h"
#include "os/scheduler.h"
#include "shell/asm.h"
#include "shell/binrun.h"
#include "shell/cmds.h"
#include "shell/term_buf.h"
#include "ui/uifont.h"

#define OUTPUT_LINES 128
#define OUTPUT_LINE_W 80
#define HISTORY_DEPTH 32
#define INPUT_CAP 128

#define INPUT_QUEUE_CAP 256
typedef struct {
    char buf[INPUT_QUEUE_CAP];
    int head, tail;
    bool has_line;
} input_queue_t;

static void iq_init(input_queue_t *q) {
    q->head = q->tail = 0;
    q->has_line = false;
}

static bool iq_empty(const input_queue_t *q) { return q->head == q->tail; }

static void iq_push(input_queue_t *q, char c) {
    int next = (q->tail + 1) % INPUT_QUEUE_CAP;
    if (next == q->head)
        return;
    q->buf[q->tail] = c;
    q->tail = next;
    if (c == '\n')
        q->has_line = true;
}

static char iq_pop(input_queue_t *q) {
    if (iq_empty(q))
        return 0;
    char c = q->buf[q->head];
    q->head = (q->head + 1) % INPUT_QUEUE_CAP;
    if (c == '\n') {

        q->has_line = false;
        for (int i = q->head; i != q->tail; i = (i + 1) % INPUT_QUEUE_CAP)
            if (q->buf[i] == '\n') {
                q->has_line = true;
                break;
            }
    }
    return c;
}

typedef struct {
    window *win; /* must stay first */
    control *out, *sb, *prompt, *input;

    bool should_exit;

    char lines[OUTPUT_LINES][OUTPUT_LINE_W];
    int line_head;
    int line_count;
    int scroll_top;
    int visible_rows;
    int cols;

    char history[HISTORY_DEPTH][INPUT_CAP];
    int hist_count;
    int hist_pos;
    char hist_draft[INPUT_CAP];

    bool binary_waiting; /* a program is blocked in readline/getchar */
    bool line_mode;      /* inside readline(): typed text is echoed */
    bool gopher_mode;
    input_queue_t iq;
    term_context_t term_ctx;
} asmterm_state_t;

app_descriptor asmterm_app;

static inline int line_idx(const asmterm_state_t *s, int n) {
    return (s->line_head + n) % OUTPUT_LINES;
}

static void term_push_line(asmterm_state_t *s, const char *text) {
    int idx;
    if (s->line_count < OUTPUT_LINES) {
        idx = line_idx(s, s->line_count);
        s->line_count++;
    } else {
        idx = s->line_head;
        s->line_head = (s->line_head + 1) % OUTPUT_LINES;
    }
    strncpy(s->lines[idx], text, OUTPUT_LINE_W - 1);
    s->lines[idx][OUTPUT_LINE_W - 1] = '\0';
    wm_invalidate(s->win); /* output can come from a background task */
}

static void term_push_output(asmterm_state_t *s, const char *text) {
    char lb[OUTPUT_LINE_W];
    int pos = 0, col = 0;
    while (text[pos]) {
        char c = text[pos++];
        if (c == '\n' || col >= OUTPUT_LINE_W - 1) {
            lb[col] = '\0';
            term_push_line(s, lb);
            col = 0;
            if (c == '\n')
                continue;
        }
        lb[col++] = c;
    }
    if (col > 0) {
        lb[col] = '\0';
        term_push_line(s, lb);
    }
}

static int max_scroll(const asmterm_state_t *s) {
    int ms = s->line_count - s->visible_rows;
    return ms > 0 ? ms : 0;
}

static void term_scroll_to(asmterm_state_t *s, int top) {
    int ms = max_scroll(s);
    s->scroll_top = top < 0 ? 0 : top > ms ? ms : top;
    wm_invalidate(s->win);
}

static void term_scroll_to_bottom(asmterm_state_t *s) {
    term_scroll_to(s, max_scroll(s));
}

static void term_clear(asmterm_state_t *s) {
    s->line_head = s->line_count = s->scroll_top = 0;
    wm_invalidate(s->win);
}

/* ── term_context for shell programs ───────────────────────────────────── */

static void tctx_print(term_context_t *ctx, const char *str) {
    asmterm_state_t *s = (asmterm_state_t *)ctx->userdata;
    if (!str)
        return;
    term_push_output(s, str);
    term_buf_push_text(str);
    term_scroll_to_bottom(s);
}

static void tctx_putchar(term_context_t *ctx, char c) {
    char tmp[2] = {c, 0};
    tctx_print(ctx, tmp);
}

static int tctx_readline(term_context_t *ctx, char *buf, int maxchars) {
    asmterm_state_t *s = (asmterm_state_t *)ctx->userdata;
    if (!buf || maxchars <= 0)
        return 0;
    s->binary_waiting = true;
    s->line_mode = true;
    int len = 0;
    buf[0] = '\0';
    while (1) {
        task_yield();
        while (!iq_empty(&s->iq)) {
            char c = iq_pop(&s->iq);
            if (c == '\n') {
                buf[len] = '\0';
                s->binary_waiting = false;
                s->line_mode = false;
                return len;
            }
            if ((unsigned char)c >= 0x80)
                continue; /* special keys mean nothing to a text line */
            if (c == '\b') {
                if (len > 0)
                    buf[--len] = '\0';
                continue;
            }
            if (len < maxchars - 1) {
                buf[len++] = c;
                buf[len] = '\0';
            }
        }
    }
}

static int tctx_getchar(term_context_t *ctx) {
    asmterm_state_t *s = (asmterm_state_t *)ctx->userdata;
    s->binary_waiting = true;
    while (iq_empty(&s->iq))
        task_yield();
    char c = iq_pop(&s->iq);
    s->binary_waiting = false;
    return (int)(unsigned char)c;
}

static void tctx_clear(term_context_t *ctx) {
    term_clear((asmterm_state_t *)ctx->userdata);
}

static void tctx_size(term_context_t *ctx, int *cols, int *rows) {
    asmterm_state_t *s = (asmterm_state_t *)ctx->userdata;
    if (cols)
        *cols = s->cols > 10 ? s->cols : 60;
    if (rows)
        *rows = s->visible_rows;
}

void asmterm_set_gopher_mode(term_context_t *ctx, bool on) {
    if (ctx && ctx->userdata)
        ((asmterm_state_t *)ctx->userdata)->gopher_mode = on;
}

/* ── command line ──────────────────────────────────────────────────────── */

static void history_add(asmterm_state_t *s, const char *line) {
    if (s->hist_count > 0 &&
        !strcmp(s->history[(s->hist_count - 1) % HISTORY_DEPTH], line))
        return;
    strncpy(s->history[s->hist_count % HISTORY_DEPTH], line, INPUT_CAP - 1);
    s->hist_count++;
}

static void term_execute(asmterm_state_t *s, const char *input) {
    char echo[INPUT_CAP + 4];
    snprintf(echo, sizeof(echo), "> %s", input);
    term_push_line(s, input[0] ? echo : "");
    term_scroll_to_bottom(s);
    if (!input[0])
        return;
    history_add(s, input);
    s->hist_pos = -1;

    if (!strcmp(input, "exit")) {
        s->should_exit = true;
        return;
    }

    char command[64] = "", arg[256] = "";
    const char *p = input;
    int ci = 0;
    while (*p == ' ')
        p++;
    while (*p && *p != ' ' && ci < 63)
        command[ci++] = *p++;
    command[ci] = '\0';
    while (*p == ' ')
        p++;
    strncpy(arg, p, sizeof(arg) - 1);

    static char outbuf[1024];
    outbuf[0] = '\0';
    cmd_status_t status = CMD_STATUS_OK;
    if (!strcmp(command, "run"))
        cmd_run(&s->term_ctx, arg, outbuf, sizeof(outbuf));
    else if (!strcmp(command, "gopher"))
        cmd_gopher(&s->term_ctx, arg, outbuf, sizeof(outbuf));
    else
        status = cli_execute_command(input, outbuf, sizeof(outbuf));

    if (outbuf[0]) {
        term_push_output(s, outbuf);
        term_buf_push_text(outbuf);
    }
    if (status == CMD_STATUS_CLEAR)
        term_clear(s);
    term_scroll_to_bottom(s);
}

static void history_step(asmterm_state_t *s, int dir) {
    if (dir < 0) {
        if (!s->hist_count)
            return;
        if (s->hist_pos == -1) {
            strncpy(s->hist_draft, ctl_text(s->input), INPUT_CAP - 1);
            s->hist_pos = s->hist_count - 1;
        } else if (s->hist_pos > 0 &&
                   s->hist_pos > s->hist_count - HISTORY_DEPTH)
            s->hist_pos--;
        ctl_set_text(s->input, s->history[s->hist_pos % HISTORY_DEPTH]);
    } else {
        if (s->hist_pos == -1)
            return;
        if (s->hist_pos < s->hist_count - 1) {
            s->hist_pos++;
            ctl_set_text(s->input, s->history[s->hist_pos % HISTORY_DEPTH]);
        } else {
            s->hist_pos = -1;
            ctl_set_text(s->input, s->hist_draft);
        }
    }
    edit_select(s->input, s->input->text_len, s->input->text_len);
}

static void on_input(control *c, int ev) {
    asmterm_state_t *s = c->user;
    if (ev == EV_SUBMIT) {
        char line[INPUT_CAP];
        strncpy(line, ctl_text(c), INPUT_CAP - 1);
        line[INPUT_CAP - 1] = 0;
        ctl_set_text(c, "");
        term_execute(s, line);
    } else if (ev == EV_KEY) {
        uint8_t sc = kb.last_scancode;
        if (sc == UP_ARROW || sc == DOWN_ARROW)
            history_step(s, sc == UP_ARROW ? -1 : 1);
        else if (sc == PAGE_UP || sc == PAGE_DOWN)
            term_scroll_to(s, s->scroll_top +
                                  (sc == PAGE_UP ? -1 : 1) * s->visible_rows);
    } else if (ev == EV_CHANGE) {
        s->hist_pos = -1;
    }
}

/* ── output pane (also takes raw keys while a program waits) ───────────── */

static void paint_out(control *c, int x, int y, int w, int h) {
    asmterm_state_t *s = c->user;
    fill_rect(x, y, w, h, BLACK);
    int rows = (h - 4) / TH_LINE_H;
    if (rows < 1)
        rows = 1;
    if (rows != s->visible_rows) {
        s->visible_rows = rows;
        term_scroll_to(s, s->scroll_top);
    }
    s->cols = (w - 4) / UI_TERM_CW;
    for (int r = 0; r < rows; r++) {
        int n = s->scroll_top + r;
        if (n >= s->line_count)
            break;
        draw_text_n(x + 2, y + 2 + r * TH_LINE_H, s->lines[line_idx(s, n)],
                    s->cols, WHITE, UI_FONT_TERM);
    }
}

static void raw_key(asmterm_state_t *s) {
    uint8_t sc = kb.last_scancode;
    char ch = kb.last_char;
    bool echo = s->line_mode || !s->gopher_mode;
    control *in = s->input;
    if (sc == ENTER) {
        if (in->text_len > 0) {
            term_push_line(s, ctl_text(in));
            term_scroll_to_bottom(s);
            ctl_set_text(in, "");
        }
        iq_push(&s->iq, '\n');
    } else if (sc == BACKSPACE) {
        if (in->text_len > 0) {
            char t[INPUT_CAP];
            strncpy(t, ctl_text(in), INPUT_CAP - 1);
            t[in->text_len - 1 < INPUT_CAP - 1 ? in->text_len - 1
                                               : INPUT_CAP - 1] = 0;
            ctl_set_text(in, t);
        }
        iq_push(&s->iq, '\b');
    } else if (sc == UP_ARROW || sc == DOWN_ARROW || sc == LEFT_ARROW ||
               sc == RIGHT_ARROW || sc == PAGE_UP || sc == PAGE_DOWN ||
               sc == ESC || sc == HOME || sc == END) {
        iq_push(&s->iq, (char)(sc == UP_ARROW      ? TERM_KEY_UP
                               : sc == DOWN_ARROW  ? TERM_KEY_DOWN
                               : sc == LEFT_ARROW  ? TERM_KEY_LEFT
                               : sc == RIGHT_ARROW ? TERM_KEY_RIGHT
                               : sc == PAGE_UP     ? TERM_KEY_PGUP
                               : sc == PAGE_DOWN   ? TERM_KEY_PGDN
                               : sc == HOME        ? TERM_KEY_HOME
                               : sc == END         ? TERM_KEY_END
                                                   : TERM_KEY_ESC));
    } else if (ch >= 32 && ch < 127) {
        if (echo) {
            char t[2] = {ch, 0};
            edit_select(in, in->text_len, in->text_len);
            edit_insert(in, t);
        }
        iq_push(&s->iq, ch);
    }
}

static void on_out(control *c, int ev) {
    asmterm_state_t *s = c->user;
    if (ev != EV_KEY)
        return;
    uint8_t sc = kb.last_scancode;
    if (s->binary_waiting) {
        raw_key(s);
    } else if (sc == PAGE_UP || sc == PAGE_DOWN) {
        term_scroll_to(s, s->scroll_top +
                              (sc == PAGE_UP ? -1 : 1) * s->visible_rows);
    } else if (kb.last_char >= 32 && kb.last_char < 127) {
        /* typing over the output goes to the command line */
        char t[2] = {kb.last_char, 0};
        ctl_focus(s->input);
        edit_select(s->input, s->input->text_len, s->input->text_len);
        edit_insert(s->input, t);
    } else if (sc == ENTER || sc == UP_ARROW || sc == DOWN_ARROW) {
        ctl_focus(s->input);
    }
}

static void on_sb(control *c, int ev) {
    if (ev == EV_CHANGE)
        term_scroll_to(c->user, ctl_value(c));
}

/* ── window ────────────────────────────────────────────────────────────── */

static void menu_paste(void) {
    asmterm_state_t *s = ui_active_state();
    if (!s || !clipboard_has_text())
        return;
    char t[INPUT_CAP];
    int n = 0;
    for (int i = 0; i < g_clipboard.text_len && n < INPUT_CAP - 1; i++)
        if (g_clipboard.text[i] >= 32 && g_clipboard.text[i] < 127)
            t[n++] = g_clipboard.text[i];
    t[n] = 0;
    edit_insert(s->input, t);
    ctl_focus(s->input);
}
static void menu_close(void) {
    asmterm_state_t *s = ui_active_state();
    if (s)
        os_close_own_instance(s->win);
}
static void menu_about(void) {
    dlg_message(NULL, "About ASMTerm",
                "ASMTerm v1.2\nUp/Down: history, PgUp/PgDn: scroll.\n"
                "(c)2026 @drclcomputers");
}

static void asmterm_init(void *state) {
    asmterm_state_t *s = state;
    s->win =
        app_window(s, "ASMTerm", 20, 20, 220, 160, DARK_GRAY, AW_RESIZABLE);
    if (!s->win)
        return;
    s->win->lazy_custom = true; /* output: the term_* functions invalidate */
    window *w = s->win;
    s->out = ctl_custom(w, 1, 1, 208, 126, paint_out, on_out,
                        CS_BORDER | CS_FOCUSABLE | CS_STRETCH_W | CS_STRETCH_H);
    s->sb = ctl_vscroll(w, 210, 1, 126, on_sb);
    ctl_style_add(s->sb, CS_ANCHOR_RIGHT | CS_STRETCH_H | CS_NOFOCUS);
    s->prompt = ctl_label(w, 2, 131, 32, 9, ">", CS_ANCHOR_BOTTOM);
    ctl_set_colors(s->prompt, BLUE, 0xFF);
    s->input =
        ctl_edit(w, 14, 129, 194, 12, "", INPUT_CAP - 1,
                 CS_STRETCH_W | CS_ANCHOR_BOTTOM | ES_TERMFONT, on_input);

    menu *fm = window_add_menu(w, "File");
    menu_add_item(fm, "Paste", menu_paste);
    menu_add_separator(fm);
    menu_add_item(fm, "About ASMTerm", menu_about);
    menu_add_separator(fm);
    menu_add_item(fm, "Close", menu_close);

    s->visible_rows = 15;
    s->hist_pos = -1;
    iq_init(&s->iq);
    s->term_ctx.print = tctx_print;
    s->term_ctx.putchar = tctx_putchar;
    s->term_ctx.readline = tctx_readline;
    s->term_ctx.getchar = tctx_getchar;
    s->term_ctx.clear = tctx_clear;
    s->term_ctx.size = tctx_size;
    s->term_ctx.userdata = s;

    term_push_line(s, "ASMOS Terminal v1.2");
    term_push_line(s, "Commands: help  asm  run  gopher");
    term_push_line(s, "");
    ctl_focus(s->input);
}

static void asmterm_on_frame(void *state) {
    asmterm_state_t *s = state;
    if (!s->win)
        return;
    if (s->should_exit) {
        os_close_own_instance(s->win);
        return;
    }
    /* while a program waits for keys, the output pane owns the keyboard */
    static const char *const P[] = {">", "BIN>", "GOPHER>"};
    const char *pr = !s->binary_waiting ? P[0] : s->gopher_mode ? P[2] : P[1];
    if (strcmp(ctl_text(s->prompt), pr)) {
        int pw = text_width(pr, UI_FONT_SMALL) + 3; /* the label's font */
        ctl_set_text(s->prompt, pr);
        ctl_set_colors(s->prompt, s->binary_waiting ? GREEN : BLUE, 0xFF);
        int wx, wy, cw, ch;
        win_client_rect(s->win, &wx, &wy, &cw, &ch);
        /* where the box is now (its y from creation is relative to the
         * window's size back then, wrong once it has been resized) */
        ctl_move(s->input, 2 + pw, s->input->ay - wy, cw - 4 - pw,
                 s->input->ah);
        ctl_enable(s->input, !s->binary_waiting);
        ctl_focus(s->binary_waiting ? s->out : s->input);
    }
    if (s->binary_waiting && window_is_focused(s->win) &&
        s->win->focus != s->out)
        ctl_focus(s->out);
    /* keep the scrollbar in sync with output */
    ctl_set_range(s->sb, 0, max_scroll(s), s->visible_rows);
    if (ctl_value(s->sb) != s->scroll_top)
        ctl_set_value(s->sb, s->scroll_top);
}

app_descriptor asmterm_app = {
    .name = "ASMTERM",
    .state_size = sizeof(asmterm_state_t),
    .init = asmterm_init,
    .on_frame = asmterm_on_frame,
};
