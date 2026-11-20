#include "shell/cmds.h"
#include "drivers/gpu.h"
#include "drivers/lpt.h"
#include "drivers/pci.h"
#include "drivers/sb16.h"
#include "drivers/sermouse.h"
#include "fonts/fonts.h"
#include "lib/wav.h"

#include "fs/fs.h"

#include "config/runtime_config.h"
#include "drivers/fdd.h"
#include "drivers/ne2000.h"
#include "fs/fdformat.h"
#include "fs/install.h"
#include "lib/audio.h"
#include "lib/cpu.h"
#include "lib/fpu_emu.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"
#include "network/dns.h"
#include "network/gopher.h"
#include "network/net.h"
#include "os/app_registry.h"
#include "os/appload.h"
#include "os/os.h"
#include "os/print.h"
#include "os/scheduler.h"
#include "ui/fileops.h"

static void append(char *buf, size_t max, const char *text) {
    size_t cl = strlen(buf), tl = strlen(text);
    if (cl + tl + 1 < max)
        strcpy(buf + cl, text);
}

/* One argument: "in quotes" (may hold spaces) or up to the next space.
 * Gives where the next one starts. */
static const char *next_arg(const char *p, char *out, int sz) {
    while (*p == ' ')
        p++;
    int n = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (n < sz - 1)
                out[n++] = *p;
            p++;
        }
        if (*p == '"')
            p++;
    } else {
        while (*p && *p != ' ') {
            if (n < sz - 1)
                out[n++] = *p;
            p++;
        }
    }
    out[n] = '\0';
    while (*p == ' ')
        p++;
    return p;
}

/* the whole rest as one path; quotes optional: cd My Documents works too */
static const char *whole_path(const char *in, char *buf, int sz) {
    if (!in)
        in = "";
    while (*in == ' ')
        in++;
    if (*in == '"') {
        next_arg(in, buf, sz);
        return buf;
    }
    strncpy(buf, in, sz - 1);
    buf[sz - 1] = '\0';
    int n = (int)strlen(buf);
    while (n > 0 && buf[n - 1] == ' ')
        buf[--n] = '\0';
    return buf;
}

/* help: a short index; "help <topic>" for each group */
typedef struct {
    const char *name, *title, *text;
} help_topic_t;

static const help_topic_t HELP[] = {
    {"files", "Files and folders",
     "  pwd              Working directory\n"
     "  cd <dir>         Change directory (/HDA, /HDB, /FDD0...)\n"
     "  ls [dir]         List a directory\n"
     "  mkdir <d>        Create a directory\n"
     "  rmdir <d>        Remove an empty directory\n"
     "  cat <f>          Print a file\n"
     "  find <p> [dir]   Find names (TXT, *.PIC)\n"
     "  touch <f>        Create an empty file\n"
     "  write <f> <text> Write text to a file\n"
     "  cp <src> <dst>   Copy a file or directory\n"
     "  mv <src> <dst>   Move or rename\n"
     "  rm [-r] <f>      Delete a file or directory\n"
     "  format fdd0 [720|1440] [/q] [label]  Format a floppy\n"
     "  sys hda|hdb      Make a hard disk start ASMOS: boot code and\n"
     "                   SYSTEM\\KERNEL.SYS (files kept). Setup: new disks\n"
     "Names: up to 63 characters, spaces allowed. Quote them\n"
     "where two names follow: cp \"My File.txt\" \"Copy.txt\"\n"},
    {"time", "Date, time and calendar",
     "  date             Show the date and time\n"
     "  date <date>      Set the date: 2026-11-16 or 16.11.2026\n"
     "  time             Show the time\n"
     "  time <hh:mm[:ss]> Set the time (local, 24-hour)\n"
     "  cal [m] [y]      Calendar of a month: cal, cal 2 2028\n"
     "  Time zone: Settings > Clock. The Calendar app shows months\n"
     "  and keeps notes.\n"},
    {"info", "System information",
     "  sysinfo          CPU, FPU and uptime\n"
     "  mem              Memory use\n"
     "  df               Disk use\n"
     "  lspci            PCI devices\n"},
    {"network", "Network",
     "  ping <host> [n]  Echo requests (n = 1..10, default 4)\n"
     "  gopher <host> [port] [selector]\n"
     "                   Browse Gopher; also gopher://host/1/sel\n"
     "  netconf          Network card and settings\n"
     "  Address, gateway and DNS are in Settings > Network.\n"},
    {"devices", "Sound, printer and ports",
     "  play <f> [loop]  Play a WAV or MIDI file; play stop\n"
     "  lpt              Parallel and serial ports\n"
     "  print <f> [n]    Print a text file (on LPTn); it is queued\n"
     "  Printer type and queue: Settings > Printers.\n"},
    {"programs", "Programs",
     "  asm <f> [out]    Assemble .ASM into a .BIN\n"
     "  run <f.bin>      Run a flat .BIN (Ctrl+C stops it)\n"
     "  app run|scan|list|unload  External .APP packages\n"},
    {"shell", "The shell",
     "  help [topic]     This help\n"
     "  clear            Clear the screen\n"
     "  echo <text>      Print text\n"
     "  history          Recent output\n"
     "  tee <f>          Save the output to a file\n"
     "  F5 / F6          Scroll; Up / Down: earlier commands\n"
     "  gui              Start the desktop\n"
     "  exit             Leave the shell\n"
     "  shutdown [s] / reboot [s]  (optional delay)\n"},
};
#define HELP_N ((int)(sizeof(HELP) / sizeof(HELP[0])))

