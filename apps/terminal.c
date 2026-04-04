#include "os/api.h"

#define TERM_X          4
#define TERM_Y          2
#define CHAR_W          5
#define CHAR_H          6
#define LINE_H          8
#define INPUT_H         10

#define MAX_COLS        38
#define SCROLL_BAR_W    6

#define OUTPUT_LINES    128
#define OUTPUT_LINE_W   80
#define HISTORY_DEPTH   32
#define INPUT_CAP       128

typedef struct {
    window *win;

    char    lines[OUTPUT_LINES][OUTPUT_LINE_W];
    int     line_head;
    int     line_count;

    int     scroll_top;
    int     visible_rows;

    char    input[INPUT_CAP];
    int     input_len;
    bool    input_focused;

    char    history[HISTORY_DEPTH][INPUT_CAP];
    int     hist_count;
    int     hist_pos;
    char    hist_draft[INPUT_CAP];

    bool    waiting_for_enter;
} terminal_state_t;

app_descriptor terminal_app;

static void term_push_line(terminal_state_t *s, const char *text);
static void term_push_output(terminal_state_t *s, const char *text);
static void term_execute(terminal_state_t *s);
static void term_scroll_to_bottom(terminal_state_t *s);

static bool terminal_close(window *w) {
    (void)w;
    os_quit_app_by_desc(&terminal_app);
    return true;
}
static void on_file_close(void) { terminal_close(NULL); }

static inline int line_idx(const terminal_state_t *s, int n) { return (s->line_head + n) % OUTPUT_LINES; }

