#include "ui/desktop.h"
#include "ui/desktop_fs.h"
#include "ui/dialogs.h"
#include "ui/fileops.h"
#include "ui/icons.h"
#include "ui/menubar.h"
#include "ui/popup.h"
#include "ui/uifont.h"
#include "ui/window.h"

#include "os/app_registry.h"
#include "os/appload.h"
#include "os/clipboard.h"
#include "os/os.h"

#include "config/config.h"
#include "config/runtime_config.h"

#include "drivers/ne2000.h"
#include "drivers/opl2.h"
#include "drivers/sb16.h"
#include "drivers/serial.h"
#include "drivers/speaker.h"
#include "fs/fdd_ctrl.h"
#include "lib/alloc.h"
#include "lib/core.h"
#include "lib/cpu.h"
#include "lib/pmm.h"

/* Defined in apps/filef.c (apps have no public headers). */
void ff_open_dir_pub(uint16_t cluster, const char *path);
bool ff_folder_at(int mx, int my, char *out, int sz);
#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"

#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "interrupts/idt.h"

/* ── wallpaper ──────────────────────────────────────────────────────────── */
void draw_wallpaper_pattern(void) {
    uint8_t main_col = g_cfg.wallpaper_main_color;
    uint8_t sec_col = g_cfg.wallpaper_secondary_color;

    switch (g_cfg.wallpaper_pattern) {
    case WALLPAPER_CHECKERBOARD:
        for (int y = 0; y < SCREEN_HEIGHT; y += 10)
            for (int x = 0; x < SCREEN_WIDTH; x += 10) {
                uint8_t color =
                    ((x / 10) + (y / 10)) % 2 == 0 ? main_col : sec_col;
                fill_rect(x, y, 10, 10, color);
            }
        break;
    case WALLPAPER_STRIPES:
        for (int y = 0; y < SCREEN_HEIGHT; y++)
            draw_line(0, y, SCREEN_WIDTH - 1, y,
                      (y / 5) % 2 == 0 ? main_col : sec_col);
        break;
    case WALLPAPER_VERTICAL_STRIPES:
    case WALLPAPER_DOTS: {
        /* build one row, then copy it: whole rows at memcpy speed */
        static uint8_t row[640];
        bool dots = g_cfg.wallpaper_pattern == WALLPAPER_DOTS;
        for (int x = 0; x < SCREEN_WIDTH; x++)
            row[x] = dots ? ((x % 10) == 5 ? sec_col : main_col)
                          : ((x / 5) % 2 == 0 ? main_col : sec_col);
        if (dots)
            clear_screen(main_col);
        for (int y = dots ? 5 : 0; y < SCREEN_HEIGHT; y += dots ? 10 : 1)
            draw_pixels(0, y, SCREEN_WIDTH, 1, row, SCREEN_WIDTH);
        break;
    }
    default:
        clear_screen(main_col);
        break;
    }
}

/* ── desktop state ──────────────────────────────────────────────────────── */
#define DESK_ORIGIN_X 0
#define DESK_ORIGIN_Y MENUBAR_H

static int s_drag_idx = -1;
static int s_press_idx = -1, s_press_x, s_press_y; /* button-down spot */
static int s_drag_off_x = 0;
static int s_drag_off_y = 0;
static int s_last_click_idx = -1;
static uint32_t s_last_click_tick = 0;

/* ── power animations ───────────────────────────────────────────────────── */
#define BAR_W 160
#define BAR_H 12
#define BAR_X ((SCREEN_WIDTH - BAR_W) / 2)
#define BAR_Y ((SCREEN_HEIGHT / 2) + 16)
#define STEPS 40

