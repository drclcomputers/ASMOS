#include "ui/desktop.h"
#include "ui/desktop_fs.h"
#include "ui/dialogs.h"
#include "ui/icons.h"
#include "ui/menubar.h"
#include "ui/modal.h"
#include "ui/window.h"

#include "os/app_registry.h"
#include "os/appload.h"
#include "os/clipboard.h"
#include "os/os.h"

#include "config/config.h"
#include "config/runtime_config.h"

#include "drivers/serial.h"
#include "drivers/speaker.h"
#include "fs/fdd_ctrl.h"
#include "lib/core.h"

/* Defined in apps/filef.c (apps have no public headers). */
void ff_open_dir_pub(uint16_t cluster, const char *path);
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
        for (int x = 0; x < SCREEN_WIDTH; x++)
            draw_line(x, 0, x, SCREEN_HEIGHT - 1,
                      (x / 5) % 2 == 0 ? main_col : sec_col);
        break;
    case WALLPAPER_DOTS:
        clear_screen(main_col);
        for (int y = 5; y < SCREEN_HEIGHT; y += 10)
            for (int x = 5; x < SCREEN_WIDTH; x += 10)
                draw_dot(x, y, sec_col);
        break;
    default:
        clear_screen(main_col);
        break;
    }
}

/* ── desktop state ──────────────────────────────────────────────────────── */
#define DESK_ORIGIN_X 0
#define DESK_ORIGIN_Y MENUBAR_H

static int s_drag_idx = -1;
static int s_drag_off_x = 0;
static int s_drag_off_y = 0;
static int s_last_click_idx = -1;
static uint32_t s_last_click_tick = 0;
static int s_rename_idx = -1;

/* ── power animations ───────────────────────────────────────────────────── */
#define BAR_W 160
#define BAR_H 12
#define BAR_X ((SCREEN_WIDTH - BAR_W) / 2)
#define BAR_Y ((SCREEN_HEIGHT / 2) + 16)
#define STEPS 40

void power_anim_run(bool is_restart) {
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

        int msg_x = (SCREEN_WIDTH - (int)strlen(msg) * 5) / 2;
        draw_string(msg_x, SCREEN_HEIGHT / 2 - 24, (char *)msg, WHITE, 2);

        char pct[5];
        int p = (step * 100) / STEPS;
        pct[0] = '0' + p / 100;
        pct[1] = '0' + (p / 10) % 10;
        pct[2] = '0' + p % 10;
        pct[3] = '%';
        pct[4] = '\0';
        int px = (SCREEN_WIDTH - 4 * 5) / 2;
        draw_string(px, SCREEN_HEIGHT / 2 - 10, pct, LIGHT_GRAY, 2);

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
    int msg_x = (SCREEN_WIDTH - (int)strlen(msg) * 5) / 2;
    draw_string(msg_x, SCREEN_HEIGHT / 2 - 4, (char *)msg, bar_col, 2);
    blit();
    sleep_s(2);
}

/* ── dialog state ───────────────────────────────────────────────────────── */

static bool s_newname_is_dir = false;

/* ── open item ──────────────────────────────────────────────────────────── */

static void launch_app_ud(void *userdata) {
    os_launch_app((app_descriptor *)userdata);
}

