#include "os/error.h"
#include "lib/primitive_graphics.h"
#include "lib/string.h"
#include "lib/mem.h"
#include "lib/alloc.h"
#include "lib/io.h"
#include "fs/ata.h"
#include "fs/fat16.h"
#include "config/config.h"

extern void modal_show(int type, const char *title, const char *message,
                       void (*on_confirm)(void), void (*on_cancel)(void));

static bool s_gui_mode = false;

void error_set_gui_mode(bool gui) { s_gui_mode = gui; }
bool error_gui_mode(void)         { return s_gui_mode; }

static const char *err_code_str[ERR_COUNT] = {
    [ERR_OK]             = "OK",
    [ERR_ATA_TIMEOUT]    = "ATA timeout",
    [ERR_ATA_READ]       = "ATA read error",
    [ERR_ATA_WRITE]      = "ATA write error",
    [ERR_FAT_MOUNT]      = "FAT16 mount failed",
    [ERR_FAT_NO_SPACE]   = "FAT16 out of space",
    [ERR_FAT_CORRUPT]    = "FAT16 corrupt",
    [ERR_FAT_NOT_FOUND]  = "File not found",
    [ERR_FAT_EXISTS]     = "File already exists",
    [ERR_FAT_WRITE]      = "FAT16 write error",
    [ERR_FAT_READ]       = "FAT16 read error",
    [ERR_OOM]            = "Out of memory",
    [ERR_HEAP_CORRUPT]   = "Heap corruption",
    [ERR_WM_MAX_WINDOWS] = "Too many windows",
    [ERR_WM_ALLOC]       = "Window alloc failed",
    [ERR_APP_MAX_RUNNING]= "Too many apps",
    [ERR_APP_ALLOC]      = "App state alloc failed",
    [ERR_APP_NOT_FOUND]  = "App not found",
    [ERR_NULL_PTR]       = "Null pointer",
    [ERR_INVALID_ARG]    = "Invalid argument",
};

static const char *sev_prefix[3] = {
    "[INFO ]  ",
    "[WARN ]  ",
    "[FATAL]  ",
};

static int boot_cx = 0;
static int boot_cy = 15;
#define BOOT_LINE_H  8
#define BOOT_CHAR_W  5

static void boot_putchar(char c, uint8_t color) {
    if (c == '\n') {
        boot_cx  = 0;
        boot_cy += BOOT_LINE_H;
        if (boot_cy + BOOT_LINE_H >= SCREEN_HEIGHT) {
            uint8_t *buf = (uint8_t *)BACKBUF;
            int line_bytes = SCREEN_WIDTH;
            memmove(buf + MENUBAR_H * line_bytes,
                    buf + (MENUBAR_H + BOOT_LINE_H) * line_bytes,
                    (SCREEN_HEIGHT - MENUBAR_H - BOOT_LINE_H) * line_bytes);
            memset(buf + (SCREEN_HEIGHT - BOOT_LINE_H) * line_bytes,
                   BLACK, BOOT_LINE_H * line_bytes);
            boot_cy = SCREEN_HEIGHT - BOOT_LINE_H * 2;
        }
        return;
    }
    if (boot_cx + BOOT_CHAR_W >= SCREEN_WIDTH) {
        boot_cx  = 0;
        boot_cy += BOOT_LINE_H;
    }
    draw_char(boot_cx, boot_cy, c, color, 2);
    boot_cx += BOOT_CHAR_W;
}

static void boot_puts(const char *s, uint8_t color) {
    while (*s) boot_putchar(*s++, color);
    blit();
}

static void boot_check_line(const char *label, bool ok, const char *detail) {
    boot_puts("  ", WHITE);
    boot_puts(ok ? "[ OK ] " : "[FAIL] ", ok ? LIGHT_GREEN : LIGHT_RED);
    boot_puts(label, WHITE);
    if (!ok && detail) {
        boot_puts(": ", WHITE);
        boot_puts(detail, LIGHT_RED);
    }
    boot_puts("\n", WHITE);
}

