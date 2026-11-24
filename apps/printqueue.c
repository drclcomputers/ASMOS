// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "config/runtime_config.h"
#include "os/api.h"
#include "os/print.h"

typedef struct {
    window *win; /* must stay first */
    control *list, *pause, *del, *status;
    uint32_t rev, tick;
} printq_state_t;

app_descriptor printq_app;

static void refresh(printq_state_t *s) {
    int keep = list_sel(s->list);
    list_clear(s->list);
    list_add(s->list, "Document\tPages\tStatus");
    for (int i = 0; i < print_job_count(); i++) {
        const print_job_t *j = print_job(i);
        char st[32], line[96];
        if (i == 0 && j->state == PJ_ERROR)
            snprintf(st, sizeof(st), "Stopped");
        else if (i == 0 && j->sent)
            snprintf(st, sizeof(st), "Printing %u%%",
                     (unsigned)(j->len > 40000000u ? j->sent / (j->len / 100)
                                                   : j->sent * 100 / j->len));
        else
            snprintf(st, sizeof(st), "Waiting");
        snprintf(line, sizeof(line), "%s\t%d\t%s", j->name, j->pages, st);
        list_add_data(s->list, line, (uint32_t)j->id);
    }
    int n = list_count(s->list) - 1; /* without the header */
    if (keep < 1 || keep > n)
        keep = n ? 1 : -1;
    list_set_sel(s->list, keep);
    ctl_enable(s->del, n > 0);
    ctl_set_text(s->pause, print_paused() ? "Resume" : "Pause");

    char b[96];
    snprintf(b, sizeof(b), "%s on %s: %s",
             PRN_TYPE_NAMES[g_cfg.printer_type % PRN_TYPES], print_port_name(),
             print_status_text());
    ctl_set_text(s->status, b);
    s->rev = print_queue_rev();
}

static void on_button(control *c, int ev) {
    printq_state_t *s = c->user;
    if (ev != EV_CLICK)
        return;
    if (c == s->pause) {
        print_set_paused(!print_paused());
    } else if (c == s->del) {
        int i = list_sel(s->list);
        if (i >= 1)
            print_delete((int)list_data(s->list, i));
    }
    refresh(s);
}

static void on_list(control *c, int ev) {
    if (ev == EV_KEY && kb.last_scancode == DELETE)
        on_button(((printq_state_t *)c->user)->del, EV_CLICK);
}

static void m_close(void) { os_quit_app_by_desc(&printq_app); }

static void m_about(void) {
    dlg_message(NULL, "About Print Queue",
                "Print Queue 1.0\nJobs print in the background while you "
                "work.\nPrinter and port: Settings > Printers.");
}

static void printq_init(void *state) {
    printq_state_t *s = state;
    s->win = app_window(s, "Print Queue", 40, 30, 240, 132, DARK_GRAY, 0);
    if (!s->win)
        return;
    window *w = s->win;
    s->list = ctl_list(w, 4, 4, 230, 84, LBS_HEADER, on_list);
    int cols[3] = {110, 36, 0};
    list_set_columns(s->list, cols, 3);
    s->pause = ctl_button(w, 4, 92, 56, 13, "Pause", 0, on_button);
    s->del = ctl_button(w, 64, 92, 56, 13, "Delete", 0, on_button);
    ctl_set_tooltip(s->pause, "Hold all printing; Resume goes on");
    ctl_set_tooltip(s->del, "Take the job out of the queue (Del)");
    s->status = ctl_status(w, "");
    menu *f = window_add_menu(w, "Queue");
    menu_add_item(f, "About Print Queue", m_about);
    menu_add_item(f, "Close", m_close);
    refresh(s);
    ctl_focus(s->list);
}

/* follow the queue: on change, and the printer's state once a second */
static void printq_frame(void *state) {
    printq_state_t *s = state;
    if (!s->win)
        return;
    if (print_queue_rev() != s->rev || pit_ticks - s->tick > 1000) {
        s->tick = pit_ticks;
        refresh(s);
    }
}

app_descriptor printq_app = {
    .name = "PRINTQ",
    .state_size = sizeof(printq_state_t),
    .init = printq_init,
    .on_frame = printq_frame,
    .single_instance = true,
};
