#include "ui/desktop.h"
#include "drivers/ata.h"
#include "drivers/cdrom.h"
#include "drivers/fdd.h"
#include "drivers/gpu.h"
#include "os/help.h"
#include "os/watchdog.h"
#include "ui/cursor.h"
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

// wallpaper
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
        static uint8_t row[800];
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
    case WALLPAPER_CUSTOM: {
        /* eight rows across the screen, then copies of them */
        static uint8_t rows[8][800];
        for (int r = 0; r < 8; r++)
            for (int x = 0; x < SCREEN_WIDTH; x++)
                rows[r][x] = cfg_wall_px(x, r);
        for (int y = 0; y < SCREEN_HEIGHT; y++)
            draw_pixels(0, y, SCREEN_WIDTH, 1, rows[y & 7], SCREEN_WIDTH);
        break;
    }
    default:
        clear_screen(main_col);
        break;
    }
}

// desktop state
#define DESK_ORIGIN_X 0
#define DESK_ORIGIN_Y MENUBAR_H

static int s_drag_idx = -1;
static int s_press_idx = -1, s_press_x, s_press_y; /* button-down spot */
static int s_drag_off_x = 0;
static int s_drag_off_y = 0;
static int s_last_click_idx = -1;
static uint32_t s_last_click_tick = 0;

// shut down / restart

/* Show a final status while saving settings, flushing floppies, and stopping
 * drives; the caller then powers off or restarts. */
void power_anim_run(bool is_restart) {
    wd_stop(); /* no pointer from here on */
    const char *msg = is_restart ? "Restarting..." : "Shutting down...";
    uint8_t col = is_restart ? LIGHT_GREEN : LIGHT_CYAN;
    clear_screen(BLACK);
    draw_string((SCREEN_WIDTH - text_width(msg, UI_FONT_BOLD)) / 2,
                SCREEN_HEIGHT / 2 - 4, (char *)msg, col, UI_FONT_BOLD);
    gpu_invalidate();
    blit();
    uint32_t t0 = time_millis();

    cfg_save();          /* (not in Setup: nothing is installed yet) */
    wm_save_positions(); /* the open windows come back there */
    bool spun = false;
    for (uint8_t d = 0; d < 2; d++) {
        fdd_flush(d);
        spun |= fdd_motor_running(d);
        fdd_motor_off(d);
    }
    ata_park(!is_restart);
    if (spun) /* a floppy motor takes a moment to run down */
        sleep_ms(700);
    uint32_t took = time_millis() - t0;
    if (took < 1000) /* long enough to read it */
        sleep_ms(1000 - took);
}

// open item

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
    char path[FS_PATH_SZ];
    if (desktop_fs_item_path(idx, path, sizeof(path)))
        fo_open(NULL, path);
}

