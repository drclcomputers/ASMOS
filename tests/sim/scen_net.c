/* the network: DHCP from the fake NIC's server, and TCP between a client
 * and a server in the OS over the "wire" (10.0.2.99 mirrors), with frames
 * lost on the way */
#include "fs/netfs.h"
#include "network/dhcp.h"
#include "network/dns.h"
#include "network/ftpd.h"
#include "network/net.h"
#include "scen.h"

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

int scen_net(const char *name) {
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
