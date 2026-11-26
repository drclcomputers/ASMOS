// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "os/clipboard.h"
#include "lib/memory.h"
#include "lib/string.h"

clipboard_t *g_clipboard_ptr = NULL;
int g_clip_history_count = 0;
int g_clip_history_current = -1;

/* a scrapbook item: text (its own copy) or a picture (shared) */
typedef struct {
    clip_type_t type;
    char *text;
    int text_len;
    clip_image_t *image;
} clip_item_t;

static clip_item_t s_items[CLIP_HISTORY_MAX];

// pictures, shared by count

static uint32_t image_bytes(const clip_image_t *im) {
    return im ? (uint32_t)(im->w * im->h) + sizeof(clip_image_t) : 0;
}

static void image_drop(clip_image_t *im) {
    if (im && --im->refs <= 0) {
        kfree(im->pix);
        kfree(im);
    }
}

static clip_image_t *image_keep(clip_image_t *im) {
    if (im)
        im->refs++;
    return im;
}

void clipboard_init(void) {
    if (!g_clipboard_ptr)
        g_clipboard_ptr = (clipboard_t *)kzalloc(sizeof(clipboard_t));
    if (g_clipboard_ptr)
        g_clipboard_ptr->type = CLIP_EMPTY;
}

static void clipboard_ensure_init(void) {
    if (!g_clipboard_ptr)
        clipboard_init();
}

void clipboard_clear(void) {
    clipboard_ensure_init();
    if (!g_clipboard_ptr)
        return;
    image_drop(g_clipboard_ptr->image);
    memset(g_clipboard_ptr, 0, sizeof(clipboard_t));
    g_clipboard_ptr->type = CLIP_EMPTY;
    g_clip_history_current = -1;
}

// the scrapbook

static uint32_t item_bytes(const clip_item_t *it) {
    return it->type == CLIP_TEXT ? (uint32_t)it->text_len + 1
                                 : image_bytes(it->image);
}

static void item_free(clip_item_t *it) {
    kfree(it->text);
    image_drop(it->image);
    memset(it, 0, sizeof(*it));
}

void clipboard_history_remove(int index) {
    if (index < 0 || index >= g_clip_history_count)
        return;
    item_free(&s_items[index]);
    for (int i = index; i < g_clip_history_count - 1; i++)
        s_items[i] = s_items[i + 1];
    memset(&s_items[--g_clip_history_count], 0, sizeof(clip_item_t));
    if (g_clip_history_current == index)
        g_clip_history_current = -1;
    else if (g_clip_history_current > index)
        g_clip_history_current--;
}

void clipboard_history_clear(void) {
    while (g_clip_history_count > 0)
        clipboard_history_remove(g_clip_history_count - 1);
}

/* room for `more` bytes and one more item. Too many: the oldest goes.
 * Too big: the oldest picture (they are what fills it), then the oldest
 * of anything. Never the one on the clipboard, unless it is all there
 * is. */
static void make_room(uint32_t more) {
    while (g_clip_history_count) {
        uint32_t total = more;
        for (int i = 0; i < g_clip_history_count; i++)
            total += item_bytes(&s_items[i]);
        bool full = g_clip_history_count >= CLIP_HISTORY_MAX;
        if (!full && total <= CLIP_SCRAP_BYTES)
            return;
        int victim = -1;
        for (int i = 0; !full && victim < 0 && i < g_clip_history_count; i++)
            if (s_items[i].type == CLIP_IMAGE && i != g_clip_history_current)
                victim = i;
        if (victim < 0)
            victim =
                g_clip_history_current == 0 && g_clip_history_count > 1 ? 1 : 0;
        clipboard_history_remove(victim);
    }
}

/* a new item at the end (the caller makes it the one on the clipboard) */
static clip_item_t *push_item(clip_type_t type, uint32_t bytes) {
    make_room(bytes);
    if (g_clip_history_count >= CLIP_HISTORY_MAX)
        return NULL;
    clip_item_t *it = &s_items[g_clip_history_count];
    memset(it, 0, sizeof(*it));
    it->type = type;
    return it;
}

