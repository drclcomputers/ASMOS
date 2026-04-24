#include "ui/desktop.h"
#include "ui/desktop_fs.h"
#include "ui/icons.h"
#include "ui/menubar.h"
#include "ui/modal.h"
#include "ui/window.h"

#include "os/app_registry.h"
#include "os/clipboard.h"
#include "os/os.h"

#include "config/config.h"
#include "config/runtime_config.h"

#include "lib/core.h"
#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"

#include "interrupts/idt.h"
#include "io/keyboard.h"
#include "io/mouse.h"

extern menubar g_menubar;
extern bool g_menubar_click_consumed;

/* ── wallpaper ──────────────────────────────────────────────────────────── */

#define WALLPAPER_SOLID 0
#define WALLPAPER_CHECKERBOARD 1
#define WALLPAPER_STRIPES 2
#define WALLPAPER_DOTS 3

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
#define DBLCLICK_TICKS 60

/* ── name validation ────────────────────────────────────────────────────── */

static bool fat_char_ok(char c) {
    if (c >= 'A' && c <= 'Z')
        return true;
    if (c >= 'a' && c <= 'z')
        return true;
    if (c >= '0' && c <= '9')
        return true;
    const char *ok = "!#$%&'()-@^_`{}~";
    for (int i = 0; ok[i]; i++)
        if (c == ok[i])
            return true;
    return false;
}

static bool validate_fat_name(const char *name_buf, bool is_dir,
                              const char **err_msg) {
    if (!name_buf || name_buf[0] == '\0') {
        *err_msg = "Name cannot be empty.";
        return false;
    }
    const char *dot = strchr(name_buf, '.');
    int base_len, ext_len;
    if (dot) {
        base_len = (int)(dot - name_buf);
        ext_len = (int)strlen(dot + 1);
        if (is_dir && ext_len > 0) {
            *err_msg = "Folder: no extension.";
            return false;
        }
        if (ext_len > 3) {
            *err_msg = "Extension too long.";
            return false;
        }
        for (int i = 0; i < ext_len; i++)
            if (!fat_char_ok(dot[1 + i])) {
                *err_msg = "Invalid ext char.";
                return false;
            }
    } else {
        base_len = (int)strlen(name_buf);
        ext_len = 0;
    }
    if (base_len == 0) {
        *err_msg = "Base name empty.";
        return false;
    }
    if (base_len > 8) {
        *err_msg = "Base name too long (8).";
        return false;
    }
    for (int i = 0; i < base_len; i++)
        if (!fat_char_ok(name_buf[i])) {
            *err_msg = "Invalid character.";
            return false;
        }
    (void)ext_len;
    return true;
}

/* ── dialog state ───────────────────────────────────────────────────────── */

#define DESK_MODE_NORMAL 0
#define DESK_MODE_CONFIRM 1
#define DESK_MODE_NEWNAME 2

static int s_mode = DESK_MODE_NORMAL;
static char s_newname_buf[13];
static int s_newname_len = 0;
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

        app_descriptor *desc = NULL;
        for (int n = 0; n < 3 && !desc; n++)
            desc = os_find_app(names[n]);

        if (desc) {
            os_launch_app(desc);
        } else {
            char msg[256];
            sprintf(msg, "Could not find application '%s'.", names[0]);
            modal_show(MODAL_ERROR, "App Not Found", msg, NULL, NULL);
        }
    } else if (it->kind == DESKTOP_ITEM_DIR) {
        extern void ff_open_dir_pub(uint16_t cluster, const char *path);
        dir_entry_t de;
        uint16_t saved = dir_context.current_cluster;
        dir_context.current_cluster = desktop_fs_cluster();
        bool found = fat16_find(it->name, &de);
        dir_context.current_cluster = saved;
        if (found && (de.attr & ATTR_DIRECTORY)) {
            char path[280];
            sprintf(path, "%s/%s", desktop_fs_path(), it->name);
            ff_open_dir_pub(de.cluster_lo, path);
        }
    } else if (it->kind == DESKTOP_ITEM_FDD || it->kind == DESKTOP_ITEM_HDD) {
        extern void ff_open_dir_pub(uint16_t cluster, const char *path);
        const char *vfs_path = NULL;
        for (int m = 0; m < VFS_MOUNT_COUNT; m++) {
            if (g_vfs_mounts[m].drive_id == it->drive_id) {
                vfs_path = g_vfs_mounts[m].path;
                break;
            }
        }
        if (vfs_path && fat16_drive_mounted(it->drive_id))
            ff_open_dir_pub(0, vfs_path);
    } else {
        extern void ff_open_dir_pub(uint16_t cluster, const char *path);
        ff_open_dir_pub(desktop_fs_cluster(), desktop_fs_path());
    }
}

