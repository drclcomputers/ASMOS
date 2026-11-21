/* Single/multiline edits support keyboard and mouse selection, clipboard
 * shortcuts, word/document navigation, and scrolling. */
#include "config/runtime_config.h"
#include "ui/ctl_internal.h"
#include "ui/uifont.h"

#include "config/config.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "fs/fs.h"
#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"
#include "os/clipboard.h"
#include "ui/window.h"

#define SB_W 8
#define PAD 3
#define MULTI_DEFAULT_MAX 65535
#define SINGLE_DEFAULT_MAX 255

static uint32_t s_last_activity; /* keeps the caret solid while typing */
static int s_sb_part;

static bool multi(const control *c) { return (c->style & ES_MULTILINE) != 0; }
static int maxlen(const control *c) {
    return c->max_len > 0 ? c->max_len
                          : (multi(c) ? MULTI_DEFAULT_MAX : SINGLE_DEFAULT_MAX);
}

// positions

static int line_start(const control *c, int pos) {
    while (pos > 0 && c->text[pos - 1] != '\n')
        pos--;
    return pos;
}
static int line_end(const control *c, int pos) {
    while (pos < c->text_len && c->text[pos] != '\n')
        pos++;
    return pos;
}

int edit_line_of(const control *c, int pos) {
    int n = 0;
    for (int i = 0; i < pos && i < c->text_len; i++)
        if (c->text[i] == '\n')
            n++;
    return n;
}
int edit_col_of(const control *c, int pos) { return pos - line_start(c, pos); }

int edit_line_count(const control *c) {
    return edit_line_of(c, c->text_len) + 1;
}

static int pos_of(const control *c, int line, int col) {
    int p = 0;
    while (line > 0 && p < c->text_len) {
        if (c->text[p] == '\n')
            line--;
        p++;
    }
    int e = line_end(c, p);
    if (col < 0)
        col = 0;
    return p + col > e ? e : p + col;
}

/* character cell and font: the 4x6 font, or the terminal one */
static int cw_of(const control *c) {
    return (c->style & ES_TERMFONT) ? UI_TERM_CW : TH_CHAR_W;
}
static int font_of(const control *c) {
    return (c->style & ES_TERMFONT) ? UI_FONT_TERM : FONT_MONO;
}

static int rows_of(const control *c) {
    int r = (c->ah - 4) / TH_LINE_H;
    return r < 1 ? 1 : r;
}
static bool has_vsb(const control *c) {
    return multi(c) && edit_line_count(c) > rows_of(c);
}
static int cols_of(const control *c) {
    int w = c->aw - 2 * PAD - (has_vsb(c) ? SB_W : 0);
    int n = w / cw_of(c);
    return n < 1 ? 1 : n;
}

void edit_fix_scroll(control *c) {
    if (c->cursor > c->text_len)
        c->cursor = c->text_len;
    if (c->anchor > c->text_len)
        c->anchor = c->text_len;
    int cols = cols_of(c);
    int col = multi(c) ? edit_col_of(c, c->cursor) : c->cursor;
    if (col < c->scroll_x)
        c->scroll_x = col;
    if (col >= c->scroll_x + cols)
        c->scroll_x = col - cols + 1;
    if (c->scroll_x < 0)
        c->scroll_x = 0;
    if (multi(c)) {
        int rows = rows_of(c);
        int line = edit_line_of(c, c->cursor);
        if (line < c->scroll_y)
            c->scroll_y = line;
        if (line >= c->scroll_y + rows)
            c->scroll_y = line - rows + 1;
        int maxs = edit_line_count(c) - rows;
        if (c->scroll_y > maxs)
            c->scroll_y = maxs;
        if (c->scroll_y < 0)
            c->scroll_y = 0;
    }
}

// editing primitives

static void sel_range(const control *c, int *a, int *b) {
    *a = c->cursor < c->anchor ? c->cursor : c->anchor;
    *b = c->cursor < c->anchor ? c->anchor : c->cursor;
}

