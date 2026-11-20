#include "scen.h"

int scen_zip(const char *name) {
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
    return -1;
}
