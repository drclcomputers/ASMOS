#include "shell/cli.h"
#include "shell/asm/asm.h"
#include "shell/binrun.h"
#include "shell/cmds.h"
#include "shell/term_buf.h"

#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"

#include "io/keyboard.h"
#include "io/ps2.h"

#include "config/config.h"
#include "fs/fs.h"
#include "os/os.h"
#include "os/scheduler.h"

/* ── tunables ─────────────────────────────────────────────── */
#define CLI_INPUT_CAP 256
#define CMD_OUTPUT_MAX 2048
#define HISTORY_DEPTH 32

#define CHAR_W 5
#define LINE_H 8
#define MARGIN_X 4
#define SCROLL_W 6

#define USABLE_W (SCREEN_WIDTH - MARGIN_X - SCROLL_W - 2)
#define COLS (USABLE_W / CHAR_W)
#define INPUT_H (LINE_H + 4)
#define DIVIDER_H 1
#define OUTPUT_H (SCREEN_HEIGHT - INPUT_H - DIVIDER_H)
#define VISIBLE_ROWS (OUTPUT_H / LINE_H)

typedef struct {
    int scroll_top;

    char input[CLI_INPUT_CAP];
    int input_len;
    int input_scroll;

    char history[HISTORY_DEPTH][CLI_INPUT_CAP];
    int hist_count;
    int hist_pos;
    char hist_draft[CLI_INPUT_CAP];
} cli_state_t;

static cli_state_t s;

/* ── helpers ───────────────────────────────────────── */
static void scroll_to_bottom(void) {
    int ms = term_buf_count() - VISIBLE_ROWS;
    s.scroll_top = ms > 0 ? ms : 0;
}
static void scroll_up(int n) {
    s.scroll_top -= n;
    if (s.scroll_top < 0)
        s.scroll_top = 0;
}
static void scroll_down(int n) {
    int ms = term_buf_count() - VISIBLE_ROWS;
    s.scroll_top += n;
    if (s.scroll_top > ms)
        s.scroll_top = ms > 0 ? ms : 0;
}

/* ── drawing ──────────────────────────────────────────────── */
static void draw_scrollbar(void) {
    int sb_x = SCREEN_WIDTH - SCROLL_W - 1;
    fill_rect(sb_x, 0, SCROLL_W, OUTPUT_H, DARK_GRAY);
    draw_rect(sb_x, 0, SCROLL_W, OUTPUT_H, BLACK);

    int total = term_buf_count();
    if (total > VISIBLE_ROWS) {
        int thumb_h = (VISIBLE_ROWS * OUTPUT_H) / total;
        if (thumb_h < 4)
            thumb_h = 4;
        int thumb_range = OUTPUT_H - thumb_h;
        int max_scroll = total - VISIBLE_ROWS;
        int thumb_y = (s.scroll_top * thumb_range) / max_scroll;
        fill_rect(sb_x + 1, thumb_y, SCROLL_W - 2, thumb_h, LIGHT_GRAY);
    }
}

static void draw_output(void) {
    fill_rect(0, 0, SCREEN_WIDTH - SCROLL_W - 1, OUTPUT_H, BLACK);

    for (int row = 0; row < VISIBLE_ROWS; row++) {
        int logical = s.scroll_top + row;
        if (logical >= term_buf_count())
            break;

        const char *line = term_buf_get(logical);
        if (!line)
            continue;

        char buf[TERM_BUF_LINE_W + 1];
        int limit = COLS < TERM_BUF_LINE_W ? COLS : TERM_BUF_LINE_W;
        int ci = 0;
        while (line[ci] && ci < limit) {
            buf[ci] = line[ci];
            ci++;
        }
        buf[ci] = '\0';

        draw_string(MARGIN_X, row * LINE_H, buf, WHITE, 2);
    }
}

