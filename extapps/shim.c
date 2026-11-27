// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "extapps/shim.h"

const os_api_t *A;
extern const app_meta_t app_meta;

// entry points (mkapp: init / on_frame / destroy)

void init(const os_api_t *api, void *state) {
    A = api;
    if (api->size < sizeof(os_api_t)) { /* an older kernel */
        api->ui->message(NULL, app_meta.name, "This app needs a newer ASMOS.");
        api->quit_self(state);
        return;
    }
    if (app_meta.init)
        app_meta.init(state);
}

void on_frame(const os_api_t *api, void *state) {
    A = api;
    if (app_meta.frame && api->size >= sizeof(os_api_t))
        app_meta.frame(state);
}

void destroy(const os_api_t *api, void *state) {
    A = api;
    if (app_meta.destroy && api->size >= sizeof(os_api_t))
        app_meta.destroy(state);
}

bool app_open_request(char *path, int sz) { return A->open_request(path, sz); }
int shim_screen_w(void) {
    int w, h;
    A->screen_size(&w, &h);
    return w;
}
int shim_screen_h(void) {
    int w, h;
    A->screen_size(&w, &h);
    return h;
}

// memory: the app's own heap
void *kmalloc(uint32_t size) { return A->kmalloc(size); }
void kfree(void *p) { A->kfree(p); }
uint32_t time_millis(void) { return A->time_millis(); }

// windows and controls
window *app_window(void *state, const char *title, int x, int y, int w, int h,
                   uint8_t bar, uint32_t flags) {
    return A->ui->app_window(state, title, x, y, w, h, bar, flags);
}
control *ctl_label(window *w, int x, int y, int cw, int ch, const char *text,
                   uint32_t style) {
    return A->ui->label(w, x, y, cw, ch, text, style);
}
control *ctl_button(window *w, int x, int y, int cw, int ch, const char *text,
                    int id, ctl_handler h) {
    return A->ui->button(w, x, y, cw, ch, text, id, h);
}
control *ctl_edit(window *w, int x, int y, int cw, int ch, const char *text,
                  int max_len, uint32_t style, ctl_handler h) {
    return A->ui->edit(w, x, y, cw, ch, text, max_len, style, h);
}
control *ctl_list(window *w, int x, int y, int cw, int ch, uint32_t style,
                  ctl_handler h) {
    return A->ui->list(w, x, y, cw, ch, style, h);
}
control *ctl_vscroll(window *w, int x, int y, int ch, ctl_handler h) {
    return A->ui->vscroll(w, x, y, ch, h);
}
control *ctl_slider(window *w, int x, int y, int cw, int min, int max,
                    int value, ctl_handler h) {
    return A->ui->slider(w, x, y, cw, min, max, value, h);
}
control *ctl_custom(window *w, int x, int y, int cw, int ch, ctl_painter p,
                    ctl_handler h, uint32_t style) {
    return A->ui->custom(w, x, y, cw, ch, p, h, style);
}
control *ctl_status(window *w, const char *text) {
    return A->ui->status(w, text);
}
void ctl_set_text(control *c, const char *t) { A->ui->set_text(c, t); }
const char *ctl_text(const control *c) { return A->ui->text(c); }
void ctl_set_value(control *c, int v) { A->ui->set_value(c, v); }
int ctl_value(const control *c) { return A->ui->value(c); }
void ctl_set_range(control *c, int mn, int mx, int pg) {
    A->ui->set_range(c, mn, mx, pg);
}
void ctl_enable(control *c, bool on) { A->ui->enable(c, on); }
void ctl_focus(control *c) { A->ui->focus(c); }
void ctl_set_colors(control *c, uint8_t fg, uint8_t bg) {
    A->ui->set_colors(c, fg, bg);
}
void ctl_style_add(control *c, uint32_t st) { A->ui->style_add(c, st); }
void ctl_set_tooltip(control *c, const char *t) { A->ui->set_tooltip(c, t); }
control *ctl_focused(window *w) { return A->focused_control(w); }

int list_add(control *c, const char *t) { return A->ui->list_add(c, t); }
int list_add_data(control *c, const char *t, uint32_t d) {
    return A->ui->list_add_data(c, t, d);
}
void list_clear(control *c) { A->ui->list_clear(c); }
uint32_t list_data(const control *c, int i) { return A->ui->list_data(c, i); }
int list_sel(const control *c) { return A->ui->list_sel(c); }
void list_set_sel(control *c, int i) { A->ui->list_set_sel(c, i); }
int list_count(const control *c) { return A->ui->list_count(c); }
const char *list_get(const control *c, int i) { return A->ui->list_get(c, i); }

