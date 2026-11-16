/* Format Disk - format a floppy for DOS, Windows and ASMOS (fs/fdformat.h).
 * One track a frame, so the window keeps moving while it works. */
#include "drivers/fdd.h"
#include "fs/fdformat.h"
#include "fs/fs.h"
#include "os/api.h"

typedef struct {
    window *win; /* must stay first */
    control *drive, *size, *label, *quick, *start, *bar, *msg;
    int sizes[FDD_MAX_FORMATS], nsizes;
    bool running;
    fdformat_t f;
} fmt_state_t;

app_descriptor fdformat_app;

static int unit_of(fmt_state_t *s) { return ctl_value(s->drive); }

/* the sizes this drive can format; the disk's own size first if known */
static void fill_sizes(fmt_state_t *s) {
    int u = unit_of(s);
    list_clear(s->size);
    s->nsizes = fdd_drive_formats((uint8_t)u, s->sizes);
    for (int i = 0; i < s->nsizes; i++)
        list_add(s->size, FDD_FORMATS[s->sizes[i]].name);
    int pick = 0;
    for (int i = 0; i < s->nsizes; i++)
        if (s->sizes[i] == fdd_media((uint8_t)u))
            pick = i;
    list_set_sel(s->size, s->nsizes ? pick : -1);
    ctl_enable(s->start, s->nsizes > 0);
    ctl_set_text(s->msg, s->nsizes ? "Everything on the disk will be erased."
                                   : "There is no such floppy drive.");
}

static void on_drive(control *c, int ev) {
    if (ev == EV_CHANGE)
        fill_sizes(c->user);
}

static void set_busy(fmt_state_t *s, bool busy) {
    s->running = busy;
    ctl_enable(s->drive, !busy);
    ctl_enable(s->size, !busy);
    ctl_enable(s->label, !busy);
    ctl_enable(s->quick, !busy);
    ctl_set_text(s->start, busy ? "Stop" : "Start");
}

static void go(void *user, bool yes) {
    fmt_state_t *s = user;
    if (!yes)
        return;
    int i = ctl_value(s->size);
    if (i < 0 || i >= s->nsizes)
        return;
    uint8_t drive = unit_of(s) ? DRIVE_FDD1 : DRIVE_FDD0;
    if (!fdformat_begin(&s->f, drive, s->sizes[i], ctl_text(s->label),
                        ctl_value(s->quick) != 0)) {
        dlg_message(s->win, "Format Disk", s->f.err);
        return;
    }
    ctl_set_value(s->bar, 0);
    ctl_set_text(s->msg, ctl_value(s->quick) ? "Writing the file system..."
                                             : "Formatting...");
    set_busy(s, true);
}

static void on_start(control *c, int ev) {
    fmt_state_t *s = c->user;
    if (ev != EV_CLICK)
        return;
    if (s->running) { /* stop: the disk is left half formatted */
        set_busy(s, false);
        ctl_set_text(s->msg, "Stopped. Format it again before use.");
        return;
    }
    char q[96];
    snprintf(q, sizeof(q),
             "Format the disk in drive %c: as %s?\n\nEverything "
             "on it will be erased.",
             'A' + unit_of(s), FDD_FORMATS[s->sizes[ctl_value(s->size)]].name);
    dlg_confirm(s->win, "Format Disk", q, go, s);
}

static void fmt_frame(void *state) {
    fmt_state_t *s = state;
    if (!s->win || !s->running)
        return;
    int pct = fdformat_step(&s->f);
    if (pct < 0) {
        set_busy(s, false);
        ctl_set_text(s->msg, "Failed.");
        dlg_message(s->win, "Format Disk", s->f.err);
        return;
    }
    ctl_set_value(s->bar, pct);
    if (pct >= 100) {
        set_busy(s, false);
        const fdd_format_t *fm = &FDD_FORMATS[s->f.media];
        uint32_t data = (uint32_t)(fm->spt * fm->heads * fm->tracks) - 1 -
                        2u * fm->fat_sectors - fm->root_entries / 16;
        char b[64];
        snprintf(b, sizeof(b), "Done: %u bytes free.", data * 512);
        ctl_set_text(s->msg, b);
    } else if (!s->f.quick) {
        char b[48];
        snprintf(b, sizeof(b), "Formatting track %d of %d...", s->f.track,
                 s->f.tracks);
        ctl_set_text(s->msg, b);
    }
}

static void m_close(void) { os_quit_app_by_desc(&fdformat_app); }

static void fmt_init(void *state) {
    fmt_state_t *s = state;
    s->win = app_window(s, "Format Disk", 60, 40, 220, 134, DARK_GRAY, 0);
    if (!s->win)
        return;
    window *w = s->win;
    ctl_label(w, 8, 6, 50, 12, "Drive:", 0);
    s->drive = ctl_combo(w, 60, 6, 70, on_drive);
    list_add(s->drive, "A:");
    list_add(s->drive, "B:");
    list_set_sel(s->drive, 0);
    ctl_label(w, 8, 22, 50, 12, "Size:", 0);
    s->size = ctl_combo(w, 60, 22, 70, NULL);
    ctl_label(w, 8, 38, 50, 12, "Label:", 0);
    s->label = ctl_edit(w, 60, 38, 80, 12, "", 11, ES_UPPER, NULL);
    s->quick =
        ctl_check(w, 8, 56, 200, "Quick (only erase the files)", 0, NULL);
    ctl_set_tooltip(s->quick, "For a disk that is already formatted");
    s->bar = ctl_progress(w, 8, 72, 202, 9, 100);
    s->msg = ctl_label(w, 8, 85, 202, 9, "", 0);
    s->start = ctl_button(w, 160, 98, 50, 13, "Start", 1, on_start);
    ctl_style_add(s->start, CS_DEFAULT);
    menu *f = window_add_menu(w, "File");
    menu_add_item(f, "Close", m_close);
    fill_sizes(s);
}

app_descriptor fdformat_app = {
    .name = "FORMAT",
    .state_size = sizeof(fmt_state_t),
    .init = fmt_init,
    .on_frame = fmt_frame,
    .single_instance = true,
};
