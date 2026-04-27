#include "shell/cmds.h"

#include "fs/fs.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"

static void append(char *buf, size_t max, const char *text) {
    size_t cl = strlen(buf), tl = strlen(text);
    if (cl + tl + 1 < max)
        strcpy(buf + cl, text);
}

void cmd_help(char *out, size_t max) {
    append(out, max, "Available commands:\n");
    append(out, max, "help          - This message\n");
    append(out, max, "clear         - Clear screen\n");
    append(out, max, "pwd           - Working directory\n");
    append(out, max, "cd <d>        - Change directory\n");
    append(out, max, "ls            - List directory\n");
    append(out, max, "cat <f>       - Print file\n");
    append(out, max, "touch <f>     - Create empty file\n");
    append(out, max, "rm [-r] <f>   - Delete file/dir\n");
    append(out, max, "write <f> <t> - Write text to file\n");
    append(out, max, "echo <t>      - Print text\n");
    append(out, max, "cp <s> <d>    - Copy file/dir\n");
    append(out, max, "mv <s> <d>    - Move/rename\n");
    append(out, max, "mkdir <d>     - Create directory\n");
    append(out, max, "rmdir <d>     - Remove empty dir\n");
    append(out, max, "df            - Disk usage\n");
    append(out, max, "mem           - Memory usage\n");
    append(out, max, "clock         - System time\n");
    append(out, max, "tee <f>       - Save terminal buffer to file\n");
    append(out, max, "history       - Recent terminal output\n");
    append(out, max, "asm <f> [out] - Assemble .ASM -> .BIN\n");
    append(out, max, "run <f>       - Execute flat .BIN binary\n");
    append(out, max, "gui           - Start GUI\n");
    append(out, max, "exit          - Exit CLI\n\n");
}

void cmd_pwd(char *out, size_t max) {
    const char *drv = g_drive_paths[dir_context.drive_id];
    if (!drv) drv = "/?";
    if (strcmp(dir_context.path, "/") == 0)
        snprintf(out, max, "%s\n\n", drv);
    else
        snprintf(out, max, "%s%s\n\n", drv, dir_context.path);
}

void cmd_cd(const char *path, char *out, size_t max) {
    if (!path || path[0] == '\0') {
        append(out, max, "Usage: cd <path>\n\n");
        return;
    }

    for (int d = 0; d < DRIVE_COUNT; d++) {
        if (!fs_drive_mounted(d)) continue;
        const char *root = g_drive_paths[d];
        int rlen = strlen(root);
        if (strncmp(path, root, rlen) == 0 &&
            (path[rlen] == '\0' || path[rlen] == '/')) {
            dir_context.drive_id = d;
            dir_context.current_cluster = 0;
            if (path[rlen] == '\0') {
                strcpy(dir_context.path, "/");
            } else {
                if (!fs_chdir(path + rlen)) {
                    append(out, max, "Error: directory not found\n\n");
                }
            }
            return;
        }
    }

    if (!fs_chdir(path))
        append(out, max, "Error: directory not found\n\n");
}

void cmd_ls(const char *path, char *out, size_t max) {
    uint8_t drive = dir_context.drive_id;
    uint16_t cluster = dir_context.current_cluster;
    if (path && path[0] != '\0') {
        if (!fs_resolve_dir(path, &drive, &cluster)) {
            append(out, max, "Error: path not found\n\n");
            return;
        }
    }
    dir_entry_t entries[32];
    int count = 0;
    if (!fs_list_dir(drive, cluster, entries, 32, &count)) {
        append(out, max, "Error listing files\n\n");
        return;
    }

    append(out, max, "Files:\n");
    for (int i = 0; i < count; i++) {
        char nb[16];
        int j = 0;
        for (int k = 0; k < 8 && entries[i].name[k] != ' '; k++)
            nb[j++] = entries[i].name[k];
        if (entries[i].ext[0] != ' ') {
            nb[j++] = '.';
            for (int k = 0; k < 3 && entries[i].ext[k] != ' '; k++)
                nb[j++] = entries[i].ext[k];
        }
        nb[j] = '\0';
        append(out, max, "  ");
        append(out, max, nb);
        if (entries[i].attr & ATTR_DIRECTORY)
            append(out, max, " [DIR]\n");
        else {
            char ss[32];
            sprintf(ss, " (%ub)\n", entries[i].file_size);
            append(out, max, ss);
        }
    }
    append(out, max, "\n");
}

