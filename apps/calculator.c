#include "os/api.h"

#define CALC_W 105
#define CALC_H 125
#define CALC_X 40
#define CALC_Y 30

#define DISPLAY_X 4
#define DISPLAY_Y 6
#define DISPLAY_W 94
#define DISPLAY_H 14

#define BTN_COLS 4
#define BTN_ROWS 5
#define BTN_W 22
#define BTN_H 14
#define BTN_PAD 2
#define BTN_START_X 4
#define BTN_START_Y 24

#define MAX_DISPLAY 16

typedef struct {
    window *win;

    char display[MAX_DISPLAY + 1];
    int display_len;

    double accumulator;
    double operand;
    char pending_op;
    bool fresh_operand;
    bool result_shown;
    bool error;
} calc_state_t;

app_descriptor calculator_app;

typedef struct {
    const char *label;
    int col, row;
    int span;
    uint8_t bg;
} btn_def_t;

static const btn_def_t BUTTONS[] = {
    {"C", 0, 0, 1, DARK_GRAY},  {"+/-", 1, 0, 1, DARK_GRAY},
    {"%", 2, 0, 1, DARK_GRAY},  {"/", 3, 0, 1, LIGHT_BLUE},

    {"7", 0, 1, 1, LIGHT_GRAY}, {"8", 1, 1, 1, LIGHT_GRAY},
    {"9", 2, 1, 1, LIGHT_GRAY}, {"*", 3, 1, 1, LIGHT_BLUE},

    {"4", 0, 2, 1, LIGHT_GRAY}, {"5", 1, 2, 1, LIGHT_GRAY},
    {"6", 2, 2, 1, LIGHT_GRAY}, {"-", 3, 2, 1, LIGHT_BLUE},

    {"1", 0, 3, 1, LIGHT_GRAY}, {"2", 1, 3, 1, LIGHT_GRAY},
    {"3", 2, 3, 1, LIGHT_GRAY}, {"+", 3, 3, 1, LIGHT_BLUE},

    {"0", 0, 4, 2, LIGHT_GRAY}, {".", 2, 4, 1, LIGHT_GRAY},
    {"=", 3, 4, 1, RED},
};

#define NUM_BUTTONS ((int)(sizeof(BUTTONS) / sizeof(BUTTONS[0])))

static void double_to_str(double v, char *buf, int max) {
    int idx = 0;
    if (v < 0.0) {
        buf[idx++] = '-';
        v = -v;
    }

    uint32_t ipart = (uint32_t)v;
    double fpart = v - (double)ipart;

    char tmp[32];
    int ti = 0;
    if (ipart == 0) {
        tmp[ti++] = '0';
    } else {
        uint32_t n = ipart;
        while (n > 0) {
            tmp[ti++] = '0' + (n % 10);
            n /= 10;
        }
        for (int a = 0, b = ti - 1; a < b; a++, b--) {
            char c = tmp[a];
            tmp[a] = tmp[b];
            tmp[b] = c;
        }
    }
    for (int i = 0; i < ti && idx < max - 1; i++)
        buf[idx++] = tmp[i];

    if (fpart > 0.000001) {
        if (idx < max - 1)
            buf[idx++] = '.';
        int decimals = 0;
        while (fpart > 0.000001 && decimals < 6 && idx < max - 1) {
            fpart *= 10.0;
            int digit = (int)fpart;
            buf[idx++] = '0' + digit;
            fpart -= digit;
            decimals++;
        }
        while (idx > 1 && buf[idx - 1] == '0')
            idx--;
        if (buf[idx - 1] == '.')
            idx--;
    }

    buf[idx] = '\0';
}

static void calc_set_display(calc_state_t *s, const char *str) {
    int i = 0;
    while (str[i] && i < MAX_DISPLAY) {
        s->display[i] = str[i];
        i++;
    }
    s->display[i] = '\0';
    s->display_len = i;
}

static void calc_reset(calc_state_t *s) {
    calc_set_display(s, "0");
    s->accumulator = 0.0;
    s->operand = 0.0;
    s->pending_op = 0;
    s->fresh_operand = true;
    s->result_shown = false;
    s->error = false;
}

