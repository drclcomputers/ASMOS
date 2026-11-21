/* the game port: the driver on a fake port (sim.c), the calibration, the
 * Devices tab and the Joystick app */
#include "drivers/joystick.h"
#include "scen.h"

extern unsigned g_sim_joy_t[4], g_sim_joy_btn;

static control *find(window *w, ctl_type t, const char *text) {
    for (control *c = w ? w->controls : NULL; c; c = c->next)
        if (c->type == t && (!text || (c->text && strstr(c->text, text))))
            return c;
    return NULL;
}

static void stick(unsigned x, unsigned y) {
    g_sim_joy_t[0] = x;
    g_sim_joy_t[1] = y;
}

static void click_ctl(control *c) { sim_click(c->ax + 3, c->ay + 3); }

static int near(int a, int b, int d) { return a - b <= d && b - a <= d; }

int scen_joy(const char *name) {
    if (strcmp(name, "joystick"))
        return -1;
    int fails = 0;
    char b[300];

    /* the driver */
    T("no stick on the port: none found", !joy_detect() && !joy.present);
    stick(1000, 1000);
    T("a stick answers", joy_detect() && joy.present && joy.axes == 0x03);
    T("its time is measured (~1000 units)",
      near(joy_raw(0), 1000, 12) && near(joy_raw(1), 1000, 12));
    T("axes B: nothing there", joy_raw(2) == JOY_RAW_NONE);
    sim_frames(2);
    T("uncalibrated: centred as found", joy.axis[0] == 0 && joy.axis[1] == 0);
    stick(1400, 600); /* a guess: +-75% of the centre is the travel */
    sim_frames(2);
    T("uncalibrated: pushed right 47%, up -47%",
      near(joy.axis[0], 47, 5) && near(joy.axis[1], -47, 5));
    stick(1000, 1000);
    g_sim_joy_btn = 0x05;
    sim_frame();
    T("buttons 1 and 3", joy.buttons == 0x05 && joy.pressed == 0x05);
    sim_frame();
    T("pressed is for one frame", joy.buttons == 0x05 && joy.pressed == 0);
    g_sim_joy_btn = 0;
    sim_frame();
    T("released", joy.buttons == 0);

    /* Settings > Devices */
    sim_launch("Settings");
    sim_frames(3);
    window *sw = wm_focused_window();
    control *tabs = find(sw, CTL_TABS, NULL);
    T("Devices tab", tabs && strstr(tabs->text, "Devices"));
    int dev = 5;
    ctl_set_value(tabs, dev);
    sim_frames(2);
    control *list = find(sw, CTL_LIST, NULL);
    int jrow = -1;
    for (int i = 1; i < list_count(list); i++)
        if (strstr(list_get(list, i), "Joystick"))
            jrow = i;
    T("the list has a Joystick row", jrow > 0);
    T("it says 2 axes, uncalibrated",
      jrow > 0 && strstr(list_get(list, jrow), "2 axes") &&
          strstr(list_get(list, jrow), "uncalibrated"));
    T("and a keyboard and a mouse", strstr(list_get(list, 1), "Keyboard") &&
                                        strstr(list_get(list, 2), "Mouse"));
    sim_shot("joy_devices");
    stick(0, 0); /* unplugged */
    click_ctl(find(sw, CTL_BUTTON, "Refresh"));
    list = find(sw, CTL_LIST, NULL);
    bool gone = false;
    for (int i = 1; i < list_count(list); i++)
        if (strstr(list_get(list, i), "Joystick") &&
            strstr(list_get(list, i), "Not found"))
            gone = true;
    T("Refresh: unplugged says Not found", gone && !joy.present);
    stick(1000, 1000);
    click_ctl(find(sw, CTL_BUTTON, "Refresh"));
    T("Refresh: plugged again", joy.present);

    /* double-click the row: the Joystick app */
    list = find(sw, CTL_LIST, NULL);
    list_set_sel(list, 1);
    for (int r = 0;
         r < 12 && !find(wm_focused_window(), CTL_BUTTON, "Calibrate"); r++)
        sim_dblclick(list->ax + 20, list->ay + 2 + 9 + (r % 4) * 9);
    sim_frames(2);
    window *jw = wm_focused_window();
    control *cal = find(jw, CTL_BUTTON, "Calibrate");
    T("a double-click on Joystick opens its window", cal != NULL);
    if (!cal)
        return fails;
    sim_shot("joy_app");

    /* calibrate: centre, every edge */
    click_ctl(cal);
    T("step 1: the button says Next",
      !strcmp(cal->text, "Next") && find(jw, CTL_LABEL, "1 of 2"));
    click_ctl(cal);
    T("centre taken (~1000)",
      near(g_cfg.joy.mid[0], 1000, 12) && near(g_cfg.joy.mid[1], 1000, 12));
    T("step 2", find(jw, CTL_LABEL, "2 of 2") != NULL);
    click_ctl(cal);
    T("too early: not done, still step 2",
      !g_cfg.joy.calibrated && !strcmp(cal->text, "Next"));
    stick(400, 400);
    sim_frames(2);
    stick(1900, 1900);
    sim_frames(2);
    stick(1000, 1000);
    sim_frames(2);
    sim_shot("joy_range");
    click_ctl(cal);
    T("calibrated", g_cfg.joy.calibrated && !strcmp(cal->text, "Calibrate"));
    T("min and max are the extremes",
      near(g_cfg.joy.min[0], 400, 12) && near(g_cfg.joy.max[0], 1900, 12) &&
          near(g_cfg.joy.min[1], 400, 12) && near(g_cfg.joy.max[1], 1900, 12));
    stick(1900, 400);
    sim_frames(2);
    T("full right, full up: 100 and -100",
      near(joy.axis[0], 100, 2) && near(joy.axis[1], -100, 2));
    stick(1450, 1000); /* half way to the right edge: 50%, dead zone 10 */
    sim_frames(2);
    T("half: 44 with the dead zone (10%)", near(joy.axis[0], 44, 4));
    stick(1040, 960); /* inside the dead zone (10% of 900 = 90) */
    sim_frames(2);
    T("inside the dead zone: 0", joy.axis[0] == 0 && joy.axis[1] == 0);

    /* the dead zone and Reverse */
    control *dz = find(jw, CTL_SLIDER, NULL);
    ctl_set_value(dz, 0);
    dz->handler(dz, EV_CHANGE);
    stick(1450, 1000);
    sim_frames(2);
    T("dead zone 0: half is 50", near(joy.axis[0], 50, 3));
    control *rx = find(jw, CTL_CHECK, "X");
    ctl_set_value(rx, 1);
    rx->handler(rx, EV_CHANGE);
    sim_frames(2);
    T("Reverse X: -50", near(joy.axis[0], -50, 3) && (g_cfg.joy.invert & 1));
    ctl_set_value(rx, 0);
    rx->handler(rx, EV_CHANGE);

    /* OK keeps it in ASMOS.CFG */
    click_ctl(find(jw, CTL_BUTTON, "OK"));
    sim_frames(2);
    T("OK closes the window",
      !find(wm_focused_window(), CTL_BUTTON, "Calibrate"));
    joy_cfg_t kept = g_cfg.joy;
    g_cfg.joy.calibrated = 0;
    g_cfg.joy.mid[0] = 0;
    g_cfg.joy.deadzone = 33;
    T("ASMOS.CFG loads again", cfg_load());
    T("with the calibration",
      g_cfg.joy.calibrated && g_cfg.joy.mid[0] == kept.mid[0] &&
          g_cfg.joy.max[1] == kept.max[1] && g_cfg.joy.deadzone == 0);

    /* Cancel puts back what it started with */
    sim_launch("JOYSTICK");
    sim_frames(3);
    jw = wm_focused_window();
    cal = find(jw, CTL_BUTTON, "Calibrate");
    T("opens again from the registry", cal != NULL);
    if (!cal)
        return fails;
    click_ctl(cal);
    g_sim_joy_btn = 1; /* a button on the stick is Next */
    sim_frames(2);
    g_sim_joy_btn = 0;
    T("a stick button went to step 2", find(jw, CTL_LABEL, "2 of 2") != NULL);
    T("(and the old calibration is off meanwhile)", !g_cfg.joy.calibrated);
    click_ctl(find(jw, CTL_BUTTON, "Cancel"));
    sim_frames(2);
    T("Cancel: the calibration is back", g_cfg.joy.calibrated &&
                                             g_cfg.joy.mid[0] == kept.mid[0] &&
                                             g_cfg.joy.min[0] == kept.min[0]);

    /* a file from before the joystick (version 3) */
    {
        fat_file_t f;
        fs_delete("/ASMOS.CFG");
        fs_create("/ASMOS.CFG", &f);
        g_cfg.version = 3;
        fs_write(&f, &g_cfg, __builtin_offsetof(os_config_t, joy));
        fs_close(&f);
        g_cfg.wallpaper_pattern = 3;
        cfg_init_defaults();
        T("an older ASMOS.CFG loads", cfg_load());
        T("with the stick uncalibrated, dead zone 10",
          !g_cfg.joy.calibrated && g_cfg.joy.deadzone == 10);
    }

    /* unplugged: it goes after a while */
    g_sim_joy_t[0] = g_sim_joy_t[1] = 0;
    sim_frames(40);
    T("unplugged: gone", !joy.present && joy.axis[0] == 0);
    return fails;
}
