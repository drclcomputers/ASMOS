#include "scen.h"

int scen_misc2(const char *name) {
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
