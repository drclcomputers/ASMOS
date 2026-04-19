#include "os/api.h"
#include "shell/asm/asm.h"

#define ASMASM_W 280
#define ASMASM_H 165

typedef struct {
    window *win;
    char src_path[64];
    char dst_path[64];
    char status[128];
    uint32_t status_timer;
    int fname_mode;
    char fname_buf[64];
    int fname_len;
} asmasm_state_t;

app_descriptor asmasm_app;

static asmasm_state_t *active_asmasm(void) {
    window *fw = wm_focused_window();
    if (!fw)
        return NULL;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (!a->running || a->desc != &asmasm_app)
            continue;
        asmasm_state_t *s = (asmasm_state_t *)a->state;
        if (s->win == fw)
            return s;
    }
    return NULL;
}

static void set_status(asmasm_state_t *s, const char *msg) {
    strncpy(s->status, msg, 127);
    s->status[127] = '\0';
    s->status_timer = 360;
}

static bool asmasm_close(window *w) {
    (void)w;
    os_quit_app_by_desc(&asmasm_app);
    return true;
}

static void do_assemble_gui(asmasm_state_t *s) {
    if (s->src_path[0] == '\0') {
        set_status(s, "Set source file first.");
        return;
    }

    char args[130];
    if (s->dst_path[0]) {
        sprintf(args, "%s %s", s->src_path, s->dst_path);
    } else {
        strncpy(args, s->src_path, 127);
        args[127] = '\0';
        strncpy(s->dst_path, s->src_path, 63);
        char *dot = strrchr(s->dst_path, '.');
        if (dot)
            strcpy(dot, ".BIN");
        else
            strcat(s->dst_path, ".BIN");
    }

    static char out_buf[256];
    out_buf[0] = '\0';
    cmd_asmasm(args, out_buf, sizeof(out_buf));

    int len = (int)strlen(out_buf);
    while (len > 0 && (out_buf[len - 1] == '\n' || out_buf[len - 1] == '\r'))
        out_buf[--len] = '\0';

    set_status(s, len > 0 ? out_buf : "Done.");
}

static void menu_set_src(void) {
    asmasm_state_t *s = active_asmasm();
    if (!s)
        return;
    s->fname_buf[0] = '\0';
    s->fname_len = 0;
    s->fname_mode = 1;
}
static void menu_set_dst(void) {
    asmasm_state_t *s = active_asmasm();
    if (!s)
        return;
    s->fname_buf[0] = '\0';
    s->fname_len = 0;
    s->fname_mode = 2;
}
static void menu_assemble(void) {
    asmasm_state_t *s = active_asmasm();
    if (!s)
        return;
    do_assemble_gui(s);
}
static void menu_about_asm(void) {
    modal_show(MODAL_INFO, "About ASMASM",
               "ASMASM v1.1\nx86 Assembler for ASMOS\n"
               "GUI front-end.  Also available in CLI:\n"
               "  asm <source.asm> [output.bin]",
               NULL, NULL);
}
static void menu_close_asm(void) { os_quit_app_by_desc(&asmasm_app); }

