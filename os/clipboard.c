#include "os/clipboard.h"
#include "lib/memory.h"
#include "lib/string.h"

clipboard_t *g_clipboard_ptr = NULL;
clip_entry_t *g_clip_history = NULL;
int g_clip_history_count = 0;
int g_clip_history_current = -1;

void clipboard_init(void) {
    if (!g_clipboard_ptr)
        g_clipboard_ptr = (clipboard_t *)kzalloc(sizeof(clipboard_t));
    if (!g_clip_history)
        g_clip_history =
            (clip_entry_t *)kzalloc(CLIP_HISTORY_MAX * sizeof(clip_entry_t));
    if (g_clipboard_ptr)
        g_clipboard_ptr->type = CLIP_EMPTY;
}

static void clipboard_ensure_init(void) {
    if (!g_clipboard_ptr || !g_clip_history)
        clipboard_init();
}

void clipboard_clear(void) {
    clipboard_ensure_init();
    if (!g_clipboard_ptr)
        return;
    memset(g_clipboard_ptr, 0, sizeof(clipboard_t));
    g_clipboard_ptr->type = CLIP_EMPTY;
}

void clipboard_set_text(const char *text, int len) {
    clipboard_clear();
    if (!text || len <= 0)
        return;
    if (len >= CLIP_TEXT_MAX)
        len = CLIP_TEXT_MAX - 1;
    memcpy(g_clipboard_ptr->text, text, len);
    g_clipboard_ptr->text[len] = '\0';
    g_clipboard_ptr->text_len = len;
    g_clipboard_ptr->type = CLIP_TEXT;

    clipboard_push_text(text, len);
}

void clipboard_set_file(const char *src_path, const char *name, bool is_dir,
                        bool is_cut, uint8_t src_drive) {
    clipboard_clear();
    if (!src_path || !name || name[0] == '\0')
        return;
    strncpy(g_clipboard_ptr->src_path, src_path, CLIP_PATH_MAX - 1);
    g_clipboard_ptr->src_path[CLIP_PATH_MAX - 1] = '\0';
    strncpy(g_clipboard_ptr->name, name, CLIP_NAME_MAX - 1);
    g_clipboard_ptr->name[CLIP_NAME_MAX - 1] = '\0';
    g_clipboard_ptr->is_dir = is_dir;
    g_clipboard_ptr->is_cut = is_cut;
    g_clipboard_ptr->src_drive = src_drive;
    g_clipboard_ptr->type = CLIP_FILE;
}

void clipboard_push_text(const char *text, int len) {
    clipboard_ensure_init();
    if (!text || len <= 0 || !g_clip_history)
        return;

    if (g_clip_history_count > 0 && g_clip_history_current >= 0) {
        clip_entry_t *last = &g_clip_history[g_clip_history_current];
        if (last->type == CLIP_TEXT && last->text_len == len &&
            memcmp(last->text, text, len) == 0)
            return;
    }

    if (g_clip_history_count >= CLIP_HISTORY_MAX) {
        memmove(g_clip_history, g_clip_history + 1,
                (CLIP_HISTORY_MAX - 1) * sizeof(clip_entry_t));
        g_clip_history_count = CLIP_HISTORY_MAX - 1;
        g_clip_history_current--;
    }

    int idx = g_clip_history_current + 1;
    if (idx >= CLIP_HISTORY_MAX)
        idx = CLIP_HISTORY_MAX - 1;
    if (idx >= g_clip_history_count)
        idx = g_clip_history_count;

    clip_entry_t *entry = &g_clip_history[idx];
    memset(entry, 0, sizeof(*entry));
    entry->type = CLIP_TEXT;
    if (len >= CLIP_TEXT_MAX)
        len = CLIP_TEXT_MAX - 1;
    memcpy(entry->text, text, len);
    entry->text[len] = '\0';
    entry->text_len = len;

    g_clip_history_current = idx;
    if (idx >= g_clip_history_count)
        g_clip_history_count = idx + 1;
}

int clipboard_history_count(void) { return g_clip_history_count; }

const char *clipboard_history_get_text(int index) {
    if (!g_clip_history)
        return NULL;
    if (index < 0 || index >= g_clip_history_count)
        return NULL;
    if (g_clip_history[index].type != CLIP_TEXT)
        return NULL;
    return g_clip_history[index].text;
}
