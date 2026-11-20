#include "scen.h"

int scen_fs(const char *name) {
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
    return -1;
}
