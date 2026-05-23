#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#include "lib/core.h"

#define CLIP_TEXT_MAX 4096
#define CLIP_PATH_MAX 256
#define CLIP_NAME_MAX 13

#define CLIP_HISTORY_MAX 20

typedef enum {
    CLIP_EMPTY = 0,
    CLIP_TEXT,
    CLIP_FILE,
} clip_type_t;

typedef struct {
    clip_type_t type;

    char text[CLIP_TEXT_MAX];
    int text_len;

    char src_path[CLIP_PATH_MAX];
    char name[CLIP_NAME_MAX];
    bool is_dir;
    bool is_cut;
    uint8_t src_drive;
} clipboard_t;

typedef clipboard_t clip_entry_t;

/* Both heap-allocated by clipboard_init() at boot. */
extern clipboard_t *g_clipboard_ptr;
extern clip_entry_t *g_clip_history;
extern int g_clip_history_count;
extern int g_clip_history_current;

/* Convenience macro so existing code using g_clipboard.field still compiles. */
#define g_clipboard (*g_clipboard_ptr)

void clipboard_init(void);
void clipboard_clear(void);
void clipboard_set_text(const char *text, int len);
void clipboard_set_file(const char *src_path, const char *name, bool is_dir,
                        bool is_cut, uint8_t src_drive);

void clipboard_push_text(const char *text, int len);
int clipboard_history_count(void);
const char *clipboard_history_get_text(int index);

static inline bool clipboard_has_text(void) {
    return g_clipboard_ptr && g_clipboard_ptr->type == CLIP_TEXT &&
           g_clipboard_ptr->text_len > 0;
}
static inline bool clipboard_has_file(void) {
    return g_clipboard_ptr && g_clipboard_ptr->type == CLIP_FILE &&
           g_clipboard_ptr->name[0] != '\0';
}
static inline bool clipboard_empty(void) {
    return !g_clipboard_ptr || g_clipboard_ptr->type == CLIP_EMPTY;
}

#endif