void cmd_cat(const char *filename, char *out, size_t max) {
    if (!filename || filename[0] == '\0') {
        append(out, max, "Usage: cat <filename>\n\n");
        return;
    }
    fs_file_t file;
    if (!fs_open(filename, &file)) {
        append(out, max, "Error: file not found\n\n");
        return;
    }
    char buf[512];
    char tmp[513];
    int n;
    while ((n = fs_read(&file, buf, 512)) > 0) {
        int ti = 0;
        for (int i = 0; i < n; i++)
            if (buf[i] == '\n' || (buf[i] >= 32 && buf[i] < 127))
                tmp[ti++] = buf[i];
        tmp[ti] = '\0';
        append(out, max, tmp);
    }
    fs_close(&file);
    append(out, max, "\n\n");
}

void cmd_write(const char *args, char *out, size_t max) {
    if (!args || args[0] == '\0') {
        append(out, max, "Usage: write <file> <text>\n\n");
        return;
    }
    int i = 0;
    while (args[i] == ' ')
        i++;
    int fs = i;
    while (args[i] != ' ' && args[i] != '\0')
        i++;
    if (i == fs) {
        append(out, max, "Usage: write <file> <text>\n\n");
        return;
    }
    char fn[64];
    int fl = i - fs;
    if (fl >= 64)
        fl = 63;
    memcpy(fn, &args[fs], fl);
    fn[fl] = '\0';
    while (args[i] == ' ')
        i++;
    if (args[i] == '\0') {
        append(out, max, "Usage: write <file> <text>\n\n");
        return;
    }
    fs_file_t file;
    dir_entry_t entry;
    if (fs_find(fn, &entry)) {
        if (!fs_open(fn, &file)) {
            append(out, max, "Error: cannot open\n\n");
            return;
        }
    } else {
        if (!fs_create(fn, &file)) {
            append(out, max, "Error: cannot create\n\n");
            return;
        }
    }
    int tl = 0;
    while (args[i + tl] != '\0')
        tl++;
    int written = fs_write(&file, &args[i], tl);
    fs_close(&file);
    char tmp[64];
    sprintf(tmp, "Wrote %d bytes\n\n", written);
    append(out, max, tmp);
}

void cmd_touch(const char *filename, char *out, size_t max) {
    if (!filename || filename[0] == '\0') {
        append(out, max, "Usage: touch <filename>\n\n");
        return;
    }
    fs_file_t file;
    if (!fs_create(filename, &file)) {
        append(out, max, "Error: exists or invalid\n\n");
        return;
    }
    fs_close(&file);
    append(out, max, "Created: ");
    append(out, max, filename);
    append(out, max, "\n\n");
}

void cmd_rm(const char *args, char *out, size_t max) {
    if (!args || args[0] == '\0') {
        append(out, max, "Usage: rm [-r] <filename>\n\n");
        return;
    }
    if (strncmp(args, "-r ", 3) == 0) {
        if (!fs_rm_rf(args + 3)) {
            append(out, max, "Error: delete failed\n\n");
            return;
        }
        append(out, max, "Deleted (recursive): ");
        append(out, max, args + 3);
        append(out, max, "\n\n");
    } else {
        if (!fs_delete(args)) {
            append(out, max, "Error: file not found\n\n");
            return;
        }
        append(out, max, "Deleted: ");
        append(out, max, args);
        append(out, max, "\n\n");
    }
}

void cmd_mkdir(const char *dirname, char *out, size_t max) {
    if (!dirname || dirname[0] == '\0') {
        append(out, max, "Usage: mkdir <dirname>\n\n");
        return;
    }
    if (!fs_mkdir(dirname)) {
        append(out, max, "Error: could not create '");
        append(out, max, dirname);
        append(out, max, "'\n\n");
        return;
    }
    append(out, max, "Created: ");
    append(out, max, dirname);
    append(out, max, "\n\n");
}

void cmd_rmdir(const char *dirname, char *out, size_t max) {
    if (!dirname || dirname[0] == '\0') {
        append(out, max, "Usage: rmdir <dirname>\n\n");
        return;
    }
    if (!fs_rmdir(dirname)) {
        append(out, max, "Error: '");
        append(out, max, dirname);
        append(out, max, "' not found/not empty\n\n");
        return;
    }
    append(out, max, "Removed: ");
    append(out, max, dirname);
    append(out, max, "\n\n");
}