// drawing

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
        const char *dot = strrchr(it->name, '.');
        const char *ext = dot ? dot + 1 : "";
        if (strcasecmp(ext, "PIC") == 0)
            draw_pic_icon(ax, ay, it->selected);
        else if (strcasecmp(ext, "TXT") == 0)
            draw_txt_icon(ax, ay, it->selected);
        else if (strcasecmp(ext, "CFG") == 0)
            draw_cfg_icon(ax, ay, it->selected);
        else if (strcasecmp(ext, "ZIP") == 0)
            draw_zip_icon(ax, ay, it->selected);
        else
            draw_unknown_icon(ax, ay, it->selected);
        break;
    }
    case DESKTOP_ITEM_FDD:
        if (it->drive_id == DRIVE_CD0)
            draw_cd_icon(ax, ay, it->selected);
        else
            draw_floppy_icon(ax, ay, it->selected);
        break;
    case DESKTOP_ITEM_TRASH:
        draw_trash_icon(ax, ay, it->selected, it->full);
        break;
    default:
        draw_file_icon(ax, ay, it->selected);
        break;
    }

    /* A long name is cut to the icon's column with "..."; the selected
     * icon shows it whole, on a dark band (like Windows 95). */
    char disp[DESKTOP_NAME_MAX + 4];
    int room = ICON_SPACING_X + 4; /* as wide as 8 characters were */
    snprintf(disp, sizeof(disp), "%s", it->name);
    int lw = text_width(disp, UI_FONT_SMALL);
    bool whole = it->selected && !dragged_ghost;
    if (!whole && lw > room) {
        int n = (int)strlen(disp);
        while (n > 1 && text_width_n(disp, n, UI_FONT_SMALL) +
                                text_width("...", UI_FONT_SMALL) >
                            room)
            n--;
        strcpy(disp + n, "...");
        lw = text_width(disp, UI_FONT_SMALL);
    }
    int lx = ax + ICON_W / 2 - lw / 2;
    int ly = ay + ICON_H + 2;
    if (lx < 1)
        lx = 1;
    if (lx + lw > SCREEN_WIDTH - 1)
        lx = SCREEN_WIDTH - 1 - lw;

    uint8_t label_col = is_cut ? DARK_GRAY : WHITE;
    if (whole && text_width(it->name, UI_FONT_SMALL) > room)
        fill_rect(lx - 1, ly - 1, lw + 2, font_height(UI_FONT_SMALL) + 2,
                  BLACK);
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