/* ── drawing ──────────────────────────────────────────────────────────────
 */

static void draw_desktop_item(const desktop_item_t *it, bool dragged_ghost) {
    int ax = DESK_ORIGIN_X + it->x;
    int ay = DESK_ORIGIN_Y + it->y;

    if (dragged_ghost) {
        draw_rect(ax, ay, ICO_W, ICO_H, DARK_GRAY);
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
    int lx = ax + ICO_W / 2 - lw / 2;
    int ly = ay + ICO_H + 2;

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
        if (mx >= ax && mx < ax + ICO_W && my >= ay &&
            my < ay + ICO_H + ICO_LABEL_H + 2)
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
    if (fat16_find(dst_path, &de)) {
        return false;
    }

    dir_entry_t src_de;
    if (!fat16_find(src_path, &src_de))
        return false;

    bool ok;
    if (src_de.attr & ATTR_DIRECTORY)
        ok = fat16_move_dir(src_path, dst_path);
    else
        ok = fat16_move_file(src_path, dst_path);

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

static void menu_new_file(void) {
    s_newname_buf[0] = '\0';
    s_newname_len = 0;
    s_newname_is_dir = false;
    s_mode = DESK_MODE_NEWNAME;
}
static void menu_new_folder(void) {
    s_newname_buf[0] = '\0';
    s_newname_len = 0;
    s_newname_is_dir = true;
    s_mode = DESK_MODE_NEWNAME;
}
static void menu_reload(void) { desktop_fs_set_dirty(); }

static void menu_copy(void) {
    int sel = selected_idx();
    if (sel < 0) {
        modal_show(MODAL_ERROR, "Copy", "Nothing selected.", NULL, NULL);
        return;
    }
    desktop_item_t *it = &desktop_fs_items()[sel];
    if (path_is_protected(it->name)) {
        modal_show(MODAL_ERROR, "Copy", "Protected item.", NULL, NULL);
        return;
    }

    char disk_name[16];
    if (it->kind == DESKTOP_ITEM_APP) {
        int fi = 0;
        for (int k = 0; it->name[k] && fi < 8; k++) {
            char c = it->name[k];
            if (c >= 'a' && c <= 'z')
                c -= 32;
            disk_name[fi++] = c;
        }
        disk_name[fi++] = '.';
        disk_name[fi++] = 'A';
        disk_name[fi++] = 'P';
        disk_name[fi++] = 'P';
        disk_name[fi] = '\0';
    } else {
        strncpy(disk_name, it->name, 15);
        disk_name[15] = '\0';
    }

    clipboard_set_file(desktop_fs_path(), disk_name,
                       it->kind == DESKTOP_ITEM_DIR, false, DRIVE_HDA);
    modal_show(MODAL_INFO, "Copy", "Item copied to clipboard.", NULL, NULL);
}

static void menu_cut(void) {
    int sel = selected_idx();
    if (sel < 0) {
        modal_show(MODAL_ERROR, "Cut", "Nothing selected.", NULL, NULL);
        return;
    }
    desktop_item_t *it = &desktop_fs_items()[sel];
    if (path_is_protected(it->name)) {
        modal_show(MODAL_ERROR, "Cut", "Protected item.", NULL, NULL);
        return;
    }

    char disk_name[16];
    if (it->kind == DESKTOP_ITEM_APP) {
        int fi = 0;
        for (int k = 0; it->name[k] && fi < 8; k++) {
            char c = it->name[k];
            if (c >= 'a' && c <= 'z')
                c -= 32;
            disk_name[fi++] = c;
        }
        disk_name[fi++] = '.';
        disk_name[fi++] = 'A';
        disk_name[fi++] = 'P';
        disk_name[fi++] = 'P';
        disk_name[fi] = '\0';
    } else {
        strncpy(disk_name, it->name, 15);
        disk_name[15] = '\0';
    }

    clipboard_set_file(desktop_fs_path(), disk_name,
                       it->kind == DESKTOP_ITEM_DIR, true, DRIVE_HDA);
    modal_show(MODAL_INFO, "Cut", "Item cut. Paste to move.", NULL, NULL);
}

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
    if (fat16_find(dst_path, &de)) {
        modal_show(MODAL_ERROR, "Paste Failed", "Name already exists.", NULL, NULL);
        return;
    }

    bool ok;
    if (g_clipboard.is_cut) {
        if (g_clipboard.is_dir)
            ok = fat16_move_dir(src_path, dst_path);
        else
            ok = fat16_move_file(src_path, dst_path);
        if (ok)
            clipboard_clear();
    } else {
        if (g_clipboard.is_dir)
            ok = fat16_copy_dir(src_path, dst_path);
        else
            ok = fat16_copy_file(src_path, dst_path);
    }

    if (ok) {
        desktop_fs_set_dirty();
        modal_show(MODAL_INFO, "Paste", "Item pasted successfully.", NULL, NULL);
    } else {
        modal_show(MODAL_ERROR, "Paste Failed", "Could not paste item.", NULL, NULL);
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

static void menu_sort_name(void) { desktop_fs_set_dirty(); }
static void menu_about_desktop(void) {
    modal_show(MODAL_INFO, "About Desktop", "Desktop v1.1\nASMOS Shell", NULL,
               NULL);
}

static void do_shutdown(void) {
    sleep_s(2);
    cpu_shutdown();
}
static void menu_shutdown(void) {
    modal_show(MODAL_CONFIRM, "Shut Down", "Shut down ASMOS?", do_shutdown,
               NULL);
}
static void do_restart(void) {
    sleep_s(2);
    cpu_reset();
}
static void menu_restart(void) {
    modal_show(MODAL_CONFIRM, "Restart", "Restart ASMOS?", do_restart, NULL);
}

/* ── new name dialog ──────────────────────────────────────────────────────
 */

static void handle_newname(void) {
    const char *err = NULL;
    if (!validate_fat_name(s_newname_buf, s_newname_is_dir, &err)) {
        s_mode = DESK_MODE_NORMAL;
        modal_show(MODAL_ERROR, "Invalid Name", err, NULL, NULL);
        return;
    }
    uint16_t saved = dir_context.current_cluster;
    dir_context.current_cluster = desktop_fs_cluster();
    bool ok;
    if (s_newname_is_dir) {
        ok = fat16_mkdir(s_newname_buf);
    } else {
        fat16_file_t f;
        ok = fat16_create(s_newname_buf, &f);
        if (ok)
            fat16_close(&f);
    }
    dir_context.current_cluster = saved;
    s_mode = DESK_MODE_NORMAL;
    if (!ok)
        modal_show(MODAL_ERROR, "Create Failed", "Name exists or disk full.",
                   NULL, NULL);
    desktop_fs_set_dirty();
}

#define CHAR_W 5

static void draw_newname_dialog(void) {
    int bx = 60, by = 80, bw = 200, bh = 36;
    fill_rect(bx + 3, by + 3, bw, bh, BLACK);
    fill_rect(bx, by, bw, bh, LIGHT_GRAY);
    draw_rect(bx, by, bw, bh, BLACK);
    const char *prompt = s_newname_is_dir ? "Folder name:" : "File name:";
    draw_string(bx + 4, by + 4, (char *)prompt, BLACK, 2);
    fill_rect(bx + 4, by + 14, bw - 8, 12, WHITE);
    draw_rect(bx + 4, by + 14, bw - 8, 12, BLACK);
    draw_string(bx + 6, by + 16, s_newname_buf, BLACK, 2);
    extern volatile uint32_t pit_ticks;
    if ((pit_ticks / 50) % 2 == 0) {
        int cx = bx + 6 + s_newname_len * CHAR_W;
        if (cx < bx + bw - 10)
            draw_string(cx, by + 16, "|", BLACK, 2);
    }
}

/* ── desktop init ─────────────────────────────────────────────────────────
 */

static menu *s_apps_menu;

void desktop_init(void) {
    static const window_spec_t spec = {
        .x = 0,
        .y = 0,
        .w = SCREEN_WIDTH,
        .h = SCREEN_HEIGHT - MENUBAR_H - TASKBAR_H,
        .title = "",
        .visible = true,
    };
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
    menu_add_item(file_menu, "Reload", menu_reload);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "About Desktop", menu_about_desktop);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "Restart", menu_restart);
    menu_add_item(file_menu, "Shut Down", menu_shutdown);

    menu *edit_menu = window_add_menu(win, "Edit");
    menu_add_item(edit_menu, "Copy", menu_copy);
    menu_add_item(edit_menu, "Cut", menu_cut);
    menu_add_item(edit_menu, "Paste", menu_paste);
    menu_add_separator(edit_menu);
    menu_add_item(edit_menu, "Delete", menu_delete);

    menu *view_menu = window_add_menu(win, "View");
    menu_add_item(view_menu, "Reload", menu_sort_name);

    desktop_fs_init();
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

    if (s_mode == DESK_MODE_NEWNAME) {
        draw_items(count, items);

        if (kb.key_pressed) {
            if (kb.last_scancode == ENTER && s_newname_len > 0) {
                handle_newname();
            } else if (kb.last_scancode == ESC) {
                s_mode = DESK_MODE_NORMAL;
            } else if (kb.last_scancode == BACKSPACE) {
                if (s_newname_len > 0)
                    s_newname_buf[--s_newname_len] = '\0';
            } else if (kb.last_char >= 32 && kb.last_char < 127 &&
                       s_newname_len < 12) {
                char c = kb.last_char;
                bool allow = fat_char_ok(c) || (c == '.' && !s_newname_is_dir);
                if (allow) {
                    s_newname_buf[s_newname_len++] = c;
                    s_newname_buf[s_newname_len] = '\0';
                }
            }
        }
        draw_newname_dialog();
        return;
    }

    /* ── drag
     * ────────────────────────────────────────────────────────────── */
    if (s_drag_idx >= 0) {
        desktop_item_t *it = &items[s_drag_idx];
        if (mouse.left) {
            int new_x = mouse.x - DESK_ORIGIN_X - s_drag_off_x;
            int new_y = mouse.y - DESK_ORIGIN_Y - s_drag_off_y;
            if (new_x < 0)
                new_x = 0;
            if (new_y < 0)
                new_y = 0;
            if (new_x > SCREEN_WIDTH - ICO_W)
                new_x = SCREEN_WIDTH - ICO_W;
            if (new_y >
                SCREEN_HEIGHT - MENUBAR_H - ICO_H - ICO_LABEL_H - TASKBAR_H)
                new_y =
                    SCREEN_HEIGHT - MENUBAR_H - ICO_H - ICO_LABEL_H - TASKBAR_H;
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
            if (mouse.x >= ax && mouse.x < ax + ICO_W && mouse.y >= ay &&
                mouse.y < ay + ICO_H) {
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
                (now - s_last_click_tick) <= DBLCLICK_TICKS) {
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

    draw_items(count, items);
}