static void term_push_line(terminal_state_t *s, const char *text) {
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

static void term_push_output(terminal_state_t *s, const char *text) {
    char line_buf[OUTPUT_LINE_W];
    int  pos = 0;
    int  col = 0;

    while (text[pos]) {
        char c = text[pos++];
        if (c == '\n' || col >= MAX_COLS) {
            line_buf[col] = '\0';
            term_push_line(s, line_buf);
            col = 0;
            if (c == '\n') continue;
        }
        line_buf[col++] = c;
    }
    if (col > 0) {
        line_buf[col] = '\0';
        term_push_line(s, line_buf);
    }
}

static void term_scroll_to_bottom(terminal_state_t *s) {
    int max_scroll = s->line_count - s->visible_rows;
    s->scroll_top  = (max_scroll > 0) ? max_scroll : 0;
}

static void term_scroll_up(terminal_state_t *s, int lines) {
    s->scroll_top -= lines;
    if (s->scroll_top < 0) s->scroll_top = 0;
}

static void term_scroll_down(terminal_state_t *s, int lines) {
    int max_scroll = s->line_count - s->visible_rows;
    s->scroll_top += lines;
    if (s->scroll_top > max_scroll) s->scroll_top = (max_scroll > 0) ? max_scroll : 0;
}

static void term_execute(terminal_state_t *s) {
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

    bool dup = (s->hist_count > 0
                && strcmp(s->history[(s->hist_count - 1) % HISTORY_DEPTH],
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
    cmd_status_t status = cli_execute_command(s->input, outbuf, sizeof(outbuf));

    if (outbuf[0])
        term_push_output(s, outbuf);

    if (status == CMD_STATUS_CLEAR) {
        s->line_count = 0;
        s->line_head  = 0;
        s->scroll_top = 0;
    }

    s->input[0]  = '\0';
    s->input_len = 0;

    term_scroll_to_bottom(s);
}

void terminal_window_draw(window *win, void *userdata) {
    terminal_state_t *s = (terminal_state_t *)userdata;
    if (!win || !win->visible) return;
    if (win->dragging) return;

    int wx = win->x;
    int wy = win->y + MENUBAR_H + 16;
    int ww = win->w;
    int wh = win->h - 16;

    int content_x = wx + TERM_X;
    int text_area_w = ww - TERM_X * 2 - SCROLL_BAR_W;

    int output_h = wh - INPUT_H - 6;

    for (int row = 0; row < s->visible_rows; row++) {
        int logical = s->scroll_top + row;
        if (logical >= s->line_count) break;

        int py = wy + TERM_Y + row * LINE_H;
        const char *line = s->lines[line_idx(s, logical)];

        char draw_buf[MAX_COLS + 1];
        int ci = 0;
        while (line[ci] && ci < MAX_COLS) { draw_buf[ci] = line[ci]; ci++; }
        draw_buf[ci] = '\0';

        draw_string(content_x, py, draw_buf, WHITE, 2);
    }

    int sb_x = wx + ww - SCROLL_BAR_W - 2;
    int sb_y = wy + TERM_Y;
    int sb_h = output_h;

    fill_rect(sb_x, sb_y, SCROLL_BAR_W, sb_h, DARK_GRAY);
    draw_rect(sb_x, sb_y, SCROLL_BAR_W, sb_h, BLACK);

    if (s->line_count > s->visible_rows) {
        int total       = s->line_count;
        int thumb_h     = (s->visible_rows * sb_h) / total;
        if (thumb_h < 4) thumb_h = 4;
        int thumb_range = sb_h - thumb_h;
        int max_scroll  = total - s->visible_rows;
        int thumb_y     = sb_y + (s->scroll_top * thumb_range) / max_scroll;
        fill_rect(sb_x + 1, thumb_y, SCROLL_BAR_W - 2, thumb_h, LIGHT_GRAY);
    }

    int divider_y = wy + wh - INPUT_H - 4;
    draw_line(wx + 2, divider_y, wx + ww - 2, divider_y, DARK_GRAY);

    int input_y  = wy + wh - INPUT_H - 1;
    int input_x  = wx + 2;
    int input_w  = ww - 4;

    uint8_t input_bg     = BLACK;
    uint8_t input_border = s->input_focused ? CYAN : DARK_GRAY;

    fill_rect(input_x, input_y, input_w, INPUT_H, input_bg);
    draw_rect(input_x, input_y, input_w, INPUT_H, input_border);

    draw_string(input_x + 2, input_y + 2, ">", CYAN, 2);

    draw_string(input_x + 10, input_y + 2, s->input, WHITE, 2);

    extern volatile uint32_t pit_ticks;
    if (s->input_focused && (pit_ticks / 50) % 2 == 0) {
        int cx = input_x + 10 + s->input_len * CHAR_W;
        if (cx < input_x + input_w - 4)
            draw_string(cx, input_y + 2, "|", CYAN, 2);
    }

    (void)text_area_w;
}

static void terminal_init(void *state) {
    terminal_state_t *s = (terminal_state_t *)state;

    const window_spec_t spec = {
        .x             = 20,
        .y             = 20,
        .w             = 210,
        .h             = 160,
        .title         = "Terminal",
        .title_color   = WHITE,
        .bar_color     = DARK_GRAY,
        .content_color = BLACK,
        .visible       = true,
        .on_close      = terminal_close,
    };
    s->win = wm_register(&spec);
    if (!s->win) return;
    s->win->on_draw = terminal_window_draw;
	s->win->on_draw_userdata = s;

    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "Close", on_file_close);

    int content_h    = s->win->h - 16;
    int output_area_h = content_h - INPUT_H - 4;
    s->visible_rows  = output_area_h / LINE_H;
    if (s->visible_rows < 1) s->visible_rows = 1;

    s->input_focused = true;
    s->hist_pos      = -1;

    term_push_line(s, "ASMOS Terminal");
    term_push_line(s, "Type 'help' for commands.");
    term_push_line(s, "");
    term_scroll_to_bottom(s);
    //terminal_window_draw(s->win, s->win->on_draw_userdata);
}

static void terminal_on_frame(void *state) {
    terminal_state_t *s = (terminal_state_t *)state;
    if (!s->win || !s->win->visible) return;

    if (mouse.left_clicked) {
        int wx  = s->win->x;
        int wy  = s->win->y + MENUBAR_H + 16;
        int ww  = s->win->w;
        int wh  = s->win->h - 16;
        int iy  = wy + wh - INPUT_H - 1;
        bool on_input = mouse.x >= wx + 2 && mouse.x < wx + ww - 2
                     && mouse.y >= iy     && mouse.y < iy + INPUT_H;
        s->input_focused = on_input;
    }

    if (mouse.left_clicked || mouse.left) {
        int wx   = s->win->x;
        int wy   = s->win->y + MENUBAR_H + 16;
        int ww   = s->win->w;
        int sb_x = wx + ww - SCROLL_BAR_W - 2;
        int sb_y = wy + TERM_Y;
        int sb_h = (s->win->h - 16) - INPUT_H - 6;

        if (mouse.x >= sb_x && mouse.x < sb_x + SCROLL_BAR_W
         && mouse.y >= sb_y && mouse.y < sb_y + sb_h
         && s->line_count > s->visible_rows) {
            int max_scroll = s->line_count - s->visible_rows;
            int new_scroll = ((mouse.y - sb_y) * max_scroll) / sb_h;
            if (new_scroll < 0)          new_scroll = 0;
            if (new_scroll > max_scroll) new_scroll = max_scroll;
            s->scroll_top = new_scroll;
        }
    }

    if (!s->input_focused || !kb.key_pressed) {
        terminal_window_draw(s->win, s->win->on_draw_userdata);
        return;
    }

    uint8_t sc = kb.last_scancode;

    if (sc == F5) { term_scroll_up(s, s->visible_rows);   terminal_window_draw(s->win, s->win->on_draw_userdata); return; }
    if (sc == F6) { term_scroll_down(s, s->visible_rows); terminal_window_draw(s->win, s->win->on_draw_userdata); return; }

    if (sc == ENTER) {
        term_execute(s);
        terminal_window_draw(s->win, s->win->on_draw_userdata);
        return;
    }

    if (sc == BACKSPACE) {
        if (s->input_len > 0) {
            s->input[--s->input_len] = '\0';
        }
        terminal_window_draw(s->win, s->win->on_draw_userdata);
        return;
    }

    if (sc == 0x48) {
        if (s->hist_count == 0) { terminal_window_draw(s->win, s->win->on_draw_userdata); return; }

        if (s->hist_pos == -1) {
            int di = 0;
            while (s->input[di] && di < INPUT_CAP - 1)
                s->hist_draft[di] = s->input[di++];
            s->hist_draft[di] = '\0';
            s->hist_pos = s->hist_count - 1;
        } else if (s->hist_pos > 0) {
            s->hist_pos--;
        }

        const char *entry = s->history[s->hist_pos % HISTORY_DEPTH];
        s->input_len = 0;
        while (entry[s->input_len] && s->input_len < INPUT_CAP - 1) {
            s->input[s->input_len] = entry[s->input_len];
            s->input_len++;
        }
        s->input[s->input_len] = '\0';
        terminal_window_draw(s->win, s->win->on_draw_userdata);
        return;
    }

    if (sc == 0x50) {
        if (s->hist_pos == -1) { terminal_window_draw(s->win, s->win->on_draw_userdata); return; }

        if (s->hist_pos < s->hist_count - 1) {
            s->hist_pos++;
            const char *entry = s->history[s->hist_pos % HISTORY_DEPTH];
            s->input_len = 0;
            while (entry[s->input_len] && s->input_len < INPUT_CAP - 1) {
                s->input[s->input_len] = entry[s->input_len];
                s->input_len++;
            }
            s->input[s->input_len] = '\0';
        } else {
            s->hist_pos  = -1;
            s->input_len = 0;
            while (s->hist_draft[s->input_len] && s->input_len < INPUT_CAP - 1) {
                s->input[s->input_len] = s->hist_draft[s->input_len];
                s->input_len++;
            }
            s->input[s->input_len] = '\0';
        }
        terminal_window_draw(s->win, s->win->on_draw_userdata);
        return;
    }

    if (kb.last_char >= 32 && kb.last_char < 127) {
        if (s->input_len < INPUT_CAP - 1) {
            s->input[s->input_len++] = kb.last_char;
            s->input[s->input_len]   = '\0';
            s->hist_pos = -1;
        }
    }

    terminal_window_draw(s->win, s->win->on_draw_userdata);
}

static void terminal_destroy(void *state) {
    terminal_state_t *s = (terminal_state_t *)state;
    if (s->win) {
        wm_unregister(s->win);
        s->win = NULL;
    }
}

app_descriptor terminal_app = {
    .name       = "Terminal",
    .state_size = sizeof(terminal_state_t),
    .init       = terminal_init,
    .on_frame   = terminal_on_frame,
    .destroy    = terminal_destroy,
};