static void open_item(desktop_item_t *it) {
    if (it->kind == DESKTOP_ITEM_APP) {
        char names[3][64];
        int ai = 0;
        for (int k = 0; it->name[k] && ai < 63; k++)
            names[0][ai++] = it->name[k];
        names[0][ai] = '\0';

        for (int k = 0; k <= ai; k++) {
            char c = names[0][k];
            names[1][k] = (k == 0 && c >= 'a' && c <= 'z')  ? c - 32
                          : (k > 0 && c >= 'A' && c <= 'Z') ? c + 32
                                                            : c;
        }
        for (int k = 0; k <= ai; k++) {
            char c = names[0][k];
            names[2][k] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        }

        /* 1. Prefer a real dynamic .APP package on disk.
         *    Try exact locations only (no spam).  Floppy first since that
         *    is how test packages are delivered. */
        char pathbuf[280];
        bool launched = false;

        snprintf(pathbuf, sizeof(pathbuf), "/FDD0/%s.APP", names[0]);
        if (appload_try_open(pathbuf))
            launched = true;

        if (!launched) {
            snprintf(pathbuf, sizeof(pathbuf), "/FDD1/%s.APP", names[0]);
            if (appload_try_open(pathbuf))
                launched = true;
        }
        if (!launched) {
            snprintf(pathbuf, sizeof(pathbuf), "%s/%s.APP", desktop_fs_path(),
                     names[0]);
            if (appload_try_open(pathbuf))
                launched = true;
        }
        if (!launched) {
            snprintf(pathbuf, sizeof(pathbuf), "/apps/%s.APP", names[0]);
            if (appload_try_open(pathbuf))
                launched = true;
        }

        /* 2. Fall back to the classic static registry */
        if (!launched) {
            app_descriptor *desc = NULL;
            for (int n = 0; n < 3 && !desc; n++)
                desc = os_find_app(names[n]);

            if (desc) {
                os_launch_app(desc);
                launched = true;
            }
        }

        if (!launched) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Could not find application '%s'.\n"
                     "Put %s.APP in /apps or /FDD0, or rebuild the kernel.",
                     names[0], names[0]);
            modal_show(MODAL_INFO, "App Not Found", msg, NULL, NULL);
        }
    } else if (it->kind == DESKTOP_ITEM_DIR) {
        dir_entry_t de;
        uint16_t saved = dir_context.current_cluster;
        dir_context.current_cluster = desktop_fs_cluster();
        bool found = fs_find(it->name, &de);
        dir_context.current_cluster = saved;
        if (found && (de.attr & ATTR_DIRECTORY)) {
            char path[280];
            sprintf(path, "%s/%s", desktop_fs_path(), it->name);
            ff_open_dir_pub(de.cluster_lo, path);
        }
    } else if (it->kind == DESKTOP_ITEM_FDD || it->kind == DESKTOP_ITEM_HDD) {
        const char *vfs_path = NULL;
        for (int m = 0; m < VFS_MOUNT_COUNT; m++) {
            if (g_vfs_mounts[m].drive_id == it->drive_id) {
                vfs_path = g_vfs_mounts[m].path;
                break;
            }
        }
        if (vfs_path && fs_drive_mounted(it->drive_id))
            ff_open_dir_pub(0, vfs_path);
    } else {
        ff_open_dir_pub(desktop_fs_cluster(), desktop_fs_path());
    }
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

    int lw = dlen * ICO_LABEL_W;
    int lx = ax + ICON_W / 2 - lw / 2;
    int ly = ay + ICON_H + 2;

    uint8_t label_col = is_cut ? DARK_GRAY : WHITE;
    draw_string(lx + 1, ly + 1, disp, BLACK, 2);
    draw_string(lx, ly, disp, label_col, 2);
}

static int desktop_hit(int mx, int my) {
    desktop_item_t *items = desktop_fs_items();
    int count = desktop_fs_count();
    for (int i = 0; i < count; i++) {
        if (!items[i].used)
            continue;
        int ax = DESK_ORIGIN_X + items[i].x;
        int ay = DESK_ORIGIN_Y + items[i].y;
        if (mx >= ax && mx < ax + ICON_W && my >= ay &&
            my < ay + ICON_H + ICO_LABEL_H + 2)
            return i;
    }
    return -1;
}

bool desktop_accept_drop(const char *src_path, const char *src_name,
                         uint8_t src_drive) {
    (void)src_drive;
    if (!src_path || !src_name)
        return false;

    char dst_path[270];
    snprintf(dst_path, sizeof(dst_path), "%s/%s", desktop_fs_path(), src_name);

    dir_entry_t de;
    if (fs_find(dst_path, &de)) {
        return false;
    }

    dir_entry_t src_de;
    if (!fs_find(src_path, &src_de))
        return false;

    bool ok;
    if (src_de.attr & ATTR_DIRECTORY)
        ok = fs_move_dir(src_path, dst_path);
    else
        ok = fs_move_file(src_path, dst_path);

    if (ok)
        desktop_fs_set_dirty();
    return ok;
}

static int selected_idx(void) {
    desktop_item_t *items = desktop_fs_items();
    int count = desktop_fs_count();
    for (int i = 0; i < count; i++)
        if (items[i].selected)
            return i;
    return -1;
}

/* ── menu actions ───────────────────────────────────────────────────────── */
static void do_delete(void) {
    int sel = selected_idx();
    if (sel < 0) {
        modal_show(MODAL_ERROR, "Delete", "Selection lost.", NULL, NULL);
        return;
    }
    desktop_fs_delete(sel);
}

static void newname_done(void *user, const char *name);
static void rename_done(void *user, const char *name);

