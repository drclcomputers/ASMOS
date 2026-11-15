#include "os/error.h"
#include "config/config.h"
#include "config/runtime_config.h"
#include "drivers/ata.h"
#include "drivers/gpu.h"
#include "drivers/lpt.h"
#include "drivers/mouse.h"
#include "drivers/ne2000.h"
#include "drivers/opl2.h"
#include "drivers/pci.h"
#include "drivers/sb16.h"
#include "drivers/sermouse.h"
#include "fs/fs.h"
#include "lib/core.h"
#include "lib/cpu.h"
#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "ui/dialogs.h"

static bool s_gui_mode = false;

void error_set_gui_mode(bool gui) { s_gui_mode = gui; }
bool error_gui_mode(void) { return s_gui_mode; }

static const char *err_code_str[ERR_COUNT] = {
    [ERR_OK] = "OK",
    [ERR_ATA_TIMEOUT] = "ATA timeout",
    [ERR_ATA_READ] = "ATA read error",
    [ERR_ATA_WRITE] = "ATA write error",
    [ERR_FAT_MOUNT] = "FS mount failed",
    [ERR_FAT_NO_SPACE] = "FS out of space",
    [ERR_FAT_CORRUPT] = "FS corrupt",
    [ERR_FAT_NOT_FOUND] = "File not found",
    [ERR_FAT_EXISTS] = "File already exists",
    [ERR_FAT_WRITE] = "FS write error",
    [ERR_FAT_READ] = "FS read error",
    [ERR_OOM] = "Out of memory",
    [ERR_HEAP_CORRUPT] = "Heap corruption",
    [ERR_TASK_STACK_OVERFLOW] = "Task stack overflow",
    [ERR_CPU_EXCEPTION] = "CPU exception",
    [ERR_WM_MAX_WINDOWS] = "Too many windows",
    [ERR_WM_ALLOC] = "Window alloc failed",
    [ERR_APP_MAX_RUNNING] = "Too many apps",
    [ERR_APP_ALLOC] = "App state alloc failed",
    [ERR_APP_NOT_FOUND] = "App not found",
    [ERR_NULL_PTR] = "Null pointer",
    [ERR_INVALID_ARG] = "Invalid argument",
};

static const char *sev_prefix[3] = {
    "[INFO ]  ",
    "[WARN ]  ",
    "[FATAL]  ",
};

static int boot_cx = 0;
static int boot_cy = 15;
#define BOOT_LINE_H 8
#define BOOT_CHAR_W 5

static void boot_putchar(char c, uint8_t color) {
    if (c == '\n') {
        boot_cx = 0;
        boot_cy += BOOT_LINE_H;
        if (boot_cy + BOOT_LINE_H >= SCREEN_HEIGHT) {
            uint8_t *buf = (uint8_t *)BACKBUF;
            int line_bytes = SCREEN_WIDTH;
            memmove(buf + MENUBAR_H * line_bytes,
                    buf + (MENUBAR_H + BOOT_LINE_H) * line_bytes,
                    (SCREEN_HEIGHT - MENUBAR_H - BOOT_LINE_H) * line_bytes);
            memset(buf + (SCREEN_HEIGHT - BOOT_LINE_H) * line_bytes, BLACK,
                   BOOT_LINE_H * line_bytes);
            boot_cy = SCREEN_HEIGHT - BOOT_LINE_H * 2;
        }
        return;
    }
    if (boot_cx + BOOT_CHAR_W >= SCREEN_WIDTH) {
        boot_cx = 0;
        boot_cy += BOOT_LINE_H;
    }
    draw_char(boot_cx, boot_cy, c, color, 2);
    boot_cx += BOOT_CHAR_W;
}

static void boot_puts(const char *s, uint8_t color) {
    while (*s)
        boot_putchar(*s++, color);
    blit();
}

static void boot_check_line(const char *label, bool ok, const char *detail) {
    boot_puts("  ", WHITE);
    boot_puts(ok ? "[ OK ] " : "[FAIL] ", ok ? LIGHT_GREEN : LIGHT_RED);
    boot_puts(label, WHITE);
    if (detail) {
        boot_puts(": ", WHITE);
        uint8_t detail_color = ok ? LIGHT_GREEN : LIGHT_RED;
        boot_puts(detail, detail_color);
    }
    boot_puts("\n", WHITE);
}

static char gui_msg[128];

