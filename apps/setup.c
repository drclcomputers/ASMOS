// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "config/runtime_config.h"
#include "config/video_cfg.h"
#include "drivers/ata.h"
#include "drivers/gpu.h"
#include "drivers/lpt.h"
#include "fs/fs.h"
#include "fs/install.h"
#include "lib/graphics.h"
#include "lib/io.h"
#include "lib/string.h"
#include "lib/time.h"
#include "os/api.h"
#include "os/os.h"
#include "os/print.h"
#include "os/watchdog.h"
#include "ui/desktop.h"
#include "ui/menubar.h"

/* Setup: puts ASMOS on a hard disk, from the setup floppy, with the apps
 * the user picks (SETUP.INF: Minimal, Medium, Full, or each one), sound
 * and printer settings. Run on an installed ASMOS with the floppy in, it
 * also adds and removes apps.
 *
 *   install:  Welcome, Disk, How (erase/keep, screen), Parts (network?
 *             media? games?), Size (Minimal..Custom), [Custom], Settings,
 *             Confirm, Work, Done
 *   change:   Welcome, Parts, Custom, Confirm, Work, Done */

enum {
    P_WELCOME,
    P_DISK,
    P_HOW,
    P_PARTS,
    P_SIZE,
    P_CUSTOM,
    P_SETTINGS,
    P_CONFIRM,
    P_WORK,
    P_DONE,
    P_FAILED
};
enum { S_ERASE, S_BOOT, S_FILES, S_SETTINGS, S_CHANGE, S_END };
enum { SZ_MIN, SZ_MEDIUM, SZ_FULL, SZ_CUSTOM };

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
    /* the apps */
    bool change; /* adding and removing apps on this ASMOS */
    control *mode_change;
    inst_part_t parts[INST_PARTS_MAX];
    int nparts;
    bool pick[INST_PARTS_MAX]; /* to be there afterwards */
    bool had[INST_PARTS_MAX];  /* there now (an update, a change) */
    control *c_app[INST_PARTS_MAX];
    bool net, media, games, picked; /* picked: Custom's ticks made */
    control *c_net, *c_media, *c_games;
    int size;
    control *c_size[4];
    /* the settings */
    inst_settings_t set;
    control *c_sound, *c_chime, *c_ptype, *c_pport;
    bool net_was; /* (a change: networking on before) */
} setup_t;

app_descriptor setup_app;
void desktop_refresh_apps_menu(void);

#define W 296
#define H 150

// helpers

static void text(setup_t *s, int y, const char *t) {
    ctl_label(s->win, 10, y, W - 20, 10, t, 0);
}

static void bold(setup_t *s, int y, const char *t) {
    control *c = ctl_label(s->win, 10, y, W - 20, 11, t, 0);
    ctl_set_colors(c, BLUE, LIGHT_GRAY);
}

