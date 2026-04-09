#include "ui/desktop_fs.h"
#include "fs/fat16.h"
#include "lib/string.h"
#include "lib/mem.h"

static desktop_item_t s_items[DESKTOP_MAX_ITEMS];
static int            s_count   = 0;
static bool           s_dirty   = false;
static uint16_t       s_cluster = 0;
static bool           s_inited  = false;

static void entry_display_name(const dir_entry_t *e, char *out) {
    int j = 0;
    for (int k = 0; k < 8 && e->name[k] != ' '; k++)
        out[j++] = e->name[k];
    if (!(e->attr & ATTR_DIRECTORY) && e->ext[0] != ' ') {
        out[j++] = '.';
        for (int k = 0; k < 3 && e->ext[k] != ' '; k++)
            out[j++] = e->ext[k];
    }
    out[j] = '\0';
}

static bool entry_is_app(const dir_entry_t *e) {
    if (e->attr & ATTR_DIRECTORY) return false;
    return (e->ext[0] == 'A' && e->ext[1] == 'P' && e->ext[2] == 'P');
}

static void grid_slot(int n, int *out_x, int *out_y) {
    int rows_avail = (200 - 10 - 14) / 34;
    if (rows_avail < 1) rows_avail = 1;
    int col = n / rows_avail;
    int row = n % rows_avail;
    *out_x = 8  + col * 36;
    *out_y = 14 + row * 34;
}

static void create_default_shortcuts(void) {
    static const char *defaults[] = { "FINDER", NULL };

    uint16_t saved = dir_context.current_cluster;
    dir_context.current_cluster = s_cluster;

    for (int i = 0; defaults[i]; i++) {
        char fname[16];
        int fi = 0;
        for (int k = 0; defaults[i][k] && fi < 8; k++)
            fname[fi++] = defaults[i][k];
        fname[fi++] = '.';
        fname[fi++] = 'A';
        fname[fi++] = 'P';
        fname[fi++] = 'P';
        fname[fi]   = '\0';

        fat16_file_t f;
        if (fat16_create(fname, &f))
            fat16_close(&f);
    }

    dir_context.current_cluster = saved;
}

void desktop_fs_init(void) {
    if (s_inited) return;
    s_inited = true;

    dir_entry_t de;
    bool exists = fat16_find(DESKTOP_PATH, &de);

    if (!exists || !(de.attr & ATTR_DIRECTORY)) {
        uint16_t saved = dir_context.current_cluster;
        dir_context.current_cluster = 0;
        fat16_mkdir("DESKTOP");
        dir_context.current_cluster = saved;

        if (!fat16_find(DESKTOP_PATH, &de)) {
            s_cluster = 0;
        } else {
            s_cluster = de.cluster_lo;
        }
        create_default_shortcuts();
    } else {
        s_cluster = de.cluster_lo;
    }

    desktop_fs_reload();
}

void desktop_fs_reload(void) {
    s_dirty = false;
    s_count = 0;
    memset(s_items, 0, sizeof(s_items));

    dir_entry_t raw[DESKTOP_MAX_ITEMS];
    int raw_count = 0;

    uint16_t saved = dir_context.current_cluster;
    dir_context.current_cluster = s_cluster;
    fat16_list_dir(s_cluster, raw, DESKTOP_MAX_ITEMS, &raw_count);
    dir_context.current_cluster = saved;

    int slot = 0;
    for (int i = 0; i < raw_count && s_count < DESKTOP_MAX_ITEMS; i++) {
        if (raw[i].name[0] == '.') continue;
        if (raw[i].attr & ATTR_VOLUME_ID) continue;
        if (raw[i].attr & ATTR_HIDDEN) continue;

        desktop_item_t *it = &s_items[s_count];
        memset(it, 0, sizeof(desktop_item_t));
        entry_display_name(&raw[i], it->name);

        if (raw[i].attr & ATTR_DIRECTORY) {
            it->kind = DESKTOP_ITEM_DIR;
        } else if (entry_is_app(&raw[i])) {
            it->kind = DESKTOP_ITEM_APP;
            char *dot = strchr(it->name, '.');
            if (dot) *dot = '\0';
        } else {
            it->kind = DESKTOP_ITEM_FILE;
        }

        it->used = true;
        grid_slot(slot++, &it->x, &it->y);
        s_count++;
    }
}

void desktop_fs_set_dirty(void) { s_dirty = true; }
bool desktop_fs_is_dirty(void)  { return s_dirty; }

desktop_item_t *desktop_fs_items(void) { return s_items; }
int             desktop_fs_count(void) { return s_count; }
uint16_t        desktop_fs_cluster(void) { return s_cluster; }
const char     *desktop_fs_path(void)   { return DESKTOP_PATH; }

bool desktop_fs_add_app(const char *app_name) {
    if (!app_name) return false;

    char fname[16];
    int fi = 0;
    for (int k = 0; app_name[k] && fi < 8; k++) {
        char c = app_name[k];
        if (c >= 'a' && c <= 'z') c -= 32;
        fname[fi++] = c;
    }
    fname[fi++] = '.';
    fname[fi++] = 'A';
    fname[fi++] = 'P';
    fname[fi++] = 'P';
    fname[fi]   = '\0';

    uint16_t saved = dir_context.current_cluster;
    dir_context.current_cluster = s_cluster;

    fat16_file_t f;
    bool ok = fat16_create(fname, &f);
    if (ok) fat16_close(&f);

    dir_context.current_cluster = saved;

    if (ok) desktop_fs_reload();
    return ok;
}

bool desktop_fs_delete(int idx) {
    if (idx < 0 || idx >= s_count || !s_items[idx].used) return false;
    desktop_item_t *it = &s_items[idx];

    uint16_t saved = dir_context.current_cluster;
    dir_context.current_cluster = s_cluster;

    bool ok = false;
    if (it->kind == DESKTOP_ITEM_DIR) {
        ok = fat16_rm_rf(it->name);
    } else {
        char fname[DESKTOP_NAME_MAX];
        if (it->kind == DESKTOP_ITEM_APP) {
            int fi = 0;
            for (int k = 0; it->name[k] && fi < 8; k++) {
                char c = it->name[k];
                if (c >= 'a' && c <= 'z') c -= 32;
                fname[fi++] = c;
            }
            fname[fi++] = '.';
            fname[fi++] = 'A';
            fname[fi++] = 'P';
            fname[fi++] = 'P';
            fname[fi]   = '\0';
            ok = fat16_delete(fname);
        } else {
            ok = fat16_delete(it->name);
        }
    }

    dir_context.current_cluster = saved;
    if (ok) desktop_fs_reload();
    return ok;
}

bool desktop_fs_rename(int idx, const char *new_name) {
    if (idx < 0 || idx >= s_count || !s_items[idx].used) return false;

    uint16_t saved = dir_context.current_cluster;
    dir_context.current_cluster = s_cluster;

    bool ok = fat16_rename(s_items[idx].name, new_name);

    dir_context.current_cluster = saved;
    if (ok) desktop_fs_reload();
    return ok;
}

void desktop_fs_move_icon(int idx, int new_x, int new_y) {
    if (idx < 0 || idx >= s_count) return;
    s_items[idx].x = new_x;
    s_items[idx].y = new_y;
    /* Positions are runtime-only (no sidecar persistence in this version) */
}