void error_report(err_severity_t sev, err_code_t code, const char *context) {
    const char *code_s =
        (code >= 0 && code < ERR_COUNT) ? err_code_str[code] : "unknown error";
    const char *ctx = context ? context : "";

    if (!s_gui_mode) {
        uint8_t color = (sev == ERR_FATAL)     ? LIGHT_RED
                        : (sev == ERR_WARNING) ? YELLOW
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
            __asm__ volatile("cli");
            for (;;)
                __asm__ volatile("hlt");
        }
    } else {
        int mi = 0;
        for (int i = 0; code_s[i] && mi < 100; i++)
            gui_msg[mi++] = code_s[i];
        if (ctx[0]) {
            gui_msg[mi++] = ' ';
            gui_msg[mi++] = '(';
            for (int i = 0; ctx[i] && mi < 120; i++)
                gui_msg[mi++] = ctx[i];
            gui_msg[mi++] = ')';
        }
        gui_msg[mi] = '\0';

        const char *title = (sev == ERR_FATAL)     ? "Fatal Error"
                            : (sev == ERR_WARNING) ? "Warning"
                                                   : "Info";
        if (sev != ERR_FATAL)
            dlg_post_message(title, gui_msg); /* shown next frame */

        if (sev == ERR_FATAL) {
            dlg_draw_now(title, gui_msg);
            blit();
            volatile uint32_t delay = 0x8000000;
            while (delay--)
                __asm__ volatile("nop");
            __asm__ volatile("cli");
            for (;;)
                __asm__ volatile("hlt");
        }
    }
}

void boot_check_ata(void) {
    boot_puts("ATA / Disk\n", LIGHT_GRAY);

    boot_check_line("Controller", true, ata_controller_desc());
    for (uint8_t u = 0; u < 2; u++) {
        const ata_disk_t *d = ata_unit(u);
        char det[64];
        if (d)
            snprintf(det, sizeof(det), "%.30s, %u MB, %s",
                     d->model[0] ? d->model : "?", d->sectors / 2048,
                     d->lba ? "LBA" : "CHS");
        boot_check_line(u ? "HDB" : "HDA (boot)", d || u,
                        d ? det : "not found");
    }

    uint8_t buf[512];
    bool ok = ata_read_sector(0, 0, buf);
    boot_check_line("Sector 0 read", ok, ok ? NULL : "timeout or error");

    if (!ok)
        error_report(ERR_FATAL, ERR_ATA_READ, "boot_check_ata sector 0");

    bool sig_ok = (buf[510] == 0x55 && buf[511] == 0xAA);
    boot_check_line("Boot signature", sig_ok, sig_ok ? NULL : "0x55AA missing");
    if (!sig_ok)
        error_report(ERR_WARNING, ERR_FAT_CORRUPT, "boot signature");
}

void boot_check_fat(void) {
    boot_puts("Filesystem\n", LIGHT_GRAY);

    bool mounted = g_drives[0].mounted;
    boot_check_line("Mounted", mounted,
                    mounted ? NULL : "fs_mount() failed to mount HDA");

    if (!mounted) {
        error_report(ERR_FATAL, ERR_FAT_MOUNT, "boot_check_fat");
        return;
    }

    fat_vol_t *boot_fs = &g_drives[0];

    bool bps_ok = (boot_fs->bpb.bytes_per_sector == 512);
    boot_check_line("Bytes/sector = 512", bps_ok,
                    bps_ok ? NULL : "unexpected sector size");
    if (!bps_ok)
        error_report(ERR_FATAL, ERR_FAT_CORRUPT, "bytes_per_sector");

    bool spc_ok = (boot_fs->bpb.sectors_per_cluster >= 1 &&
                   boot_fs->bpb.sectors_per_cluster <= 128);
    boot_check_line("Sectors/cluster sane", spc_ok,
                    spc_ok ? NULL : "out of range");
    if (!spc_ok)
        error_report(ERR_WARNING, ERR_FAT_CORRUPT, "sectors_per_cluster");

    bool fat_ok = (boot_fs->bpb.fat_count == 1 || boot_fs->bpb.fat_count == 2);
    boot_check_line("FAT count in [1,2]", fat_ok,
                    fat_ok ? NULL : "bad fat_count");
    if (!fat_ok)
        error_report(ERR_FATAL, ERR_FAT_CORRUPT, "fat_count");

    uint32_t total = 0, used = 0;
    bool usage_ok = fs_get_usage(&total, &used);
    boot_check_line("Usage readable", usage_ok,
                    usage_ok ? NULL : "FAT scan error");

    if (usage_ok) {
        if (total > 0 && used > (total / 10 * 9))
            error_report(ERR_WARNING, ERR_FAT_NO_SPACE, "disk >90% full");
    }

    bool clust_ok =
        (boot_fs->cluster_count >= 1 && boot_fs->cluster_count <= 65524);
    boot_check_line("Cluster count sane", clust_ok,
                    clust_ok ? NULL : "cluster_count out of range");
    if (!clust_ok)
        error_report(ERR_WARNING, ERR_FAT_CORRUPT, "cluster_count");
}