static void draw_input(void) {
    int iy = OUTPUT_H + DIVIDER_H;
    fill_rect(0, iy, SCREEN_WIDTH, INPUT_H, BLACK);
    draw_rect(0, iy, SCREEN_WIDTH, INPUT_H, DARK_GRAY);
    draw_line(0, OUTPUT_H, SCREEN_WIDTH - 1, OUTPUT_H, DARK_GRAY);

    char prompt[40];
    const char *drv = g_drive_paths[dir_context.drive_id];
    if (strcmp(dir_context.path, "/") == 0)
        snprintf(prompt, sizeof(prompt), "%s> ", drv);
    else
        snprintf(prompt, sizeof(prompt), "%s%s> ", drv, dir_context.path);

    draw_string(MARGIN_X, iy + 2, prompt, CYAN, 2);

    int prompt_w = (int)strlen(prompt) * CHAR_W;
    int tax = MARGIN_X + prompt_w;
    int taw = SCREEN_WIDTH - tax - SCROLL_W - 4;
    int mvc = taw / CHAR_W;
    if (mvc < 1)
        mvc = 1;

    s.input_scroll = s.input_len - mvc;
    if (s.input_scroll < 0)
        s.input_scroll = 0;

    char iv[CLI_INPUT_CAP];
    int tc = s.input_len - s.input_scroll;
    if (tc > mvc)
        tc = mvc;
    if (tc >= (int)sizeof(iv))
        tc = (int)sizeof(iv) - 1;
    for (int i = 0; i < tc; i++)
        iv[i] = s.input[s.input_scroll + i];
    iv[tc] = '\0';

    draw_string(tax, iy + 2, iv, WHITE, 2);

    extern volatile uint32_t pit_ticks;
    if ((pit_ticks / 50) % 2 == 0) {
        int cx = tax + tc * CHAR_W;
        if (cx < SCREEN_WIDTH - SCROLL_W - 2)
            draw_string(cx, iy + 2, "|", CYAN, 2);
    }
}

static void redraw(void) {
    draw_output();
    draw_scrollbar();
    draw_input();
    blit();
}

/* ── history ──────────────────────────────────────────────── */
static void hist_push(const char *cmd) {
    if (s.hist_count > 0 &&
        strcmp(s.history[(s.hist_count - 1) % HISTORY_DEPTH], cmd) == 0)
        return;
    strncpy(s.history[s.hist_count % HISTORY_DEPTH], cmd, CLI_INPUT_CAP - 1);
    s.history[s.hist_count % HISTORY_DEPTH][CLI_INPUT_CAP - 1] = '\0';
    s.hist_count++;
}

static void hist_load(int pos) {
    const char *e = s.history[pos % HISTORY_DEPTH];
    s.input_len = 0;
    while (e[s.input_len] && s.input_len < CLI_INPUT_CAP - 1) {
        s.input[s.input_len] = e[s.input_len];
        s.input_len++;
    }
    s.input[s.input_len] = '\0';
    s.input_scroll = 0;
}

/* ── execute ──────────────────────────────────────────────── */
static cmd_status_t s_exec_status;

static void execute(void) {
    s_exec_status = CMD_STATUS_OK;

    if (s.input_len > 0) {
        hist_push(s.input);
        s.hist_pos = -1;
        s.hist_draft[0] = '\0';
    }

    if (strcmp(s.input, "exit") == 0) {
        term_buf_push("Exiting CLI...");
        scroll_to_bottom();
        redraw();
        sleep_s(1);
        s_exec_status = CMD_STATUS_EXIT;
        goto done;
    }
    if (strcmp(s.input, "gui") == 0) {
        term_buf_push("Starting GUI...");
        scroll_to_bottom();
        redraw();
        sleep_s(1);
        s_exec_status = CMD_STATUS_GUI;
        goto done;
    }

    {
        static char out_buf[CMD_OUTPUT_MAX];
        out_buf[0] = '\0';
        cmd_status_t st = cli_execute_command(s.input, out_buf, CMD_OUTPUT_MAX);

        if (st == CMD_STATUS_CLEAR) {
            term_buf_clear();
            s.scroll_top = 0;
        } else {
            scroll_to_bottom();
        }
        s_exec_status = st;
    }

done:
    s.input[0] = '\0';
    s.input_len = 0;
    s.input_scroll = 0;
}

