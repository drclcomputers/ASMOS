#ifndef UI_CONTROLS_H
#define UI_CONTROLS_H

/*
 * ASMOS controls — a small Win32-style UI toolkit.
 *
 *   window  *w = app_window(s, "Notes", 40, 30, 200, 120, BLUE, AW_RESIZABLE);
 *   control *e = ctl_edit(w, 4, 4, 192, 12, "", 64, 0, on_edit);
 *   control *b = ctl_button(w, 140, 20, 56, 13, "OK", ID_OK, on_btn);
 *   ctl_style_add(b, CS_DEFAULT);                 // Enter clicks it
 *
 *   static void on_btn(control *c, int ev) {
 *       app_state *s = c->user;                   // = the window's userdata
 *       if (ev == EV_CLICK) ...
 *   }
 *
 * Controls are created once and live until their window is destroyed. The
 * window manager draws them, routes mouse/keyboard to them, keeps keyboard
 * focus (Tab / Shift+Tab), clicks the default button on Enter and the cancel
 * button on Esc. Coordinates are relative to the window's client area (below
 * the title bar). Anchor styles keep controls glued to the right/bottom edge
 * or stretch them when the window is resized.
 */

#include "lib/core.h"

struct window;
typedef struct window window;
typedef struct control control;

/* events (second argument of a control's handler) */
enum {
    EV_CLICK = 1, /* button / checkbox / radio pressed                   */
    EV_CHANGE,    /* text, value or selection changed                    */
    EV_SUBMIT,    /* Enter in an edit / list, double-click in a list     */
    EV_KEY,       /* key the control did not use (see kb.*)             */
    EV_FOCUS,     /* custom control got keyboard focus                   */
    EV_BLUR,      /* custom control lost keyboard focus                  */
    EV_MOUSEDOWN, /* custom: c->mx/c->my are relative to the control     */
    EV_MOUSEMOVE, /* custom: while the button is held                    */
    EV_MOUSEUP,   /* custom                                              */
    EV_DBLCLICK,  /* custom / list item double-clicked                   */
    EV_EXPAND,    /* tree: a node opens for the first time; add its
                     children now (tree_event_node says which)          */
    EV_MENU,      /* right-click / Menu key / Shift+F10: c->mx/my = where;
                     show a context menu (ui/popup.h)                   */
};

typedef void (*ctl_handler)(control *c, int ev);
typedef void (*ctl_painter)(control *c, int x, int y, int w, int h);

typedef enum {
    CTL_LABEL,
    CTL_BUTTON,
    CTL_CHECK,
    CTL_RADIO,
    CTL_EDIT,
    CTL_LIST,
    CTL_COMBO,
    CTL_VSCROLL,
    CTL_HSCROLL,
    CTL_SLIDER,
    CTL_PROGRESS,
    CTL_GROUP,
    CTL_TABS,
    CTL_CUSTOM,
    CTL_TREE,
    CTL_STATUS,
} ctl_type;

/* ── styles ──────────────────────────────────────────────────────────────
 * Common (any control) */
#define CS_ANCHOR_RIGHT (1u << 0)  /* keep distance to the right edge     */
#define CS_ANCHOR_BOTTOM (1u << 1) /* keep distance to the bottom edge    */
#define CS_STRETCH_W (1u << 2)     /* grow with the window width          */
#define CS_STRETCH_H (1u << 3)     /* grow with the window height         */
#define CS_DEFAULT (1u << 4)       /* button: Enter in the window clicks  */
#define CS_CANCEL (1u << 5)        /* button: Esc in the window clicks    */
#define CS_FOCUSABLE (1u << 6)     /* custom: can take keyboard focus     */
#define CS_BORDER (1u << 7)        /* label/custom: draw a sunken frame   */
#define CS_FLAT (1u << 8)          /* button: flat (toolbar) look         */
#define CS_TOGGLE (1u << 9)        /* button: stays down; value = 0/1     */
#define CS_NOFOCUS (1u << 10)      /* never takes keyboard focus          */
/* Labels */
#define LS_CENTER (1u << 12)
#define LS_RIGHT (1u << 13)
#define LS_BIG (1u << 14)  /* 8x8 font instead of the small one */
#define LS_BOLD (1u << 15) /* the bold system font */
/* Edits */
#define ES_MULTILINE (1u << 12)
#define ES_READONLY (1u << 13)
#define ES_NUMBER (1u << 14)   /* digits only                         */
#define ES_UPPER (1u << 15)    /* force upper case                    */
#define ES_PASSWORD (1u << 16) /* show '*'                            */
#define ES_FILENAME (1u << 17) /* file name characters (+ /), any case */
#define ES_DECIMAL (1u << 18)  /* digits, '.', '-'                    */
#define ES_TERMFONT (1u << 19) /* terminal font (6 px cells), as ASMTerm */
/* Lists */
#define LBS_HEADER (1u << 12) /* first row is a header (see list_*) */
#define TS_FOLDERS (1u << 12) /* tree: folder icon before each node      */