static double calc_parse(const char *buf) {
    double result = 0.0;
    int i = 0;
    int sign = 1;
    if (buf[i] == '-') {
        sign = -1;
        i++;
    }
    while (buf[i] >= '0' && buf[i] <= '9') {
        result = result * 10.0 + (buf[i] - '0');
        i++;
    }
    if (buf[i] == '.') {
        i++;
        double frac = 0.1;
        while (buf[i] >= '0' && buf[i] <= '9') {
            result += (buf[i] - '0') * frac;
            frac *= 0.1;
            i++;
        }
    }
    return result * sign;
}

static void calc_apply_op(calc_state_t *s) {
    double a = s->accumulator;
    double b = calc_parse(s->display);
    double r = 0.0;
    switch (s->pending_op) {
    case '+':
        r = a + b;
        break;
    case '-':
        r = a - b;
        break;
    case '*':
        r = a * b;
        break;
    case '/':
        if (b == 0.0) {
            s->error = true;
            calc_set_display(s, "Error");
            return;
        }
        r = a / b;
        break;
    default:
        r = b;
        break;
    }
    char buf[MAX_DISPLAY + 1];
    double_to_str(r, buf, MAX_DISPLAY);
    calc_set_display(s, buf);
    s->accumulator = r;
    s->fresh_operand = true;
    s->result_shown = true;
}

static void calc_on_button(calc_state_t *s, const char *lbl) {
    if (s->error && lbl[0] != 'C')
        return;

    if (lbl[0] == 'C') {
        calc_reset(s);
        return;
    }

    if (lbl[0] == '+' && lbl[1] == '/') {
        if (s->display[0] == '-') {
            calc_set_display(s, s->display + 1);
        } else {
            char buf[MAX_DISPLAY + 2];
            buf[0] = '-';
            int i = 0;
            while (s->display[i]) {
                buf[i + 1] = s->display[i];
                i++;
            }
            buf[i + 1] = '\0';
            calc_set_display(s, buf);
        }
        return;
    }

    if (lbl[0] == '%') {
        double v = calc_parse(s->display) / 100.0;
        char buf[MAX_DISPLAY + 1];
        double_to_str(v, buf, MAX_DISPLAY);
        calc_set_display(s, buf);
        s->fresh_operand = true;
        return;
    }

    if (lbl[0] == '+' || lbl[0] == '-' || lbl[0] == '*' || lbl[0] == '/') {
        if (s->pending_op && !s->fresh_operand)
            calc_apply_op(s);
        s->accumulator = calc_parse(s->display);
        s->pending_op = lbl[0];
        s->fresh_operand = true;
        s->result_shown = false;
        return;
    }

    if (lbl[0] == '=') {
        if (s->pending_op)
            calc_apply_op(s);
        s->pending_op = 0;
        return;
    }

    if (lbl[0] == '.') {
        if (s->fresh_operand) {
            calc_set_display(s, "0.");
            s->fresh_operand = false;
            return;
        }
        for (int i = 0; s->display[i]; i++)
            if (s->display[i] == '.')
                return;
        if (s->display_len < MAX_DISPLAY) {
            s->display[s->display_len++] = '.';
            s->display[s->display_len] = '\0';
        }
        return;
    }

    if (lbl[0] >= '0' && lbl[0] <= '9') {
        if (s->fresh_operand || s->result_shown) {
            calc_set_display(s, lbl);
            s->fresh_operand = false;
            s->result_shown = false;
        } else {
            if (s->display_len < MAX_DISPLAY) {
                if (s->display_len == 1 && s->display[0] == '0') {
                    s->display[0] = lbl[0];
                } else {
                    s->display[s->display_len++] = lbl[0];
                    s->display[s->display_len] = '\0';
                }
            }
        }
    }
}

static void calc_draw(window *win, void *ud) {
    calc_state_t *s = (calc_state_t *)ud;
    if (!s)
        return;

    int wx = win->x + 1;
    int wy = win->y + MENUBAR_H + 16;
    int ww = win->w - 2;
    (void)ww;

    fill_rect(wx + DISPLAY_X, wy + DISPLAY_Y, DISPLAY_W, DISPLAY_H, BLACK);
    draw_rect(wx + DISPLAY_X, wy + DISPLAY_Y, DISPLAY_W, DISPLAY_H, DARK_GRAY);

    int txt_w = s->display_len * 5;
    int txt_x = wx + DISPLAY_X + DISPLAY_W - txt_w - 3;
    if (txt_x < wx + DISPLAY_X + 2)
        txt_x = wx + DISPLAY_X + 2;
    draw_string(txt_x, wy + DISPLAY_Y + 4, s->display, s->error ? RED : WHITE,
                2);

    for (int i = 0; i < NUM_BUTTONS; i++) {
        const btn_def_t *b = &BUTTONS[i];
        int bx = wx + BTN_START_X + b->col * (BTN_W + BTN_PAD);
        int by = wy + BTN_START_Y + b->row * (BTN_H + BTN_PAD);
        int bw = BTN_W * b->span + BTN_PAD * (b->span - 1);

        fill_rect(bx, by, bw, BTN_H, b->bg);
        draw_rect(bx, by, bw, BTN_H, BLACK);

        int lw = (int)strlen(b->label) * 5;
        int lx = bx + bw / 2 - lw / 2;
        int ly = by + BTN_H / 2 - 3;
        draw_string(lx, ly, (char *)b->label, WHITE, 2);
    }
}

