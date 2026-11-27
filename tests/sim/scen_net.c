/* the network: DHCP from the fake NIC's server, and TCP between a client
 * and a server in the OS over the "wire" (10.0.2.99 mirrors), with frames
 * lost on the way */
#include "fs/netfs.h"
#include "network/dhcp.h"
#include "network/dns.h"
#include "network/ftpd.h"
#include "network/net.h"
#include "os/clipboard.h"
#include "os/prefs.h"
#include "scen.h"
#include "ui/fileops.h"
#include "ui/popup.h"

extern int g_sim_hw, g_sim_dhcp, g_sim_dhcp_msgs[8], g_sim_drop, g_sim_dropped;
extern uint32_t g_sim_dhcp_lease;

static const uint8_t MIRROR[4] = {10, 0, 2, 99};

static void poll_ms(uint32_t ms) {
    uint32_t until = pit_ticks + ms;
    while (pit_ticks < until)
        net_poll();
}

static bool ip_is(const uint8_t *a, int b0, int b1, int b2, int b3) {
    return a[0] == b0 && a[1] == b1 && a[2] == b2 && a[3] == b3;
}

static void net_up(void) {
    static bool up;
    g_cfg.networking_enabled = 1;
    g_sim_hw = 1;
    if (!up)
        net_init();
    up = true;
}

static int dhcp_test(void) {
    int fails = 0;
    char b[300];
    net_up();
    dhcp_start();
    uint8_t ip[4], gw[4], nm[4], dns[4];
    net_get_config(ip, gw, nm);
    T("asking: no address meanwhile", ip_is(ip, 0, 0, 0, 0) && !net_ready());
    poll_ms(100);
    dhcp_info_t d;
    dhcp_info(&d);
    net_get_config(ip, gw, nm);
    dns_get_server(dns);
    T("DISCOVER, OFFER, REQUEST, ACK: bound", d.state == DHCP_BOUND &&
                                                  g_sim_dhcp_msgs[1] == 1 &&
                                                  g_sim_dhcp_msgs[3] == 1);
    T("the address, mask and gateway in use",
      ip_is(ip, 10, 0, 2, 15) && ip_is(nm, 255, 255, 255, 0) &&
          ip_is(gw, 10, 0, 2, 2) && net_ready());
    T("and the DNS server", ip_is(dns, 10, 0, 2, 3));
    T("the lease: an hour, from 10.0.2.2", d.lease_s == 3600 &&
                                               d.lease_left_s >= 3599 &&
                                               ip_is(d.server, 10, 0, 2, 2));
    T("ping works on it", icmp_ping(gw, 1, 1, 500));

    /* half the lease: renewed */
    pit_ticks += 1800 * 1000;
    poll_ms(50);
    dhcp_info(&d);
    T("at half the lease: renewed (REQUEST again)",
      d.state == DHCP_BOUND && g_sim_dhcp_msgs[3] == 2 &&
          d.lease_left_s >= 3599);

    /* the server goes away: renewing, rebinding, then it starts over */
    g_sim_dhcp = 0;
    pit_ticks += 1801 * 1000;
    poll_ms(50);
    T("no answer: renewing", dhcp_state() == DHCP_RENEWING);
    pit_ticks += 1400 * 1000;
    poll_ms(50);
    T("7/8 of it: rebinding (broadcast)", dhcp_state() == DHCP_REBINDING);
    net_get_config(ip, gw, nm);
    T("the address still in use meanwhile", ip_is(ip, 10, 0, 2, 15));
    pit_ticks += 600 * 1000;
    poll_ms(50);
    net_get_config(ip, gw, nm);
    T("the lease ran out: searching, no address",
      dhcp_state() == DHCP_SEARCHING && ip_is(ip, 0, 0, 0, 0));
    int before = g_sim_dhcp_msgs[1];
    for (int k = 0; k < 4; k++)
        poll_ms(16100);
    T("asks again: after 2, 4, 8, 16, 30 s", g_sim_dhcp_msgs[1] - before == 5);
    g_sim_dhcp = 1;
    poll_ms(31000);
    T("the server back: bound again", dhcp_state() == DHCP_BOUND);

    /* the shell */
    static char o[1024];
    o[0] = 0;
    cmd_dhcp("", o, sizeof o);
    wr(1, o);
    T("dhcp: says bound, the address",
      strstr(o, "bound") && strstr(o, "10.0.2.15"));
    o[0] = 0;
    cmd_netconf("", o, sizeof o);
    wr(1, o);
    T("netconf: the address and DHCP",
      strstr(o, "IP 10.0.2.15") && strstr(o, "(DHCP)"));
    o[0] = 0;
    cmd_dhcp("release", o, sizeof o);
    net_get_config(ip, gw, nm);
    T("release: RELEASE sent, no address", g_sim_dhcp_msgs[7] == 1 &&
                                               ip_is(ip, 0, 0, 0, 0) &&
                                               dhcp_state() == DHCP_OFF);
    o[0] = 0;
    cmd_dhcp("renew", o, sizeof o);
    net_get_config(ip, gw, nm);
    T("renew: an address again",
      dhcp_state() == DHCP_BOUND && ip_is(ip, 10, 0, 2, 15));
    g_sim_dhcp_lease = 120;
    dhcp_stop(false);
    return fails;
}

// TCP

static uint8_t pat(uint32_t i) { return (uint8_t)(i * 7 + (i >> 9)); }

/* n bytes from a to b, both read and written here: the bytes that came
 * right (a count) */
static uint32_t pump(int a, int b, uint32_t n, uint32_t *ms) {
    static uint8_t buf[3000];
    uint32_t sent = 0, got = 0, good = 0, t0 = pit_ticks;
    while (got < n && pit_ticks - t0 < 120000) {
        if (sent < n) {
            uint32_t k = n - sent < 1500 ? n - sent : 1500;
            for (uint32_t j = 0; j < k; j++)
                buf[j] = pat(sent + j);
            int w = tcp_write(a, buf, (uint16_t)k);
            if (w > 0)
                sent += (uint32_t)w;
        }
        net_poll();
        int r = tcp_recv(b, buf, sizeof buf);
        for (int j = 0; j < r; j++, got++)
            good += buf[j] == pat(got);
        if (r < 0)
            break;
    }
    *ms = pit_ticks - t0;
    return good;
}

static int tcp_test(void) {
    int fails = 0;
    char b[300];
    net_up();
    static const uint8_t ip[4] = {10, 0, 2, 15}, gw[4] = {10, 0, 2, 2},
                         nm[4] = {255, 255, 255, 0};
    net_set_ip(ip, gw, nm);

    int none = tcp_connect(MIRROR, 2121, 0);
    T("nobody listening: refused at once (RST)", none < 0 && pit_ticks > 0);
    int l = tcp_listen(2121);
    T("listening on 2121", l >= 0 && tcp_state(l) == TCP_LISTEN);
    T("the port can't be taken twice", tcp_listen(2121) < 0);
    T("nothing to accept yet", tcp_accept(l) < 0);
    int c = tcp_connect(MIRROR, 2121, 0);
    poll_ms(5); /* (the last ACK of the handshake) */
    int s = tcp_accept(l);
    T("connected, and accepted", c >= 0 && s >= 0 &&
                                     tcp_state(c) == TCP_ESTABLISHED &&
                                     tcp_state(s) == TCP_ESTABLISHED);
    uint8_t pip[4];
    uint16_t pport = 0;
    tcp_peer(s, pip, &pport);
    T("the server sees the client's port",
      ip_is(pip, 10, 0, 2, 99) && pport == tcp_local_port(c));

    uint32_t ms;
    uint32_t good = pump(c, s, 200000, &ms);
    snprintf(b, sizeof b, "  (200 KB, no loss: %u ms)\n", ms);
    wr(1, b);
    T("200 KB client -> server, all of it right", good == 200000);
    g_sim_drop = 7;
    good = pump(s, c, 200000, &ms);
    snprintf(b, sizeof b, "  (200 KB, 1 frame in 7 lost: %u ms, %d lost)\n", ms,
             g_sim_dropped);
    wr(1, b);
    T("200 KB back, 1 in 7 frames lost: all of it right",
      good == 200000 && g_sim_dropped > 20);
    g_sim_drop = 3;
    good = pump(c, s, 50000, &ms);
    snprintf(b, sizeof b, "  (50 KB, 1 in 3 lost: %u ms)\n", ms);
    wr(1, b);
    T("50 KB with 1 in 3 lost: right too", good == 50000);
    g_sim_drop = 0;

    /* the window: the server doesn't read, the client fills it */
    static uint8_t blk[1024];
    uint32_t in = 0;
    for (int k = 0; k < 40; k++) {
        int w = tcp_write(c, blk, sizeof blk);
        in += w > 0 ? (uint32_t)w : 0;
        poll_ms(20);
    }
    T("not read: the sender stops at both buffers (16 KB)",
      in == TCP_RXBUF + TCP_TXBUF && tcp_available(s) == TCP_RXBUF);
    poll_ms(3000);
    T("a shut window: the connection waits, doesn't give up",
      tcp_state(c) == TCP_ESTABLISHED && !tcp_was_reset(c));
    static uint8_t sink[4096];
    uint32_t out = 0;
    for (int k = 0; k < 400 && out < in; k++) {
        int r = tcp_recv(s, sink, sizeof sink);
        out += r > 0 ? (uint32_t)r : 0;
        poll_ms(10);
    }
    T("read again: the rest comes (the window reopens)",
      out == in && tcp_pending(c) == 0);

    /* closing: the client first, data still on the way */
    for (uint32_t j = 0; j < sizeof blk; j++)
        blk[j] = pat(j);
    g_sim_drop = 4;
    tcp_write(c, blk, sizeof blk);
    tcp_release(c);
    uint32_t got = 0, ok = 0;
    for (int k = 0; k < 2000 && tcp_state(s) == TCP_ESTABLISHED; k++) {
        poll_ms(5);
        int r = tcp_recv(s, sink, sizeof sink);
        for (int j = 0; j < r; j++, got++)
            ok += sink[j] == pat(got);
    }
    int r = tcp_recv(s, sink, sizeof sink);
    for (int j = 0; j < r; j++, got++)
        ok += sink[j] == pat(got);
    T("released with data unsent: it arrives, then the FIN",
      got == sizeof blk && ok == got && tcp_state(s) == TCP_CLOSE_WAIT);
    tcp_close(s);
    g_sim_drop = 0;
    poll_ms(3000);
    int used = 0;
    for (int k = 0; k < NET_MAX_SOCKETS; k++)
        used += tcp_state(k) != TCP_CLOSED;
    T("both ends freed (only the listener left)", used == 1);

    /* the listener closed: new connections are refused */
    tcp_close(l);
    T("listener closed: connecting fails", tcp_connect(MIRROR, 2121, 0) < 0);

    /* many at once */
    l = tcp_listen(2121);
    int cs[4], ss[4], n = 0;
    for (int k = 0; k < 4; k++) {
        cs[k] = tcp_connect(MIRROR, 2121, 0);
        poll_ms(5);
        ss[k] = tcp_accept(l);
        n += cs[k] >= 0 && ss[k] >= 0;
    }
    T("four connections at once", n == 4);
    for (int k = 0; k < 4; k++) {
        char m[8] = "hello0";
        m[5] = (char)('0' + k);
        tcp_send(cs[k], (const uint8_t *)m, 6);
    }
    poll_ms(50);
    n = 0;
    for (int k = 0; k < 4; k++) {
        char m[8] = {0};
        tcp_recv(ss[k], (uint8_t *)m, 7);
        n += m[5] == '0' + k && !strncmp(m, "hello", 5);
    }
    T("each gets its own bytes", n == 4);
    for (int k = 0; k < 4; k++) {
        tcp_release(ss[k]);
        tcp_release(cs[k]);
    }
    tcp_release(l);
    poll_ms(4000);
    used = 0;
    for (int k = 0; k < NET_MAX_SOCKETS; k++)
        used += tcp_state(k) != TCP_CLOSED;
    T("all released: all freed", used == 0);
    return fails;
}

