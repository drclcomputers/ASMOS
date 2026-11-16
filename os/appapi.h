#ifndef OS_APPAPI_H
#define OS_APPAPI_H

/* For DYNAMIC .APP packages only (built-in apps use os/api.h).
 * Stable function table handed to v2 .APP packages as arg 0 of
 * init/on_frame/destroy.  Apps must NOT reference kernel symbols directly
 * (the flat binary has no linker access to them) – go through this table.
 * Only ever append new members; bump `size`, not `version`. */

#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "lib/core.h"
#include "ui/controls.h"
#include "ui/dialogs.h"
#include "ui/window.h"

/* The UI toolkit (see ui/controls.h and ui/dialogs.h for documentation).
 * Same functions as built-in apps use, reached through api->ui->... */
typedef struct ui_api {
    uint32_t size; /* sizeof(ui_api_t) */

    window *(*app_window)(void *state, const char *title, int x, int y, int w,
                          int h, uint8_t bar_color, uint32_t flags);
    void (*client_rect)(const window *w, int *x, int *y, int *cw, int *ch);

    control *(*label)(window *, int, int, int, int, const char *, uint32_t);
    control *(*button)(window *, int, int, int, int, const char *, int,
                       ctl_handler);
    control *(*check)(window *, int, int, int, const char *, bool, ctl_handler);
    control *(*radio)(window *, int, int, int, const char *, int, bool,
                      ctl_handler);
    control *(*edit)(window *, int, int, int, int, const char *, int, uint32_t,
                     ctl_handler);
    control *(*list)(window *, int, int, int, int, uint32_t, ctl_handler);
    control *(*combo)(window *, int, int, int, ctl_handler);
    control *(*vscroll)(window *, int, int, int, ctl_handler);
    control *(*hscroll)(window *, int, int, int, ctl_handler);
    control *(*slider)(window *, int, int, int, int, int, int, ctl_handler);
    control *(*progress)(window *, int, int, int, int, int);
    control *(*group)(window *, int, int, int, int, const char *);
    control *(*tabs)(window *, int, int, int, const char *, ctl_handler);
    control *(*custom)(window *, int, int, int, int, ctl_painter, ctl_handler,
                       uint32_t);

    void (*destroy)(control *);
    void (*set_text)(control *, const char *);
    const char *(*text)(const control *);
    void (*set_value)(control *, int);
    int (*value)(const control *);
    void (*set_range)(control *, int, int, int);
    void (*show)(control *, bool);
    void (*enable)(control *, bool);
    void (*focus)(control *);
    void (*set_handler)(control *, ctl_handler, void *);
    void (*set_colors)(control *, uint8_t, uint8_t);
    void (*style_add)(control *, uint32_t);
    void (*move)(control *, int, int, int, int);
    void (*on_page)(control *, control *, int);
    control *(*by_id)(window *, int);

    int (*list_add)(control *, const char *);
    int (*list_add_data)(control *, const char *, uint32_t);
    void (*list_set)(control *, int, const char *);
    void (*list_remove)(control *, int);
    void (*list_clear)(control *);
    int (*list_count)(const control *);
    const char *(*list_get)(const control *, int);
    uint32_t (*list_data)(const control *, int);
    int (*list_sel)(const control *);
    void (*list_set_sel)(control *, int);
    void (*list_set_columns)(control *, const int *, int);

    void (*edit_select)(control *, int, int);
    void (*edit_insert)(control *, const char *);
    int (*edit_line_of)(const control *, int);
    int (*edit_col_of)(const control *, int);
    void (*edit_goto)(control *, int, int);
    bool (*edit_find)(control *, const char *, bool);

    void (*message)(window *, const char *, const char *);
    void (*confirm)(window *, const char *, const char *, dlg_bool_cb, void *);
    void (*input)(window *, const char *, const char *, const char *, int,
                  uint32_t, dlg_text_cb, void *);
    void (*open_file)(window *, const char *, const char *, const char *,
                      dlg_text_cb, void *);
    void (*save_file)(window *, const char *, const char *, const char *,
                      const char *, dlg_text_cb, void *);

    /* clipboard for edits (Ctrl+C/X/V already work from the keyboard) */
    void (*edit_copy)(control *);
    void (*edit_cut)(control *);
    void (*edit_paste)(control *);

    /* window menus: fn(state) runs when the item is picked */
    menu *(*add_menu)(window *w, const char *title);
    void (*menu_item)(menu *m, const char *label, void (*fn)(void *state));
    void (*menu_separator)(menu *m);

    /* input dialog with a check box (*checked is written before cb) */
    void (*input_check)(window *, const char *, const char *, const char *, int,
                        uint32_t, const char *, bool *, dlg_text_cb, void *);

    /* ── appended in ui size 2 (check api->ui->size) ── */

    /* context menus: on EV_MENU, fill popup_menu() with menu_item /
     * menu_separator, then popup_show_for(c). Labels must stay valid
     * (string literals). Items run fn(state) like window menu items. */
    menu *(*popup_menu)(void);
    void (*popup_show)(int x, int y);         /* screen coordinates */
    void (*popup_show_for)(const control *c); /* at c->mx/my */
    void (*menu_enable)(menu *m, bool on);    /* greys out the last item */
    bool (*menu_from_key)(void); /* EV_MENU came from Menu key/Shift+F10 */

    void (*set_tooltip)(control *c, const char *text); /* copied */

    /* tree view (see ui/controls.h "tree_*") */
    control *(*tree)(window *, int, int, int, int, uint32_t, ctl_handler);
    int (*tree_add)(control *, int parent, const char *, uint32_t, bool);
    void (*tree_clear)(control *);
    void (*tree_clear_children)(control *, int);
    int (*tree_parent)(const control *, int);
    int (*tree_first_child)(const control *, int);
    int (*tree_next_sibling)(const control *, int);
    const char *(*tree_text)(const control *, int);
    void (*tree_set_text)(control *, int, const char *);
    uint32_t (*tree_data)(const control *, int);
    int (*tree_sel)(const control *);
    void (*tree_set_sel)(control *, int);
    void (*tree_expand)(control *, int, bool);
    bool (*tree_is_expanded)(const control *, int);
    int (*tree_event_node)(const control *);
    void (*tree_path)(const control *, int, char *, int);
    int (*tree_find_path)(control *, const char *);

    void (*edit_select_all)(control *);

    /* ── appended in ui size 3 ── */
    control *(*status)(window *, const char *); /* status bar, see ctl_status */
} ui_api_t;

