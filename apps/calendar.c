/* Calendar - a month at a time, with a note for any day.
 *
 *   click a day           select it (a greyed day of the month before or
 *                         after goes to that month)
 *   double-click / Enter  write or change the day's note
 *   arrows                a day / a week      PgUp / PgDn  a month
 *   Home                  today               Del          delete the note
 *
 * Notes live in /PREFS/CALNOTES.TXT, one per line: "YYYYMMDD text".
 * The week starts on Monday (ISO); View switches it to Sunday (kept in
 * /PREFS/CALENDAR.INI). Dates are proleptic Gregorian (lib/time.h). */
#include "fs/fs.h"
#include "lib/time.h"
#include "os/api.h"
#include "os/prefs.h"
#include "ui/uifont.h"

#define NOTES_FILE "/PREFS/CALNOTES.TXT"
#define MAX_NOTES 64
#define NOTE_LEN 60

#define CW 22 /* cell */
#define CH 15
#define HEAD 11 /* weekday names */
#define GRID_W (7 * CW)
#define GRID_H (HEAD + 6 * CH)

typedef struct {
    int32_t day; /* time_days_from_civil */
    char text[NOTE_LEN];
} cal_note_t;

typedef struct {
    window *win; /* must stay first */
    control *title, *grid, *note, *status;
    int y, m;      /* month shown */
    int32_t sel;   /* selected day */
    int32_t today; /* as last drawn */
    bool sunday;   /* week starts on Sunday */
    int nnotes;
    cal_note_t notes[MAX_NOTES];
} cal_state_t;

app_descriptor calendar_app;

static const char *const WD2[7] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};

/* ── notes ──────────────────────────────────────────────────────────── */

static int note_find(const cal_state_t *s, int32_t day) {
    for (int i = 0; i < s->nnotes; i++)
        if (s->notes[i].day == day)
            return i;
    return -1;
}

static void notes_load(cal_state_t *s) {
    static char buf[MAX_NOTES * (NOTE_LEN + 10) + 1];
    fat_file_t f;
    s->nnotes = 0;
    if (!fs_open(NOTES_FILE, &f))
        return;
    int n = fs_read(&f, buf, sizeof(buf) - 1);
    fs_close(&f);
    if (n <= 0)
        return;
    buf[n] = 0;
    for (char *p = buf; *p && s->nnotes < MAX_NOTES;) {
        char *e = p;
        while (*e && *e != '\n')
            e++;
        int y = 0, m = 0, d = 0, k;
        for (k = 0; k < 8 && p + k < e && p[k] >= '0' && p[k] <= '9'; k++)
            if (k < 4)
                y = y * 10 + p[k] - '0';
            else if (k < 6)
                m = m * 10 + p[k] - '0';
            else
                d = d * 10 + p[k] - '0';
        if (k == 8 && p[8] == ' ' && time_date_valid(y, m, d)) {
            cal_note_t *nt = &s->notes[s->nnotes++];
            nt->day = time_days_from_civil(y, m, d);
            int len = (int)(e - (p + 9));
            if (len > 0 && p[9 + len - 1] == '\r')
                len--;
            if (len > NOTE_LEN - 1)
                len = NOTE_LEN - 1;
            memcpy(nt->text, p + 9, len);
            nt->text[len] = 0;
        }
        p = *e ? e + 1 : e;
    }
}

static bool notes_save(cal_state_t *s) {
    static char buf[MAX_NOTES * (NOTE_LEN + 10) + 1];
    int n = 0;
    for (int i = 0; i < s->nnotes; i++) {
        int y, m, d;
        time_civil_from_days(s->notes[i].day, &y, &m, &d);
        n += snprintf(buf + n, sizeof(buf) - n, "%04d%02d%02d %s\n", y, m, d,
                      s->notes[i].text);
    }
    dir_entry_t de;
    if (!fs_find("/PREFS", &de)) {
        if (!fs_mkdir("/PREFS"))
            return false;
        fs_set_hidden("/PREFS", true);
    }
    if (fs_find(NOTES_FILE, &de))
        fs_delete(NOTES_FILE);
    if (!s->nnotes)
        return true;
    fat_file_t f;
    if (!fs_create(NOTES_FILE, &f))
        return false;
    bool ok = fs_write(&f, buf, n) == n;
    fs_close(&f);
    return ok;
}

/* ── state ──────────────────────────────────────────────────────────── */

static int32_t today_days(void) {
    time_full_t t = time_rtc_local();
    return time_days_from_civil((int)t.year, t.month, t.day);
}

