#include "shell/cli.h"
#include "lib/string.h"
#include "lib/mem.h"
#include "lib/alloc.h"
#include "lib/primitive_graphics.h"
#include "io/keyboard.h"
#include "io/ps2.h"
#include "fs/fat16.h"
#include "os/os.h"
#include "config/config.h"
#include "lib/time.h"

#define CLI_BUFFER_SIZE 256
#define CHAR_WIDTH       4
#define CHAR_HEIGHT      6
#define CHAR_SPACING     5

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

//UI
static void cli_print(const char *str) {
    while (*str) {
        if (*str == '\n') {
            cli.cursor_y += cli.line_height;
            cli.cursor_x = 0;

            if (cli.cursor_y + CHAR_HEIGHT >= SCREEN_HEIGHT) {
                clear_screen(0x00);
                cli.cursor_y = 0;
            }
        } else {
            draw_char(cli.cursor_x, cli.cursor_y, *str, 0x0F, 0);
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

static void cli_print_prompt(void) {
    char pwd[256];
    if (fat16_pwd(pwd, sizeof(pwd))) {
        cli_print(pwd);
    }
    cli_print("> ");
}

// Core cmds
void cmd_clear(void) {
    clear_screen(0x00);
    cli.cursor_x = 0;
    cli.cursor_y = 0;
    blit();
}

void cmd_help(void) {
    cli_print("Available commands:\n");
    cli_print("help         - Show this message\n");
    cli_print("clear        - Clear screen\n");
    cli_print("pwd          - Print current working directory\n");
    cli_print("cd <d>       - Change directory\n");
    cli_print("ls           - List files and dirs\n");
    cli_print("cat <f>      - Print file contents\n");
    cli_print("touch <f>    - Create empty file\n");
    cli_print("rm <f>       - Delete file\n");
    cli_print("rm -r <d>    - Delete directory recursively\n");
    cli_print("write <f> t  - Write text to file\n");
    cli_print("echo <t>     - Print text\n");
    cli_print("cp <s> <d>   - Copy file or directory\n");
    cli_print("mv <s> <d>   - Move/rename file or directory\n");
    cli_print("mkdir <d>    - Create directory\n");
    cli_print("rmdir <d>    - Remove empty directory\n");
    cli_print("df           - Show disk usage\n");
    cli_print("gui          - Start the GUI\n");
    cli_print("exit         - Exit CLI\n\n");
}

void cmd_pwd(void) {
    char pwd[256];
    if (fat16_pwd(pwd, sizeof(pwd))) {
        cli_print(pwd);
        cli_print("\n\n");
    }
}

void cmd_cd(const char *path) {
    if (!path || path[0] == '\0') {
        cli_print("Usage: cd <path>\n\n");
        return;
    }
    if (!fat16_chdir(path)) {
        cli_print("Error: directory not found\n\n");
        return;
    }
}

void cmd_ls(void) {
    dir_entry_t entries[32];
    int count = 0;

    if (!fat16_list_dir(dir_context.current_cluster, entries, 32, &count)) {
        cli_print("Error listing files\n\n");
        return;
    }

    cli_print("Files:\n");
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

        cli_print("  ");
        cli_print(name_buffer);

        if (entries[i].attr & ATTR_DIRECTORY) {
            cli_print(" [DIR]");
        } else {
            cli_print(" (");
            char size_str[16];
            int size = entries[i].file_size;
            int len = 0;
            if (size == 0) {
                size_str[len++] = '0';
            } else {
                int temp = size;
                int digits = 0;
                while (temp > 0) { digits++; temp /= 10; }
                temp = size;
                for (int d = 0; d < digits; d++) {
                    size_str[digits - d - 1] = '0' + (temp % 10);
                    temp /= 10;
                }
                len = digits;
            }
            size_str[len] = '\0';
            cli_print(size_str);
            cli_print("b)");
        }
        cli_print("\n");
    }
    cli_print("\n");
}

// fs
void cmd_cat(const char *filename) {
    if (!filename || filename[0] == '\0') {
        cli_print("Usage: cat <filename>\n\n");
        return;
    }

    fat16_file_t file;
    if (!fat16_open(filename, &file)) {
        cli_print("Error: file not found\n\n");
        return;
    }

    char buffer[512];
    int bytes_read;
    while ((bytes_read = fat16_read(&file, buffer, 512)) > 0) {
        for (int i = 0; i < bytes_read; i++) {
            if (buffer[i] == '\n') {
                cli_print("\n");
            } else if (buffer[i] >= 32 && buffer[i] < 127) {
                char c[2] = {buffer[i], '\0'};
                cli_print(c);
            }
        }
    }

    fat16_close(&file);
    cli_print("\n\n");
}

void cmd_write(const char *args) {
    if (!args || args[0] == '\0') {
        cli_print("Usage: write <file> <text>\n\n");
        return;
    }

    int i = 0;
    while (args[i] == ' ') i++;

    int fname_start = i;
    while (args[i] != ' ' && args[i] != '\0') i++;

    if (i == fname_start) {
        cli_print("Usage: write <file> <text>\n\n");
        return;
    }

    char filename[64];
    int fname_len = i - fname_start;
    if (fname_len >= 64) fname_len = 63;
    memcpy(filename, &args[fname_start], fname_len);
    filename[fname_len] = '\0';

    while (args[i] == ' ') i++;
    if (args[i] == '\0') {
        cli_print("Usage: write <file> <text>\n\n");
        return;
    }

    fat16_file_t file;
    dir_entry_t entry;
    if (fat16_find(filename, &entry)) {
        if (!fat16_open(filename, &file)) {
            cli_print("Error: cannot open file\n\n");
            return;
        }
    } else {
        if (!fat16_create(filename, &file)) {
            cli_print("Error: cannot create file\n\n");
            return;
        }
    }

    int text_len = 0;
    while (args[i + text_len] != '\0') text_len++;

    int written = fat16_write(&file, &args[i], text_len);
    fat16_close(&file);

    cli_print("Wrote ");
    char num_str[16];
    int temp = written;
    int digits = 0;
    if (temp == 0) {
        num_str[0] = '0'; num_str[1] = '\0';
    } else {
        while (temp > 0) { digits++; temp /= 10; }
        temp = written;
        for (int d = 0; d < digits; d++) {
            num_str[digits - d - 1] = '0' + (temp % 10);
            temp /= 10;
        }
        num_str[digits] = '\0';
    }
    cli_print(num_str);
    cli_print(" bytes\n\n");
}

void cmd_touch(const char *filename) {
    if (!filename || filename[0] == '\0') {
        cli_print("Usage: touch <filename>\n\n");
        return;
    }

    fat16_file_t file;
    if (!fat16_create(filename, &file)) {
        cli_print("Error: file already exists or invalid path\n\n");
        return;
    }
    fat16_close(&file);
    cli_print("Created: ");
    cli_print(filename);
    cli_print("\n\n");
}

void cmd_rm(const char *args) {
    if (!args || args[0] == '\0') {
        cli_print("Usage: rm [-r] <filename>\n\n");
        return;
    }

    if (strncmp(args, "-r ", 3) == 0) {
        if (!fat16_rm_rf(args + 3)) {
            cli_print("Error: dir not found or delete failed\n\n");
            return;
        }
        cli_print("Deleted (recursive): ");
        cli_print(args + 3);
        cli_print("\n\n");
    } else {
        if (!fat16_delete(args)) {
            cli_print("Error: file not found\n\n");
            return;
        }
        cli_print("Deleted: ");
        cli_print(args);
        cli_print("\n\n");
    }
}

void cmd_mkdir(const char *dirname) {
    if (!dirname || dirname[0] == '\0') {
        cli_print("Usage: mkdir <dirname>\n\n");
        return;
    }

    if (!fat16_mkdir(dirname)) {
        cli_print("Error: could not create '");
        cli_print(dirname);
        cli_print("' (may already exist)\n\n");
        return;
    }

    cli_print("Created directory: ");
    cli_print(dirname);
    cli_print("\n\n");
}

void cmd_rmdir(const char *dirname) {
    if (!dirname || dirname[0] == '\0') {
        cli_print("Usage: rmdir <dirname>\n\n");
        return;
    }

    if (!fat16_rmdir(dirname)) {
        cli_print("Error: '");
        cli_print(dirname);
        cli_print("' not found, not a dir, or not empty\n\n");
        return;
    }
    cli_print("Removed directory: ");
    cli_print(dirname);
    cli_print("\n\n");
}

void cmd_cp(const char *args) {
    if (!args || args[0] == '\0') {
        cli_print("Usage: cp <src> <dest>\n\n");
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
        cli_print("Usage: cp <src> <dest>\n\n");
        return;
    }

    char src[64], dest[64];
    if (src_len >= 64) src_len = 63;
    if (dest_len >= 64) dest_len = 63;
    memcpy(src, &args[src_start], src_len); src[src_len] = '\0';
    memcpy(dest, &args[dest_start], dest_len); dest[dest_len] = '\0';

    dir_entry_t entry;
    if (!fat16_find(src, &entry)) {
        cli_print("Error: source not found\n\n");
        return;
    }

    bool ok = (entry.attr & ATTR_DIRECTORY) ? fat16_copy_dir(src, dest) : fat16_copy_file(src, dest);
    if (ok) {
        cli_print("Copied: "); cli_print(src); cli_print(" -> "); cli_print(dest); cli_print("\n\n");
    } else {
        cli_print("Error: copy failed\n\n");
    }
}

void cmd_mv(const char *args) {
    if (!args || args[0] == '\0') {
        cli_print("Usage: mv <src> <dest>\n\n");
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
        cli_print("Usage: mv <src> <dest>\n\n");
        return;
    }

    char src[64], dest[64];
    memcpy(src, &args[src_start], src_len); src[src_len] = '\0';
    memcpy(dest, &args[dest_start], dest_len); dest[dest_len] = '\0';

    dir_entry_t entry;
    if (!fat16_find(src, &entry)) {
        cli_print("Error: source not found\n\n");
        return;
    }

    bool ok = (entry.attr & ATTR_DIRECTORY) ? fat16_move_dir(src, dest) : fat16_move_file(src, dest);
    if (ok) {
        cli_print("Moved: "); cli_print(src); cli_print(" -> "); cli_print(dest); cli_print("\n\n");
    } else {
        cli_print("Error: move failed\n\n");
    }
}

// other
void cmd_df(void) {
    uint32_t total_bytes = 0, used_bytes = 0;
    if (!fat16_get_usage(&total_bytes, &used_bytes)) {
        cli_print("Error reading storage info\n\n");
        return;
    }

    uint32_t total_kb = total_bytes / 1024;
    uint32_t used_kb = used_bytes / 1024;
    uint32_t free_kb = (total_bytes - used_bytes) / 1024;
    char buf[256];

    cli_print("Storage Total: "); cli_print(uint32_to_str(total_kb, buf)); cli_print(" KB\n");
    cli_print("Storage Used:  "); cli_print(uint32_to_str(used_kb, buf));  cli_print(" KB\n");
    cli_print("Storage Free:  "); cli_print(uint32_to_str(free_kb, buf));  cli_print(" KB\n\n");
}

void cmd_mem(void) {
    char buf[32];
    cli_print("Heap used:  ");
    cli_print(uint32_to_str(heap_used() / 1024, buf));
    cli_print(" KB\n");
    cli_print("Heap free:  ");
    cli_print(uint32_to_str(heap_remaining() / 1024, buf));
    cli_print(" KB\n\n");
}

void cmd_echo(const char *text) {
    if (!text || text[0] == '\0') { cli_print("\n"); return; }
    cli_print(text); cli_print("\n\n");
}

void cmd_clock(){
	time_full_t t = time_rtc();
	char buf[256];
	sprintf(buf, "%d-%d-%d / %d:%d:%d\n", t.year, t.month, t.day, t.hours, t.minutes, t.seconds);
	cli_print(buf);
}

void cmd_gui(void)  { cli_print("Starting GUI...\n\n"); }
void cmd_exit(void) { cli_print("Exiting...\n"); }

//parser
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

bool cli_execute_command(const char *cmd) {
    char command[64];
    char argument[1000];

    parse_command(cmd, command, argument);

    if (strcmp(command, "help") == 0) {
        cmd_help();
        return false;
    } else if (strcmp(command, "clear") == 0) {
        cmd_clear();
        return false;
    } else if (strcmp(command, "pwd") == 0) {
        cmd_pwd();
        return false;
    } else if (strcmp(command, "cd") == 0) {
        cmd_cd(argument);
        return false;
    } else if (strcmp(command, "ls") == 0) {
        cmd_ls();
        return false;
    } else if (strcmp(command, "cat") == 0) {
        cmd_cat(argument);
        return false;
    } else if (strcmp(command, "touch") == 0) {
        cmd_touch(argument);
        return false;
    } else if (strcmp(command, "rm") == 0) {
        cmd_rm(argument);
        return false;
    } else if (strcmp(command, "write") == 0) {
        cmd_write(argument);
        return false;
    } else if (strcmp(command, "echo") == 0) {
        cmd_echo(argument);
        return false;
    } else if (strcmp(command, "clock") == 0) {
    	cmd_clock();
     	return false;
    } else if (strcmp(command, "df") == 0) {
        cmd_df();
        return false;
    } else if (strcmp(command, "mem") == 0) {
    	cmd_mem();
     	return false;
    } else if (strcmp(command, "cp") == 0) {
        cmd_cp(argument);
        return false;
    } else if (strcmp(command, "mv") == 0) {
        cmd_mv(argument);
        return false;
    } else if (strcmp(command, "mkdir") == 0) {
        cmd_mkdir(argument);
        return false;
    } else if (strcmp(command, "rmdir") == 0) {
        cmd_rmdir(argument);
        return false;
    } else if (strcmp(command, "gui") == 0) {
        cmd_gui();
        return true;
    } else if (strcmp(command, "exit") == 0) {
        cmd_exit();
        return true;
    } else if (command[0] != '\0') {
        cli_print("Unknown command: ");
        cli_print(command);
        cli_print("\n\n");
        return false;
    }

    return false;
}

void cli_run(void) {
    cmd_clear();
    cli_print("ASMOS CLI\n");
    cli_print("Type 'help' for available commands\n\n");

    while (1) {
        cli_print_prompt();
        cli.buffer_pos = 0;

        while (1) {
            ps2_update();
            if (kb.key_pressed) {
                if (kb.last_scancode == ENTER) {
                    cli_print("\n\n");
                    if (cli_execute_command(cli.buffer)) return;
                    break;
                } else if (kb.last_scancode == BACKSPACE) {
                    if (cli.buffer_pos > 0) {
                        cli.buffer_pos--;
                        cli.buffer[cli.buffer_pos] = '\0';
                        cli.cursor_x -= CHAR_SPACING;
                        fill_rect(cli.cursor_x, cli.cursor_y, CHAR_SPACING, CHAR_HEIGHT, 0x00);
                        blit();
                    }
                } else if (kb.last_char >= 32 && kb.last_char < 127) {
                    if (cli.buffer_pos < CLI_BUFFER_SIZE - 1) {
                        cli.buffer[cli.buffer_pos++] = kb.last_char;
                        cli.buffer[cli.buffer_pos] = '\0';
                        char c[2] = {kb.last_char, '\0'};
                        cli_print(c);
                    }
                }
            }
        }
    }
}
