#include "scen.h"

int scen_menu(const char *name) {
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
        /* Ejection is detected without seeking; insertion is detected when
         * FileF opens the drive. */
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
    return -1;
}