void clipboard_push_text(const char *text, int len) {
    if (!text || len <= 0)
        return;
    if (len >= CLIP_TEXT_MAX)
        len = CLIP_TEXT_MAX - 1;
    if (g_clip_history_count > 0) { /* the same as the newest: that one */
        clip_item_t *last = &s_items[g_clip_history_count - 1];
        if (last->type == CLIP_TEXT && last->text_len == len &&
            memcmp(last->text, text, (size_t)len) == 0) {
            g_clip_history_current = g_clip_history_count - 1;
            return;
        }
    }
    char *copy = (char *)kmalloc((uint32_t)len + 1);
    if (!copy)
        return;
    memcpy(copy, text, (size_t)len);
    copy[len] = 0;
    clip_item_t *it = push_item(CLIP_TEXT, (uint32_t)len + 1);
    if (!it) {
        kfree(copy);
        return;
    }
    it->text = copy;
    it->text_len = len;
    g_clip_history_current = g_clip_history_count++;
}

static void push_image(clip_image_t *im) {
    clip_item_t *it = push_item(CLIP_IMAGE, image_bytes(im));
    if (!it)
        return;
    it->image = image_keep(im);
    g_clip_history_current = g_clip_history_count++;
}

int clipboard_history_count(void) { return g_clip_history_count; }

int clipboard_history_kind(int index) {
    if (index < 0 || index >= g_clip_history_count)
        return CLIP_EMPTY;
    return s_items[index].type;
}

const char *clipboard_history_get_text(int index) {
    if (clipboard_history_kind(index) != CLIP_TEXT)
        return NULL;
    return s_items[index].text;
}

const clip_image_t *clipboard_history_image(int index) {
    if (clipboard_history_kind(index) != CLIP_IMAGE)
        return NULL;
    return s_items[index].image;
}

// the clipboard

/* text on the clipboard, without a new scrapbook item */
static void put_text(const char *text, int len) {
    clipboard_clear();
    if (!g_clipboard_ptr || !text || len <= 0)
        return;
    if (len >= CLIP_TEXT_MAX)
        len = CLIP_TEXT_MAX - 1;
    memcpy(g_clipboard_ptr->text, text, (size_t)len);
    g_clipboard_ptr->text[len] = '\0';
    g_clipboard_ptr->text_len = len;
    g_clipboard_ptr->type = CLIP_TEXT;
}

void clipboard_set_text(const char *text, int len) {
    put_text(text, len);
    if (clipboard_has_text())
        clipboard_push_text(text, len);
}

void clipboard_set_file(const char *src_path, const char *name, bool is_dir,
                        bool is_cut, uint8_t src_drive) {
    clipboard_clear();
    if (!g_clipboard_ptr || !src_path || !name || name[0] == '\0')
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

bool clipboard_set_image(int w, int h, const uint8_t *pix, int stride,
                         const uint8_t (*pal)[3]) {
    if (!pix || !pal || w < 1 || h < 1 || stride < w ||
        (uint32_t)w * (uint32_t)h > CLIP_IMAGE_MAX)
        return false;
    clip_image_t *im = (clip_image_t *)kzalloc(sizeof(clip_image_t));
    uint8_t *p = im ? (uint8_t *)kmalloc((uint32_t)(w * h)) : NULL;
    if (!p) {
        kfree(im);
        return false;
    }
    for (int y = 0; y < h; y++)
        memcpy(p + y * w, pix + y * stride, (size_t)w);
    im->w = w;
    im->h = h;
    im->pix = p;
    memcpy(im->pal, pal, sizeof(im->pal));
    im->refs = 1; /* the clipboard's */
    clipboard_clear();
    if (!g_clipboard_ptr) {
        image_drop(im);
        return false;
    }
    g_clipboard_ptr->type = CLIP_IMAGE;
    g_clipboard_ptr->image = im;
    push_image(im);
    return true;
}

bool clipboard_history_use(int index) {
    int k = clipboard_history_kind(index);
    if (k == CLIP_TEXT) {
        put_text(s_items[index].text, s_items[index].text_len);
    } else if (k == CLIP_IMAGE) {
        clip_image_t *im = image_keep(s_items[index].image);
        clipboard_clear();
        if (!g_clipboard_ptr) {
            image_drop(im);
            return false;
        }
        g_clipboard_ptr->type = CLIP_IMAGE;
        g_clipboard_ptr->image = im;
    } else {
        return false;
    }
    g_clip_history_current = index;
    return true;
}

int clipboard_kind(void) {
    return g_clipboard_ptr ? g_clipboard_ptr->type : CLIP_EMPTY;
}

const char *clipboard_text(void) {
    return clipboard_has_text() ? g_clipboard_ptr->text : NULL;
}

const clip_image_t *clipboard_image(void) {
    return clipboard_has_image() ? g_clipboard_ptr->image : NULL;
}
