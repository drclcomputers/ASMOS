#include "os/api.h"
#include "shell/binrun.h"
#include "shell/term_buf.h"

#define TERM_X 4
#define TERM_Y 2
#define CHAR_W 5
#define CHAR_H 6
#define LINE_H 8
#define INPUT_H 10

#define SCROLL_BAR_W 6
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
    window *win;

    bool should_exit;

    char lines[OUTPUT_LINES][OUTPUT_LINE_W];
    int line_head;
    int line_count;

    int scroll_top;
    int visible_rows;

    char input[INPUT_CAP];
    int input_len;
    int input_scroll;
    bool input_focused;

    char history[HISTORY_DEPTH][INPUT_CAP];
    int hist_count;
    int hist_pos;
    char hist_draft[INPUT_CAP];

    bool binary_waiting;
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
    int i = 0;
    while (text[i] && i < OUTPUT_LINE_W - 1) {
        s->lines[idx][i] = text[i];
        i++;
    }
    s->lines[idx][i] = '\0';
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

static void term_scroll_to_bottom(asmterm_state_t *s) {
    int ms = s->line_count - s->visible_rows;
    s->scroll_top = (ms > 0) ? ms : 0;
}
static void term_scroll_up(asmterm_state_t *s, int l) {
    s->scroll_top -= l;
    if (s->scroll_top < 0)
        s->scroll_top = 0;
}
static void term_scroll_down(asmterm_state_t *s, int l) {
    int ms = s->line_count - s->visible_rows;
    s->scroll_top += l;
    if (s->scroll_top > ms)
        s->scroll_top = (ms > 0) ? ms : 0;
}

