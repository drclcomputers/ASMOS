#include "scen.h"

int scen_help(const char *name) {
    if (!strcmp(name, "submenu")) {
        int fails = 0;
        char b[300];
        extern void wm_focus(window *);
        static uint8_t buf[16384];
        uint32_t n = 0;
        fat_file_t f;
        if (load_file(FIXDIR "lz/SUDOKU.APP", buf, sizeof buf, &n)) {
            fs_mkdir("/APPS");
        }
        sim_frames(3);
        menu *apps = NULL;
        int ai = -1;
        for (int i = 0; i < g_menubar.menu_count; i++)
            if (!strcmp(g_menubar.menus[i].title, "Apps")) {
                apps = &g_menubar.menus[i];
                ai = i;
            }
        T("Apps menu on the bar", apps != NULL);
        snprintf(b, sizeof b, "Apps items: %d\n", apps->item_count);
        wr(1, b);
        T("groups first",
          apps->items[0].sub && !strcmp(apps->items[0].label, "Accessories"));
        sim_click(apps->bar_x + 4, 4);
        sim_frames(2);
        T("open", g_menubar.open_index == ai);
        sim_move(apps->bar_x + 10, MENUBAR_H + 3);
        sim_frames(2); /* hover Accessories */
        T("hover opens the submenu", g_menubar.sub_item == 0);
        sim_shot("sm1");
        menu *acc = g_menubar.menus[ai].items[0].sub;
        T("Accessories has 8", acc->item_count == 8);
        /* diagonally: right and down across the Games row, quickly */
        for (int k = 1; k <= 10; k++) {
            sim_move(apps->bar_x + 10 + k * 11, MENUBAR_H + 3 + k);
            sim_frame();
        }
        T("brushing Games on the way keeps Accessories",
          g_menubar.sub_item == 0);
        sim_move(apps->bar_x + 120, MENUBAR_H + 10 + 4); /* (2nd row) */
        sim_frames(2);
        T("still open while inside it", g_menubar.sub_item == 0);
        sim_shot("sm2");
        mouse.left = true;
        sim_frame();
        mouse.left = false;
        sim_frames(6);
        window *w = wm_focused_window();
        snprintf(b, sizeof b, "launched: %s\n", w ? w->title : "-");
        wr(1, b);
        T("Calculator launched from the submenu (2nd, sorted)",
          w && strstr(w->title, "Calc"));
        T("menu closed", g_menubar.open_index < 0);
        /* a .APP in /APPS/GAMES joins Games */
        fs_mkdir("/APPS/GAMES");
        fs_delete("/APPS/GAMES/SUDOKU.APP");
        fs_create("/APPS/GAMES/SUDOKU.APP", &f);
        fs_write(&f, buf, n);
        fs_close(&f);
        wm_focus(win_stack[0]);
        sim_frames(70);
        for (int i = 0; i < g_menubar.menu_count; i++)
            if (!strcmp(g_menubar.menus[i].title, "Apps")) {
                apps = &g_menubar.menus[i];
                ai = i;
            }
        T("Games gets Sudoku from /APPS/GAMES (Puzzle, Solitaire, Sudoku)",
          apps->items[1].sub->item_count == 3);
        sim_click(apps->bar_x + 4, 4);
        sim_frames(1);
        sim_move(apps->bar_x + 10, MENUBAR_H + 1 * 10 + 4);
        sim_frames(2);
        sim_shot("sm3");
        sim_move(apps->bar_x + 120, MENUBAR_H + 1 * 10 + 4);
        sim_frames(1);
        sim_move(apps->bar_x + 120, MENUBAR_H + 3 * 10 + 4);
        sim_frames(1);
        mouse.left = true;
        sim_frame();
        mouse.left = false;
        sim_frames(8);
        w = wm_focused_window();
        T("Sudoku launched from Games", w && !strcmp(w->title, "Sudoku"));
        /* a click on a group item keeps the menu open */
        wm_focus(win_stack[0]);
        sim_frames(2);
        sim_click(apps->bar_x + 4, 4);
        sim_frames(1);
        sim_click(apps->bar_x + 10, MENUBAR_H + 1 * 10 + 4);
        sim_frames(1);
        T("click on a group: stays open",
          g_menubar.open_index >= 0 && g_menubar.sub_item == 1);
        sim_click(5, 300);
        sim_frames(2);
        T("click outside closes", g_menubar.open_index < 0);
        return fails;
    }
    if (!strcmp(name, "appsub")) {
        int fails = 0;
        char b[300];
        static uint8_t buf[16384];
        uint32_t n = 0;
        fat_file_t f;
        if (!load_file(FIXDIR "lz/SUDOKU.APP", buf, sizeof buf, &n))
            return 1;
        fs_delete("/PREFS/SUDOKU.INI");
        fs_delete("/SUDOKU.APP");
        fs_create("/SUDOKU.APP", &f);
        fs_write(&f, buf, n);
        fs_close(&f);
        appload_launch("/SUDOKU.APP");
        sim_frames(6);
        window *w = wm_focused_window();
        typedef struct {
            window *win;
            const void *api;
            control *view, *status;
            uint8_t sol[81], given[81], cur[81];
            uint16_t notes[81];
            uint8_t wrong[81];
            int sel, level;
        } su_t;
        su_t *s = w->dyn_state;
        T("easy at first", s->level == 0);
        menu *g = NULL;
        int gi = -1;
        for (int i = 0; i < g_menubar.menu_count; i++)
            if (!strcmp(g_menubar.menus[i].title, "Game")) {
                g = &g_menubar.menus[i];
                gi = i;
            }
        T("Game menu, first item a submenu",
          g && g->items[0].sub && !strcmp(g->items[0].label, "New Game") &&
              g->items[0].sub->item_count == 3);
        sim_click(g->bar_x + 4, 4);
        sim_frames(1);
        sim_move(g->bar_x + 10, MENUBAR_H + 4);
        sim_frames(2);
        T("hover opens it",
          g_menubar.open_index == gi && g_menubar.sub_item == 0);
        sim_shot("as1");
        sim_move(g->bar_x + 100, MENUBAR_H + 4);
        sim_frames(1);
        sim_move(g->bar_x + 100, MENUBAR_H + 2 * 10 + 4);
        sim_frames(2);
        sim_shot("as2");
        mouse.left = true;
        sim_frame();
        mouse.left = false;
        sim_frames(4);
        T("Hard chosen from the submenu", s->level == 2);
        /* refusals */
        extern bool popup_is_menu(const menu *);
        extern menu *popup_menu(void);
        T("no submenu in a submenu",
          menu_add_submenu(w->win_menus[0].items[0].sub, "x") == NULL);
        return fails;
    }
    if (!strcmp(name, "menupage") || !strcmp(name, "menupage320")) {
        int fails = 0;
        char b[300];
        if (name[8]) {
            extern int g_screen_width, g_screen_height, g_video_mode;
            g_screen_width = 320;
            g_screen_height = 200;
            g_video_mode = 0;
        }
        static window_spec_t spec = {.x = 40,
                                     .y = 30,
                                     .w = 200,
                                     .h = 100,
                                     .title = "Many Menus",
                                     .visible = true};
        window *w = wm_register(&spec);
        static const char *const T14[14] = {
            "File",    "Edit",     "View",      "Insert",      "Formatting",
            "Tools",   "Tables",   "Windows",   "Help Topics", "Extras",
            "Options", "Plug-ins", "Debugging", "Last One"};
        static char lab[16][12];
        static char tt[14][24];
        for (int i = 0; i < 14; i++) {
            snprintf(tt[i], 24, name[8] ? "%s" : "%s Menu", T14[i]);
            menu *m = window_add_menu(w, tt[i]);
            int n = i == 0 ? 17 : 3;
            for (int k = 0; k < n; k++) {
                snprintf(lab[k], 12, "Item %d", k + 1);
                menu_add_item(m, lab[k], NULL);
            }
        }
        T("16 items fit (17th refused)", w->win_menus[0].item_count == 16);
        wm_focus(w);
        sim_frames(3);
        T("all 14 on the bar (+ Apps)", g_menubar.menu_count == 15);
        int shown = 0, last = -1;
        for (int i = 0; i < 14; i++)
            if (g_menubar.menus[i].bar_w) {
                shown++;
                last = i;
            }
        snprintf(b, sizeof b, "page 1: %d shown (0..%d), >> at %d\n", shown,
                 last, g_menubar.next_x);
        wr(1, b);
        T("page 1: some shown, >> there, no <<",
          shown < 14 && g_menubar.next_x > 0 && g_menubar.prev_x < 0);
        sim_shot(name[8] ? "mp4" : "mp1");
        sim_click(g_menubar.menus[0].bar_x + 4, 4);
        sim_frames(2);
        sim_shot(name[8] ? "mp5" : "mp2");
        sim_click(g_menubar.next_x + 5, 4);
        sim_frames(2);
        T("menu closed by >>", g_menubar.open_index < 0);
        T("page 2 starts after the last shown",
          g_menubar.first == last + 1 && g_menubar.prev_x > 0);
        int a = -1;
        for (int i = 0; i < 14; i++)
            if (g_menubar.menus[i].bar_w) {
                a = i;
                break;
            }
        sim_click(g_menubar.menus[a].bar_x + 4, 4);
        sim_frames(2);
        T("a page-2 menu opens", g_menubar.open_index == a);
        sim_shot(name[8] ? "mp6" : "mp3");
        sim_click(5, 150);
        sim_frames(1);
        int pages = 2;
        while (g_menubar.next_x > 0 && pages < 6) {
            sim_click(g_menubar.next_x + 5, 4);
            sim_frames(1);
            pages++;
        }
        T("the last page shows Last",
          g_menubar.menus[13].bar_w > 0 && g_menubar.next_x < 0);
        snprintf(b, sizeof b, "pages: %d\n", pages);
        wr(1, b);
        for (int k = 0; k < pages - 1; k++) {
            sim_click(g_menubar.prev_x + 5, 4);
            sim_frames(1);
        }
        T("<< back to page 1", g_menubar.first == 0 && g_menubar.prev_x < 0);
        sim_click(g_menubar.next_x + 5, 4);
        sim_frames(1);
        wm_focus(win_stack[0]);
        sim_frames(2);
        wm_focus(w);
        sim_frames(2);
        T("another window and back: page 1", g_menubar.first == 0);
        return fails;
    }
    if (!strcmp(name, "appsdir")) {
        int fails = 0;
        char b[300];
        static uint8_t su[16384], lz[8192];
        uint32_t ns = 0, nl = 0;
        fat_file_t f;
        if (!load_file(FIXDIR "lz/SUDOKU.APP", su, sizeof su, &ns) ||
            !load_file(FIXDIR "lz/LAZY.APP", lz, sizeof lz, &nl))
            return 1;
#define PUT(p, d, n)                                                           \
    do {                                                                       \
        fs_delete(p);                                                          \
        fs_create(p, &f);                                                      \
        fs_write(&f, d, n);                                                    \
        fs_close(&f);                                                          \
    } while (0)
        fs_mkdir("/APPS");
        fs_mkdir("/APPS/GAMES");
        fs_mkdir("/APPS/ACME");
        fs_mkdir("/APPS/EMPTY");
        PUT("/APPS/LAZY.APP", lz, nl);
        PUT("/APPS/GAMES/SUDOKU.APP", su, ns);
        PUT("/APPS/ACME/SUDOKU.APP", su, ns);
        PUT("/APPS/ACME/LAZY.APP", lz, nl);
        PUT("/APPS/ACME/GROUP.TXT", "Acme Software\r\nrest", 19);
        PUT("/APPS/ACME/JUNK.APP", "not an app at all", 17);
        {
            static char o[256];
            extern int cli_execute_command(const char *, char *, size_t);
            o[0] = 0;
            cli_execute_command("write /APPS/ACME/CALC.LNK Calculator", o,
                                sizeof o);
            wr(1, o);
        }
        PUT("/APPS/ACME/MYGAME.LNK", "/APPS/GAMES/SUDOKU.APP\r\nMy Sudoku\r\n",
            34);
        PUT("/APPS/ACME/BAD.LNK", "Nothing here", 12);
        sim_frames(70); /* the menu follows within a second */
        menu *apps = NULL;
        for (int i = 0; i < g_menubar.menu_count; i++)
            if (!strcmp(g_menubar.menus[i].title, "Apps"))
                apps = &g_menubar.menus[i];
        for (int i = 0; i < apps->item_count; i++) {
            const menu_item *it = &apps->items[i];
            snprintf(b, sizeof b, "  %s%s\n", it->label ? it->label : "---",
                     it->sub ? "  >" : "");
            wr(1, b);
            if (it->sub)
                for (int k = 0; k < it->sub->item_count; k++) {
                    snprintf(b, sizeof b, "      %s\n",
                             it->sub->items[k].label);
                    wr(1, b);
                }
        }
#define GRP(nm)                                                                \
    ({                                                                         \
        menu *g_ = NULL;                                                       \
        for (int i_ = 0; i_ < apps->item_count; i_++)                          \
            if (apps->items[i_].label && !strcmp(apps->items[i_].label, nm))   \
                g_ = apps->items[i_].sub ? apps->items[i_].sub : (menu *)1;    \
        g_;                                                                    \
    })
#define HAS(m, nm)                                                             \
    ({                                                                         \
        bool h_ = false;                                                       \
        for (int k_ = 0; m && k_ < (m)->item_count; k_++)                      \
            h_ |= (m)->items[k_].label && !strcmp((m)->items[k_].label, nm);   \
        h_;                                                                    \
    })
        menu *games = GRP("Games"), *acme = GRP("Acme Software"),
             *acc = GRP("Accessories"), *sys = GRP("System Tools");
        T("groups sorted: Accessories first",
          !strcmp(apps->items[0].label, "Accessories"));
        T("Accessories: bundled .APPs + TEditor shortcut",
          acc && acc->item_count == 8 && HAS(acc, "Calculator") &&
              HAS(acc, "TEditor") && HAS(acc, "ASMDraw") &&
              HAS(acc, "Picture Viewer") && HAS(acc, "Character Map"));
        T("System Tools: HexView .APP + built-in shortcuts",
          sys && HAS(sys, "HexView") && HAS(sys, "Monitor") &&
              HAS(sys, "ASMTerm") && HAS(sys, "Format Disk") &&
              !HAS(sys, "Print Queue") && !HAS(sys, "Joystick") &&
              !HAS(sys, "Mouse"));
        T("Games has Puzzle, Solitaire, Sudoku",
          games && games->item_count == 3 && HAS(games, "Puzzle") &&
              HAS(games, "Solitaire") && HAS(games, "Sudoku"));
        T("Acme: 2 apps + 2 shortcuts, junk and the bad shortcut left out",
          acme && acme->item_count == 4);
        T("Calculator .APP by shortcut name; My Sudoku label from line 2",
          HAS(acme, "Calculator") && HAS(acme, "My Sudoku"));
        {
            extern void fo_open(window *, const char *);
            fo_open(NULL, "/APPS/ACME/CALC.LNK");
            sim_frames(6);
            window *cw = wm_focused_window();
            T("opening CALC.LNK runs Calculator",
              cw && strstr(cw->title, "Calc"));
            wm_close(cw);
            sim_frames(4);
            wm_focus(win_stack[0]);
            sim_frames(2);
        }
        T("Empty group is empty",
          GRP("Empty") && GRP("Empty")->item_count == 0);
        T("root items: FileF, Lazy, Settings",
          HAS(apps, "FileF") && HAS(apps, "Lazy") && HAS(apps, "Settings"));
        /* launch from the Acme group */
        int ai = 0;
        for (int i = 0; i < apps->item_count; i++)
            if (apps->items[i].sub == acme)
                ai = i;
        sim_click(apps->bar_x + 4, 4);
        sim_frames(1);
        sim_move(apps->bar_x + 10, MENUBAR_H + ai * 10 + 4);
        sim_frames(2);
        sim_shot("ad1");
        sim_move(apps->bar_x + 120, MENUBAR_H + ai * 10 + 4);
        sim_frames(1);
        mouse.left = true;
        sim_frame();
        mouse.left = false;
        sim_frames(8);
        window *w = wm_focused_window();
        snprintf(b, sizeof b, "launched %s\n", w ? w->title : "-");
        wr(1, b);
        T("an Acme app runs",
          w && (strstr(w->title, "Calc") || strstr(w->title, "Lazy") ||
                strstr(w->title, "Sudoku")));
        /* take the group away: delete its files and the folder */
        fs_delete("/APPS/ACME/SUDOKU.APP");
        fs_delete("/APPS/ACME/LAZY.APP");
        fs_delete("/APPS/ACME/GROUP.TXT");
        fs_delete("/APPS/ACME/JUNK.APP");
        fs_delete("/APPS/ACME/CALC.LNK");
        fs_delete("/APPS/ACME/MYGAME.LNK");
        fs_delete("/APPS/ACME/BAD.LNK");
        fs_rmdir("/APPS/ACME");
        wm_focus(win_stack[0]);
        sim_frames(70);
        for (int i = 0; i < g_menubar.menu_count; i++)
            if (!strcmp(g_menubar.menus[i].title, "Apps"))
                apps = &g_menubar.menus[i];
        T("gone from the menu", !GRP("Acme Software"));
        /* copy one in: it appears */
        PUT("/APPS/Multimedia/SUDOKU.APP", su, ns);
        PUT("/APPS/Multimedia/SUDOKU.APP", su, ns);
        sim_frames(70);
        for (int i = 0; i < g_menubar.menu_count; i++)
            if (!strcmp(g_menubar.menus[i].title, "Apps"))
                apps = &g_menubar.menus[i];
        {
            menu *mm = GRP("Multimedia");
            T("copied into Multimedia",
              mm && mm != (menu *)1 && mm->item_count == 2);
        }
        /* /APPS deleted: made again at boot, with the shortcuts */
        fs_rm_rf("/APPS");
        extern void apps_dir_defaults_pub(void);
        apps_dir_defaults_pub();
        T("defaults: Accessories/TEditor.LNK",
          fs_find("/APPS/Accessories/TEditor.LNK", NULL) || ({
              dir_entry_t d_;
              fs_find("/APPS/Accessories/TEditor.LNK", &d_);
          }));
        T("defaults: FileF.LNK at the top", ({
              dir_entry_t d_;
              fs_find("/APPS/FileF.LNK", &d_);
          }));
        return fails;
    }
    return -1;
}
