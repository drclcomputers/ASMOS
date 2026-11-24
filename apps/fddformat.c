// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "drivers/ata.h"
#include "drivers/fdd.h"
#include "fs/fdformat.h"
#include "fs/fs.h"
#include "fs/install.h"
#include "os/api.h"

typedef struct {
    window *win; /* must stay first */
    control *drive, *size, *label, *quick, *start, *bar, *msg;
    int sizes[FDD_MAX_FORMATS], nsizes;
    uint8_t ids[3]; /* the drive list: DRIVE_FDD0, DRIVE_FDD1, DRIVE_HDB */
    int nids;
    bool running;
    fdformat_t f;
} fmt_state_t;

app_descriptor fdformat_app;

static uint8_t drive_of(fmt_state_t *s) {
    int i = ctl_value(s->drive);
    return i >= 0 && i < s->nids ? s->ids[i] : DRIVE_COUNT;
}
static int unit_of(fmt_state_t *s) { return drive_of(s) == DRIVE_FDD1; }
static bool is_hdb(fmt_state_t *s) { return drive_of(s) == DRIVE_HDB; }
static const char *name_of(uint8_t d) {
    return d == DRIVE_FDD0 ? "FDD0" : d == DRIVE_FDD1 ? "FDD1" : "HDB";
}

/* HDB: its size as one volume; no sizes, no quick (it always is) */
static void fill_hdb(fmt_state_t *s) {
    inst_disk_t d;
    inst_probe(1, &d);
    char b[48];
    snprintf(b, sizeof(b), "%u MB", (unsigned)(d.new_sectors / 2048));
    list_add(s->size, b);
    list_set_sel(s->size, 0);
    s->nsizes = 0;
    ctl_set_value(s->quick, 1);
    ctl_enable(s->quick, false);
    ctl_enable(s->size, false);
    ctl_enable(s->start, d.new_sectors > 0);
    ctl_set_text(s->msg, d.new_sectors ? "Everything on HDB will be erased."
                                       : "HDB is too small (5 MB at least).");
}

/* the sizes this drive can format; the disk's own size first if known */
static void fill_sizes(fmt_state_t *s) {
    int u = unit_of(s);
    list_clear(s->size);
    ctl_enable(s->quick, !s->running);
    ctl_enable(s->size, !s->running);
    if (is_hdb(s)) {
        fill_hdb(s);
        return;
    }
    if (drive_of(s) == DRIVE_COUNT) {
        s->nsizes = 0;
        ctl_enable(s->start, false);
        ctl_set_text(s->msg, "There is no drive to format.");
        return;
    }
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

static void hdb_progress(void *user, int pct, const char *what) {
    fmt_state_t *s = user;
    (void)what;
    ctl_set_value(s->bar, pct * 4); /* (it goes up to 25) */
}

/* the whole disk at once: a new partition and an empty FAT16 volume (a
 * few hundred sectors written: quick) */
static void go_hdb(fmt_state_t *s) {
    ctl_set_value(s->bar, 0);
    const char *err = inst_erase_label(1, ctl_text(s->label), hdb_progress, s);
    bool ok = !err && fs_mount_drive(DRIVE_HDB);
    ctl_set_value(s->bar, ok ? 100 : 0);
    desktop_fs_set_dirty(); /* its icon */
    if (!ok) {
        ctl_set_text(s->msg, "Failed.");
        dlg_message(s->win, "Format Disk",
                    err ? err : "HDB was erased but can't be read.");
        return;
    }
    uint32_t total = 0, used = 0;
    char b[64];
    if (fs_get_usage_drive(DRIVE_HDB, &total, &used))
        snprintf(b, sizeof(b), "Done: %u KB free.", (total - used) / 1024);
    else
        strcpy(b, "Done.");
    ctl_set_text(s->msg, b);
}

static void go(void *user, bool yes) {
    fmt_state_t *s = user;
    if (!yes)
        return;
    if (is_hdb(s)) {
        go_hdb(s);
        return;
    }
    int i = ctl_value(s->size);
    if (i < 0 || i >= s->nsizes)
        return;
    uint8_t drive = drive_of(s);
    if (!fdformat_begin(&s->f, drive, s->sizes[i], ctl_text(s->label),
                        ctl_value(s->quick) != 0)) {
        ctl_set_value(s->bar, 0);
        ctl_set_text(s->msg, "Not formatted.");
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
    char q[240];
    if (is_hdb(s)) {
        const ata_disk_t *a = ata_unit(1);
        snprintf(q, sizeof(q),
                 "Erase the hard disk HDB (%s) and make it one empty "
                 "volume?\n\nEverything on it will be lost: every "
                 "partition, every file.",
                 a ? a->model : "?");
    } else {
        int media = s->sizes[ctl_value(s->size)];
        bool dd525 = media == FDD_MEDIA_720K &&
                     fdd_drive_kind((uint8_t)unit_of(s)) == FDD_MEDIA_1200K;
        snprintf(q, sizeof(q),
                 "Format the disk in %s as %s?\n\nEverything on it will "
                 "be erased.%s",
                 name_of(drive_of(s)), FDD_FORMATS[media].name,
                 dd525 ? "\n(720 KB needs a 96 tpi double-density disk, "
                         "not a 1.2 MB one.)"
                       : "");
    }
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
    int tmp[FDD_MAX_FORMATS];
    /* the floppy drives found at boot (the BIOS setup's drive types, and
     * the controller answered), HDB if there is one */
    for (uint8_t u = 0; u < 2; u++)
        if (fdd_is_present(u) && fdd_drive_formats(u, tmp) > 0) {
            s->ids[s->nids++] = u ? DRIVE_FDD1 : DRIVE_FDD0;
            list_add(s->drive, u ? "FDD1" : "FDD0");
        }
    if (ata_unit(1) && ata_unit(1)->present) {
        s->ids[s->nids++] = DRIVE_HDB;
        list_add(s->drive, "HDB");
    }
    list_set_sel(s->drive, s->nids ? 0 : -1);
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