static void tctx_print(term_context_t *ctx, const char *str) {
    asmterm_state_t *s = (asmterm_state_t *)ctx->userdata;
    if (!str)
        return;
    term_push_output(s, str);
    term_buf_push_text(str);
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
    int len = 0;
    buf[0] = '\0';

    while (1) {
        task_yield();

        while (!iq_empty(&s->iq)) {
            char c = iq_pop(&s->iq);
            if (c == '\n') {
                buf[len] = '\0';
                s->binary_waiting = false;
                return len;
            }
            if (c == '\b') {
                if (len > 0) {
                    len--;
                    buf[len] = '\0';
                }
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

static void term_execute(asmterm_state_t *s) {
    if (s->input_len == 0) {
        term_push_line(s, "");
        term_scroll_to_bottom(s);
        return;
    }

    char echo[INPUT_CAP + 4];
    echo[0] = '>';
    echo[1] = ' ';
    int ei = 2;
    for (int i = 0; i < s->input_len && ei < (int)sizeof(echo) - 1; i++)
        echo[ei++] = s->input[i];
    echo[ei] = '\0';
    term_push_line(s, echo);

    bool dup = (s->hist_count > 0 &&
                strcmp(s->history[(s->hist_count - 1) % HISTORY_DEPTH],
                       s->input) == 0);
    if (!dup) {
        int slot = s->hist_count % HISTORY_DEPTH;
        int ci = 0;
        while (s->input[ci] && ci < INPUT_CAP - 1) {
            s->history[slot][ci] = s->input[ci];
            ci++;
        }
        s->history[slot][ci] = '\0';
        s->hist_count++;
    }
    s->hist_pos = -1;
    s->hist_draft[0] = '\0';

    char outbuf[1024];
    outbuf[0] = '\0';
    if (strcmp("exit", s->input) == 0) {
        s->should_exit = true;
        return;
    }

    char command[64] = "";
    char arg[256] = "";
    {
        const char *p = s->input;
        int ci = 0;
        while (*p == ' ')
            p++;
        while (*p && *p != ' ' && ci < 63)
            command[ci++] = *p++;
        command[ci] = '\0';
        while (*p == ' ')
            p++;
        int ai = 0;
        while (*p && ai < 255)
            arg[ai++] = *p++;
        arg[ai] = '\0';
    }

    cmd_status_t status = CMD_STATUS_OK;
    if (strcmp(command, "run") == 0) {
        cmd_run(&s->term_ctx, arg, outbuf, sizeof(outbuf));
    } else {
        status = cli_execute_command(s->input, outbuf, sizeof(outbuf));
    }

    if (outbuf[0]) {
        term_push_output(s, outbuf);
        term_buf_push_text(outbuf);
    }

    if (status == CMD_STATUS_CLEAR) {
        s->line_count = 0;
        s->line_head = 0;
        s->scroll_top = 0;
    }

    s->input[0] = '\0';
    s->input_len = 0;
    s->input_scroll = 0;
    term_scroll_to_bottom(s);
}

void asmterm_window_draw(window *win, void *userdata) {
    asmterm_state_t *s = (asmterm_state_t *)userdata;
    if (!win || !win->visible)
        return;
    if (win->dragging)
        return;

    int wx = win->x, wy = win->y + MENUBAR_H + 16, ww = win->w,
        wh = win->h - 16;
    int content_x = wx + TERM_X;
    int output_h = wh - INPUT_H - 6;

    int new_visible = output_h / LINE_H;
    if (new_visible < 1)
        new_visible = 1;
    if (new_visible != s->visible_rows) {
        s->visible_rows = new_visible;
        int ms = s->line_count - s->visible_rows;
        if (s->scroll_top > ms)
            s->scroll_top = (ms > 0) ? ms : 0;
    }

    int max_out_cols = (ww - SCROLL_BAR_W - TERM_X - 4) / CHAR_W;
    if (max_out_cols < 1)
        max_out_cols = 1;

    for (int row = 0; row < s->visible_rows; row++) {
        int logical = s->scroll_top + row;
        if (logical >= s->line_count)
            break;
        int py = wy + TERM_Y + row * LINE_H;
        const char *line = s->lines[line_idx(s, logical)];
        char draw_buf[OUTPUT_LINE_W + 1];
        int limit = max_out_cols < OUTPUT_LINE_W ? max_out_cols : OUTPUT_LINE_W;
        int ci = 0;
        while (line[ci] && ci < limit) {
            draw_buf[ci] = line[ci];
            ci++;
        }
        draw_buf[ci] = '\0';
        draw_string(content_x, py, draw_buf, WHITE, 2);
    }

    int sb_x = wx + ww - SCROLL_BAR_W - 2, sb_y = wy + TERM_Y, sb_h = output_h;
    fill_rect(sb_x, sb_y, SCROLL_BAR_W, sb_h, DARK_GRAY);
    draw_rect(sb_x, sb_y, SCROLL_BAR_W, sb_h, BLACK);
    if (s->line_count > s->visible_rows) {
        int total = s->line_count;
        int thumb_h = (s->visible_rows * sb_h) / total;
        if (thumb_h < 4)
            thumb_h = 4;
        int thumb_range = sb_h - thumb_h;
        int max_scroll = total - s->visible_rows;
        int thumb_y = sb_y + (s->scroll_top * thumb_range) / max_scroll;
        fill_rect(sb_x + 1, thumb_y, SCROLL_BAR_W - 2, thumb_h, LIGHT_GRAY);
    }

    int div_y = wy + wh - INPUT_H - 4;
    draw_line(wx + 2, div_y, wx + ww - 2, div_y, DARK_GRAY);

    int iy = wy + wh - INPUT_H - 1, ix = wx + 2, iw = ww - 4;
    uint8_t iborder = s->input_focused ? CYAN : DARK_GRAY;
    fill_rect(ix, iy, iw, INPUT_H, BLACK);
    draw_rect(ix, iy, iw, INPUT_H, iborder);

    if (s->binary_waiting) {
        draw_string(ix + 2, iy + 2, "BIN>", LIGHT_GREEN, 2);
    } else {
        draw_string(ix + 2, iy + 2, ">", CYAN, 2);
    }

    int tax = ix + 10 + (s->binary_waiting ? 15 : 0);
    int taw = iw - 12 - CHAR_W - (s->binary_waiting ? 15 : 0);
    int mvc = taw / CHAR_W;
    if (mvc < 1)
        mvc = 1;

    s->input_scroll = s->input_len - mvc;
    if (s->input_scroll < 0)
        s->input_scroll = 0;

    char iv[256];
    int tc = s->input_len - s->input_scroll;
    if (tc > mvc)
        tc = mvc;
    if (tc >= (int)sizeof(iv))
        tc = sizeof(iv) - 1;
    for (int i = 0; i < tc; i++)
        iv[i] = s->input[s->input_scroll + i];
    iv[tc] = '\0';
    draw_string(tax, iy + 2, iv, WHITE, 2);

    extern volatile uint32_t pit_ticks;
    if (s->input_focused && (pit_ticks / 50) % 2 == 0) {
        int cx = tax + tc * CHAR_W;
        if (cx < ix + iw - 2)
            draw_string(cx, iy + 2, "|", CYAN, 2);
    }
}

static bool asmterm_close(window *w) {
    (void)w;
    os_quit_app_by_desc(&asmterm_app);
    return true;
}
static void on_file_close(void) { asmterm_close(NULL); }
static void on_about(void) {
    modal_show(
        MODAL_INFO, "About ASMTerm",
        "ASMTerm v1.1\nASMOS Terminal\nBinary I/O via context callbacks.", NULL,
        NULL);
}

static void asmterm_init(void *state) {
    asmterm_state_t *s = (asmterm_state_t *)state;

    const window_spec_t spec = {
        .x = 20,
        .y = 20,
        .w = 210,
        .h = 160,
        .min_w = 60,
        .min_h = 60,
        .resizable = true,
        .title = "ASMTerm",
        .title_color = WHITE,
        .bar_color = DARK_GRAY,
        .content_color = BLACK,
        .visible = true,
        .on_close = asmterm_close,
    };
    s->win = wm_register(&spec);
    if (!s->win)
        return;
    s->win->on_draw = asmterm_window_draw;
    s->win->on_draw_userdata = s;

    menu *fm = window_add_menu(s->win, "File");
    menu_add_item(fm, "Close", on_file_close);
    menu_add_separator(fm);
    menu_add_item(fm, "About ASMTerm", on_about);

    int ch = s->win->h - 16, oah = ch - INPUT_H - 4;
    s->visible_rows = oah / LINE_H;
    if (s->visible_rows < 1)
        s->visible_rows = 1;
    s->input_focused = true;
    s->hist_pos = -1;
    s->input_scroll = 0;

    iq_init(&s->iq);
    s->binary_waiting = false;
    s->term_ctx.print = tctx_print;
    s->term_ctx.putchar = tctx_putchar;
    s->term_ctx.readline = tctx_readline;
    s->term_ctx.getchar = tctx_getchar;
    s->term_ctx.userdata = s;

    term_push_line(s, "ASMOS Terminal v1.1");
    term_push_line(s, "Commands: help  asm  run");
    term_push_line(s, "");
    term_scroll_to_bottom(s);
}

static void asmterm_on_frame(void *state) {
    asmterm_state_t *s = (asmterm_state_t *)state;
    if (!s->win || !s->win->visible)
        return;

    if (s->should_exit) {
        asmterm_close(NULL);
        return;
    }

    if (mouse.left_clicked) {
        int wx = s->win->x, wy = s->win->y + MENUBAR_H + 16, ww = s->win->w,
            wh = s->win->h - 16;
        int iy = wy + wh - INPUT_H - 1;
        bool on_input = (mouse.x >= wx + 2 && mouse.x < wx + ww - 2 &&
                         mouse.y >= iy && mouse.y < iy + INPUT_H);
        s->input_focused = on_input;
    }

    if (mouse.left_clicked || mouse.left) {
        int wx = s->win->x, wy = s->win->y + MENUBAR_H + 16, ww = s->win->w;
        int sb_x = wx + ww - SCROLL_BAR_W - 2, sb_y = wy + TERM_Y;
        int sb_h = (s->win->h - 16) - INPUT_H - 6;
        if (mouse.x >= sb_x && mouse.x < sb_x + SCROLL_BAR_W &&
            mouse.y >= sb_y && mouse.y < sb_y + sb_h &&
            s->line_count > s->visible_rows) {
            int ms = s->line_count - s->visible_rows;
            int ns = ((mouse.y - sb_y) * ms) / sb_h;
            if (ns < 0)
                ns = 0;
            if (ns > ms)
                ns = ms;
            s->scroll_top = ns;
        }
    }

    if (!s->input_focused || !kb.key_pressed) {
        asmterm_window_draw(s->win, s->win->on_draw_userdata);
        return;
    }

    uint8_t sc = kb.last_scancode;
    char ch = kb.last_char;

    if (sc == F5) {
        term_scroll_up(s, s->visible_rows);
        asmterm_window_draw(s->win, s->win->on_draw_userdata);
        return;
    }
    if (sc == F6) {
        term_scroll_down(s, s->visible_rows);
        asmterm_window_draw(s->win, s->win->on_draw_userdata);
        return;
    }

    if (s->binary_waiting) {
        if (sc == ENTER) {
            if (s->input_len > 0) {
                term_push_line(s, s->input);
                term_scroll_to_bottom(s);
                for (int i = 0; i < s->input_len; i++)
                    iq_push(&s->iq, s->input[i]);
                s->input[0] = '\0';
                s->input_len = 0;
                s->input_scroll = 0;
            }
            iq_push(&s->iq, '\n');
        } else if (sc == BACKSPACE) {
            if (s->input_len > 0) {
                s->input[--s->input_len] = '\0';
                iq_push(&s->iq, '\b');
            }
        } else if (ch >= 32 && ch < 127) {
            if (s->input_len < INPUT_CAP - 1) {
                s->input[s->input_len++] = ch;
                s->input[s->input_len] = '\0';
                iq_push(&s->iq, ch);
            }
        }
        asmterm_window_draw(s->win, s->win->on_draw_userdata);
        return;
    }

    if (sc == ENTER) {
        term_execute(s);
        asmterm_window_draw(s->win, s->win->on_draw_userdata);
        return;
    }
    if (sc == BACKSPACE) {
        if (s->input_len > 0)
            s->input[--s->input_len] = '\0';
        asmterm_window_draw(s->win, s->win->on_draw_userdata);
        return;
    }

    if (sc == 0x48) {
        if (s->hist_count == 0) {
            asmterm_window_draw(s->win, s->win->on_draw_userdata);
            return;
        }
        if (s->hist_pos == -1) {
            int di = 0;
            while (s->input[di] && di < INPUT_CAP - 1)
                s->hist_draft[di] = s->input[di++];
            s->hist_draft[di] = '\0';
            s->hist_pos = s->hist_count - 1;
        } else if (s->hist_pos > 0)
            s->hist_pos--;
        const char *e = s->history[s->hist_pos % HISTORY_DEPTH];
        s->input_len = 0;
        while (e[s->input_len] && s->input_len < INPUT_CAP - 1) {
            s->input[s->input_len] = e[s->input_len];
            s->input_len++;
        }
        s->input[s->input_len] = '\0';
        s->input_scroll = 0;
        asmterm_window_draw(s->win, s->win->on_draw_userdata);
        return;
    }

    if (sc == 0x50) {
        if (s->hist_pos == -1) {
            asmterm_window_draw(s->win, s->win->on_draw_userdata);
            return;
        }
        if (s->hist_pos < s->hist_count - 1) {
            s->hist_pos++;
            const char *e = s->history[s->hist_pos % HISTORY_DEPTH];
            s->input_len = 0;
            while (e[s->input_len] && s->input_len < INPUT_CAP - 1) {
                s->input[s->input_len] = e[s->input_len];
                s->input_len++;
            }
            s->input[s->input_len] = '\0';
        } else {
            s->hist_pos = -1;
            s->input_len = 0;
            while (s->hist_draft[s->input_len] &&
                   s->input_len < INPUT_CAP - 1) {
                s->input[s->input_len] = s->hist_draft[s->input_len];
                s->input_len++;
            }
            s->input[s->input_len] = '\0';
        }
        s->input_scroll = 0;
        asmterm_window_draw(s->win, s->win->on_draw_userdata);
        return;
    }

    if (ch >= 32 && ch < 127) {
        if (s->input_len < INPUT_CAP - 1) {
            s->input[s->input_len++] = ch;
            s->input[s->input_len] = '\0';
            s->hist_pos = -1;
        }
    }
    asmterm_window_draw(s->win, s->win->on_draw_userdata);
}

static void asmterm_destroy(void *state) {
    asmterm_state_t *s = (asmterm_state_t *)state;
    if (s->win) {
        wm_unregister(s->win);
        s->win = NULL;
    }
}

app_descriptor asmterm_app = {
    .name = "ASMTERM",
    .state_size = sizeof(asmterm_state_t),
    .init = asmterm_init,
    .on_frame = asmterm_on_frame,
    .destroy = asmterm_destroy,
};
