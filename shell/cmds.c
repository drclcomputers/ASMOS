#include "shell/cmds.h"

#include "fs/fs.h"

#include "config/runtime_config.h"
#include "drivers/ne2000.h"
#include "lib/cpu.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"
#include "network/dns.h"
#include "network/gopher.h"
#include "network/net.h"
#include "os/scheduler.h"

static void append(char *buf, size_t max, const char *text) {
    size_t cl = strlen(buf), tl = strlen(text);
    if (cl + tl + 1 < max)
        strcpy(buf + cl, text);
}

void cmd_help(char *out, size_t max) {
    append(out, max, "Available commands:\n\n");

    append(out, max, "  help          - This message\n");
    append(out, max, "  clear         - Clear screen\n");
    append(out, max, "  exit          - Exit CLI/ASMTerm\n\n");

    append(out, max, "  pwd           - Working directory\n");
    append(out, max, "  cd <d>        - Change directory\n");
    append(out, max, "  ls            - List directory\n");
    append(out, max, "  mkdir <d>     - Create directory\n");
    append(out, max, "  rmdir <d>     - Remove empty dir\n\n");

    append(out, max, "  cat <f>       - Print file\n");
    append(out, max, "  touch <f>     - Create empty file\n");
    append(out, max, "  write <f> <t> - Write text to file\n");
    append(out, max, "  cp <s> <d>    - Copy file/dir\n");
    append(out, max, "  mv <s> <d>    - Move/rename\n");
    append(out, max, "  rm [-r] <f>   - Delete file/dir\n\n");

    append(out, max, "  echo <t>      - Print text\n");
    append(out, max, "  tee <f>       - Save terminal buffer to file\n");
    append(out, max, "  history       - Recent terminal output\n\n");

    append(out, max, "  clock         - System time\n");
    append(out, max, "  df            - Disk usage\n");
    append(out, max, "  mem           - Memory usage\n");
    append(out, max, "  sysinfo       - CPU model and uptime\n\n");

    append(out, max, "  shutdown [s]  - Shut down (optional delay)\n");
    append(out, max, "  reboot [s]    - Reboot (optional delay)\n\n");

    append(out, max, "  asm <f> [out] - Assemble .ASM -> .BIN\n");
    append(out, max, "  run <f>       - Execute flat .BIN binary\n\n");

    append(out, max, "  gopher <ip> [port] [sel] - Browse Gopher\n");
    append(out, max, "  ping <host>   - ICMP echo request\n");
    append(out, max, "  gui           - Start GUI\n\n");
}