void error_report(err_severity_t sev, err_code_t code, const char *context) {
    const char *code_s = (code >= 0 && code < ERR_COUNT) ? err_code_str[code] : "unknown error";
    const char *ctx    = context ? context : "";

    if (!s_gui_mode) {
        uint8_t color = (sev == ERR_FATAL)   ? LIGHT_RED
                      : (sev == ERR_WARNING) ? LIGHT_YELLOW
                                             : LIGHT_CYAN;
        boot_puts(sev_prefix[sev], color);
        boot_puts(code_s, color);
        if (ctx[0]) {
            boot_puts(" (", WHITE);
            boot_puts(ctx, LIGHT_GRAY);
            boot_puts(")", WHITE);
        }
        boot_puts("\n", WHITE);

        if (sev == ERR_FATAL) {
            boot_puts("\n  System halted.\n", LIGHT_RED);
            blit();
            __asm__ volatile ("cli");
            for (;;) __asm__ volatile ("hlt");
        }
    } else {
        char msg[128];
        int  mi = 0;
        for (int i = 0; code_s[i] && mi < 100; i++) msg[mi++] = code_s[i];
        if (ctx[0]) {
            msg[mi++] = ' '; msg[mi++] = '(';
            for (int i = 0; ctx[i] && mi < 120; i++) msg[mi++] = ctx[i];
            msg[mi++] = ')';
        }
        msg[mi] = '\0';

        int modal_t = (sev == ERR_FATAL || sev == ERR_WARNING) ? 2 : 0; // ERROR or INFO

        const char *title = (sev == ERR_FATAL)   ? "Fatal Error"
                          : (sev == ERR_WARNING) ? "Warning"
                                                 : "Info";
        modal_show(modal_t, title, msg, NULL, NULL);

        if (sev == ERR_FATAL) {
            blit();
            volatile uint32_t delay = 0x8000000;
            while (delay--) __asm__ volatile ("nop");
            __asm__ volatile ("cli");
            for (;;) __asm__ volatile ("hlt");
        }
    }
}

void boot_check_ata(void) {
    boot_puts("ATA / Disk\n", LIGHT_GRAY);

    uint8_t buf[512];
    bool ok = ata_read_sector(0, buf);
    boot_check_line("Sector 0 read", ok, ok ? NULL : "timeout or error");

    if (!ok) error_report(ERR_FATAL, ERR_ATA_READ, "boot_check_ata sector 0");

    bool sig_ok = (buf[510] == 0x55 && buf[511] == 0xAA);
    boot_check_line("Boot signature", sig_ok, sig_ok ? NULL : "0x55AA missing");
    if (!sig_ok) error_report(ERR_WARNING, ERR_FAT_CORRUPT, "boot signature");
}

void boot_check_fat(void) {
    boot_puts("FAT16 Filesystem\n", LIGHT_GRAY);

    bool mounted = fs.mounted;
    boot_check_line("Mounted", mounted, mounted ? NULL : "fat16_mount() returned false");
    if (!mounted) {
        error_report(ERR_FATAL, ERR_FAT_MOUNT, "boot_check_fat");
        return;
    }

    bool bps_ok = (fs.bpb.bytes_per_sector == 512);
    boot_check_line("Bytes/sector = 512", bps_ok, bps_ok ? NULL : "unexpected sector size");
    if (!bps_ok) error_report(ERR_FATAL, ERR_FAT_CORRUPT, "bytes_per_sector");

    bool spc_ok = (fs.bpb.sectors_per_cluster >= 1 && fs.bpb.sectors_per_cluster <= 128);
    boot_check_line("Sectors/cluster sane", spc_ok, spc_ok ? NULL : "out of range");
    if (!spc_ok) error_report(ERR_WARNING, ERR_FAT_CORRUPT, "sectors_per_cluster");

    bool fat_ok = (fs.bpb.fat_count == 1 || fs.bpb.fat_count == 2);
    boot_check_line("FAT count in [1,2]", fat_ok, fat_ok ? NULL : "bad fat_count");
    if (!fat_ok) error_report(ERR_FATAL, ERR_FAT_CORRUPT, "fat_count");

    uint32_t total = 0, used = 0;
    bool usage_ok = fat16_get_usage(&total, &used);
    boot_check_line("Usage readable", usage_ok, usage_ok ? NULL : "FAT scan error");

    if (usage_ok) {
        if (total > 0 && used > (total / 10 * 9))
            error_report(ERR_WARNING, ERR_FAT_NO_SPACE, "disk >90% full");
    }

    bool clust_ok = (fs.cluster_count >= 4096 && fs.cluster_count <= 65524);
    boot_check_line("Cluster count sane", clust_ok, clust_ok ? NULL : "not a valid FAT16 range");
    if (!clust_ok) error_report(ERR_WARNING, ERR_FAT_CORRUPT, "cluster_count");
}

void boot_check_heap(void) {
    boot_puts("Heap\n", LIGHT_GRAY);

    void *p = kmalloc(16);
    bool alloc_ok = (p != NULL);
    boot_check_line("kmalloc(16)", alloc_ok, alloc_ok ? NULL : "returned NULL");
    if (!alloc_ok) {
        error_report(ERR_FATAL, ERR_OOM, "boot_check_heap");
        return;
    }
    kfree(p);
    boot_check_line("kfree", true, NULL);

    uint32_t rem = heap_remaining();
    bool space_ok = rem >= 1024;
    char rem_buf[12];
    uint32_to_str(rem / 1024, rem_buf);
    char detail[32];
    int di = 0;
    for (int i = 0; rem_buf[i]; i++) detail[di++] = rem_buf[i];
    const char *kb = " KB free";
    for (int i = 0; kb[i]; i++) detail[di++] = kb[i];
    detail[di] = '\0';

    boot_check_line("Heap space >= 1KB", space_ok, space_ok ? detail : "critically low");
    if (!space_ok) error_report(ERR_FATAL, ERR_OOM, "heap_remaining < 1024");
}