static control *find(window *w, ctl_type t, const char *text) {
    for (control *c = w ? w->controls : NULL; c; c = c->next)
        if (c->type == t && (!text || (c->text && strstr(c->text, text))))
            return c;
    return NULL;
}

/* Settings > Network: DHCP on (the fields show what it got), then off */
static int net_ui(void) {
    int fails = 0;
    char b[300];
    net_up();
    g_cfg.net_dhcp = 1;
    dhcp_start();
    poll_ms(50); /* (the sim's frames don't poll the network) */
    app_instance_t *a = sim_launch("Settings");
    sim_frames(3);
    window *w = wm_focused_window();
    control *tabs = find(w, CTL_TABS, NULL);
    ctl_set_value(tabs, 3);
    sim_frames(80);
    sim_shot("net0");
    control *dh = find(w, CTL_CHECK, "DHCP");
    control *st = find(w, CTL_LABEL, "From");
    control *ipf = find(w, CTL_EDIT, "10.0.2.15");
    T("DHCP ticked; the status says where from",
      a && dh && ctl_value(dh) && st);
    T("the address it got, greyed", ipf && !ipf->enabled);
    ctl_set_value(dh, 0);
    dh->handler(dh, EV_CHANGE);
    sim_frames(3);
    T("off: the fields can be typed in", ipf && ipf->enabled);
    ctl_set_text(ipf, "10.0.2.77");
    control *save = NULL;
    for (control *c = w->controls; c; c = c->next)
        if (c->type == CTL_BUTTON && c->text && !strcmp(c->text, "Save"))
            save = c;
    save->handler(save, EV_CLICK);
    sim_frames(70);
    sim_shot("net1");
    uint8_t ip[4], gw[4], nm[4];
    net_get_config(ip, gw, nm);
    T("saved: the typed address at once, DHCP stopped",
      ip_is(ip, 10, 0, 2, 77) && dhcp_state() == DHCP_OFF && !g_cfg.net_dhcp);
    T("the status: fixed", find(w, CTL_LABEL, "Fixed address.") != NULL);
    ctl_set_value(dh, 1);
    dh->handler(dh, EV_CHANGE);
    save->handler(save, EV_CLICK);
    poll_ms(50);
    sim_frames(70);
    net_get_config(ip, gw, nm);
    T("DHCP again: its address",
      ip_is(ip, 10, 0, 2, 15) && dhcp_state() == DHCP_BOUND);
    return fails;
}

// FTP: the server here, /NET0 mounted from it over the wire

static void put_file(const char *path, uint32_t n, uint32_t seed) {
    static uint8_t b[4096];
    fat_file_t f;
    fs_delete(path);
    fs_create(path, &f);
    for (uint32_t o = 0; o < n;) {
        uint32_t k = n - o < sizeof b ? n - o : sizeof b;
        for (uint32_t j = 0; j < k; j++)
            b[j] = pat(o + j + seed);
        fs_write(&f, b, (int)k);
        o += k;
    }
    fs_close(&f);
}

/* the file is n bytes of the pattern: -1 = no file, else the bad count */
static int check_file(const char *path, uint32_t n, uint32_t seed) {
    static uint8_t b[3000];
    fat_file_t f;
    if (!fs_open(path, &f))
        return -1;
    uint32_t o = 0;
    int bad = 0, r;
    while ((r = fs_read(&f, b, sizeof b)) > 0) {
        for (int j = 0; j < r; j++)
            bad += b[j] != pat(o + (uint32_t)j + seed);
        o += (uint32_t)r;
    }
    fs_close(&f);
    return bad + (o != n);
}

typedef struct {
    char names[16][FS_NAME_SZ];
    int n, dirs;
} names_t;

static bool name_cb(const dir_entry_t *e, const char *name, void *user) {
    names_t *t = user;
    if (t->n < 16)
        strcpy(t->names[t->n++], name);
    t->dirs += (e->attr & ATTR_DIRECTORY) != 0;
    return true;
}

static bool has(const names_t *t, const char *n) {
    for (int i = 0; i < t->n; i++)
        if (!strcmp(t->names[i], n))
            return true;
    return false;
}

static bool list_of(const char *path, names_t *t) {
    uint8_t d;
    uint16_t c;
    memset(t, 0, sizeof(*t));
    return fs_resolve_dir(path, &d, &c) && fs_walk_dir(d, c, name_cb, t);
}

static int ftp_test(void) {
    int fails = 0;
    char b[300];
    net_up();
    static const uint8_t ip[4] = {10, 0, 2, 15}, gw[4] = {10, 0, 2, 2},
                         nm[4] = {255, 255, 255, 0};
    net_set_ip(ip, gw, nm);
    fs_mkdir("/FTPT");
    fs_mkdir("/FTPT/SUB");
    put_file("/FTPT/HELLO.TXT", 300, 1);
    put_file("/FTPT/BIG.BIN", 150000, 2);
    put_file("/FTPT/Long Name File.txt", 5000, 3);
    put_file("/FTPT/SUB/A.TXT", 10, 4);

    const char *err = NULL;
    T("the server starts on 2121", ftpd_start(2121, false, "", &err));
    T("not twice", !ftpd_start(2121, false, "", &err) && err);
    bool up = netfs_connect("10.0.2.99", 2121, "", "", NULL, &err);
    if (!up) {
        wr(1, err);
        wr(1, "\n");
    }
    T("/NET0 mounted from it", up && fs_drive_mounted(DRIVE_NET0));
    names_t t;
    T("its top: the drives", list_of("/NET0", &t) && has(&t, "HDA") &&
                                 !has(&t, "NET0") && t.dirs == t.n);
    T("a folder over FTP: names, long ones too",
      list_of("/NET0/HDA/FTPT", &t) && t.n == 4 && has(&t, "HELLO.TXT") &&
          has(&t, "Long Name File.txt") && has(&t, "SUB"));
    dir_entry_t e;
    T("its size and kind", fs_find("/NET0/HDA/FTPT/BIG.BIN", &e) &&
                               e.file_size == 150000 &&
                               !(e.attr & ATTR_DIRECTORY));
    T("the 8.3 alias of a long name works",
      fs_find("/NET0/HDA/FTPT/LONGNA~1.TXT", &e) && e.file_size == 5000);
    T("a small file reads right",
      check_file("/NET0/HDA/FTPT/HELLO.TXT", 300, 1) == 0);
    T("a long-named one",
      check_file("/NET0/HDA/FTPT/Long Name File.txt", 5000, 3) == 0);
    uint32_t t0 = pit_ticks;
    T("a big one (streamed)",
      check_file("/NET0/HDA/FTPT/BIG.BIN", 150000, 2) == 0);
    snprintf(b, sizeof b, "  (150 KB down: %u ms)\n", pit_ticks - t0);
    wr(1, b);

    /* reading from the middle (REST), then on */
    fat_file_t f;
    static uint8_t buf[2000];
    bool ok = fs_open("/NET0/HDA/FTPT/BIG.BIN", &f) && fs_seek(&f, 100000) &&
              fs_read(&f, buf, 1000) == 1000;
    for (int j = 0; ok && j < 1000; j++)
        ok = buf[j] == pat(100000 + (uint32_t)j + 2);
    ok = ok && fs_seek(&f, 5) && fs_read(&f, buf, 10) == 10 &&
         buf[0] == pat(5 + 2);
    fs_close(&f);
    T("seek to the middle, and back", ok);

    /* uploads: copy onto /NET0, into a new folder, with frames lost */
    bool mk = fs_mkdir("/NET0/HDA/FTPT/New Folder");
    if (!mk) {
        wr(1, netfs_last_error());
        wr(1, "\n");
    }
    T("a folder made over FTP",
      mk && fs_resolve_dir("/FTPT/New Folder", &(uint8_t){0}, &(uint16_t){0}));
    g_sim_drop = 5;
    t0 = pit_ticks;
    T("a file copied up (1 frame in 5 lost)",
      fs_copy_file("/FTPT/BIG.BIN", "/NET0/HDA/FTPT/New Folder/up.bin") &&
          check_file("/FTPT/New Folder/up.bin", 150000, 2) == 0);
    snprintf(b, sizeof b, "  (150 KB up: %u ms, %d frames lost)\n",
             pit_ticks - t0, g_sim_dropped);
    wr(1, b);
    T("and down again",
      fs_copy_file("/NET0/HDA/FTPT/New Folder/up.bin", "/DOWN.BIN") &&
          check_file("/DOWN.BIN", 150000, 2) == 0);
    g_sim_drop = 0;
    fs_delete("/DOWN.BIN");

    /* written in pieces, through fs_create / fs_write */
    ok = fs_create("/NET0/HDA/FTPT/MADE.TXT", &f);
    for (int k = 0; ok && k < 4; k++)
        ok = fs_write(&f, "line of text\n", 13) == 13;
    ok = fs_close(&f) && ok;
    T("written in pieces: there, 52 bytes",
      ok && fs_find("/FTPT/MADE.TXT", &e) && e.file_size == 52);
    T("seen in the listing at once",
      fs_find("/NET0/HDA/FTPT/MADE.TXT", &e) && e.file_size == 52);

    /* rename, move, delete */
    T("renamed", fs_rename("/NET0/HDA/FTPT/MADE.TXT", "Renamed Notes.txt") &&
                     fs_find("/FTPT/Renamed Notes.txt", &e));
    T("moved into the folder",
      fs_move_file("/NET0/HDA/FTPT/Renamed Notes.txt",
                   "/NET0/HDA/FTPT/New Folder/notes.txt") &&
          fs_find("/FTPT/New Folder/notes.txt", &e));
    T("a whole folder copied down",
      fs_copy_dir("/NET0/HDA/FTPT/New Folder", "/COPYDN") &&
          check_file("/COPYDN/up.bin", 150000, 2) == 0 &&
          fs_find("/COPYDN/notes.txt", &e));
    T("and up", fs_copy_dir("/COPYDN", "/NET0/HDA/FTPT/UPDIR") &&
                    check_file("/FTPT/UPDIR/up.bin", 150000, 2) == 0);
    fs_rm_rf("/COPYDN");
    T("a file deleted", fs_delete("/NET0/HDA/FTPT/New Folder/notes.txt") &&
                            !fs_find("/FTPT/New Folder/notes.txt", &e));
    T("a folder with files deleted", fs_rm_rf("/NET0/HDA/FTPT/New Folder") &&
                                         !fs_find("/FTPT/New Folder", &e));
    T("a missing file: no",
      !fs_find("/NET0/HDA/FTPT/NOPE.TXT", &e) &&
          check_file("/NET0/HDA/FTPT/NOPE.TXT", 0, 0) == -1);

    /* the shell */
    static char o[1024];
    o[0] = 0;
    cmd_ftp("", o, sizeof o);
    wr(1, o);
    T("ftp: says what's on",
      strstr(o, "ftp://anonymous@10.0.2.99:2121/") && strstr(o, "Server: on"));
    o[0] = 0;
    cmd_ftp("umount", o, sizeof o);
    T("ftp umount: /NET0 gone",
      !fs_drive_mounted(DRIVE_NET0) &&
          !fs_resolve_dir("/NET0", &(uint8_t){0}, &(uint16_t){0}));
    o[0] = 0;
    cmd_ftp("mount 10.0.2.99 2121", o, sizeof o);
    wr(1, o);
    T("ftp mount: back",
      fs_drive_mounted(DRIVE_NET0) &&
          check_file("/NET0/HDA/FTPT/HELLO.TXT", 300, 1) == 0);
    netfs_disconnect();

    /* read only, and a password */
    ftpd_stop();
    T("stopped: nobody answers",
      !netfs_connect("10.0.2.99", 2121, "", "", NULL, &err));
    ftpd_start(2121, true, "secret", &err);
    T("the wrong password: no",
      !netfs_connect("10.0.2.99", 2121, "me", "nope", NULL, &err) &&
          strstr(err, "530"));
    T("the right one",
      netfs_connect("10.0.2.99", 2121, "me", "secret", "/HDA/FTPT", &err));
    T("mounted at the folder asked for",
      list_of("/NET0", &t) && has(&t, "HELLO.TXT"));
    T("read only: no writing", !fs_mkdir("/NET0/X") &&
                                   !fs_delete("/NET0/HELLO.TXT") &&
                                   strstr(netfs_last_error(), "550"));
    netfs_disconnect();
    ftpd_stop();
    poll_ms(3000);
    int used = 0;
    for (int k = 0; k < NET_MAX_SOCKETS; k++)
        used += tcp_state(k) != TCP_CLOSED;
    T("all of it closed", used == 0);
    fs_rm_rf("/FTPT");
    return fails;
}