/* column of a weekday (0 = Monday) */
static int col_of(const cal_state_t *s, int wd) {
    return s->sunday ? (wd + 1) % 7 : wd;
}

/* day in the first cell of the grid */
static int32_t grid_first(const cal_state_t *s) {
    int32_t first = time_days_from_civil(s->y, s->m, 1);
    return first - col_of(s, time_weekday(s->y, s->m, 1));
}

static void update_text(cal_state_t *s) {
    char b[80];
    snprintf(b, sizeof(b), "%s %d", time_month_name(s->m), s->y);
    ctl_set_text(s->title, b);

    int y, m, d;
    time_civil_from_days(s->sel, &y, &m, &d);
    int32_t diff = s->sel - s->today;
    char rel[20] = "";
    if (diff == 0)
        snprintf(rel, sizeof(rel), ", today");
    else if (diff == 1)
        snprintf(rel, sizeof(rel), ", tomorrow");
    else if (diff == -1)
        snprintf(rel, sizeof(rel), ", yesterday");
    else if (diff > 0 && diff < 10000)
        snprintf(rel, sizeof(rel), ", in %d days", (int)diff);
    else if (diff < 0 && diff > -10000)
        snprintf(rel, sizeof(rel), ", %d days ago", (int)-diff);
    snprintf(b, sizeof(b), "%.3s %d %.3s %d, week %d%s",
             time_weekday_name(time_weekday(y, m, d)), d, time_month_name(m), y,
             time_iso_week(y, m, d), rel);
    ctl_set_text(s->status, b);

    int i = note_find(s, s->sel);
    ctl_set_text(s->note, i >= 0 ? s->notes[i].text
                                 : "No note. Double-click to write one.");
    ctl_set_colors(s->note, i >= 0 ? BLACK : DARK_GRAY, 0xFF);
    wm_invalidate(s->win);
}

/* select a day; the shown month follows it */
static void select_day(cal_state_t *s, int32_t day) {
    int y, m, d;
    time_civil_from_days(day, &y, &m, &d);
    if (y < 1 || y > 9999)
        return;
    s->sel = day;
    s->y = y;
    s->m = m;
    update_text(s);
}

/* the same day `n` months away (31 Jan + 1 month = 28/29 Feb) */
static void move_months(cal_state_t *s, int n) {
    int y, m, d;
    time_civil_from_days(s->sel, &y, &m, &d);
    int mm = y * 12 + (m - 1) + n;
    y = mm / 12;
    m = mm % 12 + 1;
    if (y < 1 || y > 9999)
        return;
    int dim = time_days_in_month(m, y);
    select_day(s, time_days_from_civil(y, m, d > dim ? dim : d));
}

/* ── drawing ────────────────────────────────────────────────────────── */

static void paint(control *c, int x, int y, int w, int h) {
    cal_state_t *s = c->user;
    fill_rect(x, y, w, h, WHITE);
    int gx = x + (w - GRID_W) / 2, gy = y + 1;
    int f = UI_FONT_SMALL, fb = UI_FONT_BOLD;

    /* weekday names; the weekend in red */
    for (int col = 0; col < 7; col++) {
        int wd = s->sunday ? (col + 6) % 7 : col;
        const char *t = WD2[wd];
        draw_string(gx + col * CW + (CW - text_width(t, f)) / 2, gy + 1, t,
                    wd >= 5 ? RED : DARK_GRAY, f);
    }
    draw_line(gx, gy + HEAD - 2, gx + GRID_W - 1, gy + HEAD - 2, LIGHT_GRAY);

    int32_t day = grid_first(s);
    for (int row = 0; row < 6; row++)
        for (int col = 0; col < 7; col++, day++) {
            int cx = gx + col * CW, cy = gy + HEAD + row * CH;
            int yy, mm, dd;
            time_civil_from_days(day, &yy, &mm, &dd);
            bool in = mm == s->m;
            int wd = s->sunday ? (col + 6) % 7 : col;
            uint8_t fg = !in ? LIGHT_GRAY : wd >= 5 ? RED : BLACK;
            if (day == s->sel) {
                fill_rect(cx + 1, cy, CW - 2, CH - 1, YELLOW);
                fg = in ? (wd >= 5 ? RED : BLACK) : DARK_GRAY;
            }
            if (day == s->today) {
                fill_rect(cx + 2, cy + 1, CW - 4, CH - 3, BLUE);
                fg = WHITE;
            }
            char n[3];
            snprintf(n, sizeof(n), "%d", dd);
            int ff = day == s->today ? fb : f;
            draw_string(cx + (CW - text_width(n, ff)) / 2,
                        cy + (CH - font_height(ff)) / 2, n, fg, ff);
            if (note_find(s, day) >= 0) /* a note: corner mark */
                fill_rect(cx + CW - 5, cy + 1, 3, 3,
                          day == s->today ? YELLOW : GREEN);
            if (day == s->sel) /* frame, visible over today too */
                draw_rect(cx + 1, cy, CW - 2, CH - 1, BLACK);
        }
}

