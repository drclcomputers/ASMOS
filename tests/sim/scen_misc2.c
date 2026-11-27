#include "os/meminfo.h"
#include "os/os.h"
#include "scen.h"
#include "ui/desktop_fs.h"
#include "ui/popup.h"

// asking to save before closing (wm_ask_save), the Clock

extern void teditor_open_file(const char *path);

/* the button labelled t in window d, clicked */
static bool push(window *d, const char *t) {
    for (control *c = d ? d->controls : NULL; c; c = c->next)
        if (c->type == CTL_BUTTON && !strcmp(ctl_text(c), t)) {
            sim_click(c->ax + 5, c->ay + 5);
            sim_frames(3);
            return true;
        }
    return false;
}

static bool run_menu(window *w, const char *prefix) {
    for (int m = 0; w && m < w->win_menu_count; m++)
        for (int k = 0; k < w->win_menus[m].item_count; k++) {
            menu_item *it = &w->win_menus[m].items[k];
            if (it->label && !strncmp(it->label, prefix, strlen(prefix))) {
                menu_item_run(it);
                return true;
            }
        }
    return false;
}

static int file_is(const char *path, const char *want) {
    char buf[64] = {0};
    fat_file_t f;
    if (!fs_open(path, &f))
        return 0;
    int n = fs_read(&f, buf, sizeof(buf) - 1);
    fs_close(&f);
    buf[n < 0 ? 0 : n] = 0;
    return !strcmp(buf, want);
}

static window *teditor_typed(const char *text) {
    sim_launch("TEDITOR");
    sim_frames(3);
    window *w = wm_focused_window();
    sim_type(text);
    sim_frames(2);
    return w;
}

static int ask_save(void) {
    int fails = 0;
    char b[200];
    fs_delete("/ASK.TXT");

    /* typed in: not saved; closing asks */
    window *w = teditor_typed("hello");
    T("typed: the window has unsaved changes", w && w->unsaved);
    wm_close(w);
    sim_frames(2);
    window *d = wm_focused_window();
    T("closing asks: Save changes?",
      d && d != w && !strcmp(d->title, "Save changes?") && w->modal_child == d);
    sim_shot("asksave");
    push(d, "Cancel");
    T("Cancel: still open, still not saved",
      wm_is_registered(w) && w->unsaved && !w->modal_child);

    /* asked again; Don't save closes it (Ctrl+Q does wm_close too: the
     * scheduler's, not run here) */
    wm_close(w);
    sim_frames(2);
    d = wm_focused_window();
    T("asked again", d && d != w && !strcmp(d->title, "Save changes?"));
    push(d, "Don't save");
    T("Don't save: closed", !wm_is_registered(w));

    /* Save, untitled: Save As first; saved, it closes */
    w = teditor_typed("hello");
    wm_close(w);
    sim_frames(2);
    push(wm_focused_window(), "Save");
    d = wm_focused_window();
    T("Save, untitled: Save As", d && d != w && wm_is_registered(w));
    control *e = NULL;
    for (control *c = d ? d->controls : NULL; c; c = c->next)
        if (c->type == CTL_EDIT && !e)
            e = c;
    if (e) {
        ctl_set_text(e, "ASK.TXT");
        ctl_focus(e);
    }
    sim_key(0x1C);
    sim_frames(4);
    T("... saved: it closes", !wm_is_registered(w));
    T("... the file", file_is("/ASK.TXT", "hello"));

    /* Save As cancelled: it stays, and doesn't close later */
    w = teditor_typed("x");
    wm_close(w);
    sim_frames(2);
    push(wm_focused_window(), "Save");
    push(wm_focused_window(), "Cancel");
    sim_frames(4);
    T("Save As cancelled: still open, not saved",
      wm_is_registered(w) && w->unsaved);

    /* shutting down names it */
    char names[80];
    int n = wm_unsaved_titles(names, sizeof names);
    snprintf(b, sizeof b, "    not saved: %d, %s\n", n, names);
    wr(1, b);
    T("shutting down: the window not saved is named",
      n == 1 && strstr(names, "TEditor"));
    w->unsaved = false;
    wm_close(w);
    sim_frames(2);

    /* a file: Save saves it there, and it closes */
    teditor_open_file("/ASK.TXT");
    sim_frames(3);
    w = wm_focused_window();
    T("opened: nothing to save", w && !w->unsaved);
    sim_type("!");
    sim_frames(1);
    wm_close(w);
    sim_frames(2);
    push(wm_focused_window(), "Save");
    sim_frames(2);
    T("a file: Save saves it, closes",
      !wm_is_registered(w) &&
          (file_is("/ASK.TXT", "!hello") || file_is("/ASK.TXT", "hello!")));

    /* ASMDraw too: drawn on, closing asks */
    sim_launch("ASMDRAW");
    sim_frames(3);
    w = wm_focused_window();
    T("ASMDraw: nothing to save yet", w && !w->unsaved);
    control *cv = NULL;
    for (control *c = w->controls; c; c = c->next)
        if (c->type == CTL_CUSTOM && (!cv || c->aw * c->ah > cv->aw * cv->ah))
            cv = c;
    sim_drag(cv->ax + 10, cv->ay + 10, cv->ax + 40, cv->ay + 30);
    T("drawn on: not saved", w->unsaved);
    wm_close(w);
    sim_frames(2);
    d = wm_focused_window();
    T("ASMDraw asks", d && d != w && !strcmp(d->title, "Save changes?"));
    push(d, "Don't save");
    T("... Don't save: closed", !wm_is_registered(w));
    return fails;
}