/* a window's menu item, by its label */
static bool menu_do(window *w, const char *label) {
    for (int i = 0; w && i < w->win_menu_count; i++)
        for (int k = 0; k < w->win_menus[i].item_count; k++) {
            menu_item *it = &w->win_menus[i].items[k];
            if (it->label && !strcmp(it->label, label) && it->action) {
                it->action();
                return true;
            }
        }
    return false;
}

/* FileF: Drive > Connect to Server..., the window on /NET0, the desktop
 * icon, Disconnect */
static int ftp_ui(void) {
    int fails = 0;
    char b[300];
    net_up();
    static const uint8_t ip[4] = {10, 0, 2, 15}, gw[4] = {10, 0, 2, 2},
                         nm[4] = {255, 255, 255, 0};
    net_set_ip(ip, gw, nm);
    fs_mkdir("/SHARE");
    put_file("/SHARE/README.TXT", 600, 1);
    put_file("/SHARE/Holiday Photo.pic", 3000, 2);
    fs_mkdir("/SHARE/Music");
    const char *err;
    ftpd_start(2121, false, "", &err);
    sim_launch("FILEF");
    sim_frames(3);
    window *ff = wm_focused_window();
    T("FileF: Drive > Connect to Server...",
      menu_do(ff, "Connect to Server..."));
    sim_frames(3);
    window *dlg = wm_focused_window();
    T("the dialog",
      dlg && dlg->title && !strcmp(dlg->title, "Connect to Server"));
    control *host = NULL, *port = NULL, *dir = NULL, *ok = NULL;
    int edits = 0;
    for (control *c = dlg ? dlg->controls : NULL; c; c = c->next) {
        if (c->type == CTL_EDIT) {
            edits++;
            if (edits == 1)
                host = c;
            else if (edits == 2)
                port = c;
            else if (edits == 5)
                dir = c;
        }
        if (c->type == CTL_BUTTON && c->text && !strcmp(c->text, "Connect"))
            ok = c;
    }
    ctl_set_text(host, "10.0.2.99");
    ctl_set_text(port, "2121");
    ctl_set_text(dir, "/HDA/SHARE");
    sim_frames(2);
    sim_shot("ftp0");
    ok->handler(ok, EV_CLICK);
    sim_frames(5);
    T("connected: FileF on /NET0", ff->title && !strcmp(ff->title, "/NET0"));
    sim_shot("ftp1");
    bool icon = false;
    for (int i = 0; i < desktop_fs_count(); i++)
        icon |= desktop_fs_items()[i].drive_id == DRIVE_NET0;
    T("a desktop icon for it", icon);
    extern void ff_open_dir_pub(uint16_t, const char *);
    ff_open_dir_pub(0, "/");
    sim_frames(3);
    sim_shot("ftp2");
    char info[400];
    fo_info_text("/NET0", info, sizeof info);
    T("Get Info on it: the server", strstr(info, "ftp://anonymous@10.0.2.99"));
    /* the details are kept for next time (not the password) */
    char h[64] = "";
    app_prefs_get("NET0", "host", h, sizeof h);
    T("the server is remembered", !strcmp(h, "10.0.2.99"));
    /* Disconnect: asks first */
    window *ff2 = wm_focused_window();
    T("Drive > Disconnect Server...", menu_do(ff2, "Disconnect Server..."));
    sim_frames(3);
    sim_shot("ftp3");
    sim_key(0x1C); /* Enter: yes */
    sim_frames(5);
    icon = false;
    for (int i = 0; i < desktop_fs_count(); i++)
        icon |= desktop_fs_items()[i].drive_id == DRIVE_NET0;
    T("gone, and its icon", !fs_drive_mounted(DRIVE_NET0) && !icon);
    T("the window on it: back at Drives",
      ff->title && !strcmp(ff->title, "Drives"));
    /* the server off: the dialog says why */
    ftpd_stop();
    menu_do(ff2, "Connect to Server...");
    sim_frames(3);
    dlg = wm_focused_window();
    for (control *c = dlg ? dlg->controls : NULL; c; c = c->next)
        if (c->type == CTL_BUTTON && c->text && !strcmp(c->text, "Connect"))
            c->handler(c, EV_CLICK);
    sim_frames(3);
    sim_shot("ftp4");
    bool said = false;
    for (control *c = dlg ? dlg->controls : NULL; c; c = c->next)
        said |= c->type == CTL_LABEL && c->text &&
                strstr(c->text, "doesn't answer");
    T("no server: the dialog says so, stays",
      said && wm_focused_window() == dlg);
    snprintf(b, sizeof b, "  (%s)\n", netfs_last_error());
    wr(1, b);
    fs_rm_rf("/SHARE");
    return fails;
}

// World View on a FrogFind of its own: the sites' names all lead to a
// server here (the sim's DNS says 10.0.2.99, the mirror), which answers
// like FrogFind, with pictures from fixtures/web

#define WV_SIM
#include "extapps/worldview/wv.h"

extern int g_sim_dns_queries;

static bool run_menu(window *w, const char *prefix) {
    for (int m = 0; w && m < w->win_menu_count; m++)
        for (int k = 0; k < w->win_menus[m].item_count; k++) {
            menu_item *it = &w->win_menus[m].items[k];
            if (it->label && !strncmp(it->label, prefix, strlen(prefix))) {
                menu_item_run(it);
                return true;
            }
        }
    return false;
}

typedef struct {
    int sock;
    char req[4096];
    int req_len;
    char *out;
    int out_len, out_sent;
    bool answered;
    bool slow; /* (64 bytes a frame: a download to stop) */
} web_conn_t;
static web_conn_t s_web[6];
static int s_web_listen = -1, s_web_requests;
static char s_web_last[600]; /* the last request line, and a POST's body */
static char s_web_body[600];

static void web_put(web_conn_t *c, const char *type, const void *body, int n,
                    int status, const char *extra) {
    c->out = (char *)kmalloc((uint32_t)n + 400);
    if (!c->out)
        return;
    int h = snprintf(c->out, 400,
                     "HTTP/1.0 %d %s\r\nContent-Type: %s\r\n%sContent-Length: "
                     "%d\r\n\r\n",
                     status, status == 200 ? "OK" : "Moved", type, extra, n);
    memcpy(c->out + h, body, (uint32_t)n);
    c->out_len = h + n;
}

static void web_html(web_conn_t *c, const char *html) {
    web_put(c, "text/html; charset=utf-8", html, (int)strlen(html), 200, "");
}

static void web_file(web_conn_t *c, const char *name, const char *type) {
    static uint8_t b[16384];
    uint32_t n = 0;
    char path[64];
    snprintf(path, sizeof path, FIXDIR "web/%s", name);
    if (!load_file(path, b, sizeof b, &n))
        n = 0;
    web_put(c, type, b, (int)n, 200, "");
}

static const char *ARTICLE =
    "<html><head><title>The Story</title></head><body>"
    "<p><font size=\"4\"><a href=\"/?q=\">Back to <b><font "
    "color=\"#008000\">Frog</font>Find!</b></a></font></p><hr>"
    "<h3>The Story of Old Computers</h3>"
    "<p>A caf\xc3\xa9 in Bra\xc8\x99ov &amp; \xe2\x80\x9cquotes\xe2\x80\x9d "
    "&mdash; and &eacute;t&eacute;. Lorem ipsum dolor sit amet, consectetur "
    "adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore "
    "magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation.</p>"
    "<img src=\"/photo.jpg\"><br>"
    "<img src=\"/big.jpg\" width=\"100\" height=\"75\"> "
    "<img src=\"/pic.bmp\"> <img src=\"/prog.jpg\" alt=\"[prog]\">"
    "<ul><li>One<li>Two<ul><li>Two and a half</ul><li>Three</ul>"
    "<ol><li>First</li><li>Second</li></ol>"
    "<pre>  code\tlines\n  kept</pre>"
    "<blockquote>Quoted words, set in.</blockquote>"
    "<p align=\"center\">Centred</p>"
    "<p>More text to make the page longer than the window. More text to make "
    "the page longer than the window. More text to make the page longer than "
    "the window.</p><p>End of the story. <a href=\"/old\">An old link</a></p>"
    "</body></html>";