// dialogs
void dlg_message(window *owner, const char *title, const char *msg) {
    A->ui->message(owner, title, msg);
}
void dlg_confirm(window *owner, const char *title, const char *msg,
                 dlg_bool_cb cb, void *user) {
    A->ui->confirm(owner, title, msg, cb, user);
}
void dlg_input(window *owner, const char *title, const char *prompt,
               const char *initial, int max_len, uint32_t style, dlg_text_cb cb,
               void *user) {
    A->ui->input(owner, title, prompt, initial, max_len, style, cb, user);
}
void dlg_open_file(window *owner, const char *title, const char *dir,
                   const char *ext, dlg_text_cb cb, void *user) {
    A->ui->open_file(owner, title, dir, ext, cb, user);
}
void dlg_save_file(window *owner, const char *title, const char *dir,
                   const char *name, const char *ext, dlg_text_cb cb,
                   void *user) {
    A->ui->save_file(owner, title, dir, name, ext, cb, user);
}
void dlg_print(window *owner, const print_doc_t *doc) { A->print(owner, doc); }

// menus: items run fn(state); a void fn(void) ignores it
menu *window_add_menu(window *win, const char *title) {
    return A->ui->add_menu(win, title);
}
void menu_add_item(menu *m, char *label, MenuAction action) {
    A->ui->menu_item(m, label, (void (*)(void *))action);
}
void menu_add_separator(menu *m) { A->ui->menu_separator(m); }
void menu_enable_last(menu *m, bool on) { A->ui->menu_enable(m, on); }

// asking to save before closing (ui/window.h)
void wm_ask_save(window *w, bool (*save)(window *w)) {
    A->ui->ask_save(w, save);
}
void wm_set_unsaved(window *w, bool unsaved) { A->ui->set_unsaved(w, unsaved); }
void wm_close(window *w) { A->ui->close(w); }
void dlg_ask(window *owner, const char *title, const char *msg,
             const char *const *buttons, int n, dlg_int_cb cb, void *user) {
    A->ui->ask(owner, title, msg, buttons, n, cb, user);
}

// right-click menus: fill popup_menu(), then popup_show_for(c)
menu *popup_menu(void) { return A->ui->popup_menu(); }
void popup_show(int x, int y) { A->ui->popup_show(x, y); }
void popup_show_for(const control *c) { A->ui->popup_show_for(c); }

// the window manager
void wm_invalidate(window *w) { A->invalidate(w); }
void wm_focus(window *w) { A->wm_focus(w); }
bool window_is_focused(window *w) { return A->window_focused(w); }
void *ui_active_state(void) { return A->active_state(); }
bool os_close_own_instance(window *win) {
    A->quit_self(win ? win->userdata : A->active_state());
    return true;
}
void os_quit_app_by_desc(app_descriptor *d) {
    (void)d; /* only ever its own: the app whose window is in front */
    A->quit_self(A->active_state());
}

// drawing
void draw_line(int x0, int y0, int x1, int y1, unsigned char c) {
    A->draw_line(x0, y0, x1, y1, c);
}
void draw_rect(int x, int y, int w, int h, unsigned char c) {
    A->draw_rect(x, y, w, h, c);
}
void fill_rect(int x, int y, int w, int h, unsigned char c) {
    A->fill_rect(x, y, w, h, c);
}
void draw_char(int x, int y, char ch, unsigned char c, int size) {
    A->draw_char(x, y, ch, c, size);
}
void draw_string(int x, int y, const char *s, unsigned char c, int size) {
    A->draw_string(x, y, s, c, size);
}
void draw_text_n(int x, int y, const char *s, int n, unsigned char c,
                 int size) {
    A->draw_text_n(x, y, s, n, c, size);
}
void draw_pixels(int x, int y, int w, int h, const uint8_t *src, int stride) {
    A->draw_pixels(x, y, w, h, src, stride);
}
int text_width(const char *s, int size) { return A->text_width(s, size); }
int font_height(int size) { return A->font_height(size); }

// settings: /PREFS/<app name>.INI (the API picks the file)
bool app_prefs_get(const char *app, const char *key, char *out, int sz) {
    (void)app;
    return A->prefs_get(key, out, sz);
}
int app_prefs_get_int(const char *app, const char *key, int def) {
    (void)app;
    return A->prefs_get_int(key, def);
}
bool app_prefs_set(const char *app, const char *key, const char *value) {
    (void)app;
    return A->prefs_set(key, value);
}
bool app_prefs_set_int(const char *app, const char *key, int value) {
    (void)app;
    return A->prefs_set_int(key, value);
}
void app_prefs_begin(const char *app) { (void)app; }
bool app_prefs_end(void) { return true; }

// time
time_full_t time_rtc_local(void) { return A->time->now(); }
int time_days_in_month(int m, int y) { return A->time->days_in_month(m, y); }
bool time_date_valid(int y, int m, int d) {
    return A->time->date_valid(y, m, d);
}
int32_t time_days_from_civil(int y, int m, int d) {
    return A->time->days_from_civil(y, m, d);
}
void time_civil_from_days(int32_t z, int *y, int *m, int *d) {
    A->time->civil_from_days(z, y, m, d);
}
int time_weekday(int y, int m, int d) { return A->time->weekday(y, m, d); }
int time_iso_week(int y, int m, int d) { return A->time->iso_week(y, m, d); }
const char *time_month_name(int m) { return A->time->month_name(m); }
const char *time_weekday_name(int wd) { return A->time->weekday_name(wd); }

