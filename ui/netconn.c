// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "ui/netconn.h"
#include "fs/netfs.h"
#include "lib/string.h"
#include "network/net.h"
#include "os/prefs.h"
#include "os/watchdog.h"
#include "ui/dialogs.h"

typedef struct {
    window *win;
    control *host, *port, *user, *pass, *dir, *status;
    void (*done)(void);
} nc_t;

static void nc_cancel(control *b, int ev) {
    (void)ev;
    dlg_form_close(((nc_t *)b->user)->win);
}

static void nc_ok(control *b, int ev) {
    (void)ev;
    nc_t *c = b->user;
    const char *host = ctl_text(c->host);
    uint32_t port = (uint32_t)str_to_int(ctl_text(c->port));
    if (!*host) {
        ctl_set_text(c->status, "Which server?");
        ctl_focus(c->host);
        return;
    }
    if (!port || port > 65535) {
        ctl_set_text(c->status, "The port is 1..65535 (FTP: 21).");
        ctl_focus(c->port);
        return;
    }
    ctl_set_text(c->status, "Connecting...");
    wd_busy(); /* (it waits for the server) */
    const char *err;
    if (!netfs_connect(host, (uint16_t)port, ctl_text(c->user),
                       ctl_text(c->pass), ctl_text(c->dir), &err)) {
        ctl_set_text(c->status, err);
        return;
    }
    app_prefs_begin("NET0");
    app_prefs_set("NET0", "host", host);
    app_prefs_set_int("NET0", "port", (int)port);
    app_prefs_set("NET0", "user", ctl_text(c->user));
    app_prefs_set("NET0", "dir", ctl_text(c->dir));
    app_prefs_end();
    void (*done)(void) = c->done;
    dlg_form_close(c->win);
    if (done)
        done();
}

void netconn_dialog(window *owner, void (*done)(void)) {
    if (!net_ready()) {
        dlg_message(owner, "Connect to Server",
                    "There is no network, or no address yet.\n"
                    "See Settings > Network.");
        return;
    }
    window *w = dlg_form(owner, "Connect to Server", 236, 114, sizeof(nc_t));
    if (!w)
        return;
    nc_t *c = dlg_form_state(w);
    c->win = w;
    c->done = done;
    char host[64] = "", user[32] = "anonymous", dir[FS_PATH_SZ] = "";
    app_prefs_get("NET0", "host", host, sizeof(host));
    app_prefs_get("NET0", "user", user, sizeof(user));
    app_prefs_get("NET0", "dir", dir, sizeof(dir));
    char port[8];
    snprintf(port, sizeof(port), "%d", app_prefs_get_int("NET0", "port", 21));

    ctl_label(w, 8, 5, 220, 10, "An FTP server's folder, as drive NET0.", 0);
    ctl_label(w, 8, 21, 50, 10, "Server:", 0);
    c->host = ctl_edit(w, 62, 19, 102, 12, host, 63, 0, NULL);
    ctl_set_tooltip(c->host, "A name (ftp.example.com) or an address");
    ctl_label(w, 168, 21, 20, 10, "Port", 0);
    c->port = ctl_edit(w, 190, 19, 38, 12, port, 5, ES_NUMBER, NULL);
    ctl_label(w, 8, 36, 50, 10, "User:", 0);
    c->user = ctl_edit(w, 62, 34, 166, 12, user, 31, 0, NULL);
    ctl_label(w, 8, 51, 50, 10, "Password:", 0);
    c->pass = ctl_edit(w, 62, 49, 166, 12, "", 31, ES_PASSWORD, NULL);
    ctl_set_tooltip(c->pass, "Not kept. Anonymous servers take anything.");
    ctl_label(w, 8, 66, 50, 10, "Folder:", 0);
    c->dir = ctl_edit(w, 62, 64, 166, 12, dir, FS_PATH_MAX, 0, NULL);
    ctl_set_tooltip(c->dir, "Empty: where the server puts you");
    c->status = ctl_label(w, 8, 82, 220, 18, "", 0);
    control *ok = ctl_button(w, 118, 80, 54, 13, "Connect", 1, nc_ok);
    control *ca = ctl_button(w, 176, 80, 52, 13, "Cancel", 2, nc_cancel);
    ok->user = ca->user = c;
    ctl_style_add(ok, CS_DEFAULT);
    ctl_style_add(ca, CS_CANCEL);
    ctl_focus(*host ? c->pass : c->host);
}

typedef struct {
    void (*done)(void);
} dc_t;

static dc_t s_dc;

static void dc_yes(void *user, bool yes) {
    dc_t *d = user;
    if (!yes)
        return;
    netfs_disconnect();
    if (d->done)
        d->done();
}

void netconn_disconnect(window *owner, void (*done)(void)) {
    if (!netfs_connected()) {
        dlg_message(owner, "Disconnect", "No server is connected (NET0).");
        return;
    }
    char where[96], m[160];
    netfs_describe(where, sizeof(where));
    snprintf(m, sizeof(m), "Disconnect from\n%s?\n\nFiles open on NET0 close.",
             where);
    s_dc.done = done;
    dlg_confirm(owner, "Disconnect", m, dc_yes, &s_dc);
}