// menu actions: file operations go through ui/fileops

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
    char p[FS_PATH_SZ];
    if (sel_path("Rename", p, sizeof(p)))
        fo_ask_rename(NULL, p, NULL, NULL);
}
static void menu_delete(void) {
    char p[FS_PATH_SZ];
    if (sel_path("Delete", p, sizeof(p)))
        fo_ask_delete(NULL, p, NULL, NULL);
}
static void menu_info(void) {
    char p[FS_PATH_SZ];
    if (sel_path("Get Info", p, sizeof(p)))
        fo_show_info(NULL, p);
}
static void menu_reload(void) { desktop_fs_set_dirty(); }
static void menu_compress(void) {
    char p[FS_PATH_SZ];
    if (sel_path("Compress", p, sizeof(p)))
        fo_ask_compress(NULL, p, NULL, NULL);
}
static void menu_extract(void) {
    char p[FS_PATH_SZ];
    if (!sel_path("Extract", p, sizeof(p)))
        return;
    bool dir;
    if (fo_is_zip(p) && fo_stat(p, &dir, NULL) && !dir)
        fo_ask_extract(NULL, p, NULL, NULL);
    else
        dlg_message(NULL, "Extract", "Select a .ZIP file first.");
}
static void menu_copy(void) {
    char p[FS_PATH_SZ];
    if (sel_path("Copy", p, sizeof(p)))
        fo_report(NULL, "Copy", fo_clip(p, false));
}
static void menu_cut(void) {
    char p[FS_PATH_SZ];
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

/* the selected floppy, or else the one with a disk in */
static void menu_eject_fdd(void) {
    uint8_t d = DRIVE_COUNT;
    int sel = selected_idx();
    if (sel >= 0 && desktop_fs_items()[sel].kind == DESKTOP_ITEM_FDD &&
        fs_drive_mounted(desktop_fs_items()[sel].drive_id))
        d = desktop_fs_items()[sel].drive_id;
    for (uint8_t i = DRIVE_FDD0; i <= DRIVE_FDD1 && d == DRIVE_COUNT; i++)
        if (fs_drive_mounted(i))
            d = i;
    if (d != DRIVE_COUNT && fdd_eject(d)) {
        desktop_fs_set_dirty();
        return;
    }
    dlg_message(NULL, "Eject", "There is no floppy disk in the drive.");
}

/* opens the tray; the disc's icon and apps go at once */
static void menu_eject_cd(void) {
    if (!cd_eject())
        dlg_message(NULL, "Eject", "The CD drive did not open.");
    fs_unmount(DRIVE_CD0);
    desktop_cd_links();
    desktop_fs_set_dirty();
}

extern app_descriptor fdformat_app;
static void menu_format_disk(void) { os_launch_app(&fdformat_app); }
static void menu_help(void) { help_open_app("ASMOS"); }

// the Apps menu
static menu *s_apps_menu;

/* The Apps menu mirrors /APPS: .APP/.LNK files are entries, folders are
 * groups named by folder or GROUP.TXT. Built-ins are shortcuts; defaults
 * return at boot, and file changes rebuild the sorted menu once per second. */
#define APPS_DIR "/APPS"
#define MAX_EXT_APPS 64
#define MAX_GROUPS 12

typedef struct {
    char label[24];
    char path[192];       /* a .APP to load, or */
    app_descriptor *desc; /* a built-in app (from a shortcut) */
} ext_app_t;

static ext_app_t s_ext[MAX_EXT_APPS];
static int s_next;
static struct {
    char name[24];        /* shown */
    char dir[FS_NAME_SZ]; /* folder */
} s_groups[MAX_GROUPS];
static int s_ngroups;
static uint32_t s_apps_rev = 0xFFFFFFFF, s_apps_tick;

static void launch_ext(void *ud) {
    const ext_app_t *e = ud;
    if (e->desc)
        os_launch_app(e->desc);
    else if (!appload_launch(e->path))
        dlg_message(NULL, e->label, appload_last_error());
}

// shortcuts (.LNK)
/*
 * A .LNK contains a target on line 1 and an optional label on line 2.
 * Targets may be built-in names, .APP names, or absolute paths. Shortcuts
 * can place apps in any group; deleting one does not delete its target.
 */
static bool read_shortcut(const char *path, ext_app_t *x) {
    char buf[256];
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
        if (strchr(l1, '/')) /* a path */
            snprintf(x->path, sizeof(x->path), "%s", l1);
        else if (!appload_find_named(l1, x->path, sizeof(x->path)))
            return false; /* no such app in /APPS */
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

static bool is_ext(const dir_entry_t *e, const char *x) {
    return !(e->attr & (ATTR_DIRECTORY | 0x08)) && (e->ext[0] | 0x20) == x[0] &&
           (e->ext[1] | 0x20) == x[1] && (e->ext[2] | 0x20) == x[2];
}

static bool is_folder(const dir_entry_t *e) {
    return (e->attr & ATTR_DIRECTORY) && e->name[0] != '.' && e->name[0] != 0 &&
           e->name[0] != 0xE5 && !(e->attr & ATTR_HIDDEN);
}

/* the .APPs and shortcuts of one folder, sorted by label, as items of m */
static void add_folder_apps(menu *m, const char *dir) {
    uint8_t drive;
    uint16_t cluster;
    static dir_entry_t ents[48];
    static char names[48][FS_NAME_SZ];
    int count = 0;
    if (!m || !fs_resolve_dir(dir, &drive, &cluster) ||
        !fs_list_dir_names(drive, cluster, ents, names, 48, &count))
        return;
    int first = s_next;
    for (int i = 0; i < count && s_next < MAX_EXT_APPS; i++) {
        const dir_entry_t *e = &ents[i];
        bool app = is_ext(e, "app"), lnk = is_ext(e, "lnk");
        if (e->name[0] == 0 || e->name[0] == 0xE5 || !(app || lnk))
            continue;
        if (s_next - first + m->item_count >= MAX_MENU_ITEMS)
            break;
        ext_app_t *x = &s_ext[s_next];
        char full[192];
        snprintf(full, sizeof(full), "%s/%s", dir, names[i]);
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
    }
    for (int i = first + 1; i < s_next; i++) /* by label */
        for (int j = i;
             j > first && strcasecmp(s_ext[j - 1].label, s_ext[j].label) > 0;
             j--) {
            ext_app_t t = s_ext[j];
            s_ext[j] = s_ext[j - 1];
            s_ext[j - 1] = t;
        }
    for (int i = first; i < s_next; i++)
        menu_add_item_ud(m, s_ext[i].label, launch_ext, &s_ext[i]);
}

/* the folders in /APPS: the groups, sorted by name */
static void find_groups(void) {
    s_ngroups = 0;
    uint8_t drive;
    uint16_t cluster;
    static dir_entry_t ents[48];
    static char names[48][FS_NAME_SZ];
    int count = 0;
    if (!fs_resolve_dir(APPS_DIR, &drive, &cluster) ||
        !fs_list_dir_names(drive, cluster, ents, names, 48, &count))
        return;
    for (int i = 0; i < count && s_ngroups < MAX_GROUPS; i++) {
        if (!is_folder(&ents[i]))
            continue;
        char *dn = names[i];
        strcpy(s_groups[s_ngroups].dir, dn);
        /* the name: GROUP.TXT, else the folder's ("ACME" -> "Acme") */
        char path[FS_PATH_SZ], *nm = s_groups[s_ngroups].name;
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
            bool upper = fs_name_is_83(dn);
            for (int c = 0; dn[c]; c++)
                if (dn[c] >= 'a' && dn[c] <= 'z')
                    upper = false; /* "Games" stays as it is */
            for (k = 0; dn[k] && k < 23; k++)
                nm[k] = upper && k
                            ? (char)(dn[k] >= 'A' && dn[k] <= 'Z' ? dn[k] + 32
                                                                  : dn[k])
                            : dn[k];
            nm[k] = 0;
        }
        int at = s_ngroups++;
        while (at > 0 &&
               strcasecmp(s_groups[at - 1].name, s_groups[at].name) > 0) {
            char tn[24], td[FS_NAME_SZ];
            strcpy(tn, s_groups[at].name), strcpy(td, s_groups[at].dir);
            strcpy(s_groups[at].name, s_groups[at - 1].name);
            strcpy(s_groups[at].dir, s_groups[at - 1].dir);
            strcpy(s_groups[at - 1].name, tn), strcpy(s_groups[at - 1].dir, td);
            at--;
        }
    }
}

/* /APPS gone (a new disk, or deleted): make it again with a shortcut for
 * each built-in app in its group (the .APPs come with the disk image) */
static void apps_dir_defaults(void) {
    dir_entry_t de;
    if (g_setup_mode || fs_find(APPS_DIR, &de) || !fs_mkdir(APPS_DIR))
        return;
    char path[FS_PATH_SZ], text[40];
    for (int g = 1; g < APP_GROUPS; g++) {
        snprintf(path, sizeof(path), "%s/%s", APPS_DIR, APP_GROUP_NAMES[g]);
        fs_mkdir(path);
    }
    for (int i = 0; i < app_registry_count; i++) {
        const registered_app_t *r = &app_registry[i];
        if (!r->menu_label || r->setup_only || r->hidden)
            continue;
        if (r->group == APP_TOP)
            snprintf(path, sizeof(path), "%s/%s.LNK", APPS_DIR, r->menu_label);
        else
            snprintf(path, sizeof(path), "%s/%s/%s.LNK", APPS_DIR,
                     APP_GROUP_NAMES[r->group], r->menu_label);
        int n = snprintf(text, sizeof(text), "%s\r\n", r->menu_label);
        fat_file_t f;
        if (fs_create(path, &f)) {
            fs_write(&f, text, n);
            fs_close(&f);
        }
    }
}

void apps_dir_defaults_pub(void) { apps_dir_defaults(); } /* sim tests */

void desktop_refresh_apps_menu(void) {
    if (!s_apps_menu)
        return;
    menubar_close_all(&g_menubar); /* no dropdown on the old submenus */
    menu_clear(s_apps_menu);
    s_next = 0;
    s_apps_rev = g_fs_rev;
    dir_entry_t de;
    if (g_setup_mode || !fs_find(APPS_DIR, &de)) { /* the built-ins */
        for (int i = 0; i < app_registry_count; i++)
            if (app_registry[i].menu_label && !app_registry[i].hidden &&
                (!app_registry[i].setup_only || g_setup_mode))
                menu_add_item_ud(s_apps_menu,
                                 (char *)app_registry[i].menu_label,
                                 launch_app_ud, app_registry[i].desc);
        return;
    }
    find_groups();
    char dir[FS_PATH_SZ];
    for (int i = 0; i < s_ngroups; i++) {
        menu *sub = menu_add_submenu(s_apps_menu, s_groups[i].name);
        snprintf(dir, sizeof(dir), "%s/%s", APPS_DIR, s_groups[i].dir);
        add_folder_apps(sub, dir);
    }
    add_folder_apps(s_apps_menu, APPS_DIR); /* the ones in /APPS itself */
}

// CD apps in /APPS/CDROM
/*
 * On insertion, create shortcuts for .APPs in the disc root and /APPS
 * folders. CDLINKS.LST tracks them for cleanup on eject or boot; preserve
 * /APPS/CDROM if it contains user files. */
#define CD_APPS_DIR APPS_DIR "/CDROM"
#define CD_LIST CD_APPS_DIR "/CDLINKS.LST"
#define CD_APPS_MAX 32
#define CD_LIST_SZ ((CD_APPS_MAX + 1) * 64) /* short names: < 64 a line */

typedef struct {
    int made;
    char *list; /* the paths made, a line each */
    int len;
} cd_links_ctx;

static void cd_made(cd_links_ctx *c, const char *path) {
    if ((int)strlen(path) + 3 > CD_LIST_SZ - c->len)
        return;
    c->len += snprintf(c->list + c->len, CD_LIST_SZ - c->len, "%s\r\n", path);
    if (c->len > CD_LIST_SZ - 1)
        c->len = CD_LIST_SZ - 1;
}

/* the files the last disc's list names (in the folder only), the list,
 * then the folder if it is empty */
static void cd_links_remove(void) {
    static char t[CD_LIST_SZ];
    fat_file_t f;
    int n = 0;
    if (fs_open(CD_LIST, &f)) {
        n = fs_read(&f, t, sizeof(t) - 1);
        fs_close(&f);
    }
    t[n > 0 ? n : 0] = 0;
    const int dl = (int)strlen(CD_APPS_DIR "/");
    char *p = t;
    while (*p) {
        char *e = p;
        while (*e && *e != '\r' && *e != '\n')
            e++;
        char end = *e;
        *e = 0;
        if (!strncmp(p, CD_APPS_DIR "/", dl) && !strchr(p + dl, '/'))
            fs_delete(p);
        p = end ? e + 1 : e;
    }
    fs_delete(CD_LIST);
    uint8_t drive;
    uint16_t cl;
    if (fs_resolve_dir(CD_APPS_DIR, &drive, &cl) && fs_is_dir_empty(drive, cl))
        fs_rmdir(CD_APPS_DIR);
}

static void cd_link(const char *app_path, cd_links_ctx *c) {
    char label[24], name[FS_NAME_SZ], path[FS_PATH_SZ], text[FS_PATH_SZ + 32];
    if (c->made >= CD_APPS_MAX ||
        !appload_peek_name(app_path, label, sizeof(label)))
        return;
    int k = 0; /* the label, as a file name */
    for (const char *p = label; *p && k < 24; p++)
        name[k++] = fs_new_name_char_ok(*p) ? *p : '_';
    name[k] = 0;
    const char *err;
    if (!k || !fs_validate_name(name, false, &err))
        snprintf(name, sizeof(name), "CDAPP%d", c->made + 1);
    snprintf(path, sizeof(path), "%s/%s.LNK", CD_APPS_DIR, name);
    for (int n = 2; n < 10; n++) { /* two apps with one name */
        dir_entry_t de;
        if (!fs_find(path, &de))
            break;
        snprintf(path, sizeof(path), "%s/%s_%d.LNK", CD_APPS_DIR, name, n);
    }
    dir_entry_t de;
    if (fs_find(path, &de))
        return; /* never over a file that is there */
    int len = snprintf(text, sizeof(text), "%s\r\n%s\r\n", app_path, label);
    fat_file_t f;
    if (fs_create(path, &f)) {
        fs_write(&f, text, len);
        fs_close(&f);
        c->made++;
        cd_made(c, path);
    }
}

typedef struct {
    const char *dir;
    cd_links_ctx *c;
    bool sub; /* also the folders in it */
} cd_scan_ctx;

static void cd_scan(const char *dir, cd_links_ctx *c, bool sub);

static bool cd_scan_cb(const dir_entry_t *e, const char *name, void *user) {
    cd_scan_ctx *s = user;
    char p[FS_PATH_SZ];
    snprintf(p, sizeof(p), "%s/%s", s->dir, name);
    if ((e->attr & ATTR_DIRECTORY) && s->sub)
        cd_scan(p, s->c, false);
    else if (is_ext(e, "app"))
        cd_link(p, s->c);
    return s->c->made < CD_APPS_MAX;
}

static void cd_scan(const char *dir, cd_links_ctx *c, bool sub) {
    uint8_t drive;
    uint16_t cluster;
    if (!fs_resolve_dir(dir, &drive, &cluster))
        return;
    cd_scan_ctx s = {dir, c, sub};
    fs_walk_dir(drive, cluster, cd_scan_cb, &s);
}

void desktop_cd_links(void) {
    if (g_setup_mode)
        return; /* nothing is written to the hard disk from the floppy */
    dir_entry_t de;
    if (fs_find(CD_APPS_DIR, &de) && (de.attr & ATTR_DIRECTORY))
        cd_links_remove();
    if (!fs_drive_mounted(DRIVE_CD0) || !fs_find(APPS_DIR, &de))
        return;
    static char list[CD_LIST_SZ];
    cd_links_ctx c = {0, list, 0};
    bool had = fs_find(CD_APPS_DIR, &de); /* yours: kept as it is */
    if (had && !(de.attr & ATTR_DIRECTORY))
        return; /* a file with that name */
    if (!had && !fs_mkdir(CD_APPS_DIR))
        return;
    cd_scan("/CD0", &c, false);
    cd_scan("/CD0/APPS", &c, true);
    fat_file_t f;
    if (c.made && !fs_find(CD_APPS_DIR "/GROUP.TXT", &de) &&
        fs_create(CD_APPS_DIR "/GROUP.TXT", &f)) { /* its name in the menu */
        fs_write(&f, "CDROM\r\n", 7);
        fs_close(&f);
        cd_made(&c, CD_APPS_DIR "/GROUP.TXT");
    }
    if (c.made && fs_create(CD_LIST, &f)) {
        fs_write(&f, list, c.len);
        fs_close(&f);
        fs_set_attr(CD_LIST, ATTR_HIDDEN);
    }
    if (!c.made && !had)
        fs_rmdir(CD_APPS_DIR); /* a disc without apps: no empty group */
}

/* a disc in or out: its drive icon, its apps */
static void cd_follow(void) {
    if (!fs_cd_poll())
        return;
    desktop_cd_links();
    desktop_fs_set_dirty();
}

/* a floppy taken out without Eject */
static void fdd_follow(void) {
    uint8_t lost;
    if (!fs_fdd_poll(&lost))
        return;
    desktop_fs_set_dirty();
    if (lost != DRIVE_COUNT)
        dlg_message(NULL, lost == DRIVE_FDD0 ? "FDD0" : "FDD1",
                    "The disk was taken out before everything was "
                    "written to it. Put it back and save again.");
}

/* files changed: rebuild, once a second at most, never under an open menu */
static void apps_menu_follow(void) {
    if (g_fs_rev == s_apps_rev || g_menubar.open_index >= 0 ||
        pit_ticks - s_apps_tick < 1000)
        return;
    s_apps_tick = pit_ticks;
    desktop_refresh_apps_menu();
}

// desktop init

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
    apps_dir_defaults();         /* only if /APPS is missing */
    desktop_cd_links();          /* the disc's apps (or none left over) */
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
    menu_add_separator(edit_menu);
    menu_add_item(edit_menu, "Compress...", menu_compress);
    menu_add_item(edit_menu, "Extract...", menu_extract);

    menu *action_menu = window_add_menu(win, "Action");
    menu_add_item(action_menu, "Eject Floppy", menu_eject_fdd);
    menu_add_item(action_menu, "Eject CD", menu_eject_cd);
    menu_enable_last(action_menu, cd_present());
    menu_add_item(action_menu, "Format Disk...", menu_format_disk);
    menu_add_separator(action_menu);
    menu_add_item(action_menu, "Restart", menu_restart);
    menu_add_item(action_menu, "Shut Down", menu_shutdown);

    menu *help_menu = window_add_menu(win, "Help");
    menu_add_item(help_menu, "ASMOS Help", menu_help);

    desktop_fs_init();

    clipboard_init();
}

