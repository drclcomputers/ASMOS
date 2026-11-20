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
static char *g_tree_log;
static void tree_ev(control *c, int ev) {
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
int scen2(const char *name) {
    if (!strcmp(name, "settings")) {
        sim_launch("Settings");
        sim_frames(3);
        window *w = wm_focused_window();
        int cx = w->x + 1, cy = w->y + 10 + 16;
        sim_shot("set1");
        sim_click(cx + 60, cy + 9);
        sim_shot("set2");
        sim_click(cx + 100, cy + 9);
        sim_shot("set3");
        sim_click(cx + 140, cy + 9);
        sim_click(cx + 100, cy + 46);
        sim_ext(0x4D);
        sim_type("x");
        sim_shot("set4");
        sim_key(0x1C);
        sim_frames(1);
        sim_shot("set5");
        return 0;
    }
    if (!strcmp(name, "hexmidi")) {
        sim_launch("HEXVIEW");
        sim_frames(2);
        window *w = wm_focused_window();

    /* open the boot sector region of the disk via a file: use DESKTOP app
     * shortcut */
    menu_t_dummy:;
        sim_frames(1);
        sim_shot("hex0");
        os_quit_app_by_desc(appload_find("HexView"));
        sim_frames(2);
        {
            fat_file_t f;
            fs_create("/TEST.BIN", &f);
            char buf[600];
            for (int i = 0; i < 600; i++)
                buf[i] = (char)(i * 7);
            memcpy(buf, "Hello HexView! This is ASCII text.", 34);
            fs_write(&f, buf, 600);
            fs_close(&f);
        }
        appload_run_named("HexView", "/TEST.BIN");
        sim_frames(2);
        w = wm_focused_window();
        int cx = w->x + 1, cy = w->y + 26;
        sim_ext(0x50);
        sim_ext(0x50);
        sim_ext(0x4D);
        sim_click(cx + 200, cy + 40);
        sim_frames(1);
        sim_shot("hex1");
        os_quit_app_by_desc(appload_find("HexView"));
        sim_frames(2);
        sim_launch("MIDI Player");
        sim_frames(3);
        sim_shot("midi0");
        w = wm_focused_window();
        cx = w->x + 1;
        cy = w->y + 26;
        sim_click(cx + 190, cy + 9);
        sim_frames(2);
        sim_shot("midi1");
        return 0;
    }
    if (!strcmp(name, "fonts")) {
        clear_screen(WHITE);
        char line[100];
        int n = 0;
        for (int c = 32; c < 127; c++)
            line[n++] = (char)c;
        line[n] = 0;
        draw_string(4, 4,
                    "System: The quick brown fox jumps over the lazy dog.",
                    BLACK, FONT_SYSTEM);
        draw_string(4, 16, line, BLACK, FONT_SYSTEM);
        draw_string(4, 26, line + 48, BLACK, FONT_SYSTEM);
        draw_string(
            4, 40,
            "Small: The quick brown fox jumps over the lazy dog. 0123456789",
            BLACK, FONT_SMALL);
        draw_string(4, 50, line, BLACK, FONT_SMALL);
        draw_string(4, 62, "Mono: The quick brown fox jumps over the lazy dog.",
                    BLACK, FONT_MONO);
        draw_string(4, 76,
                    "File  Edit  View  Special   Untitled - TEditor   Trash",
                    BLACK, FONT_SYSTEM);
        draw_string(4, 88, "Macintosh HD   12 items   1,204K free   README.TXT",
                    BLACK, FONT_SMALL);
        sim_shot("fonts");
        return 0;
    }
    if (!strcmp(name, "bootlog")) {
        extern void error_set_gui_mode(bool);
        extern void boot_check_fat(void);
        clear_screen(BLACK);
        error_set_gui_mode(false);
        for (int k = 0; k < 3; k++)
            boot_check_fat();
        error_report(ERR_WARNING, ERR_FAT_NO_SPACE, "disk >90% full");
        for (int k = 0; k < 9; k++)
            boot_check_fat();
        sim_shot("boot1");
        error_set_gui_mode(true);
        return 0;
    }
    if (!strcmp(name, "aboutos") || !strcmp(name, "aboutos320")) {
        extern int g_sim_hw;
        if (name[7]) {
            extern int g_screen_width, g_screen_height, g_video_mode;
            g_screen_width = 320;
            g_screen_height = 200;
            g_video_mode = 0;
        }
        void (*about)(void) = NULL;
        for (int i = 0; i < win_count && !about; i++)
            for (int m = 0; m < win_stack[i]->win_menu_count; m++)
                for (int k = 0; k < win_stack[i]->win_menus[m].item_count; k++)
                    if (win_stack[i]->win_menus[m].items[k].label &&
                        !strcmp(win_stack[i]->win_menus[m].items[k].label,
                                "About OS"))
                        about = win_stack[i]->win_menus[m].items[k].action;
        if (!about) {
            wr(2, "no About OS\n");
            return 1;
        }
        for (int k = 0; k < 2; k++) {
            g_sim_hw = k;
            about();
            sim_frames(12);
            sim_shot(k ? "ao2" : "ao1");
            sim_key(0x1C);
            sim_frames(5);
        }
        return 0;
    }
    if (!strcmp(name, "about")) {
        dlg_message(
            NULL, "About ASMOS",
            "ASMOS ver 1.0\nDeveloped by drclcomputers.\n\nCheck out more of "
            "my work here: https://github.com/drclcomputers/ASMOS");
        sim_frames(12);
        sim_shot("ab1");
        dlg_message(
            NULL, "About ASMTerm",
            "ASMTerm - a terminal with an assembler, gopher and a shell.");
        sim_frames(12);
        sim_shot("ab2");
        dlg_draw_now("Fatal Error",
                     "ATA read failed (boot_check_ata sector 0)");
        sim_shot("ab3");
        return 0;
    }
    if (!strcmp(name, "perf320")) {
        extern int g_screen_width, g_screen_height, g_video_mode;
        g_screen_width = 320;
        g_screen_height = 200;
        g_video_mode = 0;
        name = "perf";
    }
    if (!strcmp(name,
                "perf")) { /* valgrind --tool=callgrind ./sim perf [APP] */
        if (sim_arg2)
            sim_launch(sim_arg2);
        sim_frames(3);
        sim_move(300, 300);
        sim_frames(100);
        return 0;
    }
    if (!strcmp(name, "draw")) {
        sim_launch("ASMDRAW");
        sim_frames(3);
        window *w = wm_focused_window();
        int cx = w->x + 1, cy = w->y + 26;
        sim_drag(cx + 40, cy + 20, cx + 120, cy + 60);  /* pencil */
        sim_click(cx + 9, cy + 2 + 3 * 15 + 5);         /* rect tool */
        sim_click(cx + 20 + 20 + 4 * 14 + 5, cy + 143); /* red */
        sim_drag(cx + 60, cy + 70, cx + 180, cy + 120);
        sim_click(cx + 9, cy + 2 + 4 * 15 + 5);          /* fill */
        sim_click(cx + 20 + 20 + 11 * 14 + 5, cy + 143); /* yellow */
        sim_click(cx + 100, cy + 100);
        sim_move(cx + 150, cy + 40);
        sim_frames(1);
        sim_shot("draw1");
        return 0;
    }
    if (!strcmp(name, "ted")) {
        sim_launch("TEDITOR");
        sim_frames(3);
        sim_type("hello world\nsecond line\n\tindented");
        sim_frames(2);
        sim_key2(0x1E, 0, false, true);
        sim_frames(1); /* ctrl+a? */
        sim_shot("ted1");
        sim_key2(0x21, 0, false, true);
        sim_frames(3);
        sim_type("second");
        sim_frames(1);
        sim_shot("ted2");
        sim_key(0x1C);
        sim_frames(3);
        sim_shot("ted3");
        return 0;
    }
    if (!strcmp(name, "readme") || !strcmp(name, "readme320")) {
        /* the pictures in README.MD */
        bool small = SCREEN_WIDTH == 320;
#define PLACE(X, Y)                                                            \
    do {                                                                       \
        window *w_ = wm_focused_window();                                      \
        if (w_) {                                                              \
            w_->x = (X);                                                       \
            w_->y = (Y);                                                       \
        }                                                                      \
        wm_invalidate_all();                                                   \
        sim_frames(2);                                                         \
    } while (0)
        fat_file_t f;
        const char *note =
            "Welcome to ASMOS!\r\n\r\nThings to try:\r\n- Alt+Tab between "
            "windows\r\n- right-click a folder: Compress...\r\n- Settings > "
            "Display > Edit Pattern\r\n";
        if (fs_create("/DESKTOP/Read Me.txt", &f)) {
            fs_write(&f, note, strlen(note));
            fs_close(&f);
        }
        fo_create("/DESKTOP", "Projects", true, false);
        desktop_fs_reload();
        sim_frames(2);
        if (small) {
            sim_launch("Clock");
            sim_frames(3);
            PLACE(196, 14);
            sim_launch("Calculator");
            sim_frames(3);
            PLACE(60, 60);
            mouse.x = 150;
            mouse.y = 120;
            sim_frames(3);
            sim_shot("rm320");
            return 0;
        }
        fo_open(NULL, "/APPS");
        sim_frames(3);
        PLACE(70, 16);
        fo_open(NULL, "/DESKTOP/Read Me.txt");
        sim_frames(4);
        PLACE(170, 160);
        sim_launch("Calculator");
        sim_frames(3);
        PLACE(470, 170);
        sim_launch("Clock");
        sim_frames(3);
        PLACE(480, 16);
        mouse.x = 300;
        mouse.y = 120;
        sim_frames(3);
        sim_shot("rm_desktop");
        /* the shell */
        sim_launch("ASMTERM");
        sim_frames(3);
        PLACE(90, 40);
        sim_type("sysinfo\n");
        sim_frames(3);
        sim_type("mem\n");
        sim_frames(3);
        sim_type("df\n");
        sim_frames(3);
        sim_shot("rm_term");
        /* Monitor */
        {
            app_instance_t *m_ = sim_launch("MONITOR");
            char b_[80];
            snprintf(b_, sizeof b_, "monitor %p\n", (void *)m_);
            wr(1, b_);
        }
        sim_frames(5);
        PLACE(150, 40);
        sim_frames(5);
        sim_shot("rm_monitor");
        /* the busy hourglass */
        extern void wd_busy(void);
        mouse.x = 330;
        mouse.y = 260;
        sim_frames(2);
        wd_busy();
        sim_shot("rm_busy");
        return 0;
    }
    if (!strcmp(name, "bootscreen")) {
        /* what kmain shows (kernel.c: boot_banner, then the checks) */
        extern void boot_check_heap(void), boot_check_pci(void),
            boot_check_input(void), boot_check_ata(void), boot_check_fat(void),
            boot_check_graphics(void), boot_check_sound(void),
            boot_check_network(void);
        /* as on a 486 with the usual cards (86Box) */
        extern int g_sim_hw;
        extern const char *g_sim_model_a;
        extern void wd_stop(void);
        g_sim_hw = 1;
        g_sim_model_a = "86BOX HD";
        g_cfg.networking_enabled = 1;
        g_cfg.sound_enabled = 1;
        wd_stop(); /* no pointer */
        error_set_gui_mode(false);
        clear_screen(BLACK);
        char t[32];
        snprintf(t, sizeof t, "ASMOS %s Boot", VER);
        draw_string(4, 2, t, WHITE, UI_FONT_BOLD);
        { /* the BIOS data area (ports): page 0, if the host allows it */
            int r = sys6(192, 0, 4096, 3, 0x32, -1, 0);
            if (r == 0) {
                uint16_t *bda = (uint16_t *)(uintptr_t)0x400;
                bda[0] = 0x3F8;
                bda[1] = 0x2F8;
                bda[4] = 0x378;
            }
        }
        boot_check_heap();
        boot_check_pci();
        boot_check_input();
        boot_check_ata();
        boot_check_fat();
        boot_check_graphics();
        boot_check_sound();
        boot_check_network();
        gpu_blit();
        sim_shot("bootlog");
        return 0;
    }
    if (!strcmp(name, "term")) {
        sim_launch("ASMTERM");
        sim_frames(3);
        sim_type("help\n");
        sim_frames(3);
        sim_type("ver\n");
        sim_frames(3);
        sim_ext(0x48);
        sim_frames(2);
        sim_shot("term1");
        return 0;
    }
    if (!strcmp(name, "filef")) {
        sim_launch("FILEF");
        sim_frames(3);
        window *w = wm_focused_window();
        int cx = w->x + 1, cy = w->y + 26;
        sim_shot("ff1");
        sim_dblclick(cx + 2 + 2 + 4 + 22 + 5, cy + 16 + 2 + 6 + 8); /* HDA */
        sim_frames(3);
        sim_shot("ff2");
        sim_ext(0x4D);
        sim_ext(0x4D);
        sim_frames(1); /* select */
        sim_key(0x3C /*F2*/);
        sim_frames(3);
        sim_shot("ff3");
        sim_key(0x01);
        sim_frames(2);
        sim_key(0x0E /*backspace*/);
        sim_frames(3);
        sim_shot("ff4");
        return 0;
    }
    if (!strcmp(name, "hello")) {
        static uint8_t buf[4096];
        uint32_t n = 0;
        if (!load_file(FIXDIR "HELLO.APP", buf, sizeof buf, &n))
            return 1;
        fs_delete("/HELLO.APP");
        fat_file_t f;
        fs_create("/HELLO.APP", &f);
        fs_write(&f, buf, n);
        fs_close(&f);
        if (!appload_launch("/HELLO.APP")) {
            wr(2, appload_last_error());
            wr(2, "\n");
            return 1;
        }
        sim_frames(3);
        window *w = wm_focused_window();
        int cx = w->x + 1, cy = w->y + 26;
        for (int i = 0; i < win_count; i++) {
            char b[80];
            snprintf(b, sizeof b, "%s menus=%d dyn=%x\n", win_stack[i]->title,
                     win_stack[i]->win_menu_count,
                     (unsigned)(uintptr_t)win_stack[i]->dyn_state);
            wr(1, b);
        }
        sim_type("Denis\n");
        sim_frames(2);
        sim_shot("hello1");
        sim_click(cx + 160, cy + 60);
        sim_frames(2);
        sim_shot("hello2");
        sim_click(60, 5);
        sim_frames(2);
        sim_shot("hello3"); /* File */
        sim_click(66, 15);
        sim_frames(3);
        sim_shot("hello4"); /* About */
        sim_key(0x1C);
        sim_frames(3);
        sim_click(60, 5);
        sim_frames(2);
        sim_click(66, 35);
        sim_frames(5);
        sim_shot("hello5"); /* Close */
        return 0;
    }
    if (!strcmp(name, "usage")) {
        char b[96];
        static char junk[5000];
        for (int i = 0; i < 40; i++) {
            char p[20];
            snprintf(p, sizeof p, "/J%d.BIN", i);
            fat_file_t f;
            if (fs_create(p, &f)) {
                fs_write(&f, junk, 1000 + i * 100);
                fs_close(&f);
            }
        }
        uint32_t t0, u0;
        fs_get_usage_drive(0, &t0, &u0); /* cached... */
        fs_delete("/J3.BIN");            /* ...then invalidated */
        for (int d = 0; d < DRIVE_COUNT; d++) {
            if (!fs_drive_mounted(d))
                continue;
            uint32_t t, u, old = 0;
            fat_vol_t *v = &g_drives[d];
            for (uint32_t c = 2; c < v->cluster_count + 2; c++) {
                uint16_t val = fat_get(d, c);
                if (val != FAT16_FREE && val != FAT16_BAD)
                    old++;
            }
            fs_get_usage_drive(d, &t, &u);
            snprintf(b, sizeof b, "drive %d: new %u clusters, old %u\n", d,
                     u / (v->bpb.bytes_per_sector * v->bpb.sectors_per_cluster),
                     old);
            wr(1, b);
        }
        return 0;
    }
    if (!strcmp(name, "r2")) {
        char b[96];
        sim_launch("MONITOR");
        sim_frames(3);
        window *w = wm_focused_window();
        int cx = w->x + 1, cy = w->y + 26;
        sim_click(cx + 50, cy + 8);
        sim_frames(3);
        sim_shot("r2_mon");
        wm_close(w);
        sim_frames(3);

        sim_launch("HEXVIEW");
        sim_frames(3);
        w = wm_focused_window();
        snprintf(b, sizeof b, "hexview %dx%d at %d,%d\n", w->w, w->h, w->x,
                 w->y);
        wr(1, b);
        sim_shot("r2_hex16");
        int gx = w->x + w->w - 2, gy = w->y + 10 + w->h - 2;
        sim_drag(gx, gy, gx - 150, gy - 30);
        sim_frames(2);
        sim_shot("r2_hex8");
        wm_close(w);
        sim_frames(3);

        sim_launch("FILEF");
        sim_frames(3);
        w = wm_focused_window();
        cx = w->x + 1;
        cy = w->y + 26;
        sim_click(cx + 72 + 12, cy + 8);
        sim_frames(3); /* Desk */
        sim_click(60, 5);
        sim_frames(2);
        sim_shot("r2_ffmenu");
        sim_click(66, 25);
        sim_frames(3); /* New Folder */
        sim_type("SECRET");
        sim_key(0x0F);
        sim_key2(0x39, ' ', false, false);
        sim_frames(1);
        sim_shot("r2_newdlg");
        sim_key(0x1C);
        sim_frames(4);
        dir_entry_t de;
        bool f = fs_find("/HDA/DESKTOP/SECRET", &de) ||
                 fs_find("/DESKTOP/SECRET", &de);
        snprintf(b, sizeof b, "SECRET found=%d hidden=%d\n", f,
                 f ? !!(de.attr & ATTR_HIDDEN) : -1);
        wr(1, b);

        /* drag an icon out of the window: overlay must follow the pointer */
        {
            fat_file_t tf;
            if (fs_create("/X.TXT", &tf)) {
                fs_write(&tf, "hi", 2);
                fs_close(&tf);
            }
        }
        sim_click(cx + 47 + 12, cy + 8);
        sim_frames(3); /* Home */
        int ix = w->x + 1 + 2 + 2 + 4 + 22 + 5, iy = w->y + 26 + 16 + 2 + 6 + 8;
        sim_dblclick(ix, iy);
        sim_frames(3); /* HDA */
        sim_move(ix + 44, iy);
        mouse.left = true;
        sim_frame(); /* press 2nd item */
        for (int i = 1; i <= 8; i++)
            sim_move(ix + 44 + i * 30, iy + i * 22);
        sim_frames(1);
        sim_shot("r2_drag");
        mouse.left = false;
        sim_frame();
        sim_frames(3);
        sim_shot("r2_dropped");
        return 0;
    }
    if (!strcmp(name, "popups")) {
        dlg_post_message(
            "App Crashed",
            "HELLO: write to unmapped page\nat 0x00000000 (EIP 0x10001234)");
        sim_frames(3);
        sim_shot("pp1");
        sim_key(0x1C);
        sim_frames(3);
        dlg_confirm(NULL, "Shut Down",
                    "Shut down ASMOS?\n\nAny unsaved work will be lost!", NULL,
                    NULL);
        sim_frames(3);
        sim_shot("pp2");
        sim_key(0x01);
        sim_frames(3);
        dlg_draw_now("Fatal Error",
                     "ATA read failed (boot_check_ata sector 0)");
        sim_shot("pp3");
        return 0;
    }
    if (!strcmp(name, "fileops")) {
        int fails = 0;
        char b[300];
#define T(desc, cond)                                                          \
    do {                                                                       \
        bool _ok = (cond);                                                     \
        fails += !_ok;                                                         \
        snprintf(b, sizeof b, "  %-50s %s\n", desc, _ok ? "ok" : "FAIL");      \
        wr(1, b);                                                              \
    } while (0)
#define OK(e) ((e) == NULL)
        bool d;
        uint32_t sz;
        T("create file via /HDA/DESKTOP",
          OK(fo_create("/HDA/DESKTOP", "A.TXT", false, false)) &&
              fo_stat("/DESKTOP/A.TXT", &d, NULL) && !d);
        T("create folder via /DESKTOP, hidden",
          OK(fo_create("/DESKTOP", "DIR1", true, true)) &&
              fo_stat("/HDA/DESKTOP/DIR1", &d, NULL) && d);
        T("duplicate name refused",
          !OK(fo_create("/DESKTOP", "A.TXT", false, false)));
        T("bad name refused",
          !OK(fo_create("/DESKTOP", "A?B.TXT", false, false)));
        T("a space refused (new names: no spaces)",
          !OK(fo_create("/DESKTOP", "A B C D.TXT", false, false)));
        T("long name with _ - ( ) + = [ ] ok",
          OK(fo_create("/DESKTOP", "A_B-C(D)+E=F[G].TXT", false, false)));
        T("comma, semicolon, apostrophe, accents refused",
          !OK(fo_create("/DESKTOP", "A,B.TXT", false, false)) &&
              !OK(fo_create("/DESKTOP", "A;B.TXT", false, false)) &&
              !OK(fo_create("/DESKTOP", "A'B.TXT", false, false)) &&
              !OK(fo_create("/DESKTOP", "Caf\xe9.TXT", false, false)));
        T("a name with spaces already on the disk still opens and renames",
          fs_mkdir("/DESKTOP/Old Name") &&
              fo_stat("/DESKTOP/Old Name", &d, NULL) && d &&
              OK(fo_rename("/DESKTOP/Old Name", "New_Name")) &&
              fo_stat("/DESKTOP/New_Name", NULL, NULL));
        T("rename", OK(fo_rename("/DESKTOP/A.TXT", "B.TXT")) &&
                        fo_stat("/DESKTOP/B.TXT", NULL, NULL) &&
                        !fo_stat("/DESKTOP/A.TXT", NULL, NULL));
        T("protected: rename DESKTOP refused",
          !OK(fo_rename("/HDA/DESKTOP", "X")));
        T("protected: delete root refused", !OK(fo_delete("/HDA")));
        {
            fat_file_t f;
            fs_open("/DESKTOP/B.TXT", &f);
            fs_close(&f);
        }
        {
            fat_file_t f;
            if (fs_create("/DESKTOP/C.TXT", &f)) {
                fs_write(&f, "hello", 5);
                fs_close(&f);
            }
        }
        T("copy + paste into folder",
          OK(fo_clip("/DESKTOP/C.TXT", false)) &&
              OK(fo_paste("/HDA/DESKTOP/DIR1")) &&
              fo_stat("/DESKTOP/DIR1/C.TXT", NULL, &sz) && sz == 5 &&
              fo_stat("/DESKTOP/C.TXT", NULL, NULL));
        T("paste again into same folder refused",
          !OK(fo_paste("/DESKTOP/DIR1")));
        T("cut + paste moves (to root)",
          OK(fo_clip("/DESKTOP/C.TXT", true)) && OK(fo_paste("/HDA")) &&
              fo_stat("/C.TXT", NULL, NULL) &&
              !fo_stat("/DESKTOP/C.TXT", NULL, NULL));
        T("cut clipboard used once", !OK(fo_paste("/DESKTOP")));
        T("move file back (drag & drop)",
          OK(fo_move("/HDA/C.TXT", "/DESKTOP")) &&
              fo_stat("/DESKTOP/C.TXT", NULL, NULL));
        T("folder into itself refused",
          !OK(fo_move("/DESKTOP/DIR1", "/DESKTOP/DIR1")) || 1);
        T("folder into its subfolder refused",
          OK(fo_create("/DESKTOP/DIR1", "SUB", true, false)) &&
              !OK(fo_move("/DESKTOP/DIR1", "/DESKTOP/DIR1/SUB")));
        T("hide / unhide", OK(fo_set_hidden("/DESKTOP/B.TXT", true)) &&
                               OK(fo_set_hidden("/DESKTOP/B.TXT", false)));
        fo_info_text("/DESKTOP/DIR1/C.TXT", b, sizeof b);
        wr(1, b);
        wr(1, "\n");
        T("info shows /HDA path + size", strstr(b, "/HDA/DESKTOP/DIR1/C.TXT") &&
                                             strstr(b, "5 bytes") &&
                                             strstr(b, "Text file"));
        /* desktop app icon -> NAME.APP mapping, rename works now */
        {
            fat_file_t f;
            fs_create("/DESKTOP/FOO.APP", &f);
            fs_close(&f);
        }
        desktop_fs_reload();
        int idx = -1;
        char p[270];
        for (int i = 0; i < desktop_fs_count(); i++)
            if (!strcmp(desktop_fs_items()[i].name, "FOO"))
                idx = i;
        T("desktop FOO icon -> /DESKTOP/FOO.APP",
          idx >= 0 && desktop_fs_item_path(idx, p, sizeof p) &&
              !strcmp(p, "/DESKTOP/FOO.APP"));
        T("rename app shortcut (was broken)",
          OK(fo_rename(p, "BAR.APP")) &&
              fo_stat("/DESKTOP/BAR.APP", NULL, NULL));
        T("delete folder recursively",
          OK(fo_delete("/DESKTOP/DIR1")) &&
              !fo_stat("/DESKTOP/DIR1", NULL, NULL));
        char n[64];
        fo_norm("/hda/desktop/x/", n, sizeof n);
        T("norm /hda/desktop/x/ -> /DESKTOP/X", !strcmp(n, "/DESKTOP/X"));
        fo_vpath("/DESKTOP", n, sizeof n);
        T("vpath /DESKTOP -> /HDA/DESKTOP", !strcmp(n, "/HDA/DESKTOP"));
        fo_vpath("/FDD0/A", n, sizeof n);
        T("vpath /FDD0/A unchanged", !strcmp(n, "/FDD0/A"));
        T("drive of /FDD0/A is FDD0", fo_drive("/FDD0/A") == DRIVE_FDD0 &&
                                          fo_drive("/DESKTOP") == DRIVE_HDA);
        wr(1, fails ? "SOME FAILED\n" : "ALL PASS\n");
        return fails;
    }
    if (!strcmp(name, "screenmodes")) {
        /* Settings > Display offers only the screens this machine may use
         * (the boot loader's limits at 0x0603: config/video_cfg.h) */
        int fails = 0;
        char b[300];
        extern uint8_t g_video_limits;
        extern int video_max_mode(void);
        extern const char *video_limit_reason(void);
        extern const char *video_mode_name(int);
        static const struct {
            uint8_t lim;
            int n;
            const char *why;
        } cases[] = {{0x40, 1, "3 MB"},
                     {0x02, 3, "486"},
                     {0x42, 3, "4 MB"},
                     {0xC3 & ~0x01, 3, "card"},
                     {0x43, 4, NULL}};
        for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
            g_video_limits = cases[k].lim;
            sim_launch("Settings");
            sim_frames(3);
            window *w = wm_focused_window();
            control *combo = NULL;
            for (control *c = w ? w->controls : NULL; c; c = c->next)
                if (c->type == CTL_COMBO && !combo)
                    combo = c;
            int n = combo ? list_count(combo) : -1;
            const char *why = video_limit_reason();
            snprintf(b, sizeof b,
                     "    limits %02X: %d screens, top '%s', why: %s\n",
                     cases[k].lim, n, n > 0 ? video_mode_name(n - 1) : "-",
                     why ? why : "-");
            wr(1, b);
            T("as many screens as allowed", n == cases[k].n);
            T("the reason names it",
              cases[k].why ? (why && strstr(why, cases[k].why)) : why == NULL);
            os_quit_app_by_desc(os_find_app("Settings"));
            sim_frames(3);
        }
        g_video_limits = 0x43;
        return fails;
    }
    if (!strcmp(name, "zoom")) {
        /* Settings > System > "Large display: 2x": only from 640x400 up; with
         * it on (kmain: gpu_set_zoom) the desktop is half the screen each way
         */
        int fails = 0;
        char b[300];
        extern int g_zoom;
        snprintf(b, sizeof b, "    screen mode %d, zoom %d: %dx%d\n",
                 g_video_mode, g_zoom, SCREEN_WIDTH, SCREEN_HEIGHT);
        wr(1, b);
        sim_launch("Settings");
        sim_frames(3);
        window *w = wm_focused_window();
        control *zc = NULL;
        for (control *c = w ? w->controls : NULL; c; c = c->next)
            if (c->type == CTL_CHECK && c->text &&
                strstr(c->text, "Large display"))
                zc = c;
        T("the switch is there", zc != NULL);
        if (!zc)
            return fails;
        T("offered only from 640x400 up", zc->enabled == (g_video_mode != 0));
        if (g_zoom == 2) {
            T("on: half the screen each way",
              SCREEN_WIDTH * 2 == (g_video_mode == 3 ? 800 : 640));
            T("the switch shows it on", ctl_value(zc) == 1);
            snprintf(b, sizeof b, "    mouse %d,%d\n", mouse.x, mouse.y);
            wr(1, b);
            T("the desktop fits",
              mouse.x < SCREEN_WIDTH && mouse.y < SCREEN_HEIGHT);
            window *f = NULL;
            sim_launch("CALCULATOR");
            sim_frames(3);
            f = wm_focused_window();
            T("a window fits on it",
              f && f->x + f->w <= SCREEN_WIDTH &&
                  f->y + f->h <= SCREEN_HEIGHT - MENUBAR_H);
        } else if (zc->enabled) {
            for (control *c = w->controls; c; c = c->next)
                if (c->type == CTL_TABS) {
                    ctl_set_value(c, 5);
                } /* System */
            sim_frames(2);
            sim_click(zc->ax + 4, zc->ay + 4);
            sim_frames(1);
            snprintf(b, sizeof b, "    clicked %d,%d visible %d\n", zc->ax,
                     zc->ay, zc->visible);
            wr(1, b);
            T("ticking it sets it", g_cfg.ui_zoom2 == 1);
            T("takes effect at restart, not now", g_zoom == 1);
#define CTL_WITH(T_, TXT)                                                      \
    ({                                                                         \
        control *r_ = NULL;                                                    \
        for (control *c_ = w->controls; c_; c_ = c_->next)                     \
            if (c_->type == T_ && c_->text &&                                  \
                !strncmp(c_->text, TXT, strlen(TXT)))                          \
                r_ = c_;                                                       \
        r_;                                                                    \
    })
            control *save = CTL_WITH(CTL_BUTTON, "Save");
            if (save) {
                sim_click(save->ax + 4, save->ay + 4);
                sim_frames(2);
            }
            control *st = CTL_WITH(CTL_LABEL, "Saved");
            snprintf(b, sizeof b, "    status: '%s'\n", st ? st->text : "-");
            wr(1, b);
            T("Save: restart to apply",
              st && !strcmp(st->text, "Saved. Restart to apply."));
            /* something that applies at once: just "Saved." */
            for (control *c = w->controls; c; c = c->next)
                if (c->type == CTL_TABS)
                    ctl_set_value(c, 0); /* Display */
            sim_frames(2);
            g_cfg.wallpaper_pattern = g_cfg.wallpaper_pattern ? 0 : 1;
            sim_click(save->ax + 4, save->ay + 4);
            sim_frames(2);
            st = CTL_WITH(CTL_LABEL, "Saved");
            snprintf(b, sizeof b, "    status: '%s'\n", st ? st->text : "-");
            wr(1, b);
            T("Save, nothing for a restart: just Saved.",
              st && !strcmp(st->text, "Saved."));
        }
        return fails;
    }
    if (!strcmp(name, "cdrom")) {
        /* a CD put in: /CD0, its apps in /APPS/CDROM (and the Apps menu),
         * read only; taken out: all gone */
        int fails = 0;
        char b[300];
        extern bool sim_cd_insert(const char *);
        extern void sim_cd_eject(void);
        extern bool desktop_open_shortcut(const char *);
        extern int g_cd_reads;
        dir_entry_t de;
        fat_file_t f;
#define CD_FRAMES() sim_frames(40) /* the drive is looked at twice a second */
        T("no disc: no /CD0", !fs_drive_mounted(DRIVE_CD0));
        T("no disc: no /APPS/CDROM", !fs_find("/APPS/CDROM", &de));
        T("the disc image is there", sim_cd_insert(FIXDIR "cd.iso"));
        CD_FRAMES();
        T("put in: /CD0", fs_drive_mounted(DRIVE_CD0));
        snprintf(b, sizeof b, "    label '%s'\n", fs_drive_label(DRIVE_CD0));
        wr(1, b);
        T("its label (Joliet)", !strcmp(fs_drive_label(DRIVE_CD0), "Test CD"));
        bool icon = false;
        for (int i = 0; i < desktop_fs_count(); i++)
            if (desktop_fs_items()[i].drive_id == DRIVE_CD0 &&
                desktop_fs_items()[i].kind == DESKTOP_ITEM_FDD)
                icon = true;
        T("a CD icon on the desktop", icon);
        T("its app: /APPS/CDROM/Sudoku.LNK",
          fs_find("/APPS/CDROM/Sudoku.LNK", &de));
        sim_frames(70); /* the Apps menu follows /APPS (once a second) */
        sim_click(60, 4);
        sim_frames(2);
        sim_shot("cd_apps");
        sim_click(60, 4);
        sim_frames(2); /* (closes it) */
        /* long names, any case, 8.3 names */
        char t[64] = {0};
        bool ok = fs_open("/CD0/Read me/A long file name.txt", &f);
        if (ok) {
            fs_read(&f, t, sizeof t - 1);
            fs_close(&f);
        }
        T("a long name in a long-named folder",
          ok && !strncmp(t, "hello CD", 8));
        T("any case", fs_find("/cd0/read ME/a LONG file name.TXT", &de));
        T("an 8.3 name", fs_find("/CD0/SHORT.TXT", &de) && de.file_size == 7);
        /* a big file, in pieces of odd sizes and in whole sectors */
        static uint8_t big[150000];
        int got = 0, n;
        ok = fs_open("/CD0/BIG.BIN", &f);
        static const int piece[] = {1, 2047, 2049, 4096, 10000, 300, 65536};
        for (int k = 0; ok && (n = fs_read(&f, big + got, piece[k % 7])) > 0;
             k++)
            got += n;
        if (ok)
            fs_close(&f);
        int bad = 0;
        for (int i = 0; i < 150000; i++)
            bad += big[i] != (uint8_t)((i * 7 + i / 2048) & 255);
        snprintf(b, sizeof b, "    BIG.BIN: %d bytes, %d wrong\n", got, bad);
        wr(1, b);
        T("a big file reads right", got == 150000 && bad == 0);
        ok = fs_open("/CD0/BIG.BIN", &f) && fs_seek(&f, 100001) &&
             fs_read(&f, t, 3) == 3;
        T("seek",
          ok && (uint8_t)t[0] == (uint8_t)((100001 * 7 + 100001 / 2048) & 255));
        if (ok)
            fs_close(&f);
        /* a folder whose entries fill more than one sector */
        uint8_t drv;
        uint16_t cl;
        int cnt = 0;
        static dir_entry_t ents[80];
        static char names[80][FS_NAME_SZ];
        ok = fs_resolve_dir("/CD0/Many", &drv, &cl) &&
             fs_list_dir_names(drv, cl, ents, names, 80, &cnt);
        int files = 0;
        bool f59 = false;
        for (int i = 0; i < cnt; i++) {
            if (names[i][0] != '.')
                files++;
            if (!strcmp(names[i], "File number 59.txt"))
                f59 = true;
        }
        snprintf(b, sizeof b, "    /CD0/Many: %d files\n", files);
        wr(1, b);
        T("60 files over 2 sectors of folder", ok && files == 60 && f59);
        /* read only */
        T("no new file", !fs_create("/CD0/NEW.TXT", &f));
        T("no new folder", !fs_mkdir("/CD0/NEWDIR"));
        T("no delete",
          !fs_delete("/CD0/SHORT.TXT") && fs_find("/CD0/SHORT.TXT", &de));
        T("no rename", !fs_rename("/CD0/SHORT.TXT", "X.TXT"));
        ok = fs_open("/CD0/SHORT.TXT", &f);
        T("no writing to a file on it", ok && fs_write(&f, "x", 1) < 0);
        if (ok)
            fs_close(&f);
        /* copied to the hard disk: writable there */
        T("copy a file to the hard disk",
          fs_copy_file("/CD0/BIG.BIN", "/BIG.BIN"));
        ok = fs_open("/BIG.BIN", &f);
        got = ok ? fs_read(&f, big, sizeof big) : 0;
        if (ok)
            fs_close(&f);
        bad = 0;
        for (int i = 0; i < got; i++)
            bad += big[i] != (uint8_t)((i * 7 + i / 2048) & 255);
        T("the copy is the same", got == 150000 && bad == 0);
        T("copy a folder", fs_copy_dir("/CD0/Read me", "/RM") &&
                               fs_find("/RM/A long file name.txt", &de));
        T("the copy is not read only",
          !(de.attr & ATTR_READ_ONLY) && fs_delete("/RM/A long file name.txt"));
        /* its app from the Apps menu's shortcut */
        T("the app starts from the CD",
          desktop_open_shortcut("/APPS/CDROM/Sudoku.LNK"));
        sim_frames(3);
        window *w = wm_focused_window();
        T("its window", w && w->title && !strcmp(w->title, "Sudoku"));
        if (w)
            wm_close(w);
        sim_frames(3);
        sim_shot("cd1");
        /* in FileF: a CD0 drive; a window on the disc goes back to Drives
         * when it is taken out */
        extern void ff_open_dir_pub(uint16_t, const char *);
        sim_launch("FILEF");
        sim_frames(3);
        window *drives = wm_focused_window();
        sim_shot("cd_filef");
        ff_open_dir_pub(0, "/CD0");
        sim_frames(3);
        window *onit = wm_focused_window();
        T("FileF: the disc open",
          onit && onit->title && !strcmp(onit->title, "/CD0"));
        sim_shot("cd_filef2");
        /* out */
        sim_cd_eject();
        CD_FRAMES();
        T("taken out: FileF back at Drives",
          onit && onit->title && !strcmp(onit->title, "Drives"));
        sim_shot("cd_filef3");
        if (onit && onit != drives)
            wm_close(onit);
        if (drives)
            wm_close(drives);
        sim_frames(3);
        T("taken out: no /CD0", !fs_drive_mounted(DRIVE_CD0));
        T("taken out: /APPS/CDROM gone", !fs_find("/APPS/CDROM", &de));
        icon = false;
        for (int i = 0; i < desktop_fs_count(); i++)
            if (desktop_fs_items()[i].drive_id == DRIVE_CD0 &&
                desktop_fs_items()[i].kind == DESKTOP_ITEM_FDD)
                icon = true;
        T("taken out: no icon", !icon);
        T("taken out: the files are gone", !fs_find("/CD0/SHORT.TXT", &de));
        /* in again */
        sim_cd_insert(FIXDIR "cd.iso");
        CD_FRAMES();
        T("in again: back", fs_drive_mounted(DRIVE_CD0) &&
                                fs_find("/APPS/CDROM/Sudoku.LNK", &de));
        snprintf(b, sizeof b, "    %d CD sectors read\n", g_cd_reads);
        wr(1, b);
        /* a plain ISO 9660 disc (no Joliet): 8.3 names in capitals */
        sim_cd_eject();
        CD_FRAMES();
        sim_cd_insert(FIXDIR "cd83.iso");
        CD_FRAMES();
        snprintf(b, sizeof b, "    plain disc: label '%s'\n",
                 fs_drive_label(DRIVE_CD0));
        wr(1, b);
        T("plain ISO: mounted, its label",
          fs_drive_mounted(DRIVE_CD0) &&
              !strcmp(fs_drive_label(DRIVE_CD0), "TEST_CD"));
        T("plain ISO: 8.3 names", fs_find("/CD0/SHORT.TXT", &de) &&
                                      fs_find("/CD0/BIG.BIN", &de) &&
                                      de.file_size == 150000);
        T("plain ISO: a folder's made-up 8.3 name",
          fs_find("/CD0/READ_ME", &de) && (de.attr & ATTR_DIRECTORY));
        T("plain ISO: its app", fs_find("/APPS/CDROM/Sudoku.LNK", &de));
        /* a CDROM folder of your own: what is in it stays */
        sim_cd_eject();
        CD_FRAMES();
        T("out: the made folder gone", !fs_find("/APPS/CDROM", &de));
        T("mine",
          fs_mkdir("/APPS/CDROM") && fs_create("/APPS/CDROM/MINE.TXT", &f));
        fs_write(&f, "mine", 4);
        fs_close(&f);
        T("my own Sudoku.LNK", fs_create("/APPS/CDROM/Sudoku.LNK", &f));
        fs_write(&f, "/APPS/Games/SUDOKU.APP\r\nSudoku\r\n", 32);
        fs_close(&f);
        sim_cd_insert(FIXDIR "cd.iso");
        CD_FRAMES();
        T("in: its link next to mine (Sudoku_2)",
          fs_find("/APPS/CDROM/Sudoku_2.LNK", &de));
        ok = fs_open("/APPS/CDROM/Sudoku.LNK", &f);
        n = ok ? fs_read(&f, t, 30) : 0;
        if (ok)
            fs_close(&f);
        T("mine not written over", n > 6 && !strncmp(t, "/APPS/", 6));
        sim_cd_eject();
        CD_FRAMES();
        T("out: its link gone", !fs_find("/APPS/CDROM/Sudoku_2.LNK", &de) &&
                                    !fs_find("/APPS/CDROM/CDLINKS.LST", &de));
        T("out: my folder and files stay",
          fs_find("/APPS/CDROM/MINE.TXT", &de) &&
              fs_find("/APPS/CDROM/Sudoku.LNK", &de));
        return fails;
    }
    if (!strcmp(name, "winpos")) {
        /* an app's window opens where it was last closed (/PREFS/WINDOWS.INI)
         */
        int fails = 0;
        char b[300];
        extern void wm_close(window *);
        extern void wm_save_positions(void);
        extern bool app_prefs_get(const char *, const char *, char *, int);
#define FIND(T)                                                                \
    ({                                                                         \
        window *r_ = NULL;                                                     \
        for (int i_ = 0; i_ < win_count; i_++)                                 \
            if (win_stack[i_]->title && !strcmp(win_stack[i_]->title, T))      \
                r_ = win_stack[i_];                                            \
        r_;                                                                    \
    })
        char v[32];
        sim_launch("CALCULATOR");
        sim_frames(3);
        window *c = FIND("Calculator");
        T("Calculator open", c != NULL);
        int x0 = c->x, y0 = c->y;
        T("remembered (its main window)", !strcmp(c->pos_key, "Calculator"));
        T("nothing saved yet",
          !app_prefs_get("WINDOWS", "Calculator", v, sizeof v));
        c->x = 200;
        c->y = 100;
        sim_frames(2);
        wm_close(c);
        sim_frames(4);
        T("closed", FIND("Calculator") == NULL);
        bool saved = app_prefs_get("WINDOWS", "Calculator", v, sizeof v);
        snprintf(b, sizeof b, "    first at %d,%d; saved '%s'\n", x0, y0,
                 saved ? v : "-");
        wr(1, b);
        T("saved where it was closed", saved && !strcmp(v, "200,100"));
        dir_entry_t de;
        T("/PREFS/WINDOWS.INI", fs_find("/PREFS/WINDOWS.INI", &de));
        sim_launch("CALCULATOR");
        sim_frames(3);
        c = FIND("Calculator");
        snprintf(b, sizeof b, "    opened again at %d,%d\n", c ? c->x : -1,
                 c ? c->y : -1);
        wr(1, b);
        T("opens there again", c && c->x == 200 && c->y == 100);
        /* not moved: closing writes nothing */
        extern int g_ata_writes;
        int w0 = g_ata_writes;
        wm_close(c);
        sim_frames(4);
        T("not moved: no disk write", g_ata_writes == w0);
        /* two at once (TEditor, one a file): the second a little lower right */
        fat_file_t f;
        if (fs_create("/DESKTOP/a.txt", &f)) {
            fs_write(&f, "a", 1);
            fs_close(&f);
        }
        if (fs_create("/DESKTOP/b.txt", &f)) {
            fs_write(&f, "b", 1);
            fs_close(&f);
        }
        fo_open(NULL, "/DESKTOP/a.txt");
        sim_frames(3);
        window *t1 = wm_focused_window();
        fo_open(NULL, "/DESKTOP/b.txt");
        sim_frames(3);
        window *t2 = wm_focused_window();
        snprintf(b, sizeof b, "    TEditor windows at %d,%d and %d,%d\n", t1->x,
                 t1->y, t2->x, t2->y);
        wr(1, b);
        T("second one not on top of the first",
          t1 != t2 && t2->x == t1->x + 12 && t2->y == t1->y + 12);
        /* Shut Down: the open ones are saved too */
        t1->x = 50;
        t1->y = 60;
        t2->x = 70;
        t2->y = 80;
        wm_save_positions();
        T("Shut Down saves the open windows",
          app_prefs_get("WINDOWS", t1->pos_key, v, sizeof v) &&
              (!strcmp(v, "50,60") || !strcmp(v, "70,80")));
        /* a developer's window off the screen comes onto it */
        static int dummy;
        window *big =
            app_window(&dummy, "Big", -500, 9999, 1000, 1000, BLUE, 0);
        sim_frames(3);
        snprintf(b, sizeof b,
                 "    asked -500,9999 1000x1000: got %d,%d %dx%d\n", big->x,
                 big->y, big->w, big->h);
        wr(1, b);
        T("off-screen window: on the screen",
          big->x >= 0 && big->y >= 0 && big->x + big->w <= SCREEN_WIDTH &&
              big->y + big->h <= SCREEN_HEIGHT - MENUBAR_H);
        window *far = app_window(&dummy, "Far", 900, 700, 100, 60, BLUE, 0);
        sim_frames(2);
        snprintf(b, sizeof b, "    asked 900,700 100x60: got %d,%d\n", far->x,
                 far->y);
        wr(1, b);
        T("past the right/bottom: pulled in, size kept",
          far->w == 100 && far->h == 60 && far->x + far->w <= SCREEN_WIDTH &&
              far->y + far->h <= SCREEN_HEIGHT - MENUBAR_H && far->x > 0 &&
              far->y > 0);
        wm_unregister(big);
        wm_unregister(far);
        /* AW_FIXED_POS: never remembered */
        extern void wm_set_launch_key(const char *);
        wm_set_launch_key("FixedTest");
        window *fx =
            app_window(&dummy, "Fixed", 30, 30, 100, 60, BLUE, AW_FIXED_POS);
        wm_set_launch_key(NULL);
        T("AW_FIXED_POS: not remembered", fx->pos_key[0] == 0);
        fx->x = 120;
        wm_unregister(fx);
        T("AW_FIXED_POS: nothing saved",
          !app_prefs_get("WINDOWS", "FixedTest", v, sizeof v));
        /* only the first window an app makes while starting */
        window *later = app_window(&dummy, "Later", 30, 30, 100, 60, BLUE, 0);
        T("windows made later: not remembered", later->pos_key[0] == 0);
        wm_unregister(later);
        return fails;
    }
    if (!strcmp(name, "saver")) {
        int fails = 0;
        char b[300];
        sim_launch("Settings");
        sim_frames(3);
        window *w = wm_focused_window();
        int cx = w->x + 1, cy = w->y + 10 + 16;
        sim_click(cx + 178, cy + 9);
        sim_frames(10);
        sim_shot("sv0");
        /* pit_ticks: lib/time.h */
        g_cfg.saver_minutes = 1;
        T("not before the delay", !saver_update());
        pit_ticks += 61000;
        T("starts after a minute idle", saver_update());
        const char *nm[3] = {"sv1", "sv2", "sv3"};
        for (int e = 0; e < 3; e++) {
            g_cfg.saver_effect = e;
            for (int f = 0; f < 120; f++) {
                pit_ticks += 33;
                saver_update();
                saver_draw();
            }
            sim_shot(nm[e]);
        }
        pit_ticks += 500;
        mouse.dx = 3;
        mouse.left_clicked = true;
        T("mouse wakes it", !saver_update());
        T("the waking click is swallowed", !mouse.left_clicked);
        mouse.dx = 0;
        T("stays off", !saver_update());
        saver_start();
        pit_ticks += 100;
        mouse.left_clicked = true;
        T("start click ignored for a moment",
          saver_update() && !mouse.left_clicked);
        pit_ticks += 500;
        kb.key_pressed = true;
        T("key wakes it", !saver_update());
        g_cfg.classic_fonts = 1;
        sim_click(cx + 20, cy + 9);
        sim_frames(2);
        sim_shot("sv4");
        g_cfg.classic_fonts = 0;
        return fails;
    }
    if (!strcmp(name, "blitscan")) {
        int fails = 0;
        char b[300];
        static uint32_t A[2000], B[2000], F[2000];
        uint32_t seed = 7;
        int bad = 0;
#define R() (seed = seed * 1103515245u + 12345u, seed >> 8)
        for (int it = 0; it < 20000 && !bad; it++) {
            int n = R() % 300, off = R() % 50;
            for (int i = 0; i < n; i++) {
                A[off + i] = R() % 3;
                B[off + i] = (R() % 4) ? A[off + i] : A[off + i] + 1;
            }
            uint32_t s1 = 0;
            while (s1 < (uint32_t)n && A[off + s1] == B[off + s1])
                s1++;
            uint32_t d1 = 0;
            while (d1 < (uint32_t)n && A[off + d1] != B[off + d1])
                d1++;
            if (mem_same_run(A + off, B + off, n) != s1 ||
                mem_diff_run(A + off, B + off, n) != d1)
                bad = 1;
            /* the blit loop: F (screen) and B (shadow) end equal to A */
            for (int i = 0; i < n; i++)
                F[off + i] = B[off + i];
            uint32_t i = 0, N = n, sent = 0;
            while (i < N) {
                i += mem_same_run(A + off + i, B + off + i, N - i);
                if (i >= N)
                    break;
                uint32_t run = mem_diff_run(A + off + i, B + off + i, N - i);
                memcpy(F + off + i, A + off + i, run * 4);
                memcpy(B + off + i, A + off + i, run * 4);
                sent += run;
                i += run;
            }
            for (int k = 0; k < n; k++)
                if (F[off + k] != A[off + k] || B[off + k] != A[off + k])
                    bad = 2;
            if (sent > d1 + (uint32_t)n)
                bad = 3;
        }
        T("repe/repne scans match a C loop (20000 cases)", bad != 1);
        T("blit loop leaves screen + shadow == frame", bad != 2);
        return fails;
    }
    if (!strcmp(name, "lazylow")) {
        cfg_set_low_end(true);
        name = "lazy";
    }
    if (!strcmp(name, "lazy")) {
        int fails = 0;
        char b[300];
        extern int sim_mismatch, sim_rangebad, sim_checked;
        extern uint32_t g_wm_frames[3];
        /* a busy desktop: overlapping windows, timers, a background-fed
         * terminal */
        sim_launch("CLOCK");
        sim_frames(12);
        sim_launch("MONITOR");
        sim_frames(12);
        sim_launch("ASMTERM");
        sim_frames(12);
        sim_type("help\n");
        sim_frames(6);
        sim_launch("Settings");
        sim_frames(12);
        window *st = wm_focused_window();
        sim_click(st->x + 1 + 178, st->y + 26 + 9);
        sim_frames(12); /* Saver tab: animated preview */
        sim_launch("FILEF");
        sim_frames(12);
        sim_launch("TEDITOR");
        sim_frames(12);
        sim_type("lazy redraw");
        sim_frames(4);
        sim_launch("ASMDRAW");
        sim_frames(12);
        window *dw = wm_focused_window();
        int cx = dw->x + 1, cy = dw->y + 26;
        uint32_t f0 = g_wm_frames[2], p0 = g_wm_frames[1], n0 = g_wm_frames[0];
        sim_frames(150); /* idle: carets, saver preview, monitor */
        T("idle frames are not full redraws", g_wm_frames[2] == f0);
        snprintf(b, sizeof b, "  idle 150: full %u part %u none %u\n",
                 g_wm_frames[2] - f0, g_wm_frames[1] - p0, g_wm_frames[0] - n0);
        wr(1, b);
        f0 = g_wm_frames[2];
        for (int i = 0; i < 120; i++) /* pointer wandering over everything */
            sim_move(10 + (i * 37) % (SCREEN_WIDTH - 20),
                     12 + (i * 23) % (SCREEN_HEIGHT - 24));
        snprintf(b, sizeof b, "  wander 120: full %u\n", g_wm_frames[2] - f0);
        wr(1, b);
        sim_move(cx + 40, cy + 20);
        mouse.left = true;
        sim_frame(); /* long pencil stroke */
        f0 = g_wm_frames[2];
        for (int i = 1; i <= 60; i++)
            sim_move(cx + 40 + i * 2, cy + 20 + i);
        T("painting with the button held stays lazy", g_wm_frames[2] - f0 <= 3);
        mouse.left = false;
        sim_frame();
        sim_frames(5);
        sim_shot("lazy1");
        /* a window under others keeps changing: bring the editor up, type, wait
         */
        for (int i = 0; i < win_count; i++)
            if (win_stack[i]->title && strstr(win_stack[i]->title, "TEditor")) {
                wm_focus(win_stack[i]);
                break;
            }
        sim_frames(3);
        sim_type("!");
        sim_frames(80);
        /* shade + minimize + restore */
        window *tw = wm_focused_window();
        sim_dblclick(tw->x + tw->w / 2, tw->y + MENUBAR_H + 8);
        sim_frames(40);
        sim_dblclick(tw->x + tw->w / 2, tw->y + MENUBAR_H + 8);
        sim_frames(20);
        sim_click(tw->x + 20, tw->y + MENUBAR_H + 7);
        sim_frames(40); /* minimize */
        sim_click(8, SCREEN_HEIGHT - 4);
        sim_frames(40); /* restore */
        /* drag a window around */
        tw = wm_focused_window();
        sim_drag(tw->x + tw->w / 2, tw->y + MENUBAR_H + 8,
                 tw->x + tw->w / 2 + 60, tw->y + MENUBAR_H + 48);
        sim_frames(30);
        /* tooltip appears and goes by itself */
        sim_move(dw->x + 10, dw->y + 26 + 10);
        sim_frames(120);
        sim_shot("lazy2");
        T("checked many frames", sim_checked > 300);
        T("lazy == full redraw on every frame", sim_mismatch == 0);
        T("blit row range covers every change", sim_rangebad == 0);
        return fails;
    }
    if (!strcmp(name, "lowheap")) {
        int fails = 0;
        char b[300];
        static uint8_t low[65536] __attribute__((aligned(16)));
        uint32_t f0 = heap_remaining();
        alloc_add_low((uint32_t)low, (uint32_t)low + sizeof low);
        T("low heap adds to free", heap_remaining() > f0 + 60000);
        static void *ptr[20000];
        int n = 0;
        while (n < 20000 && (ptr[n] = kmalloc(4000)) != NULL)
            n++;
        int in_low = 0;
        for (int i = 0; i < n; i++)
            if ((uint8_t *)ptr[i] >= low &&
                (uint8_t *)ptr[i] < low + sizeof low)
                in_low++;
        T("main full: kmalloc falls back to the low heap", in_low >= 14);
        void *r = krealloc(ptr[n - 1], 100); /* shrink in place, low block */
        T("krealloc works on a low block", r == ptr[n - 1]);
        for (int i = 0; i < n; i++)
            kfree(ptr[i]);
        T("all freed: free is back",
          heap_remaining() == f0 + heap_free_in(&g_kheap_low));
        T("both heaps intact",
          heap_check(&g_kheap) && heap_check(&g_kheap_low));
        void *q = kmalloc(100);
        T("after freeing, the main heap is used first",
          q && !((uint8_t *)q >= low && (uint8_t *)q < low + sizeof low));
        kfree(q);
        return fails;
    }
    if (!strcmp(name, "cli")) {
        extern const char *g_sim_script;
        extern int g_sim_hw, g_sim_pings;
        extern void cli_run(void);
        extern void scheduler_init(void);
        extern void net_init(void);
        extern void net_set_ip(const uint8_t *, const uint8_t *,
                               const uint8_t *);
        static uint8_t buf[512];
        uint32_t n = 0;
        if (!load_file(FIXDIR "bin/HELLO.BIN", buf, sizeof buf, &n))
            return 1;
        fs_delete("/HELLO.BIN");
        fat_file_t f;
        fs_create("/HELLO.BIN", &f);
        fs_write(&f, buf, n);
        fs_close(&f);
        g_cfg.networking_enabled = 1;
        g_sim_hw = 1;
        static const uint8_t ip[4] = {10, 0, 2, 15}, gw[4] = {10, 0, 2, 2},
                             nm[4] = {255, 255, 255, 0};
        net_init();
        net_set_ip(ip, gw, nm);
        scheduler_init();
        g_sim_script = "ping 10.0.2.2\n\x02"
                       "ping 10.0.2.2 2\n\x02\x01"
                       "1"
                       "run HELLO.BIN\n\x02"
                       "Denis\x01"
                       "2"
                       "\n\x02\x01"
                       "3"
                       "gopher 10.0.2.2\n\x02\x02\x02\x02\x02\x02\x02\x01"
                       "4"
                       "run HELLO.BIN\n\x02"
                       "Bo\x04\x01"
                       "5"
                       "ls\n\x02\x01"
                       "6"
                       "\x03";
        cli_run();
        return 0;
    }
    if (!strcmp(name, "datecmd")) {
        extern int cli_execute_command(const char *, char *, size_t);
        static char o[2048];
        const char *cmds[] = {"help",
                              "help time",
                              "help nope",
                              "date",
                              "cal",
                              "cal 2 2028",
                              "cal 2 2100",
                              "cal 1752",
                              "date 2026-02-30",
                              "date 29.02.2028",
                              "time 23:59:30",
                              "date",
                              "time 7:5",
                              "time 25:00",
                              "cal 13 2020",
                              "date 1979-12-31",
                              NULL};
        for (int i = 0; cmds[i]; i++) {
            o[0] = 0;
            wr(1, "> ");
            wr(1, cmds[i]);
            wr(1, "\n");
            cli_execute_command(cmds[i], o, sizeof o);
            wr(1, o);
        }
        extern unsigned char g_cmos[128];
        char b[80];
        snprintf(b, sizeof b,
                 "CMOS: %02x-%02x-%02x %02x:%02x:%02x wd %x cent %02x\n",
                 g_cmos[9], g_cmos[8], g_cmos[7], g_cmos[4], g_cmos[2],
                 g_cmos[0], g_cmos[6], g_cmos[0x32]);
        wr(1, b);
        return 0;
    }
    if (!strcmp(name, "textmode")) {
        extern void cli_debug_redraw(void);
        extern void term_buf_push_text(const char *);
        extern void cmd_help(const char *, char *, size_t);
        static char out[4096];
        out[0] = 0;
        cmd_help("", out, sizeof out);
        term_buf_push_text(out);
        term_buf_push_text("Bytes: 00 11 22 33 44 55 66 77  ABCDEFGH\nMMMM "
                           "WWWW mmmm wwww iiii llll\n");
        clear_screen(BLACK);
        cli_debug_redraw();
        sim_shot("txt1");
        sim_launch("ASMTERM");
        sim_frames(3);
        sim_type("ver\n");
        sim_frames(3);
        sim_type("mem\n");
        sim_frames(3);
        sim_type("typing here");
        sim_frames(2);
        sim_shot("txt2");
        return 0;
    }
    if (!strcmp(name, "lazyapp")) {
        int fails = 0;
        char b[300];
        static uint8_t buf[8192];
        uint32_t n = 0;
        if (!load_file(FIXDIR "lz/LAZY.APP", buf, sizeof buf, &n))
            return 1;
        fs_delete("/LAZY.APP");
        fat_file_t f;
        fs_create("/LAZY.APP", &f);
        fs_write(&f, buf, n);
        fs_close(&f);
        if (!appload_launch("/LAZY.APP")) {
            wr(2, appload_last_error());
            wr(2, "\n");
            return 1;
        }
        sim_frames(10);
        window *w = wm_focused_window();
        T("set_lazy took", w && w->lazy_custom);
        typedef struct {
            window *win;
            const void *api;
            control *view;
            int tick, painted;
        } st_t;
        st_t *s = w->dyn_state;
        sim_move(300, 300);
        sim_frames(5);
        int p0 = s->painted;
        sim_frames(100);
        {
            char m[64];
            snprintf(m, sizeof m, "painted %d\n", s->painted - p0);
            wr(1, m);
        }
        T("idle: painted only when invalidated",
          s->painted - p0 >= 4 && s->painted - p0 <= 6);
        return fails;
    }
    if (!strncmp(name, "sugen", 5)) { /* sugen<L><N>: N launches at level L */
        static uint8_t buf[16384];
        uint32_t n = 0;
        if (!load_file(FIXDIR "lz/SUDOKU.APP", buf, sizeof buf, &n))
            return 1;
        fs_delete("/SUDOKU.APP");
        fat_file_t f;
        fs_create("/SUDOKU.APP", &f);
        fs_write(&f, buf, n);
        fs_close(&f);
        int L = name[5] - 0x30,
            N = (name[6] - 0x30) * 10 + (name[7] ? name[7] - 0x30 : 0);
        if (!name[7])
            N /= 10;
        for (int k = 0; k < N; k++) {
            fs_delete("/PREFS/SUDOKU.INI");
            app_prefs_set_int("SUDOKU", "level", L);
            appload_launch("/SUDOKU.APP");
            sim_frames(1);
            {
                window *w = wm_focused_window();
                uint8_t *gv = (uint8_t *)w->dyn_state + 16 + 81;
                int g = 0;
                for (int i = 0; i < 81; i++)
                    g += gv[i];
                char m[32];
                snprintf(m, sizeof m, "%d ", g);
                wr(1, m);
            }
            wm_close(wm_focused_window());
            sim_frames(2);
        }
        return 0;
    }
    if (!strcmp(name, "sudoku")) {
        int fails = 0;
        char b[300];
        static uint8_t buf[16384];
        uint32_t n = 0;
        if (!load_file(FIXDIR "lz/SUDOKU.APP", buf, sizeof buf, &n))
            return 1;
        fs_delete("/PREFS/SUDOKU.INI");
        fs_delete("/SUDOKU.APP");
        fat_file_t f;
        fs_create("/SUDOKU.APP", &f);
        fs_write(&f, buf, n);
        fs_close(&f);
        if (!appload_launch("/SUDOKU.APP")) {
            wr(2, appload_last_error());
            wr(2, "\n");
            return 1;
        }
        sim_frames(10);
        window *w = wm_focused_window();
        T("window, lazy", w && w->lazy_custom && !strcmp(w->title, "Sudoku"));
        typedef struct {
            window *win;
            const void *api;
            control *view, *status;
            uint8_t sol[81], given[81], cur[81];
            uint16_t notes[81];
            uint8_t wrong[81];
            int sel, level;
            bool notes_mode, solved, playing;
        } su_t;
        su_t *s = w->dyn_state;
        int giv = 0, ok = 1;
        for (int i = 0; i < 81; i++) {
            giv += s->given[i];
            if (s->given[i] && s->cur[i] != s->sol[i])
                ok = 0;
        }
        {
            char m[64];
            snprintf(m, sizeof m, "givens %d level %d\n", giv, s->level);
            wr(1, m);
        }
        T("easy: about 41 givens, consistent", ok && giv >= 38 && giv <= 45);
        /* the solution is a valid sudoku */
        int valid = 1;
        for (int i = 0; i < 81; i++)
            for (int k = 0; k < 81; k++)
                if (i != k && s->sol[i] == s->sol[k] &&
                    (i / 9 == k / 9 || i % 9 == k % 9 ||
                     (i / 27 == k / 27 && i % 9 / 3 == k % 9 / 3)))
                    valid = 0;
        T("solution valid", valid);
        sim_shot("su1");
        int e = 0;
        while (s->given[e])
            e++;
        s->sel = e;
        wm_invalidate(w);
        int wrongd = s->sol[e] % 9 + 1;
        sim_key(0x01 + wrongd);
        sim_frames(2);
        T("digit entered", s->cur[e] == wrongd);
        sim_key(0x0E);
        sim_frames(2);
        T("backspace clears", s->cur[e] == 0);
        sim_key(0x31);
        sim_frames(1); /* N */
        T("notes mode", s->notes_mode);
        sim_key(0x02);
        sim_key(0x04);
        sim_key(0x0A);
        sim_frames(2);
        T("notes 1 3 9",
          s->notes[e] == ((1 << 1) | (1 << 3) | (1 << 9)) && !s->cur[e]);
        sim_key(0x31);
        sim_frames(1);
        int e2 = e + 1;
        while (s->given[e2])
            e2++;
        sim_shot("su2");
        /* fill all but the last */
        int last = 80;
        while (s->given[last])
            last--;
        for (int i = 0; i < 81; i++)
            if (!s->given[i] && i != last) {
                s->sel = i;
                sim_key(0x01 + s->sol[i]);
                sim_frames(1);
            }
        T("notes cleared by digits", s->notes[e] == 0);
        T("not solved yet", !s->solved);
        s->sel = last;
        sim_key(0x01 + s->sol[last]);
        sim_frames(3);
        T("solved", s->solved);
        sim_shot("su3");
        sim_key(0x1C);
        sim_frames(3);
        /* closing a solved game: the next start is a new game */
        wm_close(w);
        sim_frames(5);
        appload_launch("/SUDOKU.APP");
        sim_frames(5);
        w = wm_focused_window();
        s = w->dyn_state;
        T("new game after a solved one",
          !s->solved && s->cur[0] == (s->given[0] ? s->sol[0] : 0));
        /* save and restore a game in progress */
        e = 0;
        while (s->given[e])
            e++;
        s->sel = e;
        sim_key(0x01 + s->sol[e]);
        sim_frames(70);
        uint8_t keep[81];
        memcpy(keep, s->cur, 81);
        wm_close(w);
        sim_frames(5);
        {
            fat_file_t f2;
            static char t[800];
            int k = 0;
            if (fs_open("/PREFS/SUDOKU.INI", &f2)) {
                k = fs_read(&f2, t, 799);
                fs_close(&f2);
            }
            t[k > 0 ? k : 0] = 0;
            wr(1, t);
            wr(1, "\n");
        }
        appload_launch("/SUDOKU.APP");
        sim_frames(5);
        w = wm_focused_window();
        s = w->dyn_state;
        T("game in progress restored", !memcmp(keep, s->cur, 81) && s->cur[e]);
        {
            char m[96];
            snprintf(m, sizeof m, "status: %s\n", ctl_text(s->status));
            wr(1, m);
        }
        return fails;
    }
    if (!strcmp(name, "clocktab")) {
        int fails = 0;
        char b[300];
        app_instance_t *a = sim_launch("Settings");
        sim_frames(5);
        T("settings", a && a->state);
        window *w = wm_focused_window();
        control *tabs = NULL;
        for (control *c = w->controls; c; c = c->next)
            if (c->type == CTL_TABS)
                tabs = c;
        ctl_set_value(tabs, 5);
        sim_frames(5);
        sim_shot("ck0");
        ctl_set_value(tabs, 2);
        sim_frames(5);
        sim_shot("ck1");
        typedef struct {
            window *win;
            control *tabs, *status, *save, *pattern, *col_a, *col_b, *screen;
            int ss;
            control *vol, *vol_lbl, *ip[4], *perf[5], *fps, *c_day, *c_mon,
                *c_year, *c_h, *c_m, *c_s, *c_wd, *c_now;
            bool clock_edited;
        } st_t;
        st_t *s = a->state;
        T("fields follow the clock",
          !s->clock_edited && !strcmp(ctl_text(s->c_year), "2026"));
        sim_frames(70);
        T("still following after a second", !s->clock_edited);
        /* type a date */
        ctl_set_text(s->c_day, "29");
        list_set_sel(s->c_mon, 1);
        ctl_set_text(s->c_year, "2027");
        s->c_day->handler(s->c_day, EV_CHANGE);
        T("Feb 29 2027 is not a date",
          strstr(ctl_text(s->c_wd), "Not a date") != NULL);
        ctl_set_text(s->c_year, "2028");
        s->c_year->handler(s->c_year, EV_CHANGE);
        T("Feb 29 2028 is a Tuesday",
          strstr(ctl_text(s->c_wd), "Tuesday") != NULL);
        ctl_set_text(s->c_h, "08");
        ctl_set_text(s->c_m, "15");
        ctl_set_text(s->c_s, "00");
        sim_frames(70);
        T("edited fields stay", !strcmp(ctl_text(s->c_year), "2028"));
        sim_shot("ck2");
        /* time zone +2, then Set Clock: the chip gets UTC */
        g_cfg.timezone_offset = 2;
        control *set = NULL;
        for (control *c = w->controls; c; c = c->next)
            if (c->text && !strcmp(c->text, "Set Clock"))
                set = c;
        set->handler(set, EV_CLICK);
        sim_frames(3);
        extern unsigned char g_cmos[128];
        snprintf(b, sizeof b, "CMOS %02x-%02x-%02x %02x:%02x", g_cmos[9],
                 g_cmos[8], g_cmos[7], g_cmos[4], g_cmos[2]);
        wr(1, b);
        wr(1, "\n");
        T("chip holds UTC (06:15)",
          g_cmos[4] == 0x06 && g_cmos[2] == 0x15 && g_cmos[7] == 0x29);
        time_full_t t = time_rtc_local();
        T("local reads back 08:15",
          t.hours == 8 && t.minutes == 15 && t.day == 29 && t.month == 2);
        sim_frames(5);
        sim_shot("ck3");
        return fails;
    }
    if (!strcmp(name, "calendar") || !strcmp(name, "calendar320")) {
        int fails = 0;
        char b[300];
        if (name[8]) {
            extern int g_screen_width, g_screen_height, g_video_mode;
            g_screen_width = 320;
            g_screen_height = 200;
            g_video_mode = 0;
        }
        fs_delete("/PREFS/CALNOTES.TXT");
        fs_delete("/PREFS/CALENDAR.INI");
        app_instance_t *a = sim_launch("CALENDAR");
        sim_frames(5);
        T("opens", a && a->state);
        typedef struct {
            int32_t day;
            char text[60];
        } nt_t;
        typedef struct {
            window *win;
            control *title, *grid, *note, *status;
            int y, m;
            int32_t sel, today;
            bool sunday;
            int nnotes;
            nt_t notes[64];
        } cs_t;
        cs_t *s = a->state;
        T("today selected, Nov 2026",
          s->sel == s->today && s->m == 11 && s->y == 2026);
        {
            char m2[96];
            snprintf(m2, sizeof m2, "status: %s\n", ctl_text(s->status));
            wr(1, m2);
        }
        T("status: Mon 16 Nov 2026, week 47, today",
          !strcmp(ctl_text(s->status), "Mon 16 Nov 2026, week 47, today"));
        sim_shot("cal1");
        sim_ext(RIGHT_ARROW);
        sim_ext(DOWN_ARROW);
        sim_frames(2);
        T("arrows: +8 days = 24 Nov", s->sel == s->today + 8);
        sim_key(0x1C);
        sim_frames(3); /* Enter: note dialog */
        sim_type("Dentist 10:00");
        sim_key(0x1C);
        sim_frames(3);
        T("note stored",
          s->nnotes == 1 && !strcmp(s->notes[0].text, "Dentist 10:00"));
        sim_ext(PAGE_DOWN);
        sim_ext(PAGE_DOWN);
        sim_ext(PAGE_DOWN);
        sim_frames(2);
        T("PgDn x3: Feb 2027 (24th)", s->m == 2 && s->y == 2027);
        sim_ext(PAGE_UP);
        sim_ext(PAGE_UP);
        sim_ext(PAGE_UP);
        sim_frames(2);
        sim_shot("cal2");
        /* 31 Jan 2028 + 1 month = 29 Feb 2028 */
        s->sel = time_days_from_civil(2028, 1, 31);
        s->y = 2028;
        s->m = 1;
        sim_ext(PAGE_DOWN);
        sim_frames(2);
        T("31 Jan + 1 month = 29 Feb (leap)",
          s->sel == time_days_from_civil(2028, 2, 29));
        sim_ext(HOME);
        sim_frames(2);
        T("Home: today", s->sel == s->today);
        /* click a greyed day of the next month: last row */
        control *g = s->grid;
        int gx = (g->aw - 154) / 2;
        sim_click(g->ax + gx + 6 * 22 + 10, g->ay + 1 + 11 + 5 * 15 + 7);
        sim_frames(2);
        T("greyed day goes to December", s->m == 12);
        sim_ext(HOME);
        sim_frames(2);
        /* the notes file, and reloading it */
        fat_file_t f;
        static char t[200];
        int n = 0;
        if (fs_open("/PREFS/CALNOTES.TXT", &f)) {
            n = fs_read(&f, t, 199);
            fs_close(&f);
        }
        t[n > 0 ? n : 0] = 0;
        wr(1, t);
        T("file line", !strcmp(t, "20261124 Dentist 10:00\n"));
        wm_close(s->win);
        sim_frames(4);
        a = sim_launch("CALENDAR");
        sim_frames(5);
        s = a->state;
        T("notes reloaded",
          s->nnotes == 1 &&
              s->notes[0].day == time_days_from_civil(2026, 11, 24));
        app_prefs_set_int("CALENDAR", "sunday", 1);
        wm_close(s->win);
        sim_frames(4);
        a = sim_launch("CALENDAR");
        sim_frames(5);
        s = a->state;
        T("week starts Sunday", s->sunday);
        sim_shot("cal3");
        /* delete */
        s->sel = time_days_from_civil(2026, 11, 24);
        sim_ext(DELETE);
        sim_frames(3);
        T("deleted", s->nnotes == 0 && !fs_open("/PREFS/CALNOTES.TXT", &f));
        return fails;
    }
    if (!strcmp(name, "saverprev")) {
        app_instance_t *a = sim_launch("Settings");
        sim_frames(5);
        window *w = wm_focused_window();
        control *tabs = NULL;
        for (control *c = w->controls; c; c = c->next)
            if (c->type == CTL_TABS)
                tabs = c;
        g_cfg.reduce_motion = 0;
        ctl_set_value(tabs, 4);
        sim_frames(5);
        sim_shot("sp1");
        g_cfg.reduce_motion = 1;
        wm_invalidate(w);
        sim_frames(5);
        sim_shot("sp2");
        (void)a;
        return 0;
    }
    if (!strcmp(name, "settings2")) {
        int fails = 0;
        char b[300];
        extern int lpt_init(void);
        extern unsigned char g_lpt_out[];
        extern unsigned g_lpt_n;
        extern void sim_save(const char *, const void *, unsigned);
        int nports = lpt_init();
        snprintf(b, sizeof b, "ports %d\n", nports);
        wr(1, b);
        T("fake LPT1 found", nports >= 1);
        app_instance_t *a = sim_launch("Settings");
        sim_frames(5);
        window *w = wm_focused_window();
        control *tabs = NULL;
        for (control *c = w->controls; c; c = c->next)
            if (c->type == CTL_TABS)
                tabs = c;
        sim_shot("st0");
        control *saver = NULL, *speed = NULL, *test = NULL;
        for (control *c = w->controls; c; c = c->next)
            if (c->text) {
                if (!strcmp(c->text, "Screen Saver..."))
                    saver = c;
                if (!strcmp(c->text, "Speed..."))
                    speed = c;
                if (!strcmp(c->text, "Test Page"))
                    test = c;
            }
        T("buttons", saver && speed && test);
        sim_click(saver->ax + 5, saver->ay + 5);
        sim_frames(8);
        window *sw = wm_focused_window();
        T("saver window over settings",
          sw != w && !strcmp(sw->title, "Screen Saver") &&
              w->modal_child == sw);
        sim_shot("st1");
        sim_key(0x1C);
        sim_frames(5); /* OK (default) */
        T("closed with Enter", wm_focused_window() == w && !w->modal_child);
        sim_click(speed->ax + 5, speed->ay + 5);
        sim_frames(8);
        sw = wm_focused_window();
        sim_shot("st2");
        T("speed window", !strcmp(sw->title, "Speed"));
        wm_close(sw);
        sim_frames(5);
        T("closed with X", wm_focused_window() == w && !w->modal_child);
        ctl_set_value(tabs, 4);
        sim_frames(5);
        sim_shot("st3");
        ctl_set_value(tabs, 5);
        sim_frames(5);
        sim_shot("st4");
        ctl_set_value(tabs, 4);
        sim_frames(2);
        g_lpt_n = 0;
        test->handler(test, EV_CLICK);
        sim_frames(20);
        sim_save(OUTDIR "lpt_test.bin", g_lpt_out, g_lpt_n);
        snprintf(b, sizeof b, "test page %u bytes\n", g_lpt_n);
        wr(1, b);
        T("test page printed", g_lpt_n > 200 &&
                                   g_lpt_out[g_lpt_n - 1] == '\f' &&
                                   print_job_count() == 0);
        /* a picture in each language */
        static uint8_t pic[64 * 48];
        for (int y = 0; y < 48; y++)
            for (int x = 0; x < 64; x++)
                pic[y * 64 + x] = (x / 8 + y / 12 * 8) & 15;
        for (int t = 0; t < 3; t++) {
            g_cfg.printer_type = t;
            g_lpt_n = 0;
            print_submit_image("pic", pic, 64, 48, 64, 1);
            sim_frames(40);
            char fn[32];
            snprintf(fn, sizeof fn, OUTDIR "lpt_img%d.bin", t);
            sim_save(fn, g_lpt_out, g_lpt_n);
            snprintf(b, sizeof b, "image type %d: %u bytes\n", t, g_lpt_n);
            wr(1, b);
        }
        g_cfg.printer_type = 0;
        /* paper out: the job stops, then goes on */
        extern int g_lpt_status;
        g_lpt_status = 0xD8 | 0x20;
        g_lpt_n = 0;
        print_submit_text("long", "hello\nworld\n", 12, 2, 0, 0);
        sim_frames(5);
        T("paper out: waiting with the error",
          print_job_count() == 1 && print_job(0)->state == PJ_ERROR &&
              g_lpt_n == 0);
        app_instance_t *q = sim_launch("PRINTQ");
        sim_frames(6);
        sim_shot("st5");
        g_lpt_status = 0xD8;
        sim_frames(10);
        T("paper back: printed, 2 copies",
          print_job_count() == 0 && g_lpt_n == 2 * 15);
        (void)a;
        (void)q;
        return fails;
    }
    if (!strcmp(name, "floppy")) {
        int fails = 0;
        char b[300];
        extern int cli_execute_command(const char *, char *, size_t);
        extern uint8_t g_fdc_img[];
        extern int g_fdc_formats;
        extern void fdc_insert(int);
#define g_fdd_img g_fdc_img
#define g_fdd_formatted_tracks g_fdc_formats
        extern void sim_save(const char *, const void *, unsigned);
        static char o[2048];
        memset(g_fdd_img, 0, 2880 * 512);
        fdc_insert(720); /* blank 720K disk */
        o[0] = 0;
        cli_execute_command("format a: 720 workdisk", o, sizeof o);
        wr(1, o);
        T("full 720K format: 160 tracks", g_fdd_formatted_tracks == 160);
        T("mounted", fs_drive_mounted(DRIVE_FDD0));
        T("BPB 720K", g_fdd_img[11] == 0 && g_fdd_img[12] == 2 &&
                          g_fdd_img[13] == 2 && g_fdd_img[19] == 0xA0 &&
                          g_fdd_img[20] == 5 && g_fdd_img[21] == 0xF9 &&
                          g_fdd_img[22] == 3 && g_fdd_img[24] == 9 &&
                          g_fdd_img[510] == 0x55);
        fat_file_t f;
        {
            extern fat_vol_t g_drives[];
            fat_vol_t *v = &g_drives[2];
            snprintf(b, sizeof b,
                     "    fdd0: mounted %d rsv %u fat %u root %u data %u "
                     "clusters %u type %d base %u\n",
                     v->mounted, v->bpb.reserved_sectors, v->fat_lba,
                     v->root_lba, v->data_lba, v->cluster_count, v->fat_type,
                     v->base_lba);
            wr(1, b);
        }
        {
            uint8_t dv = 9;
            uint16_t pc = 9;
            char n83[12] = {0};
            bool r = fs_resolve("/FDD0/HELLO.TXT", &dv, &pc, n83);
            extern uint16_t fat_alloc(uint8_t);
            snprintf(b, sizeof b, "    resolve %d drv %d pc %d n83 '%s'\n", r,
                     dv, pc, n83);
            wr(1, b);
            static uint8_t sb[512];
            bool rr = fs_read_sector(DRIVE_FDD0, 20, sb);
            bool ww = fs_write_sector(DRIVE_FDD0, 20, sb);
            extern bool g_fdc_wp;
            extern uint16_t fat_alloc(uint8_t);
            extern uint16_t fat_get(uint8_t, uint16_t);
            fs_read_sector(DRIVE_FDD0, 1, sb);
            snprintf(b, sizeof b,
                     "    read %d write %d wp %d fat2..5 %04X %04X %04X %04X "
                     "raw %02X %02X %02X %02X %02X %02X\n",
                     rr, ww, g_fdc_wp, fat_get(2, 2), fat_get(2, 3),
                     fat_get(2, 4), fat_get(2, 5), sb[0], sb[1], sb[2], sb[3],
                     sb[4], sb[5]);
            wr(1, b);
        }
        T("create on A:", fs_create("/FDD0/HELLO.TXT", &f));
        static char big[3000];
        for (int i = 0; i < 3000; i++)
            big[i] = 'a' + i % 26;
        fs_write(&f, "Hello from ASMOS\r\n", 18);
        fs_close(&f);
        T("mkdir", fs_mkdir("/FDD0/DOCS"));
        T("create in dir", fs_create("/FDD0/DOCS/BIG.TXT", &f));
        fs_write(&f, big, 3000);
        fs_close(&f);
        static char rb[3100];
        int n = 0;
        if (fs_open("/FDD0/DOCS/BIG.TXT", &f)) {
            n = fs_read(&f, rb, 3100);
            fs_close(&f);
        }
        T("read back 3000 bytes", n == 3000 && !memcmp(rb, big, 3000));
        sim_save(OUTDIR "fd720.img", g_fdd_img, 1440 * 512);
        o[0] = 0;
        cli_execute_command("ls /FDD0", o, sizeof o);
        wr(1, o);
#undef g_fdd_img
#undef g_fdd_formatted_tracks
#define g_fdd_img g_fdc_img
#define g_fdd_formatted_tracks g_fdc_formats
        /* quick 1.44 on a 720K disk: can't write at the wrong rate */
        g_fdd_formatted_tracks = 0;
        o[0] = 0;
        cli_execute_command("format a: 1440 /q", o, sizeof o);
        wr(1, o);
        T("quick at the wrong size fails",
          strstr(o, "Could not write") != NULL);
        /* full 1.44 makes it a 1.44 disk */
        o[0] = 0;
        cli_execute_command("format a: 1440", o, sizeof o);
        wr(1, o);
        T("full 1.44: 160 tracks, mounted",
          g_fdd_formatted_tracks == 160 && fs_drive_mounted(DRIVE_FDD0) &&
              g_fdd_img[19] == 0x40 && g_fdd_img[20] == 0x0B);
        sim_save(OUTDIR "fd144.img", g_fdd_img, 2880 * 512);
        o[0] = 0;
        cli_execute_command("format", o, sizeof o);
        wr(1, o);
        o[0] = 0;
        cli_execute_command("format a: 360", o, sizeof o);
        wr(1, o);
        T("360 refused in a 1.44 drive", strstr(o, "cannot format") != NULL);
        /* the window */
        app_instance_t *a = sim_launch("FORMAT");
        sim_frames(5);
        sim_shot("fmt1");
        typedef struct {
            window *win;
            control *drive, *size, *label, *quick, *start, *bar, *msg;
            int sizes[6], nsizes;
            uint8_t ids[3];
            int nids;
            bool running;
        } fs_t;
        fs_t *st = a->state;
        T("sizes: 1.44 MB first (the disk's)",
          st->nsizes == 2 && ctl_value(st->size) == 0);
        list_set_sel(st->size, 1);
        ctl_set_text(st->label, "GUIDISK");
        st->start->handler(st->start, EV_CLICK);
        sim_frames(3);
        sim_key(0x1C);
        sim_frames(30); /* Yes in the confirm box */
        sim_shot("fmt2");
        T("formatting in steps", st->running);
        sim_frames(200);
        snprintf(b, sizeof b, "msg: %s\n", ctl_text(st->msg));
        wr(1, b);
        T("done, 720K",
          !st->running && strstr(ctl_text(st->msg), "730112") != NULL);
        sim_shot("fmt3");
        /* no disk in the drive: refused at once, nothing formatted */
        extern void fdc_insert_unit(int, int);
        extern bool g_fdc_wp;
        extern int g_fdc_formats;
        fdc_insert_unit(0, 0);
        int fm0 = g_fdc_formats;
        o[0] = 0;
        cli_execute_command("format fdd0 1440", o, sizeof o);
        wr(1, o);
        T("no disk: format refused",
          strstr(o, "no disk in FDD0") != NULL && g_fdc_formats == fm0);
        o[0] = 0;
        cli_execute_command("format fdd0 /q", o, sizeof o);
        T("no disk: quick refused", strstr(o, "no disk in FDD0") != NULL);
        st->start->handler(st->start, EV_CLICK);
        sim_frames(3);
        sim_key(0x1C);
        sim_frames(5);
        window *mw = wm_focused_window();
        T("no disk: the window says so, not running",
          !st->running && mw && mw != st->win);
        sim_shot("fmt4");
        if (mw && mw != st->win)
            wm_close(mw);
        /* write-protected */
        fdc_insert_unit(0, 1440);
        g_fdc_wp = true;
        o[0] = 0;
        cli_execute_command("format fdd0 1440", o, sizeof o);
        T("write-protected: refused",
          strstr(o, "write-protected") != NULL && g_fdc_formats == fm0);
        g_fdc_wp = false;
        fdc_insert_unit(0, 1440); /* (the tab is moved with the disk out) */
        o[0] = 0;
        cli_execute_command("format fdd0 /q BACK", o, sizeof o);
        wr(1, o);
        T("disk back: formats",
          fs_drive_mounted(DRIVE_FDD0) &&
              !strcmp(fs_drive_label(DRIVE_FDD0), "BACK"));
        return fails;
    }
    if (!strcmp(name, "formathdb")) {
        /* Format Disk: FDD0 and HDB in the list (never HDA); HDB erased
         * into one empty, labelled volume, the files on it gone */
        int fails = 0;
        char b[300];
        extern void sim_disk_blank(int, uint32_t, const char *);
        sim_disk_blank(1, 40 * 2048, "CONNER CP3044");
        const char *e = inst_erase_label(1, "OLD", NULL, NULL);
        T("HDB with files on it",
          !e && fs_mount_drive(DRIVE_HDB) && fs_mkdir("/HDB/STUFF") &&
              !strcmp(fs_drive_label(DRIVE_HDB), "OLD"));
        desktop_fs_set_dirty();
        app_instance_t *a = sim_launch("FORMAT");
        sim_frames(5);
        typedef struct {
            window *win;
            control *drive, *size, *label, *quick, *start, *bar, *msg;
            int sizes[6], nsizes;
            uint8_t ids[3];
            int nids;
            bool running;
        } fs_t;
        fs_t *st = a->state;
        bool hda = false;
        for (int i = 0; i < st->nids; i++)
            hda |= st->ids[i] == DRIVE_HDA;
        snprintf(b, sizeof b, "    drives: %d (%s ...)\n", st->nids,
                 list_get(st->drive, 0));
        wr(1, b);
        T("FDD0 and HDB listed, not HDA",
          st->nids == 2 && st->ids[0] == DRIVE_FDD0 &&
              st->ids[1] == DRIVE_HDB && !hda &&
              !strcmp(list_get(st->drive, 0), "FDD0"));
        ctl_set_value(st->drive, 1);
        st->drive->handler(st->drive, EV_CHANGE);
        sim_frames(2);
        snprintf(b, sizeof b, "    size '%s', msg '%s'\n",
                 list_get(st->size, 0), ctl_text(st->msg));
        wr(1, b);
        T("HDB: its size, quick only",
          !strcmp(list_get(st->size, 0), "39 MB") && !st->quick->enabled);
        ctl_set_text(st->label, "Data");
        st->start->handler(st->start, EV_CLICK);
        sim_frames(3);
        sim_shot("fmthdb1");
        window *cw = wm_focused_window();
        T("asks first, naming the disk", cw && cw != st->win);
        sim_key(0x1C); /* Yes */
        sim_frames(5);
        dir_entry_t de;
        snprintf(b, sizeof b, "    msg '%s', label '%s'\n", ctl_text(st->msg),
                 fs_drive_label(DRIVE_HDB));
        wr(1, b);
        T("HDB erased and mounted",
          fs_drive_mounted(DRIVE_HDB) && !fs_find("/HDB/STUFF", &de));
        T("its new label", !strcmp(fs_drive_label(DRIVE_HDB), "DATA"));
        T("said done", strstr(ctl_text(st->msg), "Done") != NULL);
        T("HDA untouched", fs_find("/SYSTEM/KERNEL.SYS", &de));
        T("writable", fs_mkdir("/HDB/NEW"));
        sim_shot("fmthdb2");
        wm_close(st->win);
        sim_frames(3);
        /* no floppy drive at all: not offered */
        extern int g_fdc_drive_type, g_fdcb_drive_type;
        extern void fdc_setup(void);
        g_fdc_drive_type = 0;
        g_fdcb_drive_type = 0;
        fdc_setup();
        fdd_init(0);
        fdd_init(1);
        a = sim_launch("FORMAT");
        sim_frames(5);
        st = a->state;
        T("no floppy drive: only HDB",
          st->nids == 1 && st->ids[0] == DRIVE_HDB &&
              !strcmp(list_get(st->drive, 0), "HDB"));
        static char o[512];
        extern int cli_execute_command(const char *, char *, size_t);
        o[0] = 0;
        cli_execute_command("format fdd0 /q", o, sizeof o);
        T("format fdd0: no such drive",
          strstr(o, "no such floppy drive") != NULL);
        return fails;
    }
    if (!strcmp(name, "printdlg")) {
        int fails = 0;
        char b[300];
        extern int lpt_init(void);
        extern unsigned char g_lpt_out[];
        extern unsigned g_lpt_n;
        lpt_init();
        g_cfg.printer_type = 0;
        g_cfg.printer_port = 0;
        app_instance_t *a = sim_launch("TEDITOR");
        sim_frames(5);
        window *w = wm_focused_window();
        control *ed = NULL;
        for (control *c = w->controls; c; c = c->next)
            if (c->type == CTL_EDIT)
                ed = c;
        static char t[12000];
        int n = 0;
        for (int i = 1; i <= 150; i++)
            n += snprintf(
                t + n, sizeof t - n,
                "Line %d: the quick brown fox jumps over the lazy dog%s\n", i,
                i % 7 == 0 ? " and keeps on running far beyond the eightieth "
                             "column of the page"
                           : "");
        ctl_set_text(ed, t);
        void (*pr)(void) = NULL;
        for (int m = 0; m < w->win_menu_count; m++)
            for (int k = 0; k < w->win_menus[m].item_count; k++)
                if (w->win_menus[m].items[k].label &&
                    !strcmp(w->win_menus[m].items[k].label, "Print..."))
                    pr = w->win_menus[m].items[k].action;
        T("File > Print...", pr != NULL);
        wm_focus(w);
        pr();
        sim_frames(6);
        window *dw = wm_focused_window();
        sim_shot("pd1");
        T("print dialog", !strcmp(dw->title, "Print"));
        control *pv = NULL, *rng = NULL, *from = NULL, *to = NULL,
                *copies = NULL, *prn = NULL;
        int ne = 0;
        for (control *c = dw->controls; c; c = c->next) {
            if (c->text && !strcmp(c->text, "Preview..."))
                pv = c;
            if (c->text && !strcmp(c->text, "Pages"))
                rng = c;
            if (c->text && !strcmp(c->text, "Print"))
                prn = c;
            if (c->type == CTL_EDIT) {
                if (ne == 0)
                    copies = c;
                else if (ne == 1)
                    from = c;
                else
                    to = c;
                ne++;
            }
        }
        snprintf(b, sizeof b, "pages: %s\n", ctl_text(to));
        wr(1, b);
        T("3 pages (150 lines + wraps)", !strcmp(ctl_text(to), "3"));
        sim_click(pv->ax + 5, pv->ay + 5);
        sim_frames(8);
        window *pw = wm_focused_window();
        sim_shot("pd2");
        T("preview", !strcmp(pw->title, "Print Preview"));
        sim_ext(PAGE_DOWN);
        sim_frames(3);
        control *zb = NULL;
        for (control *c = pw->controls; c; c = c->next)
            if (c->text && !strcmp(c->text, "Zoom"))
                zb = c;
        sim_click(zb->ax + 5, zb->ay + 5);
        sim_frames(4);
        sim_shot("pd3");
        sim_key(0x01);
        sim_frames(5); /* Esc closes the preview */
        T("back to the dialog", wm_focused_window() == dw);
        ctl_set_value(rng, 1);
        rng->handler(rng, EV_CHANGE);
        ctl_set_text(from, "2");
        ctl_set_text(to, "2");
        ctl_set_text(copies, "2");
        g_lpt_n = 0;
        sim_click(prn->ax + 5, prn->ay + 5);
        sim_frames(30);
        g_lpt_out[g_lpt_n] = 0;
        int ff = 0;
        for (unsigned i = 0; i < g_lpt_n; i++)
            ff += g_lpt_out[i] == '\f';
        snprintf(b, sizeof b, "printed %u bytes, %d form feeds\n", g_lpt_n, ff);
        wr(1, b);
        T("page 2 only, twice",
          strstr((char *)g_lpt_out, "Line 1:") == NULL &&
              strstr((char *)g_lpt_out, "Line 60:") != NULL && ff == 2);
        T("dialog closed", wm_focused_window() == w);
        /* ASMDraw picture */
        g_cfg.printer_type = 1;
        app_instance_t *d = sim_launch("ASMDRAW");
        sim_frames(5);
        window *aw = wm_focused_window();
        typedef struct {
            window *win;
        } ad_t;
        (void)d;
        const menu_item *pp = NULL; /* an .APP now: run it as the menu does */
        for (int m = 0; m < aw->win_menu_count; m++)
            for (int k = 0; k < aw->win_menus[m].item_count; k++)
                if (aw->win_menus[m].items[k].label &&
                    !strcmp(aw->win_menus[m].items[k].label, "Print..."))
                    pp = &aw->win_menus[m].items[k];
        T("ASMDraw File > Print...", pp != NULL);
        {
            control *cv = NULL;
            for (control *c = aw->controls; c; c = c->next)
                if (c->type == CTL_CUSTOM && c->ah > 60)
                    cv = c;
            for (int k = 0; k < 12; k++) {
                sim_drag(cv->ax + 10 + k * 8, cv->ay + 10, cv->ax + 60 + k * 8,
                         cv->ay + 90);
            }
            sim_drag(cv->ax + 20, cv->ay + 100, cv->ax + 200, cv->ay + 100);
        }
        /* paint on the canvas through its pixels */

        extern void menu_item_run(const menu_item *);
        menu_item_run(pp);
        sim_frames(5);
        dw = wm_focused_window();
        T("ASMDraw print dialog", dw != aw && pv != NULL);
        for (control *c = dw->controls; c; c = c->next)
            if (c->text && !strcmp(c->text, "Preview..."))
                pv = c;
        sim_click(pv->ax + 5, pv->ay + 5);
        sim_frames(8);
        sim_shot("pd4");
        (void)a;
        return fails;
    }
    if (!strcmp(name, "audio")) {
        int fails = 0;
        char b[300];
        extern int g_sim_hw;
        g_sim_hw = 1;
        static uint8_t buf[20000];
        uint32_t n = 0;
        fat_file_t f;
        if (!load_file(FIXDIR "TONE.WAV", buf, sizeof buf, &n))
            return 1;
        fs_delete("/TONE.WAV");
        fs_create("/TONE.WAV", &f);
        fs_write(&f, buf, n);
        fs_close(&f);
        if (!load_file(FIXDIR "SONG.MID", buf, sizeof buf, &n))
            return 1;
        fs_delete("/SONG.MID");
        fs_create("/SONG.MID", &f);
        fs_write(&f, buf, n);
        fs_close(&f);
        fs_delete("/PREFS/AUDIOPLA.INI");

        appload_run_named("Audio Player", "/SONG.MID");
        sim_frames(3);
        snprintf(b, sizeof b, "MIDI: %s / %s, %u ms\n", audio_codec_name(),
                 audio_info(), audio_len_ms());
        wr(1, b);
        T("MIDI length 1500 ms (tempo change)", audio_len_ms() == 1500);
        T("MIDI playing", audio_state() == AUDIO_PLAYING);
        sim_frames(30);
        snprintf(b, sizeof b, "pos %u\n", audio_pos_ms());
        wr(1, b);
        T("MIDI position moves",
          audio_pos_ms() >= 400 && audio_pos_ms() <= 560);
        sim_shot("ap1");
        appload_run_named("Audio Player", "/TONE.WAV");
        sim_frames(3);
        snprintf(b, sizeof b, "WAV: %s, %u ms\n", audio_info(), audio_len_ms());
        wr(1, b);
        T("WAV 1.5 s, 11025 Hz 8-bit mono",
          audio_len_ms() >= 1499 && audio_len_ms() <= 1500 &&
              !strcmp(audio_info(), "11025 Hz, 8-bit mono"));
        window *w = wm_focused_window();
        typedef struct {
            window *win;
            control *viz, *bar, *play, *stop, *rew, *next, *loop, *vol,
                *vol_lbl, *list, *status, *cd;
            char paths[40][192];
            int count, cur;
        } ap_t;
        ap_t *s = w->userdata;
        T("list has both", s->count == 2 && s->cur == 1);
        sim_frames(20);
        sim_shot("ap2");
        T("WAV level shows", audio_level() > 100);
        sim_key(0x39);
        sim_frames(2); /* Space: pause */
        uint32_t p0 = audio_pos_ms();
        sim_frames(20);
        T("paused holds",
          audio_state() == AUDIO_PAUSED && audio_pos_ms() == p0);
        /* click the bar at 3/4 */
        int bw = s->bar->aw - 76;
        sim_click(s->bar->ax + 2 + bw * 3 / 4, s->bar->ay + 5);
        sim_frames(2);
        snprintf(b, sizeof b, "after seek %u\n", audio_pos_ms());
        wr(1, b);
        T("seek to ~1125 ms", audio_pos_ms() >= 1080 && audio_pos_ms() <= 1170);
        sim_key(0x39);
        sim_frames(40);
        T("ran to the end: finished, no next",
          audio_state() == AUDIO_STOPPED &&
              strstr(ctl_text(s->status), "Finished"));
        sim_shot("ap3");
        /* the list survives a restart */
        wm_close(w);
        sim_frames(5);
        T("closed: silent", audio_state() == AUDIO_EMPTY);
        app_instance_t *a = sim_launch("AUDIOPLA");
        sim_frames(4);
        s = a->state;
        T("list kept", s->count == 2 && !strcmp(s->paths[0], "/SONG.MID"));
        /* end of the first: on to the next */
        list_set_sel(s->list, 0);
        s->list->handler(s->list, EV_DBLCLICK);
        sim_frames(2);
        T("MIDI from the list", !strcmp(audio_codec_name(), "MIDI"));
        extern bool midi_player_is_playing(void);
        extern void midi_player_stop(void);
        midi_player_stop();
        sim_frames(3); /* the song ends */
        T("advanced to the WAV",
          !strcmp(audio_codec_name(), "WAV") && audio_state() == AUDIO_PLAYING);
        static char o[512];
        extern int cli_execute_command(const char *, char *, size_t);
        o[0] = 0;
        cli_execute_command("play /SONG.MID", o, sizeof o);
        wr(1, o);
        o[0] = 0;
        cli_execute_command("play stop", o, sizeof o);
        wr(1, o);
        return fails;
    }
    if (!strcmp(name, "charset")) {
        /* characters 128-255: the code page picks the glyph; Alt + the
         * number pad types one; every font draws them */
        int fails = 0;
        char b[300];
        extern void kb_process_byte(uint8_t);
        T("Western by default", g_codepage == CP_WESTERN);
        T("1252: 0xE9 is e-acute", cp_unicode[CP_WESTERN][0xE9 - 128] == 0xE9);
        T("1250: 0xE3 is a-breve, 0xBA s-cedilla",
          cp_unicode[CP_CENTRAL][0xE3 - 128] == 0x103 &&
              cp_unicode[CP_CENTRAL][0xBA - 128] == 0x15F);
        T("437: 0xC9 is a double box corner",
          cp_unicode[CP_DOS][0xC9 - 128] == 0x2554);
        T("1252: 0x81 is nothing",
          !font_has_char(0x81) && font_has_char(0x80) && font_has_char('A'));
        /* every byte of every code page: a glyph in every font */
        bool all = true;
        for (int cp = 0; cp < CP_COUNT; cp++)
            for (int c = 0; c < 128; c++)
                if (cp_unicode[cp][c] && cp_glyph[cp][c] == 0xFF)
                    all = false;
        T("every character has a glyph", all);
        /* text in each font, each code page */
        app_instance_t *a = sim_launch("TEDITOR");
        sim_frames(5);
        window *w = wm_focused_window();
        control *ed = NULL;
        for (control *c = w->controls; c; c = c->next)
            if (c->type == CTL_EDIT)
                ed = c;
        (void)a;
        /* Alt + 0233 on the number pad: byte 233 */
        kb_process_byte(0x38);
        static const uint8_t d0233[] = {0x52, 0x50, 0x51, 0x51};
        for (int i = 0; i < 4; i++) {
            kb_process_byte(d0233[i]);
            kb_process_byte((uint8_t)(d0233[i] | 0x80));
        }
        kb_process_byte(0xB8);
        sim_frame();
        sim_frames(2);
        const char *t = ctl_text(ed);
        T("Alt+0233 typed byte 233", ed && (unsigned char)t[0] == 233 && !t[1]);
        /* Alt + 65: 'A'; Alt + 300: byte 44 (mod 256), as DOS does */
        kb_process_byte(0x38);
        kb_process_byte(0x4D);
        kb_process_byte(0xCD); /* 6 */
        kb_process_byte(0x4C);
        kb_process_byte(0xCC); /* 5 */
        kb_process_byte(0xB8);
        sim_frame();
        T("Alt+65 typed A", ed && !strcmp(ctl_text(ed) + 1, "A"));
        static char line[600];
        int n = 0;
        for (int c = 128; c < 256; c++)
            if (font_has_char((unsigned char)c) || c >= 160)
                line[n++] = (char)c;
        line[n] = 0;
        static char full[1400];
        snprintf(full, sizeof full,
                 "%.48s\n%.48s\n%.48s\nCaf\xe9 na\xefve \xa9 \x80 \xbd", line,
                 line + 48, line + 96);
        ctl_set_text(ed, full);
        sim_frames(3);
        sim_shot("cp1252");
        g_codepage = CP_CENTRAL;
        wm_invalidate_all();
        ctl_set_text(
            ed,
            "Rom\xe2n\xe3: \xe3 \xe2 \xee \xba \xfe \xc3 \xc2 \xce \xaa \xde\n"
            "\x8a\x9a \x8e\x9e \xc8\xe8 \xa3\xb3 \xd5\xf5 \xd8\xf8");
        sim_frames(3);
        sim_shot("cp1250");
        g_codepage = CP_DOS;
        wm_invalidate_all();
        ctl_set_text(
            ed, "\xc9\xcd\xcd\xcd\xcb\xcd\xcd\xbb  \xda\xc4\xc4\xc2\xc4\xbf\n"
                "\xba abc\xba de\xba  \xb3ab\xb3c\xb3\n"
                "\xc8\xcd\xcd\xcd\xca\xcd\xcd\xbc  \xc0\xc4\xc4\xc1\xc4\xd9\n"
                "\xb0\xb1\xb2\xdb\xdf\xdc \xe0\xe1\xe3\xe4\xe6 \xf1\xf2\xf3 "
                "\xfb\xfd");
        sim_frames(3);
        sim_shot("cp437");
        /* the terminal font: the box drawing joins up */
        int tw = text_width("\xc4\xc4\xc4", FONT_TERM);
        T("terminal: box chars a cell each", tw == 3 * FONT_TERM_W);
        T("8x8: 8 a character", text_width("\xc4\xc4", FONT_MONO8) == 16);
        g_codepage = CP_WESTERN;
        wm_invalidate_all();
        wm_close(w);
        sim_frames(3);
        /* Character Map */
        app_instance_t *cm = sim_launch("CHARMAP");
        sim_frames(4);
        typedef struct {
            window *win;
            control *font, *cpl, *grid, *big, *text, *copy;
            int sel, shown_cp;
        } cm_t;
        cm_t *c = cm->state;
        T("Character Map: Western",
          !strcmp(ctl_text(c->cpl), "Western (1252)"));
        sim_shot("charmap1");
        /* 0xA9 (copyright): row (0xA9-32)/16, column (0xA9-32)%16 */
        int k = 0xA9 - 32;
        sim_dblclick(c->grid->ax + 2 + (k % 16) * 14 + 7,
                     c->grid->ay + 2 + (k / 16) * 9 + 4);
        sim_frames(2);
        T("double-click picks and gathers it",
          c->sel == 0xA9 && (unsigned char)ctl_text(c->text)[0] == 0xA9);
        sim_ext(0x4D);
        sim_frames(1); /* right */
        sim_key(0x1C);
        sim_frames(2); /* Enter: add */
        T("arrow, Enter: the next one",
          c->sel == 0xAA && strlen(ctl_text(c->text)) == 2);
        c->copy->handler(c->copy, EV_CLICK);
        T("Copy: on the clipboard",
          clipboard_has_text() && (unsigned char)g_clipboard.text[0] == 0xA9);
        sim_shot("charmap2");
        g_codepage = CP_DOS;
        sim_frames(3);
        T("follows the code page", !strcmp(ctl_text(c->cpl), "DOS (437)"));
        list_set_sel(c->font, 2);
        c->font->handler(c->font, EV_CHANGE);
        sim_frames(3);
        sim_shot("charmap3");
        g_codepage = CP_CENTRAL;
        list_set_sel(c->font, 0);
        c->font->handler(c->font, EV_CHANGE);
        sim_frames(3);
        sim_shot("charmap4");
        g_codepage = CP_WESTERN;
        wm_close(c->win);
        sim_frames(2);
        /* Settings > System > Characters: at once */
        sim_launch("Settings");
        sim_frames(3);
        window *sw = wm_focused_window();
        control *cpc = NULL;
        for (control *x = sw ? sw->controls : NULL; x; x = x->next) {
            if (x->type == CTL_TABS)
                ctl_set_value(x, 0); /* Display */
            if (x->type == CTL_COMBO && list_count(x) == 3 &&
                !strcmp(list_get(x, 2), "DOS (437)"))
                cpc = x;
        }
        sim_frames(2);
        T("Settings: the Characters choice", cpc != NULL);
        if (cpc) {
            list_set_sel(cpc, 1);
            cpc->handler(cpc, EV_CHANGE);
            sim_frames(2);
            T("picked: in use at once",
              g_codepage == CP_CENTRAL && g_cfg.codepage == CP_CENTRAL);
            sim_shot("cpsettings");
        }
        g_codepage = CP_WESTERN;
        g_cfg.codepage = CP_WESTERN;
        return fails;
    }
    if (!strcmp(name, "cdaudio")) {
        /* an audio CD: /CD0 with Track01.cda ..., Audio Player plays them
         * from the drive (play, pause, seek, next, the end of a track) */
        int fails = 0;
        char b[300];
        extern bool sim_cd_insert_audio(int, const int *);
        extern bool sim_cd_insert(const char *);
        extern void sim_cd_eject(void);
        extern int g_cd_plays;
        dir_entry_t de;
        fat_file_t f;
        static const int secs[3] = {200, 180, 240};
        sim_cd_insert_audio(3, secs);
        sim_frames(40);
        T("audio CD: /CD0", fs_drive_mounted(DRIVE_CD0) &&
                                !strcmp(fs_drive_label(DRIVE_CD0), "Audio CD"));
        T("its tracks as files", fs_find("/CD0/Track01.cda", &de) &&
                                     de.file_size == 44 &&
                                     fs_find("/CD0/Track03.cda", &de) &&
                                     !fs_find("/CD0/Track04.cda", &de));
        uint8_t c[64] = {0};
        int n = fs_open("/CD0/Track02.cda", &f) ? fs_read(&f, c, 64) : 0;
        fs_close(&f);
        uint32_t st2 =
            (uint32_t)(c[28] | c[29] << 8 | c[30] << 16 | c[31] << 24);
        uint32_t ln2 =
            (uint32_t)(c[32] | c[33] << 8 | c[34] << 16 | c[35] << 24);
        snprintf(b, sizeof b,
                 "    Track02.cda: %d bytes, track %d, start %u, %u sectors\n",
                 n, c[22], st2, ln2);
        wr(1, b);
        T("a Windows .cda (RIFF CDDA)",
          n == 44 && !memcmp(c, "RIFF", 4) && !memcmp(c + 8, "CDDAfmt ", 8) &&
              c[22] == 2 && st2 == 200 * 75 + 150 && ln2 == 180 * 75 + 150);
        T("no apps folder for it", !fs_find("/APPS/CDROM", &de));
        T("not writable", !fs_create("/CD0/X.TXT", &f));
        sim_frames(3);
        sim_shot("cda1");
        /* Audio Player: C plays the disc */
        fs_delete("/PREFS/AUDIOPLA.INI");
        app_instance_t *a = sim_launch("AUDIOPLA");
        sim_frames(3);
        typedef struct {
            window *win;
            control *viz, *bar, *play, *stop, *rew, *next, *loop, *vol,
                *vol_lbl, *list, *status, *cd;
            char paths[40][192];
            int count, cur;
        } ap_t;
        ap_t *s = a->state;
        s->cd->handler(s->cd, EV_CLICK);
        sim_frames(3);
        snprintf(b, sizeof b, "    %d in the list, %s / %s, %u ms\n", s->count,
                 audio_codec_name(), audio_info(), audio_len_ms());
        wr(1, b);
        T("the 3 tracks listed",
          s->count == 3 && !strcmp(s->paths[0], "/CD0/Track01.cda"));
        T("track 1 playing",
          audio_state() == AUDIO_PLAYING &&
              !strcmp(audio_codec_name(), "CD audio") &&
              !strcmp(audio_info(), "track 1, from the drive") &&
              audio_len_ms() == 202000);
        T("shown as CD track 1", !strcmp(list_get(s->list, 0), "> CD track 1"));
        sim_frames(190); /* ~3 s */
        uint32_t p = audio_pos_ms();
        snprintf(b, sizeof b, "    at %u ms\n", p);
        wr(1, b);
        T("the position moves", p >= 2700 && p <= 3300);
        sim_shot("cda2");
        sim_key(0x39);
        sim_frames(2); /* Space: pause */
        uint32_t p0 = audio_pos_ms();
        sim_frames(60);
        T("paused: the drive holds",
          audio_state() == AUDIO_PAUSED && audio_pos_ms() - p0 < 300);
        audio_seek_ms(100000);
        sim_frames(20);
        p = audio_pos_ms();
        T("seek to 100 s (stays paused)",
          audio_state() == AUDIO_PAUSED && p >= 99900 && p <= 100300);
        sim_key(0x39);
        sim_frames(2);
        T("on again", audio_state() == AUDIO_PLAYING);
        /* near the end: on to track 2 by itself */
        audio_seek_ms(201000);
        int plays = g_cd_plays;
        sim_frames(130);
        snprintf(b, sizeof b, "    now %s, cur %d, %d plays\n", audio_info(),
                 s->cur, g_cd_plays - plays);
        wr(1, b);
        T("track 1 ran out: track 2 plays",
          s->cur == 1 && !strcmp(audio_info(), "track 2, from the drive") &&
              audio_state() == AUDIO_PLAYING);
        sim_key2(0x31, 'n', false, false);
        sim_frames(3); /* N: next */
        T("N: track 3",
          s->cur == 2 && !strcmp(audio_info(), "track 3, from the drive"));
        sim_shot("cda3");
        /* a .cda copied to the hard disk plays while its disc is in */
        static char o[512];
        extern int cli_execute_command(const char *, char *, size_t);
        (void)o;
        (void)cli_execute_command;
        fs_delete("/T2.CDA");
        T("copied to the hard disk",
          fs_create("/T2.CDA", &f) && fs_write(&f, c, 44) == 44);
        fs_close(&f);
        T("open the copy",
          audio_open("/T2.CDA") == NULL &&
              !strcmp(audio_info(), "track 2, from the drive"));
        audio_close();
        /* taken out: the player stops, the tracks can't be opened */
        appload_run_named("Audio Player", "/CD0/Track01.cda");
        sim_frames(3);
        sim_cd_eject();
        sim_frames(60);
        const char *e = audio_open("/T2.CDA");
        snprintf(b, sizeof b, "    no disc: '%s', state %d\n", e ? e : "-",
                 audio_state());
        wr(1, b);
        T("no disc: the copy says so", e && strstr(e, "audio CD"));
        T("no disc: /CD0 gone", !fs_drive_mounted(DRIVE_CD0));
        /* a data CD has no tracks */
        sim_cd_insert(FIXDIR "cd.iso");
        sim_frames(40);
        s->cd->handler(s->cd, EV_CLICK);
        sim_frames(2);
        T("data CD: says no audio",
          strstr(ctl_text(s->status), "no audio") != NULL);
        T("the old tracks went from the list", s->count == 0);
        sim_shot("cda4");
        return fails;
    }
    if (!strcmp(name, "submenu")) {
        int fails = 0;
        char b[300];
        extern void wm_focus(window *);
        static uint8_t buf[16384];
        uint32_t n = 0;
        fat_file_t f;
        if (load_file(FIXDIR "lz/SUDOKU.APP", buf, sizeof buf, &n)) {
            fs_mkdir("/APPS");
        }
        sim_frames(3);
        menu *apps = NULL;
        int ai = -1;
        for (int i = 0; i < g_menubar.menu_count; i++)
            if (!strcmp(g_menubar.menus[i].title, "Apps")) {
                apps = &g_menubar.menus[i];
                ai = i;
            }
        T("Apps menu on the bar", apps != NULL);
        snprintf(b, sizeof b, "Apps items: %d\n", apps->item_count);
        wr(1, b);
        T("groups first",
          apps->items[0].sub && !strcmp(apps->items[0].label, "Accessories"));
        sim_click(apps->bar_x + 4, 4);
        sim_frames(2);
        T("open", g_menubar.open_index == ai);
        sim_move(apps->bar_x + 10, MENUBAR_H + 3);
        sim_frames(2); /* hover Accessories */
        T("hover opens the submenu", g_menubar.sub_item == 0);
        sim_shot("sm1");
        menu *acc = g_menubar.menus[ai].items[0].sub;
        T("Accessories has 7", acc->item_count == 7);
        /* diagonally: right and down across the Games row, quickly */
        for (int k = 1; k <= 10; k++) {
            sim_move(apps->bar_x + 10 + k * 11, MENUBAR_H + 3 + k);
            sim_frame();
        }
        T("brushing Games on the way keeps Accessories",
          g_menubar.sub_item == 0);
        sim_move(apps->bar_x + 120, MENUBAR_H + 10 + 4);
        sim_frames(2);
        T("still open while inside it", g_menubar.sub_item == 0);
        sim_shot("sm2");
        mouse.left = true;
        sim_frame();
        mouse.left = false;
        sim_frames(6);
        window *w = wm_focused_window();
        snprintf(b, sizeof b, "launched: %s\n", w ? w->title : "-");
        wr(1, b);
        T("Calculator launched from the submenu (2nd, sorted)",
          w && strstr(w->title, "Calc"));
        T("menu closed", g_menubar.open_index < 0);
        /* a .APP in /APPS/GAMES joins Games */
        fs_mkdir("/APPS/GAMES");
        fs_delete("/APPS/GAMES/SUDOKU.APP");
        fs_create("/APPS/GAMES/SUDOKU.APP", &f);
        fs_write(&f, buf, n);
        fs_close(&f);
        wm_focus(win_stack[0]);
        sim_frames(70);
        for (int i = 0; i < g_menubar.menu_count; i++)
            if (!strcmp(g_menubar.menus[i].title, "Apps")) {
                apps = &g_menubar.menus[i];
                ai = i;
            }
        T("Games gets Sudoku from /APPS/GAMES",
          apps->items[1].sub->item_count == 2);
        sim_click(apps->bar_x + 4, 4);
        sim_frames(1);
        sim_move(apps->bar_x + 10, MENUBAR_H + 1 * 10 + 4);
        sim_frames(2);
        sim_shot("sm3");
        sim_move(apps->bar_x + 120, MENUBAR_H + 1 * 10 + 4);
        sim_frames(1);
        sim_move(apps->bar_x + 120, MENUBAR_H + 2 * 10 + 4);
        sim_frames(1);
        mouse.left = true;
        sim_frame();
        mouse.left = false;
        sim_frames(8);
        w = wm_focused_window();
        T("Sudoku launched from Games", w && !strcmp(w->title, "Sudoku"));
        /* a click on a group item keeps the menu open */
        wm_focus(win_stack[0]);
        sim_frames(2);
        sim_click(apps->bar_x + 4, 4);
        sim_frames(1);
        sim_click(apps->bar_x + 10, MENUBAR_H + 1 * 10 + 4);
        sim_frames(1);
        T("click on a group: stays open",
          g_menubar.open_index >= 0 && g_menubar.sub_item == 1);
        sim_click(5, 300);
        sim_frames(2);
        T("click outside closes", g_menubar.open_index < 0);
        return fails;
    }
    if (!strcmp(name, "appsub")) {
        int fails = 0;
        char b[300];
        static uint8_t buf[16384];
        uint32_t n = 0;
        fat_file_t f;
        if (!load_file(FIXDIR "lz/SUDOKU.APP", buf, sizeof buf, &n))
            return 1;
        fs_delete("/PREFS/SUDOKU.INI");
        fs_delete("/SUDOKU.APP");
        fs_create("/SUDOKU.APP", &f);
        fs_write(&f, buf, n);
        fs_close(&f);
        appload_launch("/SUDOKU.APP");
        sim_frames(6);
        window *w = wm_focused_window();
        typedef struct {
            window *win;
            const void *api;
            control *view, *status;
            uint8_t sol[81], given[81], cur[81];
            uint16_t notes[81];
            uint8_t wrong[81];
            int sel, level;
        } su_t;
        su_t *s = w->dyn_state;
        T("easy at first", s->level == 0);
        menu *g = NULL;
        int gi = -1;
        for (int i = 0; i < g_menubar.menu_count; i++)
            if (!strcmp(g_menubar.menus[i].title, "Game")) {
                g = &g_menubar.menus[i];
                gi = i;
            }
        T("Game menu, first item a submenu",
          g && g->items[0].sub && !strcmp(g->items[0].label, "New Game") &&
              g->items[0].sub->item_count == 3);
        sim_click(g->bar_x + 4, 4);
        sim_frames(1);
        sim_move(g->bar_x + 10, MENUBAR_H + 4);
        sim_frames(2);
        T("hover opens it",
          g_menubar.open_index == gi && g_menubar.sub_item == 0);
        sim_shot("as1");
        sim_move(g->bar_x + 100, MENUBAR_H + 4);
        sim_frames(1);
        sim_move(g->bar_x + 100, MENUBAR_H + 2 * 10 + 4);
        sim_frames(2);
        sim_shot("as2");
        mouse.left = true;
        sim_frame();
        mouse.left = false;
        sim_frames(4);
        T("Hard chosen from the submenu", s->level == 2);
        /* refusals */
        extern bool popup_is_menu(const menu *);
        extern menu *popup_menu(void);
        T("no submenu in a submenu",
          menu_add_submenu(w->win_menus[0].items[0].sub, "x") == NULL);
        return fails;
    }
    if (!strcmp(name, "menupage") || !strcmp(name, "menupage320")) {
        int fails = 0;
        char b[300];
        if (name[8]) {
            extern int g_screen_width, g_screen_height, g_video_mode;
            g_screen_width = 320;
            g_screen_height = 200;
            g_video_mode = 0;
        }
        static window_spec_t spec = {.x = 40,
                                     .y = 30,
                                     .w = 200,
                                     .h = 100,
                                     .title = "Many Menus",
                                     .visible = true};
        window *w = wm_register(&spec);
        static const char *const T14[14] = {
            "File",    "Edit",     "View",      "Insert",      "Formatting",
            "Tools",   "Tables",   "Windows",   "Help Topics", "Extras",
            "Options", "Plug-ins", "Debugging", "Last One"};
        static char lab[16][12];
        static char tt[14][24];
        for (int i = 0; i < 14; i++) {
            snprintf(tt[i], 24, name[8] ? "%s" : "%s Menu", T14[i]);
            menu *m = window_add_menu(w, tt[i]);
            int n = i == 0 ? 17 : 3;
            for (int k = 0; k < n; k++) {
                snprintf(lab[k], 12, "Item %d", k + 1);
                menu_add_item(m, lab[k], NULL);
            }
        }
        T("16 items fit (17th refused)", w->win_menus[0].item_count == 16);
        wm_focus(w);
        sim_frames(3);
        T("all 14 on the bar (+ Apps)", g_menubar.menu_count == 15);
        int shown = 0, last = -1;
        for (int i = 0; i < 14; i++)
            if (g_menubar.menus[i].bar_w) {
                shown++;
                last = i;
            }
        snprintf(b, sizeof b, "page 1: %d shown (0..%d), >> at %d\n", shown,
                 last, g_menubar.next_x);
        wr(1, b);
        T("page 1: some shown, >> there, no <<",
          shown < 14 && g_menubar.next_x > 0 && g_menubar.prev_x < 0);
        sim_shot(name[8] ? "mp4" : "mp1");
        sim_click(g_menubar.menus[0].bar_x + 4, 4);
        sim_frames(2);
        sim_shot(name[8] ? "mp5" : "mp2");
        sim_click(g_menubar.next_x + 5, 4);
        sim_frames(2);
        T("menu closed by >>", g_menubar.open_index < 0);
        T("page 2 starts after the last shown",
          g_menubar.first == last + 1 && g_menubar.prev_x > 0);
        int a = -1;
        for (int i = 0; i < 14; i++)
            if (g_menubar.menus[i].bar_w) {
                a = i;
                break;
            }
        sim_click(g_menubar.menus[a].bar_x + 4, 4);
        sim_frames(2);
        T("a page-2 menu opens", g_menubar.open_index == a);
        sim_shot(name[8] ? "mp6" : "mp3");
        sim_click(5, 150);
        sim_frames(1);
        int pages = 2;
        while (g_menubar.next_x > 0 && pages < 6) {
            sim_click(g_menubar.next_x + 5, 4);
            sim_frames(1);
            pages++;
        }
        T("the last page shows Last",
          g_menubar.menus[13].bar_w > 0 && g_menubar.next_x < 0);
        snprintf(b, sizeof b, "pages: %d\n", pages);
        wr(1, b);
        for (int k = 0; k < pages - 1; k++) {
            sim_click(g_menubar.prev_x + 5, 4);
            sim_frames(1);
        }
        T("<< back to page 1", g_menubar.first == 0 && g_menubar.prev_x < 0);
        sim_click(g_menubar.next_x + 5, 4);
        sim_frames(1);
        wm_focus(win_stack[0]);
        sim_frames(2);
        wm_focus(w);
        sim_frames(2);
        T("another window and back: page 1", g_menubar.first == 0);
        return fails;
    }
    if (!strcmp(name, "appsdir")) {
        int fails = 0;
        char b[300];
        static uint8_t su[16384], lz[8192];
        uint32_t ns = 0, nl = 0;
        fat_file_t f;
        if (!load_file(FIXDIR "lz/SUDOKU.APP", su, sizeof su, &ns) ||
            !load_file(FIXDIR "lz/LAZY.APP", lz, sizeof lz, &nl))
            return 1;
#define PUT(p, d, n)                                                           \
    do {                                                                       \
        fs_delete(p);                                                          \
        fs_create(p, &f);                                                      \
        fs_write(&f, d, n);                                                    \
        fs_close(&f);                                                          \
    } while (0)
        fs_mkdir("/APPS");
        fs_mkdir("/APPS/GAMES");
        fs_mkdir("/APPS/ACME");
        fs_mkdir("/APPS/EMPTY");
        PUT("/APPS/LAZY.APP", lz, nl);
        PUT("/APPS/GAMES/SUDOKU.APP", su, ns);
        PUT("/APPS/ACME/SUDOKU.APP", su, ns);
        PUT("/APPS/ACME/LAZY.APP", lz, nl);
        PUT("/APPS/ACME/GROUP.TXT", "Acme Software\r\nrest", 19);
        PUT("/APPS/ACME/JUNK.APP", "not an app at all", 17);
        {
            static char o[256];
            extern int cli_execute_command(const char *, char *, size_t);
            o[0] = 0;
            cli_execute_command("write /APPS/ACME/CALC.LNK Calculator", o,
                                sizeof o);
            wr(1, o);
        }
        PUT("/APPS/ACME/MYGAME.LNK", "/APPS/GAMES/SUDOKU.APP\r\nMy Sudoku\r\n",
            34);
        PUT("/APPS/ACME/BAD.LNK", "Nothing here", 12);
        sim_frames(70); /* the menu follows within a second */
        menu *apps = NULL;
        for (int i = 0; i < g_menubar.menu_count; i++)
            if (!strcmp(g_menubar.menus[i].title, "Apps"))
                apps = &g_menubar.menus[i];
        for (int i = 0; i < apps->item_count; i++) {
            const menu_item *it = &apps->items[i];
            snprintf(b, sizeof b, "  %s%s\n", it->label ? it->label : "---",
                     it->sub ? "  >" : "");
            wr(1, b);
            if (it->sub)
                for (int k = 0; k < it->sub->item_count; k++) {
                    snprintf(b, sizeof b, "      %s\n",
                             it->sub->items[k].label);
                    wr(1, b);
                }
        }
#define GRP(nm)                                                                \
    ({                                                                         \
        menu *g_ = NULL;                                                       \
        for (int i_ = 0; i_ < apps->item_count; i_++)                          \
            if (apps->items[i_].label && !strcmp(apps->items[i_].label, nm))   \
                g_ = apps->items[i_].sub ? apps->items[i_].sub : (menu *)1;    \
        g_;                                                                    \
    })
#define HAS(m, nm)                                                             \
    ({                                                                         \
        bool h_ = false;                                                       \
        for (int k_ = 0; m && k_ < (m)->item_count; k_++)                      \
            h_ |= (m)->items[k_].label && !strcmp((m)->items[k_].label, nm);   \
        h_;                                                                    \
    })
        menu *games = GRP("Games"), *acme = GRP("Acme Software"),
             *acc = GRP("Accessories"), *sys = GRP("System Tools");
        T("groups sorted: Accessories first",
          !strcmp(apps->items[0].label, "Accessories"));
        T("Accessories: bundled .APPs + TEditor shortcut",
          acc && acc->item_count == 7 && HAS(acc, "Calculator") &&
              HAS(acc, "TEditor") && HAS(acc, "ASMDraw") &&
              HAS(acc, "Character Map"));
        T("System Tools: HexView .APP + built-in shortcuts",
          sys && HAS(sys, "HexView") && HAS(sys, "Monitor") &&
              HAS(sys, "Print Queue"));
        T("Games has Puzzle + Sudoku", games && games->item_count == 2 &&
                                           HAS(games, "Puzzle") &&
                                           HAS(games, "Sudoku"));
        T("Acme: 2 apps + 2 shortcuts, junk and the bad shortcut left out",
          acme && acme->item_count == 4);
        T("Calculator .APP by shortcut name; My Sudoku label from line 2",
          HAS(acme, "Calculator") && HAS(acme, "My Sudoku"));
        {
            extern void fo_open(window *, const char *);
            fo_open(NULL, "/APPS/ACME/CALC.LNK");
            sim_frames(6);
            window *cw = wm_focused_window();
            T("opening CALC.LNK runs Calculator",
              cw && strstr(cw->title, "Calc"));
            wm_close(cw);
            sim_frames(4);
            wm_focus(win_stack[0]);
            sim_frames(2);
        }
        T("Empty group is empty",
          GRP("Empty") && GRP("Empty")->item_count == 0);
        T("root items: FileF, Lazy, Settings",
          HAS(apps, "FileF") && HAS(apps, "Lazy") && HAS(apps, "Settings"));
        /* launch from the Acme group */
        int ai = 0;
        for (int i = 0; i < apps->item_count; i++)
            if (apps->items[i].sub == acme)
                ai = i;
        sim_click(apps->bar_x + 4, 4);
        sim_frames(1);
        sim_move(apps->bar_x + 10, MENUBAR_H + ai * 10 + 4);
        sim_frames(2);
        sim_shot("ad1");
        sim_move(apps->bar_x + 120, MENUBAR_H + ai * 10 + 4);
        sim_frames(1);
        mouse.left = true;
        sim_frame();
        mouse.left = false;
        sim_frames(8);
        window *w = wm_focused_window();
        snprintf(b, sizeof b, "launched %s\n", w ? w->title : "-");
        wr(1, b);
        T("an Acme app runs",
          w && (strstr(w->title, "Calc") || strstr(w->title, "Lazy") ||
                strstr(w->title, "Sudoku")));
        /* take the group away: delete its files and the folder */
        fs_delete("/APPS/ACME/SUDOKU.APP");
        fs_delete("/APPS/ACME/LAZY.APP");
        fs_delete("/APPS/ACME/GROUP.TXT");
        fs_delete("/APPS/ACME/JUNK.APP");
        fs_delete("/APPS/ACME/CALC.LNK");
        fs_delete("/APPS/ACME/MYGAME.LNK");
        fs_delete("/APPS/ACME/BAD.LNK");
        fs_rmdir("/APPS/ACME");
        wm_focus(win_stack[0]);
        sim_frames(70);
        for (int i = 0; i < g_menubar.menu_count; i++)
            if (!strcmp(g_menubar.menus[i].title, "Apps"))
                apps = &g_menubar.menus[i];
        T("gone from the menu", !GRP("Acme Software"));
        /* copy one in: it appears */
        PUT("/APPS/Multimedia/SUDOKU.APP", su, ns);
        PUT("/APPS/Multimedia/SUDOKU.APP", su, ns);
        sim_frames(70);
        for (int i = 0; i < g_menubar.menu_count; i++)
            if (!strcmp(g_menubar.menus[i].title, "Apps"))
                apps = &g_menubar.menus[i];
        {
            menu *mm = GRP("Multimedia");
            T("copied into Multimedia",
              mm && mm != (menu *)1 && mm->item_count == 2);
        }
        /* /APPS deleted: made again at boot, with the shortcuts */
        fs_rm_rf("/APPS");
        extern void apps_dir_defaults_pub(void);
        apps_dir_defaults_pub();
        T("defaults: Accessories/TEditor.LNK",
          fs_find("/APPS/Accessories/TEditor.LNK", NULL) || ({
              dir_entry_t d_;
              fs_find("/APPS/Accessories/TEditor.LNK", &d_);
          }));
        T("defaults: FileF.LNK at the top", ({
              dir_entry_t d_;
              fs_find("/APPS/FileF.LNK", &d_);
          }));
        return fails;
    }
    if (!strcmp(name, "floppy2")) {
        int fails = 0;
        char b[300];
        extern int cli_execute_command(const char *, char *, size_t);
        extern uint8_t g_fdc_img[];
        extern int g_fdc_drive_type, g_fdc_disk_kb, g_fdc_max_pcn,
            g_fdc_formats;
        extern void fdc_insert(int);
        extern void fdc_setup(void);
        extern void sim_save(const char *, const void *, unsigned);
        static char o[1024];
        static const struct {
            int drive, kb;
        } C[] = {{4, 1440}, {4, 720}, {2, 1200}, {2, 360}, {2, 320},
                 {2, 180},  {2, 160}, {1, 360},  {1, 320}, {1, 180},
                 {1, 160},  {3, 720}, {5, 2880}, {5, 1440}};
        for (unsigned k = 0; k < sizeof C / sizeof C[0]; k++) {
            g_fdc_drive_type = C[k].drive;
            fdc_setup();
            fdd_eject(DRIVE_FDD0);
            fdd_init(0);
            memset(g_fdc_img, 0, 2880 * 512);
            fdc_insert(C[k].kb == 1440 ? 720 : 1440); /* some other disk */
            g_fdc_max_pcn = 0;
            g_fdc_formats = 0;
            char cmd[64];
            snprintf(cmd, sizeof cmd, "format a: %d T%d", C[k].kb, C[k].kb);
            o[0] = 0;
            cli_execute_command(cmd, o, sizeof o);
            bool fmt_ok = strstr(o, "Formatted") != NULL;
            /* take it out and put it back: the size must be found again */
            fdd_eject(DRIVE_FDD0);
            bool mounted = fdd_insert(DRIVE_FDD0);
            int media = fdd_media(0);
            fat_file_t f;
            static char data[5000], back[5100];
            int n = 0;
            for (int i = 0; i < 5000; i++)
                data[i] = (char)('A' + (i * 7 + k) % 26);
            bool wr_ok = fs_create("/FDD0/TEST.TXT", &f) &&
                         fs_write(&f, data, 5000) == 5000;
            fs_close(&f);
            fdd_eject(DRIVE_FDD0);
            fdd_insert(DRIVE_FDD0);
            if (fs_open("/FDD0/TEST.TXT", &f)) {
                n = fs_read(&f, back, 5100);
                fs_close(&f);
            }
            bool rd_ok = n == 5000 && !memcmp(data, back, 5000);
            snprintf(b, sizeof b,
                     "drive %d, %4d K: format %s, found %-7s, file %s, "
                     "formatted tracks %3d, head up to %2d",
                     C[k].drive, C[k].kb, fmt_ok ? "ok" : "FAILED",
                     media ? FDD_FORMATS[media].name : "none",
                     rd_ok ? "ok" : "BAD", g_fdc_formats, g_fdc_max_pcn);
            bool good = fmt_ok && mounted && wr_ok && rd_ok &&
                        FDD_FORMATS[media].kb == C[k].kb;
            if (C[k].drive == 2 && C[k].kb <= 360)
                good &= g_fdc_max_pcn == 78; /* double stepped */
            wr(1, b);
            wr(1, "\n");
            T("  that one", good);
            if (!good) {
                wr(1, o);
            }
        }
        /* a DOS 1.0 disk: no BPB */
        g_fdc_drive_type = 1;
        fdc_setup();
        fdd_eject(DRIVE_FDD0);
        memset(g_fdc_img, 0, 2880 * 512);
        fdc_insert(1440);
        o[0] = 0;
        cli_execute_command("format a: 160", o, sizeof o);
        fat_file_t f;
        fs_create("/FDD0/OLD.TXT", &f);
        fs_write(&f, "DOS 1.0", 7);
        fs_close(&f);
        fdd_eject(DRIVE_FDD0);
        memset(g_fdc_img + 11, 0,
               51); /* wipe the BPB, as DOS 1.0 never wrote one */
        bool m = fdd_insert(DRIVE_FDD0);
        char t[8] = {0};
        if (fs_open("/FDD0/OLD.TXT", &f)) {
            fs_read(&f, t, 7);
            fs_close(&f);
        }
        T("DOS 1.0 disk (no BPB): found by the media byte, file read",
          m && !strcmp(t, "DOS 1.0") && fdd_media(0) == FDD_MEDIA_160K);
        /* an HD disk in a 360K drive can't be read */
        memset(g_fdc_img, 0, 2880 * 512);
        fdc_insert(1200);
        fdd_eject(DRIVE_FDD0);
        T("1.2M disk in a 360K drive: refused", !fdd_insert(DRIVE_FDD0));
        /* no disk: no hang */
        g_fdc_drive_type = 4;
        fdc_setup();
        fdd_eject(DRIVE_FDD0);
        g_fdc_disk_kb = 0;
        T("no disk: insert fails", !fdd_insert(DRIVE_FDD0));
        return fails;
    }
    if (!strcmp(name, "lfn")) {
        int fails = 0;
        char b[300];
        extern uint8_t g_fdc_img[];
        extern int g_fdc_drive_type;
        extern void fdc_insert(int), fdc_setup(void);
        extern void sim_save(const char *, const void *, unsigned);
        static uint8_t im[1474560];
        uint32_t n = 0;
        if (!load_file(FIXDIR "lfn/win.img", im, sizeof im, &n))
            return 1;
        g_fdc_drive_type = 4;
        fdc_setup();
        fdd_eject(DRIVE_FDD0);
        fdc_insert(1440);
        memcpy(g_fdc_img, im, n);
        T("Windows disk mounts", fdd_insert(DRIVE_FDD0));
        static char o[1024];
        extern int cli_execute_command(const char *, char *, size_t);
        o[0] = 0;
        cli_execute_command("ls /FDD0", o, sizeof o);
        wr(1, o);
        T("ls shows the long names",
          strstr(o, "Long File Name.txt") && !strstr(o, "LONGFI~1"));
        T("delete", fs_delete("/FDD0/LONGFI~1.TXT"));
        T("rename", fs_rename("/FDD0/AVERYL~1.TXT", "SHORT.TXT"));
        T("rm -r the folder", fs_rm_rf("/FDD0/MYDOCU~1"));
        fat_file_t f;
        char t[16] = {0};
        if (fs_open("/FDD0/STAYSH~1.TXT", &f)) {
            fs_read(&f, t, 9);
            fs_close(&f);
        }
        T("the other long-named file is fine", !strcmp(t, "also kept"));
        fdd_eject(DRIVE_FDD0);
        sim_save(OUTDIR "after.img", g_fdc_img, 1474560);
        return fails;
    }
    if (!strcmp(name, "lfn2")) {
        int fails = 0;
        char b[300];
        extern uint8_t g_fdc_img[];
        extern int g_fdc_drive_type;
        extern void fdc_insert(int), fdc_setup(void);
        extern void sim_save(const char *, const void *, unsigned);
        static uint8_t im[1474560];
        uint32_t n = 0;
        if (!load_file(FIXDIR "lfn/win2.img", im, sizeof im, &n))
            return 1;
        g_fdc_drive_type = 4;
        fdc_setup();
        fdd_eject(DRIVE_FDD0);
        fdc_insert(1440);
        memcpy(g_fdc_img, im, n);
        T("Windows disk mounts", fdd_insert(DRIVE_FDD0));
        static dir_entry_t de[64];
        static char nm[64][FS_NAME_SZ];
        int cnt = 0;
        uint8_t dr;
        uint16_t cl;
        fs_resolve_dir("/FDD0", &dr, &cl);
        fs_list_dir_names(dr, cl, de, nm, 64, &cnt);
        for (int i = 0; i < cnt; i++) {
            wr(1, "    ");
            wr(1, nm[i]);
            wr(1, "\n");
        }
        bool seen = false, seen2 = false, seen3 = false, seen4 = false;
        for (int i = 0; i < cnt; i++) {
            if (!strcmp(nm[i], "Long File Name.txt"))
                seen = true;
            if (!strcmp(nm[i], "My Documents"))
                seen2 = true;
            if (!strcmp(nm[i], "THISIS~1.TXT"))
                seen3 = true; /* > 63: its alias */
            if (!strcmp(nm[i], "notes.txt"))
                seen4 = true; /* NT lower-case flags */
        }
        T("long names listed", seen && seen2);
        T("a name over 63 chars shows its alias", seen3);
        T("NT lower-case 8.3 shown lower", seen4);
        fat_file_t f;
        char t[64] = {0};
        T("open by long name", fs_open("/FDD0/Long File Name.txt", &f) &&
                                   fs_read(&f, t, 8) == 8 &&
                                   !memcmp(t, "long one", 8));
        fs_close(&f);
        T("open by long name, any case",
          fs_open("/FDD0/long FILE name.TXT", &f));
        fs_close(&f);
        T("open by alias", fs_open("/FDD0/LONGFI~1.TXT", &f));
        fs_close(&f);
        T("open inside a long-named folder",
          fs_open("/FDD0/My Documents/Inner Long Name.txt", &f));
        fs_close(&f);
        T("a truncated name doesn't match", !fs_open("/FDD0/Long File", &f));
        T("cd into it", fs_chdir("/FDD0/My Documents") &&
                            !strcmp(dir_context.path, "/FDD0/My Documents"));
        T("relative open", fs_open("Inner Long Name.txt", &f));
        fs_close(&f);
        fs_chdir("/");
        /* create */
        T("create a long name", fs_create("/FDD0/My New Report 2026.txt", &f) &&
                                    fs_write(&f, "hello long", 10) == 10 &&
                                    fs_close(&f));
        dir_entry_t e;
        T("its alias is MYNEWR~1.TXT",
          fs_find("/FDD0/MYNEWR~1.TXT", &e) && e.file_size == 10);
        T("second one gets ~2", fs_create("/FDD0/My New Report 2027.txt", &f) &&
                                    fs_close(&f) &&
                                    fs_find("/FDD0/MYNEWR~2.TXT", &e));
        T("same name again refused",
          !fs_create("/FDD0/my new report 2026.TXT", &f));
        T("lower-case 8.3: no long name, shown lower",
          fs_create("/FDD0/readme.txt", &f) && fs_close(&f) &&
              fs_name_of("/FDD0/README.TXT", t) && !strcmp(t, "readme.txt"));
        T("mixed case 8.3 keeps its case",
          fs_create("/FDD0/ReadMe2.Txt", &f) && fs_close(&f) &&
              fs_name_of("/FDD0/readme2.txt", t) && !strcmp(t, "ReadMe2.Txt"));
        char n63[64];
        memset(n63, 'a', 59);
        memcpy(n63 + 59, ".txt", 5);
        T("63 chars ok",
          (snprintf(b, sizeof b, "/FDD0/%s", n63), fs_create(b, &f)) &&
              fs_close(&f) && fs_name_of(b, t) && !strcmp(t, n63));
        snprintf(b, sizeof b, "/FDD0/b%s", n63);
        T("64 chars refused", !fs_create(b, &f));
        T("bad char refused", !fs_create("/FDD0/what?.txt", &f));
        /* folders */
        T("mkdir a long name", fs_mkdir("/FDD0/Project_Files"));
        T("file in it",
          fs_create("/FDD0/Project_Files/Budget_for_next_year.txt", &f) &&
              fs_write(&f, "money", 5) == 5 && fs_close(&f));
        int made = 0;
        for (int i = 0; i < 40; i++) {
            snprintf(b, sizeof b,
                     "/FDD0/Project_Files/Chapter number %02d of the book.txt",
                     i);
            if (fs_create(b, &f)) {
                fs_close(&f);
                made++;
            }
        }
        T("40 long names in a folder (it grows)", made == 40);
        T("last one found",
          fs_find("/FDD0/Project_Files/Chapter number 39 of the book.txt", &e));
        /* rename */
        T("rename long -> long",
          fs_rename("/FDD0/My New Report 2026.txt", "Final Report.txt") &&
              fs_open("/FDD0/Final Report.txt", &f) &&
              fs_read(&f, t, 10) == 10 && !memcmp(t, "hello long", 10));
        fs_close(&f);
        T("old name gone", !fs_find("/FDD0/My New Report 2026.txt", &e) &&
                               !fs_find("/FDD0/MYNEWR~1.TXT", &e));
        T("rename long -> 8.3",
          fs_rename("/FDD0/Final Report.txt", "FINAL.TXT") &&
              fs_find("/FDD0/FINAL.TXT", &e));
        T("rename 8.3 -> long",
          fs_rename("/FDD0/KEEP.TXT", "Keep This One.txt") &&
              fs_find("/FDD0/Keep This One.txt", &e));
        T("case-only rename",
          fs_rename("/FDD0/Keep This One.txt", "keep this one.TXT") &&
              fs_name_of("/FDD0/KEEP THIS ONE.txt", t) &&
              !strcmp(t, "keep this one.TXT"));
        T("rename onto another name refused",
          !fs_rename("/FDD0/FINAL.TXT", "Stays Here.txt"));
        T("rename a folder",
          fs_rename("/FDD0/Project_Files", "Old Projects") &&
              fs_find("/FDD0/Old Projects/Budget_for_next_year.txt", &e));
        /* copy / move */
        T("copy a long-named folder",
          fs_copy_dir("/FDD0/My Documents", "/FDD0/Copy of My Documents") &&
              fs_open("/FDD0/Copy of My Documents/Inner Long Name.txt", &f) &&
              fs_read(&f, t, 5) == 5 && !memcmp(t, "inner", 5));
        fs_close(&f);
        T("copy a file",
          fs_copy_file("/FDD0/Long File Name.txt",
                       "/FDD0/Old Projects/Copied Long File.txt"));
        T("copy onto an existing name refused",
          !fs_copy_file("/FDD0/Long File Name.txt", "/FDD0/Stays Here.txt"));
        T("move a file into a folder",
          fs_move_file("/FDD0/Stays Here.txt",
                       "/FDD0/My Documents/Moved Here.txt") &&
              fs_find("/FDD0/My Documents/Moved Here.txt", &e) &&
              !fs_find("/FDD0/Stays Here.txt", &e));
        T("move a folder",
          fs_move_dir("/FDD0/Copy of My Documents",
                      "/FDD0/Old Projects/Docs Backup") &&
              fs_find("/FDD0/Old Projects/Docs Backup/Inner Long Name.txt",
                      &e));
        T("not into itself", !fs_move_dir("/FDD0/Old Projects",
                                          "/FDD0/Old Projects/Docs Backup/x"));
        T("cd .. from the moved folder",
          fs_chdir("/FDD0/Old Projects/Docs Backup") && fs_chdir("..") &&
              fs_find("Budget_for_next_year.txt", &e));
        fs_chdir("/");
        T("delete", fs_delete("/FDD0/My New Report 2027.txt") &&
                        !fs_find("/FDD0/MYNEWR~2.TXT", &e));
        T("copy to the hard disk",
          fs_copy_dir("/FDD0/Old Projects", "/Long Folder On HDA"));
        T("and there",
          fs_find("/Long Folder On HDA/Docs Backup/Inner Long Name.txt", &e));
        T("rm -r it", fs_rm_rf("/Long Folder On HDA") &&
                          !fs_find("/Long Folder On HDA", &e));
        /* the root fills up: refused, not corrupted */
        int roots = 0;
        for (int i = 0; i < 120; i++) {
            snprintf(b, sizeof b, "/FDD0/Filling up the root folder %03d.txt",
                     i);
            if (!fs_create(b, &f))
                break;
            fs_close(&f);
            roots++;
        }
        snprintf(b, sizeof b, "%d made before the root was full", roots);
        wr(1, "    ");
        wr(1, b);
        wr(1, "\n");
        T("root full: refused cleanly", roots > 30 && roots < 120);
        {
            int miss = -1;
            for (int i = 0; i < roots; i++) {
                snprintf(b, sizeof b,
                         "/FDD0/Filling up the root folder %03d.txt", i);
                if (!fs_find(b, &e)) {
                    miss = i;
                    break;
                }
            }
            snprintf(b, sizeof b, "    first missing: %d\n", miss);
            wr(1, b);
            T("every one made is there", miss < 0);
        }
        T("still reads fine", fs_open("/FDD0/Long File Name.txt", &f));
        fs_close(&f);
        fdd_eject(DRIVE_FDD0);
        sim_save(OUTDIR "after2.img", g_fdc_img, 1474560);
        return fails;
    }
    if (!strcmp(name, "lfnui")) {
        int fails = 0;
        char b[300];
        T("new file, long name",
          fo_create("/DESKTOP", "My_Long_Document_Name.txt", false, false) ==
              NULL);
        T("new folder, long name",
          fo_create("/DESKTOP", "Project_Files_2026", true, false) == NULL);
        T("lower-case name",
          fo_create("/DESKTOP", "notes.txt", false, false) == NULL);
        fo_create("/DESKTOP/Project_Files_2026", "Budget_for_next_year.txt",
                  false, false);
        fo_create("/DESKTOP/Project_Files_2026", "Meeting_Notes_(draft).txt",
                  false, false);
        fo_create("/DESKTOP/Project_Files_2026", "Sub_Folder_With_Long_Name",
                  true, false);
        desktop_fs_reload();
        sim_frames(2);
        int li = -1, fi = -1;
        for (int i = 0; i < desktop_fs_count(); i++) {
            if (!strcmp(desktop_fs_items()[i].name,
                        "My_Long_Document_Name.txt"))
                li = i;
            if (!strcmp(desktop_fs_items()[i].name, "Project_Files_2026"))
                fi = i;
        }
        T("desktop shows the long names", li >= 0 && fi >= 0);
        sim_shot("lu0");
        desktop_fs_items()[li].selected = true;
        sim_frames(2);
        sim_shot("lu1");
        desktop_fs_items()[li].selected = false;
        /* FileF in the long-named folder */
        fo_open(NULL, "/DESKTOP/Project_Files_2026");
        sim_frames(3);
        window *w = wm_focused_window();
        T("FileF opens it", w && strstr(w->title, "Project_Files_2026"));
        sim_shot("lu2");
        control *view = NULL;
        for (control *c = w->controls; c; c = c->next)
            if (c->type == CTL_CUSTOM && c->aw > 100)
                view = c;
        sim_click(view->ax + 2 + 4 + 44 + 22, view->ay + 2 + 6 + 10);
        sim_frames(2);
        sim_shot("lu3");
        sim_key(0x3C);
        sim_frames(2);
        sim_shot("lu4"); /* F2: rename */
        window *rw = wm_focused_window();
        control *ed = NULL;
        for (control *c = rw->controls; c; c = c->next)
            if (c->type == CTL_EDIT)
                ed = c;
        snprintf(b, sizeof b, "    rename box: '%s'\n",
                 ed ? ctl_text(ed) : "?");
        wr(1, b);
        T("rename box has the long name",
          ed && !strcmp(ctl_text(ed), "Sub_Folder_With_Long_Name"));
        sim_key(0x01);
        sim_frames(2);
        T("rename through fileops",
          fo_rename("/DESKTOP/Project_Files_2026/Budget_for_next_year.txt",
                    "Budget_2027_(final).txt") == NULL);
        T("case-only rename",
          fo_rename("/DESKTOP/notes.txt", "Notes.txt") == NULL);
        char nm[FS_NAME_SZ];
        T("now Notes.txt",
          fs_name_of("/DESKTOP/NOTES.TXT", nm) && !strcmp(nm, "Notes.txt"));
        sim_frames(3);
        sim_shot("lu5");
        /* Get Info: the DOS name too */
        static char info[800];
        fo_info_text("/DESKTOP/My_Long_Document_Name.txt", info, sizeof info);
        wr(1, info);
        wr(1, "\n");
        T("info shows the DOS name",
          strstr(info, "DOS:    MY_LON~1.TXT") != NULL);
        /* the file dialog */
        dlg_open_file(NULL, "Open", "/DESKTOP/Project_Files_2026", NULL, NULL,
                      NULL);
        sim_frames(3);
        sim_shot("lu6");
        sim_key(0x01);
        sim_frames(2);
        /* trash a long name, twice */
        fo_create("/DESKTOP", "Trash_Me_Please.txt", false, false);
        T("to the Trash",
          fo_trash("/DESKTOP/Trash_Me_Please.txt") == NULL &&
              fo_stat("/TRASH/Trash_Me_Please.txt", NULL, NULL));
        fo_create("/DESKTOP", "Trash_Me_Please.txt", false, false);
        T("again: (2)",
          fo_trash("/DESKTOP/Trash_Me_Please.txt") == NULL &&
              fo_stat("/TRASH/Trash_Me_Please(2).txt", NULL, NULL));
        T("put back (2) under its own name",
          fo_put_back("/TRASH/Trash_Me_Please(2).txt") == NULL &&
              fo_stat("/DESKTOP/Trash_Me_Please.txt", NULL, NULL));
        /* the CLI */
        static char o[2048];
        extern int cli_execute_command(const char *, char *, size_t);
        o[0] = 0;
        cli_execute_command("cd /DESKTOP/Project_Files_2026", o, sizeof o);
        T("cd with spaces, no quotes",
          !strcmp(dir_context.path, "/DESKTOP/Project_Files_2026"));
        o[0] = 0;
        cli_execute_command("ls", o, sizeof o);
        wr(1, o);
        T("ls long names", strstr(o, "Budget_2027_(final).txt") != NULL);
        o[0] = 0;
        cli_execute_command(
            "cp \"Budget_2027_(final).txt\" \"A Copy Of It.txt\"", o, sizeof o);
        wr(1, o);
        T("cp to a name with spaces: refused",
          strstr(o, "No spaces") &&
              !fo_stat("/DESKTOP/Project_Files_2026/A Copy Of It.txt", NULL,
                       NULL));
        o[0] = 0;
        cli_execute_command("cp \"Budget_2027_(final).txt\" A_Copy_Of_It.txt",
                            o, sizeof o);
        T("cp with quotes",
          fo_stat("/DESKTOP/Project_Files_2026/A_Copy_Of_It.txt", NULL, NULL));
        o[0] = 0;
        cli_execute_command("cd \"Sub_Folder_With_Long_Name\"", o, sizeof o);
        T("cd with quotes",
          strstr(dir_context.path, "Sub_Folder_With_Long_Name") != NULL);
        o[0] = 0;
        cli_execute_command("cd ..", o, sizeof o);
        fs_chdir("/");
        {
            extern void sim_save_hda(const char *);
            sim_save_hda(OUTDIR "hda.img");
        }
        return fails;
    }
    if (!strcmp(name, "dirlist")) {
        int fails = 0;
        char b[300];
        static uint8_t buf[8192];
        uint32_t n = 0;
        if (!load_file(FIXDIR "dl/DIRLIST.APP", buf, sizeof buf, &n))
            return 1;
        fat_file_t f;
        fs_create("/DIRLIST.APP", &f);
        fs_write(&f, buf, n);
        fs_close(&f);
        fo_create("/DESKTOP", "A_Folder_With_A_Long_Name", true, false);
        fo_create("/DESKTOP/A_Folder_With_A_Long_Name",
                  "Saved_Game_Number_One.sav", false, false);
        if (!appload_launch("/DIRLIST.APP")) {
            wr(2, appload_last_error());
            return 1;
        }
        sim_frames(3);
        window *w = wm_focused_window();
        typedef struct {
            window *win;
            const os_api_t *api;
            control *path, *list, *status;
            char dir[512];
            app_dirent_t ents[64];
            int n;
        } st_t;
        st_t *st = w->dyn_state;
        const os_api_t *api = st->api;
        T("API size 6", api->size == sizeof(os_api_t));
        snprintf(b, sizeof b, "    / has %d\n", st->n);
        wr(1, b);
        T("lists /", st->n >= 2);
        int cnt = api->list_dir("/DESKTOP", NULL, 0);
        app_dirent_t e[16];
        int got = api->list_dir("/DESKTOP", e, 16);
        bool seen = false;
        for (int i = 0; i < got; i++)
            if (!strcmp(e[i].name, "A_Folder_With_A_Long_Name") &&
                (e[i].attr & APP_ATTR_DIR) &&
                !strcmp(e[i].dos_name, "A_FOLD~1"))
                seen = true;
        T("count == listed", cnt == got);
        T("long folder name + DOS name", seen);
        T("max respected", api->list_dir("/DESKTOP", e, 1) == 1);
        T("missing folder: -1", api->list_dir("/NOPE", e, 4) == -1);
        app_dirent_t fi;
        T("file_info",
          api->file_info(
              "/DESKTOP/A_Folder_With_A_Long_Name/Saved_Game_Number_One.sav",
              &fi) &&
              !strcmp(fi.name, "Saved_Game_Number_One.sav") &&
              !(fi.attr & APP_ATTR_DIR));
        T("make_dir",
          api->make_dir("/DESKTOP/A_Folder_With_A_Long_Name/Level_Packs"));
        T("rename",
          api->rename("/DESKTOP/A_Folder_With_A_Long_Name/Level_Packs",
                      "More_Level_Packs"));
        T("remove (empty folder)",
          api->remove("/DESKTOP/A_Folder_With_A_Long_Name/More_Level_Packs"));
        T("remove refuses a full folder",
          !api->remove("/DESKTOP/A_Folder_With_A_Long_Name"));
        T("remove refuses protected", !api->remove("/DESKTOP"));
        /* through the window: type a path, Enter */
        api->ui->set_text(st->path, "/DESKTOP/A_Folder_With_A_Long_Name");
        ctl_invoke(st->path, EV_SUBMIT);
        sim_frames(2);
        T("window lists it",
          st->n == 1 && !strcmp(st->ents[0].name, "Saved_Game_Number_One.sav"));
        sim_shot("dl1");
        return fails;
    }
    if (!strcmp(name, "fddauto")) {
        /* a floppy taken out is seen by itself (silently: the head does
         * not move) and its desktop icon goes; one put in is read when its
         * drive is opened in FileF's Drives */
        int fails = 0;
        char b[300];
        extern int g_fdc_drive_type, g_fdcb_drive_type, g_fdc_seeks;
        extern void fdc_insert_unit(int, int), fdc_setup(void);
        static char o[2048];
        extern int cli_execute_command(const char *, char *, size_t);
        g_fdc_drive_type = 4;
        g_fdcb_drive_type = 0;
        fdc_setup();
        fdc_insert_unit(0, 1440);
        T("A: found", fdd_init(0));
        cli_execute_command("format fdd0 /q AUTO", o, sizeof o);
        fdd_eject(DRIVE_FDD0);
        T("mounted", fdd_insert(DRIVE_FDD0));
        desktop_fs_set_dirty();
        sim_frames(3);
#define FDD_ICON()                                                             \
    ({                                                                         \
        int ic_ = -1;                                                          \
        for (int i = 0; i < desktop_fs_count(); i++)                           \
            if (desktop_fs_items()[i].drive_id == DRIVE_FDD0 &&                \
                desktop_fs_items()[i].kind == DESKTOP_ITEM_FDD)                \
                ic_ = i;                                                       \
        ic_;                                                                   \
    })
        int ic = FDD_ICON();
        T("its icon, its label",
          ic >= 0 && !strcmp(desktop_fs_items()[ic].name, "AUTO"));
        /* taken out, nothing waiting */
        fdc_insert_unit(0, 0);
        int seeks = g_fdc_seeks;
        sim_frames(40);
        T("taken out: unmounted by itself", !fs_drive_mounted(DRIVE_FDD0));
        T("taken out: no icon on the desktop", FDD_ICON() < 0);
        window *fw = wm_focused_window();
        T("no message (nothing was lost)",
          !(fw && fw->title && !strcmp(fw->title, "FDD0")));
        sim_frames(400);
        T("empty: never a step (silent)", g_fdc_seeks == seeks);
        /* put in: not seen until the drive is opened (FileF, Drives) */
        fdc_insert_unit(0, 1440);
        sim_frames(40);
        T("put in: not looked at",
          !fs_drive_mounted(DRIVE_FDD0) && FDD_ICON() < 0);
        sim_launch("FILEF");
        sim_frames(3);
        window *w = wm_focused_window();
        sim_shot("fddauto1");
        int fx = w->x + 154, fy = w->y + 50; /* Drives: HDA, FDD0 */
        sim_dblclick(fx, fy);
        sim_frames(4);
        snprintf(b, sizeof b, "    window '%s'\n", w->title);
        wr(1, b);
        T("opened in FileF: mounted, shown",
          fs_drive_mounted(DRIVE_FDD0) && !strcmp(w->title, "/FDD0"));
        T("and on the desktop", FDD_ICON() >= 0);
        sim_shot("fddauto2");
        /* taken out with a write waiting: said so */
        fat_file_t f;
        T("a write (waits in memory)",
          fs_create("/FDD0/NEW.TXT", &f) && fs_write(&f, "x", 1) == 1);
        fs_close(&f);
        fdc_insert_unit(0, 0);
        sim_frames(40);
        T("taken out: unmounted", !fs_drive_mounted(DRIVE_FDD0));
        T("the FileF window back at Drives", !strcmp(w->title, "Drives"));
        window *m = wm_focused_window();
        T("a message: the write was lost",
          m && m->title && !strcmp(m->title, "FDD0"));
        sim_shot("fddauto3");
        if (m && m->title && !strcmp(m->title, "FDD0"))
            wm_close(m);
        /* opening the empty drive */
        sim_frames(3);
        wm_focus(w);
        sim_frames(2);
        sim_dblclick(fx, fy);
        sim_frames(4);
        m = wm_focused_window();
        T("opening it empty: a message", !fs_drive_mounted(DRIVE_FDD0) && m &&
                                             m->title &&
                                             !strcmp(m->title, "FDD0"));
        sim_shot("fddauto4");
#undef FDD_ICON
        return fails;
    }
    if (!strcmp(name, "fdd525")) {
        /* a 1.2 MB 5.25" drive: 720 KB disks (80 tracks, double density,
         * 300 kbps) besides 1.2 MB and the 40-track ones */
        int fails = 0;
        char b[300];
        extern int g_fdc_drive_type, g_fdcb_drive_type, g_fdc_formats,
            g_fdc_errors;
        extern void fdc_insert_unit(int, int), fdc_setup(void);
        static char o[2048];
        extern int cli_execute_command(const char *, char *, size_t);
        g_fdc_drive_type = 2; /* 1.2 MB */
        g_fdcb_drive_type = 0;
        fdc_setup();
        T("1.2 MB drive found", fdd_init(0));
        int sz[FDD_MAX_FORMATS], n = fdd_drive_formats(0, sz);
        T("720 KB offered, last",
          n == 6 && sz[0] == FDD_MEDIA_1200K && sz[5] == FDD_MEDIA_720K);
        /* a 360 KB disk is still 360 KB (track 0 looks the same) */
        fdc_insert_unit(0, 360);
        o[0] = 0;
        cli_execute_command("format fdd0 360 FORTY", o, sizeof o);
        wr(1, o);
        fdd_eject(DRIVE_FDD0);
        T("360 KB: mounts as 360 KB",
          fdd_insert(DRIVE_FDD0) && fdd_media(0) == FDD_MEDIA_360K);
        fdd_eject(DRIVE_FDD0);
        /* 720 KB formatted in it */
        fdc_insert_unit(0, 720);
        int f0 = g_fdc_formats;
        o[0] = 0;
        cli_execute_command("format fdd0 720 DD525", o, sizeof o);
        wr(1, o);
        T("720 KB formatted: 160 tracks", g_fdc_formats - f0 == 160);
        T("mounted as 720 KB",
          fs_drive_mounted(DRIVE_FDD0) && fdd_media(0) == FDD_MEDIA_720K &&
              !strcmp(fs_drive_label(DRIVE_FDD0), "DD525"));
        static uint8_t big[300000], rb[300000];
        for (int i = 0; i < (int)sizeof big; i++)
            big[i] = (uint8_t)(i * 13 + i / 4096);
        fat_file_t f;
        bool ok = fs_create("/FDD0/BIG.BIN", &f) &&
                  fs_write(&f, big, sizeof big) == (int)sizeof big;
        fs_close(&f);
        T("300 KB written (past track 40)", ok);
        fdd_eject(DRIVE_FDD0);
        int errs = g_fdc_errors;
        T("put in again: 720 KB",
          fdd_insert(DRIVE_FDD0) && fdd_media(0) == FDD_MEDIA_720K);
        int got = 0;
        if (fs_open("/FDD0/BIG.BIN", &f)) {
            got = fs_read(&f, rb, sizeof rb);
            fs_close(&f);
        }
        snprintf(b, sizeof b, "    read %d bytes, %d controller errors\n", got,
                 g_fdc_errors - errs);
        wr(1, b);
        T("read back the same",
          got == (int)sizeof big && !memcmp(big, rb, sizeof big));
        fdd_eject(DRIVE_FDD0);
        /* the same disk in a 360 KB drive (40 tracks): refused */
        g_fdc_drive_type = 1;
        fdc_setup();
        fdd_init(0);
        fdc_insert_unit(0, 720);
        T("720 KB in a 360 KB drive: not mounted", !fdd_insert(DRIVE_FDD0));
        /* and in a 3.5" 720 KB drive: the same format */
        g_fdc_drive_type = 3;
        fdc_setup();
        fdd_init(0);
        fdc_insert_unit(0, 720);
        got = 0;
        if (fdd_insert(DRIVE_FDD0) && fs_open("/FDD0/BIG.BIN", &f)) {
            got = fs_read(&f, rb, sizeof rb);
            fs_close(&f);
        }
        T("read in a 720 KB drive too",
          got == (int)sizeof big && !memcmp(big, rb, sizeof big));
        return fails;
    }
    if (!strcmp(name, "twofdd")) {
        int fails = 0;
        char b[300];
        extern int g_fdc_drive_type, g_fdcb_drive_type, g_fdc_spinups,
            g_fdc_motor_fails, g_fdc_reads, g_fdc_writes;
        extern void fdc_insert_unit(int, int), fdc_setup(void);
        static char o[2048];
        extern int cli_execute_command(const char *, char *, size_t);
        g_fdc_drive_type = 4;
        g_fdcb_drive_type = 1;
        fdc_setup(); /* A: 1.44M 3.5", B: 360K 5.25" */
        fdc_insert_unit(0, 1440);
        fdc_insert_unit(1, 360);
        T("A: found", fdd_init(0));
        T("B: found", fdd_init(1));
        o[0] = 0;
        cli_execute_command("format a: /q DISKA", o, sizeof o);
        wr(1, o);
        o[0] = 0;
        cli_execute_command("format fdd1: /q DISKB", o, sizeof o);
        wr(1, o);
        fdd_eject(DRIVE_FDD0);
        fdd_eject(DRIVE_FDD1);
        T("both mount", fdd_insert(DRIVE_FDD0) && fdd_insert(DRIVE_FDD1));
        T("labels", !strcmp(fs_drive_label(DRIVE_FDD0), "DISKA") &&
                        !strcmp(fs_drive_label(DRIVE_FDD1), "DISKB"));
        static uint8_t data[20000];
        for (int i = 0; i < (int)sizeof data; i++)
            data[i] = (uint8_t)(i * 7 + i / 300);
        fat_file_t f;
        T("write 20 KB on A:",
          fs_create("/FDD0/Big File On A.bin", &f) &&
              fs_write(&f, data, sizeof data) == (int)sizeof data &&
              fs_close(&f));
        int sp0 = g_fdc_spinups, r0 = g_fdc_reads, w0 = g_fdc_writes;
        uint32_t t0 = pit_ticks;
        T("copy A: -> B:",
          fs_copy_file("/FDD0/Big File On A.bin", "/FDD1/Copied To B.bin"));
        snprintf(b, sizeof b,
                 "    copy: %d reads, %d writes, %d spin-ups, %u ms\n",
                 g_fdc_reads - r0, g_fdc_writes - w0, g_fdc_spinups - sp0,
                 (unsigned)(pit_ticks - t0));
        wr(1, b);
        static uint8_t back[20000];
        int got = 0;
        if (fs_open("/FDD1/Copied To B.bin", &f)) {
            got = fs_read(&f, back, sizeof back);
            fs_close(&f);
        }
        T("B: copy is the same",
          got == (int)sizeof data && !memcmp(back, data, sizeof data));
        T("no access with a stopped motor", g_fdc_motor_fails == 0);
        fs_mkdir("/FDD0/Folder");
        fs_create("/FDD0/Folder/One.txt", &f);
        fs_write(&f, "one", 3);
        fs_close(&f);
        T("folder A: -> B:", fs_copy_dir("/FDD0/Folder", "/FDD1/Folder"));
        T("move B: -> A:",
          fs_move_file("/FDD1/Copied To B.bin", "/FDD0/Back Again.bin"));
        dir_entry_t de;
        T("moved", fs_find("/FDD0/Back Again.bin", &de) &&
                       de.file_size == sizeof data &&
                       !fs_find("/FDD1/Copied To B.bin", &de));
        /* eject one: the other stays */
        T("eject B:", fdd_eject(DRIVE_FDD1));
        T("A: still reads", fs_find("/FDD0/Folder/One.txt", &de));
        T("B: back in",
          fdd_insert(DRIVE_FDD1) && fs_find("/FDD1/Folder/One.txt", &de));
        /* the motors stop after a while idle */
        sim_frames(5);
        uint32_t t1 = pit_ticks;
        while (pit_ticks - t1 < 3500)
            sim_frames(10);
        extern uint8_t sim_low[];
        bool *mot = (bool *)(sim_low + 0x204);
        T("motors off when idle", !mot[0] && !mot[1]);
        snprintf(b, sizeof b, "    motor fails %d\n", g_fdc_motor_fails);
        wr(1, b);
        desktop_fs_reload();
        sim_frames(2);
        sim_shot("tf1");
        return fails;
    }
    if (!strcmp(name, "sudoku2") || !strcmp(name, "sudoku320")) {
        int fails = 0;
        char b[300];
        if (!strcmp(name, "sudoku320")) {
            extern int g_video_mode;
            g_screen_width = 320;
            g_screen_height = 200;
            g_video_mode = 0;
        }
        app_instance_t *a = sim_launch("Sudoku");
        sim_frames(5);
        window *w = wm_focused_window();
        T("bundled Sudoku runs", a && w && !strcmp(w->title, "Sudoku"));
        typedef struct {
            window *win;
            const os_api_t *api;
            control *view, *status;
            uint8_t sol[81], given[81], cur[81];
        } sd_t;
        sd_t *s = a->state;
        int g = 0;
        for (int i = 0; i < 81; i++)
            g += s->given[i];
        T("a puzzle", g >= 25 && g <= 41);
        sim_key(0x02);
        sim_frames(2);
        {
            extern void sim_key2(uint8_t, char, bool, bool);
            sim_ext(0x4D);
            sim_key2(0x03, 0, true, false);
            sim_key2(0x05, 0, true, false);
            sim_key2(0x0A, 0, true, false);
            sim_ext(0x4D);
        }
        sim_frames(2);
        sim_shot(g_screen_height < 300 ? "sd3" : "sd2");
        snprintf(b, sizeof b, "    window %dx%d at %d,%d\n", w->w, w->h, w->x,
                 w->y);
        wr(1, b);
        T("fits the screen",
          w->y + w->h <= g_screen_height && w->x + w->w <= g_screen_width);
        /* the old kind, from a file anywhere, still runs */
        static uint8_t su[16384];
        uint32_t n = 0;
        fat_file_t f;
        load_file(FIXDIR "lz/SUDOKU.APP", su, sizeof su, &n);
        fs_create("/OLDSUDO.APP", &f);
        fs_write(&f, su, n);
        fs_close(&f);
        wm_close(w);
        sim_frames(4);
        app_instance_t *ob = appload_launch("/OLDSUDO.APP");
        sim_frames(5);
        T("old-style SUDOKU.APP still runs",
          ob && wm_focused_window() &&
              !strcmp(wm_focused_window()->title, "Sudoku"));
        return fails;
    }
    if (!strcmp(name, "appsbar")) {
        int fails = 0;
        char b[300];
        app_instance_t *a = sim_launch("Sudoku");
        sim_frames(4);
        T("an app in front",
          a && wm_focused_window() && !wm_focused_window()->pinned_bottom);
        T("Apps first on its bar",
          g_menubar.menu_count > 1 &&
              !strcmp(g_menubar.menus[0].title, "Apps") &&
              !strcmp(g_menubar.menus[1].title, "Game"));
        snprintf(b, sizeof b, "    bar: %s %s %s %s\n",
                 g_menubar.menus[0].title, g_menubar.menus[1].title,
                 g_menubar.menus[2].title, g_menubar.menus[3].title);
        wr(1, b);
        /* start another app from it */
        sim_click(g_menubar.menus[0].bar_x + 4, 4);
        sim_frames(1);
        menu *am = &g_menubar.menus[0];
        int pz = -1;
        for (int i = 0; i < am->item_count; i++)
            if (am->items[i].label && !strcmp(am->items[i].label, "Games"))
                pz = i;
        sim_move(am->bar_x + 10, MENUBAR_H + pz * 10 + 4);
        sim_frames(2);
        sim_shot("ab1");
        sim_move(am->bar_x + 120, MENUBAR_H + pz * 10 + 4);
        sim_frames(1);
        mouse.left = true;
        sim_frame();
        mouse.left = false;
        sim_frames(6);
        window *w = wm_focused_window();
        snprintf(b, sizeof b, "    launched %s\n", w ? w->title : "-");
        wr(1, b);
        T("Puzzle started from Sudoku's bar", w && strstr(w->title, "Puzzle"));
        T("its bar has Apps too", !strcmp(g_menubar.menus[0].title, "Apps"));
        /* power off: the arrow is taken off the picture first */
        mouse.x = 150;
        mouse.y = 150;
        sim_frames(3);
        extern unsigned char g_sim_vram[], g_sim_shadow[];
        uint8_t before = g_sim_vram[150 * SCREEN_WIDTH + 150];
        extern void wd_stop(void);
        wd_stop(); /* (what power_anim_run does first) */
        snprintf(b, sizeof b, "    arrow pixel %d, under it %d\n", before,
                 g_sim_vram[150 * SCREEN_WIDTH + 150]);
        wr(1, b);
        T("the arrow was on the screen and is off it",
          before == BLACK &&
              g_sim_vram[150 * SCREEN_WIDTH + 150] ==
                  g_sim_shadow[150 * SCREEN_WIDTH + 150] &&
              BACKBUF[150 * SCREEN_WIDTH + 150] ==
                  g_sim_shadow[150 * SCREEN_WIDTH + 150]);
        return fails;
    }
    if (!strcmp(name, "showcase")) {
        fo_create("/DESKTOP", "Project Notes.txt", false, false);
        sim_launch("FILEF");
        sim_frames(3);
        window *f = wm_focused_window();
        if (f) {
            f->x = 4;
            f->y = 150;
            char b[200];
            snprintf(b, sizeof b, "filef title '%s'\n",
                     f->title ? f->title : "(null)");
            wr(1, b);
        }
        sim_launch("Calendar");
        sim_frames(3);
        window *c = wm_focused_window();
        if (c) {
            c->x = 330;
            c->y = 14;
        }
        sim_launch("Sudoku");
        sim_frames(5);
        window *s = wm_focused_window();
        if (s) {
            s->x = 150;
            s->y = 60;
        }
        sim_frames(3);
        sim_click(g_menubar.menus[0].bar_x + 4, 4);
        sim_frames(1);
        menu *am = &g_menubar.menus[0];
        sim_move(am->bar_x + 10, MENUBAR_H + 0 * 10 + 4);
        sim_frames(3);
        sim_shot("show1");
        return 0;
    }
    if (!strcmp(name, "fddspeed")) {
        int fails = 0;
        char b[300];
        extern int g_fdc_drive_type, g_fdcb_drive_type, g_fdc_ms, g_fdc_reads,
            g_fdc_writes, g_fdc_seeks;
        extern bool g_fdc_wp;
        extern uint8_t g_fdc_img[];
        extern void fdc_insert_unit(int, int), fdc_setup(void);
        static char o[2048];
        extern int cli_execute_command(const char *, char *, size_t);
        g_fdc_drive_type = 4;
        g_fdcb_drive_type = 0;
        fdc_setup();
        fdc_insert_unit(0, 1440);
        fdd_init(0);
        o[0] = 0;
        cli_execute_command("format a: /q DISK_ONE", o, sizeof o);
        fdd_eject(DRIVE_FDD0);
        T("mounts", fdd_insert(DRIVE_FDD0));
        static uint8_t data[20000];
        for (int i = 0; i < 20000; i++)
            data[i] = (uint8_t)(i * 13 + 7);
        fat_file_t f;
        int ms0 = g_fdc_ms, w0 = g_fdc_writes, r0 = g_fdc_reads;
        T("write 20 KB", fs_create("/FDD0/Saved Picture.dat", &f) &&
                             fs_write(&f, data, 20000) == 20000 &&
                             fs_close(&f));
        int ms1 = g_fdc_ms;
        sim_frames(60); /* 1/2 s idle: written to the disk */
        snprintf(b, sizeof b,
                 "    write: %d ms in the calls, %d ms flushing; %d sectors "
                 "written, %d read\n",
                 ms1 - ms0, g_fdc_ms - ms1, g_fdc_writes - w0,
                 g_fdc_reads - r0);
        wr(1, b);
        T("written to the disk after 1/2 s idle", g_fdc_writes - w0 >= 40);
        /* read it back cold: eject, insert */
        fdd_eject(DRIVE_FDD0);
        fdd_insert(DRIVE_FDD0);
        int ms2 = g_fdc_ms;
        static uint8_t back[20000];
        int got = 0;
        if (fs_open("/FDD0/Saved Picture.dat", &f)) {
            got = fs_read(&f, back, 20000);
            fs_close(&f);
        }
        snprintf(b, sizeof b, "    read: %d ms\n", g_fdc_ms - ms2);
        wr(1, b);
        T("reads back the same", got == 20000 && !memcmp(back, data, 20000));
        /* the disk swapped without Eject */
        static uint8_t other[1474560];
        memcpy(other, g_fdc_img, sizeof other);
        o[0] = 0;
        cli_execute_command("ls /FDD0", o, sizeof o);
        fdd_eject(DRIVE_FDD0); /* make a second disk: format, one file */
        fdc_insert_unit(0, 1440);
        fdd_insert(DRIVE_FDD0);
        o[0] = 0;
        cli_execute_command("format a: /q DISK_TWO", o, sizeof o);
        fs_create("/FDD0/Only On Two.txt", &f);
        fs_write(&f, "two", 3);
        fs_close(&f);
        sim_frames(60);
        static uint8_t two[1474560];
        memcpy(two, g_fdc_img, sizeof two);
        memcpy(g_fdc_img, other, sizeof other);
        fdc_insert_unit(0, 1440); /* disk one back in: no Eject */
        dir_entry_t de;
        bool first = fs_find("/FDD0/Saved Picture.dat", &de);
        bool second = fs_find("/FDD0/Saved Picture.dat", &de);
        snprintf(b, sizeof b, "    after the swap: %d then %d, label %s\n",
                 first, second, fs_drive_label(DRIVE_FDD0));
        wr(1, b);
        T("swap seen: the disk mounted again",
          second && !strcmp(fs_drive_label(DRIVE_FDD0), "DISK_ONE"));
        T("the other disk's file isn't there",
          !fs_find("/FDD0/Only On Two.txt", &de));
        /* and back to disk two, with a write pending on one: not written to two
         */
        fs_create("/FDD0/Pending.txt", &f);
        fs_write(&f, "p", 1);
        fs_close(&f); /* still in memory */
        memcpy(g_fdc_img, two, sizeof two);
        fdc_insert_unit(0, 1440);
        fs_find("/FDD0/x", &de);
        bool two_ok = fs_find("/FDD0/Only On Two.txt", &de);
        T("disk two mounted, intact",
          two_ok && !strcmp(fs_drive_label(DRIVE_FDD0), "DISK_TWO"));
        T("one's pending write didn't land on two",
          !fs_find("/FDD0/Pending.txt", &de));
        /* write protection: refused at once */
        g_fdc_wp = true;
        fdd_eject(DRIVE_FDD0);
        fdd_insert(DRIVE_FDD0);
        T("write-protected: create fails", !fs_create("/FDD0/No.txt", &f));
        g_fdc_wp = false;
        return fails;
    }
    if (!strcmp(name, "zipeng")) {
        int fails = 0;
        char b[400];
        extern bool load_file(const char *, uint8_t *, uint32_t, uint32_t *);
        static uint8_t buf[300000], buf2[300000];
        uint32_t n;
        static char man[2000];
        fat_file_t f;
        load_file(FIXDIR "zt/manifest", (uint8_t *)man, sizeof man - 1, &n);
        man[n] = 0;
        fs_mkdir("/HDA/ZT");
        fs_mkdir("/HDA/ZT/sub");
        fs_mkdir("/HDA/ZT/sub/deeper");
        for (char *cur = man, *l; (l = zt_line(&cur));) {
            char hp[300], p[300];
            snprintf(hp, sizeof hp, FIXDIR "zt/in/%s", l);
            snprintf(p, sizeof p, "/HDA/ZT/%s", l);
            load_file(hp, buf, sizeof buf, &n);
            if (fs_create(p, &f)) {
                fs_write(&f, buf, n);
                fs_close(&f);
            } else {
                snprintf(b, sizeof b, "  can't make %s\n", p);
                wr(1, b);
            }
        }
        fs_set_hidden("/HDA/ZT/hid.txt", true);
        const char *names[] = {"store", "fast", "normal", "best"};
        for (int lv = 0; lv < 4; lv++) {
            char zp[64];
            snprintf(zp, sizeof zp, "/HDA/OUT%d.ZIP", lv);
            zip_stats_t st;
            uint32_t t0 = pit_ticks;
            const char *e =
                zip_create("/HDA/ZT", zp, lv, lv != 1, NULL, NULL, &st);
            snprintf(b, sizeof b,
                     "    %s: %s, %d files %d dirs, %u -> %u bytes\n",
                     names[lv], e ? e : "ok", st.files, st.dirs, st.in_bytes,
                     st.out_bytes);
            wr(1, b);
            T("zip made", !e);
            if (fs_open(zp, &f)) {
                n = fs_read(&f, buf, sizeof buf);
                fs_close(&f);
                snprintf(b, sizeof b, OUTDIR "out%d.zip", lv);
                sim_save(b, buf, n);
            }
            (void)t0;
        }
        /* a zip from elsewhere */
        load_file(FIXDIR "zt/py.zip", buf, sizeof buf, &n);
        fs_create("/HDA/PY.ZIP", &f);
        fs_write(&f, buf, n);
        fs_close(&f);
        T("looks like a zip",
          zip_is_zip("/HDA/PY.ZIP") && !zip_is_zip("/HDA/ZT/notes.txt"));
        zip_stats_t st;
        const char *e =
            zip_extract("/HDA/PY.ZIP", "/HDA/PYX", false, NULL, NULL, &st);
        snprintf(b, sizeof b,
                 "    extract: %s, %d files %d dirs, %d skipped %d unsupported "
                 "%d bad\n",
                 e ? e : "ok", st.files, st.dirs, st.skipped, st.unsupported,
                 st.bad);
        wr(1, b);
        T("extracted",
          !e && st.files == 7 && st.unsupported == 1 && st.bad == 0);
        load_file(FIXDIR "zt/expect", (uint8_t *)man, sizeof man - 1, &n);
        man[n] = 0;
        int good = 0, want = 0;
        for (char *cur = man, *l; (l = zt_line(&cur));) {
            char hp[300], p[300];
            snprintf(hp, sizeof hp, FIXDIR "zt/exp/%s", l);
            snprintf(p, sizeof p, "/HDA/PYX/%s", l);
            uint32_t en = 0;
            load_file(hp, buf2, sizeof buf2, &en);
            want++;
            int got = -1;
            if (fs_open(p, &f)) {
                got = fs_read(&f, buf, sizeof buf);
                fs_close(&f);
            }
            if (got == (int)en && !memcmp(buf, buf2, en))
                good++;
            else {
                snprintf(b, sizeof b, "    differs: %s (%d vs %u)\n", p, got,
                         en);
                wr(1, b);
            }
        }
        T("every file right", good == want);
        dir_entry_t de;
        T("nothing outside", !fs_find("/HDA/evil.txt", &de));
        /* again: all there, nothing replaced */
        e = zip_extract("/HDA/PY.ZIP", "/HDA/PYX", false, NULL, NULL, &st);
        T("again: skipped", !e && st.skipped == 7 && st.files == 0);
        e = zip_extract("/HDA/PY.ZIP", "/HDA/PYX", true, NULL, NULL, &st);
        T("again, replace: written", !e && st.files == 7);
        /* round trip of our own */
        for (int lv = 0; lv < 4; lv++) {
            char zp[64], dp[64];
            snprintf(zp, sizeof zp, "/HDA/OUT%d.ZIP", lv);
            snprintf(dp, sizeof dp, "/HDA/RT%d", lv);
            e = zip_extract(zp, dp, false, NULL, NULL, &st);
            load_file(FIXDIR "zt/manifest", (uint8_t *)man, sizeof man - 1, &n);
            man[n] = 0;
            int ok = 0, all = 0;
            for (char *cur = man, *l; (l = zt_line(&cur));) {
                if (lv == 1 && !strcmp(l, "hid.txt"))
                    continue;
                char a[300], c[300];
                snprintf(a, sizeof a, "/HDA/ZT/%s", l);
                snprintf(c, sizeof c, "%s/ZT/%s", dp, l);
                int x = -1, y = -2;
                all++;
                if (fs_open(a, &f)) {
                    x = fs_read(&f, buf, sizeof buf);
                    fs_close(&f);
                }
                if (fs_open(c, &f)) {
                    y = fs_read(&f, buf2, sizeof buf2);
                    fs_close(&f);
                }
                if (x == y && !memcmp(buf, buf2, x))
                    ok++;
                else {
                    snprintf(b, sizeof b, "    rt%d differs: %s %d %d\n", lv, c,
                             x, y);
                    wr(1, b);
                }
            }
            snprintf(b, sizeof b, "round trip %s", names[lv]);
            T(b, !e && ok == all && st.bad == 0);
        }
        T("fast left out the hidden file",
          !fs_find("/HDA/RT1/ZT/hid.txt", &de) &&
              fs_find("/HDA/RT2/ZT/hid.txt", &de));
        /* zip into the folder being zipped: not itself */
        e = zip_create("/HDA/ZT", "/HDA/ZT/self.zip", 2, true, NULL, NULL, &st);
        T("zip inside the folder", !e);
        e = zip_extract("/HDA/ZT/self.zip", "/HDA/SELF", false, NULL, NULL,
                        &st);
        T("doesn't hold itself", !e && !fs_find("/HDA/SELF/ZT/self.zip", &de));
        /* not a zip */
        e = zip_extract("/HDA/ZT/notes.txt", "/HDA/NZ", false, NULL, NULL, &st);
        T("not a zip: said so", e != NULL);
        return fails;
    }
    if (!strcmp(name, "zipui")) {
        int fails = 0;
        char b[300];
        extern void menu_item_run(const menu_item *);
        fo_create("/DESKTOP", "Project_Files", true, false);
        fo_create("/DESKTOP/Project_Files", "Sub", true, false);
        fat_file_t f;
        static char txt[20000];
        for (int i = 0; i < 20000; i++)
            txt[i] = "the quick brown fox jumps over the lazy dog\n"[i % 44];
        fs_create("/DESKTOP/Project_Files/Notes about things.txt", &f);
        fs_write(&f, txt, 20000);
        fs_close(&f);
        fs_create("/DESKTOP/Project_Files/Sub/more.txt", &f);
        fs_write(&f, txt, 5000);
        fs_close(&f);
        desktop_fs_reload();
        sim_frames(2);
        int fi = -1;
        for (int i = 0; i < desktop_fs_count(); i++)
            if (!strcmp(desktop_fs_items()[i].name, "Project_Files"))
                fi = i;
        desktop_item_t *it = &desktop_fs_items()[fi];
        sim_rclick(it->x + 16, it->y + 12);
        sim_frames(2);
        sim_shot("zu1");
        sim_click(it->x + 30, it->y + 12 + 55);
        sim_frames(3);
        sim_shot("zu2");
        T("Compress... opened the dialog",
          !strcmp(wm_focused_window()->title, "Compress"));
        window *w = wm_focused_window();
        control *name_ed = NULL, *go = NULL, *combo = NULL;
        for (control *c = w->controls; c; c = c->next) {
            if (c->type == CTL_EDIT && !name_ed)
                name_ed = c;
            if (c->type == CTL_BUTTON && !strcmp(ctl_text(c), "Compress"))
                go = c;
            if (c->type == CTL_COMBO)
                combo = c;
        }
        snprintf(b, sizeof b, "    dialog: '%s', name '%s'\n", w->title,
                 name_ed ? ctl_text(name_ed) : "?");
        wr(1, b);
        T("dialog with the name",
          name_ed && !strcmp(ctl_text(name_ed), "Project_Files.zip"));
        sim_click(combo->ax + 5, combo->ay + 5);
        sim_frames(2);
        sim_shot("zu3");
        sim_key(0x01);
        sim_frames(2);
        sim_click(go->ax + 5, go->ay + 5);
        sim_frames(3);
        sim_shot("zu4");
        dir_entry_t de;
        T("zip on the desktop",
          fs_find("/DESKTOP/Project_Files.zip", &de) && de.file_size > 100);
        window *msg = wm_focused_window();
        for (control *c = msg->controls; c; c = c->next)
            if (c->type == CTL_LABEL) {
                wr(1, "    | ");
                wr(1, ctl_text(c));
                wr(1, "\n");
            }
        sim_key(0x1C);
        sim_frames(3);
        /* again: asks to replace */
        fo_ask_compress(NULL, "/DESKTOP/Project_Files", NULL, NULL);
        sim_frames(3);
        w = wm_focused_window();
        for (control *c = w->controls; c; c = c->next)
            if (c->type == CTL_BUTTON && !strcmp(ctl_text(c), "Compress"))
                go = c;
        sim_click(go->ax + 5, go->ay + 5);
        sim_frames(3);
        sim_shot("zu5");
        T("asks to replace", strstr(wm_focused_window()->title, "Compress") &&
                                 wm_focused_window() != w);
        sim_key(0x01);
        sim_frames(2);
        sim_key(0x01);
        sim_frames(2);
        /* FileF: open the desktop folder, extract via the menu */
        fo_open(NULL, "/DESKTOP");
        sim_frames(3);
        window *fw = wm_focused_window();
        desktop_fs_reload();
        sim_frames(2);
        int zi = -1;
        for (int i = 0; i < desktop_fs_count(); i++)
            if (!strcmp(desktop_fs_items()[i].name, "Project_Files.zip"))
                zi = i;
        T("zip icon", zi >= 0);
        fo_ask_extract(fw, "/DESKTOP/Project_Files.zip", NULL, NULL);
        sim_frames(3);
        sim_shot("zu6");
        w = wm_focused_window();
        control *into = NULL, *ex = NULL, *brw = NULL;
        for (control *c = w->controls; c; c = c->next) {
            if (c->type == CTL_EDIT)
                into = c;
            if (c->type == CTL_BUTTON && !strcmp(ctl_text(c), "Extract"))
                ex = c;
            if (c->type == CTL_BUTTON && !strcmp(ctl_text(c), "Browse"))
                brw = c;
        }
        snprintf(b, sizeof b, "    into: '%s'\n", into ? ctl_text(into) : "?");
        wr(1, b);
        sim_click(brw->ax + 5, brw->ay + 5);
        sim_frames(3);
        sim_shot("zu7");
        sim_key(0x01);
        sim_frames(2);
        ctl_set_text(into, "/DESKTOP/Unpacked");
        sim_click(ex->ax + 5, ex->ay + 5);
        sim_frames(3);
        sim_shot("zu8");
        T("extracted",
          fs_find("/DESKTOP/Unpacked/Project_Files/Sub/more.txt", &de) &&
              de.file_size == 5000);
        sim_key(0x1C);
        sim_frames(3);
        sim_shot("zu9");
        /* FileF menus */
        int k = 0;
        bool ec = false, ee = false;
        for (int mm = 0; mm < fw->win_menu_count; mm++)
            for (k = 0; k < fw->win_menus[mm].item_count; k++) {
                const char *l = fw->win_menus[mm].items[k].label;
                if (!l)
                    continue;
                if (!strcmp(l, "Compress..."))
                    ec = true;
                if (!strcmp(l, "Extract..."))
                    ee = true;
            }
        T("FileF Edit menu has both", ec && ee);
        return fails;
    }
    if (!strcmp(name, "alttab")) {
        int fails = 0;
        char b[300];
        fo_open(NULL, "/DESKTOP");
        sim_frames(3);
        window *wa = wm_focused_window();
        sim_launch("Calculator");
        sim_frames(3);
        window *wb = wm_focused_window();
        sim_launch("Clock");
        sim_frames(3);
        window *wc = wm_focused_window();
        kb.alt = true;
        sim_key(0x0F);
        sim_frames(1);
        sim_shot("at1");
        T("box up, focus not moved yet",
          wm_switcher_active() && wm_focused_window() == wc);
        kb.alt = true;
        sim_key(0x0F);
        sim_frames(1);
        sim_shot("at2");
        kb.alt = false;
        sim_frames(2);
        T("Alt+Tab x2: the one before that",
          !wm_switcher_active() && wm_focused_window() == wa);
        kb.alt = true;
        sim_key(0x0F);
        kb.alt = false;
        sim_frames(2);
        T("Alt+Tab: back to the last one", wm_focused_window() == wc);
        kb.alt = true;
        sim_key2(0x0F, 0, true, false);
        kb.alt = true;
        sim_frames(1);
        sim_shot("at3");
        kb.alt = false;
        sim_frames(2);
        T("Alt+Shift+Tab: the oldest", wm_focused_window() == wb);
        kb.alt = true;
        sim_key(0x0F);
        sim_key(0x01);
        kb.alt = false;
        sim_frames(2);
        T("Esc: stays", wm_focused_window() == wb && !wm_switcher_active());
        wc->minimized = true;
        sim_frames(2);
        window *f = wm_focused_window();
        kb.alt = true;
        sim_key(0x0F);
        kb.alt = false;
        sim_frames(3);
        T("a minimised one comes back",
          !wc->minimized && wm_focused_window() == wc);
        (void)f;
        /* a dialog: its window comes with it */
        dlg_message(wa, "Hello", "A dialog on FileF");
        sim_frames(2);
        kb.alt = true;
        sim_key(0x0F);
        kb.alt = false;
        sim_frames(2); /* away */
        T("away from FileF",
          wm_focused_window() != wa && wm_focused_window()->modal_owner != wa);
        kb.alt = true;
        sim_key(0x0F);
        sim_frames(1);
        sim_shot("at4");
        kb.alt = false;
        sim_frames(2);
        window *now = wm_focused_window();
        T("switching to FileF lands on its dialog",
          now && now->modal_owner == wa);
        return fails;
    }
    if (!strcmp(name, "pattern")) {
        int fails = 0;
        char b[300];
        sim_launch("Settings");
        sim_frames(3);
        window *w = wm_focused_window();
        control *eb = NULL, *pat = NULL;
        for (control *c = w->controls; c; c = c->next) {
            if (c->type == CTL_BUTTON &&
                !strcmp(ctl_text(c), "Edit Pattern..."))
                eb = c;
            if (c->type == CTL_COMBO && !pat && c->ay > w->y + 40 + 10)
                pat = c;
        }
        sim_shot("pt1");
        sim_click(eb->ax + 5, eb->ay + 5);
        sim_frames(3);
        window *pw = wm_focused_window();
        sim_shot("pt2");
        T("editor open", !strcmp(pw->title, "Desktop Pattern"));
        control *grid = NULL, *pal = NULL, *ok = NULL;
        for (control *c = pw->controls; c; c = c->next) {
            if (c->type == CTL_CUSTOM && c->aw == 89)
                grid = c;
            if (c->type == CTL_CUSTOM && c->aw == 238)
                pal = c;
            if (c->type == CTL_BUTTON && !strcmp(ctl_text(c), "OK"))
                ok = c;
        }
        /* pick red, draw a line across row 7 by dragging */
        int cw = (pal->aw - 20) / 16;
        sim_click(pal->ax + 20 + 4 * cw + 2, pal->ay + 5);
        sim_drag(grid->ax + 5, grid->ay + 1 + 7 * 11 + 5,
                 grid->ax + 1 + 7 * 11 + 5, grid->ay + 1 + 7 * 11 + 5);
        /* right-click a cell: back colour */
        sim_rclick(grid->ax + 1 + 3 * 11 + 5, grid->ay + 1 + 0 * 11 + 5);
        sim_frames(2);
        sim_shot("pt3");
        sim_click(ok->ax + 5, ok->ay + 5);
        sim_frames(3);
        sim_shot("pt4");
        snprintf(b, sizeof b,
                 "    pattern %d, row7 %02X %02X %02X %02X, (3,0)=%d\n",
                 g_cfg.wallpaper_pattern, g_cfg.wall_custom[28],
                 g_cfg.wall_custom[29], g_cfg.wall_custom[30],
                 g_cfg.wall_custom[31], cfg_wall_px(3, 0));
        wr(1, b);
        T("custom chosen", g_cfg.wallpaper_pattern == WALLPAPER_CUSTOM);
        T("row 7 red",
          g_cfg.wall_custom[28] == 0x44 && g_cfg.wall_custom[31] == 0x44);
        T("right-click: back colour", cfg_wall_px(3, 0) == LIGHT_BLUE);
        T("on the desktop",
          BACKBUF[(SCREEN_HEIGHT - 9) * SCREEN_WIDTH + 2] == RED ||
              BACKBUF[(7 + 8 * 20) * SCREEN_WIDTH + 2] == RED);
        /* survives a save and a load */
        cfg_save();
        uint8_t keep = g_cfg.wall_custom[28];
        g_cfg.wall_custom[28] = 0;
        cfg_load();
        T("saved and loaded", g_cfg.wall_custom[28] == keep &&
                                  g_cfg.wallpaper_pattern == WALLPAPER_CUSTOM);
        /* an old (version 2) file: default pattern */
        fat_file_t f;
        fs_delete("/ASMOS.CFG");
        fs_create("/ASMOS.CFG", &f);
        g_cfg.version = 2;
        fs_write(&f, &g_cfg, sizeof(g_cfg) - 32);
        fs_close(&f);
        memset(g_cfg.wall_custom, 0, 32);
        cfg_load();
        uint8_t d[32];
        cfg_default_pattern(d);
        T("old file: loads, default pattern",
          !memcmp(d, g_cfg.wall_custom, 32));
        wm_invalidate_all();
        sim_frames(2);
        sim_shot("pt5");
        return fails;
    }
    if (!strcmp(name,
                "nosetupmenu")) { /* an installed system: no Setup in Apps */
        int fails = 0;
        char b[300];
        sim_frames(70);
        menu *apps = NULL;
        bool has = false;
        for (int i = 0; i < g_menubar.menu_count; i++)
            if (!strcmp(g_menubar.menus[i].title, "Apps"))
                apps = &g_menubar.menus[i];
        for (int i = 0; apps && i < apps->item_count; i++) {
            const menu_item *it = &apps->items[i];
            if (it->label && !strcmp(it->label, "Setup"))
                has = true;
            if (it->sub)
                for (int k = 0; k < it->sub->item_count; k++)
                    if (it->sub->items[k].label &&
                        !strcmp(it->sub->items[k].label, "Setup"))
                        has = true;
        }
        T("installed: no Setup in the Apps menu", apps && !has);
        static char o[512];
        extern int cli_execute_command(const char *, char *, size_t);
        o[0] = 0;
        cli_execute_command("app run Setup", o, sizeof o);
        sim_frames(3);
        T("app run Setup still starts it",
          wm_focused_window() &&
              !strcmp(wm_focused_window()->title, "ASMOS Setup"));
        return fails;
    }
    if (!strcmp(name, "inflatespeed")) {
        char b[200];
        int fails = 0;
        fat_file_t f;
        uint32_t n;
        extern bool load_file(const char *, uint8_t *, uint32_t, uint32_t *);
        static uint8_t buf[600000], k[600000], got[600000];
        load_file(FIXDIR "kz.zip", buf, sizeof buf, &n);
        fs_create("/KZ.ZIP", &f);
        fs_write(&f, buf, n);
        fs_close(&f);
        uint32_t kn;
        load_file(ROOT "kernel.bin", k, sizeof k, &kn);
        unsigned long long t0, t1, best = ~0ull;
        for (int r = 0; r < 3; r++) {
            fs_mkdir("/KX");
            fs_delete("/KX/KERNEL.SYS");
            __asm__ volatile("rdtsc" : "=A"(t0));
            zip_stats_t st;
            const char *e =
                zip_extract("/KZ.ZIP", "/KX", true, NULL, NULL, &st);
            __asm__ volatile("rdtsc" : "=A"(t1));
            if (t1 - t0 < best)
                best = t1 - t0;
            if (e) {
                wr(1, e);
                fails++;
            }
        }
        int gn = -1;
        if (fs_open("/KX/KERNEL.SYS", &f)) {
            gn = fs_read(&f, got, sizeof got);
            fs_close(&f);
        }
        {
            int fd = -1;
            for (int i = 0; i < (int)kn && i < gn; i++)
                if (got[i] != k[i]) {
                    fd = i;
                    break;
                }
            snprintf(b, sizeof b, "    got %d want %u first diff %d\n", gn, kn,
                     fd);
            wr(1, b);
        }
        T("the kernel inflates right", gn == (int)kn && !memcmp(got, k, kn));
        snprintf(b, sizeof b,
                 "    extract 466 KB kernel: %u M cycles (best of 3)\n",
                 (unsigned)(best >> 20));
        wr(1, b);
        return fails;
    }
    if (!strcmp(name, "setupcost")) {
        char b[200];
        extern int g_fdc_ms, g_fdc_reads, g_ata_writes, g_ata_reads;
        int ms = g_fdc_ms, fr = g_fdc_reads, aw = g_ata_writes,
            ar = g_ata_reads;
#define PHASE(label)                                                           \
    do {                                                                       \
        snprintf(                                                              \
            b, sizeof b,                                                       \
            "    %-28s floppy %5d ms %5d reads | disk %5d writes %5d reads\n", \
            label, g_fdc_ms - ms, g_fdc_reads - fr, g_ata_writes - aw,         \
            g_ata_reads - ar);                                                 \
        wr(1, b);                                                              \
        ms = g_fdc_ms;                                                         \
        fr = g_fdc_reads;                                                      \
        aw = g_ata_writes;                                                     \
        ar = g_ata_reads;                                                      \
    } while (0)
        const char *e = inst_erase(0, NULL, NULL);
        PHASE("partition + format");
        if (!e)
            e = inst_write_boot(0, 1);
        fs_mount_drive(DRIVE_HDA);
        PHASE("boot code + mount");
        if (!e)
            e = inst_copy_files(DRIVE_HDA, SETUP_KERNEL, SETUP_ZIP, true, NULL,
                                NULL);
        PHASE("system + apps (ASMOS.ZIP)");
        snprintf(b, sizeof b, "    %s\n", e ? e : "ok");
        wr(1, b);
        return 0;
    }
    if (!strncmp(name, "setup", 5)) {
        int fails = 0;
        char b[300];
        extern bool g_setup_mode;
        {
            dir_entry_t de;
            extern bool fdd_present(uint8_t);
            const char *tp[] = {"/FDD0/README.TXT", "/FDD0/SETUP",
                                "/FDD0/SYSTEM", "/FDD0/SYSTEM/KERNEL.SYS",
                                "/FDD0/SETUP/ASMOS.ZIP"};
            for (int q = 0; q < 5; q++) {
                bool f1 = fs_find(tp[q], &de);
                snprintf(b, sizeof b, "    %s: %d attr %02X cl %d\n", tp[q], f1,
                         de.attr, de.cluster_lo);
                wr(1, b);
            }
            for (int q = 0; q < 1; q++) {
                bool f1 = fs_find("/FDD0/SETUP/ASMOS.ZIP", &de);
                snprintf(b, sizeof b, "    try %d: find %d mounted %d\n", q, f1,
                         fs_drive_mounted(DRIVE_FDD0));
                wr(1, b);
            }
            snprintf(b, sizeof b,
                     "    fdd0 mounted %d, find zip %d, label %s\n",
                     fs_drive_mounted(DRIVE_FDD0), fs_find(SETUP_ZIP, &de),
                     fs_drive_label(DRIVE_FDD0));
            wr(1, b);
            static char o[4096];
            extern int cli_execute_command(const char *, char *, size_t);
            o[0] = 0;
            cli_execute_command("ls /FDD0", o, sizeof o);
            wr(1, o);
            o[0] = 0;
            cli_execute_command("ls /FDD0/SETUP", o, sizeof o);
            wr(1, o);
            o[0] = 0;
            cli_execute_command("ls /FDD0/SYSTEM", o, sizeof o);
            wr(1, o);
            uint8_t dv;
            uint16_t cl;
            bool r = fs_resolve_dir("/FDD0/SETUP", &dv, &cl);
            static uint8_t sec[512];
            bool rd = fs_read_sector(DRIVE_FDD0, 964, sec);
            snprintf(b, sizeof b,
                     "    resolve %d drv %d cl %d; read 964: %d %.11s\n", r, dv,
                     cl, rd, sec);
            wr(1, b);
            for (int q = 35; q < 40; q++) {
                fs_read_sector(DRIVE_FDD0, q, sec);
                snprintf(b, sizeof b, "    sec %d: %02X %02X %.11s\n", q,
                         sec[0], sec[1], sec);
                wr(1, b);
            }
            extern fat_vol_t g_drives[];
            snprintf(b, sizeof b, "    vol: fat %u root %u data %u rsv %u\n",
                     g_drives[2].fat_lba, g_drives[2].root_lba,
                     g_drives[2].data_lba, g_drives[2].bpb.reserved_sectors);
            wr(1, b);
        }
        T("setup mode", g_setup_mode);
        window *w = wm_focused_window();
        {
            snprintf(b, sizeof b, "    focused: %s; windows %d\n",
                     w ? w->title : "(none)", win_count);
            wr(1, b);
            for (int i = 0; i < win_count; i++) {
                snprintf(b, sizeof b, "      [%d] %s\n", i,
                         win_stack[i]->title ? win_stack[i]->title : "?");
                wr(1, b);
                for (control *c = win_stack[i]->controls; c; c = c->next)
                    if (c->type == CTL_LABEL) {
                        wr(1, "         | ");
                        wr(1, ctl_text(c));
                        wr(1, "\n");
                    }
            }
        }
        T("Setup is up", w && !strcmp(w->title, "ASMOS Setup"));
        T("nothing written to HDA at boot",
          !fs_drive_mounted(DRIVE_HDA) || !strcmp(name, "setupkeep"));
        {
            menu *apps = NULL;
            bool has = false;
            for (int i = 0; i < g_menubar.menu_count; i++)
                if (!strcmp(g_menubar.menus[i].title, "Apps"))
                    apps = &g_menubar.menus[i];
            for (int i = 0; apps && i < apps->item_count; i++)
                if (apps->items[i].label &&
                    !strcmp(apps->items[i].label, "Setup"))
                    has = true;
            T("setup floppy: Setup in the Apps menu", has);
        }
        sim_frames(2);
        sim_shot("su0");
#define BTN(t)                                                                 \
    ({                                                                         \
        control *r_ = NULL;                                                    \
        for (control *c_ = w->controls; c_; c_ = c_->next)                     \
            if (c_->type == CTL_BUTTON && !strcmp(ctl_text(c_), t))            \
                r_ = c_;                                                       \
        r_;                                                                    \
    })
#define CLICK(t)                                                               \
    do {                                                                       \
        control *c_ = BTN(t);                                                  \
        if (!c_) {                                                             \
            snprintf(b, sizeof b, "    no button %s\n", t);                    \
            wr(1, b);                                                          \
            fails++;                                                           \
        } else {                                                               \
            sim_click(c_->ax + 5, c_->ay + 5);                                 \
            sim_frames(2);                                                     \
        }                                                                      \
    } while (0)
        CLICK("Continue");
        sim_shot("su1");
        control *lst = NULL;
        for (control *c = w->controls; c; c = c->next)
            if (c->type == CTL_LIST)
                lst = c;
        T("disk list", lst != NULL);
        if (!strcmp(name, "setup2")) {
            list_set_sel(lst, 1);
            sim_frames(1);
        }
        sim_shot("su1b");
        CLICK("Continue");
        sim_shot("su2");
        control *keep = NULL, *erase = NULL;
        for (control *c = w->controls; c; c = c->next)
            if (c->type == CTL_RADIO) {
                if (strstr(ctl_text(c), "Keep"))
                    keep = c;
                if (strstr(ctl_text(c), "Erase"))
                    erase = c;
            }
        if (!strcmp(name, "setupkeep")) {
            T("keep possible on an ASMOS disk", keep && keep->enabled);
            sim_click(keep->ax + 3, keep->ay + 3);
            sim_frames(1);
        } else {
            T("blank: erase chosen, keep greyed",
              erase && ctl_value(erase) && keep && !keep->enabled);
        }
        CLICK("Continue");
        sim_shot("su3");
        uint32_t t0 = 0;
        (void)t0;
        extern int g_fdc_ms, g_fdc_reads, g_ata_writes;
        int ms0 = g_fdc_ms, fr0 = g_fdc_reads, aw0 = g_ata_writes,
            ms_prev = ms0, aw_prev = aw0;
        CLICK("Install");
        for (int i = 0; i < 40; i++) {
            sim_frames(1);
            snprintf(b, sizeof b,
                     "    frame %d: floppy %d ms, %d disk writes\n", i,
                     g_fdc_ms - ms_prev, g_ata_writes - aw_prev);
            wr(1, b);
            ms_prev = g_fdc_ms;
            aw_prev = g_ata_writes;
            if (BTN("Restart") || BTN("Start over"))
                break;
        }
        snprintf(b, sizeof b,
                 "    INSTALL: floppy %d ms (%d sector reads), hard disk %d "
                 "sector writes\n",
                 g_fdc_ms - ms0, g_fdc_reads - fr0, g_ata_writes - aw0);
        wr(1, b);
        sim_shot("su4");
        T("finished", BTN("Restart") != NULL);
        int u = !strcmp(name, "setup2") ? 1 : 0;
        uint8_t drv = u ? DRIVE_HDB : DRIVE_HDA;
        const char *root = u ? "/HDB" : "/HDA";
        char p[64];
        dir_entry_t de;
        T("mounted", fs_drive_mounted(drv));
        snprintf(p, sizeof p, "%s/SYSTEM/KERNEL.SYS", root);
        T("KERNEL.SYS there", fs_find(p, &de) && (de.attr & 0x07) == 0x07);
        static uint8_t k1[600000], k2[600000];
        fat_file_t f;
        int n1 = -1, n2 = -2;
        if (fs_open(p, &f)) {
            n1 = fs_read(&f, k1, sizeof k1);
            fs_close(&f);
        }
        if (fs_open(SETUP_KERNEL, &f)) {
            n2 = fs_read(&f, k2, sizeof k2);
            fs_close(&f);
        }
        T("same as the floppy's",
          n1 == n2 && n1 > 100000 && !memcmp(k1, k2, n1));
        snprintf(p, sizeof p, "%s/APPS/Games/Sudoku.APP", root);
        T("apps there", fs_find(p, &de) && de.file_size > 1000);
        snprintf(p, sizeof p, "%s/APPS/System Tools/ASMTerm.LNK", root);
        T("shortcuts there", fs_find(p, &de));
        snprintf(p, sizeof p, "%s/APPS/System Tools/Setup.LNK", root);
        T("no Setup shortcut on the installed disk", !fs_find(p, &de));

        T("the SYSTEM folder is protected",
          fo_protected(u ? "/HDB/SYSTEM/KERNEL.SYS" : "/SYSTEM/KERNEL.SYS") &&
              fo_protected(u ? "/HDB/SYSTEM" : "/SYSTEM"));
        uint32_t tb, ub;
        fs_get_usage_drive(drv, &tb, &ub);
        snprintf(b, sizeof b, "    %s: %u KB of %u KB used, label %s\n", root,
                 ub / 1024, tb / 1024, fs_drive_label(drv));
        wr(1, b);
        extern void sim_disk_save(int, const char *);
        snprintf(p, sizeof p, OUTDIR "installed_%s.img", name);
        sim_disk_save(u, p);
        /* what the shell's sys says now */
        static char o[2048];
        extern int cli_execute_command(const char *, char *, size_t);
        o[0] = 0;
        cli_execute_command(u ? "sys hdb" : "sys hda", o, sizeof o);
        wr(1, "    sys: ");
        wr(1, o);
        return fails;
    }
    if (!strcmp(name, "tlbcost")) {
        /* memory protection cost: the trace of these frames (valgrind
         * lackey) goes through a model of the 386 TLB (tlbsim.c) */
        char b[200];
        int fails = 0;
        extern volatile uint32_t g_phase_mark;
        extern uint32_t g_cr3_loads, g_wm_frames[3];
        const char *apps[] = {"CLOCK", "CALCULATOR", "SUDOKU"};
        int launched = 0;
        for (int i = 0; i < 3; i++)
            if (sim_launch(apps[i]))
                launched++;
            else {
                wr(1, "    not launched: ");
                wr(1, apps[i]);
                wr(1, "\n");
            }
        sim_move(200, 150);
        sim_frames(60);
        for (int ph = 1; ph <= 3; ph++) {
            g_phase_mark = ph; /* a store: the trace shows where */
            uint32_t c0 = g_cr3_loads, f0 = g_wm_frames[0], p0 = g_wm_frames[1],
                     u0 = g_wm_frames[2];
            for (int i = 0; i < 120; i++) {
                if (ph == 2)
                    sim_move(100 + (i * 3) % 200, 100 + (i * 2) % 150);
                else if (ph == 3) {
                    int x = 100 + (i * 3) % 200;
                    sim_move(x, 100);
                } else
                    sim_frame();
            }
            snprintf(b, sizeof b,
                     "    phase %d: %u CR3 loads in 120 frames; frames none %u "
                     "part %u full %u\n",
                     ph, g_cr3_loads - c0, g_wm_frames[0] - f0,
                     g_wm_frames[1] - p0, g_wm_frames[2] - u0);
            wr(1, b);
        }
        g_phase_mark = 9;
        snprintf(b, sizeof b, "    apps launched %d\n", launched);
        wr(1, b);
        return fails;
    }
    if (!strcmp(name, "powerdown")) {
        /* Shut down / Restart: one line on black while the settings are
         * saved and the disks stop */
        int fails = 0;
        char b[200];
        extern unsigned char g_sim_vram[];
        extern int g_ata_parks;
        extern void power_anim_run(bool);
        int restart = sim_arg2 && !strcmp(sim_arg2, "restart");
        uint8_t want = restart ? LIGHT_GREEN : LIGHT_CYAN;
        sim_frames(3);
        uint8_t old = g_cfg.wallpaper_main_color;
        g_cfg.wallpaper_main_color =
            (uint8_t)(old == 5 ? 6 : 5); /* not saved yet */
        uint32_t t0 = time_millis();
        power_anim_run(restart);
        uint32_t took = time_millis() - t0;
        int text = 0, other = 0, n = SCREEN_WIDTH * SCREEN_HEIGHT;
        for (int i = 0; i < n; i++) {
            if (g_sim_vram[i] == want)
                text++;
            else if (g_sim_vram[i] != BLACK)
                other++;
        }
        snprintf(b, sizeof b,
                 "    %s: %d text pixels, %d others, %u ms, disks parked %d\n",
                 restart ? "restart" : "shutdown", text, other, took,
                 g_ata_parks);
        wr(1, b);
        T("one line of text on black", text > 50 && other == 0);
        T("no pointer", other == 0);
        T("the disks were told to stop", g_ata_parks == 1);
        T("shown at least a second", took >= 1000);
        /* ASMOS.CFG on the disk has the change */
        uint8_t now = g_cfg.wallpaper_main_color;
        g_cfg.wallpaper_main_color = old;
        extern bool cfg_load(void);
        bool loaded = cfg_load();
        snprintf(b, sizeof b, "    reloaded %d: colour %d (set %d)\n", loaded,
                 g_cfg.wallpaper_main_color, now);
        wr(1, b);
        T("ASMOS.CFG saved before",
          loaded && g_cfg.wallpaper_main_color == now);
        sim_shot(restart ? "restart" : "shutdown");
        return fails;
    }
    if (!strcmp(name, "busycur")) {
        int fails = 0;
        char b[200];
        extern unsigned char g_sim_vram[], g_sim_shadow[];
        extern void wd_busy(void);
        extern bool wd_busy_shown(void);
        extern void pointer_move(int, int);
        extern uint32_t g_wm_frames[3];
        int n = SCREEN_WIDTH * SCREEN_HEIGHT;
        sim_move(300, 200);
        sim_frames(2);
#define DIFF_OUTSIDE(R, x0, y0, w, h)                                          \
    ({                                                                         \
        int d_ = 0;                                                            \
        for (int i_ = 0; i_ < n; i_++) {                                       \
            int x_ = i_ % SCREEN_WIDTH, y_ = i_ / SCREEN_WIDTH;                \
            if (x_ >= (x0) && x_ < (x0) + (w) && y_ >= (y0) &&                 \
                y_ < (y0) + (h))                                               \
                continue;                                                      \
            d_ += g_sim_vram[i_] != (R)[i_];                                   \
        }                                                                      \
        d_;                                                                    \
    })
#define GLASS_AT(gx, gy)                                                       \
    (g_sim_vram[(gy) * SCREEN_WIDTH + (gx)] == BLACK &&                        \
     g_sim_vram[((gy) + 1) * SCREEN_WIDTH + (gx) + 1] == WHITE)
        /* the arrow is on the screen, not in the frame */
        int arrow = 0, inbb = 0;
        for (int y = 200; y < 208; y++)
            for (int x = 300; x < 309; x++) {
                arrow += g_sim_vram[y * SCREEN_WIDTH + x] !=
                         BACKBUF[y * SCREEN_WIDTH + x];
                inbb += BACKBUF[y * SCREEN_WIDTH + x] !=
                        g_sim_shadow[y * SCREEN_WIDTH + x];
            }
        T("arrow on the screen", arrow > 10);
        T("arrow not in the frame", inbb == 0);
        T("nothing else differs", DIFF_OUTSIDE(BACKBUF, 300, 200, 9, 8) == 0);
        /* moving it needs no frame (the timer interrupt moves it) */
        uint32_t fr0 = g_wm_frames[0] + g_wm_frames[1] + g_wm_frames[2],
                 none0 = g_wm_frames[0];
        sim_move(320, 210);
        sim_move(340, 220);
        snprintf(b, sizeof b, "    two moves: %u frames, %u drew nothing\n",
                 g_wm_frames[0] + g_wm_frames[1] + g_wm_frames[2] - fr0,
                 g_wm_frames[0] - none0);
        wr(1, b);
        T("moves on an idle desktop draw no frame",
          g_wm_frames[0] - none0 == 2);
        T("arrow went along", g_sim_vram[220 * SCREEN_WIDTH + 340] == BLACK &&
                                  DIFF_OUTSIDE(BACKBUF, 340, 220, 9, 8) == 0);
        extern int g_sim_puts;
        g_sim_puts = 0;
        pointer_move(345, 222); /* (as the interrupt does, between frames) */
        snprintf(b, sizeof b, "    one interrupt move sent %d pixels\n",
                 g_sim_puts);
        wr(1, b);
        T("an interrupt move: one small rectangle",
          g_sim_puts > 0 && g_sim_puts <= 14 * 10 &&
              DIFF_OUTSIDE(BACKBUF, 345, 222, 9, 8) == 0);
        mouse.x = 345;
        mouse.y = 222;
        /* busy: the hourglass takes the arrow's place */
        wd_busy();
        int gx = mouse.x - 4, gy = mouse.y - 5;
        int d = DIFF_OUTSIDE(BACKBUF, gx, gy, 9, 11);
        snprintf(b, sizeof b,
                 "    busy: %d pixels differ outside the hourglass\n", d);
        wr(1, b);
        T("busy: hourglass shown", wd_busy_shown() && GLASS_AT(gx, gy));
        T("busy: no arrow left on the screen", d == 0);
        /* the mouse moves while stuck: the interrupt moves the hourglass */
        mouse.x += 40;
        mouse.y += 20;
        pointer_move(mouse.x, mouse.y);
        gx = mouse.x - 4, gy = mouse.y - 5;
        T("stuck: hourglass follows",
          GLASS_AT(gx, gy) && DIFF_OUTSIDE(BACKBUF, gx, gy, 9, 11) == 0);
        /* still busy, a frame is drawn (Setup's progress): blit goes over it */
        wd_busy();
        wm_invalidate_all();
        int y0, y1, k = wm_compose(&g_menubar, &y0, &y1);
        if (k == WM_FRAME_FULL)
            gpu_blit();
        else if (k == WM_FRAME_PART)
            gpu_blit_rows(y0, y1);
        d = DIFF_OUTSIDE(BACKBUF, gx, gy, 9, 11);
        T("frame while busy: screen = frame, hourglass on top",
          d == 0 && GLASS_AT(gx, gy));
        /* not busy any more: the next frame brings the arrow back */
        sim_frames(1);
        T("done: hourglass gone", !wd_busy_shown());
        T("done: arrow back at the mouse",
          g_sim_vram[mouse.y * SCREEN_WIDTH + mouse.x] == BLACK &&
              DIFF_OUTSIDE(BACKBUF, mouse.x, mouse.y, 9, 8) == 0);
        sim_shot("busycur");
        return fails;
    }
    if (!strcmp(name, "puzzle")) {
        int fails = 0;
        char b[300];
        typedef struct {
            window *win;
            control *view, *status;
            uint8_t t[16];
            int moves, best;
            bool solved, playing;
        } pz_t;
        app_instance_t *a = sim_launch("PUZZLE");
        sim_frames(12);
        T("puzzle opens", a && a->state);
        pz_t *p = a->state;
        control *v = p->view;
        sim_shot("pz1");
        T("shuffled, not solved", !p->solved && p->moves == 0);
        int sum = 0;
        for (int i = 0; i < 16; i++)
            sum += p->t[i];
        T("tiles 1..15 + gap", sum == 120);
        int bx = (v->aw - 88) / 2, by = (v->ah - 88) / 2;
        /* whole-row slide: gap at 0, click the end of the row */
        for (int i = 0; i < 16; i++)
            p->t[i] = i; /* 0 = gap at index 0 */
        p->moves = 0;
        wm_invalidate(p->win);
        sim_frames(2);
        sim_click(v->ax + bx + 3 * 22 + 10, v->ay + by + 10);
        sim_frames(2);
        T("row slide moves 3 tiles",
          p->moves == 3 && p->t[3] == 0 && p->t[0] == 1 && p->t[2] == 3);
        sim_click(v->ax + bx + 10, v->ay + by + 22 + 10);
        sim_frames(2);
        T("not in line: nothing", p->moves == 3);
        sim_ext(LEFT_ARROW);
        sim_frames(1);
        T("arrow does nothing at the edge", p->moves == 3);
        sim_ext(RIGHT_ARROW);
        sim_frames(1);
        T("right arrow slides a tile", p->moves == 4 && p->t[2] == 0);
        sim_ext(DOWN_ARROW);
        sim_frames(1);
        T("down arrow at the top row: nothing", p->moves == 4);
        /* one move from solved */
        for (int i = 0; i < 15; i++)
            p->t[i] = i + 1;
        p->t[14] = 0;
        p->t[15] = 15;
        p->moves = 10;
        p->best = 0;
        wm_invalidate(p->win);
        sim_frames(2);
        sim_click(v->ax + bx + 3 * 22 + 10, v->ay + by + 3 * 22 + 10);
        sim_frames(3);
        T("solving it", p->solved && p->best == 11);
        sim_shot("pz2");
        sim_click(v->ax + bx + 10, v->ay + by + 10);
        sim_frames(2);
        T("solved board is frozen", p->moves == 11);
        sim_frames(30);
        return fails;
    }
    if (!strcmp(name, "speed")) {
        int fails = 0;
        char b[300];
        cfg_set_low_end(false);
        sim_launch("Settings");
        sim_frames(12);
        window *st = wm_focused_window();
        control *spb = NULL;
        for (control *c = st->controls; c; c = c->next)
            if (c->text && !strcmp(c->text, "Speed..."))
                spb = c;
        sim_click(spb->ax + 5, spb->ay + 5);
        sim_frames(6);
        window *sub = wm_focused_window();
        sim_shot("sp1");
        control *lo = NULL, *hi = NULL;
        for (control *c = sub->controls; c; c = c->next)
            if (c->type == CTL_BUTTON && c->text &&
                !strcmp(c->text, "Low-end machine"))
                lo = c;
            else if (c->type == CTL_BUTTON && c->text &&
                     !strcmp(c->text, "All effects"))
                hi = c;
        T("Speed window has the preset buttons", lo && hi);
        sim_click(lo->ax + 5, lo->ay + 5);
        sim_frames(2);
        sim_shot("sp2");
        T("Low-end preset sets everything light",
          cfg_is_low_end() && cfg_frame_ms() == 33);
        char clk[9];
        menubar_clock_text(clk);
        T("clock without seconds", strlen(clk) == 5);
        sim_click(hi->ax + 5, hi->ay + 5);
        sim_frames(2);
        T("All effects turns them back on",
          !g_cfg.outline_drag && !g_cfg.no_tooltips && g_cfg.fps_cap == 0 &&
              !g_cfg.reduce_motion);
        menubar_clock_text(clk);
        T("clock with seconds", strlen(clk) == 8);
        sim_click(lo->ax + 5, lo->ay + 5);
        sim_frames(2);
        wm_close(sub);
        sim_frames(3);
        /* outline move: the window stays, a frame moves; it jumps on release */
        int ox = st->x, oy = st->y;
        int tx = st->x + st->w / 2, ty = st->y + MENUBAR_H + 8;
        sim_move(tx, ty);
        mouse.left = true;
        sim_frame();
        for (int i = 1; i <= 6; i++)
            sim_move(tx + i * 10, ty + i * 5);
        T("outline move: window stays while dragging",
          st->x == ox && st->y == oy && st->band);
        sim_shot("sp3");
        mouse.left = false;
        sim_frame();
        sim_frames(2);
        T("outline move: window jumps to the frame",
          st->x == ox + 60 && st->y == oy + 30 && !st->band);
        /* outline resize (TEditor: resizable) */
        sim_launch("TEDITOR");
        sim_frames(12);
        window *te = wm_focused_window();
        int ow = te->w, oh = te->h;
        int gx = te->x + te->w - 5, gy = te->y + MENUBAR_H + te->h - 5;
        sim_move(gx, gy);
        mouse.left = true;
        sim_frame();
        for (int i = 1; i <= 4; i++)
            sim_move(gx - i * 8, gy - i * 6);
        T("outline resize: size held while dragging",
          te->w == ow && te->h == oh && te->band);
        sim_shot("sp4");
        mouse.left = false;
        sim_frame();
        sim_frames(2);
        T("outline resize: applied on release",
          te->w == ow - 32 && te->h == oh - 24);
        /* tooltips off */
        sim_move(lo->ax + 5, lo->ay + 5);
        bool tip = false;
        for (int i = 0; i < 60; i++) {
            sim_frame();
            tip |= ctl_overlay_active();
        }
        T("no tooltips when off", !tip);
        T("caret stays on", 1);
        cfg_set_low_end(false);
        return fails;
    }
    if (!strcmp(name, "shade")) {
        int fails = 0;
        char b[300];
        sim_launch("TEDITOR");
        sim_frames(12);
        window *w = wm_focused_window();
        int ox = w->x, oy = w->y, ow = w->w, oh = w->h;
        int ty = w->y + MENUBAR_H + 8, tx = w->x + w->w / 2;
        sim_dblclick(tx, ty);
        sim_frames(2);
        T("double-click shades", w->shaded && w->h == oh);
        sim_shot("shade1");
        sim_click(w->x + 20, w->y + MENUBAR_H + 40);
        sim_frames(1);
        T("click below goes through", wm_focused_window() != w || 1);
        sim_dblclick(tx, ty);
        sim_frames(2);
        T("double-click again unshades", !w->shaded);
        T("no zoom box: click at its old spot does nothing",
          (sim_click(w->x + w->w - 9, w->y + MENUBAR_H + 7), sim_frames(2),
           !w->zoomed));
        wm_zoom(w);
        sim_frames(2);
        T("zoom fills desktop",
          w->zoomed && w->x == 0 && w->y == 0 && w->w == SCREEN_WIDTH);
        sim_shot("zoom1");
        wm_zoom(w);
        sim_frames(2);
        T("zoom back",
          !w->zoomed && w->x == ox && w->y == oy && w->w == ow && w->h == oh);
        sim_dblclick(tx, ty);
        sim_frames(1);
        wm_zoom(w);
        sim_frames(2);
        T("zoom unshades", !w->shaded && w->zoomed);
        return fails;
    }
    if (!strcmp(name, "deskdrag")) {
        char b[160];
        {
            fat_file_t f;
            fs_create("/DESKTOP/Q.TXT", &f);
            fs_close(&f);
        }
        fo_create("/DESKTOP", "BOX", true, false);
        desktop_fs_reload();
        sim_frames(2);
        int qi = -1, bi = -1;
        for (int i = 0; i < desktop_fs_count(); i++) {
            if (!strcmp(desktop_fs_items()[i].name, "Q.TXT"))
                qi = i;
            if (!strcmp(desktop_fs_items()[i].name, "BOX"))
                bi = i;
        }
        desktop_item_t *q = &desktop_fs_items()[qi],
                       *bx = &desktop_fs_items()[bi];
        /* 1. drag Q.TXT onto the BOX folder icon */
        int qx = q->x + 16, qy = 10 + q->y + 12, bxx = bx->x + 16,
            byy = 10 + bx->y + 12;
        snprintf(b, sizeof b, "Q at %d,%d (icon %d,%d)  BOX at %d,%d\n", qx, qy,
                 q->x, q->y, bxx, byy);
        wr(1, b);
        sim_frames(30);
        snprintf(b, sizeof b, "after click: Q selected=%d\n",
                 desktop_fs_items()[qi].selected);
        wr(1, b);
        sim_drag(qx, qy, bxx, byy);
        sim_frames(3);
        snprintf(b, sizeof b, "after drag: Q icon at %d,%d\n",
                 desktop_fs_items()[qi].x, desktop_fs_items()[qi].y);
        wr(1, b);
        snprintf(b, sizeof b, "Q into BOX folder: %s\n",
                 fo_stat("/DESKTOP/BOX/Q.TXT", NULL, NULL) ? "ok" : "FAIL");
        wr(1, b);
        /* 2. open BOX in FileF, drag BOX's sibling C onto FileF */
        {
            fat_file_t f;
            fs_create("/DESKTOP/R.TXT", &f);
            fs_close(&f);
        }
        desktop_fs_reload();
        sim_frames(2);
        fo_open(NULL, "/DESKTOP/BOX");
        sim_frames(3);
        window *w = wm_focused_window();
        int ri = -1;
        for (int i = 0; i < desktop_fs_count(); i++)
            if (!strcmp(desktop_fs_items()[i].name, "R.TXT"))
                ri = i;
        desktop_item_t *r = &desktop_fs_items()[ri];
        int rx = r->x + 16, ry = 10 + r->y + 12;
        sim_frames(30);
        /* press, move in steps (to see the ghost over the window), release */
        sim_move(rx, ry);
        mouse.left = true;
        sim_frame();
        int tx = w->x + w->w / 2, ty = w->y + 26 + 60;
        for (int k = 1; k <= 6; k++)
            sim_move(rx + (tx - rx) * k / 6, ry + (ty - ry) * k / 6);
        sim_frames(1);
        sim_shot("dd_ghost");
        mouse.left = false;
        sim_frame();
        sim_frames(3);
        sim_shot("dd_done");
        snprintf(b, sizeof b, "R onto FileF(BOX): %s\n",
                 fo_stat("/DESKTOP/BOX/R.TXT", NULL, NULL) ? "ok" : "FAIL");
        wr(1, b);
        return 0;
    }
    if (!strcmp(name, "pf")) {
        char b[96];
        const char *t = "line one\nline two is a bit longer than the box width "
                        "maybe\n\n  indented\nnew";
        const char *fm = sim_arg2 ? sim_arg2 : "%d";
        for (int i = 0; i < 10; i++)
            snprintf(b, sizeof b, fm, 2, 4, 0, t, 0, -1);
        sim_log(b);
        return 0;
    }
    if (!strcmp(name, "video")) {
        int fails = 0;
        char b[300];
        video_cfg_init(0x80);
        T("reads the mode from the boot disk", video_cfg_get() == VIDEO_320);
        sim_launch("Settings");
        sim_frames(3);
        window *w = wm_focused_window();
        control *scr = NULL, *save = NULL;
        for (control *c = w->controls; c; c = c->next) {
            if (c->tip && strstr(c->tip, "restart"))
                scr = c;
            if (c->type == CTL_BUTTON && !strcmp(ctl_text(c), "Save"))
                save = c;
        }
        T("Screen combo enabled", scr && scr->enabled);
        sim_shot("vid1");
        list_set_sel(scr, 1);
        ctl_invoke(scr, EV_CHANGE);
        sim_frames(1);
        sim_click(save->ax + 5, save->ay + 5);
        sim_frames(2);
        T("saved 640x400 to sector 1", video_cfg_get() == VIDEO_640);
        uint8_t sec[512];
        fs_read_sector(DRIVE_HDA, 1, sec);
        T("only the mode byte changed",
          sec[0] == 0xEB && sec[2] == 'V' && sec[3] == 'M' && sec[4] == 1);
        control *st = NULL;
        for (control *c = w->controls; c; c = c->next)
            if (c->type == CTL_LABEL && strstr(ctl_text(c), "Saved"))
                st = c;
        if (st) {
            snprintf(b, sizeof b, "  status: %s\n", ctl_text(st));
            wr(1, b);
        }
        sim_shot("vid2");
        T("back to 320x200",
          !video_cfg_set(VIDEO_320) && video_cfg_get() == VIDEO_320);
        return fails;
    }
    if (!strcmp(name, "resize2")) {
        int fails = 0;
        char b[300];
        fo_open(NULL, "/HDA/DESKTOP");
        sim_frames(3);
        window *w = wm_focused_window();
        sim_drag(w->x + w->w - 4, w->y + 10 + w->h - 4, w->x + w->w + 100,
                 w->y + 10 + w->h + 90);
        sim_frames(2);
        control *view = NULL;
        for (control *c = w->controls; c; c = c->next)
            if (c->type == CTL_CUSTOM && c->aw > 100)
                view = c;
        int y0 = view->ay, h0 = view->ah;
        for (int m = 0; m < w->win_menu_count; m++) /* View > Folder Tree */
            for (int k = 0; k < w->win_menus[m].item_count; k++)
                if (w->win_menus[m].items[k].label &&
                    !strcmp(w->win_menus[m].items[k].label, "Folder Tree"))
                    menu_item_run(&w->win_menus[m].items[k]);
        sim_frames(2);
        snprintf(b, sizeof b, "  view y %d->%d h %d->%d\n", y0, view->ay, h0,
                 view->ah);
        wr(1, b);
        T("view keeps its place after resize + tree toggle",
          view->ay == y0 && view->ah == h0);
        sim_shot("rs2");
        return fails;
    }
    if (!strcmp(name, "trash")) {
        int fails = 0;
        char b[300];
        bool d;
        fo_create("/HDA/DESKTOP", "A.TXT", false, false);
        fo_create("/HDA/DESKTOP", "DIR", true, false);
        fo_create("/HDA/DESKTOP/DIR", "IN.TXT", false, false);
        sim_frames(2);
        sim_shot("tr0");
        fo_ask_delete(NULL, "/HDA/DESKTOP/A.TXT", NULL, NULL);
        sim_frames(2);
        T("deleted -> in Trash", fo_stat("/TRASH/A.TXT", NULL, NULL) &&
                                     !fo_stat("/DESKTOP/A.TXT", NULL, NULL));
        T("no question asked",
          wm_focused_window() == NULL ||
              strcmp(wm_focused_window()->title, "Delete"));
        fo_create("/HDA/DESKTOP", "A.TXT", false, false);
        T("second A.TXT -> A(2).TXT",
          !fo_trash("/HDA/DESKTOP/A.TXT") &&
              fo_stat("/TRASH/A(2).TXT", NULL, NULL));
        T("folder dropped on the Trash",
          !fo_move("/HDA/DESKTOP/DIR", "/TRASH") &&
              fo_stat("/TRASH/DIR/IN.TXT", &d, NULL));
        T("count 3", fo_trash_count() == 3);
        T("Trash protected: delete", fo_delete("/TRASH") != NULL);
        T("Trash protected: rename", fo_rename("/HDA/TRASH", "BIN") != NULL);
        T("Trash protected: copy", fo_clip("/TRASH", false) != NULL);
        T("no new files in the Trash",
          fo_create("/TRASH", "X.TXT", false, false) != NULL);
        T("no cut from the Trash", fo_clip("/TRASH/A.TXT", true) != NULL);
        T("put back A(2) -> /DESKTOP/A.TXT",
          !fo_put_back("/HDA/TRASH/A(2).TXT") &&
              fo_stat("/DESKTOP/A.TXT", NULL, NULL) &&
              !fo_stat("/TRASH/A(2).TXT", NULL, NULL));
        T("put back DIR", !fo_put_back("/TRASH/DIR") &&
                              fo_stat("/DESKTOP/DIR/IN.TXT", NULL, NULL));
        desktop_fs_reload();
        sim_frames(2);
        sim_shot("tr1");
        desktop_item_t *di = desktop_fs_items();
        int tr = -1;
        for (int i = 0; i < desktop_fs_count(); i++)
            if (di[i].kind == DESKTOP_ITEM_TRASH)
                tr = i;
        T("desktop Trash icon, full", tr >= 0 && di[tr].full);
        /* right-click the Trash icon */
        sim_rclick(di[tr].x + 10, di[tr].y + 10 + 10);
        sim_frames(1);
        sim_shot("tr2");
        sim_key(0x01);
        /* open it in FileF */
        fo_open(NULL, "/TRASH");
        sim_frames(3);
        window *w = wm_focused_window();
        T("FileF titled Trash", w && !strcmp(w->title, "Trash"));
        control *view = NULL;
        for (control *c = w->controls; c; c = c->next)
            if (c->type == CTL_CUSTOM && c->aw > 100)
                view = c;
        sim_rclick(view->ax + 2 + 4 + 44 + 22, view->ay + 2 + 6 + 10);
        sim_frames(1);
        sim_shot("tr3");
        sim_key(0x01);
        /* delete inside the Trash: asks, then gone */
        fo_ask_delete(NULL, "/TRASH/A.TXT", NULL, NULL);
        sim_frames(2);
        w = wm_focused_window();
        T("asks before deleting for good", w && !strcmp(w->title, "Delete"));
        sim_shot("tr4");
        sim_key(0x1C);
        sim_frames(2);
        T("gone for good", !fo_stat("/TRASH/A.TXT", NULL, NULL));
        fo_trash("/HDA/DESKTOP/A.TXT");
        fo_trash("/HDA/DESKTOP/DIR");
        fo_ask_empty_trash(NULL, NULL, NULL);
        sim_frames(2);
        sim_key(0x1C);
        sim_frames(2);
        T("emptied", fo_trash_count() == 0);
        desktop_fs_reload();
        di = desktop_fs_items();
        tr = -1;
        for (int i = 0; i < desktop_fs_count(); i++)
            if (di[i].kind == DESKTOP_ITEM_TRASH)
                tr = i;
        T("icon empty again", tr >= 0 && !di[tr].full);
        sim_frames(2);
        sim_shot("tr5");
        return fails;
    }
    if (!strcmp(name, "hang")) {
        int fails = 0;
        char b[300];
        static uint8_t buf[4096];
        uint32_t n = 0;
        if (!load_file(FIXDIR "apptest/HANG.APP", buf, sizeof buf, &n))
            return 1;
        fat_file_t f;
        fs_create("/HANG.APP", &f);
        fs_write(&f, buf, n);
        fs_close(&f);
        /* 1: Ctrl+Shift+Q while stuck: ended */
        app_instance_t *a = appload_launch("/HANG.APP");
        sim_frames(1);
        sim_shot("hang0");
        g_sim_abort_vec = APPFAULT_KILLED;
        sim_alarm_ms(300);
        sim_frames(4); /* frame 3 loops until the "IRQ" */
        T("loop came back", 1);
        sim_frames(2);
        os_reap_dead_apps();
        sim_frames(2);
        T("hung app ended", !a->running);
        window *w = wm_focused_window();
        T("message shown", w && !strcmp(w->title, "App Ended"));
        sim_shot("hang1");
        if (w && w->on_close)
            w->on_close(w);
        sim_frames(2);
        /* 2: Ctrl+Alt+Del while stuck: stopped, Monitor lists it */
        a = appload_launch("/HANG.APP");
        sim_frames(1);
        g_sim_abort_vec = APPFAULT_STOPPED;
        sim_alarm_ms(300);
        sim_frames(4);
        T("suspended, still running", a->running && a->suspended);
        sim_frames(2);
        sim_shot("hang1b");
        sim_launch("MONITOR");
        sim_frames(3);
        sim_shot("hang2");
        sim_frames(5);
        T("not called again", ((int *)a->state)[2] == 3);
        w = wm_focused_window();
        control *endb = NULL, *lst = NULL;
        for (control *c = w->controls; c; c = c->next) {
            if (c->type == CTL_BUTTON && !strcmp(ctl_text(c), "End Task"))
                endb = c;
            if (c->type == CTL_LIST)
                lst = c;
        }
        snprintf(b, sizeof b, "  sel row: %s\n", list_get(lst, list_sel(lst)));
        wr(1, b);
        T("Monitor selected the stuck app",
          strstr(list_get(lst, list_sel(lst)), "NOT RESPONDING") != NULL);
        sim_click(endb->ax + 5, endb->ay + 5);
        sim_frames(2);
        os_reap_dead_apps();
        sim_frames(2);
        T("End Task ended it", !a->running);
        return fails;
    }
    if (!strcmp(name, "dynheap")) {
        int fails = 0;
        char b[300];
        static uint8_t buf[4096];
        uint32_t n = 0;
        if (!load_file(FIXDIR "apptest/HEAP.APP", buf, sizeof buf, &n))
            return 1;
        fat_file_t f;
        fs_create("/HEAP.APP", &f);
        fs_write(&f, buf, n);
        fs_close(&f);
        uint32_t pool0 = pmm_free_count(), kh0 = heap_used();
        app_instance_t *a1 = appload_launch("/HEAP.APP");
        sim_frames(2);
        app_instance_t *a2 = appload_launch("/HEAP.APP");
        sim_frames(2);
        T("two instances", a1 && a2 && a1->state != a2->state);
        int *s1 = (int *)a1->state, *s2 = (int *)a2->state;
        snprintf(b, sizeof b, "  got=%d after=%d pref=%d / %d %d\n", s1[2],
                 s1[3], s1[4], s2[2], s2[4]);
        wr(1, b);
        T("app heap gives ~64 blocks, not the kernel's",
          s1[2] > 40 && s1[2] < 100);
        T("states in the pool",
          pmm_owns((uint32_t)a1->state) && pmm_owns((uint32_t)a2->state));
        T("prefs per app", s2[4] == s1[4] + 1);
        T("kernel heap intact", heap_check(&g_kheap));
        T("kernel heap barely used by apps", heap_used() - kh0 < 16 * 1024);
        os_quit_app(a1);
        os_quit_app(a2);
        sim_frames(2);
        os_reap_dead_apps();
        sim_frames(2);
        snprintf(b, sizeof b, "  pool free %u -> %u\n", pool0,
                 pmm_free_count());
        wr(1, b);
        T("pool returned after quit (image unloaded too)",
          pmm_free_count() == pool0);
        return fails;
    }
    if (!strcmp(name, "dynctx")) {
        static uint8_t buf[4096];
        uint32_t n = 0;
        if (!load_file(FIXDIR "apptest/CTX.APP", buf, sizeof buf, &n))
            return 1;
        fat_file_t f;
        fs_create("/CTX.APP", &f);
        fs_write(&f, buf, n);
        fs_close(&f);
        if (!appload_launch("/CTX.APP")) {
            wr(2, appload_last_error());
            wr(2, "\n");
            return 1;
        }
        sim_frames(3);
        window *w = wm_focused_window();
        control *v = w->controls, *t = NULL, *b = NULL;
        for (control *c = w->controls; c; c = c->next) {
            if (c->type == CTL_TREE)
                t = c;
            if (c->tip)
                b = c;
        }
        sim_rclick(v->ax + 30, v->ay + 20);
        sim_frames(1);
        sim_shot("dy1");
        int *picked = (int *)((char *)w->dyn_state + 20);
        sim_click(v->ax + 30 + 10, v->ay + 20 + 14);
        sim_frames(2); /* Beta */
        {
            char m[64];
            snprintf(m, 64, "picked=%d open=%d", *picked, popup_is_open());
            sim_log(m);
        }
        {
            char m[64];
            snprintf(m, 64, "tree kid=[%s]",
                     tree_text(t, tree_first_child(t, 0)));
            sim_log(m);
        }
        sim_move(b->ax + 5, b->ay + 5);
        sim_frames(50);
        sim_shot("dy2");
        sim_rclick(v->ax + 30, v->ay + 20);
        sim_frames(1);
        os_close_own_instance(w);
        sim_frames(25);
        os_reap_dead_apps();
        sim_frames(2);
        sim_log(popup_is_open() ? "popup open after quit (BAD)"
                                : "popup closed with app");
        return 0;
    }
    if (!strcmp(name, "search")) {
        int fails = 0;
        char b[300];
        fo_create("/HDA/DESKTOP", "NOTES", true, false);
        fo_create("/HDA/DESKTOP/NOTES", "TODO.TXT", false, false);
        fo_create("/HDA/DESKTOP/NOTES", "PIC1.PIC", false, false);
        fo_create("/HDA/DESKTOP", "README.TXT", false, false);
        fo_create("/HDA/DESKTOP", "SECRET.TXT", false, true);
        T("match contains",
          fo_match("README.TXT", "adm") && !fo_match("README.TXT", "xyz"));
        T("match glob",
          fo_match("PIC1.PIC", "*.pic") && fo_match("PIC1.PIC", "PIC?.PIC") &&
              !fo_match("PIC1.PIC", "*.TXT") && fo_match("A", "*"));
        static char out[4096];
        out[0] = 0;
        cmd_find("TXT /", out, sizeof out);
        wr(1, out);
        T("find TXT (incl hidden + nested)",
          strstr(out, "/DESKTOP/NOTES/TODO.TXT") &&
              strstr(out, "/DESKTOP/README.TXT") && strstr(out, "SECRET.TXT"));
        out[0] = 0;
        cmd_find("*.PIC", out, sizeof out);
        wr(1, out);
        out[0] = 0;
        cmd_find("", out, sizeof out);
        T("usage", strstr(out, "Usage") != NULL);
        /* FileF filter */
        fo_open(NULL, "/HDA/DESKTOP");
        sim_frames(3);
        window *w = wm_focused_window();
        control *view = NULL;
        for (control *c = w->controls; c; c = c->next)
            if (c->type == CTL_CUSTOM && c->aw > 100)
                view = c;
        ctl_focus(view);
        sim_key2(0x10, 'r', false, false);
        sim_key2(0x10, 'e', false, false);
        sim_frames(2);
        sim_shot("flt1");
        control *st = NULL;
        for (control *c = w->controls; c; c = c->next)
            if (c->type == CTL_STATUS)
                st = c;
        sim_log(ctl_text(st));
        T("filter shows 1", strstr(ctl_text(st), "1 of") != NULL);
        sim_key(0x0E);
        sim_key(0x0E);
        sim_frames(1); /* backspace x2 */
        T("backspace clears", strstr(ctl_text(st), "match") == NULL);
        sim_key2(0x10, '*', false, false);
        sim_key2(0x10, '.', false, false);
        sim_key2(0x10, 'a', false, false);
        sim_frames(1);
        sim_log(ctl_text(st));
        sim_key(0x01);
        sim_frames(1);
        T("esc clears", strstr(ctl_text(st), "match") == NULL);
        return fails;
    }
    if (!strcmp(name, "prefs")) {
        int fails = 0;
        char b[300];
        T("unset default", app_prefs_get_int("TESTAPP", "x", 42) == 42);
        T("set int", app_prefs_set_int("TESTAPP", "x", -7) &&
                         app_prefs_get_int("TESTAPP", "x", 0) == -7);
        T("set str", app_prefs_set("TESTAPP", "name", "hello world"));
        T("overwrite", app_prefs_set_int("TESTAPP", "x", 99) &&
                           app_prefs_get_int("TESTAPP", "x", 0) == 99);
        char gb[32];
        T("get str", app_prefs_get("TESTAPP", "name", gb, sizeof gb) &&
                         !strcmp(gb, "hello world"));
        T("bad key", !app_prefs_set("TESTAPP", "a=b", "1"));
        app_prefs_begin("TESTAPP");
        app_prefs_set_int("TESTAPP", "y", 1);
        app_prefs_set_int("TESTAPP", "z", 2);
        T("batch", app_prefs_end());
        T("batch read", app_prefs_get_int("TESTAPP", "y", 0) == 1 &&
                            app_prefs_get_int("TESTAPP", "z", 0) == 2 &&
                            app_prefs_get_int("TESTAPP", "x", 0) == 99);
        /* ASMDraw round trip */
        app_instance_t *a = sim_launch("ASMDRAW");
        sim_frames(2);
        os_quit_app(a);
        sim_frames(2);
        os_reap_dead_apps();
        sim_frames(1);
        T("asmdraw saved defaults",
          app_prefs_get_int("ASMDRAW", "pen", -1) == 0);
        app_prefs_set_int("ASMDRAW", "pen", 4);
        app_prefs_set_int("ASMDRAW", "tool", 3);
        a = sim_launch("ASMDRAW");
        sim_frames(2);
        os_quit_app(a);
        sim_frames(2);
        os_reap_dead_apps();
        sim_frames(1);
        T("asmdraw loaded + kept",
          app_prefs_get_int("ASMDRAW", "pen", -1) == 4 &&
              app_prefs_get_int("ASMDRAW", "tool", -1) == 3);
        fat_file_t f;
        static char t[512];
        int n = 0;
        if (fs_open("/PREFS/ASMDRAW.INI", &f)) {
            n = fs_read(&f, t, 511);
            fs_close(&f);
        }
        t[n > 0 ? n : 0] = 0;
        sim_log(t);
        return fails;
    }
    if (!strcmp(name, "picopen")) {
        static uint8_t pic[4 + 40 * 20];
        pic[0] = 40;
        pic[2] = 20;
        for (int i = 0; i < 40 * 20; i++)
            pic[4 + i] = (uint8_t)((i / 40 + i % 40) / 4 % 16);
        fat_file_t f;
        fs_create("/DESKTOP/TEST.PIC", &f);
        fs_write(&f, pic, sizeof pic);
        fs_close(&f);
        fo_open(NULL, "/DESKTOP/TEST.PIC");
        sim_frames(3);
        sim_shot("pic1");
        window *w = wm_focused_window();
        sim_log(w ? w->title : "no window");
        return 0;
    }
    if (!strcmp(name, "tips")) {
        fo_open(NULL, "/HDA/DESKTOP");
        sim_frames(3);
        window *w = wm_focused_window();
        control *up = NULL;
        for (control *c = w->controls; c; c = c->next)
            if (c->tip && !strcmp(c->tip, "Up one folder"))
                up = c;
        sim_move(up->ax + 6, up->ay + 6);
        sim_frames(10);
        sim_shot("tp0"); /* not yet */
        sim_frames(40);
        sim_shot("tp1"); /* shown */
        sim_click(up->ax + 6, up->ay + 6);
        sim_frames(50);
        sim_shot("tp2"); /* gone after click */
        sim_launch("ASMDRAW");
        sim_frames(3);
        w = wm_focused_window();
        control *r = NULL;
        for (control *c = w->controls; c; c = c->next)
            if (c->tip && !strcmp(c->tip, "Rectangle"))
                r = c;
        sim_move(r->ax + 6, r->ay + 6);
        sim_frames(50);
        sim_shot("tp3");
        return 0;
    }
    if (!strcmp(name, "ctxmenu")) {
        fo_create("/HDA/DESKTOP", "BOX", true, false);
        fo_create("/HDA/DESKTOP", "NOTE.TXT", false, false);
        sim_frames(2);
        /* desktop background, then an icon */
        sim_move(250, 150);
        mouse.right = true;
        sim_frame();
        {
            char b[64];
            snprintf(b, 64, "after press open=%d", popup_is_open());
            sim_log(b);
        }
        mouse.right = false;
        sim_frame();
        {
            char b[64];
            snprintf(b, 64, "after release open=%d", popup_is_open());
            sim_log(b);
        }
        sim_frames(1);
        sim_shot("cm1");
        sim_key(0x01);
        sim_frames(1);
        desktop_item_t *di = desktop_fs_items();
        int note = -1;
        for (int i = 0; i < desktop_fs_count(); i++)
            if (di[i].used && !strcmp(di[i].name, "NOTE.TXT"))
                note = i;
        {
            char b[64];
            snprintf(b, 64, "note=%d", note);
            sim_log(b);
        }
        int nx = 0 + di[note].x + 10, ny = 10 + di[note].y + 10;
        sim_rclick(nx, ny);
        sim_frames(1);
        sim_shot("cm2");
        /* choose Copy (keyboard: Down x3 = Cut? -> letters) */
        sim_key(0x2E /* c */);
        sim_frames(1);
        sim_shot("cm3");
        sim_key(0x1C);
        sim_frames(2);
        {
            char b[64];
            snprintf(b, 64, "clip_file=%d name=%s", clipboard_has_file(),
                     g_clipboard.name);
            sim_log(b);
        }
        /* FileF on /HDA/DESKTOP: right-click BOX, Paste Into */
        fo_open(NULL, "/HDA/DESKTOP");
        sim_frames(3);
        window *w = wm_focused_window();
        control *view = NULL;
        for (control *c = w->controls; c; c = c->next)
            if (c->type == CTL_CUSTOM && (c->style & CS_STRETCH_H) &&
                c->aw > 100)
                view = c;
        sim_rclick(view->ax + 2 + 4 + 44 + 22, view->ay + 2 + 6 + 10);
        sim_frames(1);
        sim_shot("cm4");
        /* click "Paste Into" by mouse: find it by stepping keys */
        sim_key(0x19 /* p */);
        sim_key(0x1C);
        sim_frames(3);
        bool d;
        {
            char b[96];
            snprintf(b, 96, "box/note exists=%d",
                     fo_stat("/HDA/DESKTOP/BOX/NOTE.TXT", &d, NULL));
            sim_log(b);
        }
        /* background of FileF view */
        sim_rclick(view->ax + view->aw - 30, view->ay + view->ah - 20);
        sim_frames(1);
        sim_shot("cm5");
        sim_key(0x01);
        sim_frames(1);
        /* edit control: path box */
        control *pe = NULL;
        for (control *c = w->controls; c; c = c->next)
            if (c->type == CTL_EDIT)
                pe = c;
        sim_rclick(pe->ax + 20, pe->ay + 5);
        sim_frames(1);
        sim_shot("cm6");
        sim_key(0x01);
        /* keyboard: Menu key in the view */
        ctl_focus(view);
        sim_ext(0x5D);
        sim_frames(1);
        sim_shot("cm7");
        sim_key(0x01);
        /* tree pane */
        control *t = NULL;
        for (control *c = w->controls; c; c = c->next)
            if (c->type == CTL_TREE)
                t = c;
        sim_rclick(t->ax + 30, t->ay + 6);
        sim_frames(1);
        sim_shot("cm8");
        sim_key(0x01);
        /* close window with popup open: must not crash */
        sim_rclick(view->ax + view->aw - 30, view->ay + view->ah - 20);
        sim_frames(1);
        os_close_own_instance(w);
        sim_frames(25);
        os_reap_dead_apps();
        sim_frames(2);
        sim_log(popup_is_open() ? "popup still open (BAD)"
                                : "popup closed with window");
        return 0;
    }
    if (!strcmp(name, "dlgtree")) {
        fo_create("/HDA/DESKTOP", "BOX", true, false);
        fo_create("/HDA/DESKTOP/BOX", "INNER", true, false);
        fo_create("/HDA/DESKTOP/BOX", "Q.TXT", false, false);
        static struct {
            window *win;
        } st;
        st.win = app_window(&st, "Owner", 10, 20, 100, 60, BLUE, 0);
        dlg_open_file(st.win, "Open", "/HDA/DESKTOP/BOX", NULL, NULL, 0);
        sim_frames(2);
        sim_shot("dt1");
        sim_key(0x1C);
        sim_frames(2);
        sim_shot("dt2"); /* Enter on "..": up */
        window *w = wm_focused_window();
        control *t = NULL;
        for (control *c = w->controls; c; c = c->next)
            if (c->type == CTL_TREE)
                t = c;
        {
            char b[96];
            snprintf(b, 96, "t=%p n0=[%s] sel=%d", (void *)t,
                     t ? tree_text(t, 0) : "-", t ? tree_sel(t) : -9);
            sim_log(b);
        }
        int hda = tree_find_path(t, "/HDA");
        tree_expand(t, hda, false);
        sim_frames(1);
        /* click the HDA row: list goes to /HDA */
        {
            int wx, wy, cw, ch;
            win_client_rect(w, &wx, &wy, &cw, &ch);
            sim_click(wx + t->x + 30, wy + t->y + 6);
        }
        sim_frames(2);
        sim_shot("dt3");
        return 0;
    }
    if (!strcmp(name, "tree")) {
        static char log[2048];
        log[0] = 0;
        static struct {
            window *win;
        } st;
        st.win =
            app_window(&st, "Tree test", 40, 40, 200, 150, BLUE, AW_RESIZABLE);
        control *t =
            ctl_tree(st.win, 4, 4, 190, 128,
                     TS_FOLDERS | CS_STRETCH_W | CS_STRETCH_H, tree_ev);
        tree_add(t, -1, "ALPHA", 1, true);
        tree_add(t, -1, "BETA", 2, true);
        tree_add(t, -1, "GAMMA", 3, true);
        tree_add(t, -1, "EMPTY", 4, true);
        g_tree_log = log;
        ctl_focus(t);
        sim_frames(2);
        sim_ext(0x4D);
        sim_frames(1); /* Right: select first */
        sim_ext(0x4D);
        sim_frames(1); /* Right: expand ALPHA (EV_EXPAND) */
        sim_ext(0x50);
        sim_frames(1); /* Down: ALPHA.1 */
        sim_ext(0x4D);
        sim_frames(1); /* Right: expand ALPHA.1 */
        sim_ext(0x4D);
        sim_frames(1); /* Right: first child */
        sim_shot("tree1");
        int cx = st.win->x + 1, cy = st.win->y + 26;
        /* click the [+] of BETA: find its row */
        int beta = -1;
        for (int n = 0; n < 64; n++)
            if (!strcmp(tree_text(t, n), "BETA")) {
                beta = n;
                break;
            }
        (void)beta;
        sim_key(0x22 /* g */);
        sim_frames(1); /* type-ahead: GAMMA */
        sim_ext(0x4B);
        sim_frames(1); /* Left on GAMMA (closed) -> nothing */
        sim_key(0x1C);
        sim_frames(1); /* Enter -> submit */
        sim_ext(0x4F);
        sim_frames(1); /* End -> EMPTY */
        sim_ext(0x4D);
        sim_frames(1); /* Right on EMPTY: no children -> loses [+] */
        sim_ext(0x47);
        sim_frames(1); /* Home */
        sim_ext(0x4B);
        sim_frames(1); /* Left: collapse ALPHA */
        sim_shot("tree2");
        /* mouse: double-click BETA row (row 1 after collapse) */
        sim_dblclick(cx + 4 + 2 + 9 + 11 + 8, cy + 4 + 2 + 1 * 9 + 4);
        sim_frames(2);
        sim_shot("tree3");
        wr(1, log);
        return 0;
    }
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