static bool del_range(control *c, int a, int b) {
    if (b <= a)
        return false;
    memmove(c->text + a, c->text + b, c->text_len - b + 1);
    c->text_len -= b - a;
    c->rev++;
    c->cursor = c->anchor = a;
    return true;
}

static bool del_sel(control *c) {
    int a, b;
    sel_range(c, &a, &b);
    return del_range(c, a, b);
}

static char filter(const control *c, char ch) {
    uint32_t st = c->style;
    if (ch == '\n')
        return multi(c) ? ch : 0;
    if (!font_has_char((unsigned char)ch)) /* ASCII, or the code page's */
        return 0;
    if (st & ES_NUMBER)
        return (ch >= '0' && ch <= '9') ? ch : 0;
    if (st & ES_DECIMAL)
        return ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-') ? ch : 0;
    if ((st & ES_UPPER) && ch >= 'a' && ch <= 'z')
        ch -= 32;
    if (st & ES_FILENAME) /* the characters a new name may have, + '/' */
        return (fs_new_name_char_ok(ch) || ch == '/') ? ch : 0;
    return ch;
}

/* insert raw text at the cursor (after removing the selection) */
static bool insert_raw(control *c, const char *s, int n) {
    del_sel(c);
    int room = maxlen(c) - c->text_len;
    if (n > room)
        n = room;
    if (n <= 0 || !ctl_text_reserve(c, c->text_len + n))
        return false;
    memmove(c->text + c->cursor + n, c->text + c->cursor,
            c->text_len - c->cursor + 1);
    memcpy(c->text + c->cursor, s, n);
    c->text_len += n;
    c->rev++;
    c->cursor += n;
    c->anchor = c->cursor;
    return true;
}

void edit_insert(control *c, const char *s) {
    if (!c || c->type != CTL_EDIT)
        return;
    /* filter a copy */
    int n = (int)strlen(s);
    char *tmp = (char *)kmalloc(n + 1);
    if (!tmp)
        return;
    int k = 0;
    for (int i = 0; i < n; i++) {
        char ch = (s[i] == '\t' && !multi(c)) ? ' '
                  : s[i] == '\t'              ? '\t'
                                              : filter(c, s[i]);
        if (ch)
            tmp[k++] = ch;
    }
    insert_raw(c, tmp, k);
    kfree(tmp);
    edit_fix_scroll(c);
}

void edit_select(control *c, int from, int to) {
    if (!c)
        return;
    if (from < 0)
        from = 0;
    if (to > c->text_len || to < 0)
        to = c->text_len;
    c->anchor = from;
    c->cursor = to;
    edit_fix_scroll(c);
}

void edit_select_all(control *c) { edit_select(c, 0, -1); }

void edit_goto(control *c, int line, int col) {
    if (!c)
        return;
    c->cursor = c->anchor = pos_of(c, line, col);
    c->want_col = col;
    edit_fix_scroll(c);
}

bool edit_find(control *c, const char *needle, bool from_start) {
    if (!c || !needle || !needle[0])
        return false;
    int n = (int)strlen(needle);
    int a, b;
    sel_range(c, &a, &b);
    int start = from_start ? 0 : b;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = start; i + n <= c->text_len; i++) {
            int k = 0;
            while (k < n && ((c->text[i + k] | 0x20) == (needle[k] | 0x20) ||
                             c->text[i + k] == needle[k]))
                k++;
            if (k == n) {
                edit_select(c, i, i + n);
                return true;
            }
        }
        if (start == 0)
            break;
        start = 0; /* wrap around once */
    }
    return false;
}

void edit_copy(control *c) {
    int a, b;
    if (!c || c->type != CTL_EDIT || (c->style & ES_PASSWORD))
        return;
    sel_range(c, &a, &b);
    if (b > a)
        clipboard_set_text(c->text + a, b - a);
}

