#include "os/appload.h"

#include "fs/fs.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "os/error.h"
#include "os/os.h"
#include "ui/modal.h"

static loaded_app_t s_loaded[MAX_LOADED_APPS];
static int s_loaded_count = 0;

static uint32_t s_last_fail_tick = 0;
static int s_fail_count_window = 0;

static loaded_app_t *find_free_slot(void) {
    for (int i = 0; i < MAX_LOADED_APPS; i++) {
        if (!s_loaded[i].loaded)
            return &s_loaded[i];
    }
    return NULL;
}

static loaded_app_t *find_by_desc(app_descriptor *desc) {
    if (!desc)
        return NULL;
    for (int i = 0; i < MAX_LOADED_APPS; i++) {
        if (s_loaded[i].loaded && &s_loaded[i].desc == desc)
            return &s_loaded[i];
    }
    return NULL;
}

static loaded_app_t *find_by_name(const char *name) {
    if (!name)
        return NULL;
    for (int i = 0; i < MAX_LOADED_APPS; i++) {
        if (s_loaded[i].loaded && strcmp(s_loaded[i].desc.name, name) == 0)
            return &s_loaded[i];
    }
    return NULL;
}

static void quiet_fail(const char *why) {
    extern volatile uint32_t pit_ticks;
    if (pit_ticks - s_last_fail_tick > 500) {
        s_fail_count_window = 0;
        s_last_fail_tick = pit_ticks;
    }
    s_fail_count_window++;
    if (s_fail_count_window == 1 && why) {
        char msg[160];
        snprintf(msg, sizeof(msg), "%s", why);
        modal_show(MODAL_INFO, "App Load", msg, NULL, NULL);
    }
}

static loaded_app_t *la_from_state(void *state) {
    if (!state)
        return NULL;
    for (int j = 0; j < MAX_RUNNING_APPS; j++) {
        if (running_apps[j].running && running_apps[j].state == state)
            return find_by_desc(running_apps[j].desc);
    }
    return NULL;
}

static void trampoline_init(void *state) {
    loaded_app_t *la = la_from_state(state);
    if (!la || !la->code_base || !la->hdr.entry_init)
        return;
    if (la->hdr.entry_init >= la->code_size)
        return;
    typedef void (*init_fn)(void *);
    init_fn fn = (init_fn)(la->code_base + la->hdr.entry_init);
    fn(state);
}

static void trampoline_frame(void *state) {
    loaded_app_t *la = la_from_state(state);
    if (!la || !la->code_base || !la->hdr.entry_frame)
        return;
    if (la->hdr.entry_frame >= la->code_size)
        return;
    typedef void (*frame_fn)(void *);
    frame_fn fn = (frame_fn)(la->code_base + la->hdr.entry_frame);
    fn(state);
}

static void trampoline_destroy(void *state) {
    loaded_app_t *la = la_from_state(state);
    if (!la || !la->code_base || !la->hdr.entry_destroy)
        return;
    if (la->hdr.entry_destroy >= la->code_size)
        return;
    typedef void (*destroy_fn)(void *);
    destroy_fn fn = (destroy_fn)(la->code_base + la->hdr.entry_destroy);
    fn(state);
}