void power_anim_run(bool is_restart) {
    if (g_cfg.no_power_anim) /* Settings > Performance */
        return;
    const char *msg = is_restart ? "Restarting..." : "Shutting down...";
    uint8_t bar_col = is_restart ? LIGHT_GREEN : LIGHT_CYAN;
    uint8_t glow_col = is_restart ? GREEN : CYAN;

    for (int fade = 0; fade < 8; fade++) {
        uint8_t *buf = (uint8_t *)BACKBUF;
        for (int y = 0; y < SCREEN_HEIGHT; y++) {
            if ((y + fade) % 3 == 0) {
                for (int x = 0; x < SCREEN_WIDTH; x++)
                    buf[y * SCREEN_WIDTH + x] = BLACK;
            }
        }
        blit();
        sleep_ms(50);
    }

    for (int step = 0; step <= STEPS; step++) {
        clear_screen(BLACK);

        for (int y = 1; y < SCREEN_HEIGHT; y += 3)
            draw_line(0, y, SCREEN_WIDTH - 1, y, DARK_GRAY);

        int msg_x = (SCREEN_WIDTH - text_width(msg, UI_FONT_BOLD)) / 2;
        draw_string(msg_x, SCREEN_HEIGHT / 2 - 24, (char *)msg, WHITE,
                    UI_FONT_BOLD);

        char pct[5];
        int p = (step * 100) / STEPS;
        pct[0] = '0' + p / 100;
        pct[1] = '0' + (p / 10) % 10;
        pct[2] = '0' + p % 10;
        pct[3] = '%';
        pct[4] = '\0';
        int px = (SCREEN_WIDTH - text_width(pct, UI_FONT_SMALL)) / 2;
        draw_string(px, SCREEN_HEIGHT / 2 - 10, pct, LIGHT_GRAY, UI_FONT_SMALL);

        fill_rect(BAR_X + 2, BAR_Y + 2, BAR_W, BAR_H, DARK_GRAY);

        fill_rect(BAR_X - 1, BAR_Y - 1, BAR_W + 2, BAR_H + 2, DARK_GRAY);
        fill_rect(BAR_X, BAR_Y, BAR_W, BAR_H, BLACK);

        int filled = (step * BAR_W) / STEPS;
        fill_rect(BAR_X, BAR_Y, filled, BAR_H, bar_col);

        if (filled > 2)
            fill_rect(BAR_X + filled - 2, BAR_Y, 2, BAR_H, WHITE);

        draw_rect(BAR_X - 1, BAR_Y - 1, BAR_W + 2, BAR_H + 2, glow_col);

        static const int dot_x[] = {-6, -2, 2, 6};
        for (int d = 0; d < 4; d++) {
            uint8_t dcol = ((step / 4 + d) % 4 == 0) ? WHITE : DARK_GRAY;
            int sx = SCREEN_WIDTH / 2 + dot_x[d];
            int sy = BAR_Y + BAR_H + 10;
            fill_rect(sx, sy, 2, 2, dcol);
        }

        blit();
        sleep_ms(50);
    }

    for (int f = 0; f < 3; f++) {
        clear_screen(f % 2 == 0 ? DARK_GRAY : BLACK);
        blit();
        sleep_ms(120);
    }

    clear_screen(BLACK);
    int msg_x = (SCREEN_WIDTH - text_width(msg, UI_FONT_BOLD)) / 2;
    draw_string(msg_x, SCREEN_HEIGHT / 2 - 4, (char *)msg, bar_col,
                UI_FONT_BOLD);
    blit();
    sleep_s(2);
}

/* ── open item ──────────────────────────────────────────────────────────── */

static void launch_app_ud(void *userdata) {
    os_launch_app((app_descriptor *)userdata);
}

/* Drives open their root in FileF; everything else follows the shared
 * open-by-type rules (ui/fileops). */
static void open_item(int idx) {
    desktop_item_t *it = &desktop_fs_items()[idx];
    if (it->kind == DESKTOP_ITEM_FDD || it->kind == DESKTOP_ITEM_HDD) {
        for (int m = 0; m < VFS_MOUNT_COUNT; m++)
            if (g_vfs_mounts[m].drive_id == it->drive_id &&
                fs_drive_mounted(it->drive_id)) {
                ff_open_dir_pub(0, g_vfs_mounts[m].path);
                return;
            }
        return;
    }
    char path[270];
    if (desktop_fs_item_path(idx, path, sizeof(path)))
        fo_open(NULL, path);
}

/* ── drawing ────────────────────────────────────────────────────────────── */

static void draw_desktop_item(const desktop_item_t *it, bool dragged_ghost) {
    int ax = DESK_ORIGIN_X + it->x;
    int ay = DESK_ORIGIN_Y + it->y;

    if (dragged_ghost) {
        draw_rect(ax, ay, ICON_W, ICON_H, DARK_GRAY);
        return;
    }

    bool is_cut = (clipboard_has_file() && g_clipboard.is_cut &&
                   strcmp(g_clipboard.name, it->name) == 0 &&
                   strcmp(g_clipboard.src_path, desktop_fs_path()) == 0);

    switch (it->kind) {
    case DESKTOP_ITEM_DIR:
        draw_folder_icon(ax, ay, it->selected);
        break;
    case DESKTOP_ITEM_APP:
        draw_app_icon(ax, ay, it->selected);
        break;
    case DESKTOP_ITEM_FILE: {
        const char *dot = strchr(it->name, '.');
        const char *ext = dot ? dot + 1 : "";
        if (strcmp(ext, "PIC") == 0)
            draw_pic_icon(ax, ay, it->selected);
        else if (strcmp(ext, "TXT") == 0)
            draw_txt_icon(ax, ay, it->selected);
        else if (strcmp(ext, "CFG") == 0)
            draw_cfg_icon(ax, ay, it->selected);
        else
            draw_unknown_icon(ax, ay, it->selected);
        break;
    }
    case DESKTOP_ITEM_FDD:
        draw_floppy_icon(ax, ay, it->selected);
        break;
    case DESKTOP_ITEM_TRASH:
        draw_trash_icon(ax, ay, it->selected, it->full);
        break;
    default:
        draw_file_icon(ax, ay, it->selected);
        break;
    }

    char disp[ICO_LABEL_MAX + 1];
    int nlen = (int)strlen(it->name);
    int dlen = nlen < ICO_LABEL_MAX ? nlen : ICO_LABEL_MAX;
    for (int i = 0; i < dlen; i++)
        disp[i] = it->name[i];
    disp[dlen] = '\0';

    int lw = text_width(disp, UI_FONT_SMALL);
    int lx = ax + ICON_W / 2 - lw / 2;
    int ly = ay + ICON_H + 2;

    uint8_t label_col = is_cut ? DARK_GRAY : WHITE;
    draw_string(lx + 1, ly + 1, disp, BLACK, UI_FONT_SMALL);
    draw_string(lx, ly, disp, label_col, UI_FONT_SMALL);
}