void edit_cut(control *c) {
    if (!c || c->type != CTL_EDIT || (c->style & ES_READONLY))
        return;
    edit_copy(c);
    if (del_sel(c)) {
        edit_fix_scroll(c);
        ctl_invoke(c, EV_CHANGE);
    }
}

void edit_delete_sel(control *c) {
    if (!c || c->type != CTL_EDIT || (c->style & ES_READONLY))
        return;
    if (del_sel(c)) {
        edit_fix_scroll(c);
        ctl_invoke(c, EV_CHANGE);
    }
}

bool edit_has_sel(const control *c) {
    return c && c->type == CTL_EDIT && c->cursor != c->anchor;
}

void edit_paste(control *c) {
    if (!c || c->type != CTL_EDIT || (c->style & ES_READONLY) ||
        !clipboard_has_text())
        return;
    edit_insert(c, g_clipboard.text);
    ctl_invoke(c, EV_CHANGE);
}

// word movement

static bool wordch(char ch) {
    return (ch >= '0' && ch <= '9') ||
           ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'z') || ch == '_';
}
static int word_left(const control *c, int p) {
    while (p > 0 && !wordch(c->text[p - 1]))
        p--;
    while (p > 0 && wordch(c->text[p - 1]))
        p--;
    return p;
}
static int word_right(const control *c, int p) {
    while (p < c->text_len && wordch(c->text[p]))
        p++;
    while (p < c->text_len && !wordch(c->text[p]))
        p++;
    return p;
}

// keyboard

static void moved(control *c, int np, bool keep_col) {
    c->cursor = np;
    if (!kb.shift)
        c->anchor = np;
    if (!keep_col)
        c->want_col = multi(c) ? edit_col_of(c, np) : np;
    edit_fix_scroll(c);
}

static void changed(control *c) {
    edit_fix_scroll(c);
    ctl_invoke(c, EV_CHANGE);
}

static bool window_has_default(const control *c) {
    for (control *o = c->win->controls; o; o = o->next)
        if (o->type == CTL_BUTTON && (o->style & CS_DEFAULT) && o->visible &&
            o->enabled)
            return true;
    return false;
}