// sound
void speaker_beep_async(uint32_t hz, uint32_t ms) { A->sound->beep(hz, ms); }
bool sb16_detected(void) { return A->sound->has_sb(); }
bool opl2_detected(void) { return A->sound->has_fm(); }
void sb16_set_volume(uint8_t l, uint8_t r) {
    (void)r;
    A->sound->set_volume(l); /* one volume for everything */
}
void opl2_set_master_volume(uint8_t v) { (void)v; }
void mixer_write(uint8_t reg, uint8_t v) {
    (void)reg;
    (void)v;
}
const char *audio_open(const char *path) { return A->sound->open(path); }
void audio_close(void) { A->sound->close(); }
void audio_play(void) { A->sound->play(); }
void audio_pause(void) { A->sound->pause(); }
void audio_stop(void) { A->sound->stop(); }
void audio_set_loop(bool loop) { A->sound->set_loop(loop); }
bool audio_seek_ms(uint32_t ms) { return A->sound->seek_ms(ms); }
int audio_state(void) { return A->sound->state(); }
bool audio_finished(void) { return A->sound->finished(); }
uint32_t audio_pos_ms(void) { return A->sound->pos_ms(); }
uint32_t audio_len_ms(void) { return A->sound->len_ms(); }
const char *audio_codec_name(void) { return A->sound->codec(); }
const char *audio_info(void) { return A->sound->info(); }
const char *audio_file(void) { return A->sound->file(); }
int audio_level(void) { return A->sound->level(); }
const char *const *audio_extensions(void) { return A->sound->extensions(); }
void audio_update(void) {} /* the OS does it every frame */

// clipboard
void clipboard_set_text(const char *t, int len) { A->clip->set_text(t, len); }
int clipboard_history_count(void) { return A->clip->history_count(); }
bool clipboard_set_image(int w, int h, const uint8_t *pix, int stride,
                         const uint8_t (*pal)[3]) {
    return A->clip->set_image(w, h, pix, stride, pal);
}
int clipboard_kind(void) { return A->clip->kind(); }
const char *clipboard_text(void) { return A->clip->text(); }
const clip_image_t *clipboard_image(void) { return A->clip->image(); }
int clipboard_history_kind(int i) { return A->clip->history_kind(i); }
const clip_image_t *clipboard_history_image(int i) {
    return A->clip->history_image(i);
}
bool clipboard_history_use(int i) { return A->clip->history_use(i); }
void clipboard_history_remove(int i) { A->clip->history_remove(i); }
void clipboard_history_clear(void) { A->clip->history_clear(); }
const char *clipboard_history_get_text(int i) {
    return A->clip->history_text(i);
}

// the network (API size 14): network/net.h, network/dns.h
bool net_ready(void) { return A->net->ready(); }
int dns_start(const char *host) { return A->net->resolve_start(host); }
int dns_poll(int h, uint8_t ip[4]) { return A->net->resolve_poll(h, ip); }
void dns_cancel(int h) { A->net->resolve_cancel(h); }
int tcp_open(const uint8_t ip[4], uint16_t port, uint16_t local_port) {
    (void)local_port; /* (any) */
    return A->net->tcp_open(ip, port);
}
tcp_state_t tcp_state(int sock) { return (tcp_state_t)A->net->tcp_state(sock); }
bool tcp_was_reset(int sock) { return A->net->tcp_was_reset(sock); }
int tcp_write(int sock, const uint8_t *d, uint16_t n) {
    return A->net->tcp_write(sock, d, n);
}
int tcp_recv(int sock, uint8_t *b, uint16_t n) {
    return A->net->tcp_recv(sock, b, n);
}
uint16_t tcp_available(int sock) { return A->net->tcp_available(sock); }
uint16_t tcp_pending(int sock) { return A->net->tcp_pending(sock); }
void tcp_release(int sock) { A->net->tcp_close(sock); }
void tcp_abort(int sock) { A->net->tcp_abort(sock); }

// glyphs and the code page (API size 14)
int font_glyph(int size, unsigned char c, uint8_t rows[8], int *w) {
    return A->font_glyph(size, c, rows, w);
}
const uint16_t *app_codepage(void) { return A->codepage(); }

// pictures from memory (API size 15): lib/image.h
int image_kind(const uint8_t *b, uint32_t n) { return A->image->kind(b, n); }
const char *image_info(const uint8_t *b, uint32_t n, int *w, int *h) {
    return A->image->info(b, n, w, h);
}
const char *image_decode(const uint8_t *b, uint32_t n, uint8_t *out, int dw,
                         int dh, int stride, const image_target_t *t) {
    return A->image->decode(b, n, out, dw, dh, stride, t);
}