/* Over windows the desktop layer is hidden: draw the dragged icon on top. */
static void draw_drag_overlay(void *user) {
    int idx = *(int *)user;
    if (idx >= 0 && idx < desktop_fs_count())
        draw_desktop_item(&desktop_fs_items()[idx], true);
}

/* Icon under (mx, my), ignoring `skip` (the one being dragged). */
static int desktop_hit_except(int mx, int my, int skip) {
    desktop_item_t *items = desktop_fs_items();
    int count = desktop_fs_count();
    for (int i = 0; i < count; i++) {
        if (!items[i].used || i == skip)
            continue;
        int ax = DESK_ORIGIN_X + items[i].x;
        int ay = DESK_ORIGIN_Y + items[i].y;
        if (mx >= ax && mx < ax + ICON_W && my >= ay &&
            my < ay + ICON_H + ICO_LABEL_H + 2)
            return i;
    }
    return -1;
}

static int desktop_hit(int mx, int my) {
    return desktop_hit_except(mx, my, -1);
}

static int selected_idx(void) {
    desktop_item_t *items = desktop_fs_items();
    int count = desktop_fs_count();
    for (int i = 0; i < count; i++)
        if (items[i].selected)
            return i;
    return -1;
}

/* ── menu actions: file operations go through ui/fileops ─────────────── */

/* Path of the selected file/folder/app; false (with a message) if none. */
static bool sel_path(const char *title, char *out, int sz) {
    int sel = selected_idx();
    if (sel < 0 || !desktop_fs_item_path(sel, out, sz)) {
        dlg_message(NULL, title, "Select a file or folder first.");
        return false;
    }
    return true;
}

static void menu_new_file(void) {
    fo_ask_new(NULL, DESKTOP_PATH, false, NULL, NULL);
}
static void menu_new_folder(void) {
    fo_ask_new(NULL, DESKTOP_PATH, true, NULL, NULL);
}
static void menu_rename(void) {
    char p[270];
    if (sel_path("Rename", p, sizeof(p)))
        fo_ask_rename(NULL, p, NULL, NULL);
}
static void menu_delete(void) {
    char p[270];
    if (sel_path("Delete", p, sizeof(p)))
        fo_ask_delete(NULL, p, NULL, NULL);
}
static void menu_info(void) {
    char p[270];
    if (sel_path("Get Info", p, sizeof(p)))
        fo_show_info(NULL, p);
}
static void menu_reload(void) { desktop_fs_set_dirty(); }
static void menu_copy(void) {
    char p[270];
    if (sel_path("Copy", p, sizeof(p)))
        fo_report(NULL, "Copy", fo_clip(p, false));
}
static void menu_cut(void) {
    char p[270];
    if (sel_path("Cut", p, sizeof(p)))
        fo_report(NULL, "Cut", fo_clip(p, true));
}
static void menu_paste(void) {
    fo_report(NULL, "Paste", fo_paste(DESKTOP_PATH));
}

/* dlg_confirm callback: user is the void(void) action to run on Yes */
static void run_if_yes(void *fn, bool yes) {
    if (yes)
        ((void (*)(void))fn)();
}

static void menu_about_os(void) {
    char ver[24], cpu[56], fpu[48], mem[48], disp[48], snd[48], snd2[48],
        net[48], net2[48], up[24];
    sprintf(ver, "ASMOS %s", VER);

    snprintf(cpu, sizeof(cpu), "%s", cpu_model_str());
    if (!cpu_has_fpu())
        strcpy(fpu, "None (x87 emulated)");
    else if (cpu_fpu_emulated())
        strcpy(fpu, "Present, emulation forced");
    else if (cpu_family() == 3)
        strcpy(fpu, "80387 coprocessor");
    else
        strcpy(fpu, "x87 FPU");

    uint32_t free_kb = (heap_remaining() + pmm_free_count() * 4096) / 1024;
    if (g_ram_bytes)
        sprintf(mem, "%u KB, %u KB free", g_ram_bytes / 1024, free_kb);
    else
        sprintf(mem, "Unknown, %u KB free", free_kb);

    sprintf(disp, "VGA %dx%d, %d colors", SCREEN_WIDTH, SCREEN_HEIGHT,
            SCREEN_WIDTH == 320 ? 256 : 16);

    snd2[0] = 0;
    if (sb16_detected()) {
        int v = sb_dsp_version();
        sprintf(snd, "%s, DSP %d.%02d", sb_model_name(), v >> 8, v & 0xFF);
        sprintf(snd2, "Port %Xh, IRQ %d, DMA %d", sb_port(), sb_irq(),
                sb_dma());
    } else if (opl2_detected()) {
        strcpy(snd, "AdLib (OPL2 FM)");
    } else {
        strcpy(snd, "PC speaker");
    }

    net2[0] = 0;
    if (ne2000_detected()) {
        uint8_t m[6];
        ne2000_get_mac(m);
        int irq = ne2000_irq();
        if (irq >= 0)
            sprintf(net, "NE2000 (%s), %Xh, IRQ %d", ne2000_bus(),
                    ne2000_io_base(), irq);
        else
            sprintf(net, "NE2000 (%s), %Xh", ne2000_bus(), ne2000_io_base());
        sprintf(net2, "MAC %02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2],
                m[3], m[4], m[5]);
    } else {
        strcpy(net, "None");
    }

    time_hms_t u = uptime_hms();
    sprintf(up, "%u:%02u:%02u", u.hours, u.minutes, u.seconds);

    dlg_row_t rows[10];
    int n = 0;
    rows[n++] = (dlg_row_t){"Processor", cpu};
    rows[n++] = (dlg_row_t){"Coprocessor", fpu};
    rows[n++] = (dlg_row_t){"Memory", mem};
    rows[n++] = (dlg_row_t){"Display", disp};
    rows[n++] = (dlg_row_t){"Sound", snd};
    if (snd2[0])
        rows[n++] = (dlg_row_t){"", snd2};
    rows[n++] = (dlg_row_t){"Network", net};
    if (net2[0])
        rows[n++] = (dlg_row_t){"", net2};
    rows[n++] = (dlg_row_t){"Uptime", up};

    dlg_info(NULL, "About ASMOS", ver, "Developed by drclcomputers.", rows, n,
             "Check out more of my work here: "
             "https://github.com/drclcomputers/ASMOS");
}

