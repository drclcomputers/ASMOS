#include "scen.h"

int scen_apps1(const char *name) {
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
        /* 2x display zoom is available from 640x400 and halves the desktop. */
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
                    ctl_set_value(c, 6);
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
    return -1;
}
