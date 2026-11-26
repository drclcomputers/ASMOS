// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#include "lib/core.h"

/* The clipboard holds text, a file (FileF's copy/cut), or a picture. Text
 * and pictures also go to the scrapbook (the history ClipView shows): the
 * last CLIP_HISTORY_MAX of them, at most CLIP_SCRAP_BYTES in all (the
 * oldest go first). Each item takes only the memory it needs. */

#define CLIP_TEXT_MAX 4096
#define CLIP_PATH_MAX 512 /* FS_PATH_SZ */
#define CLIP_NAME_MAX 64  /* FS_NAME_SZ: long names */

#define CLIP_HISTORY_MAX 20
#define CLIP_SCRAP_BYTES (384u * 1024u)
#define CLIP_IMAGE_MAX (320u * 1024u) /* the pixels of one picture */

typedef enum {
    CLIP_EMPTY = 0,
    CLIP_TEXT,
    CLIP_FILE,
    CLIP_IMAGE,
} clip_type_t;

typedef struct {
    int w, h;
    uint8_t *pix;
    uint8_t pal[256][3];
    int refs;
} clip_image_t;

typedef struct {
    clip_type_t type;

    char text[CLIP_TEXT_MAX];
    int text_len;

    char src_path[CLIP_PATH_MAX];
    char name[CLIP_NAME_MAX];
    bool is_dir;
    bool is_cut;
    uint8_t src_drive;

    clip_image_t *image; /* CLIP_IMAGE */
} clipboard_t;

/* Heap-allocated by clipboard_init() at boot. */
extern clipboard_t *g_clipboard_ptr;
extern int g_clip_history_count;
/* the scrapbook item that is on the clipboard now, -1 if none (a file, or
 * it was removed) */
extern int g_clip_history_current;

/* Convenience macro so existing code using g_clipboard.field still compiles. */
#define g_clipboard (*g_clipboard_ptr)

void clipboard_init(void);
void clipboard_clear(void);
void clipboard_set_text(const char *text, int len);
void clipboard_set_file(const char *src_path, const char *name, bool is_dir,
                        bool is_cut, uint8_t src_drive);
/* w x h pixels (rows of stride bytes) meaning the palette pal: copied.
 * False if it is too big (CLIP_IMAGE_MAX) or there is no memory. */
bool clipboard_set_image(int w, int h, const uint8_t *pix, int stride,
                         const uint8_t (*pal)[3]);

/* what it holds (clip_type_t), its text and picture (NULL if not) */
int clipboard_kind(void);
const char *clipboard_text(void);
const clip_image_t *clipboard_image(void);

/* the scrapbook: 0 = the oldest */
void clipboard_push_text(const char *text, int len);
int clipboard_history_count(void);
int clipboard_history_kind(int index);             /* CLIP_TEXT or CLIP_IMAGE */
const char *clipboard_history_get_text(int index); /* NULL: not text */
const clip_image_t *clipboard_history_image(int index); /* NULL: not one */
bool clipboard_history_use(int index); /* back on the clipboard */
void clipboard_history_remove(int index);
void clipboard_history_clear(void);

/* (the kernel's own: apps use clipboard_kind) */
static inline bool clipboard_has_text(void) {
    return g_clipboard_ptr && g_clipboard_ptr->type == CLIP_TEXT &&
           g_clipboard_ptr->text_len > 0;
}
static inline bool clipboard_has_file(void) {
    return g_clipboard_ptr && g_clipboard_ptr->type == CLIP_FILE &&
           g_clipboard_ptr->name[0] != '\0';
}
static inline bool clipboard_has_image(void) {
    return g_clipboard_ptr && g_clipboard_ptr->type == CLIP_IMAGE &&
           g_clipboard_ptr->image;
}
static inline bool clipboard_empty(void) {
    return !g_clipboard_ptr || g_clipboard_ptr->type == CLIP_EMPTY;
}

#endif