static void do_shutdown(void) {
    power_anim_run(false);
    cpu_shutdown();
}
static void menu_shutdown(void) {
    dlg_confirm(NULL, "Shut Down",
                "Shut down ASMOS?\n\nAny unsaved work will be lost!",
                run_if_yes, (void *)do_shutdown);
}
static void do_restart(void) {
    power_anim_run(true);
    cpu_reset();
}
static void menu_restart(void) {
    dlg_confirm(NULL, "Restart",
                "Restart ASMOS?\n\nAny unsaved work will be lost!", run_if_yes,
                (void *)do_restart);
}

static void menu_eject_fdd(void) {
    int sel = selected_idx();
    if (sel >= 0) {
        desktop_item_t *it = &desktop_fs_items()[sel];
        if (it->kind == DESKTOP_ITEM_FDD) {
            if (fdd_eject(it->drive_id)) {
                desktop_fs_set_dirty();
                return;
            }
        }
    }
    dlg_message(NULL, "Eject", "No floppy selected or mounted.");
}

extern app_descriptor fdformat_app;
static void menu_format_fdd(void) { os_launch_app(&fdformat_app); }

static void menu_insert_fdd(void) {
    if (fdd_insert(DRIVE_FDD0) || fdd_insert(DRIVE_FDD1)) {
        desktop_fs_set_dirty();
    } else {
        dlg_message(NULL, "Insert Floppy",
                    "No floppy found or already mounted.");
    }
}

/* ── the Apps menu ──────────────────────────────────────────────────────── */
static menu *s_apps_menu;

/* The Apps menu mirrors the /APPS folder (.APPs and .LNK shortcuts):
 *   /APPS/X.APP          directly in Apps
 *   /APPS/GAMES/X.APP    in the built-in Games group (folders ACCESSOR,
 *                        GAMES, MULTIMED, SYSTEM: the built-in groups)
 *   /APPS/ACME/X.APP     a group of its own, "Acme" (or the first line of
 *                        /APPS/ACME/GROUP.TXT: "Acme Software")
 * Copy a .APP in to add it; delete it (or the folder) to take it away.
 * The built-in apps are not files: they can't be deleted. The menu is
 * rebuilt when files change (g_fs_rev), at most once a second. */
#define APPS_DIR "/APPS"
#define MAX_EXT_APPS 64
#define MAX_USER_GROUPS 10

typedef struct {
    char label[24];
    char path[64];        /* a .APP to load, or */
    app_descriptor *desc; /* a built-in app (from a shortcut) */
} ext_app_t;

static ext_app_t s_ext[MAX_EXT_APPS];
static int s_next;
static struct {
    char name[24]; /* shown */
    char dir[9];   /* folder */
} s_groups[MAX_USER_GROUPS];
static int s_ngroups;
static const char *const BUILTIN_DIRS[APP_GROUPS] = {"", "ACCESSOR", "GAMES",
                                                     "MULTIMED", "SYSTEM"};
static uint32_t s_apps_rev = 0xFFFFFFFF, s_apps_tick;

static void launch_ext(void *ud) {
    const ext_app_t *e = ud;
    if (e->desc)
        os_launch_app(e->desc);
    else if (!appload_launch(e->path))
        dlg_message(NULL, e->label, appload_last_error());
}

/* ── shortcuts (.LNK) ──────────────────────────────────────────────────
 * A text file: line 1 the target, line 2 (optional) the label.
 *   Calculator             a built-in app, by its name in the Apps menu
 *   /FDD0/GAME.APP         a .APP anywhere
 * so built-in apps can be put in groups too (`write X.LNK Calculator`);
 * deleting the shortcut leaves the app. */
