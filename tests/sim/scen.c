#include "fs/install.h"
#include "fs/zip.h"
#include "sys.h"
#include "ui/uifont.h"
static char *zt_line(char **p) {
    char *s = *p;
    if (!s || !*s)
        return 0;
    char *e = s;
    while (*e && *e != '\n')
        e++;
    if (*e)
        *e++ = 0;
    *p = e;
    return s;
}
extern void sim_save(const char *, const void *, unsigned);
#include "os/appapi.h"
#include "os/appload.h"
app_instance_t *sim_launch(const char *name);
#include "config/config.h"
#include "config/runtime_config.h"
#include "config/video_cfg.h"
#include "drivers/fdd.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "fs/fdd_ctrl.h"
#include "fs/fs.h"
#include "lib/alloc.h"
#include "lib/audio.h"
#include "lib/core.h"
#include "lib/graphics.h"
#include "lib/string.h"
#include "lib/time.h"
#include "os/appguard.h"
#include "os/appload.h"
#include "os/clipboard.h"
#include "os/error.h"
#include "os/os.h"
#include "os/prefs.h"
#include "os/print.h"
#include "shell/cmds.h"
#include "ui/controls.h"
#include "ui/desktop_fs.h"
#include "ui/fileops.h"
#include "ui/menubar.h"
#include "ui/popup.h"
#include "ui/saver.h"
#include "ui/ui.h"
void sim_frame(void);
#include "fs/fat_io.h"
void wr(int fd, const char *s);
bool load_file(const char *path, uint8_t *dst, uint32_t max, uint32_t *got);
void sim_rclick(int, int);
extern const char *sim_arg2;
extern uint32_t g_sim_abort_vec;
void sim_alarm_ms(int);
void sim_shot(const char *);
void sim_frames(int);
void sim_click(int, int);
void sim_log(const char *);
void sim_key(uint8_t);
void sim_ext(uint8_t);
void sim_type(const char *);
void sim_key2(uint8_t, char, bool, bool);
void sim_dblclick(int, int);
void sim_drag(int, int, int, int);
void sim_move(int, int);

static char logbuf[4096];
static int logn;
static void L(const char *s) {
    int n = strlen(s);
    if (logn + n + 2 < 4096) {
        memcpy(logbuf + logn, s, n);
        logn += n;
        logbuf[logn++] = '\n';
        logbuf[logn] = 0;
    }
    sim_log(s);
}

typedef struct {
    window *win;
    control *e, *ml, *lst, *cmb, *sld, *prg, *tabs, *lbl, *chk, *r1, *r2, *vs;
    int clicks;
} gal_t;
static gal_t G;
static void on_any(control *c, int ev) {
    char b[96];
    snprintf(b, sizeof b, "ev %d on type %d id %d text='%s' value=%d sel=%d",
             ev, c->type, c->id, ctl_text(c), ctl_value(c), c->sel);
    L(b);
    if (c == G.sld && ev == EV_CHANGE)
        ctl_set_value(G.prg, c->value);
}
static void painter(control *c, int x, int y, int w, int h) {
    for (int i = 0; i < w; i += 4)
        draw_line(x + i, y, x + w - 1 - i, y + h - 1, (i / 4) % 16);
}
static void got(void *u, const char *t) {
    char b[80];
    snprintf(b, 80, "dialog -> %s", t ? t : "(cancel)");
    L(b);
}
static void gotb(void *u, bool y) { L(y ? "confirm -> yes" : "confirm -> no"); }

