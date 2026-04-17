#include "shell/cli.h"

#include "lib/string.h"
#include "lib/memory.h"
#include "lib/graphics.h"
#include "lib/time.h"

#include "io/keyboard.h"
#include "io/ps2.h"

#include "fs/fat16.h"
#include "os/os.h"
#include "config/config.h"

#define CLI_BUFFER_SIZE 256
#define CHAR_WIDTH      4
#define CHAR_HEIGHT     6
#define CHAR_SPACING    5
#define CMD_OUTPUT_MAX  1024

typedef struct {
    char buffer[CLI_BUFFER_SIZE];
    int  buffer_pos;
    int  cursor_x;
    int  cursor_y;
    int  line_height;
} cli_state_t;

static cli_state_t cli = {
    .buffer_pos  = 0,
    .cursor_x    = 0,
    .cursor_y    = 0,
    .line_height = 7
};

typedef struct {
    char lines[TERM_BUF_LINES][TERM_BUF_LINE_W];
    int  head;
    int  count;
} term_ring_t;

static term_ring_t s_ring = { .head = 0, .count = 0 };

void term_buf_push(const char *line) {
    if (!line) return;

    int slot;
    if (s_ring.count < TERM_BUF_LINES) {
        slot = (s_ring.head + s_ring.count) % TERM_BUF_LINES;
        s_ring.count++;
    } else {
        slot       = s_ring.head;
        s_ring.head = (s_ring.head + 1) % TERM_BUF_LINES;
    }

    strncpy(s_ring.lines[slot], line, TERM_BUF_LINE_W - 1);
    s_ring.lines[slot][TERM_BUF_LINE_W - 1] = '\0';
}

void term_buf_push_text(const char *text) {
    if (!text) return;
    char line[TERM_BUF_LINE_W];
    int li = 0;
    for (const char *p = text; ; p++) {
        if (*p == '\n' || *p == '\0') {
            line[li] = '\0';
            if (li > 0) term_buf_push(line);
            li = 0;
            if (*p == '\0') break;
        } else if (li < TERM_BUF_LINE_W - 1) {
            line[li++] = *p;
        }
    }
}

int term_buf_count(void) {
    return s_ring.count;
}

const char *term_buf_get(int i) {
    if (i < 0 || i >= s_ring.count) return NULL;
    int slot = (s_ring.head + i) % TERM_BUF_LINES;
    return s_ring.lines[slot];
}

void term_buf_clear(void) {
    s_ring.head  = 0;
    s_ring.count = 0;
}

int term_buf_read_all(char *dst, int max) {
    if (!dst || max <= 0) return 0;
    int written = 0;
    for (int i = 0; i < s_ring.count && written < max - 1; i++) {
        const char *line = term_buf_get(i);
        if (!line) continue;
        int len = (int)strlen(line);
        int space = max - 1 - written;
        if (len > space) len = space;
        memcpy(dst + written, line, len);
        written += len;
        if (written < max - 1) {
            dst[written++] = '\n';
        }
    }
    dst[written] = '\0';
    return written;
}

int term_buf_read_new(char *dst, int max, int *cursor) {
    if (!dst || max <= 0 || !cursor) return 0;
    int written = 0;
    while (*cursor < s_ring.count && written < max - 1) {
        const char *line = term_buf_get(*cursor);
        if (line) {
            int len = (int)strlen(line);
            int space = max - 1 - written;
            if (len > space) len = space;
            memcpy(dst + written, line, len);
            written += len;
            if (written < max - 1) dst[written++] = '\n';
        }
        (*cursor)++;
    }
    dst[written] = '\0';
    return written;
}

bool term_buf_save(const char *path) {
    if (!path) return false;
    dir_entry_t de;
    if (fat16_find(path, &de)) fat16_delete(path);
    fat16_file_t f;
    if (!fat16_create(path, &f)) return false;
    char line_buf[TERM_BUF_LINE_W + 1];
    bool ok = true;
    for (int i = 0; i < s_ring.count; i++) {
        const char *line = term_buf_get(i);
        if (!line) continue;
        int len = (int)strlen(line);
        strncpy(line_buf, line, TERM_BUF_LINE_W);
        line_buf[len] = '\n';
        if (fat16_write(&f, line_buf, len + 1) != len + 1) { ok = false; break; }
    }
    fat16_close(&f);
    return ok;
}