static void grey(setup_t *s, int x, int y, const char *t) {
    control *c = ctl_label(s->win, x, y, W - 10 - x, 10, t, 0);
    ctl_set_colors(c, DARK_GRAY, LIGHT_GRAY);
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

/* HDA has an ASMOS: Setup may change its apps (from the floppy) */
static bool can_change(void) {
    inst_disk_t d;
    inst_probe(0, &d);
    if (d.kind != INST_ASMOS ||
        (!fs_drive_mounted(DRIVE_HDA) && !fs_mount_drive(DRIVE_HDA)))
        return false;
    dir_entry_t de;
    return fs_find("/HDA/APPS", &de);
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

static int s_modes[VIDEO_MODES], s_nmodes; /* the screen list's modes */

/* the disk ASMOS runs from now (not on the setup floppy) */
static bool running_from(int unit) {
    return !g_setup_mode && unit == DRIVE_HDA && fs_drive_mounted(DRIVE_HDA);
}

static void go_page(setup_t *s, int page) { s->want = page; }

// the apps

static uint8_t target(const setup_t *s) {
    return s->change ? DRIVE_HDA : (uint8_t)s->unit;
}

static bool group_ok(const setup_t *s, int k) {
    switch (s->parts[k].group) {
    case PART_MEDIA:
        return s->media;
    case PART_GAMES:
        return s->games;
    case PART_NET:
        return s->net;
    default:
        return true;
    }
}

/* the apps in a group (", "-joined), for the questions */
static void group_names(const setup_t *s, int group, char *out, int sz) {
    out[0] = 0;
    for (int k = 0; k < s->nparts; k++)
        if (s->parts[k].group == group) {
            if (out[0])
                strlcat(out, ", ", (size_t)sz);
            strlcat(out, s->parts[k].name, (size_t)sz);
        }
}

/* what was there: read once the target disk is known */
static void read_had(setup_t *s) {
    s->nparts = inst_read_parts(SETUP_INF, s->parts, INST_PARTS_MAX);
    uint8_t d = target(s);
    bool there = s->change || (s->keep_files && fs_drive_mounted(d));
    bool any_had = false;
    for (int k = 0; k < s->nparts; k++) {
        s->had[k] = there && inst_part_installed(d, &s->parts[k]);
        any_had |= s->had[k];
    }
    /* the questions start from what is there (else: yes) */
    s->net = s->media = s->games = true;
    if (any_had) {
        bool g[4] = {false};
        for (int k = 0; k < s->nparts; k++)
            g[s->parts[k].group] |= s->had[k];
        s->media = g[PART_MEDIA];
        s->games = g[PART_GAMES];
        s->net = g[PART_NET];
    }
    if (s->change)
        s->net = inst_read_network(DRIVE_HDA);
    s->net_was = s->net;
    s->size = any_had ? SZ_CUSTOM : SZ_FULL;
    s->picked = false;
}

/* the ticks for a size (within the answers) */
static void pick_size(setup_t *s) {
    for (int k = 0; k < s->nparts; k++)
        s->pick[k] = group_ok(s, k) && s->parts[k].size <= s->size;
}

/* Custom's ticks, the first time: what is there (an update, a change), a
 * group newly said yes to all of it; else Minimal's */
static void pick_custom(setup_t *s) {
    bool any_had = false;
    for (int k = 0; k < s->nparts; k++)
        any_had |= s->had[k];
    for (int k = 0; k < s->nparts; k++) {
        bool group_had = false;
        for (int j = 0; j < s->nparts; j++)
            group_had |= s->parts[j].group == s->parts[k].group && s->had[j];
        bool newly = s->parts[k].group != PART_ANY && !group_had;
        s->pick[k] = group_ok(s, k) && (any_had ? s->had[k] || newly
                                                : s->parts[k].size == PART_MIN);
    }
    s->picked = true;
}

static uint32_t picked_kb(const setup_t *s, int *count) {
    uint32_t kb = 0;
    int n = 0;
    for (int k = 0; k < s->nparts; k++)
        if (s->pick[k]) {
            kb += s->parts[k].kb;
            n++;
        }
    if (count)
        *count = n;
    return kb;
}

static void read_answers(setup_t *s) {
    if (s->c_net) {
        bool net = ctl_value(s->c_net), media = ctl_value(s->c_media),
             games = ctl_value(s->c_games);
        if (net != s->net || media != s->media || games != s->games)
            s->picked = false; /* (Custom's ticks: made again) */
        s->net = net;
        s->media = media;
        s->games = games;
    }
    for (int k = 0; k < 4; k++)
        if (s->c_size[k] && ctl_value(s->c_size[k]))
            s->size = k;
    for (int k = 0; k < s->nparts; k++)
        if (s->c_app[k])
            s->pick[k] = ctl_value(s->c_app[k]) != 0;
    if (s->c_sound) {
        s->set.sound = ctl_value(s->c_sound) != 0;
        s->set.chime = ctl_value(s->c_chime) != 0;
        s->set.printer_type = (uint8_t)list_sel(s->c_ptype);
        s->set.printer_port = (uint8_t)list_sel(s->c_pport);
    }
}

// the way through

static int next_of(const setup_t *s, int page) {
    switch (page) {
    case P_WELCOME:
        return s->change ? P_PARTS : P_DISK;
    case P_DISK:
        return P_HOW;
    case P_HOW:
        return P_PARTS;
    case P_PARTS:
        return s->change ? P_CUSTOM : P_SIZE;
    case P_SIZE:
        return s->size == SZ_CUSTOM ? P_CUSTOM : P_SETTINGS;
    case P_CUSTOM:
        return s->change ? P_CONFIRM : P_SETTINGS;
    case P_SETTINGS:
        return P_CONFIRM;
    default:
        return page;
    }
}

static int back_of(const setup_t *s, int page) {
    switch (page) {
    case P_PARTS:
        return s->change ? P_WELCOME : P_HOW;
    case P_CUSTOM:
        return s->change ? P_PARTS : P_SIZE;
    case P_SETTINGS:
        return s->size == SZ_CUSTOM ? P_CUSTOM : P_SIZE;
    case P_CONFIRM:
        return s->change ? P_CUSTOM : P_SETTINGS;
    case P_FAILED:
        return P_WELCOME;
    default:
        return page > 0 ? page - 1 : 0;
    }
}

// buttons

static void act(setup_t *s, int id) {
    read_answers(s);
    switch (id) {
    case 1: /* Exit */
        os_quit_app_by_desc(&setup_app);
        return;
    case 2: /* Back */
        go_page(s, back_of(s, s->page));
        return;
    case 3: /* Continue */
        if (s->page == P_WELCOME) {
            if (!source_ok()) {
                dlg_message(s->win, "Setup",
                            "Put the ASMOS setup floppy in FDD0, "
                            "then press Continue.");
                return;
            }
            s->change = s->mode_change && ctl_value(s->mode_change);
            if (s->change)
                read_had(s);
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
        } else if (s->page == P_HOW) {
            if (!ctl_value(s->keep) && !ctl_value(s->erase)) {
                dlg_message(s->win, "Setup",
                            "Setup can't use this disk: go Back and pick "
                            "another.");
                return;
            }
            s->keep_files = ctl_value(s->keep) != 0;
            s->video = s_modes[ctl_value(s->vmode) % VIDEO_MODES];
            read_had(s);
            s->set.network = true;
        } else if (s->page == P_PARTS) {
            if (s->change) {
                if (!s->picked)
                    pick_custom(s);
            }
        } else if (s->page == P_SIZE) {
            if (s->size == SZ_CUSTOM) {
                if (!s->picked)
                    pick_custom(s);
            } else {
                pick_size(s);
                s->picked = false;
            }
        }
        go_page(s, next_of(s, s->page));
        return;
    case 4: /* Install */
        s->set.network = s->net;
        s->step = s->change ? S_CHANGE : s->keep_files ? S_BOOT : S_ERASE;
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

/* a tick changed: the total again */
static void show_total(setup_t *s);
static void on_app(control *c, int ev) {
    if (ev == EV_CHANGE) { /* (a check box: ticked or not) */
        read_answers(c->user);
        show_total(c->user);
    }
}

// pages

static void clear(setup_t *s) {
    while (s->win->controls)
        ctl_destroy(s->win->controls);
    s->list = s->erase = s->keep = s->vmode = s->bar = s->msg = NULL;
    s->mode_change = s->c_net = s->c_media = s->c_games = NULL;
    s->c_sound = s->c_chime = s->c_ptype = s->c_pport = NULL;
    memset(s->c_size, 0, sizeof(s->c_size));
    memset(s->c_app, 0, sizeof(s->c_app));
}

static void continue_button(setup_t *s, const char *t, int id) {
    control *c = button(s, W - 76, t, id, on_btn);
    ctl_style_add(c, CS_DEFAULT);
    ctl_focus(c);
}

static void page_welcome(setup_t *s) {
    bold(s, 6, "Welcome to ASMOS Setup");
    if (can_change()) {
        text(s, 24, "What do you want to do?");
        s->mode_change =
            ctl_radio(s->win, 10, 38, W - 20,
                      "Add or take out apps (the ASMOS on HDA)", 1, true, NULL);
        ctl_radio(s->win, 10, 52, W - 20, "Install ASMOS on a hard disk", 1,
                  false, NULL);
        text(s, 70, "Setup takes the apps from the setup floppy.");
    } else {
        text(s, 24, "Setup puts ASMOS on a hard disk, so the computer");
        text(s, 35, "can start from it without the floppy.");
        text(s, 52, "It asks which disk to use, which apps you want,");
        text(s, 63, "and a few settings. Nothing is written before you");
        text(s, 74, "press Install on the last page.");
    }
    if (!source_ok())
        text(s, 92, "Put the ASMOS setup floppy in FDD0 first.");
    button(s, 10, "Exit", 1, on_btn);
    continue_button(s, "Continue", 3);
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

    text(s, 70, "Screen:");
    s->vmode = ctl_combo(s->win, 60, 68, 176, NULL);
    s_nmodes = 0;
    int sel = 0;
    for (int sz = 0; sz < VIDEO_SIZES; sz++)
        for (int k = 0; k < 2; k++) {
            int m = video_mode_of(sz, k ? 256 : 16);
            if (!video_mode_allowed(m))
                continue;
            if (m == g_video_mode)
                sel = s_nmodes;
            s_modes[s_nmodes++] = m;
            list_add(s->vmode, video_mode_name(m));
        }
    list_set_sel(s->vmode, sel);
    if (!video_mode_allowed(VIDEO_800) || !video_mode_allowed(VIDEO_800_256)) {
        char why[72];
        snprintf(why, sizeof(why), "(not offered: %s)",
                 video_mode_reason(video_mode_allowed(VIDEO_800) ? VIDEO_800_256
                                                                 : VIDEO_800));
        text(s, 84, why);
    }
    button(s, 10, "Back", 2, on_btn);
    continue_button(s, "Continue", 3);
}

/* the three questions */
static void page_parts(setup_t *s) {
    char names[120], t[160];
    bold(s, 6, "What do you want on it?");
    s->c_net =
        ctl_check(s->win, 10, 22, W - 20,
                  "Network: the Internet, other computers", s->net, NULL);
    group_names(s, PART_NET, names, sizeof(names));
    snprintf(t, sizeof(t), "%s. Off: more memory free.", names);
    grey(s, 24, 33, t);
    s->c_media =
        ctl_check(s->win, 10, 46, W - 20, "Pictures and sound", s->media, NULL);
    group_names(s, PART_MEDIA, names, sizeof(names));
    grey(s, 24, 57, names);
    s->c_games = ctl_check(s->win, 10, 70, W - 20, "Games", s->games, NULL);
    group_names(s, PART_GAMES, names, sizeof(names));
    grey(s, 24, 81, names);
    text(s, 96, "Run Setup again to change these later.");
    button(s, 10, "Back", 2, on_btn);
    continue_button(s, "Continue", 3);
}

/* the size: Minimal, Medium, Full (within the answers), Custom */
static void page_size(setup_t *s) {
    static const char *const NAME[3] = {"Minimal", "Medium", "Full"};
    bold(s, 6, "How much of ASMOS?");
    for (int z = 0; z < 4; z++) {
        int y = 20 + z * 22;
        char t[96], what[120] = "";
        if (z == SZ_CUSTOM) {
            s->c_size[z] = ctl_radio(s->win, 10, y, W - 20, "Custom", 1,
                                     s->size == z, NULL);
            grey(s, 24, y + 10, "You pick each app");
            continue;
        }
        uint32_t kb = 0;
        int n = 0, shown = 0;
        for (int k = 0; k < s->nparts; k++) {
            if (!group_ok(s, k) || s->parts[k].size > z)
                continue;
            kb += s->parts[k].kb;
            n++;
            /* (each size: what it adds) */
            if (s->parts[k].size == z && z < SZ_FULL && shown < 6) {
                if (what[0])
                    strlcat(what, ", ", sizeof(what));
                strlcat(what, s->parts[k].name, sizeof(what));
                shown++;
            }
        }
        snprintf(t, sizeof(t), "%s: %d apps, %u KB", NAME[z], n, (unsigned)kb);
        s->c_size[z] =
            ctl_radio(s->win, 10, y, W - 20, t, 1, s->size == z, NULL);
        char d[140];
        if (z == SZ_MIN)
            snprintf(d, sizeof(d), "The built-in apps, %s", what);
        else if (z == SZ_MEDIUM)
            snprintf(d, sizeof(d), "And %s", what[0] ? what : "nothing more");
        else
            snprintf(d, sizeof(d), "Every app");
        grey(s, 24, y + 10, d);
    }
    button(s, 10, "Back", 2, on_btn);
    continue_button(s, "Continue", 3);
}

static void show_total(setup_t *s) {
    if (!s->msg)
        return;
    int n;
    uint32_t kb = picked_kb(s, &n);
    char t[64];
    snprintf(t, sizeof(t), "%d apps, %u KB", n, (unsigned)kb);
    ctl_set_text(s->msg, t);
}

/* each app */
static void page_custom(setup_t *s) {
    bold(s, 6,
         s->change ? "Tick the apps to have, untick to take out"
                   : "Pick the apps");
    grey(s, 10, 18, "Always: FileF, Settings, Help, TEditor, ASMTerm...");
    int cols = s->nparts > 12 ? 3 : 2, rows = (s->nparts + cols - 1) / cols;
    int cw = (W - 20) / cols;
    for (int k = 0; k < s->nparts; k++) {
        char t[48];
        snprintf(t, sizeof(t), "%s", s->parts[k].name);
        s->c_app[k] =
            ctl_check(s->win, 10 + (k / rows) * cw, 30 + (k % rows) * 11,
                      cw - 4, t, s->pick[k], on_app);
        s->c_app[k]->user = s;
        char tip[80];
        snprintf(tip, sizeof(tip), "%u KB%s", (unsigned)s->parts[k].kb,
                 s->had[k] ? ", there now" : "");
        ctl_set_tooltip(s->c_app[k], tip);
    }
    s->msg = ctl_label(s->win, 10, 32 + rows * 11, W - 20, 10, "", 0);
    show_total(s);
    button(s, 10, "Back", 2, on_btn);
    continue_button(s, "Continue", 3);
}

/* sound and the printer (the rest: Settings) */
static void page_settings(setup_t *s) {
    bold(s, 6, "Sound and printer");
    s->c_sound = ctl_check(s->win, 10, 22, W - 20,
                           "Sound (Sound Blaster, AdLib, the PC speaker)",
                           s->set.sound, NULL);
    s->c_chime = ctl_check(s->win, 10, 34, W - 20,
                           "The chime when ASMOS starts", s->set.chime, NULL);
    text(s, 52, "Printer:");
    s->c_ptype = ctl_combo(s->win, 60, 50, 150, NULL);
    for (int k = 0; k < PRN_TYPES; k++)
        list_add(s->c_ptype, PRN_TYPE_NAMES[k]);
    list_set_sel(s->c_ptype, s->set.printer_type % PRN_TYPES);
    ctl_label(s->win, 214, 52, 14, 10, "on", 0);
    s->c_pport = ctl_combo(s->win, 230, 50, 56, NULL);
    for (int k = 0; k < LPT_MAX; k++) {
        char t[8];
        snprintf(t, sizeof(t), "LPT%d", k + 1);
        list_add(s->c_pport, t);
    }
    list_set_sel(s->c_pport, s->set.printer_port % LPT_MAX);
    grey(s, 60, 64, "Text works on any printer.");
    text(s, 84, "The screen, clock, network address and the rest:");
    text(s, 95, "Settings, once ASMOS runs.");
    button(s, 10, "Back", 2, on_btn);
    continue_button(s, "Continue", 3);
}

/* the names of the apps going in (in) or out, "A, B, C" */
static void changes(const setup_t *s, bool in, char *out, int sz) {
    out[0] = 0;
    for (int k = 0; k < s->nparts; k++)
        if (in ? s->pick[k] && !s->had[k] : !s->pick[k] && s->had[k]) {
            if (out[0])
                strlcat(out, ", ", (size_t)sz);
            strlcat(out, s->parts[k].name, (size_t)sz);
        }
}

static void page_confirm(setup_t *s) {
    const inst_disk_t *d = &s->disk[s->unit];
    char t[140], names[200];
    if (s->change) {
        bold(s, 6, "Ready to change the apps");
        int y = 22;
        changes(s, true, names, sizeof(names));
        snprintf(t, sizeof(t), "Add: %s", names[0] ? names : "nothing");
        text(s, y, t);
        y += 14;
        changes(s, false, names, sizeof(names));
        snprintf(t, sizeof(t), "Take out: %s", names[0] ? names : "nothing");
        text(s, y, t);
        y += 14;
        if (s->net != s->net_was) {
            text(s, y,
                 s->net ? "Network: on (when ASMOS starts again)"
                        : "Network: off (when ASMOS starts again)");
            y += 14;
        }
        text(s, y + 4, "Your own files are not touched.");
        button(s, 10, "Back", 2, on_btn);
        continue_button(s, "Change", 4);
        return;
    }
    bold(s, 6, "Ready to install");
    const char *name = s->unit ? "HDB" : "HDA";
    if (s->keep_files) {
        snprintf(t, sizeof(t), "On %s, keeping its files, Setup will:", name);
        text(s, 22, t);
        text(s, 34, " - make it start ASMOS, copy SYSTEM\\KERNEL.SYS");
    } else {
        snprintf(t, sizeof(t), "EVERYTHING on %s (%.24s) will be erased.", name,
                 d->model);
        bold(s, 22, t);
        snprintf(t, sizeof(t),
                 " - make one FAT16 partition of %u MB, copy the system",
                 mb(d->new_sectors));
        text(s, 34, t);
    }
    int n;
    uint32_t kb = picked_kb(s, &n);
    static const char *const SIZE[4] = {"Minimal", "Medium", "Full", "Custom"};
    snprintf(t, sizeof(t), " - put in the apps: %s, %d apps, %u KB",
             SIZE[s->size], n, (unsigned)kb);
    text(s, 45, t);
    changes(s, false, names, sizeof(names));
    if (s->keep_files && names[0]) {
        snprintf(t, sizeof(t), " - take out: %s", names);
        text(s, 56, t);
    }
    snprintf(t, sizeof(t), "Screen: %s. Network %s, sound %s.",
             video_mode_name(s->video), s->net ? "on" : "off",
             s->set.sound ? "on" : "off");
    text(s, 72, t);
    snprintf(t, sizeof(t), "Printer: %s on LPT%d.",
             PRN_TYPE_NAMES[s->set.printer_type % PRN_TYPES],
             s->set.printer_port + 1);
    text(s, 83, t);
    button(s, 10, "Back", 2, on_btn);
    continue_button(s, "Install", 4);
}

static void page_work(setup_t *s) {
    bold(s, 6, s->change ? "Changing the apps" : "Installing ASMOS");
    text(s, 24,
         s->change ? "Don't take the floppy out."
                   : "Don't turn the computer off or take the "
                     "floppy out.");
    s->bar = ctl_progress(s->win, 10, 48, W - 20, 10, 100);
    s->msg = ctl_label(s->win, 10, 62, W - 20, 10, "", 0);
}

static void page_done(setup_t *s) {
    char t[96];
    if (s->change) {
        bold(s, 6, "The apps are changed");
        if (g_setup_mode) {
            text(s, 24, "Take the floppy out of FDD0, then press");
            text(s, 35, "Restart: ASMOS starts from the hard disk.");
        } else {
            text(s, 24, "They are in the Apps menu now.");
        }
        if (s->net != s->net_was)
            text(s, 52,
                 s->net ? "The network is on when ASMOS starts again."
                        : "The network is off when ASMOS starts again.");
        if (g_setup_mode) {
            button(s, 10, "Close", 1, on_btn);
            continue_button(s, "Restart", 5);
        } else {
            continue_button(s, "Close", 1);
        }
        return;
    }
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
    if (!s->change && !s->keep_files && s->step > S_ERASE)
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
    case P_PARTS:
        page_parts(s);
        break;
    case P_SIZE:
        page_size(s);
        break;
    case P_CUSTOM:
        page_custom(s);
        break;
    case P_SETTINGS:
        page_settings(s);
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

// the install

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
        s->err = inst_copy_files_parts(unit, SETUP_KERNEL, SETUP_ZIP, s->parts,
                                       s->nparts, s->pick, progress, s);
        /* (an update: what was unticked goes) */
        if (!s->err && s->keep_files)
            s->err = inst_change_parts(unit, SETUP_ZIP, s->parts, s->nparts,
                                       s->pick, NULL, NULL);
        break;
    case S_SETTINGS:
        progress(s, 99, "Saving the settings");
        s->err = inst_write_settings(unit, &s->set);
        s->step = S_END - 1; /* (S_CHANGE: not this way) */
        break;
    case S_CHANGE:
        s->err = inst_change_parts(DRIVE_HDA, SETUP_ZIP, s->parts, s->nparts,
                                   s->pick, progress, s);
        if (!s->err && s->net != s->net_was) {
            s->err = inst_write_network(DRIVE_HDA, s->net);
            if (!g_setup_mode) /* (running from HDA: its settings too) */
                g_cfg.networking_enabled = s->net;
        }
        desktop_refresh_apps_menu();
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

// app

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
    s->set.network = g_cfg.networking_enabled != 0;
    s->set.sound = g_cfg.sound_enabled != 0;
    s->set.chime = g_cfg.play_bootchime != 0;
    s->set.printer_type = g_cfg.printer_type;
    s->set.printer_port = g_cfg.printer_port;
}

app_descriptor setup_app = {
    .name = "SETUP",
    .state_size = sizeof(setup_t),
    .init = setup_init,
    .on_frame = setup_frame,
    .single_instance = true,
};
