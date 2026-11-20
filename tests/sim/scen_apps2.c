#include "scen.h"

int scen_apps2(const char *name) {
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
    return -1;
}
