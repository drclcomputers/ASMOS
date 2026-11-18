/* Setup - put ASMOS on a hard disk (fs/install.h does the work).
 *
 * Starts by itself when the computer boots from the setup floppy
 * (g_setup_mode). On an installed system it is not in the Apps menu;
 * "app run Setup" in ASMTerm starts it, to install on another disk (or over
 * this one) from a setup floppy in FDD0.
 *
 * Pages: welcome, which disk, what to do with it (erase / keep the files)
 * and the screen mode, a last look, the install (a step a frame, with a
 * progress bar), done (restart). */
#include "config/video_cfg.h"
#include "drivers/ata.h"
#include "drivers/gpu.h"
#include "fs/fs.h"
#include "fs/install.h"
#include "lib/graphics.h"
#include "lib/io.h"
#include "lib/time.h"
#include "os/api.h"
#include "os/os.h"
#include "os/watchdog.h"
#include "ui/desktop.h"
#include "ui/menubar.h"

enum { P_WELCOME, P_DISK, P_HOW, P_CONFIRM, P_WORK, P_DONE, P_FAILED };
enum { S_ERASE, S_BOOT, S_FILES, S_END };

typedef struct {
    window *win; /* must stay first */
    int page, want;
    control *list, *erase, *keep, *vmode, *bar, *msg;
    inst_disk_t disk[2];
    int unit;
    bool keep_files;
    int video;
    int step;
    const char *err;
    uint32_t last_draw;
} setup_t;

app_descriptor setup_app;

#define W 296
#define H 150

/* ── helpers ────────────────────────────────────────────────────────────── */

static void text(setup_t *s, int y, const char *t) {
    ctl_label(s->win, 10, y, W - 20, 10, t, 0);
}

static void bold(setup_t *s, int y, const char *t) {
    control *c = ctl_label(s->win, 10, y, W - 20, 11, t, 0);
    ctl_set_colors(c, BLUE, LIGHT_GRAY);
}

static control *button(setup_t *s, int x, const char *t, int id,
                       ctl_handler h) {
    control *b =
        ctl_button(s->win, x, H - 38, 66, 14, t, id, h); /* client: H - 20 */
    b->user = s;
    return b;
}

static bool source_ok(void) {
    dir_entry_t de;
    return fs_find(SETUP_KERNEL, &de) && fs_find(SETUP_ZIP, &de);
}

static const char *kind_name(const inst_disk_t *d) {
    switch (d->kind) {
    case INST_ASMOS:
        return "ASMOS";
    case INST_FAT:
        return "FAT (DOS?)";
    case INST_EMPTY:
        return "empty";
    case INST_OTHER:
        return "other";
    default:
        return "";
    }
}

static uint32_t mb(uint32_t sectors) { return sectors / 2048; }

/* the disk ASMOS runs from now (not on the setup floppy) */
static bool running_from(int unit) {
    return !g_setup_mode && unit == DRIVE_HDA && fs_drive_mounted(DRIVE_HDA);
}

static void go_page(setup_t *s, int page) { s->want = page; }

/* ── buttons ────────────────────────────────────────────────────────────── */