void cmd_cp(const char *args, char *out, size_t max) {
    if (!args || args[0] == '\0') {
        append(out, max, "Usage: cp <src> <dest>\n\n");
        return;
    }
    int i = 0;
    while (args[i] == ' ')
        i++;
    int ss = i;
    while (args[i] != ' ' && args[i] != '\0')
        i++;
    int sl = i - ss;
    while (args[i] == ' ')
        i++;
    int ds = i;
    while (args[i] != ' ' && args[i] != '\0')
        i++;
    int dl = i - ds;
    if (!sl || !dl) {
        append(out, max, "Usage: cp <src> <dest>\n\n");
        return;
    }
    char src[64], dst[64];
    if (sl >= 64)
        sl = 63;
    if (dl >= 64)
        dl = 63;
    memcpy(src, &args[ss], sl);
    src[sl] = '\0';
    memcpy(dst, &args[ds], dl);
    dst[dl] = '\0';
    dir_entry_t e;
    if (!fs_find(src, &e)) {
        append(out, max, "Error: source not found\n\n");
        return;
    }
    bool ok = (e.attr & ATTR_DIRECTORY) ? fs_copy_dir(src, dst)
                                        : fs_copy_file(src, dst);
    if (ok) {
        char tmp[150];
        sprintf(tmp, "Copied: %s -> %s\n\n", src, dst);
        append(out, max, tmp);
    } else {
        append(out, max, "Error: copy failed\n\n");
    }
}

void cmd_mv(const char *args, char *out, size_t max) {
    if (!args || args[0] == '\0') {
        append(out, max, "Usage: mv <src> <dest>\n\n");
        return;
    }
    int i = 0;
    while (args[i] == ' ')
        i++;
    int ss = i;
    while (args[i] != ' ' && args[i] != '\0')
        i++;
    int sl = i - ss;
    while (args[i] == ' ')
        i++;
    int ds = i;
    while (args[i] != ' ' && args[i] != '\0')
        i++;
    int dl = i - ds;
    if (!sl || !dl) {
        append(out, max, "Usage: mv <src> <dest>\n\n");
        return;
    }
    char src[64], dst[64];
    if (sl >= 64)
        sl = 63;
    if (dl >= 64)
        dl = 63;
    memcpy(src, &args[ss], sl);
    src[sl] = '\0';
    memcpy(dst, &args[ds], dl);
    dst[dl] = '\0';
    dir_entry_t e;
    if (!fs_find(src, &e)) {
        append(out, max, "Error: source not found\n\n");
        return;
    }
    bool ok = (e.attr & ATTR_DIRECTORY) ? fs_move_dir(src, dst)
                                        : fs_move_file(src, dst);
    if (ok) {
        char tmp[150];
        sprintf(tmp, "Moved: %s -> %s\n\n", src, dst);
        append(out, max, tmp);
    } else {
        append(out, max, "Error: move failed\n\n");
    }
}

void cmd_df(char *out, size_t max) {
    char buf[128];
    for (int d = 0; d < DRIVE_COUNT; d++) {
        if (!fs_drive_mounted(d)) continue;
        uint32_t tot = 0, used = 0;
        if (!fs_get_usage_drive(d, &tot, &used)) continue;
        const char *label = fs_drive_label(d);
        const char *vpath = g_drive_paths[d];
        uint32_t free_bytes = tot - used;
        sprintf(buf,
                "Drive %s (%s):\n  Total: %uB\n  Used:%uB ân Free: %uB\n\n",
                label, vpath, tot, used, free_bytes);
        append(out, max, buf);
    }
}

void cmd_mem(char *out, size_t max) {
    char buf[128];
    sprintf(buf, "Heap used: %u KB  free: %u KB\n\n", heap_used() / 1024,
            heap_remaining() / 1024);
    append(out, max, buf);
}

void cmd_echo(const char *text, char *out, size_t max) {
    if (!text || text[0] == '\0') {
        append(out, max, "\n");
        return;
    }
    append(out, max, text);
    append(out, max, "\n\n");
}

void cmd_clock(char *out, size_t max) {
    time_full_t t = time_rtc();
    char buf[64];
    sprintf(buf, "%04d-%02d-%02d  %02d:%02d:%02d\n\n", t.year, t.month, t.day,
            t.hours, t.minutes, t.seconds);
    append(out, max, buf);
}

void cmd_tee(const char *filename, char *out, size_t max) {
    if (!filename || filename[0] == '\0') {
        append(out, max, "Usage: tee <filename>\n\n");
        return;
    }
    if (term_buf_save(filename)) {
        char tmp[128];
        sprintf(tmp, "Saved %d lines to %s\n\n", term_buf_count(), filename);
        append(out, max, tmp);
    } else {
        append(out, max, "Error: could not save buffer\n\n");
    }
}

void cmd_history(char *out, size_t max) {
    int n = term_buf_count();
    if (n == 0) {
        append(out, max, "(empty)\n\n");
        return;
    }
    char tmp[20];
    sprintf(tmp, "%d lines:\n", n);
    append(out, max, tmp);
    int start = n > 20 ? n - 20 : 0;
    for (int i = start; i < n; i++) {
        const char *line = term_buf_get(i);
        if (line) {
            append(out, max, line);
            append(out, max, "\n");
        }
    }
    append(out, max, "\n");
}
