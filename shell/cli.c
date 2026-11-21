#include "shell/cli.h"
#include "config/runtime_config.h"
#include "drivers/fdd.h"
#include "shell/asm.h"
#include "shell/binrun.h"
#include "shell/cmds.h"
#include "shell/term_buf.h"
#include "ui/uifont.h"

#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"

#include "drivers/keyboard.h"
#include "drivers/ps2.h"

#include "config/config.h"
#include "fs/fs.h"
#include "lib/audio.h"
#include "network/net.h"
#include "os/os.h"
#include "os/print.h"
#include "os/scheduler.h"

// tunables
#define CLI_INPUT_CAP 256
#define CMD_OUTPUT_MAX 2048
#define HISTORY_DEPTH 32

#define CHAR_W UI_TERM_CW /* terminal font (ui/uifont.h) */
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

bool shouldexit = 0;
bool g_asmterm_active = false;

/* Output of programs run from the text shell (gopher, run): written a
 * character at a time into a pending line, so a prompt, the echo of what
 * is typed and putchar() output share a line as on a real terminal. */
static char s_pend[TERM_BUF_LINE_W];
static int s_pend_len;
static bool s_dirty; /* the pending line changed: redraw */

static bool tasks_running(void) { return task_count > 1; }
static void pend_putc(char c);
static void aterm_input_reset(void);
static int total_lines(void) { return term_buf_count() + (s_pend_len > 0); }

// helpers
static void scroll_to_bottom(void) {
    int ms = total_lines() - VISIBLE_ROWS;
    s.scroll_top = ms > 0 ? ms : 0;
}
static void scroll_up(int n) {
    s.scroll_top -= n;
    if (s.scroll_top < 0)
        s.scroll_top = 0;
}
static void scroll_down(int n) {
    int ms = total_lines() - VISIBLE_ROWS;
    s.scroll_top += n;
    if (s.scroll_top > ms)
        s.scroll_top = ms > 0 ? ms : 0;
}

// drawing
static void draw_scrollbar(void) {
    int sb_x = SCREEN_WIDTH - SCROLL_W - 1;
    fill_rect(sb_x, 0, SCROLL_W, OUTPUT_H, DARK_GRAY);
    draw_rect(sb_x, 0, SCROLL_W, OUTPUT_H, BLACK);

    int total = total_lines();
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
        if (logical >= total_lines())
            break;

        char pend[TERM_BUF_LINE_W + 1];
        const char *line;
        if (logical < term_buf_count()) {
            line = term_buf_get(logical);
        } else {
            memcpy(pend, s_pend, s_pend_len);
            pend[s_pend_len] = 0;
            line = pend;
        }
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

        draw_string(MARGIN_X, row * LINE_H, buf, WHITE, UI_FONT_TERM);
    }
}

static void draw_input(void) {
    int iy = OUTPUT_H + DIVIDER_H;
    fill_rect(0, iy, SCREEN_WIDTH, INPUT_H, BLACK);
    draw_rect(0, iy, SCREEN_WIDTH, INPUT_H, DARK_GRAY);
    draw_line(0, OUTPUT_H, SCREEN_WIDTH - 1, OUTPUT_H, DARK_GRAY);

    if (g_asmterm_active) {
        draw_string(
            MARGIN_X, iy + 2,
            "gopher: arrows move  Enter open  b back  q quit  Ctrl+C stop",
            YELLOW, UI_FONT_TERM);
        return;
    }
    if (tasks_running()) {
        draw_string(MARGIN_X, iy + 2,
                    "program running: keys go to it, Ctrl+C stops it", YELLOW,
                    UI_FONT_TERM);
        return;
    }

    char prompt[48];
    const char *drv = g_drive_paths[dir_context.drive_id];
    const char *dp = dir_context.path;
    if (strcmp(dp, "/") == 0)
        snprintf(prompt, sizeof(prompt), "%s> ", drv);
    else if (strlen(drv) + strlen(dp) <= 32)
        snprintf(prompt, sizeof(prompt), "%s%s> ", drv, dp);
    else { /* a long path: the drive, "...", the end */
        const char *tail = dp + strlen(dp) - 26;
        const char *sl = strchr(tail, '/');
        snprintf(prompt, sizeof(prompt), "%s/...%s> ", drv, sl ? sl : tail);
    }

    draw_string(MARGIN_X, iy + 2, prompt, CYAN, UI_FONT_TERM);

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

    draw_string(tax, iy + 2, iv, WHITE, UI_FONT_TERM);

    if (g_cfg.caret_no_blink || (pit_ticks / 500) % 2 == 0) {
        int cx = tax + tc * CHAR_W;
        if (cx < SCREEN_WIDTH - SCROLL_W - 2)
            draw_line(cx, iy + 2, cx, iy + 8, CYAN); /* caret */
    }
}