bool edit_key(control *c) {
    uint8_t sc = kb.last_scancode;
    char ch = kb.last_char;
    bool ro = (c->style & ES_READONLY) != 0;
    bool ml = multi(c);
    s_last_activity = time_millis();

    if (kb.ctrl) {
        if (sc == A_KEY) {
            edit_select_all(c);
            return true;
        }
        if (sc == C_KEY || sc == X_KEY) {
            int a, b;
            sel_range(c, &a, &b);
            if (b > a && !(c->style & ES_PASSWORD))
                clipboard_set_text(c->text + a, b - a);
            if (sc == X_KEY && !ro && del_sel(c))
                changed(c);
            return true;
        }
        if (sc == V_KEY) {
            if (!ro && clipboard_has_text()) {
                edit_insert(c, g_clipboard.text);
                changed(c);
            }
            return true;
        }
        if (sc == LEFT_ARROW) {
            moved(c, word_left(c, c->cursor), false);
            return true;
        }
        if (sc == RIGHT_ARROW) {
            moved(c, word_right(c, c->cursor), false);
            return true;
        }
        if (sc == HOME) {
            moved(c, 0, false);
            return true;
        }
        if (sc == END) {
            moved(c, c->text_len, false);
            return true;
        }
        return false; /* Ctrl+S etc. belong to the app */
    }

    int a, b;
    sel_range(c, &a, &b);
    switch (sc) {
    case LEFT_ARROW:
        if (b > a && !kb.shift)
            moved(c, a, false);
        else
            moved(c, c->cursor > 0 ? c->cursor - 1 : 0, false);
        return true;
    case RIGHT_ARROW:
        if (b > a && !kb.shift)
            moved(c, b, false);
        else
            moved(c, c->cursor < c->text_len ? c->cursor + 1 : c->text_len,
                  false);
        return true;
    case HOME:
        moved(c, ml ? line_start(c, c->cursor) : 0, false);
        return true;
    case END:
        moved(c, ml ? line_end(c, c->cursor) : c->text_len, false);
        return true;
    case UP_ARROW:
    case DOWN_ARROW:
    case PAGE_UP:
    case PAGE_DOWN: {
        if (!ml) {
            ctl_invoke(c, EV_KEY); /* e.g. command history */
            return true;
        }
        int line = edit_line_of(c, c->cursor);
        int step = (sc == PAGE_UP || sc == PAGE_DOWN) ? rows_of(c) - 1 : 1;
        if (step < 1)
            step = 1;
        line += (sc == UP_ARROW || sc == PAGE_UP) ? -step : step;
        if (line < 0)
            line = 0;
        int last = edit_line_count(c) - 1;
        if (line > last)
            line = last;
        if (sc == PAGE_UP || sc == PAGE_DOWN)
            c->scroll_y += (sc == PAGE_UP) ? -step : step;
        moved(c, pos_of(c, line, c->want_col), true);
        return true;
    }
    case BACKSPACE:
        if (ro)
            return true;
        if (b > a)
            del_sel(c);
        else if (c->cursor > 0)
            del_range(c, c->cursor - 1, c->cursor);
        else
            return true;
        changed(c);
        return true;
    case DELETE:
        if (ro)
            return true;
        if (b > a)
            del_sel(c);
        else if (c->cursor < c->text_len)
            del_range(c, c->cursor, c->cursor + 1);
        else
            return true;
        changed(c);
        return true;
    case ENTER:
        if (ml) {
            if (!ro) {
                /* keep the indentation of the current line */
                int ls = line_start(c, c->cursor), ind = 0;
                while (ls + ind < c->cursor && c->text[ls + ind] == ' ' &&
                       ind < 32)
                    ind++;
                char buf[34];
                buf[0] = '\n';
                memset(buf + 1, ' ', ind);
                insert_raw(c, buf, ind + 1);
                changed(c);
            }
            return true;
        }
        {
            /* let the window's default button run too (dialogs: Enter =
             * OK). Decide before invoking: the handler may destroy c. */
            bool pass = window_has_default(c);
            ctl_invoke(c, EV_SUBMIT);
            return !pass;
        }
    case TAB:
        if (ml && !ro) {
            int col = edit_col_of(c, c->cursor);
            int n = 4 - (col % 4);
            insert_raw(c, "    ", n);
            changed(c);
            return true;
        }
        return false;
    case ESC:
        return false;
    default:
        break;
    }
    if (ch && font_has_char((unsigned char)ch)) {
        if (ro)
            return true;
        char f = filter(c, ch);
        if (f && insert_raw(c, &f, 1))
            changed(c);
        return true;
    }
    return false;
}

// mouse

void edit_mouse(control *c, int mx, int my, bool down, bool dbl) {
    s_last_activity = time_millis();
    if (multi(c) && has_vsb(c) &&
        (mx >= c->aw - 2 - SB_W || (!down && s_sb_part))) {
        int rows = rows_of(c);
        int max = edit_line_count(c) - rows;
        if (down)
            s_sb_part = 0;
        int part = down ? 0 : 5;
        if (!down && s_sb_part != 5)
            return;
        int v = sb_mouse(my - 2, c->ah - 4, c->scroll_y, max, rows, down,
                         down ? &s_sb_part : &part);
        c->scroll_y = v;
        return;
    }
    if (down)
        s_sb_part = 0;
    int p;
    int col = c->scroll_x + (mx - PAD + 2) / cw_of(c);
    if (multi(c)) {
        int line = c->scroll_y + (my - 2) / TH_LINE_H;
        if (line < 0)
            line = 0;
        int last = edit_line_count(c) - 1;
        if (line > last)
            line = last;
        p = pos_of(c, line, col);
    } else {
        p = col < 0 ? 0 : (col > c->text_len ? c->text_len : col);
    }
    if (down && dbl) {
        int s = p, e = p;
        while (s > 0 && wordch(c->text[s - 1]))
            s--;
        while (e < c->text_len && wordch(c->text[e]))
            e++;
        c->anchor = s;
        c->cursor = e;
    } else {
        c->cursor = p;
        if (down && !kb.shift)
            c->anchor = p;
    }
    c->want_col = multi(c) ? edit_col_of(c, c->cursor) : c->cursor;
    edit_fix_scroll(c);
}