// per-frame
static void draw_items(int count, desktop_item_t *items) {
    int sel = -1;
    for (int i = 0; i < count; i++) {
        if (!items[i].used)
            continue;
        if (i == s_drag_idx)
            continue;
        if (items[i].selected && sel < 0) {
            sel = i; /* last: its whole name may cover the neighbours */
            continue;
        }
        draw_desktop_item(&items[i], false);
    }
    if (sel >= 0)
        draw_desktop_item(&items[sel], false);
    if (s_drag_idx >= 0 && s_drag_idx < count)
        draw_desktop_item(&items[s_drag_idx], false);
}

// context menus

static int s_ctx_idx = -1; /* icon the menu was opened on */

static void ctx_open(void) {
    if (s_ctx_idx >= 0 && s_ctx_idx < desktop_fs_count())
        open_item(s_ctx_idx);
}

static void ctx_paste_into(void) {
    char p[FS_PATH_SZ];
    if (s_ctx_idx >= 0 && desktop_fs_item_path(s_ctx_idx, p, sizeof(p)))
        fo_report(NULL, "Paste", fo_paste(p));
}

static void ctx_empty_trash(void) { fo_ask_empty_trash(NULL, NULL, NULL); }

static void ctx_compress(void) {
    char p[FS_PATH_SZ];
    if (s_ctx_idx >= 0 && desktop_fs_item_path(s_ctx_idx, p, sizeof(p)))
        fo_ask_compress(NULL, p, NULL, NULL);
}