void cli_run(void) {
    memset(&s, 0, sizeof(s));
    s.hist_pos = -1;

    clear_screen(BLACK);
    blit();

    term_buf_push("ASMOS CLI v2  |  F5/F6 scroll  |  UP/DOWN history");
    term_buf_push("Type 'help' for a list of commands.");
    term_buf_push("");
    scroll_to_bottom();
    redraw();

    while (1) {
        ps2_update();

        if (!kb.key_pressed) {
            draw_input();
            blit();
            continue;
        }

        uint8_t sc = kb.last_scancode;
        char ch = kb.last_char;

        /* scroll */
        if (sc == F5) {
            scroll_up(VISIBLE_ROWS);
            redraw();
            continue;
        }
        if (sc == F6) {
            scroll_down(VISIBLE_ROWS);
            redraw();
            continue;
        }

        /* history up */
        if (sc == UP_ARROW) {
            if (s.hist_count == 0)
                continue;
            if (s.hist_pos == -1) {
                strncpy(s.hist_draft, s.input, CLI_INPUT_CAP - 1);
                s.hist_draft[CLI_INPUT_CAP - 1] = '\0';
                s.hist_pos = s.hist_count - 1;
            } else if (s.hist_pos > 0) {
                s.hist_pos--;
            }
            hist_load(s.hist_pos);
            redraw();
            continue;
        }

        /* history down */
        if (sc == DOWN_ARROW) {
            if (s.hist_pos == -1) {
                redraw();
                continue;
            }
            if (s.hist_pos < s.hist_count - 1) {
                s.hist_pos++;
                hist_load(s.hist_pos);
            } else {
                s.hist_pos = -1;
                strncpy(s.input, s.hist_draft, CLI_INPUT_CAP - 1);
                s.input[CLI_INPUT_CAP - 1] = '\0';
                s.input_len = (int)strlen(s.input);
                s.input_scroll = 0;
            }
            redraw();
            continue;
        }

        /* enter */
        if (sc == ENTER) {
            execute();
            redraw();
            if (s_exec_status == CMD_STATUS_EXIT ||
                s_exec_status == CMD_STATUS_GUI)
                return;
            continue;
        }

        /* backspace */
        if (sc == BACKSPACE) {
            if (s.input_len > 0) {
                s.input[--s.input_len] = '\0';
                s.hist_pos = -1;
            }
            redraw();
            continue;
        }

        /* printable */
        if (ch >= 32 && ch < 127) {
            if (s.input_len < CLI_INPUT_CAP - 1) {
                s.input[s.input_len++] = ch;
                s.input[s.input_len] = '\0';
                s.hist_pos = -1;
            }
            redraw();
            continue;
        }

        redraw();
    }
}

static void parse_command(const char *input, char *cmd, char *arg) {
    cmd[0] = '\0';
    arg[0] = '\0';
    int i = 0;
    while (input[i] == ' ')
        i++;
    int ci = 0;
    while (input[i] && input[i] != ' ' && ci < 63)
        cmd[ci++] = input[i++];
    cmd[ci] = '\0';
    while (input[i] == ' ')
        i++;
    int ai = 0;
    while (input[i] && ai < CMD_OUTPUT_MAX - 1)
        arg[ai++] = input[i++];
    arg[ai] = '\0';
}

cmd_status_t cli_execute_command(const char *cmd_str, char *out_buffer,
                                 size_t max_len) {
    static char command[64];
    static char argument[CMD_OUTPUT_MAX];
    out_buffer[0] = '\0';
    parse_command(cmd_str, command, argument);

    char el[280];
    snprintf(el, sizeof(el), "> %s", cmd_str);
    term_buf_push(el);

    if (!strcmp(command, "help"))
        cmd_help(out_buffer, max_len);
    else if (!strcmp(command, "clear")) {
        term_buf_clear();
        return CMD_STATUS_CLEAR;
    } else if (!strcmp(command, "pwd"))
        cmd_pwd(out_buffer, max_len);
    else if (!strcmp(command, "cd"))
        cmd_cd(argument, out_buffer, max_len);
    else if (!strcmp(command, "ls"))
        cmd_ls(argument, out_buffer, max_len);
    else if (!strcmp(command, "cat"))
        cmd_cat(argument, out_buffer, max_len);
    else if (!strcmp(command, "touch"))
        cmd_touch(argument, out_buffer, max_len);
    else if (!strcmp(command, "rm"))
        cmd_rm(argument, out_buffer, max_len);
    else if (!strcmp(command, "write"))
        cmd_write(argument, out_buffer, max_len);
    else if (!strcmp(command, "echo"))
        cmd_echo(argument, out_buffer, max_len);
    else if (!strcmp(command, "clock"))
        cmd_clock(out_buffer, max_len);
    else if (!strcmp(command, "shutdown"))
        cmd_shutdown(argument, out_buffer, max_len);
    else if (!strcmp(command, "restart"))
        cmd_restart(argument, out_buffer, max_len);
    else if (!strcmp(command, "df"))
        cmd_df(out_buffer, max_len);
    else if (!strcmp(command, "mem"))
        cmd_mem(out_buffer, max_len);
    else if (!strcmp(command, "sysinfo"))
        cmd_sysinfo(out_buffer, max_len);
    else if (!strcmp(command, "cp"))
        cmd_cp(argument, out_buffer, max_len);
    else if (!strcmp(command, "mv"))
        cmd_mv(argument, out_buffer, max_len);
    else if (!strcmp(command, "mkdir"))
        cmd_mkdir(argument, out_buffer, max_len);
    else if (!strcmp(command, "rmdir"))
        cmd_rmdir(argument, out_buffer, max_len);
    else if (!strcmp(command, "tee"))
        cmd_tee(argument, out_buffer, max_len);
    else if (!strcmp(command, "history"))
        cmd_history(out_buffer, max_len);
    else if (!strcmp(command, "asm"))
        cmd_asmasm(argument, out_buffer, max_len);
    else if (!strcmp(command, "run")) {
        static term_context_t dummy_ctx = {0};
        cmd_run(&dummy_ctx, argument, out_buffer, max_len);
    } else if (command[0] != '\0')
        snprintf(out_buffer, max_len, "Unknown command: %s\n", command);

    if (out_buffer[0])
        term_buf_push_text(out_buffer);

    return CMD_STATUS_OK;
}

