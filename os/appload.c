#include "os/appload.h"

#include "config/runtime_config.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/sb16.h"
#include "fs/fs.h"
#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"
#include "os/appapi.h"
#include "os/appguard.h"
#include "os/error.h"
#include "os/os.h"
#include "os/prefs.h"
#include "os/watchdog.h"
#include "ui/controls.h"
#include "ui/dialogs.h"
#include "ui/popup.h"

static loaded_app_t s_loaded[MAX_LOADED_APPS];
static char s_err[160];

const char *appload_last_error(void) { return s_err; }

static void set_err(const char *why) {
    snprintf(s_err, sizeof(s_err), "%s", why ? why : "");
}

/* ── lookup helpers ─────────────────────────────────────────────────── */

static loaded_app_t *find_by_desc(const app_descriptor *d) {
    if (!d)
        return NULL;
    for (int i = 0; i < MAX_LOADED_APPS; i++)
        if (s_loaded[i].loaded && &s_loaded[i].desc == d)
            return &s_loaded[i];
    return NULL;
}

static loaded_app_t *find_by_name(const char *n) {
    for (int i = 0; i < MAX_LOADED_APPS; i++)
        if (s_loaded[i].loaded && strcmp(s_loaded[i].desc.name, n) == 0)
            return &s_loaded[i];
    return NULL;
}

static loaded_app_t *find_by_path(const char *p) {
    for (int i = 0; i < MAX_LOADED_APPS; i++)
        if (s_loaded[i].loaded && strcmp(s_loaded[i].path, p) == 0)
            return &s_loaded[i];
    return NULL;
}

static loaded_app_t *find_free(void) {
    for (int i = 0; i < MAX_LOADED_APPS; i++)
        if (!s_loaded[i].loaded)
            return &s_loaded[i];
    return NULL;
}

static bool has_instances(const app_descriptor *d) {
    for (int i = 0; i < MAX_RUNNING_APPS; i++)
        if (running_apps[i].running && running_apps[i].desc == d)
            return true;
    return false;
}

static app_instance_t *instance_from_state(void *state) {
    if (!state)
        return NULL;
    for (int i = 0; i < MAX_RUNNING_APPS; i++)
        if (running_apps[i].running && running_apps[i].state == state)
            return &running_apps[i];
    return NULL;
}

/* ── window ownership ──────────────────────────────────────────────────
 * Windows a dynamic app creates are remembered with the app's state, so
 * the close button can end the app (it used to only hide the window and
 * the app kept running, still listed in Monitor) and so leftovers are
 * unregistered when the app goes away. */

#define MAX_DYN_WINDOWS 64
static struct {
    window *win;
    void *state;
    char title[40]; /* kernel copy: the app's string is in its memory */
} s_owned[MAX_DYN_WINDOWS];
static void *s_calling_state; /* state of the app whose code is running */

/* ── per-instance memory ───────────────────────────────────────────────
 * Each running instance of a dynamic app gets its own heap (its state is
 * the first block): api->kmalloc/kfree work there, so an overrun breaks
 * only that app, and everything is freed when it ends. The heap comes
 * from the frame pool when it can; with paging on, the instance then
 * runs in a context (lib/vmm.h) where only its own image and heap are
 * mapped from the pool: other apps' memory faults. */

#define APP_HEAP_DEFAULT (64u * 1024u)
#define APP_HEAP_MIN (16u * 1024u)

typedef struct {
    bool used;
    void *state;
    loaded_app_t *la;
    heap_t heap;
    uint8_t *mem;   /* the heap's block */
    uint32_t pages; /* in the frame pool, else 0 (kernel heap block) */
    vmm_ctx_t *ctx; /* NULL: no paging / no frames */
} dyn_inst_t;

static dyn_inst_t s_inst[MAX_RUNNING_APPS];

static dyn_inst_t *inst_of(const void *state) {
    if (!state)
        return NULL;
    for (int i = 0; i < MAX_RUNNING_APPS; i++)
        if (s_inst[i].used && s_inst[i].state == state)
            return &s_inst[i];
    return NULL;
}

static void *state_alloc(app_descriptor *d, uint32_t size);

/* pool frames are mapped in the kernel space only: allocate there */
static void *dyn_state_alloc(app_descriptor *d, uint32_t size) {
    uint32_t cr3 = vmm_current();
    vmm_load_kernel();
    void *st = state_alloc(d, size);
    vmm_switch(cr3);
    return st;
}

