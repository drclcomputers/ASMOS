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

#define CLI_BUFFER_SIZE 256
#define CHAR_SPACING 5
#define CMD_OUTPUT_MAX 2048

typedef struct {
    char buffer[CLI_BUFFER_SIZE];
    int buffer_pos;
    int cursor_x;
    int cursor_y;
    int line_height;
} cli_state_t;

static cli_state_t s_cli = {.line_height = 7};

static void cli_draw_char(char c) {
    int lh = s_cli.line_height;
    if (c == '\n') {
        s_cli.cursor_x = 0;
        s_cli.cursor_y += lh;
        if (s_cli.cursor_y + 6 >= SCREEN_HEIGHT) {
            uint8_t *buf = (uint8_t *)BACKBUF;
            memmove(buf + MENUBAR_H * SCREEN_WIDTH,
                    buf + (MENUBAR_H + lh) * SCREEN_WIDTH,
                    (SCREEN_HEIGHT - MENUBAR_H - lh) * SCREEN_WIDTH);
            memset(buf + (SCREEN_HEIGHT - lh) * SCREEN_WIDTH, BLACK,
                   lh * SCREEN_WIDTH);
            s_cli.cursor_y = SCREEN_HEIGHT - lh * 2;
        }
        return;
    }
    if (c < 32 || c > 126)
        return;
    draw_char(s_cli.cursor_x, s_cli.cursor_y, c, WHITE, 2);
    s_cli.cursor_x += CHAR_SPACING;
    if (s_cli.cursor_x >= SCREEN_WIDTH - CHAR_SPACING)
        cli_draw_char('\n');
}

static void cli_print(const char *str) {
    while (str && *str)
        cli_draw_char(*str++);
    blit();
}

static void cli_println(const char *str) {
    cli_print(str);
    cli_draw_char('\n');
    blit();
}

static void shell_draw_prompt(void) {
    char pwd[256];
    if (fs_pwd(pwd, sizeof(pwd)))
        cli_print(pwd);
    cli_print("> ");
}

static void cli_ctx_print(term_context_t *ctx, const char *str) {
    (void)ctx;
    cli_print(str);
    term_buf_push_text(str);
}

static void cli_ctx_putchar(term_context_t *ctx, char c) {
    (void)ctx;
    cli_draw_char(c);
    blit();
}

static int cli_ctx_readline(term_context_t *ctx, char *buf, int maxchars) {
    (void)ctx;
    if (!buf || maxchars <= 0)
        return 0;
    int len = 0;
    buf[0] = '\0';
    while (1) {
        ps2_update();
        if (!kb.key_pressed)
            continue;
        uint8_t sc = kb.last_scancode;
        char ch = kb.last_char;
        if (sc == ENTER) {
            cli_draw_char('\n');
            blit();
            break;
        }
        if (sc == BACKSPACE) {
            if (len > 0) {
                buf[--len] = '\0';
                s_cli.cursor_x -= CHAR_SPACING;
                if (s_cli.cursor_x < 0)
                    s_cli.cursor_x = 0;
                draw_char(s_cli.cursor_x, s_cli.cursor_y, ' ', BLACK, 2);
                blit();
            }
            continue;
        }
        if (ch >= 32 && ch < 127 && len < maxchars - 1) {
            buf[len++] = ch;
            buf[len] = '\0';
            cli_draw_char(ch);
            blit();
        }
    }
    return len;
}

static int cli_ctx_getchar(term_context_t *ctx) {
    (void)ctx;
    while (1) {
        ps2_update();
        if (!kb.key_pressed)
            continue;
        char ch = kb.last_char;
        if (ch >= 32 && ch < 127)
            return (int)(unsigned char)ch;
        if (kb.last_scancode == ENTER)
            return '\n';
        if (kb.last_scancode == BACKSPACE)
            return '\b';
    }
}

static term_context_t s_cli_ctx = {
    .print = cli_ctx_print,
    .putchar = cli_ctx_putchar,
    .readline = cli_ctx_readline,
    .getchar = cli_ctx_getchar,
    .userdata = NULL,
};

#define ASMTERM_INPUT_CAP 256

