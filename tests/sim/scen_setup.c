#include "scen.h"

int scen_setup(const char *name) {
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
    return -1;
}