static void append_output(char *buffer, size_t max_len, const char *text) {
    size_t current_len = strlen(buffer);
    size_t text_len = strlen(text);

    if (current_len + text_len + 1 < max_len) {
        strcpy(buffer + current_len, text);
    }
}

static void shell_draw_text(const char *str) {
    while (*str) {
        if (*str == '\n') {
            cli.cursor_y += cli.line_height;
            cli.cursor_x = 0;

            if (cli.cursor_y + CHAR_HEIGHT >= SCREEN_HEIGHT) {
                clear_screen(BLACK);
                cli.cursor_y = 0;
            }
        } else {
            draw_char(cli.cursor_x, cli.cursor_y, *str, WHITE, 0);
            cli.cursor_x += CHAR_SPACING;

            if (cli.cursor_x >= SCREEN_WIDTH - CHAR_SPACING) {
                cli.cursor_y += cli.line_height;
                cli.cursor_x = 0;
            }
        }
        str++;
    }
    blit();
}

static void shell_draw_prompt(void) {
    char pwd[256];
    if (fat16_pwd(pwd, sizeof(pwd))) {
        shell_draw_text(pwd);
    }
    shell_draw_text("> ");
}


void cmd_help(char *out, size_t max) {
    append_output(out, max, "Available commands:\n");
    append_output(out, max, "help         - Show this message\n");
    append_output(out, max, "clear        - Clear screen\n");
    append_output(out, max, "pwd          - Print current working directory\n");
    append_output(out, max, "cd <d>       - Change directory\n");
    append_output(out, max, "ls           - List files and dirs\n");
    append_output(out, max, "cat <f>      - Print file contents\n");
    append_output(out, max, "touch <f>    - Create empty file\n");
    append_output(out, max, "rm <f>       - Delete file\n");
    append_output(out, max, "rm -r <d>    - Delete directory recursively\n");
    append_output(out, max, "write <f> t  - Write text to file\n");
    append_output(out, max, "echo <t>     - Print text\n");
    append_output(out, max, "cp <s> <d>   - Copy file or directory\n");
    append_output(out, max, "mv <s> <d>   - Move/rename file or directory\n");
    append_output(out, max, "mkdir <d>    - Create directory\n");
    append_output(out, max, "rmdir <d>    - Remove empty directory\n");
    append_output(out, max, "df           - Show disk usage\n");
    append_output(out, max, "mem          - Show memory usage\n");
    append_output(out, max, "clock        - Show system time\n");
    append_output(out, max, "tee <f>      - Save terminal buffer to file\n");
    append_output(out, max, "history      - Show command history from buffer\n");
    append_output(out, max, "gui          - Start the GUI\n");
    append_output(out, max, "exit         - Exit CLI\n\n");
}

void cmd_pwd(char *out, size_t max) {
    char pwd[256];
    if (fat16_pwd(pwd, sizeof(pwd))) {
        append_output(out, max, pwd);
        append_output(out, max, "\n\n");
    }
}

void cmd_cd(const char *path, char *out, size_t max) {
    if (!path || path[0] == '\0') {
        append_output(out, max, "Usage: cd <path>\n\n");
        return;
    }
    if (!fat16_chdir(path)) {
        append_output(out, max, "Error: directory not found\n\n");
        return;
    }
}

void cmd_ls(char *out, size_t max) {
    dir_entry_t entries[32];
    int count = 0;

    if (!fat16_list_dir(dir_context.current_cluster, entries, 32, &count)) {
        append_output(out, max, "Error listing files\n\n");
        return;
    }

    append_output(out, max, "Files:\n");
    for (int i = 0; i < count; i++) {
        char name_buffer[16];
        int j = 0;

        for (int k = 0; k < 8 && entries[i].name[k] != ' '; k++) {
            name_buffer[j++] = entries[i].name[k];
        }

        if (entries[i].ext[0] != ' ') {
            name_buffer[j++] = '.';
            for (int k = 0; k < 3 && entries[i].ext[k] != ' '; k++) {
                name_buffer[j++] = entries[i].ext[k];
            }
        }
        name_buffer[j] = '\0';

        append_output(out, max, "  ");
        append_output(out, max, name_buffer);

        if (entries[i].attr & ATTR_DIRECTORY) {
            append_output(out, max, " [DIR]\n");
        } else {
            char size_str[32];
            sprintf(size_str, " (%db)\n", entries[i].file_size);
            append_output(out, max, size_str);
        }
    }
    append_output(out, max, "\n");
}