static void act(setup_t *s, int id) {
    switch (id) {
    case 1: /* Exit */
        os_quit_app_by_desc(&setup_app);
        return;
    case 2: /* Back */
        go_page(s, s->page == P_FAILED ? P_WELCOME : s->page - 1);
        return;
    case 3: /* Continue */
        if (s->page == P_WELCOME) {
            if (!source_ok()) {
                dlg_message(s->win, "Setup",
                            "Put the ASMOS setup floppy in FDD0, "
                            "then press Continue.");
                return;
            }
            go_page(s, P_DISK);
        } else if (s->page == P_DISK) {
            int u = list_sel(s->list);
            if (u < 0 || s->disk[u].kind == INST_NONE)
                return;
            if (!s->disk[u].new_sectors && !s->disk[u].can_keep) {
                dlg_message(s->win, "Setup",
                            "That disk is too small for ASMOS (it needs "
                            "5 MB).");
                return;
            }
            s->unit = u;
            go_page(s, P_HOW);
        } else if (s->page == P_HOW) {
            if (!ctl_value(s->keep) && !ctl_value(s->erase)) {
                dlg_message(s->win, "Setup",
                            "Setup can't use this disk: go Back and pick "
                            "another.");
                return;
            }
            s->keep_files = ctl_value(s->keep) != 0;
            s->video = ctl_value(s->vmode);
            go_page(s, P_CONFIRM);
        }
        return;
    case 4: /* Install */
        s->step = s->keep_files ? S_BOOT : S_ERASE;
        s->err = NULL;
        go_page(s, P_WORK);
        return;
    case 5: /* Restart: as Apps > Restart does */
        power_anim_run(true);
        cpu_reset();
        return;
    }
}

static void on_btn(control *c, int ev) {
    if (ev == EV_CLICK)
        act(c->user, c->id);
}

static void on_list(control *c, int ev) {
    if (ev == EV_SUBMIT) /* double-click / Enter: Continue */
        act(c->user, 3);
}

/* ── pages ──────────────────────────────────────────────────────────────── */

static void clear(setup_t *s) {
    while (s->win->controls)
        ctl_destroy(s->win->controls);
    s->list = s->erase = s->keep = s->vmode = s->bar = s->msg = NULL;
}

static void page_welcome(setup_t *s) {
    bold(s, 6, "Welcome to ASMOS Setup");
    text(s, 24, "Setup puts ASMOS on a hard disk, so the computer");
    text(s, 35, "can start from it without the floppy.");
    text(s, 52, "It asks which disk to use and what to do with what");
    text(s, 63, "is on it now. Nothing is written before you press");
    text(s, 74, "Install on the last page.");
    if (!source_ok())
        text(s, 92, "Put the ASMOS setup floppy in FDD0 first.");
    button(s, 10, "Exit", 1, on_btn);
    control *c = button(s, W - 76, "Continue", 3, on_btn);
    ctl_style_add(c, CS_DEFAULT);
    ctl_focus(c);
}

static void page_disk(setup_t *s) {
    bold(s, 6, "Which hard disk?");
    s->list = ctl_list(s->win, 10, 22, W - 20, 30, 0, on_list);
    s->list->user = s;
    int pick = -1;
    for (int u = 0; u < 2; u++) {
        inst_probe((uint8_t)u, &s->disk[u]);
        const inst_disk_t *d = &s->disk[u];
        char line[96];
        if (d->kind == INST_NONE)
            snprintf(line, sizeof(line), "%s: (no disk)", u ? "HDB" : "HDA");
        else
            snprintf(line, sizeof(line), "%s: %.24s, %u MB, %s",
                     u ? "HDB" : "HDA", d->model[0] ? d->model : "disk",
                     mb(d->sectors), kind_name(d));
        list_add(s->list, line);
        if (pick < 0 && d->kind != INST_NONE && !running_from(u))
            pick = u;
    }
    list_set_sel(s->list, pick < 0 ? 0 : pick);
    text(s, 58, "ASMOS uses one FAT16 partition of up to 2 GB. If the");
    text(s, 69, "disk is bigger, the rest stays free.");
    if (s->disk[0].kind == INST_NONE && s->disk[1].kind == INST_NONE)
        text(s, 86, "No hard disk was found.");
    button(s, 10, "Back", 2, on_btn);
    control *c = button(s, W - 76, "Continue", 3, on_btn);
    ctl_style_add(c, CS_DEFAULT);
    ctl_focus(s->list);
}

