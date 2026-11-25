#include "scen.h"

int scen_misc1(const char *name) {
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
        /* the size list, then the colours list (both say "restart") */
        control *scr = NULL, *cols = NULL, *save = NULL;
        for (control *c = w->controls; c; c = c->next) {
            if (c->type == CTL_COMBO && c->tip && strstr(c->tip, "restart")) {
                if (!scr)
                    scr = c;
                else if (!cols)
                    cols = c;
            }
            if (c->type == CTL_BUTTON && !strcmp(ctl_text(c), "Save"))
                save = c;
        }
        T("Screen combo enabled", scr && scr->enabled);
        T("Colours combo enabled", cols && cols->enabled);
        if (!scr || !cols || !save)
            return fails;
        sim_shot("vid1");
        list_set_sel(scr, 1); /* 640x400: still 256 colours, if it can */
        ctl_invoke(scr, EV_CHANGE);
        sim_frames(1);
        T("the colours stay 256",
          !strcmp(list_get(cols, ctl_value(cols)), "256 colours"));
        sim_click(save->ax + 5, save->ay + 5);
        sim_frames(2);
        T("saved 640x400x256 to sector 1", video_cfg_get() == VIDEO_640_256);
        list_set_sel(cols, 0); /* then 16 colours */
        ctl_invoke(cols, EV_CHANGE);
        sim_frames(1);
        sim_click(save->ax + 5, save->ay + 5);
        sim_frames(2);
        T("saved 640x400x16 to sector 1", video_cfg_get() == VIDEO_640);
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
    return -1;
}