app_descriptor *appload_load(const char *path) {
    if (!path || !path[0])
        return NULL;

    for (int i = 0; i < MAX_LOADED_APPS; i++) {
        if (s_loaded[i].loaded && strcmp(s_loaded[i].path, path) == 0)
            return &s_loaded[i].desc;
    }

    fat_file_t f;
    if (!fs_open(path, &f))
        return NULL;

    uint32_t file_size = f.entry.file_size;

    if (file_size == 0) {
        fs_close(&f);
        return NULL;
    }

    if (file_size < sizeof(asma_header_t) ||
        file_size > ASMA_MAX_CODE_SIZE + 512) {
        fs_close(&f);
        quiet_fail("File is not a valid .APP package (bad size).");
        return NULL;
    }

    uint8_t *buf = (uint8_t *)kmalloc(file_size);
    if (!buf) {
        fs_close(&f);
        quiet_fail("Out of memory while loading app.");
        return NULL;
    }

    int got = fs_read(&f, buf, (int)file_size);
    fs_close(&f);
    if (got != (int)file_size) {
        kfree(buf);
        quiet_fail("Could not read the whole .APP file.");
        return NULL;
    }

    asma_header_t *hdr = (asma_header_t *)buf;

    if (hdr->magic != ASMA_MAGIC || hdr->version != ASMA_VERSION) {
        kfree(buf);
        return NULL;
    }

    if (hdr->header_size < sizeof(asma_header_t) ||
        hdr->header_size > file_size || hdr->code_offset < hdr->header_size ||
        hdr->code_offset + hdr->code_size > file_size || hdr->code_size == 0) {
        kfree(buf);
        quiet_fail("Corrupt .APP header.");
        return NULL;
    }

    if (hdr->entry_init >= hdr->code_size ||
        hdr->entry_frame >= hdr->code_size ||
        (hdr->entry_destroy != 0 && hdr->entry_destroy >= hdr->code_size)) {
        kfree(buf);
        quiet_fail("Corrupt .APP entry points.");
        return NULL;
    }

    hdr->name[ASMA_NAME_MAX - 1] = '\0';

    if (find_by_name(hdr->name)) {
        kfree(buf);
        return &find_by_name(hdr->name)->desc;
    }

    loaded_app_t *slot = find_free_slot();
    if (!slot) {
        kfree(buf);
        quiet_fail("Too many dynamic apps loaded.");
        return NULL;
    }

    uint8_t *code = (uint8_t *)kmalloc(hdr->code_size);
    if (!code) {
        kfree(buf);
        quiet_fail("Out of memory for app code.");
        return NULL;
    }

    asma_header_t hdr_copy = *hdr;
    uint32_t code_off = hdr->code_offset;
    uint32_t code_sz = hdr->code_size;

    memcpy(code, buf + code_off, code_sz);
    kfree(buf);

    memset(slot, 0, sizeof(*slot));
    slot->hdr = hdr_copy;
    slot->code_base = code;
    slot->code_size = code_sz;
    slot->loaded = true;
    strncpy(slot->path, path, sizeof(slot->path) - 1);

    slot->desc.name = slot->hdr.name;
    slot->desc.state_size = slot->hdr.state_size;
    slot->desc.init = trampoline_init;
    slot->desc.on_frame = trampoline_frame;
    slot->desc.destroy = slot->hdr.entry_destroy ? trampoline_destroy : NULL;
    slot->desc.single_instance = (slot->hdr.flags & ASMA_FLAG_SINGLE) != 0;

    os_install_app(&slot->desc);
    s_loaded_count++;

    {
        char msg[160];
        snprintf(msg, sizeof(msg), "Loaded: %s\n(%u bytes)", slot->hdr.name,
                 (unsigned)slot->code_size);
        modal_show(MODAL_INFO, "Dynamic App", msg, NULL, NULL);
    }

    return &slot->desc;
}

app_instance_t *appload_launch(const char *path) {
    app_descriptor *desc = appload_load(path);
    if (!desc)
        return NULL;
    return os_launch_app(desc);
}

void appload_unload(app_descriptor *desc) {
    loaded_app_t *la = find_by_desc(desc);
    if (!la)
        return;

    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        if (running_apps[i].running && running_apps[i].desc == desc)
            return;
    }

    if (la->code_base) {
        kfree(la->code_base);
        la->code_base = NULL;
    }
    la->loaded = false;
    if (s_loaded_count > 0)
        s_loaded_count--;
}

app_descriptor *appload_find(const char *name) {
    loaded_app_t *la = find_by_name(name);
    return la ? &la->desc : NULL;
}

int appload_scan_dir(const char *dir_path) {
    (void)dir_path;
    return 0;
}

static bool try_one(const char *path) {
    if (!path || !path[0])
        return false;
    app_descriptor *desc = appload_load(path);
    if (!desc)
        return false;
    if (!os_launch_app(desc))
        return false;
    return true;
}

bool appload_try_open(const char *path_or_name) {
    if (!path_or_name || !path_or_name[0])
        return false;

    if (try_one(path_or_name))
        return true;

    char try[256];
    static const char *prefixes[] = {
        "/FDD0/",    "/FDD1/",   "/apps/",   "apps/", "/DESKTOP/",
        "/desktop/", "DESKTOP/", "desktop/", "/",     NULL};

    size_t len = strlen(path_or_name);
    int has_ext = (len >= 4 && (strcmp(path_or_name + len - 4, ".APP") == 0 ||
                                strcmp(path_or_name + len - 4, ".app") == 0));

    for (int i = 0; prefixes[i]; i++) {
        if (has_ext) {
            snprintf(try, sizeof(try), "%s%s", prefixes[i], path_or_name);
            if (try_one(try))
                return true;
        } else {
            snprintf(try, sizeof(try), "%s%s.APP", prefixes[i], path_or_name);
            if (try_one(try))
                return true;
            snprintf(try, sizeof(try), "%s%s.app", prefixes[i], path_or_name);
            if (try_one(try))
                return true;
            snprintf(try, sizeof(try), "%s%s", prefixes[i], path_or_name);
            if (try_one(try))
                return true;
        }
    }

    return false;
}