static void *state_alloc(app_descriptor *d, uint32_t size) {
    loaded_app_t *la = find_by_desc(d);
    dyn_inst_t *di = NULL;
    for (int i = 0; i < MAX_RUNNING_APPS && !di; i++)
        if (!s_inst[i].used)
            di = &s_inst[i];
    if (!la || !di)
        return NULL;
    memset(di, 0, sizeof(*di));
    uint32_t want =
        (size + APP_HEAP_DEFAULT + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t least = (size + APP_HEAP_MIN + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t bytes = want;
    uint32_t mem = pmm_alloc_frames(want / PAGE_SIZE);
    if (!mem && (mem = pmm_alloc_frames(least / PAGE_SIZE)) != 0)
        bytes = least;
    if (mem) {
        di->mem = (uint8_t *)(uintptr_t)mem;
        di->pages = bytes / PAGE_SIZE;
    } else { /* no frames: a block of the kernel heap, still its own heap */
        bytes = least;
        di->mem = (uint8_t *)kmalloc(bytes);
        if (!di->mem)
            return NULL;
    }
    heap_init(&di->heap, (uint32_t)(uintptr_t)di->mem,
              (uint32_t)(uintptr_t)di->mem + bytes);
    void *state = heap_alloc(&di->heap, size);
    if (!state) {
        if (di->pages)
            pmm_free_frames((uint32_t)(uintptr_t)di->mem, di->pages);
        else
            kfree(di->mem);
        return NULL;
    }
    if (di->pages && vmm_enabled()) {
        di->ctx = vmm_create_context(0);
        if (di->ctx && !(vmm_grant(di->ctx, (uint32_t)(uintptr_t)di->mem,
                                   di->pages, true) &&
                         (!la->img_pages ||
                          vmm_grant(di->ctx, (uint32_t)(uintptr_t)la->raw,
                                    la->img_pages, true)))) {
            vmm_destroy_context(di->ctx);
            di->ctx = NULL; /* runs unprotected rather than not at all */
        }
    }
    di->state = state;
    di->la = la;
    di->used = true;
    return state;
}

static void dyn_state_free(app_descriptor *d, void *state) {
    (void)d;
    dyn_inst_t *di = inst_of(state);
    if (!di)
        return;
    uint32_t cr3 = vmm_current();
    if (di->ctx && cr3 == di->ctx->pd_phys)
        cr3 = g_kernel_pd_phys;
    vmm_load_kernel();
    if (di->ctx)
        vmm_destroy_context(di->ctx);
    if (di->pages)
        pmm_free_frames((uint32_t)(uintptr_t)di->mem, di->pages);
    else
        kfree(di->mem);
    memset(di, 0, sizeof(*di));
    vmm_switch(cr3);
}

/* the app's memory calls: its own heap */
static void *api_kmalloc(uint32_t n) {
    dyn_inst_t *di = inst_of(s_calling_state);
    return di ? heap_alloc(&di->heap, n) : NULL;
}

static void api_kfree(void *p) {
    dyn_inst_t *di = inst_of(s_calling_state);
    if (di && p != di->state) /* the state belongs to the OS */
        heap_free(&di->heap, p);
}

/* Run app code: its state is "calling", its context loaded. */
static bool app_call(void *state, void (*fn)(void *, void *), void *a, void *b,
                     appfault_t *f) {
    app_instance_t *inst = instance_from_state(state);
    if (inst && inst->suspended)
        return true; /* not responding: its code is not called */
    void *prev = s_calling_state;
    uint32_t prev_cr3 = vmm_current();
    dyn_inst_t *di = inst_of(state);
    s_calling_state = state;
    if (di && di->ctx)
        vmm_load_context(di->ctx);
    bool ok = appguard_call2(fn, a, b, f);
    vmm_switch(prev_cr3);
    s_calling_state = prev;
    return ok;
}

static void *owner_of(window *w) {
    for (int i = 0; i < MAX_DYN_WINDOWS; i++)
        if (s_owned[i].win == w)
            return s_owned[i].state;
    return NULL;
}

static void forget_window(window *w) {
    for (int i = 0; i < MAX_DYN_WINDOWS; i++)
        if (s_owned[i].win == w)
            s_owned[i].win = NULL, s_owned[i].state = NULL;
}

/* unregister every window still owned by this app instance */
static void drop_windows_of(void *state) {
    for (int i = 0; i < MAX_DYN_WINDOWS; i++)
        if (s_owned[i].win && s_owned[i].state == state) {
            wm_unregister(s_owned[i].win);
            s_owned[i].win = NULL;
            s_owned[i].state = NULL;
        }
}

static bool dyn_on_close(window *w) {
    app_instance_t *a = instance_from_state(owner_of(w));
    w->visible = false;
    if (a)
        os_quit_app(a);
    return true;
}

static window *adopt(window *w, bool set_close) {
    if (!w || !s_calling_state)
        return w;
    if (set_close)
        w->on_close = dyn_on_close;
    w->dyn_state = s_calling_state; /* its control handlers run guarded */
    for (int i = 0; i < MAX_DYN_WINDOWS; i++)
        if (!s_owned[i].win) {
            s_owned[i].win = w;
            s_owned[i].state = s_calling_state;
            strncpy(s_owned[i].title, w->title ? w->title : "",
                    sizeof(s_owned[i].title) - 1);
            s_owned[i].title[sizeof(s_owned[i].title) - 1] = 0;
            w->title = s_owned[i].title;
            break;
        }
    return w;
}

static window *api_wm_register(const window_spec_t *spec) {
    return adopt(wm_register(spec), !spec->on_close);
}

static window *api_app_window(void *state, const char *title, int x, int y,
                              int w, int h, uint8_t bar, uint32_t flags) {
    return adopt(app_window(state, title, x, y, w, h, bar, flags), true);
}

static void api_wm_unregister(window *w) {
    forget_window(w);
    wm_unregister(w);
}

/* ── guarded callbacks from the UI toolkit ────────────────────────────── */

static void on_fault(loaded_app_t *la, void *state, const char *what,
                     const appfault_t *f);

static void call_handler(void *c, void *ev) {
    control *ctl = (control *)c;
    ctl->handler(ctl, (int)(uintptr_t)ev);
}

static void guarded(void *state, void (*fn)(void *, void *), void *a, void *b,
                    const char *what) {
    app_instance_t *inst = instance_from_state(state);
    loaded_app_t *la = inst ? find_by_desc(inst->desc) : NULL;
    if (!la || la->faulted)
        return;
    appfault_t f;
    if (!app_call(state, fn, a, b, &f))
        on_fault(la, state, what, &f);
}

static void hook_ctl(control *c, int ev) {
    guarded(c->win->dyn_state, call_handler, c, (void *)(uintptr_t)ev,
            "a control handler");
}

static void hook_call(window *w, void (*fn)(void *, void *), void *a, void *b,
                      const char *what) {
    guarded(w->dyn_state, fn, a, b, what);
}

static void hook_dlg(window *owner, void (*fn)(void *, void *), void *user,
                     void *arg) {
    guarded(owner->dyn_state, fn, user, arg, "a dialog callback");
}

static void call_menu(void *fn, void *user) { ((MenuActionUD)fn)(user); }

static void hook_menu(void *state, MenuActionUD fn, void *user) {
    guarded(state, call_menu, (void *)fn, user, "a menu command");
}

/* Menus may only go on the app's own windows; items remember the app so
 * the menubar calls them through the guard with the app's state. */
static menu *api_add_menu(window *w, const char *title) {
    if (!w || !s_calling_state || w->dyn_state != s_calling_state)
        return NULL;
    return window_add_menu(w, title);
}

static void api_menu_item(menu *m, const char *label, void (*fn)(void *)) {
    if (!m || !s_calling_state || !fn)
        return;
    int n = m->item_count;
    menu_add_item_ud(m, (char *)label, fn, s_calling_state);
    if (m->item_count > n) /* not full */
        m->items[n].dyn_state = s_calling_state;
}

/* Context menus: the popup belongs to whichever app fills it; items carry
 * the app's state like window menu items do. */
static menu *api_popup_menu(void) {
    return s_calling_state ? popup_menu() : NULL;
}

static void api_popup_show(int x, int y) {
    if (s_calling_state)
        popup_show(x, y);
}

static void api_popup_show_for(const control *c) {
    if (s_calling_state && c && c->win && c->win->dyn_state == s_calling_state)
        popup_show_for(c);
}

static void api_menu_enable(menu *m, bool on) {
    if (m)
        menu_enable_last(m, on);
}

static menu *api_submenu(menu *m, const char *label) {
    if (!m || !label || !s_calling_state || popup_is_menu(m))
        return NULL;
    return menu_add_submenu(m, (char *)label);
}

static void api_menu_separator(menu *m) {
    if (m)
        menu_add_separator(m);
}

static int api_file_read(const char *path, void *buf, int max) {
    fat_file_t f;
    if (!path || !buf || max <= 0 || !fs_open(path, &f))
        return -1;
    int n = fs_read(&f, buf, max);
    fs_close(&f);
    return n;
}

static bool api_file_write(const char *path, const void *buf, int len) {
    dir_entry_t de;
    if (!path || len < 0)
        return false;
    if (fs_find(path, &de) && !fs_delete(path))
        return false;
    fat_file_t f;
    if (!fs_create(path, &f))
        return false;
    int n = len ? fs_write(&f, buf, len) : 0;
    fs_close(&f);
    return n == len;
}

static const ui_api_t s_ui = {
    .size = sizeof(ui_api_t),
    .app_window = api_app_window,
    .client_rect = win_client_rect,
    .label = ctl_label,
    .button = ctl_button,
    .check = ctl_check,
    .radio = ctl_radio,
    .edit = ctl_edit,
    .list = ctl_list,
    .combo = ctl_combo,
    .vscroll = ctl_vscroll,
    .hscroll = ctl_hscroll,
    .slider = ctl_slider,
    .progress = ctl_progress,
    .group = ctl_group,
    .tabs = ctl_tabs,
    .custom = ctl_custom,
    .destroy = ctl_destroy,
    .set_text = ctl_set_text,
    .text = ctl_text,
    .set_value = ctl_set_value,
    .value = ctl_value,
    .set_range = ctl_set_range,
    .show = ctl_show,
    .enable = ctl_enable,
    .focus = ctl_focus,
    .set_handler = ctl_set_handler,
    .set_colors = ctl_set_colors,
    .style_add = ctl_style_add,
    .move = ctl_move,
    .on_page = ctl_on_page,
    .by_id = ctl_by_id,
    .list_add = list_add,
    .list_add_data = list_add_data,
    .list_set = list_set,
    .list_remove = list_remove,
    .list_clear = list_clear,
    .list_count = list_count,
    .list_get = list_get,
    .list_data = list_data,
    .list_sel = list_sel,
    .list_set_sel = list_set_sel,
    .list_set_columns = list_set_columns,
    .edit_select = edit_select,
    .edit_insert = edit_insert,
    .edit_line_of = edit_line_of,
    .edit_col_of = edit_col_of,
    .edit_goto = edit_goto,
    .edit_find = edit_find,
    .message = dlg_message,
    .confirm = dlg_confirm,
    .input = dlg_input,
    .open_file = dlg_open_file,
    .save_file = dlg_save_file,
    .edit_copy = edit_copy,
    .edit_cut = edit_cut,
    .edit_paste = edit_paste,
    .add_menu = api_add_menu,
    .menu_item = api_menu_item,
    .menu_separator = api_menu_separator,
    .input_check = dlg_input_check,
    .popup_menu = api_popup_menu,
    .popup_show = api_popup_show,
    .popup_show_for = api_popup_show_for,
    .menu_enable = api_menu_enable,
    .menu_from_key = ctl_menu_from_key,
    .set_tooltip = ctl_set_tooltip,
    .tree = ctl_tree,
    .tree_add = tree_add,
    .tree_clear = tree_clear,
    .tree_clear_children = tree_clear_children,
    .tree_parent = tree_parent,
    .tree_first_child = tree_first_child,
    .tree_next_sibling = tree_next_sibling,
    .tree_text = tree_text,
    .tree_set_text = tree_set_text,
    .tree_data = tree_data,
    .tree_sel = tree_sel,
    .tree_set_sel = tree_set_sel,
    .tree_expand = tree_expand,
    .tree_is_expanded = tree_is_expanded,
    .tree_event_node = tree_event_node,
    .tree_path = tree_path,
    .tree_find_path = tree_find_path,
    .edit_select_all = edit_select_all,
    .status = ctl_status,
};

/* ── API table given to v2 apps ─────────────────────────────────────── */

/* prefs of the calling app, by its name */
static const char *calling_name(void) {
    app_instance_t *a = instance_from_state(s_calling_state);
    return a && a->desc ? a->desc->name : NULL;
}
static bool api_prefs_get(const char *k, char *out, int sz) {
    const char *n = calling_name();
    return n && app_prefs_get(n, k, out, sz);
}
static int api_prefs_get_int(const char *k, int def) {
    const char *n = calling_name();
    return n ? app_prefs_get_int(n, k, def) : def;
}
static bool api_prefs_set(const char *k, const char *v) {
    const char *n = calling_name();
    return n && app_prefs_set(n, k, v);
}
static bool api_prefs_set_int(const char *k, int v) {
    const char *n = calling_name();
    return n && app_prefs_set_int(n, k, v);
}

static void api_quit_self(void *state) {
    app_instance_t *a = instance_from_state(state);
    if (a)
        os_quit_app(a);
}

/* size 4: speed */
static void api_set_lazy(window *w, bool on) {
    if (w && s_calling_state && w->dyn_state == s_calling_state) {
        w->lazy_custom = on;
        wm_invalidate(w);
    }
}

static void api_invalidate(window *w) {
    if (w)
        wm_invalidate(w);
}

static uint32_t api_speed_flags(void) {
    return (cfg_is_low_end() ? SPEED_LOW_END : 0) |
           (g_cfg.reduce_motion ? SPEED_NO_ANIM : 0) |
           (g_cfg.outline_drag ? SPEED_OUTLINE : 0) |
           (g_cfg.caret_no_blink ? SPEED_NO_BLINK : 0) |
           (g_cfg.no_tooltips ? SPEED_NO_TIPS : 0);
}

static const os_api_t s_api = {
    .version = OS_API_VERSION,
    .size = sizeof(os_api_t),
    .kmalloc = api_kmalloc,
    .kfree = api_kfree,
    .time_millis = time_millis,
    .wm_register = api_wm_register,
    .wm_unregister = api_wm_unregister,
    .wm_focus = wm_focus,
    .reserved0 = NULL,
    .draw_dot = draw_dot,
    .draw_line = draw_line,
    .draw_rect = draw_rect,
    .fill_rect = fill_rect,
    .draw_char = draw_char,
    .draw_string = draw_string,
    .mouse = &mouse,
    .quit_self = api_quit_self,
    .ui = &s_ui,
    .kb = &kb,
    .file_read = api_file_read,
    .file_write = api_file_write,
    .snprintf = snprintf,
    .prefs_get = api_prefs_get,
    .prefs_get_int = api_prefs_get_int,
    .prefs_set = api_prefs_set,
    .prefs_set_int = api_prefs_set_int,
    .set_lazy = api_set_lazy,
    .invalidate = api_invalidate,
    .draw_pixels = draw_pixels,
    .draw_text_n = draw_text_n,
    .text_width = text_width,
    .font_height = font_height,
    .mem_copy = memcpy,
    .mem_set = memset,
    .speed_flags = api_speed_flags,
    .frame_ms = cfg_frame_ms,
    .submenu = api_submenu,
};

/* ── guarded trampolines ────────────────────────────────────────────── */

typedef void (*v1_fn)(void *state);
typedef void (*v2_fn)(const os_api_t *api, void *state);

/* adapters so appguard can call either ABI with a uniform (a,b) shape */
static void call_v1(void *fn, void *state) { ((v1_fn)fn)(state); }
static void call_v2(void *fn, void *state) { ((v2_fn)fn)(&s_api, state); }

static void hex8(char *o, uint32_t v) {
    for (int i = 0; i < 8; i++) {
        uint8_t n = (v >> (28 - 4 * i)) & 0xF;
        o[i] = n < 10 ? '0' + n : 'A' + n - 10;
    }
    o[8] = 0;
}

static void on_fault(loaded_app_t *la, void *state, const char *what,
                     const appfault_t *f) {
    if (f->vector == APPFAULT_STOPPED) { /* watchdog: hold it for Monitor */
        app_instance_t *si = instance_from_state(state);
        if (si) {
            si->suspended = true;
            g_os_stopped_app = (int)(si - running_apps);
        }
        return;
    }
    static const char *names[] = {[0] = "divide error",
                                  [6] = "invalid opcode",
                                  [13] = "general protection",
                                  [14] = "page fault"};
    const char *kind = f->vector == APPFAULT_KILLED
                           ? "ended by the user (not responding)"
                       : (f->vector < 15 && names[f->vector]) ? names[f->vector]
                                                              : "exception";
    char eip[9], cr2[9], off[9];
    hex8(eip, f->eip);
    hex8(cr2, f->cr2);
    hex8(off, f->eip - (uint32_t)la->code_base);

    if (f->vector == 14) { /* say what the memory protection caught */
        if (f->cr2 < 0x1000)
            kind = "NULL pointer";
        else if (pmm_owns(f->cr2))
            kind = "touched memory that is not its own";
        else if (f->error & 2)
            kind = "wrote to read-only kernel code";
    }
    char msg[200];
    if (f->vector == APPFAULT_KILLED)
        snprintf(msg, sizeof(msg), "%s was not responding\nand has been ended.",
                 la->desc.name);
    else
        snprintf(msg, sizeof(msg), "%s crashed in %s: %s\nEIP %s (+%s) CR2 %s",
                 la->desc.name, what, kind, eip, off, cr2);

    bool killed = f->vector == APPFAULT_KILLED;
    if (!killed)
        la->faulted = true;       /* a crash disables the image; a hang just the
                                     instance */
    uint32_t cr3 = vmm_current(); /* may be another app's context (nested) */
    vmm_load_kernel();
    app_instance_t *inst = instance_from_state(state);
    if (inst) {
        inst->suspended = true; /* its destroy is not run */
        window *w = os_instance_window(inst);
        if (w) {
            forget_window(w);
            wm_unregister(w);
        }
        drop_windows_of(state);
        if (la->desc.opaque_state == false && inst->state)
            *(window **)inst->state = NULL;
        os_quit_app(inst);
    }
    vmm_switch(cr3);
    dlg_post_message(killed ? "App Ended" : "App Crashed", msg);
}

static void run_entry(app_descriptor *d, uint32_t off, void *state,
                      const char *what) {
    loaded_app_t *la = find_by_desc(d);
    if (!la || !la->code_base || la->faulted)
        return;
    if (off == ASMA_NO_ENTRY || off >= la->hdr.code_size)
        return;
    appfault_t f;
    void *fn = la->code_base + off;
    bool ok = app_call(state,
                       la->hdr.version >= ASMA_VERSION_2
                           ? (void (*)(void *, void *))call_v2
                           : (void (*)(void *, void *))call_v1,
                       fn, state, &f);
    if (!ok)
        on_fault(la, state, what, &f);
}

/* The OS calls descriptor hooks with only `state`; recover the descriptor
 * from the running instance. */
static app_descriptor *desc_from_state(void *state) {
    app_instance_t *a = instance_from_state(state);
    return a ? a->desc : NULL;
}

static void tramp_init(void *state) {
    app_descriptor *d = desc_from_state(state);
    loaded_app_t *la = find_by_desc(d);
    if (la) {
        la->was_run = true;
        run_entry(d, la->hdr.entry_init, state, "init");
    }
}
static void tramp_frame(void *state) {
    app_descriptor *d = desc_from_state(state);
    loaded_app_t *la = find_by_desc(d);
    if (la)
        run_entry(d, la->hdr.entry_frame, state, "on_frame");
}
static void tramp_destroy(void *state) {
    app_descriptor *d = desc_from_state(state);
    loaded_app_t *la = find_by_desc(d);
    if (la)
        run_entry(d, la->hdr.entry_destroy, state, "destroy");
    drop_windows_of(state); /* whatever the app forgot to unregister */
}

/* ── unload ─────────────────────────────────────────────────────────── */

static void free_slot(loaded_app_t *la) {
    os_uninstall_app(&la->desc);
    if (la->raw && la->img_pages)
        pmm_free_frames((uint32_t)(uintptr_t)la->raw, la->img_pages);
    else if (la->raw)
        kfree(la->raw);
    memset(la, 0, sizeof(*la));
}

bool appload_unload(app_descriptor *desc) {
    loaded_app_t *la = find_by_desc(desc);
    if (!la)
        return false;
    if (has_instances(desc))
        return false;
    free_slot(la);
    return true;
}

/* ── validation ─────────────────────────────────────────────────────── */

static bool ends_with_app(const char *s) {
    size_t n = strlen(s);
    if (n < 4 || s[n - 4] != '.')
        return false;
    char a = s[n - 3] | 0x20, b = s[n - 2] | 0x20, c = s[n - 1] | 0x20;
    return a == 'a' && b == 'p' && c == 'p';
}

/* returns NULL if OK, else reason */
static const char *validate(asma_header_t *h, uint32_t file_size) {
    if (h->magic != ASMA_MAGIC)
        return "Not an .APP package (bad magic).";
    if (h->version != ASMA_VERSION_1 && h->version != ASMA_VERSION_2)
        return "Unsupported .APP version.";
    bool v2 = h->version == ASMA_VERSION_2;
    uint32_t want = v2 ? ASMA_HEADER_V2 : ASMA_HEADER_V1;
    if (h->header_size < want || h->header_size > file_size)
        return "Corrupt .APP header size.";
    if (h->flags & ~ASMA_FLAGS_KNOWN)
        return "Unknown .APP flags.";
    if (!v2) { /* v1 has no v2 fields */
        h->reloc_offset = h->reloc_count = h->bss_size = 0;
        h->flags &= ~ASMA_FLAG_WINDOW_FIRST;
        if (h->entry_destroy == 0)
            h->entry_destroy = ASMA_NO_ENTRY;
    }
    if (h->code_size == 0 || h->code_size > ASMA_MAX_CODE_SIZE)
        return "Bad code size.";
    if (h->bss_size > ASMA_MAX_BSS)
        return "BSS too large.";
    if (h->state_size < 4 || h->state_size > ASMA_MAX_STATE)
        return "State size must be 4..65536.";
    if (h->code_offset < h->header_size ||
        (uint64_t)h->code_offset + h->code_size > file_size)
        return "Code lies outside the file.";
    if (h->entry_init >= h->code_size || h->entry_frame >= h->code_size ||
        (h->entry_destroy != ASMA_NO_ENTRY && h->entry_destroy >= h->code_size))
        return "Entry point outside code.";
    if (h->reloc_count > ASMA_MAX_RELOCS)
        return "Too many relocations.";
    if (h->reloc_count &&
        (h->reloc_offset < h->header_size ||
         (uint64_t)h->reloc_offset + (uint64_t)h->reloc_count * 4 > file_size))
        return "Relocation table outside the file.";
    h->name[ASMA_NAME_MAX - 1] = 0;
    if (h->name[0] == 0)
        return "App has no name.";
    for (int i = 0; h->name[i]; i++)
        if ((unsigned char)h->name[i] < 0x20 ||
            (unsigned char)h->name[i] > 0x7E)
            return "App name has invalid characters.";
    return NULL;
}

/* ── load ───────────────────────────────────────────────────────────── */

static bool read_exact(fat_file_t *f, void *buf, uint32_t n) {
    uint8_t *p = (uint8_t *)buf;
    while (n) {
        uint32_t chunk = n > 4096 ? 4096 : n;
        int got = fs_read(f, p, (int)chunk);
        if (got <= 0)
            return false;
        p += got;
        n -= (uint32_t)got;
    }
    return true;
}

app_descriptor *appload_load(const char *path) {
    wd_busy(); /* reading (maybe from floppy) and relocating */
    g_ctl_invoke_hook = hook_ctl;
    g_app_call_hook = hook_call;
    g_dlg_call_hook = hook_dlg;
    g_menu_call_hook = hook_menu;
    s_err[0] = 0;
    if (!path || !path[0] ||
        strlen(path) >= sizeof(((loaded_app_t *)0)->path)) {
        set_err("Bad path.");
        return NULL;
    }

    fat_file_t f;
    if (!fs_open(path, &f)) {
        set_err("File not found.");
        return NULL;
    }
    uint32_t fsize = f.entry.file_size;
    uint16_t mdate = f.entry.modify_date, mtime = f.entry.modify_time;

    /* cached? reuse if unchanged, hot-reload if changed and idle */
    loaded_app_t *old = find_by_path(path);
    if (old) {
        if (old->file_size == fsize && old->mod_date == mdate &&
            old->mod_time == mtime) {
            fs_close(&f);
            return &old->desc;
        }
        if (has_instances(&old->desc)) {
            fs_close(&f);
            set_err("Newer file on disk, but the app is running. Close it "
                    "to reload.");
            return &old->desc; /* keep running the loaded version */
        }
        free_slot(old);
    }

    if (fsize < ASMA_HEADER_V1 || fsize > ASMA_MAX_CODE_SIZE + ASMA_MAX_BSS +
                                              ASMA_MAX_RELOCS * 4 +
                                              ASMA_HEADER_V2) {
        fs_close(&f);
        set_err("Not a valid .APP (bad file size).");
        return NULL;
    }

    asma_header_t h;
    memset(&h, 0, sizeof(h));
    uint32_t hdr_read =
        fsize < ASMA_HEADER_V2 ? ASMA_HEADER_V1 : ASMA_HEADER_V2;
    if (!read_exact(&f, &h, hdr_read)) {
        fs_close(&f);
        set_err("Could not read .APP header.");
        return NULL;
    }
    if (h.version == ASMA_VERSION_2 && hdr_read < ASMA_HEADER_V2) {
        fs_close(&f);
        set_err("Truncated v2 header.");
        return NULL;
    }
    const char *why = validate(&h, fsize);
    if (why) {
        fs_close(&f);
        set_err(why);
        return NULL;
    }

    loaded_app_t *dup = find_by_name(h.name);
    if (dup || os_find_app(h.name)) {
        fs_close(&f);
        set_err("An app with this name already exists.");
        return NULL;
    }
    loaded_app_t *slot = find_free();
    if (!slot) {
        fs_close(&f);
        set_err("Too many dynamic apps loaded.");
        return NULL;
    }

    uint32_t image = h.code_size + h.bss_size;
    /* the image goes to the frame pool (whole pages an app context can
     * map), else to the kernel heap */
    uint32_t img_pages = (image + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t *raw = (uint8_t *)(uintptr_t)pmm_alloc_frames(img_pages);
    if (!raw) {
        img_pages = 0;
        raw = (uint8_t *)kmalloc(image + 16);
    }
    if (!raw) {
        fs_close(&f);
        set_err("Out of memory for app image.");
        return NULL;
    }
    uint8_t *base = (uint8_t *)(((uint32_t)raw + 15u) & ~15u);

    bool ok = fs_seek(&f, h.code_offset) && read_exact(&f, base, h.code_size);
    if (ok)
        memset(base + h.code_size, 0, h.bss_size);

    if (ok && h.reloc_count) {
        uint32_t tbl[128];
        ok = fs_seek(&f, h.reloc_offset);
        for (uint32_t done = 0; ok && done < h.reloc_count;) {
            uint32_t n = h.reloc_count - done;
            if (n > 128)
                n = 128;
            if (!read_exact(&f, tbl, n * 4)) {
                ok = false;
                break;
            }
            for (uint32_t i = 0; i < n; i++) {
                if ((uint64_t)tbl[i] + 4 > h.code_size) {
                    ok = false;
                    set_err("Relocation outside code.");
                    break;
                }
                uint32_t *w = (uint32_t *)(void *)(base + tbl[i]);
                *w += (uint32_t)base;
            }
            done += n;
        }
    }
    fs_close(&f);
    if (!ok) {
        if (img_pages)
            pmm_free_frames((uint32_t)(uintptr_t)raw, img_pages);
        else
            kfree(raw);
        if (!s_err[0])
            set_err("Could not read .APP contents.");
        return NULL;
    }

    memset(slot, 0, sizeof(*slot));
    slot->hdr = h;
    slot->raw = raw;
    slot->code_base = base;
    slot->image_size = image;
    slot->img_pages = img_pages;
    slot->file_size = fsize;
    slot->mod_date = mdate;
    slot->mod_time = mtime;
    snprintf(slot->path, sizeof(slot->path), "%s", path);

    slot->desc.name = slot->hdr.name;
    slot->desc.state_size = h.state_size;
    slot->desc.init = tramp_init;
    slot->desc.on_frame = tramp_frame;
    slot->desc.destroy = tramp_destroy;
    slot->desc.state_alloc = dyn_state_alloc;
    slot->desc.state_free = dyn_state_free;
    slot->desc.single_instance = (h.flags & ASMA_FLAG_SINGLE) != 0;
    slot->desc.opaque_state = (h.flags & ASMA_FLAG_WINDOW_FIRST) == 0;
    slot->loaded = true;

    os_install_app(&slot->desc);
    return &slot->desc;
}

void appload_gc(void) {
    for (int i = 0; i < MAX_LOADED_APPS; i++) {
        loaded_app_t *la = &s_loaded[i];
        if (la->loaded && la->was_run && !has_instances(&la->desc))
            free_slot(la);
    }
}

app_instance_t *appload_launch(const char *path) {
    app_descriptor *d = appload_load(path);
    if (!d)
        return NULL;
    loaded_app_t *la = find_by_desc(d);
    if (la && la->faulted && !has_instances(d)) {
        /* crashed earlier: reload fresh copy from disk */
        char p[sizeof(la->path)];
        snprintf(p, sizeof(p), "%s", la->path);
        free_slot(la);
        d = appload_load(p);
        if (!d)
            return NULL;
    }
    return os_launch_app(d);
}

app_descriptor *appload_find(const char *name) {
    if (!name)
        return NULL;
    loaded_app_t *la = find_by_name(name);
    return la ? &la->desc : NULL;
}

/* ── directory scan ─────────────────────────────────────────────────── */

/* the name in a .APP's header, without loading it (the Apps menu) */
bool appload_peek_name(const char *path, char *out, int max) {
    fat_file_t f;
    asma_header_t h;
    if (!path || !fs_open(path, &f))
        return false;
    int n = fs_read(&f, &h, (int)sizeof(h));
    fs_close(&f);
    if (n < (int)__builtin_offsetof(asma_header_t, entry_init) ||
        h.magic != ASMA_MAGIC)
        return false;
    int k = 0;
    for (; k < max - 1 && k < ASMA_NAME_MAX && h.name[k]; k++)
        out[k] = (h.name[k] >= 32 && h.name[k] < 127) ? h.name[k] : '?';
    out[k] = 0;
    return k > 0;
}

int appload_scan_dir(const char *dir_path) {
    uint8_t drive;
    uint16_t cluster;
    if (!dir_path || !fs_resolve_dir(dir_path, &drive, &cluster))
        return 0;

    static dir_entry_t ents[64];
    int count = 0;
    if (!fs_list_dir(drive, cluster, ents, 64, &count))
        return 0;

    int loaded = 0;
    for (int i = 0; i < count; i++) {
        const dir_entry_t *e = &ents[i];
        if (e->name[0] == 0 || e->name[0] == 0xE5 ||
            (e->attr & (ATTR_DIRECTORY | 0x08)))
            continue;
        if (!(((e->ext[0] | 0x20) == 'a') && ((e->ext[1] | 0x20) == 'p') &&
              ((e->ext[2] | 0x20) == 'p')))
            continue;

        char nm[13];
        int n = 0;
        for (int k = 0; k < 8 && e->name[k] != ' '; k++)
            nm[n++] = (char)e->name[k];
        nm[n++] = '.';
        nm[n++] = 'A';
        nm[n++] = 'P';
        nm[n++] = 'P';
        nm[n] = 0;

        char full[128];
        size_t dl = strlen(dir_path);
        snprintf(full, sizeof(full), "%s%s%s", dir_path,
                 (dl && dir_path[dl - 1] == '/') ? "" : "/", nm);
        if (appload_load(full))
            loaded++;
    }
    return loaded;
}

/* ── open helpers ───────────────────────────────────────────────────── */

bool appload_try_open(const char *p) {
    s_err[0] = 0;
    if (!p || !p[0])
        return false;

    static const char *const dirs[] = {"",       "/apps/",    "/FDD0/",
                                       "/FDD1/", "/DESKTOP/", NULL};
    bool ext = ends_with_app(p);
    char t[160];
    char first_err[sizeof(s_err)] = {0};

    for (int i = 0; dirs[i]; i++) {
        if (i == 0 && p[0] != '/')
            continue; /* bare names only get prefixed */
        if (ext)
            snprintf(t, sizeof(t), "%s%s", dirs[i], p);
        else
            snprintf(t, sizeof(t), "%s%s.APP", dirs[i], p);
        if (i == 0 && !ext)
            continue;

        /* does it exist, and is it a real package? Desktop shortcuts are
         * also named NAME.APP but are not packages: skip them silently so
         * the caller falls back to the built-in app of that name. */
        fat_file_t f;
        if (!fs_open(t, &f))
            continue;
        uint32_t magic = 0;
        int got = fs_read(&f, &magic, 4);
        fs_close(&f);
        if (got != 4 || magic != ASMA_MAGIC)
            continue;

        if (appload_launch(t))
            return true;
        /* found but unusable: report this, not "not found" */
        if (!first_err[0])
            snprintf(first_err, sizeof(first_err), "%s", s_err);
    }
    if (first_err[0]) {
        snprintf(s_err, sizeof(s_err), "%s", first_err);
        dlg_message(NULL, "App Load Failed", s_err);
        s_err[0] = 0;
        return true; /* handled: don't fall through to "not found" */
    }
    return false;
}
