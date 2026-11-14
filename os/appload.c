#include "os/appload.h"

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
#include "ui/controls.h"
#include "ui/dialogs.h"
#include "ui/modal.h"

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
} s_owned[MAX_DYN_WINDOWS];
static void *s_calling_state; /* state of the app whose code is running */

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
    void *prev = s_calling_state;
    s_calling_state = state;
    appfault_t f;
    bool ok = appguard_call2(fn, a, b, &f);
    s_calling_state = prev;
    if (!ok)
        on_fault(la, state, what, &f);
}

static void hook_ctl(control *c, int ev) {
    guarded(c->win->dyn_state, call_handler, c, (void *)(uintptr_t)ev,
            "a control handler");
}

static void hook_dlg(window *owner, void (*fn)(void *, void *), void *user,
                     void *arg) {
    guarded(owner->dyn_state, fn, user, arg, "a dialog callback");
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
};

/* ── API table given to v2 apps ─────────────────────────────────────── */

static void api_quit_self(void *state) {
    app_instance_t *a = instance_from_state(state);
    if (a)
        os_quit_app(a);
}

static const os_api_t s_api = {
    .version = OS_API_VERSION,
    .size = sizeof(os_api_t),
    .kmalloc = kmalloc,
    .kfree = kfree,
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
    static const char *names[] = {[0] = "divide error",
                                  [6] = "invalid opcode",
                                  [13] = "general protection",
                                  [14] = "page fault"};
    const char *kind =
        (f->vector < 15 && names[f->vector]) ? names[f->vector] : "exception";
    char eip[9], cr2[9], off[9];
    hex8(eip, f->eip);
    hex8(cr2, f->cr2);
    hex8(off, f->eip - (uint32_t)la->code_base);

    char msg[200];
    snprintf(msg, sizeof(msg), "%s crashed in %s: %s\nEIP %s (+%s) CR2 %s",
             la->desc.name, what, kind, eip, off, cr2);

    la->faulted = true;
    app_instance_t *inst = instance_from_state(state);
    if (inst) {
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
    modal_show(MODAL_INFO, "App Crashed", msg, NULL, NULL);
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
    void *prev = s_calling_state;
    s_calling_state = state;
    bool ok = appguard_call2(la->hdr.version >= ASMA_VERSION_2
                                 ? (void (*)(void *, void *))call_v2
                                 : (void (*)(void *, void *))call_v1,
                             fn, state, &f);
    s_calling_state = prev;
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
    if (la->raw)
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
    g_ctl_invoke_hook = hook_ctl;
    g_dlg_call_hook = hook_dlg;
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
    uint8_t *raw = (uint8_t *)kmalloc(image + 16);
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
    slot->file_size = fsize;
    slot->mod_date = mdate;
    slot->mod_time = mtime;
    snprintf(slot->path, sizeof(slot->path), "%s", path);

    slot->desc.name = slot->hdr.name;
    slot->desc.state_size = h.state_size;
    slot->desc.init = tramp_init;
    slot->desc.on_frame = tramp_frame;
    slot->desc.destroy = tramp_destroy;
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
        modal_show(MODAL_INFO, "App Load Failed", s_err, NULL, NULL);
        s_err[0] = 0;
        return true; /* handled: don't fall through to "not found" */
    }
    return false;
}