static bool read_shortcut(const char *path, ext_app_t *x) {
    char buf[128];
    fat_file_t f;
    if (!fs_open(path, &f))
        return false;
    int n = fs_read(&f, buf, sizeof(buf) - 1);
    fs_close(&f);
    if (n <= 0)
        return false;
    buf[n] = 0;
    char *l1 = buf, *l2 = NULL;
    for (char *p = buf; *p; p++)
        if (*p == '\r' || *p == '\n') {
            *p++ = 0;
            while (*p == '\r' || *p == '\n')
                p++;
            l2 = p;
            for (char *q = l2; *q; q++)
                if (*q == '\r' || *q == '\n')
                    *q = 0;
            break;
        }
    while (*l1 == ' ')
        l1++;
    int e = (int)strlen(l1);
    while (e && l1[e - 1] == ' ')
        l1[--e] = 0;
    if (!*l1)
        return false;
    x->desc = NULL;
    x->path[0] = 0;
    for (int i = 0; i < app_registry_count && !x->desc; i++)
        if (app_registry[i].menu_label &&
            (!strcasecmp(l1, app_registry[i].menu_label) ||
             !strcasecmp(l1, app_registry[i].desc->name))) {
            x->desc = app_registry[i].desc;
            snprintf(x->label, sizeof(x->label), "%s",
                     app_registry[i].menu_label);
        }
    if (!x->desc) {
        snprintf(x->path, sizeof(x->path), "%s", l1);
        if (!appload_peek_name(x->path, x->label, sizeof(x->label)))
            return false; /* nothing there to run */
    }
    if (l2 && *l2)
        snprintf(x->label, sizeof(x->label), "%s", l2);
    return true;
}

/* FileF and the desktop: opening a .LNK runs its target */
bool desktop_open_shortcut(const char *path) {
    ext_app_t x;
    if (!read_shortcut(path, &x))
        return false;
    launch_ext(&x);
    return true;
}

/* "GAMES   " + "APP" -> "GAMES", and the 8.3 name as text */
static void name83(const dir_entry_t *e, char *out, bool with_ext) {
    int n = 0;
    for (int k = 0; k < 8 && e->name[k] != ' '; k++)
        out[n++] = (char)e->name[k];
    if (with_ext && e->ext[0] != ' ') {
        out[n++] = '.';
        for (int k = 0; k < 3 && e->ext[k] != ' '; k++)
            out[n++] = (char)e->ext[k];
    }
    out[n] = 0;
}

static bool is_ext(const dir_entry_t *e, const char *x) {
    return !(e->attr & (ATTR_DIRECTORY | 0x08)) && (e->ext[0] | 0x20) == x[0] &&
           (e->ext[1] | 0x20) == x[1] && (e->ext[2] | 0x20) == x[2];
}

static bool is_folder(const dir_entry_t *e) {
    return (e->attr & ATTR_DIRECTORY) && e->name[0] != '.' && e->name[0] != 0 &&
           e->name[0] != 0xE5;
}

/* the .APPs of one folder, as items of `m` */
static void add_folder_apps(menu *m, const char *dir) {
    uint8_t drive;
    uint16_t cluster;
    static dir_entry_t ents[48];
    int count = 0;
    if (!m || !fs_resolve_dir(dir, &drive, &cluster) ||
        !fs_list_dir(drive, cluster, ents, 48, &count))
        return;
    for (int i = 0; i < count && s_next < MAX_EXT_APPS; i++) {
        const dir_entry_t *e = &ents[i];
        bool app = is_ext(e, "app"), lnk = is_ext(e, "lnk");
        if (e->name[0] == 0 || e->name[0] == 0xE5 || !(app || lnk))
            continue;
        if (m->item_count >= MAX_MENU_ITEMS)
            break;
        ext_app_t *x = &s_ext[s_next];
        char fn[13], full[64];
        name83(e, fn, true);
        snprintf(full, sizeof(full), "%s/%s", dir, fn);
        if (lnk) {
            if (!read_shortcut(full, x))
                continue; /* a shortcut to nothing */
        } else {
            x->desc = NULL;
            snprintf(x->path, sizeof(x->path), "%s", full);
            if (!appload_peek_name(x->path, x->label, sizeof(x->label)))
                continue; /* not a .APP we can run */
        }
        s_next++;
        menu_add_item_ud(m, x->label, launch_ext, x);
    }
}

