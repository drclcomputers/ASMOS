#include "scen.h"

int scen_storage(const char *name) {
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
    if (!strcmp(name, "help")) {
        /* Check /HELP folders, app-specific help, and help refresh on change. */
        int fails = 0;
        char b[300];
        dir_entry_t de;
        extern bool help_has(const char *);
#define VIEW_HELP(win)                                                         \
    ({                                                                         \
        menu_item *vh_ = NULL;                                                 \
        for (int i_ = 0; (win) && i_ < (win)->win_menu_count; i_++)            \
            if (!strcmp((win)->win_menus[i_].title, "Help"))                   \
                for (int k_ = 0; k_ < (win)->win_menus[i_].item_count; k_++)   \
                    if ((win)->win_menus[i_].items[k_].label &&                \
                        !strcmp((win)->win_menus[i_].items[k_].label,          \
                                "View Help"))                                  \
                        vh_ = &(win)->win_menus[i_].items[k_];                 \
        vh_;                                                                   \
    })
        T("the built-in pages are on the disk",
          fs_find("/HELP/ASMOS/01_Welcome.TXT", &de) &&
              fs_find("/HELP/FileF", &de) &&
              fs_find("/HELP/Character_Map", &de));
        T("an .APP's are not, before it runs", !fs_find("/HELP/Sudoku", &de));
        /* the Help app on its own */
        app_instance_t *h = sim_launch("HELP");
        sim_frames(4);
        typedef struct {
            window *win;
            control *tree, *view, *sb;
            int folders[64], nfolders;
            char rel[200][112];
            int nrel;
            char shown[112];
        } hs_t;
        hs_t *hs = h->state;
        snprintf(b, sizeof b, "    %d nodes, shown '%s'\n", hs->nrel,
                 hs->shown);
        wr(1, b);
        T("ASMOS first, its first page",
          !strcmp(hs->shown, "ASMOS") && hs->nfolders > 5 &&
              !strcmp(tree_text(hs->tree, hs->folders[0]), "ASMOS"));
        sim_shot("help1");
        /* FileF: Help > View Help */
        sim_launch("FILEF");
        sim_frames(3);
        window *fw = wm_focused_window();
        menu_item *vh = VIEW_HELP(fw);
        T("FileF has Help > View Help", vh != NULL);
        if (vh)
            vh->action();
        sim_frames(3);
        T("it opens Help at FileF",
          wm_focused_window() == hs->win && !strcmp(hs->shown, "FileF"));
        sim_shot("help2");
        /* an app with no pages: no Help menu */
        sim_launch("CALCULATOR");
        sim_frames(3);
        window *cw = wm_focused_window();
        T("Calculator: its pages put in /HELP, its menu",
          fs_find("/HELP/Calculator/01_Calculator.TXT", &de) &&
              VIEW_HELP(cw) != NULL);
        wm_close(cw);
        sim_frames(2);
        /* Sudoku: an .APP with pages and its own Help menu */
        sim_launch("SUDOKU");
        sim_frames(3);
        window *sw = wm_focused_window();
        T("Sudoku: its page put in /HELP",
          fs_find("/HELP/Sudoku/01_Sudoku.TXT", &de));
        T("View Help added to its own Help menu", VIEW_HELP(sw) != NULL);
        int nhelp = 0;
        for (int i = 0; i < sw->win_menu_count; i++)
            nhelp += !strcmp(sw->win_menus[i].title, "Help");
        T("(one Help menu)", nhelp == 1);
        VIEW_HELP(sw)->action();
        sim_frames(3);
        T("Help at Sudoku", !strcmp(hs->shown, "Sudoku"));
        sim_shot("help3");
        /* deleted: stays deleted while it is the same .APP */
        wm_close(sw);
        sim_frames(2);
        fs_rm_rf("/HELP/Sudoku");
        sim_frames(3);
        int sud = -1;
        for (int i = 0; i < hs->nfolders; i++)
            if (!strcmp(tree_text(hs->tree, hs->folders[i]), "Sudoku"))
                sud = hs->folders[i];
        T("Help's list follows", sud < 0 && !help_has("Sudoku"));
        sim_launch("SUDOKU");
        sim_frames(3);
        sw = wm_focused_window();
        T("run again: not put back",
          !fs_find("/HELP/Sudoku", &de) && VIEW_HELP(sw) == NULL);
        wm_close(sw);
        sim_frames(3);
        /* the .APP replaced (installed again, later): back */
        fat_file_t f;
        static uint8_t app[65536];
        int n = 0;
        if (fs_open("/APPS/Games/Sudoku.APP", &f)) {
            n = fs_read(&f, app, sizeof app);
            fs_close(&f);
        }
        extern unsigned char g_cmos[128];
        g_cmos[2] = 0x47; /* the clock: 10:47 */
        fs_delete("/APPS/Games/Sudoku.APP");
        if (fs_create("/APPS/Games/Sudoku.APP", &f)) {
            fs_write(&f, app, n);
            fs_close(&f);
        }
        sim_launch("SUDOKU");
        sim_frames(3);
        T("installed again: its page is back",
          fs_find("/HELP/Sudoku/01_Sudoku.TXT", &de));
        sw = wm_focused_window();
        if (sw)
            wm_close(sw);
        sim_frames(2);
        /* Help's File > Delete Page */
        wm_focus(hs->win);
        int fnode = -1;
        for (int i = 0; i < hs->nfolders; i++)
            if (!strcmp(tree_text(hs->tree, hs->folders[i]), "Print Queue"))
                fnode = hs->folders[i];
        T("Print Queue in the list (shown with a space)", fnode >= 0);
        tree_set_sel(hs->tree, fnode);
        hs->tree->handler(hs->tree, EV_CHANGE);
        sim_frames(2);
        menu_item *del = NULL;
        for (int i = 0; i < hs->win->win_menu_count; i++)
            if (!strcmp(hs->win->win_menus[i].title, "File"))
                del = &hs->win->win_menus[i].items[0];
        T("File > Delete Page", del && !strcmp(del->label, "Delete Page"));
        del->action();
        sim_frames(2);
        sim_key(0x1C); /* Yes */
        sim_frames(3);
        T("deleted: the folder and the node",
          !fs_find("/HELP/Print_Queue", &de) && !help_has("Print Queue"));
        sim_shot("help4");
#undef VIEW_HELP
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
        /* held keys: kb.down, while the key is down (a game's arrows) */
        kb_process_byte(0xE0);
        kb_process_byte(0x48); /* grey Up */
        kb_process_byte(0x1E); /* A */
        sim_frames(3);
        T("held: down while held, frames later",
          kb.down[0x48] && kb.down[0x1E]);
        kb_process_byte(0x9E); /* A up */
        sim_frame();
        T("up: no longer down", kb.down[0x48] && !kb.down[0x1E]);
        kb_process_byte(0xE0);
        kb_process_byte(0xC8);
        kb_process_byte(0xE0);
        kb_process_byte(0x2A); /* a fake shift */
        sim_frame();
        T("both up; a fake E0 2A shift isn't a key",
          !kb.down[0x48] && !kb.down[0x2A]);
        ctl_set_text(ed, "\xe9");
        edit_select(ed, 1, 1); /* (A above typed an a: back as it was) */
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
    return -1;
}