#define OS_API_VERSION 1

/* a file or folder, from api->list_dir / api->file_info (API size 6) */
typedef struct {
    char name[64];     /* its long name, if it has one: "My Game.sav" */
    char dos_name[13]; /* the 8.3 name DOS sees: "MYGAME~1.SAV" */
    uint8_t attr;      /* APP_ATTR_* */
    uint32_t size;     /* bytes (0 for a folder) */
    uint16_t date;     /* FAT: (year-1980)<<9 | month<<5 | day */
    uint16_t time;     /* FAT: hour<<11 | min<<5 | sec/2 */
} app_dirent_t;

typedef struct os_api {
    uint32_t version;
    uint32_t size; /* sizeof(os_api_t) – check before using newer members */

    void *(*kmalloc)(uint32_t n);
    void (*kfree)(void *p);
    uint32_t (*time_millis)(void);

    window *(*wm_register)(const window_spec_t *spec);
    void (*wm_unregister)(window *w);
    void (*wm_focus)(window *w);
    void (*reserved0)(void); /* was window_add_widget (removed) */

    void (*draw_dot)(int x, int y, unsigned char c);
    void (*draw_line)(int x0, int y0, int x1, int y1, unsigned char c);
    void (*draw_rect)(int x, int y, int w, int h, unsigned char c);
    void (*fill_rect)(int x, int y, int w, int h, unsigned char c);
    void (*draw_char)(int x, int y, char ch, unsigned char c, int size);
    void (*draw_string)(int x, int y, const char *s, unsigned char c, int size);

    const mousevar *mouse;

    /* Ask the OS to close this app instance (pass your own state ptr). */
    void (*quit_self)(void *state);

    /* ── appended in API size 2 (check api->size) ── */
    const ui_api_t *ui;
    const keyboardvar *kb;
    /* whole-file helpers: return bytes read / success */
    int (*file_read)(const char *path, void *buf, int max);
    bool (*file_write)(const char *path, const void *buf, int len);
    int (*snprintf)(char *dst, size_t n, const char *fmt, ...);

    /* ── appended in API size 3 ── */
    /* Saved settings of this app (os/prefs.h; file /PREFS/<NAME>.INI).
     * Read in init, write when they change or in destroy. */
    bool (*prefs_get)(const char *key, char *out, int sz);
    int (*prefs_get_int)(const char *key, int def);
    bool (*prefs_set)(const char *key, const char *value);
    bool (*prefs_set_int)(const char *key, int value);

    /* ── appended in API size 4: speed ── */
    /* Lazy redraw (docs/UI_TOOLKIT.md "Lazy redraw"). A window with custom
     * drawing (ui->custom painters, an on_draw) is drawn every frame. Call
     * set_lazy(w, true) when that drawing only changes in your handlers,
     * menu commands or dialogs: then it is drawn only when it changes, after
     * input, or after invalidate(w). Changing it from on_frame or a timer?
     * Call invalidate(w) then, or the window shows old content. */
    void (*set_lazy)(window *w, bool on); /* your own windows only */
    void (*invalidate)(window *w);        /* draw w again next frame */
    /* fast drawing: rep movsd / stosd, row copies, clipped */
    void (*draw_pixels)(int x, int y, int w, int h, const uint8_t *src,
                        int stride);
    void (*draw_text_n)(int x, int y, const char *s, int n, unsigned char c,
                        int size);
    int (*text_width)(const char *s, int size); /* pixels */
    int (*font_height)(int size);               /* rows */
    void *(*mem_copy)(void *dst, const void *src, size_t n);
    void *(*mem_set)(void *dst, int c, size_t n);
    /* the user's Settings > Speed choices: go lighter when asked */
    uint32_t (*speed_flags)(void); /* SPEED_* bits */
    int (*frame_ms)(void);         /* ms per frame: 16, 33 or 50 */

    /* ── appended in API size 5: submenus ── */
    /* An item of `m` (from ui->add_menu) that opens a submenu to the side,
     * with an arrow, like the groups of the Apps menu. Fill it with
     * ui->menu_item / ui->menu_separator. One level: NULL for a submenu
     * inside a submenu, in a context menu, or when `m` is full (16). */
    menu *(*submenu)(menu *m, const char *label);

    /* ── appended in API size 6: files and folders (long names) ── */
    /* Entries of a folder ("/FDD0/My Games"), not . and .., in disk order;
     * returns how many were put in out (up to max), -1 if no such folder.
     * Pass max 0 (out NULL) to just count them. */
    int (*list_dir)(const char *dir, app_dirent_t *out, int max);
    bool (*file_info)(const char *path, app_dirent_t *out);
    bool (*make_dir)(const char *path); /* new folder */
    bool (*remove)(const char *path);   /* a file, or an empty folder */
    bool (*rename)(const char *path, const char *new_name); /* name only */
} os_api_t;

/* api->list_dir / file_info (API size 6) */
#define APP_NAME_MAX 63  /* longest file name */
#define APP_PATH_MAX 511 /* longest path: char path[APP_PATH_MAX + 1] */
#define APP_ATTR_READONLY 0x01
#define APP_ATTR_HIDDEN 0x02
#define APP_ATTR_SYSTEM 0x04
#define APP_ATTR_DIR 0x10

/* api->speed_flags() */
#define SPEED_LOW_END (1u << 0)  /* the "Low-end machine" preset is on   */
#define SPEED_NO_ANIM (1u << 1)  /* no animations (reduce motion)        */
#define SPEED_OUTLINE (1u << 2)  /* outline move/resize                  */
#define SPEED_NO_BLINK (1u << 3) /* carets / cursors don't blink         */
#define SPEED_NO_TIPS (1u << 4)  /* no tooltips                          */

#endif