void cmd_pwd(char *out, size_t max) {
    const char *drv = g_drive_paths[dir_context.drive_id];
    if (!drv)
        drv = "/?";
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
        if (!fs_drive_mounted(d))
            continue;
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
        if (!fs_drive_mounted(d))
            continue;
        uint32_t tot = 0, used = 0;
        if (!fs_get_usage_drive(d, &tot, &used))
            continue;
        const char *label = fs_drive_label(d);
        const char *vpath = g_drive_paths[d];
        uint32_t free_bytes = tot - used;
        sprintf(
            buf,
            "Drive %s (%s):\n  Total: %u B\n  Used:  %u B\n  Free:  %u B\n\n",
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

void cmd_sysinfo(char *out, size_t max) {
    char buf[128];
    cpu_model_init();
    append(out, max, "CPU:    ");
    append(out, max, cpu_model_str());
    append(out, max, "\n");
    time_hms_t u = uptime_hms();
    sprintf(buf, "Uptime: %02u:%02u:%02u\n\n", u.hours, u.minutes, u.seconds);
    append(out, max, buf);
}

void cmd_shutdown(const char *args, char *out, size_t max) {
    uint32_t delay_s = 0;
    if (args && args[0] != '\0')
        delay_s = (uint32_t)str_to_int(args);
    if (delay_s > 0) {
        char tmp[48];
        sprintf(tmp, "Shutting down in %u seconds...\n", delay_s);
        append(out, max, tmp);
        term_buf_push_text(out);
        out[0] = '\0';
        sleep_s(delay_s);
    }
    append(out, max, "Shutting down...\n");
    term_buf_push_text(out);
    out[0] = '\0';
    sleep_s(1);
    cpu_shutdown();
}

void cmd_reboot(const char *args, char *out, size_t max) {
    uint32_t delay_s = 0;
    if (args && args[0] != '\0')
        delay_s = (uint32_t)str_to_int(args);
    if (delay_s > 0) {
        char tmp[48];
        sprintf(tmp, "Rebooting in %u seconds...\n", delay_s);
        append(out, max, tmp);
        term_buf_push_text(out);
        out[0] = '\0';
        sleep_s(delay_s);
    }
    append(out, max, "Rebooting...\n");
    term_buf_push_text(out);
    out[0] = '\0';
    sleep_s(1);
    cpu_reset();
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

typedef struct {
    char host[64];
    uint16_t port;
    char selector[128];
    term_context_t *ctx;
} gopher_task_args_t;

static void gopher_task_entry(void *arg) {
    gopher_task_args_t *a = (gopher_task_args_t *)arg;
    g_asmterm_active = true;
    gopher_run_host(a->ctx, a->host, a->port, a->selector);
    g_asmterm_active = false;
    kfree(a);
    scheduler_exit_current();
}

void cmd_gopher(term_context_t *ctx, const char *args, char *out, size_t max) {
    if (!args || args[0] == '\0') {
        append(out, max, "Usage: gopher <host|ip> [port] [selector]\n\n");
        return;
    }
    if (!g_cfg.networking_enabled) {
        append(out, max, "Error: networking is disabled\n\n");
        return;
    }
    if (!ne2000_detected()) {
        append(out, max, "Error: no network card detected\n\n");
        return;
    }

    gopher_task_args_t *a =
        (gopher_task_args_t *)kmalloc(sizeof(gopher_task_args_t));
    if (!a) {
        append(out, max, "gopher: out of memory\n\n");
        return;
    }

    int i = 0;
    while (args[i] == ' ')
        i++;
    int hs = i;
    while (args[i] != ' ' && args[i] != '\0')
        i++;
    int hlen = i - hs;
    if (hlen >= 64)
        hlen = 63;
    memcpy(a->host, args + hs, hlen);
    a->host[hlen] = '\0';

    a->port = 70;
    a->selector[0] = '\0';
    a->ctx = ctx;

    while (args[i] == ' ')
        i++;
    if (args[i] != '\0') {
        a->port = 0;
        while (args[i] >= '0' && args[i] <= '9')
            a->port = a->port * 10 + (args[i++] - '0');
        while (args[i] == ' ')
            i++;
        int si = 0;
        while (args[i] != '\0' && si < 127)
            a->selector[si++] = args[i++];
        a->selector[si] = '\0';
    }

    int slot = scheduler_add_task(gopher_task_entry, a);
    if (slot < 0) {
        kfree(a);
        append(out, max, "gopher: no free task slots\n\n");
        return;
    }
    char tmp[48];
    sprintf(tmp, "gopher: started (task %d)\n\n", slot);
    append(out, max, tmp);
}

void cmd_ping(const char *args, char *out, size_t max) {
    if (!args || args[0] == '\0') {
        append(out, max, "Usage: ping <host|ip>\n\n");
        return;
    }

    if (!g_cfg.networking_enabled) {
        append(out, max, "Error: networking is disabled\n\n");
        return;
    }

    char host[64];
    int i = 0;
    while (args[i] == ' ')
        i++;
    int s = i;
    while (args[i] != ' ' && args[i] != '\0')
        i++;
    int len = i - s;
    if (len >= 64)
        len = 63;
    memcpy(host, args + s, len);
    host[len] = '\0';

    uint8_t ip[4];
    if (!dns_resolve(host, ip)) {
        append(out, max, "Cannot resolve: ");
        append(out, max, host);
        append(out, max, "\n\n");
        return;
    }

    char ip_str[20];
    sprintf(ip_str, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    append(out, max, "Pinging ");
    append(out, max, ip_str);
    append(out, max, " ...\n");

    uint32_t start = time_millis();
    bool ok = icmp_ping(ip, 0x1234, 0, 2000);
    uint32_t elapsed = time_millis() - start;

    if (ok) {
        char result[64];
        sprintf(result, "Reply from %s: time=%u ms\n\n", ip_str, elapsed);
        append(out, max, result);
    } else {
        append(out, max, "Request timed out.\n\n");
    }
}

void cmd_netconf(const char *args, char *out, size_t max) {
    (void)args;

    if (!g_cfg.networking_enabled) {
        append(out, max, "Networking is disabled\n\n");
        return;
    }

    uint8_t mac[6];
    ne2000_get_mac(mac);
    append(out, max, "NE2000 detected\n\n");
    char mac_str[32];
    sprintf(mac_str, "MAC: %02X:%02X:%02X:%02X:%02X:%02X\n\n", mac[0], mac[1],
            mac[2], mac[3], mac[4], mac[5]);
    append(out, max, mac_str);

    outb(P0_CMD, CMD_RD2 | CMD_PS0 | CMD_START);
    uint8_t isr = inb(P0_ISR);
    uint8_t bnry = inb(P0_BNRY);
    outb(P0_CMD, CMD_RD2 | CMD_PS1 | CMD_START);
    uint8_t curr = inb(P1_CURR);
    char buf[64];
    sprintf(buf, "ISR=0x%02X BNRY=0x%02X CURR=0x%02X\n\n", isr, bnry, curr);
    append(out, max, buf);
}