typedef struct {
    char ring[ASMTERM_INPUT_CAP];
    int head, tail;
    bool enter_pending;
} asmterm_input_t;

static asmterm_input_t s_aterm_input = {0};

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

static void aterm_input_reset(void) {
    s_aterm_input.head = s_aterm_input.tail = 0;
    s_aterm_input.enter_pending = false;
}

#define ASMTERM_OUTPUT_CAP 4096

typedef struct {
    char ring[ASMTERM_OUTPUT_CAP];
    int head, tail;
} asmterm_output_t;

static asmterm_output_t s_aterm_output = {0};

static void aterm_output_push_str(const char *s) {
    while (s && *s) {
        int next = (s_aterm_output.tail + 1) % ASMTERM_OUTPUT_CAP;
        if (next == s_aterm_output.head)
            break;
        s_aterm_output.ring[s_aterm_output.tail] = *s++;
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

cmd_status_t cli_execute_command(const char *cmd, char *out_buffer,
                                 size_t max_len) {
    static char command[64];
    static char argument[CMD_OUTPUT_MAX];
    out_buffer[0] = '\0';
    parse_command(cmd, command, argument);

    {
        char el[256];
        sprintf(el, "> %s", cmd);
        term_buf_push(el);
    }

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
        cmd_ls(out_buffer, max_len);
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
    else if (!strcmp(command, "df"))
        cmd_df(out_buffer, max_len);
    else if (!strcmp(command, "mem"))
        cmd_mem(out_buffer, max_len);
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
        cmd_run(&s_aterm_ctx, argument, out_buffer, max_len);
    else if (!strcmp(command, "gui")) {
        sprintf(out_buffer, "Starting GUI...\n\n");
        sleep_s(1);
        return CMD_STATUS_GUI;
    } else if (!strcmp(command, "exit")) {
        sprintf(out_buffer, "Exiting...\n");
        sleep_s(1);
        return CMD_STATUS_EXIT;
    } else if (command[0] != '\0') {
        sprintf(out_buffer, "Unknown command: %s\n\n", command);
    }

    if (out_buffer[0])
        term_buf_push_text(out_buffer);
    return CMD_STATUS_OK;
}

void cli_run(void) {
    clear_screen(BLACK);
    s_cli.cursor_x = 0;
    s_cli.cursor_y = 0;
    blit();
    cli_println("ASMOS CLI");
    cli_println("Type 'help' for commands.");
    cli_draw_char('\n');
    blit();

    static char out_buf[CMD_OUTPUT_MAX];

    while (1) {
        shell_draw_prompt();
        s_cli.buffer_pos = 0;
        s_cli.buffer[0] = '\0';

        while (1) {
            ps2_update();
            if (!kb.key_pressed)
                continue;

            if (kb.last_scancode == ENTER) {
                cli_draw_char('\n');
                blit();
                cmd_status_t status =
                    cli_execute_command(s_cli.buffer, out_buf, CMD_OUTPUT_MAX);
                if (status == CMD_STATUS_EXIT || status == CMD_STATUS_GUI) {
                    cli_print(out_buf);
                    return;
                } else if (status == CMD_STATUS_CLEAR) {
                    clear_screen(BLACK);
                    s_cli.cursor_x = 0;
                    s_cli.cursor_y = 0;
                    blit();
                } else {
                    cli_print(out_buf);
                }
                break;
            }
            if (kb.last_scancode == BACKSPACE) {
                if (s_cli.buffer_pos > 0) {
                    s_cli.buffer[--s_cli.buffer_pos] = '\0';
                    s_cli.cursor_x -= CHAR_SPACING;
                    if (s_cli.cursor_x < 0)
                        s_cli.cursor_x = 0;
                    draw_char(s_cli.cursor_x, s_cli.cursor_y, ' ', BLACK, 2);
                    blit();
                }
                continue;
            }
            if (kb.last_char >= 32 && kb.last_char < 127) {
                if (s_cli.buffer_pos < CLI_BUFFER_SIZE - 1) {
                    s_cli.buffer[s_cli.buffer_pos++] = kb.last_char;
                    s_cli.buffer[s_cli.buffer_pos] = '\0';
                    cli_draw_char(kb.last_char);
                    blit();
                }
            }
        }
    }
}
