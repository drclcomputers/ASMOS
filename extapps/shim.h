#ifndef EXTAPPS_SHIM_H
#define EXTAPPS_SHIM_H
/* Compiles built-in app sources as .APPs: shim.c maps kernel calls to the
 * app API, while string/math helpers are linked in. APP_META(name, state,
 * single_instance, heap_kb, init, on_frame, destroy) declares metadata for
 * tools/mkapp.py --meta. The state's first member must be its window *;
 * heap_kb 0 = 64 KB.
 *   APP_META("Puzzle", puzzle_state_t, true, 0, puzzle_init, NULL, NULL); */
#include "os/appapi.h"

#include "drivers/opl2.h"
#include "drivers/sb16.h"
#include "drivers/speaker.h"
#include "fs/fs.h"
#include "lib/audio.h"
#include "lib/core.h"
#include "lib/graphics.h"
#include "lib/math.h"
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/time.h"
#include "os/clipboard.h"
#include "os/os.h"
#include "os/prefs.h"
#include "ui/ui.h"
#include "ui/uifont.h"

extern const os_api_t *A; /* the API, set on every call into the app */

typedef struct {
    char name[32];
    uint32_t state_size, single, heap_kb;
    void (*init)(void *), (*frame)(void *), (*destroy)(void *);
} app_meta_t;

#define APP_META(nm, type, single, heap_kb, fi, ff, fd)                        \
    const app_meta_t app_meta = {nm, sizeof(type), single, heap_kb, fi, ff, fd}

/* kernel globals the apps read: through the API. (So `kb` and `mouse` are
 * macros: write kb.ctrl or &kb, never api->kb, which they would expand.) */
#undef UI_FONT_BOLD
#undef UI_FONT_SMALL
#undef UI_FONT_TERM
#define UI_FONT_BOLD (A->classic_fonts() ? FONT_MONO : FONT_SYSTEM)
#define UI_FONT_SMALL (A->classic_fonts() ? FONT_MONO : FONT_SMALL)
#define UI_FONT_TERM (A->classic_fonts() ? FONT_MONO : FONT_TERM)
#define kb (*A->kb)
#define mouse (*A->mouse)
#define joy (*A->joy)
#define pit_ticks (A->time_millis())
int shim_screen_w(void), shim_screen_h(void);
#define g_screen_width (shim_screen_w())
#define g_screen_height (shim_screen_h())
#define g_clip_history_current (A->clip->history_current())

/* file the OS asks the app to open (FileF double-click); see open_request */
bool app_open_request(char *path, int sz);

#endif
