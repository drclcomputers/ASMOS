#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#include "lib/core.h"

#define CLIP_TEXT_MAX 4096
#define CLIP_PATH_MAX 256
#define CLIP_NAME_MAX 13

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

extern clipboard_t g_clipboard;

void clipboard_clear(void);
void clipboard_set_text(const char *text, int len);
void clipboard_set_file(const char *src_path, const char *name, bool is_dir,
                        bool is_cut, uint8_t src_drive);
static inline bool clipboard_has_text(void) {
    return g_clipboard.type == CLIP_TEXT && g_clipboard.text_len > 0;
}
static inline bool clipboard_has_file(void) {
    return g_clipboard.type == CLIP_FILE && g_clipboard.name[0] != '\0';
}
static inline bool clipboard_empty(void) {
    return g_clipboard.type == CLIP_EMPTY;
}

#endif