static void web_answer(web_conn_t *c) {
    s_web_requests++;
    char line[600];
    int i = 0;
    while (c->req[i] && c->req[i] != '\r' && i < (int)sizeof line - 1)
        line[i] = c->req[i], i++;
    line[i] = 0;
    strlcpy(s_web_last, line, sizeof s_web_last);
    char *body = strstr(c->req, "\r\n\r\n");
    strlcpy(s_web_body, body ? body + 4 : "", sizeof s_web_body);
    /* the path: "GET /x HTTP/1.0", or through a proxy "GET http://h/x" */
    char path[512];
    const char *p = strchr(line, ' ');
    p = p ? p + 1 : "/";
    if (!strncmp(p, "http://", 7) || !strncmp(p, "https://", 8)) {
        char page[700], want[500];
        strlcpy(want, p, sizeof want);
        char *sp = strchr(want, ' ');
        if (sp)
            *sp = 0;
        snprintf(page, sizeof page,
                 "<title>Proxied</title><p>The proxy was asked for %s", want);
        web_html(c, page);
        return;
    }
    i = 0;
    while (p[i] && p[i] != ' ' && i < (int)sizeof path - 1)
        path[i] = p[i], i++;
    path[i] = 0;
    if (!strcmp(path, "/")) {
        web_html(c, "<!DOCTYPE html><html lang=\"en\"><head><meta "
                    "charset=\"UTF-8\"><title>FrogFind!</title></head><body>"
                    "<br><br><center><h1><font size=7><font color=\"#008000\">"
                    "Frog</font>Find!</font></h1></center><center><h3>The "
                    "Search Engine for Vintage Computers</h3></center><br><br>"
                    "<center><form action=\"/\" method=\"get\">Leap to: <input "
                    "type=\"text\" size=\"30\" name=\"q\"><br><input "
                    "type=\"submit\" value=\"Ribbbit!\"></center><br><small>"
                    "<center>Built by <b><a href=\"https://youtube.com/"
                    "ActionRetro\" target=\"_blank\">Action Retro</a></b> on "
                    "YouTube | <a href=\"about.php\">Why build such a thing?"
                    "</a></center></small></form>"
                    "<center><img src=\"/logo.gif\" alt=\"logo\"></center>"
                    "</body></html>");
    } else if (!strncmp(path, "/?q=", 4) ||
               !strncmp(path, "/index.php?q=", 13)) {
        char page[2048];
        snprintf(page, sizeof page,
                 "<html><head><title>FrogFind!</title></head><body><p>Search "
                 "results for: <b>%s</b></p><hr>"
                 "<a href=\"/read.php?a=http://www.example.com/story\">The "
                 "Story</a><br><font size=\"2\" color=\"#008000\">"
                 "www.example.com/story</font><br>A story about old "
                 "computers.<br><br><a href=\"/read.php?a=http://b.example/"
                 "\">Another</a><br>Second result.<br><br></body></html>",
                 path + 4);
        web_html(c, page);
    } else if (!strncmp(path, "/read.php?a=", 12)) {
        web_html(c, ARTICLE);
    } else if (!strcmp(path, "/old")) {
        web_put(c, "text/html", "moved", 5, 302,
                "Location: /read.php?a=moved\r\n");
    } else if (!strcmp(path, "/chunked")) {
        const char *ch = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                         "Transfer-Encoding: chunked\r\n\r\n"
                         "7\r\n<title>\r\n"
                         "10\r\nChunks</title><h1\r\n"
                         "9\r\n>In parts\r\n"
                         "0\r\n\r\n";
        c->out = strdup(ch);
        c->out_len = (int)strlen(ch);
    } else if (!strcmp(path, "/form")) {
        web_html(c,
                 "<title>Form</title><form method=\"post\" action=\"/posted\">"
                 "Name: <input name=\"who\" value=\"Ana\"> <input "
                 "type=\"checkbox\" name=\"retro\" value=\"yes\" checked> "
                 "<input type=\"checkbox\" name=\"modern\"> <select "
                 "name=\"os\"><option>DOS<option selected value=\"asm\">"
                 "ASMOS<option>OS/2</select><input type=\"hidden\" "
                 "name=\"k\" value=\"a b&c\"><input type=\"submit\" "
                 "name=\"go\" value=\"Send\"></form>");
    } else if (!strcmp(path, "/posted")) {
        char page[800];
        snprintf(page, sizeof page, "<title>Posted</title><p>Got: %s",
                 s_web_body);
        web_html(c, page);
    } else if (!strcmp(path, "/logo.gif")) {
        web_file(c, "LOGO.GIF", "image/gif");
    } else if (!strcmp(path, "/photo.jpg")) {
        web_file(c, "PHOTO.JPG", "image/jpeg");
    } else if (!strcmp(path, "/big.jpg")) {
        web_file(c, "BIG.JPG", "image/jpeg");
    } else if (!strcmp(path, "/prog.jpg")) {
        web_file(c, "PROG.JPG", "image/jpeg");
    } else if (!strcmp(path, "/pic.bmp")) {
        web_file(c, "PIC.BMP", "image/bmp");
    } else if (!strcmp(path, "/files/")) { /* Apache's kind of listing */
        web_html(c,
                 "<html><head><title>Index of /files</title></head><body>"
                 "<h1>Index of /files</h1><table><tr><th><img "
                 "src=\"/icons/blank.gif\" alt=\"[ICO]\"></th><th><a "
                 "href=\"?C=N;O=D\">Name</a></th><th>Size</th></tr>"
                 "<tr><td><img src=\"/icons/back.gif\" alt=\"[PARENTDIR]\">"
                 "</td><td><a href=\"/\">Parent Directory</a></td><td>-</td>"
                 "</tr><tr><td><img src=\"/icons/text.gif\" alt=\"[TXT]\">"
                 "</td><td><a href=\"notes.txt\">notes.txt</a></td><td>1K"
                 "</td></tr><tr><td><img src=\"/icons/binary.gif\" "
                 "alt=\"[   ]\"></td><td><a href=\"big.bin\">big.bin</a></td>"
                 "<td>200K</td></tr><tr><td><img src=\"/icons/folder.gif\" "
                 "alt=\"[DIR]\"></td><td><a href=\"old/\">old/</a></td><td>-"
                 "</td></tr></table><address>Apache Server</address>"
                 "</body></html>");
    } else if (!strcmp(path, "/files/old/")) { /* nginx's kind */
        web_html(c, "<html><head><title>Index of /files/old/</title></head>"
                    "<body><h1>Index of /files/old/</h1><hr><pre><a "
                    "href=\"../\">../</a>\n<a href=\"game.zip\">game.zip"
                    "</a>          01-Jan-1995 10:00     11\n</pre><hr>"
                    "</body></html>");
    } else if (!strcmp(path, "/files/old/game.zip")) {
        web_put(c, "application/zip", "PK\x03\x04oldgame", 11, 200, "");
    } else if (!strcmp(path, "/files/notes.txt")) {
        web_put(c, "text/plain", "Some notes.\n", 12, 200, "");
    } else if (!strcmp(path, "/files/big.bin")) { /* 200 KB: a pattern */
        int n = 200 * 1024;
        c->out = (char *)kmalloc((uint32_t)n + 200);
        if (c->out) {
            int h = snprintf(c->out, 200,
                             "HTTP/1.0 200 OK\r\nContent-Type: application/"
                             "octet-stream\r\nContent-Length: %d\r\n\r\n",
                             n);
            for (int k = 0; k < n; k++)
                c->out[h + k] = (char)(k * 7 + k / 251);
            c->out_len = h + n;
        }
    } else if (!strcmp(path, "/frames")) { /* a frames page of 1998 */
        web_html(c, "<html><head><title>Old Site</title></head>"
                    "<frameset cols=\"150,*\"><frame name=\"menu\" "
                    "src=\"menu.htm\"><frameset rows=\"50,*\"><frame "
                    "src=\"top.htm\"><frame name=\"main\" title=\"Main "
                    "page\" src=\"main.htm\"></frameset><noframes><body>"
                    "Your browser doesn't support frames. Get Netscape!"
                    "</body></noframes></frameset></html>");
    } else if (!strcmp(path, "/main.htm")) {
        web_html(c, "<title>Main</title><p>The main frame's page.");
    } else if (!strcmp(path, "/iframe")) {
        web_html(c, "<title>Embed</title><p>Before <iframe src=\"/ad.htm\" "
                    "title=\"An ad\">no iframes here</iframe> after.");
    } else if (!strcmp(path, "/sneakyform")) { /* pressing our buttons */
        web_html(c, "<title>Sneaky</title><form action=\"about:bookmarks\">"
                    "<input type=hidden name=n value=0><input type=submit "
                    "name=do value=Remove></form>");
    } else if (!strcmp(path, "/slow.bin")) {
        int n = 100 * 1024;
        c->out = (char *)kmalloc((uint32_t)n + 200);
        if (c->out) {
            int h = snprintf(c->out, 200,
                             "HTTP/1.0 200 OK\r\nContent-Type: application/"
                             "octet-stream\r\nContent-Length: %d\r\n\r\n",
                             n);
            memset(c->out + h, 'x', (uint32_t)n);
            c->out_len = h + n;
            c->slow = true;
        }
    } else if (!strcmp(path, "/sneaky")) { /* reaching for the disk */
        web_html(c, "<title>Sneaky</title><img src=\"file:/WEB/LOGO.GIF\">"
                    "<a href=\"file:/WEB/PAGE.HTM\">a file</a>");
    } else if (!strcmp(path, "/tofile")) {
        web_put(c, "text/html", "", 0, 302, "Location: file:/WEB/PAGE.HTM\r\n");
    } else if (!strcmp(path, "/file.zip")) {
        web_put(c, "application/zip", "PK\x03\x04zipdata", 11, 200, "");
    } else {
        web_put(c, "text/html", "<title>Not Found</title>Not here.", 33, 404,
                "");
    }
}