static void asmasm_draw(window *win, void *ud) {
    asmasm_state_t *s = (asmasm_state_t *)ud;
    if (!s)
        return;

    int wx = win->x + 1;
    int wy = win->y + MENUBAR_H + 16;
    int ww = win->w - 2;
    int wh = win->h - 16;

    fill_rect(wx, wy, ww, wh, DARK_GRAY);
    fill_rect(wx, wy, ww, 10, BLACK);
    draw_string(wx + 4, wy + 2, "ASMASM  -  x86 Assembler", LIGHT_CYAN, 2);

    int y = wy + 14;

    /* Source path */
    draw_string(wx + 4, y + 2, "Source (.ASM):", LIGHT_GRAY, 2);
    fill_rect(wx + 4, y + 12, ww - 8, 10, BLACK);
    draw_rect(wx + 4, y + 12, ww - 8, 10, CYAN);
    draw_string(wx + 6, y + 13, s->src_path[0] ? s->src_path : "<not set>",
                s->src_path[0] ? WHITE : DARK_GRAY, 2);
    y += 28;

    /* Output path */
    draw_string(wx + 4, y + 2, "Output  (.BIN):", LIGHT_GRAY, 2);
    fill_rect(wx + 4, y + 12, ww - 8, 10, BLACK);
    draw_rect(wx + 4, y + 12, ww - 8, 10, CYAN);
    draw_string(wx + 6, y + 13, s->dst_path[0] ? s->dst_path : "<auto>",
                s->dst_path[0] ? WHITE : DARK_GRAY, 2);
    y += 28;

    /* CLI hint */
    draw_string(wx + 4, y + 2, "CLI: asm <src.asm> [out.bin]", LIGHT_YELLOW, 2);
    y += 12;

    /* Assemble button */
    int bw = 80, bh = 14, bx = wx + (ww - bw) / 2;
    fill_rect(bx, y, bw, bh, LIGHT_BLUE);
    draw_rect(bx, y, bw, bh, WHITE);
    draw_string(bx + 10, y + 3, "Assemble [F5]", WHITE, 2);
    y += 18;

    /* Status bar */
    fill_rect(wx + 2, y, ww - 4, 10, BLACK);
    draw_rect(wx + 2, y, ww - 4, 10, DARK_GRAY);
    if (s->status_timer > 0) {
        bool ok = (strncmp(s->status, "asm:", 4) == 0 &&
                   strstr(s->status, "error") == NULL) ||
                  (strncmp(s->status, "assembled", 9) == 0);
        draw_string(wx + 4, y + 2, s->status, ok ? LIGHT_GREEN : LIGHT_RED, 2);
    } else {
        draw_string(wx + 4, y + 2, "Ready", DARK_GRAY, 2);
    }

    /* File dialog */
    if (s->fname_mode != 0) {
        int bxd = wx + 10, byd = wy + wh / 2 - 20, bwd = ww - 20, bhd = 42;
        fill_rect(bxd + 3, byd + 3, bwd, bhd, BLACK);
        fill_rect(bxd, byd, bwd, bhd, LIGHT_GRAY);
        draw_rect(bxd, byd, bwd, bhd, BLACK);
        fill_rect(bxd, byd, bwd, 11, DARK_GRAY);
        const char *title =
            (s->fname_mode == 1) ? "Source File (.ASM)" : "Output File (.BIN)";
        draw_string(bxd + 4, byd + 2, (char *)title, WHITE, 2);
        draw_string(bxd + 4, byd + 14, "Path:", DARK_GRAY, 2);
        fill_rect(bxd + 30, byd + 13, bwd - 34, 10, WHITE);
        draw_rect(bxd + 30, byd + 13, bwd - 34, 10, BLACK);
        draw_string(bxd + 32, byd + 14, s->fname_buf, BLACK, 2);
        extern volatile uint32_t pit_ticks;
        if ((pit_ticks / 50) % 2 == 0) {
            int cx = bxd + 32 + s->fname_len * 5;
            if (cx < bxd + bwd - 6)
                draw_string(cx, byd + 14, "|", BLACK, 2);
        }
        fill_rect(bxd + 4, byd + 28, 30, 9, LIGHT_BLUE);
        draw_rect(bxd + 4, byd + 28, 30, 9, BLACK);
        draw_string(bxd + 11, byd + 30, "OK", WHITE, 2);
        fill_rect(bxd + 38, byd + 28, 40, 9, DARK_GRAY);
        draw_rect(bxd + 38, byd + 28, 40, 9, BLACK);
        draw_string(bxd + 41, byd + 30, "Cancel", WHITE, 2);
    }
}

static void commit_fname(asmasm_state_t *s) {
    int mode = s->fname_mode;
    s->fname_mode = 0;
    if (mode == 1) {
        strncpy(s->src_path, s->fname_buf, 63);
        s->src_path[63] = '\0';
        /* auto-fill dst */
        strncpy(s->dst_path, s->src_path, 63);
        char *dot = strrchr(s->dst_path, '.');
        if (dot)
            strcpy(dot, ".BIN");
        else
            strcat(s->dst_path, ".BIN");
        set_status(s, "Source path set.");
    } else {
        strncpy(s->dst_path, s->fname_buf, 63);
        s->dst_path[63] = '\0';
        set_status(s, "Output path set.");
    }
}