void cmd_cat(const char *filename, char *out, size_t max) {
    if (!filename || filename[0] == '\0') {
        append_output(out, max, "Usage: cat <filename>\n\n");
        return;
    }

    fat16_file_t file;
    if (!fat16_open(filename, &file)) {
        append_output(out, max, "Error: file not found\n\n");
        return;
    }

    char buffer[512];
    char temp[513];
    int bytes_read;
    while ((bytes_read = fat16_read(&file, buffer, 512)) > 0) {
        int t_idx = 0;
        for (int i = 0; i < bytes_read; i++) {
            if (buffer[i] == '\n' || (buffer[i] >= 32 && buffer[i] < 127)) {
                temp[t_idx++] = buffer[i];
            }
        }
        temp[t_idx] = '\0';
        append_output(out, max, temp);
    }

    fat16_close(&file);
    append_output(out, max, "\n\n");
}

void cmd_write(const char *args, char *out, size_t max) {
    if (!args || args[0] == '\0') {
        append_output(out, max, "Usage: write <file> <text>\n\n");
        return;
    }

    int i = 0;
    while (args[i] == ' ') i++;

    int fname_start = i;
    while (args[i] != ' ' && args[i] != '\0') i++;

    if (i == fname_start) {
        append_output(out, max, "Usage: write <file> <text>\n\n");
        return;
    }

    char filename[64];
    int fname_len = i - fname_start;
    if (fname_len >= 64) fname_len = 63;
    memcpy(filename, &args[fname_start], fname_len);
    filename[fname_len] = '\0';

    while (args[i] == ' ') i++;
    if (args[i] == '\0') {
        append_output(out, max, "Usage: write <file> <text>\n\n");
        return;
    }

    fat16_file_t file;
    dir_entry_t entry;
    if (fat16_find(filename, &entry)) {
        if (!fat16_open(filename, &file)) {
            append_output(out, max, "Error: cannot open file\n\n");
            return;
        }
    } else {
        if (!fat16_create(filename, &file)) {
            append_output(out, max, "Error: cannot create file\n\n");
            return;
        }
    }

    int text_len = 0;
    while (args[i + text_len] != '\0') text_len++;

    int written = fat16_write(&file, &args[i], text_len);
    fat16_close(&file);

    char temp[64];
    sprintf(temp, "Wrote %d bytes\n\n", written);
    append_output(out, max, temp);
}

void cmd_touch(const char *filename, char *out, size_t max) {
    if (!filename || filename[0] == '\0') {
        append_output(out, max, "Usage: touch <filename>\n\n");
        return;
    }

    fat16_file_t file;
    if (!fat16_create(filename, &file)) {
        append_output(out, max, "Error: file already exists or invalid path\n\n");
        return;
    }
    fat16_close(&file);

    append_output(out, max, "Created: ");
    append_output(out, max, filename);
    append_output(out, max, "\n\n");
}

void cmd_rm(const char *args, char *out, size_t max) {
    if (!args || args[0] == '\0') {
        append_output(out, max, "Usage: rm [-r] <filename>\n\n");
        return;
    }

    if (strncmp(args, "-r ", 3) == 0) {
        if (!fat16_rm_rf(args + 3)) {
            append_output(out, max, "Error: dir not found or delete failed\n\n");
            return;
        }
        append_output(out, max, "Deleted (recursive): ");
        append_output(out, max, args + 3);
        append_output(out, max, "\n\n");
    } else {
        if (!fat16_delete(args)) {
            append_output(out, max, "Error: file not found\n\n");
            return;
        }
        append_output(out, max, "Deleted: ");
        append_output(out, max, args);
        append_output(out, max, "\n\n");
    }
}