/* the server's turn: new connections, requests read, answers sent */
static void web_step(void) {
    if (s_web_listen < 0)
        s_web_listen = tcp_listen(80);
    int ns = tcp_accept(s_web_listen);
    for (int k = 0; ns >= 0 && k < 6; k++)
        if (!s_web[k].sock) {
            memset(&s_web[k], 0, sizeof s_web[k]);
            s_web[k].sock = ns + 1;
            ns = -1;
        }
    for (int k = 0; k < 6; k++) {
        web_conn_t *c = &s_web[k];
        if (!c->sock)
            continue;
        int sock = c->sock - 1;
        if (!c->answered) {
            int n = tcp_recv(sock, (uint8_t *)c->req + c->req_len,
                             (uint16_t)(sizeof c->req - 1 - c->req_len));
            if (n > 0)
                c->req_len += n;
            c->req[c->req_len] = 0;
            char *end = strstr(c->req, "\r\n\r\n");
            if (end) {
                char *cl = strstr(c->req, "Content-Length: ");
                int want = cl ? (int)str_to_uint32(cl + 16) : 0;
                if (c->req_len - (int)(end + 4 - c->req) >= want) {
                    web_answer(c);
                    c->answered = true;
                }
            }
        }
        if (c->answered && c->out && c->out_sent < c->out_len) {
            int most = c->slow ? 64 : 1400;
            int n = tcp_write(sock, (const uint8_t *)c->out + c->out_sent,
                              (uint16_t)(c->out_len - c->out_sent > most
                                             ? most
                                             : c->out_len - c->out_sent));
            if (n > 0)
                c->out_sent += n;
        }
        if (c->answered && (!c->out || c->out_sent >= c->out_len)) {
            tcp_release(sock);
            kfree(c->out);
            memset(c, 0, sizeof *c);
        }
    }
}

// a gopher server (port 70): a menu, a text, a search, a picture, a file

static int s_go_listen = -1;
static web_conn_t s_go[3];
static char s_go_last[200]; /* the last selector asked for */

static void go_answer(web_conn_t *c) {
    char sel[200];
    int i = 0;
    while (c->req[i] && c->req[i] != '\r' && c->req[i] != '\n' &&
           i < (int)sizeof sel - 1)
        sel[i] = c->req[i], i++;
    sel[i] = 0;
    strlcpy(s_go_last, sel, sizeof s_go_last);
    const char *out;
    char buf[1024];
    if (!sel[0]) {
        out = "iWelcome to the Frog Hole\t\terror.host\t1\r\n"
              "i\t\terror.host\t1\r\n"
              "0About this server\t/about.txt\tgopher.example\t70\r\n"
              "1Files & things\t/files\tgopher.example\t70\r\n"
              "7Search the hole\t/find\tgopher.example\t70\r\n"
              "gThe frog\t/frog.gif\tgopher.example\t70\r\n"
              "9A program\t/prog.zip\tgopher.example\t70\r\n"
              "hFrogFind on the web\tURL:http://frogfind.com/\t"
              "gopher.example\t70\r\n"
              ".\r\n";
    } else if (!strcmp(sel, "/about.txt")) {
        out = "This is the Frog Hole.\r\nIt has <angles> & things.\r\n.\r\n";
    } else if (!strncmp(sel, "/find\t", 6)) {
        snprintf(buf, sizeof buf,
                 "iResults for %s\t\terror.host\t1\r\n"
                 "0A result\t/about.txt\tgopher.example\t70\r\n.\r\n",
                 sel + 6);
        out = buf;
    } else if (!strcmp(sel, "/frog.gif")) {
        static uint8_t b[16384];
        uint32_t n = 0;
        load_file(FIXDIR "web/LOGO.GIF", b, sizeof b, &n);
        c->out = (char *)kmalloc(n);
        if (c->out) {
            memcpy(c->out, b, n);
            c->out_len = (int)n;
        }
        return;
    } else if (!strcmp(sel, "/prog.zip")) {
        out = "PK\x03\x04gopherzip";
    } else {
        out = "3Not here\t\terror.host\t1\r\n.\r\n";
    }
    c->out = strdup(out);
    c->out_len = (int)strlen(out);
}

static void go_step(void) {
    if (s_go_listen < 0)
        s_go_listen = tcp_listen(70);
    int ns = tcp_accept(s_go_listen);
    for (int k = 0; ns >= 0 && k < 3; k++)
        if (!s_go[k].sock) {
            memset(&s_go[k], 0, sizeof s_go[k]);
            s_go[k].sock = ns + 1;
            ns = -1;
        }
    for (int k = 0; k < 3; k++) {
        web_conn_t *c = &s_go[k];
        if (!c->sock)
            continue;
        int sock = c->sock - 1;
        if (!c->answered) {
            int n = tcp_recv(sock, (uint8_t *)c->req + c->req_len,
                             (uint16_t)(sizeof c->req - 1 - c->req_len));
            if (n > 0)
                c->req_len += n;
            c->req[c->req_len] = 0;
            if (strchr(c->req, '\n')) {
                go_answer(c);
                c->answered = true;
            }
        }
        if (c->answered && c->out && c->out_sent < c->out_len) {
            int n = tcp_write(sock, (const uint8_t *)c->out + c->out_sent,
                              (uint16_t)(c->out_len - c->out_sent > 1400
                                             ? 1400
                                             : c->out_len - c->out_sent));
            if (n > 0)
                c->out_sent += n;
        }
        if (c->answered && (!c->out || c->out_sent >= c->out_len)) {
            tcp_release(sock);
            kfree(c->out);
            memset(c, 0, sizeof *c);
        }
    }
}

static void web_frames(int n) {
    for (int i = 0; i < n; i++) {
        web_step();
        go_step();
        if (ftpd_running())
            ftpd_poll();
        net_poll();
        sim_frame();
    }
}

static window *wv_window(void) {
    for (int i = 0; i < win_count; i++) {
        const char *t = win_stack[i]->title;
        int n = t ? (int)strlen(t) : 0;
        if (n >= 10 && !strcmp(t + n - 10, "World View"))
            return win_stack[i];
    }
    return NULL;
}

/* until the page and its pictures are in (or a time is up) */
static wv_state_t *wv_wait(void) {
    wv_state_t *s = NULL;
    for (int i = 0; i < 1500; i++) {
        web_frames(1);
        window *w = wv_window();
        s = w ? (wv_state_t *)w->userdata : NULL;
        if (s && !s->loading && s->pic_k < 0 && s->page.state != F_RECV)
            break;
    }
    web_frames(3);
    return s;
}

static bool page_has(const wv_state_t *s, const char *t) {
    return s && s->doc.pool &&
           memmem(s->doc.pool, s->doc.pool_len, t, strlen(t)) != NULL;
}

/* the item of a field, or of a link's text (first one) */
static const wv_item_t *wv_item(const wv_state_t *s, int kind, int ref,
                                int link) {
    for (int i = 0; s && i < s->doc.nitems; i++) {
        const wv_item_t *it = &s->doc.items[i];
        if (it->kind == kind && (ref < 0 || it->ref == ref) &&
            (link < 0 || it->link == link))
            return it;
    }
    return NULL;
}

/* the text item that starts with a word */
static const wv_item_t *wv_word(const wv_state_t *s, const char *word) {
    int n = (int)strlen(word);
    for (int i = 0; s && i < s->doc.nitems; i++) {
        const wv_item_t *it = &s->doc.items[i];
        if (it->kind == IT_TEXT && it->len >= n &&
            !memcmp(s->doc.pool + it->text, word, (size_t)n))
            return it;
    }
    return NULL;
}

/* click an item on the page (scrolled there first) */
static void wv_click(wv_state_t *s, const wv_item_t *it) {
    if (!it)
        return;
    if (it->y < s->scroll || it->y + it->h > s->scroll + s->view->ah - 4) {
        s->scroll = it->y - 8;
        s->scroll = s->scroll < 0 ? 0 : s->scroll;
        web_frames(1);
    }
    sim_click(s->view->ax + 2 + it->x + 2,
              s->view->ay + 2 + (int)(it->y - s->scroll) + it->h / 2);
    web_frames(2);
}

static int link_named(const wv_state_t *s, const char *url_part) {
    for (int k = 0; s && k < s->doc.nlinks; k++)
        if (strstr(doc_str(&s->doc, s->doc.links[k]), url_part))
            return k;
    return -1;
}

static void wv_address(wv_state_t *s, const char *url) {
    ctl_set_text(s->addr, url);
    ctl_focus(s->addr);
    sim_key(0x1C);
}