void cmd_help(const char *args, char *out, size_t max) {
    while (args && *args == ' ')
        args++;
    if (args && *args) {
        for (int i = 0; i < HELP_N; i++)
            if (!strcmp(args, HELP[i].name)) {
                append(out, max, HELP[i].title);
                append(out, max, ":\n");
                append(out, max, HELP[i].text);
                append(out, max, "\n");
                return;
            }
        append(out, max, "No help topic '");
        append(out, max, args);
        append(out, max, "'.\n");
    }
    append(out, max, "Type 'help <topic>':\n");
    for (int i = 0; i < HELP_N; i++) {
        char line[64];
        snprintf(line, sizeof(line), "  %-10s%s\n", HELP[i].name,
                 HELP[i].title);
        append(out, max, line);
    }
    append(out, max, "\n");
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

void cmd_cd(const char *arg, char *out, size_t max) {
    char pb[FS_PATH_SZ];
    const char *path = whole_path(arg, pb, sizeof pb);
    if (path[0] == '\0') {
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

void cmd_ls(const char *arg, char *out, size_t max) {
    char pb[FS_PATH_SZ];
    const char *path = whole_path(arg, pb, sizeof pb);
    uint8_t drive = dir_context.drive_id;
    uint16_t cluster = dir_context.current_cluster;
    if (path[0] != '\0') {
        if (!fs_resolve_dir(path, &drive, &cluster)) {
            append(out, max, "Error: path not found\n\n");
            return;
        }
    }
    static dir_entry_t entries[64];
    static char names[64][FS_NAME_SZ];
    int count = 0;
    if (!fs_list_dir_names(drive, cluster, entries, names, 64, &count)) {
        append(out, max, "Error listing files\n\n");
        return;
    }

    append(out, max, "Files:\n");
    for (int i = 0; i < count; i++) {
        char ss[FS_NAME_SZ + 32];
        if (entries[i].attr & ATTR_DIRECTORY)
            snprintf(ss, sizeof ss, "  %s [DIR]\n", names[i]);
        else
            snprintf(ss, sizeof ss, "  %s (%ub)\n", names[i],
                     entries[i].file_size);
        append(out, max, ss);
    }
    append(out, max, "\n");
}

/* ── find ── */

typedef struct {
    char *out;
    size_t max;
    int shown;
} find_out_t;

static bool find_line(const char *path, bool is_dir, uint32_t size, void *u) {
    find_out_t *f = (find_out_t *)u;
    char line[FS_PATH_SZ + 32];
    if (is_dir)
        snprintf(line, sizeof(line), "  %s/\n", path);
    else
        snprintf(line, sizeof(line), "  %s (%ub)\n", path, size);
    if (strlen(f->out) + strlen(line) + 40 >= f->max)
        return false; /* output full */
    append(f->out, f->max, line);
    f->shown++;
    return true;
}

void cmd_find(const char *args, char *out, size_t max) {
    char pat[64] = "", dir[FS_PATH_SZ] = "";
    const char *p = next_arg(args ? args : "", pat, sizeof pat);
    whole_path(p, dir, sizeof dir);
    if (!pat[0]) {
        append(out, max,
               "Usage: find <pattern> [dir]\n"
               "  TXT   names containing TXT\n"
               "  *.PIC  ? = one char, * = any\n"
               "  dir: default the current one; / = all of HDA\n\n");
        return;
    }
    if (!dir[0])
        fs_pwd(dir, sizeof(dir));
    find_out_t f = {out, max, 0};
    int n = fo_find(dir, pat, true, find_line, &f);
    char tail[64];
    if (n > f.shown)
        snprintf(tail, sizeof(tail), "  ... output full (%d shown)\n\n",
                 f.shown);
    else
        snprintf(tail, sizeof(tail), "%d found.\n\n", n);
    append(out, max, tail);
}

void cmd_cat(const char *arg, char *out, size_t max) {
    char pb[FS_PATH_SZ];
    const char *filename = whole_path(arg, pb, sizeof pb);
    if (filename[0] == '\0') {
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
            if (buf[i] == '\n' || font_has_char((unsigned char)buf[i]))
                tmp[ti++] = buf[i];
        tmp[ti] = '\0';
        append(out, max, tmp);
    }
    fs_close(&file);
    append(out, max, "\n\n");
}

/* a name about to be made: what it may be made of (fs_validate_name);
 * false, with why in out, if not. One already there is left to the
 * command (it replaces it, or says it can't). */
static bool new_name_ok(const char *path, char *out, size_t max) {
    dir_entry_t e;
    if (fs_find(path, &e))
        return true;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *err = NULL;
    if (fs_validate_name(base, false, &err))
        return true;
    append(out, max, "Error: ");
    append(out, max, err ? err : "invalid name");
    append(out, max, "\n\n");
    return false;
}

void cmd_write(const char *args, char *out, size_t max) {
    if (!args || args[0] == '\0') {
        append(out, max, "Usage: write <file> <text>\n\n");
        return;
    }
    char fn[FS_PATH_SZ];
    const char *text = next_arg(args, fn, sizeof fn);
    int i = (int)(text - args);
    if (!fn[0] || args[i] == '\0') {
        append(out, max, "Usage: write <file> <text>\n\n");
        return;
    }
    if (!new_name_ok(fn, out, max))
        return;
    fs_file_t file;
    dir_entry_t entry;
    /* Replace, don't overwrite in place: file_size never shrinks, so a
     * shorter write used to leave the old content's tail behind. */
    if (fs_find(fn, &entry) && !fs_delete(fn)) {
        append(out, max, "Error: cannot replace\n\n");
        return;
    }
    if (!fs_create(fn, &file)) {
        append(out, max, "Error: cannot create\n\n");
        return;
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

void cmd_touch(const char *arg, char *out, size_t max) {
    char pb[FS_PATH_SZ];
    const char *filename = whole_path(arg, pb, sizeof pb);
    if (filename[0] == '\0') {
        append(out, max, "Usage: touch <filename>\n\n");
        return;
    }
    if (!new_name_ok(filename, out, max))
        return;
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
    char pb[FS_PATH_SZ];
    while (*args == ' ')
        args++;
    bool rec = strncmp(args, "-r ", 3) == 0;
    whole_path(rec ? args + 3 : args, pb, sizeof pb);
    args = pb;
    if (rec) {
        if (!fs_rm_rf(args)) {
            append(out, max, "Error: delete failed\n\n");
            return;
        }
        append(out, max, "Deleted (recursive): ");
        append(out, max, args);
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

void cmd_mkdir(const char *arg, char *out, size_t max) {
    char pb[FS_PATH_SZ];
    const char *dirname = whole_path(arg, pb, sizeof pb);
    if (dirname[0] == '\0') {
        append(out, max, "Usage: mkdir <dirname>\n\n");
        return;
    }
    if (!new_name_ok(dirname, out, max))
        return;
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

void cmd_rmdir(const char *arg, char *out, size_t max) {
    char pb[FS_PATH_SZ];
    const char *dirname = whole_path(arg, pb, sizeof pb);
    if (dirname[0] == '\0') {
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
        append(out, max,
               "Usage: cp <src> <dest>  (\"quote names with spaces\")\n\n");
        return;
    }
    static char src[FS_PATH_SZ], dst[FS_PATH_SZ];
    next_arg(next_arg(args, src, sizeof src), dst, sizeof dst);
    if (!src[0] || !dst[0]) {
        append(out, max,
               "Usage: cp <src> <dest>  (\"quote names with spaces\")\n\n");
        return;
    }
    dir_entry_t e;
    if (!fs_find(src, &e)) {
        append(out, max, "Error: source not found\n\n");
        return;
    }
    if (!new_name_ok(dst, out, max))
        return;
    bool ok = (e.attr & ATTR_DIRECTORY) ? fs_copy_dir(src, dst)
                                        : fs_copy_file(src, dst);
    if (ok) {
        append(out, max, "Copied: ");
        append(out, max, src);
        append(out, max, " -> ");
        append(out, max, dst);
        append(out, max, "\n\n");
    } else {
        append(out, max, "Error: copy failed\n\n");
    }
}

void cmd_mv(const char *args, char *out, size_t max) {
    if (!args || args[0] == '\0') {
        append(out, max,
               "Usage: mv <src> <dest>  (\"quote names with spaces\")\n\n");
        return;
    }
    static char src[FS_PATH_SZ], dst[FS_PATH_SZ];
    next_arg(next_arg(args, src, sizeof src), dst, sizeof dst);
    if (!src[0] || !dst[0]) {
        append(out, max,
               "Usage: mv <src> <dest>  (\"quote names with spaces\")\n\n");
        return;
    }
    dir_entry_t e;
    if (!fs_find(src, &e)) {
        append(out, max, "Error: source not found\n\n");
        return;
    }
    if (!new_name_ok(dst, out, max))
        return;
    bool ok = (e.attr & ATTR_DIRECTORY) ? fs_move_dir(src, dst)
                                        : fs_move_file(src, dst);
    if (ok) {
        append(out, max, "Moved: ");
        append(out, max, src);
        append(out, max, " -> ");
        append(out, max, dst);
        append(out, max, "\n\n");
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
    sprintf(buf, "Kernel heap used: %u KB  free: %u KB%s\n", heap_used() / 1024,
            heap_remaining() / 1024,
            heap_check(&g_kheap) &&
                    (!g_kheap_low.first || heap_check(&g_kheap_low))
                ? ""
                : "  (CORRUPT)");
    append(out, max, buf);
    sprintf(buf, "App pool: %u KB free of %u KB\n", pmm_free_count() * 4,
            pmm_total() * 4);
    append(out, max, buf);
    sprintf(buf, "Paging: %s\n\n",
            vmm_enabled() ? (cpu_family() >= 4
                                 ? "on (apps isolated, kernel code read-only)"
                                 : "on (apps isolated; 386: no write protect)")
            : g_cfg.no_paging ? "off (Settings > System)"
                              : "off");
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

/* ── date, time, cal ─────────────────────────────────────────────── */

static void show_now(char *out, size_t max) {
    time_full_t t = time_rtc_local();
    int wd = time_weekday((int)t.year, t.month, t.day);
    char b[96];
    int tz = (int8_t)g_cfg.timezone_offset;
    snprintf(b, sizeof(b), "%s, %d %s %u  %02u:%02u:%02u  (UTC%+d)\n",
             time_weekday_name(wd), t.day, time_month_name(t.month), t.year,
             t.hours, t.minutes, t.seconds, tz);
    append(out, max, b);
}

/* reads up to `n` digits; returns the number, -1 if none */
static int read_num(const char **p, int n) {
    int v = 0, k = 0;
    while (k < n && **p >= '0' && **p <= '9') {
        v = v * 10 + (**p - '0');
        (*p)++;
        k++;
    }
    return k ? v : -1;
}

void cmd_clock(char *out, size_t max) {
    show_now(out, max);
    append(out, max, "\n");
}

/* date                  show
 * date 2026-11-16       set (ISO)
 * date 16.11.2026       set (day first; also 16/11/2026) */
void cmd_date(const char *args, char *out, size_t max) {
    while (args && *args == ' ')
        args++;
    if (!args || !*args) {
        cmd_clock(out, max);
        return;
    }
    const char *p = args;
    int a = read_num(&p, 4), b = -1, c = -1;
    char sep = *p;
    if (sep == '-' || sep == '.' || sep == '/') {
        p++;
        b = read_num(&p, 2);
        if (*p == sep) {
            p++;
            c = read_num(&p, 4);
        }
    }
    int y, m, d;
    if (sep == '-')
        y = a, m = b, d = c;
    else
        d = a, m = b, y = c;
    while (*p == ' ')
        p++;
    if (a < 0 || b < 0 || c < 0 || *p || !time_date_valid(y, m, d)) {
        append(out, max,
               "Usage: date 2026-11-16  or  date 16.11.2026\n"
               "(no such date, or not a date)\n\n");
        return;
    }
    if (y < 1980 || y > 2099) {
        append(out, max, "The clock chip holds 1980 to 2099.\n\n");
        return;
    }
    time_full_t t = time_rtc_local();
    t.year = (uint32_t)y;
    t.month = (uint8_t)m;
    t.day = (uint8_t)d;
    if (!time_set_local(&t)) {
        append(out, max, "Could not set the clock.\n\n");
        return;
    }
    append(out, max, "Date set: ");
    show_now(out, max);
    append(out, max, "\n");
}

/* time            show
 * time 14:05      set (seconds 0)
 * time 14:05:30   set */
void cmd_time(const char *args, char *out, size_t max) {
    while (args && *args == ' ')
        args++;
    if (!args || !*args) {
        cmd_clock(out, max);
        return;
    }
    const char *p = args;
    int h = read_num(&p, 2), mi = -1, sec = 0;
    if (*p == ':') {
        p++;
        mi = read_num(&p, 2);
        if (*p == ':') {
            p++;
            sec = read_num(&p, 2);
        }
    }
    while (*p == ' ')
        p++;
    if (h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 59 || *p) {
        append(out, max, "Usage: time 14:05  or  time 14:05:30 (24-hour)\n\n");
        return;
    }
    time_full_t t = time_rtc_local();
    t.hours = (uint8_t)h;
    t.minutes = (uint8_t)mi;
    t.seconds = (uint8_t)sec;
    if (!time_set_local(&t)) {
        append(out, max, "Could not set the clock.\n\n");
        return;
    }
    append(out, max, "Time set: ");
    show_now(out, max);
    append(out, max, "\n");
}

/* cal             this month
 * cal 2           February of this year
 * cal 2 2028      February 2028
 * cal 2028        (a 4-digit number alone is a year: its January) */
void cmd_cal(const char *args, char *out, size_t max) {
    time_full_t now = time_rtc_local();
    int m = now.month, y = (int)now.year;
    const char *p = args ? args : "";
    while (*p == ' ')
        p++;
    int a = read_num(&p, 4);
    while (*p == ' ')
        p++;
    int b = read_num(&p, 4);
    while (*p == ' ')
        p++;
    if (*p || (a >= 0 && a > 12 && b >= 0)) {
        append(out, max, "Usage: cal [month] [year]   e.g. cal 2 2028\n\n");
        return;
    }
    if (a >= 1 && a <= 12) {
        m = a;
        if (b >= 0)
            y = b;
    } else if (a > 12) {
        m = 1, y = a;
    } else if (a == 0) {
        append(out, max, "Months are 1 to 12.\n\n");
        return;
    }
    if (y < 1 || y > 9999) {
        append(out, max, "Years are 1 to 9999.\n\n");
        return;
    }

    char line[40];
    char title[32];
    snprintf(title, sizeof(title), "%s %d", time_month_name(m), y);
    int pad = (20 - (int)strlen(title)) / 2;
    int k = 0;
    while (k < pad && k < 20)
        line[k++] = ' ';
    snprintf(line + k, sizeof(line) - k, "%s\n", title);
    append(out, max, line);
    append(out, max, "Mo Tu We Th Fr Sa Su\n");
    int first = time_weekday(y, m, 1), days = time_days_in_month(m, y);
    int col = 0, pos = 0;
    for (int i = 0; i < first; i++, col++)
        pos += snprintf(line + pos, sizeof(line) - pos, "   ");
    for (int d = 1; d <= days; d++) {
        bool today = y == (int)now.year && m == now.month && d == now.day;
        /* today in brackets-less form: a '*' after the number */
        pos += snprintf(line + pos, sizeof(line) - pos, today ? "%2d*" : "%2d ",
                        d);
        if (++col == 7 || d == days) {
            line[pos - 1] = '\n'; /* the last column's space */
            line[pos] = 0;
            append(out, max, line);
            col = 0;
            pos = 0;
        }
    }
    if (y == (int)now.year && m == now.month)
        append(out, max, "* today\n");
    append(out, max, "\n");
}

void cmd_play(const char *args, char *out, size_t max) {
    while (*args == ' ')
        args++;
    if (!*args || !strcmp(args, "stop")) {
        audio_close();
        append(out, max,
               *args ? "Stopped.\n"
                     : "usage: play <file.wav|file.mid> [loop] | play stop\n");
        return;
    }
    char path[FS_PATH_SZ];
    const char *rest = next_arg(args, path, sizeof path);
    bool loop = strstr(rest, "loop") != NULL;
    const char *err = audio_open(path);
    char buf[FS_PATH_SZ + 64];
    if (err) {
        snprintf(buf, sizeof buf, "play: %s\n", err);
    } else {
        audio_set_loop(loop);
        audio_play();
        uint32_t sec = audio_len_ms() / 1000;
        snprintf(buf, sizeof buf,
                 "Playing %s (%s, %s, %u:%02u)%s. 'play stop' stops.\n", path,
                 audio_codec_name(), audio_info(), sec / 60, sec % 60,
                 loop ? ", repeating" : "");
    }
    append(out, max, buf);
}

void cmd_lpt(char *out, size_t max) {
    char buf[96];
    if (!lpt_count())
        append(out, max, "No parallel ports.\n");
    for (int i = 0; i < lpt_count(); i++) {
        sprintf(buf, "LPT%d  %03X  %s\n", i + 1, lpt_base(i),
                lpt_status_text(lpt_status(i)));
        append(out, max, buf);
    }
    for (int i = 0; i < 4; i++)
        if (bios_data_word(i * 2)) {
            sprintf(buf, "COM%d  %03X%s\n", i + 1, bios_data_word(i * 2),
                    sermouse_port() == i + 1 ? "  (mouse)" : "");
            append(out, max, buf);
        }
}

/* print <file> [1|2|3] : text file to a parallel port */
void cmd_print(const char *args, char *out, size_t max) {
    char path[FS_PATH_SZ], buf[128];
    int port = 0;
    const char *p = next_arg(args, path, sizeof path);
    if (*p >= '1' && *p <= '3')
        port = *p - '1';
    if (!path[0]) {
        append(out, max, "usage: print <file> [port 1-3]\n");
        return;
    }
    if (!lpt_base(port)) {
        sprintf(buf, "print: LPT%d not present\n", port + 1);
        append(out, max, buf);
        return;
    }
    fat_file_t f;
    if (!fs_open(path, &f)) {
        append(out, max, "print: file not found\n");
        return;
    }
    uint32_t size = f.entry.file_size;
    char *text = (char *)kmalloc(size ? size : 1);
    if (!text) {
        fs_close(&f);
        append(out, max, "print: out of memory\n");
        return;
    }
    int got = fs_read(&f, text, (int)size);
    fs_close(&f);
    /* through the queue: it prints in the background */
    uint8_t keep = g_cfg.printer_port;
    g_cfg.printer_port = (uint8_t)port;
    const char *nm = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    int id = print_submit_text(nm, text, got > 0 ? (uint32_t)got : 0, 1, 0, 0);
    g_cfg.printer_port = keep;
    kfree(text);
    if (id < 0)
        sprintf(buf, "print: the queue is full (or out of memory)\n");
    else
        sprintf(buf, "Queued as job %d on LPT%d (%s).\n", id, port + 1,
                PRN_TYPE_NAMES[g_cfg.printer_type % PRN_TYPES]);
    append(out, max, buf);
}

/* sys hda|hdb (c: / d:): like DOS's SYS. The disk's FAT16 volume gets the
 * ASMOS boot code (fs/install.c, keeping the partition table and the BPB)
 * and SYSTEM\KERNEL.SYS, from this system or from the setup floppy. Its
 * files stay; a new or wiped disk is Setup's job. */
void cmd_sys(const char *args, char *out, size_t max) {
    char a[16];
    snprintf(a, sizeof(a), "%s", args ? args : "");
    for (char *p = a; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p = (char)(*p + 32);
    int unit = !strcmp(a, "hda") || !strcmp(a, "c:")   ? 0
               : !strcmp(a, "hdb") || !strcmp(a, "d:") ? 1
                                                       : -1;
    if (unit < 0) {
        append(out, max,
               "Usage: sys hda|hdb\n  Makes that hard disk start ASMOS: the "
               "boot code and\n  SYSTEM\\KERNEL.SYS. Its files stay. For a new "
               "disk, use Setup.\n\n");
        return;
    }
    inst_disk_t d;
    inst_probe((uint8_t)unit, &d);
    char b[160];
    if (!d.can_keep) {
        snprintf(b, sizeof(b), "sys: can't: %s\n\n",
                 d.why_not ? d.why_not : "no FAT16 volume");
        append(out, max, b);
        return;
    }
    const char *err = inst_write_boot((uint8_t)unit, -1);
    if (!err && !fs_drive_mounted((uint8_t)unit) &&
        !fs_mount_drive((uint8_t)unit))
        err = "the volume does not mount";
    /* the kernel: this system's, or the setup floppy's */
    const char *src = NULL;
    dir_entry_t de;
    if (!err) {
        if (unit != DRIVE_HDA && fs_find("/HDA/SYSTEM/KERNEL.SYS", &de))
            src = "/HDA/SYSTEM/KERNEL.SYS";
        else if (unit != DRIVE_HDB && fs_find("/HDB/SYSTEM/KERNEL.SYS", &de) &&
                 g_setup_mode)
            src = "/HDB/SYSTEM/KERNEL.SYS";
        else if (fs_find(SETUP_KERNEL, &de))
            src = SETUP_KERNEL;
        char own[32];
        snprintf(own, sizeof(own), "%s/SYSTEM/KERNEL.SYS", g_drive_paths[unit]);
        if (src)
            err = inst_copy_files((uint8_t)unit, src, NULL, true, NULL, NULL);
        else if (!fs_find(own, &de))
            err = "no KERNEL.SYS to copy (put the setup floppy in FDD0)";
    }
    if (err) {
        snprintf(b, sizeof(b), "sys: %s\n\n", err);
        append(out, max, b);
        return;
    }
    if (src)
        snprintf(b, sizeof(b), "%s now starts ASMOS (kernel from %s).\n\n",
                 unit ? "HDB" : "HDA", src);
    else
        snprintf(b, sizeof(b), "%s now starts ASMOS.\n\n",
                 unit ? "HDB" : "HDA");
    append(out, max, b);
}

/* format fdd0 [360|720|1200|1440|2880] [/q] [label] (fdd1; a: and b: too) */
void cmd_format(const char *args, char *out, size_t max) {
    const char *p = args ? args : "";
    while (*p == ' ')
        p++;
    int unit = -1, len = 0;
    char c0 = (char)(p[0] | 0x20);
    if ((c0 == 'a' || c0 == 'b') && p[1] == ':') {
        unit = c0 == 'b', len = 2;
    } else if (c0 == 'f' && (p[1] | 0x20) == 'd' && (p[2] | 0x20) == 'd' &&
               (p[3] == '0' || p[3] == '1')) {
        unit = p[3] - '0', len = p[4] == ':' ? 5 : 4;
    }
    if (unit >= 0 && p[len] && p[len] != ' ')
        unit = -1; /* (fdd0x) */
    if (unit < 0) {
        append(out, max,
               "Usage: format fdd0|fdd1 [160|180|320|360|720|1200|1440|2880] "
               "[/q] "
               "[label]\n"
               "  /q  quick: only a new file system (disk already "
               "formatted)\n  Erases everything on the disk.\n\n");
        return;
    }
    p += len;
    int kb = 0;
    bool quick = false;
    char label[12] = "";
    while (*p) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        if (p[0] == '/' && (p[1] == 'q' || p[1] == 'Q')) {
            quick = true;
            p += 2;
        } else if (*p >= '0' && *p <= '9') {
            while (*p >= '0' && *p <= '9')
                kb = kb * 10 + (*p++ - '0');
        } else {
            int n = 0;
            while (*p && *p != ' ' && n < 11)
                label[n++] = *p++;
            label[n] = 0;
            while (*p && *p != ' ')
                p++;
        }
    }
    int ok[FDD_MAX_FORMATS], n = fdd_drive_formats((uint8_t)unit, ok),
                             media = -1;
    if (!n || !fdd_is_present((uint8_t)unit)) {
        append(out, max, "format: no such floppy drive\n\n");
        return;
    }
    if (!kb)
        media = ok[0]; /* the drive's own size */
    for (int i = 0; i < n && media < 0; i++)
        if (FDD_FORMATS[ok[i]].kb == kb)
            media = ok[i];
    if (media < 0) {
        append(out, max, "format: this drive cannot format that size\n\n");
        return;
    }
    fdformat_t f;
    if (!fdformat_begin(&f, unit ? DRIVE_FDD1 : DRIVE_FDD0, media, label,
                        quick)) {
        append(out, max, "format: ");
        append(out, max, f.err);
        append(out, max, "\n\n");
        return;
    }
    int pct;
    while ((pct = fdformat_step(&f)) >= 0 && pct < 100) {
    }
    char b[96];
    if (pct < 0) {
        snprintf(b, sizeof(b), "format: %s\n\n", f.err);
    } else {
        const fdd_format_t *fm = &FDD_FORMATS[media];
        uint32_t data = (uint32_t)(fm->spt * fm->heads * fm->tracks) - 1 -
                        2u * fm->fat_sectors - fm->root_entries / 16;
        snprintf(b, sizeof(b), "Formatted FDD%d as %s. %u bytes free.\n\n",
                 unit, fm->name, data * 512);
    }
    append(out, max, b);
}

void cmd_lspci(char *out, size_t max) {
    char buf[96];
    if (!pci_mechanism()) {
        append(out, max, "No PCI bus (ISA-only machine).\n");
        return;
    }
    sprintf(buf, "PCI config mechanism #%d, %d device(s):\n", pci_mechanism(),
            pci_count());
    append(out, max, buf);
    for (int i = 0; i < pci_count(); i++) {
        const pci_dev_t *d = pci_get(i);
        uint16_t io = 0;
        for (int b = 0; b < 6 && !io; b++)
            io = pci_bar_io(d, b);
        sprintf(buf, "%02x:%02x.%u %04X:%04X %-15s", d->bus, d->dev, d->fn,
                d->vendor, d->device,
                pci_class_name(d->class_code, d->subclass));
        append(out, max, buf);
        if (d->irq != 0xFF) {
            sprintf(buf, " IRQ %u", d->irq);
            append(out, max, buf);
        }
        if (io) {
            sprintf(buf, " IO %04X", io);
            append(out, max, buf);
        }
        append(out, max, "\n");
    }
}

void cmd_sysinfo(char *out, size_t max) {
    char buf[128];
    cpu_model_init();
    append(out, max, "CPU:    ");
    append(out, max, cpu_model_str());
    if (cpu_fpu_emulated()) {
        char e[64];
        sprintf(e, "%s, x87 emulated (%u instructions so far)\n",
                cpu_has_fpu() ? " + FPU" : " (no FPU)", fpu_emu_count());
        append(out, max, e);
    } else {
        append(out, max, " + FPU\n");
    }
    time_hms_t u = uptime_hms();
    sprintf(buf, "Uptime: %02u:%02u:%02u\nFrames: %u fps\n\n", u.hours,
            u.minutes, u.seconds, gpu_fps());
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
    fdd_flush(0); /* cached floppy writes first */
    fdd_flush(1);
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
    fdd_flush(0); /* cached floppy writes first */
    fdd_flush(1);
    cpu_reset();
}

void cmd_tee(const char *arg, char *out, size_t max) {
    char pb[FS_PATH_SZ];
    const char *filename = whole_path(arg, pb, sizeof pb);
    if (filename[0] == '\0') {
        append(out, max, "Usage: tee <filename>\n\n");
        return;
    }
    if (term_buf_save(filename)) {
        char tmp[FS_PATH_SZ + 32];
        snprintf(tmp, sizeof tmp, "Saved %d lines to %s\n\n", term_buf_count(),
                 filename);
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
    char host[200]; /* host, host:port or a full gopher:// URL */
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
        append(out, max,
               "Usage: gopher <host|ip>[:port] [port] [selector]\n"
               "       gopher gopher://host[:port]/1/selector\n"
               "  e.g. gopher gopher.floodgap.com\n"
               "  Inside: arrows move, Enter opens, b back, d save, "
               "g go to, ? help\n\n");
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
    if (hlen >= (int)sizeof(a->host))
        hlen = (int)sizeof(a->host) - 1;
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

    int slot = scheduler_add_task(gopher_task_entry, a, NULL);
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
        append(out, max,
               "Usage: ping <host|ip> [count]   (count 1-10, "
               "default 4)\n\n");
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
    while (args[i] == ' ')
        i++;
    int count = 4;
    if (args[i] >= '1' && args[i] <= '9') {
        count = 0;
        while (args[i] >= '0' && args[i] <= '9')
            count = count * 10 + (args[i++] - '0');
        if (count > 10)
            count = 10;
    }

    uint8_t ip[4];
    if (!dns_resolve(host, ip)) {
        append(out, max, "Cannot resolve: ");
        append(out, max, host);
        append(out, max, "\n\n");
        return;
    }

    char ip_str[20], line[80];
    sprintf(ip_str, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    sprintf(line, "Pinging %s with 32 bytes:\n", ip_str);
    append(out, max, line);

    static uint16_t s_seq;
    int got = 0;
    uint32_t tmin = 0xFFFFFFFFu, tmax = 0, tsum = 0;
    for (int n = 0; n < count; n++) {
        uint32_t start = time_millis();
        bool ok = icmp_ping(ip, 0x1234, ++s_seq, 1000);
        uint32_t t = time_millis() - start;
        if (ok) {
            sprintf(line, "Reply from %s: seq=%d time=%u ms\n", ip_str, n + 1,
                    t);
            got++;
            tsum += t;
            if (t < tmin)
                tmin = t;
            if (t > tmax)
                tmax = t;
        } else {
            sprintf(line, "Request timed out (seq=%d).\n", n + 1);
        }
        append(out, max, line);
        while (n + 1 < count && time_millis() - start < 1000) /* 1 a second */
            net_poll();
    }
    sprintf(line, "Sent %d, received %d, lost %d (%d%%)\n", count, got,
            count - got, (count - got) * 100 / count);
    append(out, max, line);
    if (got) {
        sprintf(line, "Times: min %u ms, max %u ms, average %u ms\n", tmin,
                tmax, tsum / (uint32_t)got);
        append(out, max, line);
    }
    append(out, max, "\n");
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

void cmd_app(const char *args, char *out, size_t max) {
    if (!args)
        args = "";
    while (*args == ' ')
        args++;

    if (!*args) {
        append(
            out, max,
            "Usage: app run <name|path> | scan [dir] | list | unload <name>\n");
        return;
    }
    const char *arg = args;
    while (*arg && *arg != ' ')
        arg++;
    size_t cl = (size_t)(arg - args);
    while (*arg == ' ')
        arg++;

    if (cl == 3 && !strncmp(args, "run", 3)) {
        if (!*arg) {
            append(out, max, "app run: need a name or path\n");
            return;
        }
        /* a built-in by its name ("app run Setup"), else a .APP */
        for (int i = 0; i < app_registry_count; i++) {
            const registered_app_t *r = &app_registry[i];
            if ((r->menu_label && !strcasecmp(arg, r->menu_label)) ||
                !strcasecmp(arg, r->desc->name)) {
                os_launch_app(r->desc);
                return;
            }
        }
        char pb[FS_PATH_SZ];
        if (!appload_try_open(whole_path(arg, pb, sizeof pb)))
            snprintf(out, max, "app: '%s' not found\n", pb);
    } else if (cl == 4 && !strncmp(args, "scan", 4)) {
        int n = appload_scan_dir(*arg ? arg : "/apps");
        snprintf(out, max, "Loaded %d package(s)\n", n);
    } else if (cl == 4 && !strncmp(args, "list", 4)) {
        int shown = 0;
        for (int i = 0; i < installed_app_count; i++) {
            if (!appload_find(installed_apps[i]->name))
                continue;
            append(out, max, "  ");
            append(out, max, installed_apps[i]->name);
            append(out, max, "\n");
            shown++;
        }
        if (!shown)
            append(out, max, "No external apps loaded\n");
    } else if (cl == 6 && !strncmp(args, "unload", 6)) {
        app_descriptor *d = appload_find(arg);
        if (!d)
            snprintf(out, max, "app: '%s' is not a loaded external app\n", arg);
        else if (!appload_unload(d))
            append(out, max, "app: still running; close it first\n");
        else
            append(out, max, "Unloaded\n");
    } else {
        append(out, max, "app: unknown subcommand\n");
    }
}
