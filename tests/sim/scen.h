#include "fs/install.h"
#include "fs/zip.h"
#include "sys.h"
#include "ui/uifont.h"
#include "os/appapi.h"
#include "os/appload.h"
#include "config/config.h"
#include "config/runtime_config.h"
#include "config/video_cfg.h"
#include "drivers/fdd.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/gpu.h"
#include "fs/fdd_ctrl.h"
#include "fs/fs.h"
#include "lib/alloc.h"
#include "lib/audio.h"
#include "lib/core.h"
#include "lib/graphics.h"
#include "lib/string.h"
#include "lib/time.h"
#include "os/appguard.h"
#include "os/appload.h"
#include "os/clipboard.h"
#include "os/error.h"
#include "os/os.h"
#include "os/prefs.h"
#include "os/print.h"
#include "shell/cmds.h"
#include "ui/controls.h"
#include "ui/desktop_fs.h"
#include "ui/fileops.h"
#include "ui/menubar.h"
#include "ui/popup.h"
#include "ui/saver.h"
#include "ui/ui.h"
#include "fs/fat_io.h"

app_instance_t *sim_launch(const char *name);
void tree_ev(control *c, int ev);
void sim_save(const char *, const void *, unsigned);
void sim_frame(void);
void wr(int fd, const char *s);
bool load_file(const char *path, uint8_t *dst, uint32_t max, uint32_t *got);
void sim_rclick(int, int);
extern const char *sim_arg2;
extern uint32_t g_sim_abort_vec;
void sim_alarm_ms(int);
void sim_shot(const char *);
void sim_frames(int);
void sim_click(int, int);
void sim_log(const char *);
void sim_key(uint8_t);
void sim_ext(uint8_t);
void sim_type(const char *);
void sim_key2(uint8_t, char, bool, bool);
void sim_dblclick(int, int);
void sim_drag(int, int, int, int);
void sim_move(int, int);
extern char *g_tree_log;
void gpu_blit(void);
void gpu_blit_rows(int y0, int y1);

static char *zt_line(char **p) {
    char *s = *p;
    if (!s || !*s) return 0;
    char *e = s;
    while (*e && *e != '\n') e++;
    if (*e) *e++ = 0;
    *p = e;
    return s;
}

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

#define T(desc, cond)                                                          \
    do {                                                                       \
        bool _ok = (cond);                                                     \
        fails += !_ok;                                                         \
        snprintf(b, sizeof b, "  %-50s %s\n", desc, _ok ? "ok" : "FAIL");      \
        wr(1, b);                                                              \
    } while (0)

#define OK(e) ((e) == NULL)

#define CTL_WITH(T_, TXT)                                                      \
    ({                                                                         \
        control *r_ = NULL;                                                    \
        for (control *c_ = w->controls; c_; c_ = c_->next)                     \
            if (c_->type == T_ && c_->text &&                                  \
                !strncmp(c_->text, TXT, strlen(TXT)))                          \
                r_ = c_;                                                       \
        r_;                                                                    \
    })

#define CD_FRAMES() sim_frames(40) /* the drive is looked at twice a second */

#define FIND(T)                                                                \
    ({                                                                         \
        window *r_ = NULL;                                                     \
        for (int i_ = 0; i_ < win_count; i_++)                                 \
            if (win_stack[i_]->title && !strcmp(win_stack[i_]->title, T))      \
                r_ = win_stack[i_];                                            \
        r_;                                                                    \
    })

#define R() (seed = seed * 1103515245u + 12345u, seed >> 8)

#define g_fdd_img g_fdc_img

#define g_fdd_formatted_tracks g_fdc_formats

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

#define PUT(p, d, n)                                                           \
    do {                                                                       \
        fs_delete(p);                                                          \
        fs_create(p, &f);                                                      \
        fs_write(&f, d, n);                                                    \
        fs_close(&f);                                                          \
    } while (0)

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

#define FDD_ICON()                                                             \
    ({                                                                         \
        int ic_ = -1;                                                          \
        for (int i = 0; i < desktop_fs_count(); i++)                           \
            if (desktop_fs_items()[i].drive_id == DRIVE_FDD0 &&                \
                desktop_fs_items()[i].kind == DESKTOP_ITEM_FDD)                \
                ic_ = i;                                                       \
        ic_;                                                                   \
    })

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