static void gallery(void) {
    window *w = app_window(&G, "Gallery", 10, 20, 300, 220, BLUE,
                           AW_RESIZABLE | AW_NO_ANIM);
    G.win = w;
    ctl_label(w, 4, 4, 60, 10, "Name:", 0);
    G.e = ctl_edit(w, 40, 3, 120, 12, "Hello world", 40, 0, on_any);
    control *b = ctl_button(w, 166, 3, 50, 13, "OK", 7, on_any);
    ctl_style_add(b, CS_DEFAULT);
    G.chk = ctl_check(w, 4, 20, 80, "Enabled", true, on_any);
    G.r1 = ctl_radio(w, 90, 20, 50, "One", 1, true, on_any);
    G.r2 = ctl_radio(w, 140, 20, 50, "Two", 1, false, on_any);
    G.lst = ctl_list(w, 4, 34, 110, 60, LBS_HEADER, on_any);
    int cols[2] = {60, 40};
    list_set_columns(G.lst, cols, 2);
    list_add(G.lst, "Name\tSize");
    for (int i = 0; i < 12; i++) {
        char t[32];
        snprintf(t, 32, "FILE%02d.TXT\t%d", i, i * 100);
        list_add(G.lst, t);
    }
    G.cmb = ctl_combo(w, 120, 34, 90, on_any);
    list_add(G.cmb, "Solid");
    list_add(G.cmb, "Checker");
    list_add(G.cmb, "Stripes");
    list_set_sel(G.cmb, 0);
    G.sld = ctl_slider(w, 120, 50, 90, 0, 100, 40, on_any);
    G.prg = ctl_progress(w, 120, 64, 90, 9, 100);
    ctl_set_value(G.prg, 40);
    G.tabs = ctl_tabs(w, 4, 98, 200, "General|Sound|Network", on_any);
    control *g1 = ctl_label(w, 8, 116, 150, 10, "Page 1 content", 0);
    ctl_on_page(g1, G.tabs, 0);
    control *g2 = ctl_label(w, 8, 116, 150, 10, "Page 2 content", 0);
    ctl_on_page(g2, G.tabs, 1);
    G.ml = ctl_edit(w, 4, 130, 200, 60,
                    "line one\nline two is a bit longer than the box width "
                    "maybe\n  indented\n4\n5\n6\n7\n8\n9",
                    0, ES_MULTILINE | CS_STRETCH_W | CS_STRETCH_H, on_any);
    G.vs = ctl_vscroll(w, 216, 34, 60, on_any);
    ctl_set_range(G.vs, 0, 20, 5);
    ctl_group(w, 214, 100, 80, 40, "Group");
    ctl_custom(w, 220, 112, 68, 24, painter, on_any, CS_BORDER);
    control *ok2 = ctl_button(w, 240, 196, 50, 13, "Close", 9, on_any);
    ctl_style_add(ok2, CS_ANCHOR_RIGHT | CS_ANCHOR_BOTTOM);
}

int scen2(const char *name);
char *g_tree_log;
void tree_ev(control *c, int ev) {
    char b[80];
    const char *names[] = {"?",     "CLICK", "CHANGE", "SUBMIT",
                           "KEY",   "FOCUS", "BLUR",   "MDOWN",
                           "MMOVE", "MUP",   "DBL",    "EXPAND"};
    if (ev == EV_EXPAND) {
        int n = tree_event_node(c);
        char nm[28];
        int depth = 0;
        for (int p = tree_parent(c, n); p >= 0; p = tree_parent(c, p))
            depth++;
        if (strcmp(tree_text(c, n), "EMPTY"))
            for (int k = 1; k <= 3; k++) {
                snprintf(nm, sizeof nm, "%s.%d", tree_text(c, n), k);
                tree_add(c, n, nm, 0, depth < 1);
            }
        snprintf(b, sizeof b, "EXPAND %s\n", tree_text(c, n));
    } else
        snprintf(b, sizeof b, "%s sel=%s\n", ev < 12 ? names[ev] : "?",
                 tree_sel(c) >= 0 ? tree_text(c, tree_sel(c)) : "-");
    if (g_tree_log)
        strcat(g_tree_log, b);
}

/* before fs_mount: "setup..." scenarios start from the setup floppy with a
 * blank hard disk (and maybe a second one). 1 = booted from the floppy. */
int sim_pre_mount(const char *sc) {
    if (sc[0] == '+')
        sc++;
    if (!strcmp(sc, "readme320")) {
        extern int g_screen_width, g_screen_height, g_video_mode;
        g_screen_width = 320;
        g_screen_height = 200;
        g_video_mode = 0;
        return 0;
    }
    if (strncmp(sc, "setup", 5))
        return 0;
    if (!strcmp(sc, "setup320")) {
        extern int g_screen_width, g_screen_height, g_video_mode;
        g_screen_width = 320;
        g_screen_height = 200;
        g_video_mode = 0;
    }
    extern void sim_disk_blank(int, uint32_t, const char *);
    extern int g_fdc_drive_type, g_fdcb_drive_type;
    extern uint8_t g_fdc_img[];
    extern void fdc_insert_unit(int, int), fdc_setup(void);
    extern bool load_file(const char *, uint8_t *, uint32_t, uint32_t *);
    uint32_t n;
    if (!strcmp(sc, "setupkeep")) { /* HDA: an installed ASMOS (the image) */
    } else
        sim_disk_blank(0, 100 * 2048, "QUANTUM FIREBALL 100");
    if (!strcmp(sc, "setup2"))
        sim_disk_blank(1, 40 * 2048, "CONNER CP3044");
    g_fdc_drive_type = 4;
    g_fdcb_drive_type = 0;
    fdc_setup();
    load_file(ROOT "setup.img", g_fdc_img, 2880 * 512, &n);
    fdc_insert_unit(0, 1440);
    return 1;
}