/* the folders in /APPS that aren't built-in groups: new groups */
static void find_groups(void) {
    s_ngroups = 0;
    uint8_t drive;
    uint16_t cluster;
    static dir_entry_t ents[48];
    int count = 0;
    if (!fs_resolve_dir(APPS_DIR, &drive, &cluster) ||
        !fs_list_dir(drive, cluster, ents, 48, &count))
        return;
    for (int i = 0; i < count && s_ngroups < MAX_USER_GROUPS; i++) {
        if (!is_folder(&ents[i]))
            continue;
        char dn[9];
        name83(&ents[i], dn, false);
        bool builtin = false;
        for (int g = 1; g < APP_GROUPS; g++)
            builtin |= !strcmp(dn, BUILTIN_DIRS[g]);
        if (builtin)
            continue;
        strcpy(s_groups[s_ngroups].dir, dn);
        /* the name: GROUP.TXT, else "ACME" -> "Acme" */
        char path[40], *nm = s_groups[s_ngroups].name;
        snprintf(path, sizeof(path), "%s/%s/GROUP.TXT", APPS_DIR, dn);
        fat_file_t f;
        int n = 0;
        if (fs_open(path, &f)) {
            n = fs_read(&f, nm, 23);
            fs_close(&f);
        }
        int k = 0;
        while (k < n && nm[k] != '\r' && nm[k] != '\n' && nm[k] >= 32)
            k++;
        nm[k] = 0;
        if (!k) {
            for (k = 0; dn[k]; k++)
                nm[k] = k ? (char)(dn[k] >= 'A' && dn[k] <= 'Z' ? dn[k] + 32
                                                                : dn[k])
                          : dn[k];
            nm[k] = 0;
        }
        /* keep them sorted by name */
        int at = s_ngroups++;
        while (at > 0 && strcmp(s_groups[at - 1].name, s_groups[at].name) > 0) {
            char tn[24], td[9];
            strcpy(tn, s_groups[at].name), strcpy(td, s_groups[at].dir);
            strcpy(s_groups[at].name, s_groups[at - 1].name);
            strcpy(s_groups[at].dir, s_groups[at - 1].dir);
            strcpy(s_groups[at - 1].name, tn), strcpy(s_groups[at - 1].dir, td);
            at--;
        }
    }
}

void desktop_refresh_apps_menu(void) {
    if (!s_apps_menu)
        return;
    menubar_close_all(&g_menubar); /* no dropdown on the old submenus */
    menu_clear(s_apps_menu);
    s_next = 0;
    s_apps_rev = g_fs_rev;
    char dir[24];
    /* the built-in groups, with the .APPs of their folders */
    for (int g = 1; g < APP_GROUPS; g++) {
        menu *sub = menu_add_submenu(s_apps_menu, (char *)APP_GROUP_NAMES[g]);
        for (int i = 0; sub && i < app_registry_count; i++)
            if (app_registry[i].group == g && app_registry[i].menu_label)
                menu_add_item_ud(sub, (char *)app_registry[i].menu_label,
                                 launch_app_ud, app_registry[i].desc);
        snprintf(dir, sizeof(dir), "%s/%s", APPS_DIR, BUILTIN_DIRS[g]);
        add_folder_apps(sub, dir);
    }
    /* the groups made as folders */
    find_groups();
    for (int i = 0; i < s_ngroups; i++) {
        menu *sub = menu_add_submenu(s_apps_menu, s_groups[i].name);
        snprintf(dir, sizeof(dir), "%s/%s", APPS_DIR, s_groups[i].dir);
        add_folder_apps(sub, dir);
    }
    menu_add_separator(s_apps_menu);
    /* the .APPs in /APPS itself, then the built-ins that sit here */
    add_folder_apps(s_apps_menu, APPS_DIR);
    for (int i = 0; i < app_registry_count; i++)
        if (app_registry[i].group == APP_TOP && app_registry[i].menu_label)
            menu_add_item_ud(s_apps_menu, (char *)app_registry[i].menu_label,
                             launch_app_ud, app_registry[i].desc);
}

/* files changed: rebuild, once a second at most, never under an open menu */
static void apps_menu_follow(void) {
    if (g_fs_rev == s_apps_rev || g_menubar.open_index >= 0 ||
        pit_ticks - s_apps_tick < 1000)
        return;
    s_apps_tick = pit_ticks;
    desktop_refresh_apps_menu();
}

/* ── desktop init ───────────────────────────────────────────────────────── */

void desktop_init(void) {
    static window_spec_t spec = {
        .x = 0,
        .y = 0,
        .title = "",
        .visible = true,
        .pinned_bottom = true,
    };
    spec.w = SCREEN_WIDTH;
    spec.h = SCREEN_HEIGHT - MENUBAR_H - TASKBAR_H;
    window *win = wm_register(&spec);
    if (!win)
        return;
    win->visible_buttons = false;
    win->pinned_bottom = true;
    win->show_order = -1;

    s_apps_menu = window_add_menu(win, "Apps");
    desktop_refresh_apps_menu(); /* built from /APPS: see above */

    menu *file_menu = window_add_menu(win, "File");
    menu_add_item(file_menu, "New File", menu_new_file);
    menu_add_item(file_menu, "New Folder", menu_new_folder);
    menu_add_item(file_menu, "Rename", menu_rename);
    menu_add_item(file_menu, "Get Info", menu_info);
    menu_add_item(file_menu, "Reload", menu_reload);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "About OS", menu_about_os);

    menu *edit_menu = window_add_menu(win, "Edit");
    menu_add_item(edit_menu, "Copy", menu_copy);
    menu_add_item(edit_menu, "Cut", menu_cut);
    menu_add_item(edit_menu, "Paste", menu_paste);
    menu_add_separator(edit_menu);
    menu_add_item(edit_menu, "Delete", menu_delete);

    menu *action_menu = window_add_menu(win, "Action");
    menu_add_item(action_menu, "Insert Floppy", menu_insert_fdd);
    menu_add_item(action_menu, "Eject Floppy", menu_eject_fdd);
    menu_add_item(action_menu, "Format Floppy...", menu_format_fdd);
    menu_add_separator(action_menu);
    menu_add_item(action_menu, "Restart", menu_restart);
    menu_add_item(action_menu, "Shut Down", menu_shutdown);

    desktop_fs_init();

    clipboard_init();
}