/* caret shown right now (blinks; solid just after typing) */
bool edit_caret_on(const control *c) {
    if (g_cfg.caret_no_blink) /* Settings > Performance */
        return c->focused;
    return c->focused && (((time_millis() / 500) & 1) == 0 ||
                          time_millis() - s_last_activity < 600);
}

// drawing

void edit_draw(control *c) {
    bool ro = (c->style & ES_READONLY) != 0;
    uint8_t bg = c->bg != 0xFF         ? c->bg
                 : (c->enabled && !ro) ? TH_FIELD
                                       : TH_FACE;
    if (multi(c) && ro && c->bg == 0xFF)
        bg = TH_FIELD;
    uint8_t fg = !c->enabled ? TH_DISABLED : (c->fg != 0xFF ? c->fg : TH_TEXT);
    th_sunken(c->ax, c->ay, c->aw, c->ah, bg);
    edit_fix_scroll(c);

    bool sb = has_vsb(c);
    int iw = c->aw - 2 * PAD - (sb ? SB_W : 0);
    int rows = multi(c) ? rows_of(c) : 1;
    int a, b;
    sel_range(c, &a, &b);
    bool caret_on = edit_caret_on(c);

    int ty0 = multi(c) ? c->ay + 2 : c->ay + (c->ah - TH_LINE_H) / 2;
    clip_push(c->ax + 2, c->ay + 2, iw + 2 * PAD - 4, c->ah - 4);

    /* first visible line */
    int p = 0;
    if (multi(c)) {
        int line = 0;
        while (line < c->scroll_y && p < c->text_len) {
            if (c->text[p] == '\n')
                line++;
            p++;
        }
    }
    for (int r = 0; r < rows; r++) {
        int ls = p, le = multi(c) ? line_end(c, p) : c->text_len;
        int y = ty0 + r * TH_LINE_H;
        int x0 = c->ax + PAD;
        for (int i = ls + c->scroll_x; i <= le; i++) {
            int x = x0 + (i - ls - c->scroll_x) * cw_of(c);
            if (x > c->ax + PAD + iw)
                break;
            bool in_sel = c->focused && i >= a && i < b;
            if (i < le) {
                char ch = (c->style & ES_PASSWORD) ? '*' : c->text[i];
                if (ch == '\t' || ch == '\r')
                    ch = ' ';
                if (in_sel) {
                    fill_rect(x - 1, y, cw_of(c), TH_LINE_H, TH_SEL_BG);
                    draw_char(x, y + 1, ch, TH_SEL_FG, font_of(c));
                } else {
                    draw_char(x, y + 1, ch, fg, font_of(c));
                }
            } else if (in_sel && multi(c) && i < b) {
                fill_rect(x - 1, y, 3, TH_LINE_H, TH_SEL_BG); /* newline */
            }
            if (i == c->cursor && caret_on)
                draw_line(x - 1, y, x - 1, y + TH_LINE_H - 1, fg);
        }
        if (le >= c->text_len)
            break;
        p = le + 1;
    }
    clip_pop();
    if (sb)
        sb_draw(c->ax + c->aw - 2 - SB_W, c->ay + 2, c->ah - 4, true,
                c->scroll_y, edit_line_count(c) - rows, rows);
}

// creation

control *ctl_edit(window *w, int x, int y, int cw, int ch, const char *text,
                  int max_len, uint32_t style, ctl_handler h) {
    control *c = ctl_create(w, CTL_EDIT, x, y, cw, ch, "", style, h);
    if (!c)
        return NULL;
    c->max_len = max_len;
    ctl_set_text(c, text ? text : "");
    return c;
}