static void new_item(bool dir) {
    s_newname_is_dir = dir;
    dlg_input(NULL, dir ? "New Folder" : "New File",
              dir ? "Folder name:" : "File name:", "", 12, ES_FILENAME,
              newname_done, NULL);
}
static void menu_new_file(void) { new_item(false); }
static void menu_new_folder(void) { new_item(true); }
static void menu_rename(void) {
    desktop_item_t *items = desktop_fs_items();
    int count = desktop_fs_count();
    int sel = -1;

    for (int i = 0; i < count; i++) {
        if (items[i].used && items[i].selected) {
            sel = i;
            break;
        }
    }

    if (sel < 0) {
        return;
    }

    if (items[sel].kind == DESKTOP_ITEM_FDD ||
        items[sel].kind == DESKTOP_ITEM_HDD) {
        return;
    }

    s_rename_idx = sel;
    dlg_input(NULL, "Rename", "New name:", items[sel].name, 12, ES_FILENAME,
              rename_done, NULL);
}
static void menu_reload(void) { desktop_fs_set_dirty(); }

/* Copy (is_cut=false) or cut the selected item to the file clipboard. */
static void clip_selected(bool is_cut) {
    const char *title = is_cut ? "Cut" : "Copy";
    int sel = selected_idx();
    if (sel < 0) {
        modal_show(MODAL_ERROR, title, "Nothing selected.", NULL, NULL);
        return;
    }
    desktop_item_t *it = &desktop_fs_items()[sel];
    if (path_is_protected(it->name)) {
        modal_show(MODAL_ERROR, title, "Protected item.", NULL, NULL);
        return;
    }

    /* App icons are backed by NAME.APP on disk. */
    char disk_name[16];
    if (it->kind == DESKTOP_ITEM_APP) {
        int fi = 0;
        for (int k = 0; it->name[k] && fi < 8; k++) {
            char c = it->name[k];
            disk_name[fi++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        }
        strcpy(disk_name + fi, ".APP");
    } else {
        strncpy(disk_name, it->name, 15);
        disk_name[15] = '\0';
    }

    clipboard_set_file(desktop_fs_path(), disk_name,
                       it->kind == DESKTOP_ITEM_DIR, is_cut, DRIVE_HDA);
    modal_show(MODAL_INFO, title,
               is_cut ? "Item cut. Paste to move."
                      : "Item copied to clipboard.",
               NULL, NULL);
}
static void menu_copy(void) { clip_selected(false); }
static void menu_cut(void) { clip_selected(true); }

static void menu_paste(void) {
    if (!clipboard_has_file()) {
        modal_show(MODAL_ERROR, "Paste", "Clipboard is empty.", NULL, NULL);
        return;
    }

    char src_path[270];
    if (g_clipboard.src_path[0] == '/' && g_clipboard.src_path[1] == '\0')
        sprintf(src_path, "/%s", g_clipboard.name);
    else
        sprintf(src_path, "%s/%s", g_clipboard.src_path, g_clipboard.name);

    char dst_path[270];
    sprintf(dst_path, "%s/%s", desktop_fs_path(), g_clipboard.name);

    dir_entry_t de;
    if (fs_find(dst_path, &de)) {
        modal_show(MODAL_ERROR, "Paste Failed", "Name already exists.", NULL,
                   NULL);
        return;
    }

    bool ok;
    if (g_clipboard.is_cut) {
        if (g_clipboard.is_dir)
            ok = fs_move_dir(src_path, dst_path);
        else
            ok = fs_move_file(src_path, dst_path);
        if (ok)
            clipboard_clear();
    } else {
        if (g_clipboard.is_dir)
            ok = fs_copy_dir(src_path, dst_path);
        else
            ok = fs_copy_file(src_path, dst_path);
    }

    if (ok) {
        desktop_fs_set_dirty();
        modal_show(MODAL_INFO, "Paste", "Item pasted successfully.", NULL,
                   NULL);
    } else {
        modal_show(MODAL_ERROR, "Paste Failed", "Could not paste item.", NULL,
                   NULL);
    }
}

static void menu_delete(void) {
    int sel = selected_idx();
    if (sel < 0) {
        modal_show(MODAL_ERROR, "Delete", "Nothing selected.", NULL, NULL);
        return;
    }
    desktop_item_t *it = &desktop_fs_items()[sel];
    if (path_is_protected(it->name)) {
        modal_show(MODAL_ERROR, "Delete", "Protected item.", NULL, NULL);
        return;
    }
    char msg[256];
    sprintf(msg, "Delete %s?", it->name);
    modal_show(MODAL_CONFIRM, "Confirm Delete", msg, do_delete, NULL);
}

static void menu_about_os(void) {
    char x[200];
    sprintf(x,
            "ASMOS ver %s\nDeveloped by drclcomputers.\n\nCheck out more of my "
            "work here: https://github.com/drclcomputers/ASMOS",
            VER);
    modal_show(MODAL_INFO, "About ASMOS (c)2026 @drclcomputers", x, NULL, NULL);
}

static void do_shutdown(void) {
    power_anim_run(false);
    cpu_shutdown();
}
static void menu_shutdown(void) {
    modal_show(MODAL_CONFIRM, "Shut Down",
               "Shut down ASMOS?\n\nAny usnaved work will be lost!",
               do_shutdown, NULL);
}
static void do_restart(void) {
    power_anim_run(true);
    cpu_reset();
}
static void menu_restart(void) {
    modal_show(MODAL_CONFIRM, "Restart",
               "Restart ASMOS?\n\nAny usnaved work will be lost!", do_restart,
               NULL);
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
    modal_show(MODAL_WARNING, "Eject", "No floppy selected or mounted.", NULL,
               NULL);
}

static void menu_insert_fdd(void) {
    if (fdd_insert(DRIVE_FDD0) || fdd_insert(DRIVE_FDD1)) {
        desktop_fs_set_dirty();
    } else {
        modal_show(MODAL_WARNING, "Insert Floppy",
                   "No floppy found or already mounted.", NULL, NULL);
    }
}

/* ── new name / rename (dlg_input callbacks; name NULL = cancelled) ───── */
static void newname_done(void *user, const char *name) {
    (void)user;
    const char *err = NULL;
    if (!name)
        return;
    if (!fs_validate_name(name, s_newname_is_dir, &err)) {
        dlg_message(NULL, "Invalid Name", err);
        return;
    }
    uint16_t saved = dir_context.current_cluster;
    dir_context.current_cluster = desktop_fs_cluster();
    bool ok;
    if (s_newname_is_dir) {
        ok = fs_mkdir(name);
    } else {
        fat_file_t f;
        ok = fs_create(name, &f);
        if (ok)
            fs_close(&f);
    }
    dir_context.current_cluster = saved;
    if (!ok)
        dlg_message(NULL, "Create Failed", "Name exists or disk full.");
    desktop_fs_set_dirty();
}

static void rename_done(void *user, const char *name) {
    (void)user;
    if (!name || s_rename_idx < 0 || s_rename_idx >= desktop_fs_count())
        return;
    if (!desktop_fs_rename(s_rename_idx, name))
        dlg_message(NULL, "Rename Failed", "Invalid name or item protected.");
}

/* ── desktop init ───────────────────────────────────────────────────────── */
static menu *s_apps_menu;

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
    for (int i = 0; i < app_registry_count; i++) {
        if (app_registry[i].menu_label)
            menu_add_item_ud(s_apps_menu, (char *)app_registry[i].menu_label,
                             launch_app_ud, app_registry[i].desc);
    }

    menu *file_menu = window_add_menu(win, "File");
    menu_add_item(file_menu, "New File", menu_new_file);
    menu_add_item(file_menu, "New Folder", menu_new_folder);
    menu_add_item(file_menu, "Rename", menu_rename);
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

void desktop_on_frame(void) {
    draw_wallpaper_pattern();

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
            desktop_fs_move_icon(s_drag_idx, it->x, it->y);
            s_drag_idx = -1;
        }
    }

    if (mouse.left && !mouse.left_clicked && s_drag_idx < 0) {
        for (int i = 0; i < count; i++) {
            if (!items[i].selected || !items[i].used)
                continue;
            int ax = DESK_ORIGIN_X + items[i].x;
            int ay = DESK_ORIGIN_Y + items[i].y;
            if (mouse.x >= ax && mouse.x < ax + ICON_W && mouse.y >= ay &&
                mouse.y < ay + ICON_H) {
                if (mouse.dx != 0 || mouse.dy != 0) {
                    s_drag_off_x = mouse.x - ax;
                    s_drag_off_y = mouse.y - ay;
                    s_drag_idx = i;
                    s_last_click_idx = -1;
                }
                break;
            }
        }
    }

    if (!modal_active() && mouse.left_clicked && mouse.y >= MENUBAR_H &&
        g_menubar.open_index < 0 && !g_menubar_click_consumed) {
        int hit = desktop_hit(mouse.x, mouse.y);
        if (hit >= 0) {
            for (int i = 0; i < count; i++)
                items[i].selected = false;
            items[hit].selected = true;

            uint32_t now = pit_ticks;
            if (hit == s_last_click_idx &&
                (now - s_last_click_tick) <= DOUBLE_CLICK_SPEED) {
                open_item(&items[hit]);
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

    draw_items(count, items);

    if (g_alarm.set && !g_alarm.fired) {
        time_full_t t = time_rtc_local();
        if (t.hours == g_alarm.hour && t.minutes == g_alarm.minute &&
            t.seconds == 0) {
            g_alarm.fired = true;
            g_alarm.set = false;
            speaker_beep(880, 300);
            speaker_beep(660, 300);
            speaker_beep(880, 400);
            modal_show(MODAL_INFO, "Alarm", "Alarm time reached!", NULL, NULL);
        }
    }
}