static int clock_faces(void) {
    int fails = 0;
    char b[200];
    sim_launch("Clock");
    sim_frames(3);
    window *w = FIND("Clock");
    T("Clock: the text face", w && w->h < 80);
    run_menu(w, "Classic Clock");
    sim_frames(2);
    T("View > Classic Clock: taller, a dial", w->h > 100);
    sim_shot("clock_classic");
    /* the hands: black and red on the white face */
    control *dial = NULL;
    for (control *c = w->controls; c; c = c->next)
        if (c->type == CTL_CUSTOM)
            dial = c;
    int white = 0, red = 0;
    for (int y = 0; dial && y < dial->ah; y++)
        for (int x = 0; x < dial->aw; x++) {
            uint8_t v = BACKBUF[(dial->ay + y) * SCREEN_WIDTH + dial->ax + x];
            white += v == WHITE;
            red += v == RED;
        }
    snprintf(b, sizeof b, "    dial: %d white, %d red\n", white, red);
    wr(1, b);
    T("... a white face, a red second hand", white > 3000 && red > 10);
    T("... kept for the next time", app_prefs_get_int("Clock", "Face", 0) == 1);

    /* the alarm page: - and + */
    g_alarm.set = false;
    run_menu(w, "Set Alarm...");
    sim_frames(2);
    control *hm = NULL, *hp = NULL, *mm = NULL, *mp = NULL;
    for (control *c = w->controls; c; c = c->next)
        if (c->type == CTL_BUTTON && c->visible) {
            if (!strcmp(ctl_text(c), "-"))
                *(hm ? &mm : &hm) = c;
            if (!strcmp(ctl_text(c), "+"))
                *(hp ? &mp : &hp) = c;
        }
    T("Set Alarm: a page with - and +", hm && hp && mm && mp && w->h < 100);
    sim_shot("clock_alarm");
    time_full_t t = time_rtc_local();
    sim_click(hp->ax + 4, hp->ay + 4);
    sim_click(hp->ax + 4, hp->ay + 4);
    sim_click(mm->ax + 4, mm->ay + 4);
    /* held: it keeps going */
    sim_move(mp->ax + 4, mp->ay + 4);
    mouse.left = true;
    sim_frames(60); /* (a second) */
    mouse.left = false;
    sim_frames(2);
    push(w, "OK");
    bool set = g_alarm.set;
    int ah = g_alarm.hour, am = g_alarm.minute;
    int want_h = (t.hours + 2) % 24;
    int got_m = ((am - t.minutes + 1) % 60 + 60) % 60; /* steps held */
    snprintf(b, sizeof b, "    alarm %d %02d:%02d (now %02d:%02d), %d held\n",
             set, ah, am, t.hours, t.minutes, got_m);
    wr(1, b);
    T("+ + on the hour, - on the minutes, + held: the alarm",
      set && ah == want_h && got_m >= 3 && got_m <= 8);
    T("OK: the classic face again", w->h > 100);
    run_menu(w, "Text Clock");
    sim_frames(2);
    T("View > Text Clock: back", w->h < 80);
    wm_close(w);
    sim_frames(2);
    return fails;
}