static void page_how(setup_t *s) {
    const inst_disk_t *d = &s->disk[s->unit];
    char t[96];
    snprintf(t, sizeof(t), "%s: %.30s", s->unit ? "HDB" : "HDA", d->model);
    bold(s, 6, t);
    bool can_erase = d->new_sectors && !running_from(s->unit);
    snprintf(t, sizeof(t), "Erase the disk, then install (%u MB for ASMOS)",
             mb(d->new_sectors));
    s->erase = ctl_radio(s->win, 10, 22, W - 20, t, 1, can_erase, NULL);
    s->keep = ctl_radio(s->win, 10, 36, W - 20,
                        "Keep the files on it: install over them", 1,
                        !can_erase && d->can_keep, NULL);
    ctl_enable(s->erase, can_erase);
    ctl_enable(s->keep, d->can_keep);
    if (!can_erase && d->new_sectors)
        text(s, 50, "  (not this one: ASMOS is running from it)");
    else if (!d->can_keep) {
        snprintf(t, sizeof(t), "  (can't keep: %s)",
                 d->why_not ? d->why_not : "?");
        text(s, 50, t);
    } else if (d->kind == INST_ASMOS)
        text(s, 50, "  (an update: your files and settings stay)");
    else
        text(s, 50, "  (your files stay; ASMOS's go in SYSTEM and APPS)");
    /* the screens this machine can take (config/video_cfg.h): 0 .. max */
    text(s, 70, "Screen:");
    s->vmode = ctl_combo(s->win, 60, 68, 176, NULL);
    int max = video_max_mode();
    for (int m = 0; m <= max; m++)
        list_add(s->vmode, video_mode_name(m));
    list_set_sel(s->vmode, g_video_mode <= max ? g_video_mode : max);
    if (video_limit_reason()) {
        char why[64];
        snprintf(why, sizeof(why), "(no more: %s)", video_limit_reason());
        text(s, 84, why);
    }
    button(s, 10, "Back", 2, on_btn);
    control *c = button(s, W - 76, "Continue", 3, on_btn);
    ctl_style_add(c, CS_DEFAULT);
    ctl_focus(c);
}

static void page_confirm(setup_t *s) {
    const inst_disk_t *d = &s->disk[s->unit];
    char t[96];
    bold(s, 6, "Ready to install");
    const char *name = s->unit ? "HDB" : "HDA";
    if (s->keep_files) {
        snprintf(t, sizeof(t), "On %s, keeping its files, Setup will:", name);
        text(s, 22, t);
        text(s, 36, " - make it start ASMOS (its boot sectors)");
        text(s, 47, " - copy SYSTEM\\KERNEL.SYS");
        text(s, 58, " - copy the apps into APPS, replacing old ones");
    } else {
        snprintf(t, sizeof(t), "EVERYTHING on %s (%.24s) will be erased.", name,
                 d->model);
        bold(s, 22, t);
        text(s, 36, "Setup will:");
        snprintf(t, sizeof(t), " - make one FAT16 partition of %u MB",
                 mb(d->new_sectors));
        text(s, 47, t);
        text(s, 58, " - copy SYSTEM\\KERNEL.SYS and the apps");
    }
    text(s, 69, " - make the disk start ASMOS");
    snprintf(t, sizeof(t), "Screen: %s.", video_mode_name(s->video));
    text(s, 86, t);
    button(s, 10, "Back", 2, on_btn);
    control *c = button(s, W - 76, "Install", 4, on_btn);
    ctl_focus(c);
}

static void page_work(setup_t *s) {
    bold(s, 6, "Installing ASMOS");
    text(s, 24, "Don't turn the computer off or take the floppy out.");
    s->bar = ctl_progress(s->win, 10, 48, W - 20, 10, 100);
    s->msg = ctl_label(s->win, 10, 62, W - 20, 10, "", 0);
}