/* All fields are readable; change them through the functions below. */
struct control {
    window *win;
    control *next;
    ctl_type type;
    int id;
    uint32_t style;

    int x, y, w, h;       /* as created (client coords)                     */
    int ax, ay, aw, ah;   /* current absolute rectangle (after anchoring)  */
    int base_cw, base_ch; /* client size at creation (for anchors)      */

    bool visible, enabled;
    uint8_t fg, bg; /* 0xFF = theme default */

    char *text; /* caption / edit buffer (owned, NUL-terminated)     */
    int text_len, text_cap, max_len;

    int value, min, max, page; /* check/radio/toggle, scroll, slider, tabs */
    int group;                 /* radio group                            */

    /* edit */
    int cursor, anchor; /* selection = [min(cursor,anchor), max) */
    int scroll_x, scroll_y, want_col;

    /* list / combo */
    char **items;
    uint32_t *item_data;
    int count, cap, sel, top;
    int cols[8], ncols;
    bool open; /* combo popup */

    /* tabs: controls can live on a tab page */
    control *tab_owner;
    int tab_page;

    /* type-specific data owned by the control (tree nodes) */
    void *ext;

    /* tooltip shown when the mouse rests on the control (owned) */
    char *tip;

    /* custom */
    ctl_painter paint;
    int mx, my; /* mouse, relative to the control (mouse events) */

    ctl_handler handler;
    void *user; /* defaults to the window's userdata */
    bool pressed, focused;
    uint32_t rev; /* bumped when an edit's text changes (lazy redraw) */
};

/* ── top-level app windows ─────────────────────────────────────────────── */
#define AW_RESIZABLE (1u << 0)
#define AW_HIDDEN (1u << 1)
#define AW_NO_ANIM (1u << 2)
/* Standard app window. `state` becomes the window's userdata (and every
 * control's c->user). Closing it quits the app that owns `state`. */
window *app_window(void *state, const char *title, int x, int y, int w, int h,
                   uint8_t bar_color, uint32_t flags);
void win_set_userdata(window *w, void *ud);
/* userdata of the focused window (or of the window its dialog blocks):
 * handy in menu callbacks, which get no arguments. */
void *ui_active_state(void);
void win_client_rect(const window *w, int *x, int *y, int *cw, int *ch);
/* During EV_MENU: true if it came from the keyboard (act on the current
 * selection rather than on what is under c->mx/my). */
bool ctl_menu_from_key(void);

/* Tooltip: shown after the pointer rests on c for a moment (copied). */
void ctl_set_tooltip(control *c, const char *text);
void ctl_draw_tooltip(void); /* main loop, after everything else */

/* ── creating controls ─────────────────────────────────────────────────── */
control *ctl_create(window *w, ctl_type type, int x, int y, int cw, int ch,
                    const char *text, uint32_t style, ctl_handler h);
control *ctl_label(window *w, int x, int y, int cw, int ch, const char *text,
                   uint32_t style);
control *ctl_button(window *w, int x, int y, int cw, int ch, const char *text,
                    int id, ctl_handler h);
control *ctl_check(window *w, int x, int y, int cw, const char *text,
                   bool checked, ctl_handler h);
control *ctl_radio(window *w, int x, int y, int cw, const char *text, int group,
                   bool selected, ctl_handler h);
control *ctl_edit(window *w, int x, int y, int cw, int ch, const char *text,
                  int max_len, uint32_t style, ctl_handler h);
/* Status bar along the bottom of the client area (stretches with it). It
 * keeps clear of the resize grip of resizable windows by itself. Text
 * "left\tright": the part after a tab is right-aligned. */
control *ctl_status(window *w, const char *text);

/* Tree view (see "tree_*"). style: TS_FOLDERS, CS_* layout. */
control *ctl_tree(window *w, int x, int y, int cw, int ch, uint32_t style,
                  ctl_handler h);
control *ctl_list(window *w, int x, int y, int cw, int ch, uint32_t style,
                  ctl_handler h);
control *ctl_combo(window *w, int x, int y, int cw, ctl_handler h);
control *ctl_vscroll(window *w, int x, int y, int ch, ctl_handler h);
control *ctl_hscroll(window *w, int x, int y, int cw, ctl_handler h);
control *ctl_slider(window *w, int x, int y, int cw, int min, int max,
                    int value, ctl_handler h);
control *ctl_progress(window *w, int x, int y, int cw, int ch, int max);
control *ctl_group(window *w, int x, int y, int cw, int ch, const char *title);
/* tabs: "General|Sound|Network" */
control *ctl_tabs(window *w, int x, int y, int cw, const char *pages,
                  ctl_handler h);
control *ctl_custom(window *w, int x, int y, int cw, int ch, ctl_painter p,
                    ctl_handler h, uint32_t style);

