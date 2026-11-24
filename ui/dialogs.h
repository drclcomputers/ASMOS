// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef UI_DIALOGS_H
#define UI_DIALOGS_H

/*
 * Non-blocking dialogs report results through callbacks. owner may be NULL;
 * otherwise it is blocked while the dialog is open and closing it also
 * closes the dialog. A NULL callback text indicates cancellation.
 */

#include "lib/core.h"
#include "ui/controls.h"

typedef void (*dlg_text_cb)(void *user, const char *text); /* NULL=cancel */
typedef void (*dlg_bool_cb)(void *user, bool yes);

void dlg_message(window *owner, const char *title, const char *msg);
/* Information box: a centred heading and subtitle, then rows of
 * "key: value" (key "" continues the row above), a footer and OK. */
typedef struct {
    const char *key, *value;
} dlg_row_t;
void dlg_info(window *owner, const char *title, const char *heading,
              const char *sub, const dlg_row_t *rows, int n,
              const char *footer);
void dlg_confirm(window *owner, const char *title, const char *msg,
                 dlg_bool_cb cb, void *user);
void dlg_input(window *owner, const char *title, const char *prompt,
               const char *initial, int max_len, uint32_t edit_style,
               dlg_text_cb cb, void *user);
/* dlg_input plus a check box. *checked gives its start state and receives
 * the final state just before cb runs (on OK only); it must outlive the
 * dialog (e.g. a field of your app state). */
void dlg_input_check(window *owner, const char *title, const char *prompt,
                     const char *initial, int max_len, uint32_t edit_style,
                     const char *check_label, bool *checked, dlg_text_cb cb,
                     void *user);
/* File pickers return a full path (e.g. "/FDD0/SONG.MID").
 * ext: show only files with this extension ("TXT"), or NULL for all. */
void dlg_open_file(window *owner, const char *title, const char *dir,
                   const char *ext, dlg_text_cb cb, void *user);
void dlg_save_file(window *owner, const char *title, const char *dir,
                   const char *name, const char *ext, dlg_text_cb cb,
                   void *user);

/* A folder (full path), from a dialog like the file ones */
void dlg_choose_folder(window *owner, const char *title, const char *dir,
                       dlg_text_cb cb, void *user);

/* Creates a custom dialog with zeroed, freed-on-close state. It closes via
 * its close box, owner destruction, dlg_form_close(), or an Esc cancel
 * button; controls may use c->user for app data. */
window *dlg_form(window *owner, const char *title, int w, int h,
                 uint32_t state_size);
void *dlg_form_state(window *form);
void dlg_form_close(window *form);

/* From code that must not create windows (fault/error handlers): queued,
 * shown as a dlg_message at the start of the next frame. */
void dlg_post_message(const char *title, const char *msg);
void dlg_flush_posted(void);
/* Draw a message box into the frame right now (before a halt). */
void dlg_draw_now(const char *title, const char *msg);

/* system hooks */
void dlg_owner_destroyed(window *owner);

/* Print: printer and status, copies, pages (text), Preview and Print. The
 * job goes to the print queue (os/print.h); the document must stay as it
 * is while the dialog is open (it is modal). */
enum { PRINT_DOC_TEXT, PRINT_DOC_IMAGE };
typedef struct {
    int kind;
    const char *name; /* in the queue */
    const char *text; /* PRINT_DOC_TEXT */
    uint32_t len;
    const uint8_t *pix; /* PRINT_DOC_IMAGE: 16-colour bytes */
    int w, h, stride;
} print_doc_t;
void dlg_print(window *owner, const print_doc_t *doc);

/* call fn(user, arg) for `owner`'s app (guarded for .APP windows) */
extern void (*g_dlg_call_hook)(window *owner, void (*fn)(void *, void *),
                               void *user, void *arg);

#endif