static int web_test(void) {
    int fails = 0;
    char b[300];
    net_up();
    static const uint8_t ip[4] = {10, 0, 2, 15}, gw[4] = {10, 0, 2, 2},
                         nm[4] = {255, 255, 255, 0}, dns[4] = {10, 0, 2, 3};
    net_set_ip(ip, gw, nm);
    dns_set_server(dns);
    app_prefs_set("WorldView", "Proxy", "");
    app_prefs_set("WorldView", "Home", "");

    /* the home page: FrogFind, by its name */
    sim_launch("World View");
    wv_state_t *s = wv_wait();
    window *w = wv_window();
    T("World View opens on FrogFind", s && !strcmp(s->url, HOME_URL));
    T("... found by name (DNS), fetched",
      g_sim_dns_queries > 0 && s_web_requests >= 1);
    snprintf(b, sizeof b, "    title: %s; %d items, %d links, %d fields\n",
             w ? w->title : "-", s ? s->doc.nitems : 0, s ? s->doc.nlinks : 0,
             s ? s->doc.nfields : 0);
    wr(1, b);
    T("its title in the window's",
      w && !strcmp(w->title, "FrogFind! - World View"));
    T("its words, its form", page_has(s, "Vintage Computers") &&
                                 s->doc.nforms == 1 && s->doc.nfields == 2);
    T("the logo: a GIF, shown", s->doc.nimages == 1 &&
                                    s->doc.images[0].state == IMG_DONE &&
                                    s->doc.images[0].w == 40);
    const wv_item_t *big = NULL;
    for (int i = 0; i < s->doc.nitems; i++)
        if (s->doc.items[i].scale == 2)
            big = &s->doc.items[i];
    T("the big heading: twice the size, centred",
      big && big->h >= 16 && big->x > 20);
    sim_shot("wv_home");

    /* searching: the field, typed in, Enter */
    wv_click(s, wv_item(s, IT_FIELD, 0, -1));
    T("a click in the field: typing goes there", s->focus == 0);
    sim_type("retro computers");
    sim_key(0x1C);
    s = wv_wait();
    snprintf(b, sizeof b, "    asked: %s\n", s_web_last);
    wr(1, b);
    T("Enter: the search sent (GET, the words encoded)",
      !strcmp(s_web_last, "GET /?q=retro+computers HTTP/1.0"));
    T("the results",
      page_has(s, "Search results for:") && page_has(s, "retro+computers"));
    sim_shot("wv_results");

    /* a result: the article, its pictures */
    int story = link_named(s, "read.php?a=http://www.example.com/story");
    T("the first result is a link", story >= 0);
    wv_click(s, wv_item(s, IT_TEXT, -1, story));
    s = wv_wait();
    T("clicked: the article", w && !strcmp(w->title, "The Story - World View"));
    /* (Windows-1252, the sim's code page: it has the curly quotes and the
     * dash; not the s with a comma below: a plain s) */
    T("UTF-8 and entities in the code page",
      page_has(s, "caf\xe9") && page_has(s, "\x93quotes\x94") &&
          page_has(s, "& \x93") && page_has(s, "\xe9t\xe9") &&
          page_has(s, "\x97") && page_has(s, "Brasov"));
    int ok = 0, failed = 0;
    for (int k = 0; k < s->doc.nimages; k++) {
        ok += s->doc.images[k].state == IMG_DONE;
        failed += s->doc.images[k].state == IMG_FAILED;
    }
    snprintf(b, sizeof b, "    pictures: %d shown, %d not (of %d)\n", ok,
             failed, s->doc.nimages);
    wr(1, b);
    T("JPEG, JPEG, BMP shown; a progressive JPEG not",
      s->doc.nimages == 4 && ok == 3 && failed == 1 &&
          s->doc.images[3].state == IMG_FAILED);
    T("the big JPEG at its width=, height=",
      s->doc.images[1].w == 100 && s->doc.images[1].h == 75);
    /* the photo on the screen: red on the left, blue on the right */
    const wv_item_t *ph = wv_item(s, IT_IMAGE, 0, -1);
    s->scroll = ph ? ph->y - 4 : 0;
    web_frames(2);
    uint8_t pal[256][3];
    gpu_palette(pal, 256);
    int red = 0, blue = 0;
    if (ph) {
        int x0 = s->view->ax + 2 + ph->x, y0 = s->view->ay + 2 + 4;
        for (int y = 0; y < 20; y++)
            for (int x = 0; x < 20; x++) {
                const uint8_t *l =
                    pal[BACKBUF[(y0 + y) * SCREEN_WIDTH + x0 + 4 + x]];
                const uint8_t *r =
                    pal[BACKBUF[(y0 + y) * SCREEN_WIDTH + x0 + 60 + x]];
                red += l[0] > 40 && l[2] < 30;
                blue += r[2] > 40 && r[0] < 30;
            }
    }
    snprintf(b, sizeof b, "    photo: %d red, %d blue of 400\n", red, blue);
    wr(1, b);
    T("the photo on the screen: red left, blue right", red > 300 && blue > 300);
    sim_shot("wv_article");
    T("lists: bullets and numbers", page_has(s, "1.") && page_has(s, "2."));
    int32_t before = s->scroll;
    ctl_focus(s->view);
    sim_key(0x51); /* Page Down */
    web_frames(1);
    T("Page Down scrolls", s->scroll > before);

    /* Back: the results again */
    sim_click(s->btn[0]->ax + 4, s->btn[0]->ay + 4);
    s = wv_wait();
    T("Back: the results", page_has(s, "Search results for:"));
    sim_click(s->btn[1]->ax + 4, s->btn[1]->ay + 4);
    s = wv_wait();
    T("Forward: the article", page_has(s, "Old Computers"));

    /* a redirect; chunks; a form sent by POST */
    wv_address(s, "http://frogfind.com/old");
    s = wv_wait();
    T("a redirect followed", strstr(s->url, "/read.php?a=moved") != NULL);
    wv_address(s, "frogfind.com/chunked");
    s = wv_wait();
    T("chunks put together",
      w && !strcmp(w->title, "Chunks - World View") && page_has(s, "In parts"));
    wv_address(s, "http://frogfind.com/form");
    s = wv_wait();
    T("a form: its fields", s->doc.nfields == 6);
    wv_click(s, wv_item(s, IT_FIELD, 0, -1));
    sim_type(" Bob");
    int send = -1;
    for (int k = 0; k < s->doc.nfields; k++)
        if (s->doc.fields[k].type == FLD_SUBMIT)
            send = k;
    wv_click(s, wv_item(s, IT_FIELD, send, -1));
    s = wv_wait();
    snprintf(b, sizeof b, "    posted: %s\n", s_web_body);
    wr(1, b);
    T("POST: the fields as they were set",
      !strcmp(s_web_body, "who=Ana+Bob&retro=yes&os=asm&k=a+b%26c&go=Send"));
    T("... the answer shown", page_has(s, "Got: who=Ana"));

    /* what can't be had */
    wv_address(s, "http://nowhere.invalid/");
    s = wv_wait();
    T("a name that doesn't exist: an error page",
      w && !strcmp(w->title, "Can't open the page - World View") &&
          page_has(s, "Could not find that site"));
    wv_address(s, "https://www.example.com/");
    s = wv_wait();
    T("https without a proxy: says a proxy is needed",
      page_has(s, "need a proxy"));

    /* through a proxy: the whole URL asked for, https too */
    run_menu(w, "Proxy...");
    web_frames(2);
    window *d = wm_focused_window();
    control *e = NULL;
    for (control *c = d ? d->controls : NULL; c; c = c->next)
        if (c->type == CTL_EDIT && !e)
            e = c;
    if (e) {
        ctl_set_text(e, "proxy.lan:80");
        ctl_focus(e);
    }
    sim_key(0x1C);
    web_frames(2);
    wv_address(s, "https://secure.example/page");
    s = wv_wait();
    snprintf(b, sizeof b, "    the proxy got: %s\n", s_web_last);
    wr(1, b);
    T("a proxy: asked for the whole https:// URL",
      !strcmp(s_web_last, "GET https://secure.example/page HTTP/1.0") &&
          page_has(s, "asked for https://secure.example/page"));
    run_menu(w, "Proxy...");
    web_frames(2);
    d = wm_focused_window();
    for (control *c = d ? d->controls : NULL; c; c = c->next)
        if (c->type == CTL_EDIT)
            ctl_set_text(c, "");
    sim_key(0x1C);
    web_frames(2);

    /* a file that isn't a page: straight into /DOWNLOAD, no questions */
    fs_delete("/DOWNLOAD/file.zip");
    wv_address(s, "http://frogfind.com/file.zip");
    web_frames(60);
    T("a file: no dialog", wm_focused_window() == w);
    {
        fat_file_t ff;
        char got[16] = {0};
        bool there = fs_open("/DOWNLOAD/file.zip", &ff);
        if (there) {
            fs_read(&ff, got, 11);
            fs_close(&ff);
        }
        snprintf(b, sizeof b, "    %s\n", s->line);
        wr(1, b);
        T("... saved in /DOWNLOAD, as it came",
          there && !memcmp(got, "PK\x03\x04zipdata", 11) &&
              strstr(s->line, "Saved /DOWNLOAD/file.zip"));
    }
    wv_address(s, "http://frogfind.com/file.zip");
    web_frames(60);
    {
        dir_entry_t de;
        T("... again: a name of its own",
          fs_find("/DOWNLOAD/file (2).zip", &de));
    }
    fs_delete("/DOWNLOAD/file (2).zip");

    /* a web page can't reach the disk (pictures, links, redirects) */
    fs_mkdir("/WEB");
    {
        static uint8_t g[16384];
        uint32_t n = 0;
        fat_file_t ff;
        load_file(FIXDIR "web/LOGO.GIF", g, sizeof g, &n);
        if (fs_create("/WEB/LOGO.GIF", &ff)) {
            fs_write(&ff, g, (int)n);
            fs_close(&ff);
        }
        const char *pg = "<title>On Disk</title><h1>Saved page</h1>"
                         "<img src=\"LOGO.GIF\"><p><a href=\"TWO.HTM\">two</a>";
        if (fs_create("/WEB/PAGE.HTM", &ff)) {
            fs_write(&ff, pg, (int)strlen(pg));
            fs_close(&ff);
        }
        pg = "<title>Two</title><p>Page two<p style=\"text-align:center\">"
             "Middle<div style=\"color: red; text-align: right\">Far</div>"
             "<p align=right>Right<center>Centred</center>";
        if (fs_create("/WEB/TWO.HTM", &ff)) {
            fs_write(&ff, pg, (int)strlen(pg));
            fs_close(&ff);
        }
    }
    wv_address(s, "http://frogfind.com/sneaky");
    s = wv_wait();
    T("a web page's picture from the disk: not loaded",
      s->doc.nimages == 1 && s->doc.images[0].state == IMG_FAILED);
    wv_click(s, wv_item(s, IT_TEXT, -1, 0));
    web_frames(5);
    T("... its link to a file: not followed",
      !strcmp(s->url, "http://frogfind.com/sneaky") &&
          strstr(s->line, "can't open files"));
    wv_address(s, "http://frogfind.com/tofile");
    s = wv_wait();
    T("... a redirect to a file: not followed", page_has(s, "not followed"));

    /* closed: its connections are gone */
    wm_close(w);
    web_frames(5);
    T("closed", !wv_window());

    /* a page on the disk: opened from FileF (.HTM), its picture and link
     * beside it */
    fo_open(NULL, "/WEB/PAGE.HTM");
    s = wv_wait();
    w = wv_window();
    T("a .HTM file opens in World View",
      w && !strcmp(w->title, "On Disk - World View") &&
          page_has(s, "Saved page") && !strcmp(s->url, "file:/WEB/PAGE.HTM"));
    T("... the picture beside it",
      s->doc.nimages == 1 && s->doc.images[0].state == IMG_DONE);
    wv_click(s, wv_item(s, IT_TEXT, -1, link_named(s, "TWO.HTM")));
    s = wv_wait();
    T("... a link to the page beside it", page_has(s, "Page two"));
    {
        const wv_item_t *mid = wv_word(s, "Middle"), *far = wv_word(s, "Far"),
                        *rt = wv_word(s, "Right"), *cn = wv_word(s, "Centred"),
                        *two = wv_word(s, "Page");
        int vw = s->doc.width;
        snprintf(b, sizeof b,
                 "    width %d: page %d, middle %d, far %d+%d, "
                 "right %d+%d, centred %d\n",
                 vw, two ? two->x : -1, mid ? mid->x : -1, far ? far->x : -1,
                 far ? far->w : 0, rt ? rt->x : -1, rt ? rt->w : 0,
                 cn ? cn->x : -1);
        wr(1, b);
        T("... style=\"text-align\": center and right",
          two && mid && far && two->x < 8 &&
              abs(mid->x + mid->w / 2 - vw / 2) < 8 &&
              far->x + far->w > vw - 8);
        T("... align=right, <center>", rt && cn && rt->x + rt->w > vw - 8 &&
                                           abs(cn->x + cn->w / 2 - vw / 2) < 8);
    }
    sim_click(s->btn[0]->ax + 4, s->btn[0]->ay + 4);
    s = wv_wait();
    T("... and Back", page_has(s, "Saved page"));
    /* another one, World View running: shown in it */
    {
        fat_file_t ff;
        const char *pg = "<title>Another</title><p>Another page";
        if (fs_create("/SAVED.HTM", &ff)) {
            fs_write(&ff, pg, (int)strlen(pg));
            fs_close(&ff);
        }
    }
    fo_open(NULL, "/SAVED.HTM");
    web_frames(2);
    s = wv_wait();
    T("a .HTM with World View open: shown in it",
      wv_window() == w && page_has(s, "Another page"));
    sim_click(s->btn[0]->ax + 4, s->btn[0]->ay + 4);
    s = wv_wait();
    T("... Back to the one before", page_has(s, "Saved page"));
    if (w)
        wm_close(w);
    web_frames(3);
    fs_rm_rf("/WEB");
    return fails;
}