/* ── common operations ─────────────────────────────────────────────────── */
void ctl_destroy(control *c);
void ctl_set_text(control *c, const char *text);
const char *ctl_text(const control *c);
void ctl_set_value(control *c, int v); /* check, radio, slider, scroll... */
int ctl_value(const control *c);
void ctl_set_range(control *c, int min, int max, int page);
void ctl_show(control *c, bool visible);
void ctl_enable(control *c, bool enabled);
void ctl_focus(control *c);
void ctl_set_handler(control *c, ctl_handler h, void *user);
void ctl_set_colors(control *c, uint8_t fg, uint8_t bg);
void ctl_style_add(control *c, uint32_t style);
void ctl_move(control *c, int x, int y, int cw, int ch);
void ctl_on_page(control *c, control *tabs, int page);
control *ctl_by_id(window *w, int id);
control *ctl_focused(window *w);

/* ── lists and combo boxes ─────────────────────────────────────────────── */
int list_add(control *c, const char *text); /* returns index */
int list_add_data(control *c, const char *text, uint32_t data);
void list_set(control *c, int i, const char *text);
void list_remove(control *c, int i);
void list_clear(control *c);
int list_count(const control *c);
const char *list_get(const control *c, int i);
uint32_t list_data(const control *c, int i);
int list_sel(const control *c);
void list_set_sel(control *c, int i);
/* columns: item text is split on '\t'; widths in pixels */
void list_set_columns(control *c, const int *widths, int n);

/* ── tree view ─────────────────────────────────────────────────────────────
 * Nodes are ints. parent -1 = top level. can_expand shows [+] before the
 * children are known: on first open the handler gets EV_EXPAND with
 * tree_event_node() = that node and may tree_add() children then.
 * EV_CHANGE = selection moved, EV_SUBMIT = Enter / double-click. */
int tree_add(control *c, int parent, const char *text, uint32_t data,
             bool can_expand);
void tree_clear(control *c);
void tree_clear_children(control *c, int node); /* reloaded on next open */
int tree_parent(const control *c, int node);
int tree_first_child(const control *c, int node);
int tree_next_sibling(const control *c, int node);
const char *tree_text(const control *c, int node);
void tree_set_text(control *c, int node, const char *text);
uint32_t tree_data(const control *c, int node);
int tree_sel(const control *c);
void tree_set_sel(control *c, int node); /* opens ancestors, scrolls */
void tree_expand(control *c, int node, bool open);
bool tree_is_expanded(const control *c, int node);
int tree_event_node(const control *c);
/* Paths: node texts joined as "/A/B/C". tree_find_path opens (and loads)
 * branches down to the deepest node matching path; -1 if none. */
void tree_path(const control *c, int node, char *out, int sz);
int tree_find_path(control *c, const char *path);

/* ── edits ─────────────────────────────────────────────────────────────── */
void edit_select(control *c, int from, int to);
void edit_select_all(control *c);
void edit_insert(control *c, const char *s); /* at cursor, replaces sel */
int edit_line_of(const control *c, int pos);
int edit_col_of(const control *c, int pos);
int edit_line_count(const control *c);
void edit_goto(control *c, int line, int col);
bool edit_find(control *c, const char *needle, bool from_start);
void edit_copy(control *c); /* selection -> clipboard */
void edit_cut(control *c);
void edit_paste(control *c); /* clipboard text at the cursor */
void edit_delete_sel(control *c);
bool edit_has_sel(const control *c);

/* ── called by the system ──────────────────────────────────────────────── */
void ctl_dispatch(void);      /* before apps run, every frame */
void ctl_draw_all(window *w); /* from window_draw            */
/* lazy redraw (window.h): a checksum of everything the window's controls
 * show; *custom = it has custom-drawn controls */
uint32_t ctl_window_sig(window *w, bool *custom);
bool ctl_overlay_active(void); /* an open combo list or a tooltip */
uint32_t tree_sig(const control *c);
bool edit_caret_on(const control *c);
void ctl_window_destroyed(window *w);
bool ctl_mouse_captured(void);
/* Invoke a handler; dynamic (.APP) windows route this through the fault
 * guard so a crash kills only that app. */
extern void (*g_ctl_invoke_hook)(control *c, int ev);
/* Run app code that belongs to window w (paint, on_draw) guarded: .APP
 * windows through g_app_call_hook (os/appload.c), built-in ones through
 * os_guard_call. */
extern void (*g_app_call_hook)(window *w, void (*fn)(void *, void *), void *a,
                               void *b, const char *what);
void ui_guard(window *w, void (*fn)(void *, void *), void *a, void *b,
              const char *what);
void ctl_invoke(control *c, int ev);

/* theme */
#define TH_FACE LIGHT_GRAY
#define TH_SHADOW DARK_GRAY
#define TH_LIGHT WHITE
#define TH_TEXT BLACK
#define TH_FIELD WHITE
#define TH_SEL_BG BLUE
#define TH_SEL_FG WHITE
#define TH_DISABLED DARK_GRAY
#define TH_CHAR_W 5
#define TH_LINE_H 8

#endif