void cmd_mkdir(const char *dirname, char *out, size_t max) {
    if (!dirname || dirname[0] == '\0') {
        append_output(out, max, "Usage: mkdir <dirname>\n\n");
        return;
    }

    if (!fat16_mkdir(dirname)) {
        append_output(out, max, "Error: could not create '");
        append_output(out, max, dirname);
        append_output(out, max, "' (may already exist)\n\n");
        return;
    }

    append_output(out, max, "Created directory: ");
    append_output(out, max, dirname);
    append_output(out, max, "\n\n");
}

void cmd_rmdir(const char *dirname, char *out, size_t max) {
    if (!dirname || dirname[0] == '\0') {
        append_output(out, max, "Usage: rmdir <dirname>\n\n");
        return;
    }

    if (!fat16_rmdir(dirname)) {
        append_output(out, max, "Error: '");
        append_output(out, max, dirname);
        append_output(out, max, "' not found, not a dir, or not empty\n\n");
        return;
    }
    append_output(out, max, "Removed directory: ");
    append_output(out, max, dirname);
    append_output(out, max, "\n\n");
}

void cmd_cp(const char *args, char *out, size_t max) {
    if (!args || args[0] == '\0') {
        append_output(out, max, "Usage: cp <src> <dest>\n\n");
        return;
    }

    int i = 0;
    while (args[i] == ' ') i++;
    int src_start = i;
    while (args[i] != ' ' && args[i] != '\0') i++;
    int src_len = i - src_start;

    while (args[i] == ' ') i++;
    int dest_start = i;
    while (args[i] != ' ' && args[i] != '\0') i++;
    int dest_len = i - dest_start;

    if (src_len == 0 || dest_len == 0) {
        append_output(out, max, "Usage: cp <src> <dest>\n\n");
        return;
    }

    char src[64], dest[64];
    if (src_len >= 64) src_len = 63;
    if (dest_len >= 64) dest_len = 63;
    memcpy(src, &args[src_start], src_len); src[src_len] = '\0';
    memcpy(dest, &args[dest_start], dest_len); dest[dest_len] = '\0';

    dir_entry_t entry;
    if (!fat16_find(src, &entry)) {
        append_output(out, max, "Error: source not found\n\n");
        return;
    }

    bool ok = (entry.attr & ATTR_DIRECTORY) ? fat16_copy_dir(src, dest) : fat16_copy_file(src, dest);
    if (ok) {
        char temp[150];
        sprintf(temp, "Copied: %s -> %s\n\n", src, dest);
        append_output(out, max, temp);
    } else {
        append_output(out, max, "Error: copy failed\n\n");
    }
}

void cmd_mv(const char *args, char *out, size_t max) {
    if (!args || args[0] == '\0') {
        append_output(out, max, "Usage: mv <src> <dest>\n\n");
        return;
    }

    int i = 0;
    while (args[i] == ' ') i++;
    int src_start = i;
    while (args[i] != ' ' && args[i] != '\0') i++;
    int src_len = i - src_start;

    while (args[i] == ' ') i++;
    int dest_start = i;
    while (args[i] != ' ' && args[i] != '\0') i++;
    int dest_len = i - dest_start;

    if (src_len == 0 || dest_len == 0) {
        append_output(out, max, "Usage: mv <src> <dest>\n\n");
        return;
    }

    char src[64], dest[64];
    if (src_len >= 64) src_len = 63;
    if (dest_len >= 64) dest_len = 63;
    memcpy(src, &args[src_start], src_len); src[src_len] = '\0';
    memcpy(dest, &args[dest_start], dest_len); dest[dest_len] = '\0';

    dir_entry_t entry;
    if (!fat16_find(src, &entry)) {
        append_output(out, max, "Error: source not found\n\n");
        return;
    }

    bool ok = (entry.attr & ATTR_DIRECTORY) ? fat16_move_dir(src, dest) : fat16_move_file(src, dest);
    if (ok) {
        char temp[150];
        sprintf(temp, "Moved: %s -> %s\n\n", src, dest);
        append_output(out, max, temp);
    } else {
        append_output(out, max, "Error: move failed\n\n");
    }
}