static int calc_hit_button(calc_state_t *s, int mx, int my) {
    int wx = s->win->x + 1;
    int wy = s->win->y + MENUBAR_H + 16;

    for (int i = 0; i < NUM_BUTTONS; i++) {
        const btn_def_t *b = &BUTTONS[i];
        int bx = wx + BTN_START_X + b->col * (BTN_W + BTN_PAD);
        int by = wy + BTN_START_Y + b->row * (BTN_H + BTN_PAD);
        int bw = BTN_W * b->span + BTN_PAD * (b->span - 1);
        if (mx >= bx && mx < bx + bw && my >= by && my < by + BTN_H)
            return i;
    }
    return -1;
}

static bool calc_close(window *w) {
    (void)w;
    os_quit_app_by_desc(&calculator_app);
    return true;
}

static void on_file_close(void) { calc_close(NULL); }
static void on_about(void) {
    modal_show(MODAL_INFO, "About Calculator",
               "Calculator v1.0\nASMOS System App", NULL, NULL);
}

static void calc_init(void *state) {
    calc_state_t *s = (calc_state_t *)state;

    const window_spec_t spec = {
        .x = CALC_X,
        .y = CALC_Y,
        .w = CALC_W,
        .h = CALC_H,
        .title = "Calculator",
        .title_color = WHITE,
        .bar_color = DARK_GRAY,
        .content_color = LIGHT_GRAY,
        .visible = true,
        .resizable = false,
        .on_close = calc_close,
    };
    s->win = wm_register(&spec);
    if (!s->win)
        return;

    s->win->on_draw = calc_draw;
    s->win->on_draw_userdata = s;

    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "Close", on_file_close);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "About Calculator", on_about);

    calc_reset(s);
}

static void calc_on_frame(void *state) {
    calc_state_t *s = (calc_state_t *)state;
    if (!s->win || !s->win->visible)
        return;

    bool focused = window_is_focused(s->win);

    if (mouse.left_clicked && focused) {
        int hit = calc_hit_button(s, mouse.x, mouse.y);
        if (hit >= 0)
            calc_on_button(s, BUTTONS[hit].label);
    }

    if (kb.key_pressed && focused) {
        char c = kb.last_char;
        if (c >= '0' && c <= '9') {
            char tmp[2] = {c, '\0'};
            calc_on_button(s, tmp);
        } else if (c == '+') {
            calc_on_button(s, "+");
        } else if (c == '-') {
            calc_on_button(s, "-");
        } else if (c == '*') {
            calc_on_button(s, "*");
        } else if (c == '/') {
            calc_on_button(s, "/");
        } else if (c == '.') {
            calc_on_button(s, ".");
        } else if (c == '%') {
            calc_on_button(s, "%");
        } else if (kb.last_scancode == ENTER) {
            calc_on_button(s, "=");
        } else if (kb.last_scancode == BACKSPACE) {
            if (!s->fresh_operand && !s->result_shown && s->display_len > 1) {
                s->display[--s->display_len] = '\0';
            } else {
                calc_set_display(s, "0");
                s->fresh_operand = true;
            }
        } else if (kb.last_scancode == ESC) {
            calc_reset(s);
        }
    }

    calc_draw(s->win, s);
}

static void calc_destroy(void *state) {
    calc_state_t *s = (calc_state_t *)state;
    if (s->win) {
        wm_unregister(s->win);
        s->win = NULL;
    }
}

app_descriptor calculator_app = {
    .name = "CALCULATOR",
    .state_size = sizeof(calc_state_t),
    .init = calc_init,
    .on_frame = calc_on_frame,
    .destroy = calc_destroy,
    .single_instance = true,
};