#define ASMTERM_INPUT_CAP 256
typedef struct {
    char ring[ASMTERM_INPUT_CAP];
    int head, tail;
    bool enter_pending;
} asmterm_input_t;
static asmterm_input_t s_aterm_input;

void asmterm_input_push(char c) {
    int next = (s_aterm_input.tail + 1) % ASMTERM_INPUT_CAP;
    if (next == s_aterm_input.head)
        return;
    s_aterm_input.ring[s_aterm_input.tail] = c;
    s_aterm_input.tail = next;
}
void asmterm_input_push_enter(void) { s_aterm_input.enter_pending = true; }

static int aterm_input_pop(void) {
    if (s_aterm_input.head == s_aterm_input.tail)
        return -1;
    char c = s_aterm_input.ring[s_aterm_input.head];
    s_aterm_input.head = (s_aterm_input.head + 1) % ASMTERM_INPUT_CAP;
    return (int)(unsigned char)c;
}

#define ASMTERM_OUTPUT_CAP 4096
typedef struct {
    char ring[ASMTERM_OUTPUT_CAP];
    int head, tail;
} asmterm_output_t;
static asmterm_output_t s_aterm_output;

static void aterm_output_push_str(const char *str) {
    while (str && *str) {
        int next = (s_aterm_output.tail + 1) % ASMTERM_OUTPUT_CAP;
        if (next == s_aterm_output.head)
            break;
        s_aterm_output.ring[s_aterm_output.tail] = *str++;
        s_aterm_output.tail = next;
    }
}
int asmterm_output_read(char *dst, int max) {
    int n = 0;
    while (n < max - 1 && s_aterm_output.head != s_aterm_output.tail) {
        dst[n++] = s_aterm_output.ring[s_aterm_output.head];
        s_aterm_output.head = (s_aterm_output.head + 1) % ASMTERM_OUTPUT_CAP;
    }
    dst[n] = '\0';
    return n;
}

static void aterm_ctx_print(term_context_t *ctx, const char *str) {
    (void)ctx;
    if (!str)
        return;
    aterm_output_push_str(str);
    term_buf_push_text(str);
}
static void aterm_ctx_putchar(term_context_t *ctx, char c) {
    (void)ctx;
    char tmp[2] = {c, '\0'};
    aterm_output_push_str(tmp);
}
static int aterm_ctx_readline(term_context_t *ctx, char *buf, int maxchars) {
    (void)ctx;
    if (!buf || maxchars <= 0)
        return 0;
    int len = 0;
    buf[0] = '\0';
    s_aterm_input.enter_pending = false;
    while (1) {
        task_yield();
        int c;
        while ((c = aterm_input_pop()) >= 0) {
            if (c == '\b') {
                if (len > 0) {
                    buf[--len] = '\0';
                    aterm_output_push_str("\b \b");
                }
            } else if (c >= 32 && c < 127 && len < maxchars - 1) {
                buf[len++] = (char)c;
                buf[len] = '\0';
                char echo[2] = {(char)c, '\0'};
                aterm_output_push_str(echo);
            }
        }
        if (s_aterm_input.enter_pending) {
            s_aterm_input.enter_pending = false;
            aterm_output_push_str("\n");
            break;
        }
    }
    return len;
}
static int aterm_ctx_getchar(term_context_t *ctx) {
    (void)ctx;
    while (1) {
        task_yield();
        int c = aterm_input_pop();
        if (c >= 0)
            return c;
        if (s_aterm_input.enter_pending) {
            s_aterm_input.enter_pending = false;
            return '\n';
        }
    }
}

static term_context_t s_aterm_ctx = {
    .print = aterm_ctx_print,
    .putchar = aterm_ctx_putchar,
    .readline = aterm_ctx_readline,
    .getchar = aterm_ctx_getchar,
    .userdata = NULL,
};
term_context_t *cli_asmterm_context(void) { return &s_aterm_ctx; }