int scen_misc2(const char *name) {
    if (!strcmp(name, "asksave"))
        return ask_save();
    if (!strcmp(name, "clockfaces"))
        return clock_faces();
    if (!strcmp(name, "clickthrough")) {
        /* a click on a window is the window's: its title bar buttons
         * close or minimize it at once, and the click must not then
         * select (and drag) the desktop icon that was under them */
        int fails = 0;
        char b[200];
        g_cfg.reduce_motion = true; /* (no animation: gone at once) */
        desktop_item_t *items = desktop_fs_items();
        int trash = -1;
        for (int i = 0; i < desktop_fs_count(); i++)
            if (items[i].used && items[i].kind == DESKTOP_ITEM_TRASH)
                trash = i;
        T("the Trash is on the desktop", trash >= 0);
        if (trash < 0)
            return fails;
        int ix = items[trash].x, iy = items[trash].y;
        int cx = ix + 10, cy = MENUBAR_H + iy + 8; /* inside the icon */
#define UNSELECT()                                                             \
    for (int i = 0; i < desktop_fs_count(); i++)                               \
    items[i].selected = false
        /* 1: minimize, with the button held and the pointer moving on */
        UNSELECT();
        sim_launch("TEDITOR");
        sim_frames(3);
        window *w = wm_focused_window();
        w->x = cx - 21; /* the minimize button (x + 16..26) over the icon */
        w->y = cy - MENUBAR_H - 8;
        wm_invalidate_all();
        sim_frames(2);
        sim_drag(cx, cy, cx + 60, cy + 40);
        sim_frames(2);
        snprintf(b, sizeof b,
                 "    minimized %d, Trash selected %d, at %d,%d (was %d,%d)\n",
                 w->minimized, items[trash].selected, items[trash].x,
                 items[trash].y, ix, iy);
        wr(1, b);
        T("minimize: the window goes", w->minimized);
        T("... the Trash isn't selected or moved", !items[trash].selected &&
                                                       items[trash].x == ix &&
                                                       items[trash].y == iy);
        /* 2: a click on the title bar, over the icon */
        w->minimized = false;
        w->x = cx - 60;
        wm_focus(w);
        wm_invalidate_all();
        sim_frames(2);
        UNSELECT();
        sim_click(cx, cy);
        sim_frames(2);
        T("a click on a title bar: not the desktop's",
          !items[trash].selected && wm_focused_window() == w);
        T("... and near the bottom it doesn't minimize (not a drag)",
          !w->minimized);
        /* 3: right-click on it: the window's, no desktop menu */
        sim_move(cx, cy);
        mouse.right_clicked = true;
        sim_frame();
        sim_frames(1);
        T("a right-click on a title bar: no desktop menu", !popup_is_open());
        if (popup_is_open()) {
            sim_key(0x01);
            sim_frames(2);
        }
        /* 4: close, over the icon (the title bar's drag pulled the window
         * up onto the screen: back) */
        w->x = cx - 8; /* the close button (x + 3..13) */
        w->y = cy - MENUBAR_H - 8;
        wm_invalidate_all();
        sim_frames(2);
        UNSELECT();
        sim_click(cx, cy);
        sim_frames(2);
        T("close: the Trash isn't selected",
          !items[trash].selected && wm_focused_window() != w);
        /* and the desktop's own: a click on the icon selects it */
        UNSELECT();
        sim_click(cx, cy);
        sim_frames(2);
        T("a click on the desktop still selects the icon",
          items[trash].selected);
        UNSELECT();
#undef UNSELECT
        g_cfg.reduce_motion = false;
        return fails;
    }
    if (!strcmp(name, "memreport")) {
        /* where the memory goes (os/meminfo.h): the parts add up, the
         * About box and `mem` say them, an app's memory shows as apps */
        int fails = 0;
        static char o[4096];
        char b[300];
        meminfo_t m;
        meminfo(&m);
        snprintf(b, sizeof b,
                 "    total %u: kernel %u heap %u screen %u apps %u free %u "
                 "reserved %u KB\n",
                 m.total / 1024, m.kernel / 1024, m.heap / 1024,
                 m.screen / 1024, m.apps / 1024, m.free / 1024,
                 m.reserved / 1024);
        wr(1, b);
        T("the parts add up",
          m.kernel + m.heap + m.screen + m.apps + m.free + m.reserved ==
              m.total);
        T("the screen: the back buffer at least",
          m.screen >= (uint32_t)(SCREEN_WIDTH * SCREEN_HEIGHT));
        uint32_t apps0 = m.apps;
        sim_launch("ASMDRAW");
        sim_frames(5);
        meminfo(&m);
        snprintf(b, sizeof b, "    with ASMDraw: apps %u KB\n", m.apps / 1024);
        wr(1, b);
        T("an app's memory counts as apps",
          m.apps >= apps0 + (uint32_t)(SCREEN_WIDTH * SCREEN_HEIGHT));
        cli_execute_command("mem", o, sizeof o);
        wr(1, o);
        T("mem: the parts", strstr(o, "system") && strstr(o, "screen") &&
                                strstr(o, "apps") && strstr(o, "BIOS"));
        wm_close(wm_focused_window()); /* (the desktop's menus) */
        sim_frames(3);
        menu_item *about = NULL; /* the desktop's About OS */
        for (int i = 0; i < g_menubar.menu_count && !about; i++)
            for (int k = 0; k < g_menubar.menus[i].item_count; k++)
                if (g_menubar.menus[i].items[k].label &&
                    strstr(g_menubar.menus[i].items[k].label, "About OS"))
                    about = &g_menubar.menus[i].items[k];
        if (about) {
            menu_item_run(about);
            sim_frames(3);
        }
        window *d = wm_focused_window();
        bool row = false, disp = false;
        for (control *c = d ? d->controls : NULL; c; c = c->next)
            if (c->type == CTL_LABEL && c->text) {
                row |= !strncmp(c->text, "OS ", 3) && strstr(c->text, "apps");
                disp |= strstr(c->text, " colors") &&
                        strstr(c->text, video_mode_colors(g_video_mode) == 256
                                            ? "256"
                                            : "16");
            }
        sim_shot("about_mem");
        T("About ASMOS: what the memory is used for", about && row);
        T("... and the right number of colours", disp);
        return fails;
    }
    if (!strcmp(name, "sdkapp")) { /* built with the SDK alone (make sdk) */
        int fails = 0;
        char b[300];
        static uint8_t buf[16384];
        uint32_t n = 0;
        T("the SDK built its example",
          load_file(FIXDIR "SDKHELLO.APP", buf, sizeof buf, &n));
        if (!n)
            return fails;
        fat_file_t f;
        fs_create("/HELLO.APP", &f);
        fs_write(&f, buf, n);
        fs_close(&f);
        T("it runs", appload_launch("/HELLO.APP") != NULL);
        sim_frames(3);
        window *w = wm_focused_window();
        T("its window", w && !strcmp(w->title, "Hello"));
        if (!w)
            return fails;
        control *edit = NULL, *btn = NULL, *greet = NULL;
        for (control *c = w->controls; c; c = c->next) {
            if (c->type == CTL_EDIT)
                edit = c;
            if (c->type == CTL_BUTTON)
                btn = c;
            if (c->type == CTL_LABEL && c->y == 50)
                greet = c;
        }
        T("an edit box, a button, a label", edit && btn && greet);
        if (!edit || !btn || !greet)
            return fails;
        ctl_set_text(edit, "Ana");
        sim_click(btn->ax + 4, btn->ay + 4);
        sim_frames(2);
        snprintf(b, sizeof b, "  says: %s\n", greet->text);
        wr(1, b);
        T("Greet: snprintf from libasmos.a, ctl_* through the shim",
          !strcmp(greet->text, "Hello, Ana!"));
        dir_entry_t de;
        T("its help page installed", fs_find("/HELP/Hello/01_Hello.TXT", &de));
        return fails;
    }
    if (!strcmp(name, "appneeds")) { /* mkapp.py --min-cpu / --min-ram */
        int fails = 0;
        char b[300];
        static uint8_t buf[8192];
        uint32_t n = 0;
        extern int g_sim_cpu;
        if (!load_file(FIXDIR "NEEDY.APP", buf, sizeof buf, &n))
            return 1;
        T("the header says it: 84 bytes, 486, 4 MB",
          buf[8] == 84 && buf[80] == 4 && buf[82] + buf[83] * 256 == 64);
        fat_file_t f;
        fs_create("/NEEDY.APP", &f);
        fs_write(&f, buf, n);
        fs_close(&f);
        g_sim_cpu = 5;
        g_ram_bytes = 2048 * 1024;
        T("2 MB: refused", !appload_launch("/NEEDY.APP"));
        snprintf(b, sizeof b, "  %s\n", appload_last_error());
        wr(1, b);
        T("saying what it needs and what there is",
          strstr(appload_last_error(), "Needy needs 4 MB of RAM") &&
              strstr(appload_last_error(), "Pentium, 2 MB"));
        g_sim_cpu = 3;
        g_ram_bytes = 8192 * 1024;
        T("a 386: refused", !appload_launch("/NEEDY.APP") &&
                                strstr(appload_last_error(), "a 486 or newer"));
        g_ram_bytes = 2048 * 1024;
        T("a 386 with 2 MB: both said",
          !appload_launch("/NEEDY.APP") &&
              strstr(appload_last_error(), "a 486 and 4 MB of RAM"));
        g_sim_cpu = 4;
        g_ram_bytes = 3840 * 1024; /* (a BIOS keeping 256 KB) */
        T("a 486 with 4 MB: runs", appload_launch("/NEEDY.APP") != NULL);
        sim_frames(2);
        T("its window",
          wm_focused_window() && !strcmp(wm_focused_window()->title, "Hello"));
        g_sim_cpu = 5;
        g_ram_bytes = 2048 * 1024;
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
            if (c->tip && !strncmp(c->tip, "Rectangle", 9))
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
