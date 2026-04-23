#include "os/clipboard.h"
#include "lib/memory.h"
#include "lib/string.h"

clipboard_t g_clipboard = {0};

void clipboard_clear(void) {
    memset(&g_clipboard, 0, sizeof(clipboard_t));
    g_clipboard.type = CLIP_EMPTY;
}

void clipboard_set_text(const char *text, int len) {
    clipboard_clear();
    if (!text || len <= 0)
        return;
    if (len >= CLIP_TEXT_MAX)
        len = CLIP_TEXT_MAX - 1;
    memcpy(g_clipboard.text, text, len);
    g_clipboard.text[len] = '\0';
    g_clipboard.text_len = len;
    g_clipboard.type = CLIP_TEXT;
}

void clipboard_set_file(const char *src_path, const char *name, bool is_dir,
                        bool is_cut, uint8_t src_drive) {
    clipboard_clear();
    if (!src_path || !name || name[0] == '\0')
        return;
    strncpy(g_clipboard.src_path, src_path, CLIP_PATH_MAX - 1);
    g_clipboard.src_path[CLIP_PATH_MAX - 1] = '\0';
    strncpy(g_clipboard.name, name, CLIP_NAME_MAX - 1);
    g_clipboard.name[CLIP_NAME_MAX - 1] = '\0';
    g_clipboard.is_dir = is_dir;
    g_clipboard.is_cut = is_cut;
    g_clipboard.src_drive = src_drive;
    g_clipboard.type = CLIP_FILE;
}