void boot_check_pci(void) {
    boot_puts("CPU / Bus\n", LIGHT_GRAY);
    boot_check_line(cpu_model_str(), true, cpu_has_fpu() ? "FPU" : "no FPU");
    char d[40];
    if (pci_mechanism())
        snprintf(d, sizeof(d), "mech #%d, %d devices", pci_mechanism(),
                 pci_count());
    else
        strcpy(d, "none (ISA)");
    boot_check_line("PCI", true, d);
}

void boot_check_input(void) {
    boot_puts("Input\n", LIGHT_GRAY);
    int k = mouse_kinds();
    char d[40];
    if (!k)
        strcpy(d, "none (PS/2 or serial COM1/COM2)");
    else
        snprintf(d, sizeof(d), "%s%s%s%s", (k & MOUSE_PS2) ? "PS/2" : "",
                 (k & MOUSE_PS2) && (k & MOUSE_SERIAL) ? " + " : "",
                 (k & MOUSE_SERIAL) ? "serial COM" : "",
                 (k & MOUSE_SERIAL) ? (sermouse_port() == 1 ? "1" : "2") : "");
    boot_check_line("Mouse", k != 0, d);

    char ports[48] = "";
    for (int i = 0; i < lpt_count(); i++) {
        char one[12];
        snprintf(one, sizeof(one), "%sLPT%d %X", i ? ", " : "", i + 1,
                 lpt_base(i));
        strcat(ports, one);
    }
    boot_check_line("Parallel", true, lpt_count() ? ports : "none");
    ports[0] = 0;
    for (int i = 0; i < 4; i++)
        if (bios_data_word(i * 2)) {
            char one[12];
            snprintf(one, sizeof(one), "%sCOM%d %X", ports[0] ? ", " : "",
                     i + 1, bios_data_word(i * 2));
            strcat(ports, one);
        }
    boot_check_line("Serial", true, ports[0] ? ports : "none");
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
    for (int i = 0; rem_buf[i]; i++)
        detail[di++] = rem_buf[i];
    const char *kb = " KB free";
    for (int i = 0; kb[i]; i++)
        detail[di++] = kb[i];
    detail[di] = '\0';

    boot_check_line("Heap space >= 1KB", space_ok,
                    space_ok ? detail : "critically low");
    if (!space_ok)
        error_report(ERR_FATAL, ERR_OOM, "heap_remaining < 1024");
}

void boot_check_sound(void) {
    boot_puts("Sound\n", LIGHT_GRAY);

    // PC Speaker
    boot_check_line("PC Speaker", true, "PIT channel 2");

    // Sound Blaster (any generation)
    bool sb_ok = sb16_detected();
    char sbd[48];
    if (sb_ok)
        snprintf(sbd, sizeof(sbd), "%s, DSP %d.%02d, %X IRQ %d DMA %d",
                 sb_model_name(), sb_dsp_version() >> 8,
                 sb_dsp_version() & 0xFF, sb_port(), sb_irq(), sb_dma());
    boot_check_line("Sound Blaster", sb_ok, sb_ok ? sbd : "not found");

    // OPL2 FM Synthesizer
    bool opl_ok = opl2_detected();
    boot_check_line("OPL2 FM synth", opl_ok, opl_ok ? NULL : "not detected");
}

void boot_check_graphics(void) {
    boot_puts("Graphics\n", LIGHT_GRAY);

    // Mode 13h or X
    if (g_video_mode == 0)
        boot_check_line("Resolution = 320 x 200", true, NULL);
    else
        boot_check_line("Resolution = 640 x 400", true, NULL);
}

void boot_check_network(void) {
    boot_puts("Network\n", LIGHT_GRAY);

    // Check if networking is enabled in config
    bool enabled = g_cfg.networking_enabled;
    boot_check_line("Networking enabled", enabled,
                    enabled ? NULL : "disabled in config");

    if (!enabled) {
        boot_check_line("NE2000 card", false, "skipped - networking disabled");
        return;
    }

    // Check if NE2000 card is detected
    bool ne2k_ok = ne2000_detected();

    if (ne2k_ok) {
        uint8_t mac[6];
        ne2000_get_mac(mac);
        char mac_str[32];
        sprintf(mac_str, "detected - MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        boot_check_line("NE2000 card", true, mac_str);
        char where[40];
        if (ne2000_irq() >= 0)
            sprintf(where, "%s, I/O %X, IRQ %d", ne2000_bus(), ne2000_io_base(),
                    ne2000_irq());
        else
            sprintf(where, "%s, I/O %X, polled", ne2000_bus(),
                    ne2000_io_base());
        boot_check_line("NE2000 location", true, where);
    } else {
        boot_check_line("NE2000 card", false,
                        "not found (PCI, ISA 300/280/320/340/360/240)");
    }
}