static void redraw(void) {
    draw_output();
    draw_scrollbar();
    draw_input();
    blit();
}

/* tests (the simulator): draw the text shell as it stands */
void cli_debug_redraw(void) { redraw(); }

// history
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

// execute
static cmd_status_t s_exec_status;

static void execute(void) {
    s_exec_status = CMD_STATUS_OK;

    if (s.input_len > 0) {
        hist_push(s.input);
        s.hist_pos = -1;
        s.hist_draft[0] = '\0';
    }

    if (tasks_running() &&
        (!strcmp(s.input, "exit") || !strcmp(s.input, "gui"))) {
        term_buf_push("A program is still running: let it finish first.");
        s.input[0] = '\0';
        s.input_len = 0;
        scroll_to_bottom();
        return;
    }
    if (strcmp(s.input, "exit") == 0) {
        term_buf_push("Exiting CLI...");
        scroll_to_bottom();
        redraw();
        sleep_s(1);
        s_exec_status = CMD_STATUS_EXIT;
        s.input[0] = '\0';
        s.input_len = 0;
        s.input_scroll = 0;
        shouldexit = 1;
    }
    if (strcmp(s.input, "gui") == 0) {
        term_buf_push("Starting GUI...");
        scroll_to_bottom();
        redraw();
        sleep_s(1);
        s_exec_status = CMD_STATUS_GUI;
        s.input[0] = '\0';
        s.input_len = 0;
        s.input_scroll = 0;
        shouldexit = 1;
    }

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

    int last_buf_count = 0, last_blink = -1;

    while (1) {
        if (shouldexit)
            return;
        ps2_update();
        net_poll();
        task_yield(); /* gopher and run execute as tasks */
        print_pump();
        fdd_poll();
        audio_update();
        scheduler_reap();

        if (!kb.key_pressed) {
            int cur_count = term_buf_count();
            int blink = (int)(pit_ticks / 500) & 1;
            if (cur_count != last_buf_count || s_dirty) {
                last_buf_count = cur_count;
                s_dirty = false;
                scroll_to_bottom();
                redraw();
            } else if (blink != last_blink) { /* caret only */
                last_blink = blink;
                draw_input();
                blit();
            }
            continue;
        }
        last_buf_count = term_buf_count();

        uint8_t sc = kb.last_scancode;
        char ch = kb.last_char;

        if (g_asmterm_active || tasks_running()) { /* keys go to the task */
            if (kb.ctrl_c) {                       /* Ctrl+C: stop it */
                kb.ctrl_c = false;
                for (int t = 1; t < MAX_TASKS; t++)
                    if (tasks[t].alive)
                        scheduler_kill_task(t);
                g_asmterm_active = false;
                if (s_pend_len)
                    pend_putc('\n');
                term_buf_push("^C (program stopped)");
                aterm_input_reset();
                scroll_to_bottom();
                redraw();
                continue;
            }
            if (sc == ENTER) {
                asmterm_input_push_enter();
            } else if (sc == BACKSPACE) {
                asmterm_input_push('\b');
            } else if (sc == UP_ARROW || sc == DOWN_ARROW || sc == LEFT_ARROW ||
                       sc == RIGHT_ARROW || sc == PAGE_UP || sc == PAGE_DOWN ||
                       sc == ESC || sc == HOME || sc == END) {
                asmterm_input_push((char)(sc == UP_ARROW      ? TERM_KEY_UP
                                          : sc == DOWN_ARROW  ? TERM_KEY_DOWN
                                          : sc == LEFT_ARROW  ? TERM_KEY_LEFT
                                          : sc == RIGHT_ARROW ? TERM_KEY_RIGHT
                                          : sc == PAGE_UP     ? TERM_KEY_PGUP
                                          : sc == PAGE_DOWN   ? TERM_KEY_PGDN
                                          : sc == HOME        ? TERM_KEY_HOME
                                          : sc == END         ? TERM_KEY_END
                                                              : TERM_KEY_ESC));
            } else if (ch >= 32 && ch < 127) {
                asmterm_input_push(ch);
            }
            continue; /* the task echoes and redraws */
        }

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
            s.input[0] = '\0';
            s.input_len = 0;
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

        if (font_has_char((unsigned char)ch)) {
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
        cmd_help(argument, out_buffer, max_len);
    else if (!strcmp(command, "clear")) {
        term_buf_clear();
        return CMD_STATUS_CLEAR;
    } else if (!strcmp(command, "pwd"))
        cmd_pwd(out_buffer, max_len);
    else if (!strcmp(command, "cd"))
        cmd_cd(argument, out_buffer, max_len);
    else if (!strcmp(command, "ls"))
        cmd_ls(argument, out_buffer, max_len);
    else if (!strcmp(command, "find"))
        cmd_find(argument, out_buffer, max_len);
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
    else if (!strcmp(command, "date"))
        cmd_date(argument, out_buffer, max_len);
    else if (!strcmp(command, "time"))
        cmd_time(argument, out_buffer, max_len);
    else if (!strcmp(command, "cal"))
        cmd_cal(argument, out_buffer, max_len);
    else if (!strcmp(command, "format"))
        cmd_format(argument, out_buffer, max_len);
    else if (!strcmp(command, "sys"))
        cmd_sys(argument, out_buffer, max_len);
    else if (!strcmp(command, "shutdown"))
        cmd_shutdown(argument, out_buffer, max_len);
    else if (!strcmp(command, "reboot"))
        cmd_reboot(argument, out_buffer, max_len);
    else if (!strcmp(command, "df"))
        cmd_df(out_buffer, max_len);
    else if (!strcmp(command, "mem"))
        cmd_mem(out_buffer, max_len);
    else if (!strcmp(command, "sysinfo"))
        cmd_sysinfo(out_buffer, max_len);
    else if (!strcmp(command, "lspci"))
        cmd_lspci(out_buffer, max_len);
    else if (!strcmp(command, "lpt"))
        cmd_lpt(out_buffer, max_len);
    else if (!strcmp(command, "print"))
        cmd_print(argument, out_buffer, max_len);
    else if (!strcmp(command, "play"))
        cmd_play(argument, out_buffer, max_len);
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
    else if (!strcmp(command, "run"))
        cmd_run(cli_asmterm_context(), argument, out_buffer, max_len);
    else if (!strcmp(command, "gopher"))
        cmd_gopher(cli_asmterm_context(), argument, out_buffer, max_len);
    else if (!strcmp(command, "ping"))
        cmd_ping(argument, out_buffer, max_len);
    else if (!strcmp(command, "netconf"))
        cmd_netconf(argument, out_buffer, max_len);
    else if (!strcmp(command, "app"))
        cmd_app(argument, out_buffer, max_len);
    else if (command[0] != '\0')
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
static void aterm_input_reset(void) {
    s_aterm_input.head = s_aterm_input.tail = 0;
    s_aterm_input.enter_pending = false;
}

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

/* one character of task output into the pending line */
static void pend_putc(char c) {
    if (c == '\n') {
        s_pend[s_pend_len] = 0;
        term_buf_push(s_pend);
        s_pend_len = 0;
    } else if (c == '\b') {
        if (s_pend_len > 0)
            s_pend_len--;
    } else if (c == '\r') {
        s_pend_len = 0;
    } else if ((unsigned char)c >= 32) {
        if (s_pend_len >= COLS || s_pend_len >= TERM_BUF_LINE_W - 1)
            pend_putc('\n'); /* wrap */
        s_pend[s_pend_len++] = c;
    }
    s_dirty = true;
}

static void aterm_ctx_print(term_context_t *ctx, const char *str) {
    (void)ctx;
    if (!str)
        return;
    aterm_output_push_str(str);
    while (*str)
        pend_putc(*str++);
}
static void aterm_ctx_putchar(term_context_t *ctx, char c) {
    (void)ctx;
    char tmp[2] = {c, '\0'};
    aterm_output_push_str(tmp);
    pend_putc(c);
}
static void aterm_ctx_clear(term_context_t *ctx) {
    (void)ctx;
    term_buf_clear();
    s_pend_len = 0;
    s.scroll_top = 0;
    s_dirty = true;
}
static void aterm_ctx_size(term_context_t *ctx, int *cols, int *rows) {
    (void)ctx;
    *cols = COLS;
    *rows = VISIBLE_ROWS;
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
                    pend_putc('\b');
                }
            } else if (c >= 32 && c < 127 && len < maxchars - 1) {
                buf[len++] = (char)c;
                buf[len] = '\0';
                char echo[2] = {(char)c, '\0'};
                aterm_output_push_str(echo);
                pend_putc((char)c);
            }
        }
        if (s_aterm_input.enter_pending) {
            s_aterm_input.enter_pending = false;
            aterm_output_push_str("\n");
            pend_putc('\n');
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
    .clear = aterm_ctx_clear,
    .size = aterm_ctx_size,
};

term_context_t *cli_asmterm_context(void) { return &s_aterm_ctx; }