/* ── per-frame ──────────────────────────────────────────────────────────── */
static void draw_items(int count, desktop_item_t *items) {
    for (int i = 0; i < count; i++) {
        if (!items[i].used)
            continue;
        if (i == s_drag_idx)
            continue;
        draw_desktop_item(&items[i], false);
    }
    if (s_drag_idx >= 0 && s_drag_idx < count)
        draw_desktop_item(&items[s_drag_idx], false);
}

/* ── context menus ─────────────────────────────────────────────────────── */

static int s_ctx_idx = -1; /* icon the menu was opened on */

static void ctx_open(void) {
    if (s_ctx_idx >= 0 && s_ctx_idx < desktop_fs_count())
        open_item(s_ctx_idx);
}

static void ctx_paste_into(void) {
    char p[270];
    if (s_ctx_idx >= 0 && desktop_fs_item_path(s_ctx_idx, p, sizeof(p)))
        fo_report(NULL, "Paste", fo_paste(p));
}

static void ctx_empty_trash(void) { fo_ask_empty_trash(NULL, NULL, NULL); }

static void context_menu(int hit, int x, int y) {
    desktop_item_t *items = desktop_fs_items();
    s_ctx_idx = hit;
    menu *m = popup_menu();
    if (hit < 0) {
        menu_add_item(m, "New File", menu_new_file);
        menu_add_item(m, "New Folder", menu_new_folder);
        menu_add_separator(m);
        menu_add_item(m, "Paste", menu_paste);
        menu_enable_last(m, clipboard_has_file());
        menu_add_separator(m);
        menu_add_item(m, "Reload", menu_reload);
        menu_add_item(m, "About OS", menu_about_os);
    } else if (items[hit].kind == DESKTOP_ITEM_TRASH) {
        menu_add_item(m, "Open", ctx_open);
        menu_add_separator(m);
        menu_add_item(m, "Empty Trash...", ctx_empty_trash);
        menu_enable_last(m, items[hit].full);
    } else if (items[hit].kind == DESKTOP_ITEM_FDD) {
        menu_add_item(m, "Open", ctx_open);
        if (items[hit].drive_id == DRIVE_FDD0) {
            menu_add_separator(m);
            menu_add_item(m, "Eject Floppy", menu_eject_fdd);
        }
    } else {
        menu_add_item(m, "Open", ctx_open);
        menu_add_separator(m);
        menu_add_item(m, "Cut", menu_cut);
        menu_add_item(m, "Copy", menu_copy);
        if (items[hit].kind == DESKTOP_ITEM_DIR) {
            menu_add_item(m, "Paste Into", ctx_paste_into);
            menu_enable_last(m, clipboard_has_file());
        }
        menu_add_separator(m);
        menu_add_item(m, "Rename", menu_rename);
        menu_add_item(m, "Delete", menu_delete);
        menu_add_item(m, "Get Info", menu_info);
    }
    popup_show(x, y);
}

static void select_only(int hit) {
    desktop_item_t *items = desktop_fs_items();
    int count = desktop_fs_count();
    for (int i = 0; i < count; i++)
        items[i].selected = (i == hit);
}

/* what desktop_draw() would show, as a checksum (lazy redraw, window.h) */
uint32_t desktop_sig(void) {
    uint32_t h = 5381;
#define DMIX(v) (h = h * 33u ^ (uint32_t)(v))
    DMIX(g_cfg.wallpaper_pattern);
    DMIX(g_cfg.wallpaper_main_color << 8 | g_cfg.wallpaper_secondary_color);
    DMIX(s_drag_idx);
    DMIX(clipboard_has_file() << 1 | g_clipboard.is_cut);
    for (const char *p = g_clipboard.name; *p; p++)
        DMIX((uint8_t)*p);
    desktop_item_t *items = desktop_fs_items();
    int count = desktop_fs_count();
    DMIX(count);
    for (int i = 0; i < count; i++) {
        const desktop_item_t *it = &items[i];
        DMIX(it->used | it->selected << 1 | it->full << 2 | it->kind << 8);
        if (!it->used)
            continue;
        DMIX(it->x);
        DMIX(it->y);
        for (const char *p = it->name; *p; p++)
            DMIX((uint8_t)*p);
    }
    return h;
#undef DMIX
}

/* wallpaper and icons; the frame's logic is desktop_on_frame() */
void desktop_draw(void) {
    draw_wallpaper_pattern();
    draw_items(desktop_fs_count(), desktop_fs_items());
}