void cmd_df(char *out, size_t max) {
    uint32_t total_bytes = 0, used_bytes = 0;
    if (!fat16_get_usage(&total_bytes, &used_bytes)) {
        append_output(out, max, "Error reading storage info\n\n");
        return;
    }

    uint32_t total_b = total_bytes;
    uint32_t used_b = used_bytes;
    uint32_t free_b = (total_bytes - used_bytes);

    char buf[128];
    sprintf(buf, "Storage Total: %u B\nStorage Used:  %u b\nStorage Free:  %u B\n\n", total_b, used_b, free_b);
    append_output(out, max, buf);
}

void cmd_mem(char *out, size_t max) {
    char buf[128];
    sprintf(buf, "Heap used:  %u B\nHeap free:  %u B\n\n", heap_used() / 1024, heap_remaining() / 1024);
    append_output(out, max, buf);
}

void cmd_echo(const char *text, char *out, size_t max) {
    if (!text || text[0] == '\0') {
        append_output(out, max, "\n");
        return;
    }
    append_output(out, max, text);
    append_output(out, max, "\n\n");
}

void cmd_clock(char *out, size_t max) {
    time_full_t t = time_rtc();
    char buf[256];
    sprintf(buf, "%d-%d-%d / %d:%d:%d\n\n", t.year, t.month, t.day, t.hours, t.minutes, t.seconds);
    append_output(out, max, buf);
}

static void cmd_tee(const char *filename, char *out, size_t max) {
    if (!filename || filename[0] == '\0') {
        append_output(out, max, "Usage: tee <filename>\n\n");
        return;
    }
    if (term_buf_save(filename)) {
        char tmp[128];
        sprintf(tmp, "Saved %d lines to %s\n\n", term_buf_count(), filename);
        append_output(out, max, tmp);
    } else {
        append_output(out, max, "Error: could not save buffer\n\n");
    }
}

static void cmd_history(char *out, size_t max) {
    int n = term_buf_count();
    if (n == 0) { append_output(out, max, "(empty)\n\n"); return; }
    char tmp[16];
    sprintf(tmp, "%d lines:\n", n);
    append_output(out, max, tmp);
    int start = n > 20 ? n - 20 : 0;
    for (int i = start; i < n; i++) {
        const char *line = term_buf_get(i);
        if (line) {
            append_output(out, max, line);
            append_output(out, max, "\n");
        }
    }
    append_output(out, max, "\n");
}

static void parse_command(const char *input, char *cmd, char *arg) {
    cmd[0] = '\0'; arg[0] = '\0';
    int i = 0;
    while (input[i] == ' ') i++;
    int cmd_idx = 0;
    while (input[i] != '\0' && input[i] != ' ' && cmd_idx < 63) {
        cmd[cmd_idx++] = input[i++];
    }
    cmd[cmd_idx] = '\0';
    while (input[i] == ' ') i++;
    int arg_idx = 0;
    while (input[i] != '\0' && arg_idx < 255) {
        arg[arg_idx++] = input[i++];
    }
    arg[arg_idx] = '\0';
}