// World View and file servers: HTTP listings, gopher, FTP (the OS's own
// server, through the mirror), downloads streamed to the disk, the
// right-click menu

/* the save dialog: a name typed, Enter */
static bool save_as(window *w, const char *name) {
    window *d = wm_focused_window();
    if (!d || d == w)
        return false;
    control *e = NULL;
    for (control *c = d->controls; c; c = c->next)
        if (c->type == CTL_EDIT && !e)
            e = c;
    if (!e)
        return false;
    ctl_set_text(e, name);
    ctl_focus(e);
    sim_key(0x1C);
    web_frames(2);
    return true;
}

/* until the download is done */
static void dl_wait(wv_state_t *s) {
    for (int i = 0; i < 3000 && s->dl_on; i++)
        web_frames(1);
}

static uint32_t file_size(const char *path) {
    fat_file_t ff;
    if (!fs_open(path, &ff))
        return 0;
    uint32_t n = ff.entry.file_size;
    fs_close(&ff);
    return n;
}

/* the open popup menu's item, by its first letter, then Enter */
static void popup_pick(char letter, uint8_t sc) {
    sim_key2(sc, letter, false, false);
    sim_key(0x1C);
    web_frames(2);
}

static int web_files_test(void) {
    int fails = 0;
    char b[300];
    net_up();
    static const uint8_t ip[4] = {10, 0, 2, 15}, gw[4] = {10, 0, 2, 2},
                         nm[4] = {255, 255, 255, 0}, dns[4] = {10, 0, 2, 3};
    net_set_ip(ip, gw, nm);
    dns_set_server(dns);
    app_prefs_set("WorldView", "Proxy", "");
    app_prefs_set("WorldView", "Home", "http://frogfind.com/files/");

    /* an HTTP folder (Apache's listing): links to its files */
    sim_launch("World View");
    wv_state_t *s = wv_wait();
    window *w = wv_window();
    T("an HTTP folder listing: its files are links",
      s && link_named(s, "/files/notes.txt") >= 0 &&
          link_named(s, "/files/big.bin") >= 0 &&
          link_named(s, "/files/old/") >= 0);
    sim_shot("wv_listing");
    int big = link_named(s, "/files/big.bin");

    /* a big file: into /DOWNLOAD as it comes */
    fs_rm_rf("/DOWNLOAD");
    wv_click(s, wv_item(s, IT_TEXT, -1, big));
    web_frames(6);
    T("a file (200 KB): downloading, named as it is",
      s->dl_on && !s->dl_asking && !strcmp(s->dl_name, "big.bin"));
    T("... the page stays", !s->loading && page_has(s, "Index of /files"));
    sim_move(1, 1); /* (off the link: the status line is the download's) */
    web_frames(3);
    snprintf(b, sizeof b, "    %s\n", s->line);
    wr(1, b);
    T("... the status line says how far",
      strstr(s->line, "Downloading big.bin") != NULL);
    dl_wait(s);
    snprintf(b, sizeof b, "    %s\n", s->line);
    wr(1, b);
    uint32_t got = file_size("/DOWNLOAD/big.bin");
    bool same = got == 200 * 1024;
    if (same) {
        static uint8_t chk[4096];
        fat_file_t ff;
        fs_open("/DOWNLOAD/big.bin", &ff);
        for (int k = 0; k < 200 * 1024 && same; k += 4096) {
            fs_read(&ff, chk, 4096);
            for (int j = 0; j < 4096; j++)
                same = same && chk[j] == (uint8_t)((k + j) * 7 + (k + j) / 251);
        }
        fs_close(&ff);
    }
    T("... saved whole (200 KB, more than a page can be), byte for byte",
      same && strstr(s->line, "Saved /DOWNLOAD/big.bin (200 KB)"));

    /* nginx's listing; a file in it */
    wv_address(s, "http://frogfind.com/files/old/");
    s = wv_wait();
    T("nginx's listing", link_named(s, "/files/old/game.zip") >= 0 &&
                             link_named(s, "/files/") >= 0);

    /* right-click: Save Link As (a text file, which would be shown) */
    fs_delete("/NOTES.TXT");
    wv_address(s, "http://frogfind.com/files/");
    s = wv_wait();
    const wv_item_t *it = wv_item(s, IT_TEXT, -1, link_named(s, "notes.txt"));
    if (it)
        sim_rclick(s->view->ax + 2 + it->x + 2,
                   s->view->ay + 2 + (int)(it->y - s->scroll) + 3);
    web_frames(2);
    T("a right-click on a link: its menu", popup_is_open());
    popup_pick('s', 0x1F);
    T("... Save Link As: where?", s->dl_on && s->dl_asking);
    T("... held (not read on) while it asks", s->dl.len <= DL_HOLD + 2048);
    save_as(w, "/NOTES.TXT");
    dl_wait(s);
    T("... the text saved, not shown",
      file_size("/NOTES.TXT") == 12 && page_has(s, "Index of /files"));
    it = wv_item(s, IT_TEXT, -1, link_named(s, "notes.txt"));
    if (it)
        sim_rclick(s->view->ax + 2 + it->x + 2,
                   s->view->ay + 2 + (int)(it->y - s->scroll) + 3);
    web_frames(2);
    popup_pick('c', 0x2E);
    {
        const char *clip = clipboard_text();
        T("... Copy Link Address",
          clip && !strcmp(clip, "http://frogfind.com/files/notes.txt"));
    }

    /* gopher: a menu as a page */
    wv_address(s, "gopher://gopher.example/");
    s = wv_wait();
    snprintf(b, sizeof b, "    %s; %d links\n", w ? w->title : "-",
             s->doc.nlinks);
    wr(1, b);
    T("gopher: the menu, a page", page_has(s, "Welcome to the Frog Hole") &&
                                      page_has(s, "Files & things") &&
                                      s->doc.nlinks == 6);
    T("... its links",
      link_named(s, "gopher://gopher.example/0/about.txt") >= 0 &&
          link_named(s, "gopher://gopher.example/1/files") >= 0 &&
          link_named(s, "http://frogfind.com/") >= 0);
    sim_shot("wv_gopher");
    wv_click(s, wv_item(s, IT_TEXT, -1, link_named(s, "/0/about.txt")));
    s = wv_wait();
    T("a text: shown as it is (no '.' line)",
      !strcmp(s_go_last, "/about.txt") && page_has(s, "<angles>") &&
          page_has(s, "& things") && s->doc.html_len > 7 &&
          !strncmp(s->doc.html + s->doc.html_len - 7, "things.", 7));
    sim_click(s->btn[0]->ax + 4, s->btn[0]->ay + 4);
    s = wv_wait();
    wv_click(s, wv_item(s, IT_TEXT, -1, link_named(s, "/7/find")));
    s = wv_wait();
    T("a search: a page asking what for", s->doc.nfields == 2);
    wv_click(s, wv_item(s, IT_FIELD, 0, -1));
    sim_type("green frogs");
    sim_key(0x1C);
    s = wv_wait();
    snprintf(b, sizeof b, "    asked: %s\n", s_go_last);
    wr(1, b);
    T("... sent with a tab, the results a menu",
      !strcmp(s_go_last, "/find\tgreen frogs") &&
          page_has(s, "Results for green frogs"));
    wv_address(s, "gopher://gopher.example/g/frog.gif");
    s = wv_wait();
    T("a picture", s->doc.nimages == 1 && s->doc.images[0].state == IMG_DONE);
    wv_address(s, "gopher://gopher.example/9/prog.zip");
    web_frames(20);
    T("a file (type 9): a download", !strcmp(s->dl_name, "prog.zip"));
    dl_wait(s);
    T("... saved", file_size("/DOWNLOAD/prog.zip") == 13);

    /* FTP: the OS's own server */
    fs_mkdir("/PUB");
    {
        fat_file_t ff;
        if (fs_create("/PUB/README.TXT", &ff)) {
            fs_write(&ff, "Read me first.\r\n", 16);
            fs_close(&ff);
        }
        static uint8_t d[60000];
        for (int k = 0; k < (int)sizeof d; k++)
            d[k] = (uint8_t)(k * 13);
        if (fs_create("/PUB/DATA.BIN", &ff)) {
            fs_write(&ff, d, sizeof d);
            fs_close(&ff);
        }
    }
    const char *err = NULL;
    T("the FTP server (21)", ftpd_start(21, true, "", &err));
    wv_address(s, "ftp://ftp.example/HDA/PUB");
    s = wv_wait();
    snprintf(b, sizeof b, "    %s: %s\n", s->url, s->line);
    wr(1, b);
    T("ftp: a folder (CWD worked), its URL ends in /",
      !strcmp(s->url, "ftp://ftp.example/HDA/PUB/"));
    T("... its files, links",
      link_named(s, "ftp://ftp.example/HDA/PUB/README.TXT") >= 0 &&
          link_named(s, "ftp://ftp.example/HDA/PUB/DATA.BIN") >= 0 &&
          page_has(s, "60000"));
    sim_shot("wv_ftp");
    wv_click(s, wv_item(s, IT_TEXT, -1, link_named(s, "README.TXT")));
    s = wv_wait();
    T("a text file: shown", page_has(s, "Read me first."));
    sim_click(s->btn[0]->ax + 4, s->btn[0]->ay + 4);
    s = wv_wait();
    wv_click(s, wv_item(s, IT_TEXT, -1, link_named(s, "DATA.BIN")));
    web_frames(5);
    T("a file: a download", !strcmp(s->dl_name, "DATA.BIN"));
    dl_wait(s);
    snprintf(b, sizeof b, "    %s\n", s->line);
    wr(1, b);
    {
        bool same = file_size("/DOWNLOAD/DATA.BIN") == 60000;
        static uint8_t d[60000];
        fat_file_t ff;
        if (same && fs_open("/DOWNLOAD/DATA.BIN", &ff)) {
            fs_read(&ff, d, sizeof d);
            fs_close(&ff);
            for (int k = 0; k < 60000 && same; k++)
                same = d[k] == (uint8_t)(k * 13);
        }
        T("... saved, byte for byte", same);
    }
    wv_address(s, "ftp://ftp.example/NOPE/");
    s = wv_wait();
    T("a folder that isn't there: says so", page_has(s, "No such folder"));
    wv_address(s, "ftp://ftp.example/");
    s = wv_wait();
    T("the top: the drives", link_named(s, "ftp://ftp.example/HDA/") >= 0);
    ftpd_stop();

    wm_close(w);
    web_frames(5);
    T("closed", !wv_window());
    app_prefs_set("WorldView", "Home", "");
    fs_rm_rf("/PUB");
    fs_rm_rf("/DOWNLOAD");
    return fails;
}