static void ctx_extract(void) {
    char p[FS_PATH_SZ];
    if (s_ctx_idx >= 0 && desktop_fs_item_path(s_ctx_idx, p, sizeof(p)))
        fo_ask_extract(NULL, p, NULL, NULL);
}

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
        if (items[hit].drive_id != DRIVE_CD0) { /* FDD0, FDD1 */
            menu_add_separator(m);
            menu_add_item(m, "Eject Floppy", menu_eject_fdd);
        } else if (items[hit].drive_id == DRIVE_CD0) {
            menu_add_separator(m);
            menu_add_item(m, "Eject CD", menu_eject_cd);
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
        menu_add_item(m, "Compress...", ctx_compress);
        if (items[hit].kind != DESKTOP_ITEM_DIR && fo_is_zip(items[hit].name))
            menu_add_item(m, "Extract...", ctx_extract);
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
    if (g_cfg.wallpaper_pattern == WALLPAPER_CUSTOM)
        for (int i = 0; i < 32; i++)
            DMIX(g_cfg.wall_custom[i]);
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
    cd_follow();
    fdd_follow();
    apps_menu_follow();
    if (desktop_fs_is_dirty())
        desktop_fs_reload();

    desktop_item_t *items = desktop_fs_items();
    int count = desktop_fs_count();

    // drag *
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
            char src[FS_PATH_SZ], dst[FS_PATH_SZ];
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