void desktop_on_frame(void) {
    apps_menu_follow();
    if (desktop_fs_is_dirty())
        desktop_fs_reload();

    desktop_item_t *items = desktop_fs_items();
    int count = desktop_fs_count();

    /* ── drag * ──────────────────────────────────────────────────────────── */
    if (s_drag_idx >= 0) {
        desktop_item_t *it = &items[s_drag_idx];
        if (mouse.left) {
            int new_x = mouse.x - DESK_ORIGIN_X - s_drag_off_x;
            int new_y = mouse.y - DESK_ORIGIN_Y - s_drag_off_y;
            if (new_x < 0)
                new_x = 0;
            if (new_y < 0)
                new_y = 0;
            if (new_x > SCREEN_WIDTH - ICON_W)
                new_x = SCREEN_WIDTH - ICON_W;
            if (new_y >
                SCREEN_HEIGHT - MENUBAR_H - ICON_H - ICO_LABEL_H - TASKBAR_H)
                new_y = SCREEN_HEIGHT - MENUBAR_H - ICON_H - ICO_LABEL_H -
                        TASKBAR_H;
            it->x = new_x;
            it->y = new_y;
        } else {
            /* dropped on a FileF window or on a desktop folder: move the
             * file there; anywhere else just places the icon */
            int idx = s_drag_idx;
            s_drag_idx = -1;
            wm_clear_overlay(&s_drag_idx);
            char src[270], dst[270];
            int target = desktop_hit_except(mouse.x, mouse.y, idx);
            bool into = false;
            if (desktop_fs_item_path(idx, src, sizeof(src))) {
                if (ff_folder_at(mouse.x, mouse.y, dst, sizeof(dst)))
                    into = true;
                else if (target >= 0 && target != idx &&
                         (items[target].kind == DESKTOP_ITEM_DIR ||
                          items[target].kind == DESKTOP_ITEM_TRASH) &&
                         desktop_fs_item_path(target, dst, sizeof(dst)))
                    into = true; /* onto the Trash: fo_move throws it out */
            }
            if (into)
                fo_report(NULL, "Move", fo_move(src, dst));
            else
                desktop_fs_move_icon(idx, it->x, it->y);
        }
    }

    /* Start a drag once the pointer moves with the button held on a
     * selected icon. Decided from where the button went down: on a slow
     * machine one frame can carry the pointer well off the icon. */
    if (mouse.left && !mouse.left_clicked && s_drag_idx < 0 &&
        s_press_idx >= 0 && s_press_idx < count &&
        items[s_press_idx].selected &&
        (mouse.x != s_press_x || mouse.y != s_press_y)) {
        int i = s_press_idx;
        s_drag_off_x = s_press_x - (DESK_ORIGIN_X + items[i].x);
        s_drag_off_y = s_press_y - (DESK_ORIGIN_Y + items[i].y);
        s_drag_idx = i;
        s_press_idx = -1;
        wm_set_overlay(draw_drag_overlay, &s_drag_idx);
        s_last_click_idx = -1;
    }
    if (!mouse.left)
        s_press_idx = -1;

    if (mouse.left_clicked && mouse.y >= MENUBAR_H &&
        g_menubar.open_index < 0 && !g_menubar_click_consumed) {
        int hit = desktop_hit(mouse.x, mouse.y);
        s_press_idx = hit;
        s_press_x = mouse.x;
        s_press_y = mouse.y;
        if (hit >= 0) {
            for (int i = 0; i < count; i++)
                items[i].selected = false;
            items[hit].selected = true;

            uint32_t now = pit_ticks;
            if (hit == s_last_click_idx &&
                (now - s_last_click_tick) <= DOUBLE_CLICK_SPEED) {
                open_item(hit);
                s_press_idx = -1; /* no drag out of a double-click */
                s_last_click_idx = -1;
                s_last_click_tick = 0;
            } else {
                s_last_click_idx = hit;
                s_last_click_tick = now;
            }
        } else {
            for (int i = 0; i < count; i++)
                items[i].selected = false;
            s_last_click_idx = -1;
        }
    }

    /* right-click on the desktop (windows already took theirs) */
    if (mouse.right_clicked && mouse.y >= MENUBAR_H &&
        mouse.y < SCREEN_HEIGHT - TASKBAR_H && !g_menubar_click_consumed &&
        s_drag_idx < 0) {
        mouse.right_clicked = false;
        int hit = desktop_hit(mouse.x, mouse.y);
        select_only(hit);
        s_last_click_idx = -1;
        context_menu(hit, mouse.x, mouse.y);
    }

    if (is_desktop_focused() && kb.key_pressed &&
        (kb.last_scancode == MENU_KEY ||
         (kb.last_scancode == F10 && kb.shift))) {
        kb.key_pressed = false;
        int sel = selected_idx();
        if (sel >= 0)
            context_menu(sel, DESK_ORIGIN_X + items[sel].x + ICON_W / 2,
                         DESK_ORIGIN_Y + items[sel].y + ICON_H);
        else
            context_menu(-1, SCREEN_WIDTH / 3, SCREEN_HEIGHT / 3);
    }

    if (is_desktop_focused() && kb.key_pressed) {
        if (kb.ctrl_c) {
            menu_copy();
        } else if (kb.ctrl_x) {
            menu_cut();
        } else if (kb.ctrl_v) {
            menu_paste();
        } else if (kb.last_scancode == DELETE) {
            menu_delete();
        }
    }

    if (g_alarm.set && !g_alarm.fired) {
        time_full_t t = time_rtc_local();
        if (t.hours == g_alarm.hour && t.minutes == g_alarm.minute &&
            t.seconds == 0) {
            g_alarm.fired = true;
            g_alarm.set = false;
            speaker_beep(880, 300);
            speaker_beep(660, 300);
            speaker_beep(880, 400);
            dlg_message(NULL, "Alarm", "Alarm time reached!");
        }
    }
}