static void asmasm_init(void *state) {
    asmasm_state_t *s = (asmasm_state_t *)state;
    const window_spec_t spec = {
        .x = 30,
        .y = 25,
        .w = ASMASM_W,
        .h = ASMASM_H,
        .min_w = ASMASM_W,
        .min_h = ASMASM_H,
        .resizable = false,
        .title = "ASMASM",
        .title_color = WHITE,
        .bar_color = DARK_GRAY,
        .content_color = DARK_GRAY,
        .visible = true,
        .on_close = asmasm_close,
    };
    s->win = wm_register(&spec);
    if (!s->win)
        return;
    s->win->on_draw = asmasm_draw;
    s->win->on_draw_userdata = s;

    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "Set Source File", menu_set_src);
    menu_add_item(file_menu, "Set Output File", menu_set_dst);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "Assemble", menu_assemble);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "Close", menu_close_asm);
    menu_add_item(file_menu, "About", menu_about_asm);
}

static void asmasm_on_frame(void *state) {
    asmasm_state_t *s = (asmasm_state_t *)state;
    if (!s->win || !s->win->visible)
        return;
    if (s->status_timer > 0)
        s->status_timer--;

    if (s->fname_mode != 0) {
        if (kb.key_pressed) {
            if (kb.last_scancode == ESC) {
                s->fname_mode = 0;
            } else if (kb.last_scancode == ENTER && s->fname_len > 0) {
                commit_fname(s);
            } else if (kb.last_scancode == BACKSPACE) {
                if (s->fname_len > 0)
                    s->fname_buf[--s->fname_len] = '\0';
            } else if (kb.last_char >= 32 && kb.last_char < 127 &&
                       s->fname_len < 63) {
                char c = kb.last_char;
                bool ok = (c == '/' || c == '.' || (c >= 'A' && c <= 'Z') ||
                           (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                           c == '_' || c == '-');
                if (ok) {
                    s->fname_buf[s->fname_len++] = c;
                    s->fname_buf[s->fname_len] = '\0';
                }
            }
        }
        if (mouse.left_clicked) {
            int wx2 = s->win->x + 1, wy2 = s->win->y + MENUBAR_H + 16;
            int ww2 = s->win->w - 2, wh2 = s->win->h - 16;
            int bxd = wx2 + 10, byd = wy2 + wh2 / 2 - 20, bwd = ww2 - 20;
            bool ok_hit = (mouse.x >= bxd + 4 && mouse.x < bxd + 34 &&
                           mouse.y >= byd + 28 && mouse.y < byd + 37);
            bool can_hit = (mouse.x >= bxd + 38 && mouse.x < bxd + 78 &&
                            mouse.y >= byd + 28 && mouse.y < byd + 37);
            if (can_hit)
                s->fname_mode = 0;
            else if (ok_hit && s->fname_len > 0)
                commit_fname(s);
        }
        asmasm_draw(s->win, s);
        return;
    }

    int wx = s->win->x + 1, wy = s->win->y + MENUBAR_H + 16, ww = s->win->w - 2;
    int by = wy + 14 + 28 + 28 + 12;
    int bw = 80, bh = 14, bx = wx + (ww - bw) / 2;
    if (mouse.left_clicked && mouse.x >= bx && mouse.x < bx + bw &&
        mouse.y >= by && mouse.y < by + bh)
        do_assemble_gui(s);

    if (kb.key_pressed && kb.last_scancode == F5)
        do_assemble_gui(s);

    asmasm_draw(s->win, s);
}

static void asmasm_destroy(void *state) {
    asmasm_state_t *s = (asmasm_state_t *)state;
    if (s->win) {
        wm_unregister(s->win);
        s->win = NULL;
    }
}

app_descriptor asmasm_app = {
    .name = "ASMASM",
    .state_size = sizeof(asmasm_state_t),
    .init = asmasm_init,
    .on_frame = asmasm_on_frame,
    .destroy = asmasm_destroy,
};