/* ── notes UI ───────────────────────────────────────────────────────── */

static void note_done(void *user, const char *text) {
    cal_state_t *s = user;
    if (!text)
        return; /* cancelled */
    while (*text == ' ')
        text++;
    int i = note_find(s, s->sel);
    if (!*text) { /* empty: delete */
        if (i >= 0)
            s->notes[i] = s->notes[--s->nnotes];
    } else {
        if (i < 0) {
            if (s->nnotes >= MAX_NOTES) {
                dlg_message(s->win, "Calendar",
                            "The calendar is full: delete an old note.");
                return;
            }
            i = s->nnotes++;
            s->notes[i].day = s->sel;
        }
        strncpy(s->notes[i].text, text, NOTE_LEN - 1);
        s->notes[i].text[NOTE_LEN - 1] = 0;
    }
    if (!notes_save(s))
        dlg_message(s->win, "Calendar", "Could not save the notes.");
    update_text(s);
}

static void edit_note(cal_state_t *s) {
    int y, m, d;
    time_civil_from_days(s->sel, &y, &m, &d);
    char prompt[48];
    snprintf(prompt, sizeof(prompt), "Note for %d %s %d:", d,
             time_month_name(m), y);
    int i = note_find(s, s->sel);
    dlg_input(s->win, "Note", prompt, i >= 0 ? s->notes[i].text : "",
              NOTE_LEN - 1, 0, note_done, s);
}

static void delete_note(cal_state_t *s) {
    if (note_find(s, s->sel) >= 0)
        note_done(s, "");
}

/* ── input ──────────────────────────────────────────────────────────── */

static void on_grid(control *c, int ev) {
    cal_state_t *s = c->user;
    if (ev == EV_MOUSEDOWN || ev == EV_DBLCLICK) {
        int gx = (c->aw - GRID_W) / 2, gy = 1 + HEAD;
        int col = (c->mx - gx) / CW, row = (c->my - gy) / CH;
        if (c->mx < gx || c->my < gy || col > 6 || row > 5)
            return;
        select_day(s, grid_first(s) + row * 7 + col);
        if (ev == EV_DBLCLICK)
            edit_note(s);
    } else if (ev == EV_KEY) {
        switch (kb.last_scancode) {
        case LEFT_ARROW:
            select_day(s, s->sel - 1);
            break;
        case RIGHT_ARROW:
            select_day(s, s->sel + 1);
            break;
        case UP_ARROW:
            select_day(s, s->sel - 7);
            break;
        case DOWN_ARROW:
            select_day(s, s->sel + 7);
            break;
        case PAGE_UP:
            move_months(s, kb.ctrl ? -12 : -1);
            break;
        case PAGE_DOWN:
            move_months(s, kb.ctrl ? 12 : 1);
            break;
        case HOME:
            select_day(s, today_days());
            break;
        case DELETE:
            delete_note(s);
            break;
        case ENTER:
            edit_note(s);
            break;
        }
    }
}

static void on_nav(control *c, int ev) {
    if (ev == EV_CLICK)
        move_months(c->user, c->id);
}

/* ── menus ──────────────────────────────────────────────────────────── */

static cal_state_t *active(void) { return ui_active_state(); }

static void m_today(void) {
    cal_state_t *s = active();
    if (s)
        select_day(s, today_days());
}

static void goto_done(void *user, const char *text) {
    cal_state_t *s = user;
    if (!text)
        return;
    /* "2026-11-16", "16.11.2026", "11 2026", "2026" */
    int v[3] = {-1, -1, -1}, n = 0;
    char sep = 0;
    for (const char *p = text; *p && n < 3;) {
        if (*p >= '0' && *p <= '9') {
            int x = 0;
            while (*p >= '0' && *p <= '9')
                x = x * 10 + (*p++ - '0');
            v[n++] = x;
        } else {
            if (!sep && *p != ' ')
                sep = *p;
            p++;
        }
    }
    int y = -1, m = 1, d = 1;
    if (n == 3 && sep == '-')
        y = v[0], m = v[1], d = v[2];
    else if (n == 3)
        d = v[0], m = v[1], y = v[2];
    else if (n == 2)
        m = v[0], y = v[1];
    else if (n == 1)
        y = v[0];
    if (!time_date_valid(y, m, d)) {
        dlg_message(s->win, "Go To",
                    "Try 2026-11-16, 16.11.2026, 11 2026 "
                    "or 2026 (years 1 to 9999).");
        return;
    }
    select_day(s, time_days_from_civil(y, m, d));
}

