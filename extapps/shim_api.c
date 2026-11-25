// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "extapps/shim.h"

// controls (ui/controls.h)

void win_client_rect(const window *w, int *x, int *y, int *cw, int *ch) {
    A->ui->client_rect(w, x, y, cw, ch);
}

bool ctl_menu_from_key(void) { return A->ui->menu_from_key(); }

control *ctl_check(window *w, int x, int y, int cw, const char *text,
                   bool checked, ctl_handler h) {
    return A->ui->check(w, x, y, cw, text, checked, h);
}

control *ctl_radio(window *w, int x, int y, int cw, const char *text, int group,
                   bool selected, ctl_handler h) {
    return A->ui->radio(w, x, y, cw, text, group, selected, h);
}

control *ctl_tree(window *w, int x, int y, int cw, int ch, uint32_t style,
                  ctl_handler h) {
    return A->ui->tree(w, x, y, cw, ch, style, h);
}

control *ctl_combo(window *w, int x, int y, int cw, ctl_handler h) {
    return A->ui->combo(w, x, y, cw, h);
}

control *ctl_hscroll(window *w, int x, int y, int cw, ctl_handler h) {
    return A->ui->hscroll(w, x, y, cw, h);
}

control *ctl_progress(window *w, int x, int y, int cw, int ch, int max) {
    return A->ui->progress(w, x, y, cw, ch, max);
}

control *ctl_group(window *w, int x, int y, int cw, int ch, const char *title) {
    return A->ui->group(w, x, y, cw, ch, title);
}

control *ctl_tabs(window *w, int x, int y, int cw, const char *pages,
                  ctl_handler h) {
    return A->ui->tabs(w, x, y, cw, pages, h);
}

void ctl_destroy(control *c) { A->ui->destroy(c); }

void ctl_show(control *c, bool visible) { A->ui->show(c, visible); }

void ctl_set_handler(control *c, ctl_handler h, void *user) {
    A->ui->set_handler(c, h, user);
}

void ctl_move(control *c, int x, int y, int cw, int ch) {
    A->ui->move(c, x, y, cw, ch);
}

void ctl_on_page(control *c, control *tabs, int page) {
    A->ui->on_page(c, tabs, page);
}

control *ctl_by_id(window *w, int id) { return A->ui->by_id(w, id); }

void list_set(control *c, int i, const char *text) {
    A->ui->list_set(c, i, text);
}

void list_remove(control *c, int i) { A->ui->list_remove(c, i); }

void list_set_columns(control *c, const int *widths, int n) {
    A->ui->list_set_columns(c, widths, n);
}

int tree_add(control *c, int parent, const char *text, uint32_t data,
             bool can_expand) {
    return A->ui->tree_add(c, parent, text, data, can_expand);
}

void tree_clear(control *c) { A->ui->tree_clear(c); }

void tree_clear_children(control *c, int node) {
    A->ui->tree_clear_children(c, node);
}

int tree_parent(const control *c, int node) {
    return A->ui->tree_parent(c, node);
}

int tree_first_child(const control *c, int node) {
    return A->ui->tree_first_child(c, node);
}

int tree_next_sibling(const control *c, int node) {
    return A->ui->tree_next_sibling(c, node);
}

const char *tree_text(const control *c, int node) {
    return A->ui->tree_text(c, node);
}

void tree_set_text(control *c, int node, const char *text) {
    A->ui->tree_set_text(c, node, text);
}

uint32_t tree_data(const control *c, int node) {
    return A->ui->tree_data(c, node);
}

int tree_sel(const control *c) { return A->ui->tree_sel(c); }

void tree_set_sel(control *c, int node) { A->ui->tree_set_sel(c, node); }

void tree_expand(control *c, int node, bool open) {
    A->ui->tree_expand(c, node, open);
}

bool tree_is_expanded(const control *c, int node) {
    return A->ui->tree_is_expanded(c, node);
}

int tree_event_node(const control *c) { return A->ui->tree_event_node(c); }

void tree_path(const control *c, int node, char *out, int sz) {
    A->ui->tree_path(c, node, out, sz);
}

int tree_find_path(control *c, const char *path) {
    return A->ui->tree_find_path(c, path);
}

void edit_select(control *c, int from, int to) {
    A->ui->edit_select(c, from, to);
}

void edit_select_all(control *c) { A->ui->edit_select_all(c); }

void edit_insert(control *c, const char *s) { A->ui->edit_insert(c, s); }

int edit_line_of(const control *c, int pos) {
    return A->ui->edit_line_of(c, pos);
}

int edit_col_of(const control *c, int pos) {
    return A->ui->edit_col_of(c, pos);
}

void edit_goto(control *c, int line, int col) {
    A->ui->edit_goto(c, line, col);
}

bool edit_find(control *c, const char *needle, bool from_start) {
    return A->ui->edit_find(c, needle, from_start);
}

void edit_copy(control *c) { A->ui->edit_copy(c); }

void edit_cut(control *c) { A->ui->edit_cut(c); }

void edit_paste(control *c) { A->ui->edit_paste(c); }

// dialogs (ui/dialogs.h)

void dlg_input_check(window *owner, const char *title, const char *prompt,
                     const char *initial, int max_len, uint32_t edit_style,
                     const char *check_label, bool *checked, dlg_text_cb cb,
                     void *user) {
    A->ui->input_check(owner, title, prompt, initial, max_len, edit_style,
                       check_label, checked, cb, user);
}

void dlg_color(window *owner, const char *title, const uint8_t rgb[3],
               int preview, dlg_color_cb cb, void *user) {
    A->ui->color(owner, title, rgb, preview, cb, user);
}

// drawing (lib/graphics.h)

void draw_dot(int x, int y, unsigned char color) { A->draw_dot(x, y, color); }

// settings (config/runtime_config.h)

int cfg_frame_ms(void) { return A->frame_ms(); }

// .BMP pictures (lib/bmp.h; bmp_start, bmp_end and bmp_read_all are
// inline there and use kmalloc: the app's own heap)

const char *bmp_open(const char *path, bmp_info_t *bi) {
    return A->bmp->open(path, bi);
}
uint32_t bmp_work_size(const bmp_info_t *bi, const bmp_target_t *t) {
    return A->bmp->work_size(bi, t);
}
bmp_reader_t *bmp_begin(const char *path, const bmp_info_t *bi,
                        const bmp_target_t *t, uint8_t *out, int out_w,
                        int out_h, int stride, void *work) {
    return A->bmp->begin(path, bi, t, out, out_w, out_h, stride, work);
}
int bmp_rows(bmp_reader_t *r, int n) { return A->bmp->rows(r, n); }
const char *bmp_error(const bmp_reader_t *r) { return A->bmp->error(r); }
const char *bmp_save(const char *path, const uint8_t *pix, int w, int h,
                     int stride, const uint8_t (*pal)[3], int colors) {
    return A->bmp->save(path, pix, w, h, stride, pal, colors);
}
