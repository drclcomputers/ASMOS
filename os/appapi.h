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
} os_api_t;

#endif