static void page_done(setup_t *s) {
    char t[96];
    bold(s, 6, "ASMOS is installed");
    snprintf(t, sizeof(t), "ASMOS is on %s.", s->unit ? "HDB" : "HDA");
    text(s, 24, t);
    if (g_setup_mode) {
        text(s, 40, "Take the floppy out of FDD0, then press");
        text(s, 51, "Restart. The computer starts from the hard disk.");
    } else {
        text(s, 40, "It starts ASMOS the next time the computer starts");
        text(s, 51, "from it (the BIOS's boot order).");
        button(s, 10, "Close", 1, on_btn);
    }
    control *c = button(s, W - 76, "Restart", 5, on_btn);
    ctl_style_add(c, CS_DEFAULT);
    ctl_focus(c);
}

static void page_failed(setup_t *s) {
    bold(s, 6, "Setup could not finish");
    char t[120];
    snprintf(t, sizeof(t), "%s.", s->err ? s->err : "Something went wrong");
    text(s, 24, t);
    if (!s->keep_files && s->step > S_ERASE)
        text(s, 40, "The disk was erased: run Setup again to finish.");
    button(s, 10, "Exit", 1, on_btn);
    button(s, W - 76, "Start over", 2, on_btn);
}

static void build(setup_t *s) {
    clear(s);
    s->page = s->want;
    switch (s->page) {
    case P_WELCOME:
        page_welcome(s);
        break;
    case P_DISK:
        page_disk(s);
        break;
    case P_HOW:
        page_how(s);
        break;
    case P_CONFIRM:
        page_confirm(s);
        break;
    case P_WORK:
        page_work(s);
        break;
    case P_DONE:
        page_done(s);
        break;
    default:
        page_failed(s);
        break;
    }
    wm_invalidate(s->win);
}

/* ── the install ───────────────────────────────────────────────────────── */

/* inside a long step: the bar moves on the screen now */
static void progress(void *user, int pct, const char *what) {
    setup_t *s = user;
    ctl_set_value(s->bar, pct);
    ctl_set_text(s->msg, what);
    uint32_t now = time_millis();
    if (now - s->last_draw < 150)
        return;
    s->last_draw = now;
    int y0, y1;
    int drawn = wm_compose(&g_menubar, &y0, &y1);
    if (drawn == WM_FRAME_FULL)
        blit();
    else if (drawn == WM_FRAME_PART)
        gpu_blit_rows(y0, y1);
}

static void work(setup_t *s) {
    uint8_t unit = (uint8_t)s->unit;
    wd_busy();
    switch (s->step) {
    case S_ERASE:
        s->err = inst_erase(unit, progress, s);
        break;
    case S_BOOT:
        progress(s, 26, "Making the disk start ASMOS");
        s->err = inst_write_boot(unit, s->video);
        if (!s->err && !fs_mount_drive(unit))
            s->err = "the new volume does not mount";
        break;
    case S_FILES:
        s->err =
            inst_copy_files(unit, SETUP_KERNEL, SETUP_ZIP, true, progress, s);
        break;
    }
    if (s->err) {
        go_page(s, P_FAILED);
        return;
    }
    if (++s->step == S_END) {
        progress(s, 100, "Done");
        go_page(s, P_DONE);
    }
}

/* ── app ───────────────────────────────────────────────────────────────── */

static void setup_frame(void *state) {
    setup_t *s = state;
    if (!s->win)
        return;
    if (s->want != s->page) {
        build(s);
        return; /* drawn before the work starts */
    }
    if (s->page == P_WORK && !s->err)
        work(s);
}

static void setup_init(void *state) {
    setup_t *s = state;
    int x = (SCREEN_WIDTH - W) / 2, y = (SCREEN_HEIGHT - H) / 2 - 6;
    s->win = app_window(s, "ASMOS Setup", x < 0 ? 0 : x, y < 12 ? 12 : y, W, H,
                        BLUE, 0);
    if (!s->win)
        return;
    s->want = P_WELCOME;
    s->page = -1;
}

app_descriptor setup_app = {
    .name = "SETUP",
    .state_size = sizeof(setup_t),
    .init = setup_init,
    .on_frame = setup_frame,
    .single_instance = true,
};