// World View: frames as links; bookmarks, history, downloads (its own
// pages, about:...)

/* the k-th button (a submit field) labelled so, clicked */
static bool press(wv_state_t *s, const char *label, int k) {
    for (int i = 0; s && i < s->doc.nfields; i++) {
        const wv_field_t *f = &s->doc.fields[i];
        if (f->type == FLD_SUBMIT && !strcmp(f->value, label) && k-- == 0) {
            wv_click(s, wv_item(s, IT_FIELD, i, -1));
            web_frames(3);
            return true;
        }
    }
    return false;
}

static int links_to(const wv_state_t *s, const char *url) {
    int n = 0;
    for (int i = 0; i < s->doc.nlinks; i++)
        n += !strcmp(doc_str(&s->doc, s->doc.links[i]), url);
    return n;
}

static int web_lists_test(void) {
    int fails = 0;
    char b[300];
    net_up();
    static const uint8_t ip[4] = {10, 0, 2, 15}, gw[4] = {10, 0, 2, 2},
                         nm[4] = {255, 255, 255, 0}, dns[4] = {10, 0, 2, 3};
    net_set_ip(ip, gw, nm);
    dns_set_server(dns);
    app_prefs_set("WorldView", "Proxy", "");
    app_prefs_set("WorldView", "Home", "http://frogfind.com/frames");
    fs_delete("/PREFS/WVBOOK.TXT");
    fs_delete("/PREFS/WVHIST.TXT");
    fs_delete("/PREFS/WVDOWN.TXT");
    fs_rm_rf("/DOWNLOAD");

    /* frames: a link each (nested ones too), no "get Netscape" */
    sim_launch("World View");
    wv_state_t *s = wv_wait();
    window *w = wv_window();
    T("a frameset: its frames, links",
      page_has(s, "This page is in frames") &&
          links_to(s, "http://frogfind.com/menu.htm") &&
          links_to(s, "http://frogfind.com/top.htm") &&
          links_to(s, "http://frogfind.com/main.htm"));
    T("... named by title, name, file", page_has(s, "Main page") &&
                                            page_has(s, "menu") &&
                                            page_has(s, "top.htm"));
    T("... the noframes text not shown", !page_has(s, "Netscape"));
    sim_shot("wv_frames");
    wv_click(s, wv_item(s, IT_TEXT, -1,
                        link_named(s, "http://frogfind.com/main.htm")));
    s = wv_wait();
    T("... a frame opens as a page", page_has(s, "main frame's page"));
    wv_address(s, "http://frogfind.com/iframe");
    s = wv_wait();
    T("an iframe: a link in its place, what's in it skipped",
      links_to(s, "http://frogfind.com/ad.htm") && page_has(s, "An ad") &&
          !page_has(s, "no iframes"));

    /* bookmarks */
    wv_address(s, "http://frogfind.com/");
    s = wv_wait();
    T("Add Bookmark",
      run_menu(w, "Add Bookmark") && strstr(s->line, "Added to the bookmarks"));
    run_menu(w, "Add Bookmark");
    T("... twice: once", strstr(s->line, "already") != NULL);
    wv_address(s, "http://frogfind.com/main.htm");
    s = wv_wait();
    ctl_focus(s->view);
    sim_key2(0x20, 'd', false, true); /* Ctrl+D */
    web_frames(1);
    T("... Ctrl+D", strstr(s->line, "Added to the bookmarks") != NULL);
    sim_key2(0x30, 'b', false, true); /* Ctrl+B */
    s = wv_wait();
    snprintf(b, sizeof b, "    %s: %d links, %d fields\n", s->url,
             s->doc.nlinks, s->doc.nfields);
    wr(1, b);
    T("Show Bookmarks (Ctrl+B): both, by title",
      !strcmp(s->url, "about:bookmarks") && page_has(s, "FrogFind!") &&
          page_has(s, "Main") && links_to(s, "http://frogfind.com/") == 1 &&
          links_to(s, "http://frogfind.com/main.htm") == 1);
    sim_shot("wv_bookmarks");

    /* a web page's form can't press those buttons */
    wv_address(s, "http://frogfind.com/sneakyform");
    s = wv_wait();
    press(s, "Remove", 0);
    T("a web page's form to about:bookmarks: not sent",
      strstr(s->url, "sneakyform") && strstr(s->line, "can't send"));
    run_menu(w, "Show Bookmarks");
    s = wv_wait();
    T("... the bookmark still there", links_to(s, "http://frogfind.com/") == 1);

    T("Remove", press(s, "Remove", 0));
    s = wv_wait();
    T("... the first gone, the page at its plain address",
      !strcmp(s->url, "about:bookmarks") &&
          !links_to(s, "http://frogfind.com/") &&
          links_to(s, "http://frogfind.com/main.htm") == 1);
    wv_click(s, wv_item(s, IT_TEXT, -1,
                        link_named(s, "http://frogfind.com/main.htm")));
    s = wv_wait();
    T("... a bookmark opens its page", page_has(s, "main frame's page"));
    sim_click(s->btn[0]->ax + 4, s->btn[0]->ay + 4);
    s = wv_wait();
    T("... Back: the bookmarks (not removing again)",
      !strcmp(s->url, "about:bookmarks") &&
          links_to(s, "http://frogfind.com/main.htm") == 1);

    /* the history: each page once, newest first, under Today */
    run_menu(w, "History");
    s = wv_wait();
    snprintf(b, sizeof b, "    history: %d links\n", s->doc.nlinks);
    wr(1, b);
    T("History: the pages visited, each once",
      !strcmp(s->url, "about:history") && page_has(s, "Today") &&
          links_to(s, "http://frogfind.com/main.htm") == 1 &&
          links_to(s, "http://frogfind.com/") == 1 &&
          links_to(s, "http://frogfind.com/frames") == 1 &&
          page_has(s, "Old Site"));
    T("... not its own pages", !links_to(s, "about:bookmarks"));
    sim_shot("wv_history");
    press(s, "Clear History", 0);
    s = wv_wait();
    T("... Clear History", page_has(s, "Nothing yet"));

    /* downloads: listed; Show in FileF; moved or deleted; stopped */
    wv_address(s, "http://frogfind.com/file.zip");
    web_frames(40);
    ctl_focus(s->view);
    sim_key2(0x24, 'j', false, true); /* Ctrl+J */
    s = wv_wait();
    T("Downloads (Ctrl+J): the file, where it is",
      !strcmp(s->url, "about:downloads") && page_has(s, "file.zip") &&
          page_has(s, "/DOWNLOAD/file.zip"));
    sim_shot("wv_downloads");
    T("... Show in FileF", press(s, "Show in FileF", 0));
    web_frames(3);
    window *ff = wm_focused_window();
    snprintf(b, sizeof b, "    in front: %s\n", ff ? ff->title : "-");
    wr(1, b);
    T("... FileF at /DOWNLOAD", ff && ff != w && strstr(ff->title, "DOWNLOAD"));
    sim_shot("wv_reveal");
    if (ff && ff != w)
        wm_close(ff);
    web_frames(3);
    wm_focus(w);
    fs_delete("/DOWNLOAD/file.zip");
    run_menu(w, "Reload");
    s = wv_wait();
    T("... the file deleted: says so, no buttons for it",
      page_has(s, "Moved or deleted") && !press(s, "Show in FileF", 0));
    wv_address(s, "http://frogfind.com/slow.bin");
    for (int i = 0; i < 100 && !s->dl_on; i++)
        web_frames(1);
    run_menu(w, "Downloads");
    s = wv_wait();
    T("... one going on: how far, Stop",
      s->dl_on && page_has(s, "slow.bin") && page_has(s, "Downloading"));
    T("... Stop", press(s, "Stop", 0));
    s = wv_wait();
    T("... stopped: Try again",
      !s->dl_on && page_has(s, "Stopped") &&
          links_to(s, "http://frogfind.com/slow.bin"));
    {
        dir_entry_t de;
        T("... its half a file deleted", !fs_find("/DOWNLOAD/slow.bin", &de));
    }
    T("... Remove from List", press(s, "Remove from List", 0));
    s = wv_wait();
    T("... that one gone", !page_has(s, "slow.bin") && page_has(s, "file.zip"));
    press(s, "Clear List", 0);
    s = wv_wait();
    T("... Clear List", page_has(s, "Nothing downloaded"));

    /* a download that ends while the page is open: it shows again */
    wv_address(s, "http://frogfind.com/slow.bin");
    for (int i = 0; i < 100 && !s->dl_on; i++)
        web_frames(1);
    run_menu(w, "Downloads");
    s = wv_wait();
    T("... the page open, downloading", page_has(s, "Downloading"));
    for (int i = 0; i < 4000 && s->dl_on; i++)
        web_frames(1);
    web_frames(3);
    s = wv_wait();
    T("... it ended: the page shows it saved",
      !strcmp(s->url, "about:downloads") && !page_has(s, "Downloading") &&
          page_has(s, "/DOWNLOAD/slow.bin"));

    wm_close(w);
    web_frames(5);
    app_prefs_set("WorldView", "Home", "");
    fs_rm_rf("/DOWNLOAD");
    fs_delete("/PREFS/WVBOOK.TXT");
    fs_delete("/PREFS/WVHIST.TXT");
    fs_delete("/PREFS/WVDOWN.TXT");
    return fails;
}

int scen_net(const char *name) {
    if (!strcmp(name, "web"))
        return web_test();
    if (!strcmp(name, "webfiles"))
        return web_files_test();
    if (!strcmp(name, "weblists"))
        return web_lists_test();
    if (!strcmp(name, "ftpui"))
        return ftp_ui();
    if (!strcmp(name, "ftp"))
        return ftp_test();
    if (!strcmp(name, "netui"))
        return net_ui();
    if (!strcmp(name, "dhcp"))
        return dhcp_test();
    if (!strcmp(name, "tcp"))
        return tcp_test();
    return -1;
}