int scenario(const char *name) {
    if (scen2(name) >= 0)
        return 0;
    if (!strcmp(name, "desktop")) {
        sim_frames(2);
        sim_shot("desktop");
        return 0;
    }
    if (!strcmp(name, "gallery")) {
        gallery();
        sim_frames(2);
        sim_shot("g1");
        int cx = 11, cy = 20 + 10 + 16; /* client origin */
        /* edit: click at end, type, arrows, shift-select, backspace */
        sim_click(cx + 150, cy + 8);
        sim_type("!!");
        sim_ext(0x4B);
        sim_ext(0x4B);
        sim_type("X");
        sim_key2(0x47 | 0, 0, true, false); /* shift+Home via non-ext scancode
                                               0x47 is numpad 7 - test keypad */
        L(ctl_text(G.e));
        sim_ext(0x47);
        kb_process_byte(0x2A);
        sim_ext(0x4D);
        sim_ext(0x4D);
        kb_process_byte(0xAA); /* shift+right x2 */
        sim_key(0x0E);
        L(ctl_text(G.e));
        /* list: click row 3, down arrow, dblclick row 5 */
        sim_click(cx + 20, cy + 34 + 2 + 9 + 2 * 8 + 3);
        sim_ext(0x50);
        sim_dblclick(cx + 20, cy + 34 + 2 + 9 + 4 * 8 + 3);
        /* combo open & pick */
        sim_click(cx + 160, cy + 40);
        sim_frames(1);
        sim_shot("g2_combo");
        sim_click(cx + 150, cy + 47 + 2 + 2 * 8 + 3);
        /* slider drag */
        sim_drag(cx + 120 + 4 + 36, cy + 55, cx + 120 + 80, cy + 55);
        /* tabs */
        sim_click(cx + 50, cy + 104);
        /* checkbox, radio */
        sim_click(cx + 6, cy + 25);
        sim_click(cx + 145, cy + 25);
        /* multiline: click line 2, End, Enter, type */
        sim_click(cx + 30, cy + 130 + 2 + 8 + 3);
        sim_ext(0x4F);
        sim_key(0x1C);
        sim_type("new line");
        /* tab focus cycle and Enter = default */
        sim_click(cx + 60, cy + 8);
        sim_key(0x1C);
        sim_frames(1);
        sim_shot("g3");
        /* resize window */
        sim_drag(10 + 300 - 4, 20 + 10 + 220 - 4, 10 + 360, 20 + 10 + 260);
        sim_frames(1);
        sim_shot("g4_resized");
        /* dialogs */
        dlg_input(G.win, "Rename", "New name:", "OLD.TXT", 12, ES_FILENAME, got,
                  0);
        sim_frames(1);
        sim_type("new.txt");
        sim_frames(1);
        sim_shot("g5_input");
        sim_key(0x1C);
        dlg_confirm(G.win, "Delete",
                    "Delete FILE.TXT? This cannot be undone and the text is "
                    "long enough to wrap.",
                    gotb, 0);
        sim_frames(1);
        sim_shot("g6_confirm");
        {
            window *f = wm_focused_window();
            char b[128];
            snprintf(b, 128, "focused=%s focus=%s", f ? f->title : "-",
                     f && f->focus ? ctl_text(f->focus) : "-");
            L(b);
        }
        sim_key(0x01);
        dlg_open_file(G.win, "Open", "/", NULL, got, 0);
        sim_frames(1);
        sim_shot("g7_open");
        sim_ext(0x50);
        sim_key(0x1C);
        sim_frames(1);
        sim_shot("g8_open2");
        sim_key(0x01);
        dlg_save_file(G.win, "Save", "/", "NOTE", "TXT", got, 0);
        sim_frames(1);
        sim_key(0x1C);
        sim_frames(1);
        return 0;
    }
    app_descriptor *d = os_find_app(name);
    if (!d) {
        sim_log("no such app");
        return 1;
    }
    os_launch_app(d);
    sim_frames(5);
    sim_shot(name);
    return 0;
}
int scen_apps1(const char *name);
int scen_apps2(const char *name);
int scen_storage(const char *name);
int scen_help(const char *name);
int scen_menu(const char *name);
int scen_fs(const char *name);
int scen_zip(const char *name);
int scen_setup(const char *name);
int scen_misc1(const char *name);
int scen_misc2(const char *name);
int scen2(const char *name) {
    if (scen_apps1(name) >= 0)
        return 0;
    if (scen_apps2(name) >= 0)
        return 0;
    if (scen_storage(name) >= 0)
        return 0;
    if (scen_help(name) >= 0)
        return 0;
    if (scen_menu(name) >= 0)
        return 0;
    if (scen_fs(name) >= 0)
        return 0;
    if (scen_zip(name) >= 0)
        return 0;
    if (scen_setup(name) >= 0)
        return 0;
    if (scen_misc1(name) >= 0)
        return 0;
    if (scen_misc2(name) >= 0)
        return 0;
    return -1;
}
void sim_cli_done(void) {
    extern int g_sim_pings, task_count;
    char m[64];
    snprintf(m, sizeof m, "pings answered %d, tasks left %d\n", g_sim_pings,
             task_count - 1);
    wr(1, m);
    extern void sim_exit0(void);
    sim_exit0();
}