cmd_status_t cli_execute_command(const char *cmd, char *out_buffer, size_t max_len) {
    char command[64];
    char argument[1024-64-2];

    out_buffer[0] = '\0';
    parse_command(cmd, command, argument);

    {
        char echo_line[256];
        sprintf(echo_line, "> %s", cmd);
        term_buf_push(echo_line);
    }

    if (strcmp(command, "help") == 0) {
        cmd_help(out_buffer, max_len);
    } else if (strcmp(command, "clear") == 0) {
        term_buf_clear();
        return CMD_STATUS_CLEAR;
    } else if (strcmp(command, "pwd") == 0) {
        cmd_pwd(out_buffer, max_len);
    } else if (strcmp(command, "cd") == 0) {
        cmd_cd(argument, out_buffer, max_len);
    } else if (strcmp(command, "ls") == 0) {
        cmd_ls(out_buffer, max_len);
    } else if (strcmp(command, "cat") == 0) {
        cmd_cat(argument, out_buffer, max_len);
    } else if (strcmp(command, "touch") == 0) {
        cmd_touch(argument, out_buffer, max_len);
    } else if (strcmp(command, "rm") == 0) {
        cmd_rm(argument, out_buffer, max_len);
    } else if (strcmp(command, "write") == 0) {
        cmd_write(argument, out_buffer, max_len);
    } else if (strcmp(command, "echo") == 0) {
        cmd_echo(argument, out_buffer, max_len);
    } else if (strcmp(command, "clock") == 0) {
        cmd_clock(out_buffer, max_len);
    } else if (strcmp(command, "df") == 0) {
        cmd_df(out_buffer, max_len);
    } else if (strcmp(command, "mem") == 0) {
        cmd_mem(out_buffer, max_len);
    } else if (strcmp(command, "cp") == 0) {
        cmd_cp(argument, out_buffer, max_len);
    } else if (strcmp(command, "mv") == 0) {
        cmd_mv(argument, out_buffer, max_len);
    } else if (strcmp(command, "mkdir") == 0) {
        cmd_mkdir(argument, out_buffer, max_len);
    } else if (strcmp(command, "rmdir") == 0) {
        cmd_rmdir(argument, out_buffer, max_len);
    } else if (strcmp(command, "tee") == 0) {
        cmd_tee(argument, out_buffer, max_len);
    } else if (strcmp(command, "history") == 0) {
        cmd_history(out_buffer, max_len);
    } else if (strcmp(command, "gui") == 0) {
        append_output(out_buffer, max_len, "Starting GUI...\n\n");
        sleep_s(1);
        return CMD_STATUS_GUI;
    } else if (strcmp(command, "exit") == 0) {
        append_output(out_buffer, max_len, "Exiting...\n");
        sleep_s(1);
        return CMD_STATUS_EXIT;
    } else if (command[0] != '\0') {
        append_output(out_buffer, max_len, "Unknown command: ");
        append_output(out_buffer, max_len, command);
        append_output(out_buffer, max_len, "\n\n");
    }

    if (out_buffer[0]) term_buf_push_text(out_buffer);

    return CMD_STATUS_OK;
}

void cli_run(void) {
    clear_screen(BLACK);
    cli.cursor_x = 0;
    cli.cursor_y = 0;
    blit();

    shell_draw_text("ASMOS CLI\n");
    shell_draw_text("Type 'help' for available commands\n\n");

    char output_buffer[CMD_OUTPUT_MAX];

    while (1) {
        shell_draw_prompt();
        cli.buffer_pos = 0;
        cli.buffer[0] = '\0';

        while (1) {
            ps2_update();
            if (kb.key_pressed) {
                if (kb.last_scancode == ENTER) {
                    shell_draw_text("\n\n");

                    cmd_status_t status = cli_execute_command(cli.buffer, output_buffer, CMD_OUTPUT_MAX);

                    if (status == CMD_STATUS_EXIT || status == CMD_STATUS_GUI) {
                        shell_draw_text(output_buffer);
                        return;
                    } else if (status == CMD_STATUS_CLEAR) {
                        clear_screen(BLACK);
                        cli.cursor_x = 0;
                        cli.cursor_y = 0;
                        blit();
                    } else {
                        shell_draw_text(output_buffer);
                    }
                    break;
                } else if (kb.last_scancode == BACKSPACE) {
                    if (cli.buffer_pos > 0) {
                        cli.buffer_pos--;
                        cli.buffer[cli.buffer_pos] = '\0';
                        cli.cursor_x -= CHAR_SPACING;
                        fill_rect(cli.cursor_x, cli.cursor_y, CHAR_SPACING, CHAR_HEIGHT, BLACK);
                        blit();
                    }
                } else if (kb.last_char >= 32 && kb.last_char < 127) {
                    if (cli.buffer_pos < CLI_BUFFER_SIZE - 1) {
                        cli.buffer[cli.buffer_pos++] = kb.last_char;
                        cli.buffer[cli.buffer_pos] = '\0';
                        char c[2] = {kb.last_char, '\0'};
                        shell_draw_text(c);
                    }
                }
            }
        }
    }
}
