#ifndef FILEOPS_H
#define FILEOPS_H

/*
 * File operations shared by the desktop, FileF and anything else that shows
 * files. Paths are absolute: "/HDA/DIR/F.TXT", "/DESKTOP/F.TXT" (HDA is
 * implied), "/FDD0/F.TXT", "/HDB/...".
 *
 * The plain functions act immediately and return NULL on success or a short
 * reason. The fo_ask_* ones open a dialog and run `done(user)` after a
 * change was made (not on cancel). Every change notifies the desktop and
 * open FileF windows (fo_changed), so callers only refresh their own state.
 */

#include "fs/fs.h"
#include "lib/core.h"
#include "ui/window.h"

typedef void (*fo_done_cb)(void *user);

/* ── paths ─────────────────────────────────────────────────────────────── */
void fo_join(const char *dir, const char *name, char *out, int sz);
void fo_parent(const char *path, char *out, int sz);
const char *fo_base(const char *path);
/* "/HDA/X" and "/X" -> "/X" (upper case, no trailing '/'), for comparing */
void fo_norm(const char *path, char *out, int sz);
/* "/X" -> "/HDA/X": the form FileF shows */
void fo_vpath(const char *path, char *out, int sz);
uint8_t fo_drive(const char *path);
bool fo_stat(const char *path, bool *is_dir, uint32_t *size);
bool fo_protected(const char *path);
/* Sub-folders of `dir` (names, sorted), for folder trees. Returns count. */
int fo_list_dirs(const char *dir, char (*names)[FS_NAME_SZ], int max,
                 bool hidden);

/* ── operations ────────────────────────────────────────────────────────── */
const char *fo_create(const char *dir, const char *name, bool is_dir,
                      bool hidden);
const char *fo_rename(const char *path, const char *new_name);
const char *fo_delete(const char *path);
const char *fo_move(const char *src, const char *dst_dir); /* drag & drop */
const char *fo_set_hidden(const char *path, bool hidden);
const char *fo_clip(const char *path, bool cut); /* file clipboard */
const char *fo_paste(const char *dst_dir);
void fo_info_text(const char *path, char *out, int sz);

/* Open by type: folder -> FileF, .APP -> run, text -> TEditor,
 * .WAV -> play, .PIC -> ASMDraw, anything else -> HexView. */
void fo_open(window *owner, const char *path);
/* Run NAME.APP: package at that path, else on /apps /FDD0 /FDD1 /DESKTOP,
 * else the built-in app called NAME (desktop shortcuts). */
bool fo_launch_app(window *owner, const char *path);

/* Something in `dir` changed: refresh the desktop / FileF windows. */
void fo_changed(const char *dir);

/* ── dialogs ───────────────────────────────────────────────────────────── */
void fo_ask_new(window *owner, const char *dir, bool is_dir, fo_done_cb done,
                void *user);
void fo_ask_rename(window *owner, const char *path, fo_done_cb done,
                   void *user);
void fo_ask_delete(window *owner, const char *path, fo_done_cb done,
                   void *user);
void fo_show_info(window *owner, const char *path);
/* ZIP archives (ui/zipui.c): a dialog for the name, place and options,
 * then the job with a progress box. done runs after it worked. */
bool fo_is_zip(const char *path); /* by its name: .ZIP */
void fo_ask_compress(window *owner, const char *path, fo_done_cb done,
                     void *user);
void fo_ask_extract(window *owner, const char *zip, fo_done_cb done,
                    void *user);
/* Message for a failed operation (does nothing when err is NULL). */
void fo_report(window *owner, const char *title, const char *err);

/* ── Trash ──
 * /TRASH on HDA (hidden, protected). Deleting on HDA moves things there;
 * each item's original folder is kept (/PREFS/TRASH.INI) for Put Back.
 * Items on other drives, or already in the Trash, are deleted for good. */
#define FO_TRASH "/TRASH"
bool fo_in_trash(const char *path);     /* inside /TRASH (not /TRASH) */
bool fo_trashable(const char *path);    /* on HDA, outside the Trash */
const char *fo_trash(const char *path); /* move it to the Trash */
const char *fo_put_back(const char *trash_path);
const char *fo_empty_trash(void);
int fo_trash_count(void);
bool fo_is_trash(const char *path); /* /TRASH itself */
/* Empty the Trash after asking */
void fo_ask_empty_trash(window *owner, fo_done_cb done, void *user);

/* ── search ── */

/* Name test: with * or ? a whole-name pattern (case ignored), else
 * "contains". */
bool fo_match(const char *name, const char *pattern);

/* Walks root and its subfolders (depth <= 8) calling cb for each match;
 * cb returns false to stop. Returns the number of matches. */
typedef bool (*fo_find_cb)(const char *path, bool is_dir, uint32_t size,
                           void *user);
int fo_find(const char *root, const char *pattern, bool hidden, fo_find_cb cb,
            void *user);

#endif