static void m_goto(void) {
    cal_state_t *s = active();
    if (s)
        dlg_input(s->win, "Go To", "Date, month and year, or a year:", "", 16,
                  0, goto_done, s);
}

static void m_note(void) {
    cal_state_t *s = active();
    if (s)
        edit_note(s);
}

static void m_delete(void) {
    cal_state_t *s = active();
    if (s)
        delete_note(s);
}

static void set_week_start(bool sunday) {
    cal_state_t *s = active();
    if (!s)
        return;
    s->sunday = sunday;
    app_prefs_set_int("CALENDAR", "sunday", sunday);
    update_text(s);
}
static void m_monday(void) { set_week_start(false); }
static void m_sunday(void) { set_week_start(true); }

static void m_close(void) { os_quit_app_by_desc(&calendar_app); }

static void m_about(void) {
    dlg_message(NULL, "About Calendar",
                "Calendar 1.0\nDouble-click a day to write a note.\n"
                "Arrows, PgUp/PgDn (Ctrl: a year)");
}

/* ── life cycle ─────────────────────────────────────────────────────── */

static void cal_init(void *state) {
    cal_state_t *s = state;
    int cw = GRID_W + 12;
    int ch = 18 + GRID_H + 3 + 12 + 16;
    s->win = app_window(s, "Calendar", 50, 20, cw + 2, ch + 17, DARK_GRAY, 0);
    if (!s->win)
        return;
    s->win->lazy_custom = true; /* changes come from handlers and on_frame */

    control *b;
    b = ctl_button(s->win, 4, 3, 18, 13, "<<", 0, on_nav);
    b->id = -12;
    ctl_set_tooltip(b, "A year back");
    b = ctl_button(s->win, 24, 3, 14, 13, "<", 0, on_nav);
    b->id = -1;
    ctl_set_tooltip(b, "A month back (PgUp)");
    s->title = ctl_label(s->win, 40, 3, cw - 80, 13, "", LS_CENTER | LS_BOLD);
    b = ctl_button(s->win, cw - 38, 3, 14, 13, ">", 0, on_nav);
    b->id = 1;
    ctl_set_tooltip(b, "A month on (PgDn)");
    b = ctl_button(s->win, cw - 22, 3, 18, 13, ">>", 0, on_nav);
    b->id = 12;
    ctl_set_tooltip(b, "A year on");

    s->grid = ctl_custom(s->win, 4, 19, cw - 8, GRID_H + 2, paint, on_grid,
                         CS_FOCUSABLE | CS_BORDER);
    s->note = ctl_label(s->win, 4, 19 + GRID_H + 4, cw - 8, 10, "", 0);
    s->status = ctl_status(s->win, "");

    menu *f = window_add_menu(s->win, "Calendar");
    menu_add_item(f, "Today", m_today);
    menu_add_item(f, "Go To Date...", m_goto);
    menu_add_separator(f);
    menu_add_item(f, "Edit Note...  Enter", m_note);
    menu_add_item(f, "Delete Note  Del", m_delete);
    menu_add_separator(f);
    menu_add_item(f, "About Calendar", m_about);
    menu_add_item(f, "Close", m_close);
    menu *v = window_add_menu(s->win, "View");
    menu_add_item(v, "Week Starts on Monday", m_monday);
    menu_add_item(v, "Week Starts on Sunday", m_sunday);

    s->sunday = app_prefs_get_int("CALENDAR", "sunday", 0) != 0;
    notes_load(s);
    s->today = today_days();
    select_day(s, s->today);
    ctl_focus(s->grid);
}

/* at midnight "today" moves */
static void cal_frame(void *state) {
    cal_state_t *s = state;
    if (!s->win)
        return;
    static uint32_t last;
    if (pit_ticks - last < 1000)
        return;
    last = pit_ticks;
    int32_t t = today_days();
    if (t != s->today) {
        s->today = t;
        update_text(s);
    }
}

app_descriptor calendar_app = {
    .name = "CALENDAR",
    .state_size = sizeof(cal_state_t),
    .init = cal_init,
    .on_frame = cal_frame,
    .single_instance = true,
};
