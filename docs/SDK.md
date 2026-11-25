# ASMOS SDK

Everything you need to write apps for ASMOS, without the ASMOS source.
`make sdk` in the source tree makes it (`build/sdk/ASMOS-SDK.zip`, this
file its README.md):

```
include/asmos.h      the one header to include (it brings in the rest)
lib/libasmos.a       strings, numbers, memory, and the kernel-style calls
tools/mkapp.py       packs your program into an .APP
tools/mkiso.py       makes a CD image to ship it on
app.mk               the build: include it from your Makefile
docs/                the app API in detail
```

Apps built with it run on the ASMOS it was made for, or newer (the line
under the title says when); on an older one they say "This app needs a
newer ASMOS."

## What you need

- A cross compiler for 32-bit x86: `i686-elf-gcc` and `i686-elf-ld`
  (binutils + GCC; on macOS, Homebrew's `i686-elf-gcc`). On Linux the
  system's `gcc -m32` and `ld` work too.
- Python 3 (for mkapp.py).

No libc: apps are freestanding. What there is, is in `asmos.h`.

## Your first app

A folder with two files. `Makefile`:

```
# the file: HELLO.APP, and your C files
APP   = HELLO
SRC   = hello.c
# optional: help pages, and what it needs to run
HELP  = help
NEEDS = --min-cpu 486 --min-ram 4096
SDK  ?= /path/to/ASMOS-SDK
include $(SDK)/app.mk
```

(A comment on its own line: after a value, make would keep the spaces
before it.)

`hello.c`:

```c
#include "asmos.h"

typedef struct {
    window *win; /* must be first */
    control *name, *greeting;
} hello_t;

static void on_greet(control *c, int ev) {
    hello_t *s = c->win->userdata;
    char msg[64];
    if (ev != EV_CLICK && ev != EV_SUBMIT)
        return;
    snprintf(msg, sizeof(msg), "Hello, %s!", ctl_text(s->name));
    ctl_set_text(s->greeting, msg);
}

static void hello_init(void *state) {
    hello_t *s = state;
    s->win = app_window(s, "Hello", 60, 50, 200, 80, BLUE, 0);
    if (!s->win)
        return;
    ctl_label(s->win, 8, 8, 40, 12, "Name:", 0);
    s->name = ctl_edit(s->win, 48, 6, 144, 14, "", 32, 0, on_greet);
    ctl_button(s->win, 8, 28, 60, 14, "Greet", 1, on_greet);
    s->greeting = ctl_label(s->win, 8, 50, 184, 12, "", 0);
}

APP_META("Hello", hello_t, true, 0, hello_init, NULL, NULL);
```

Then:

```
make                            # build/HELLO.APP
make CC="gcc -m32" LD=ld        # on Linux
make cd                         # build/HELLO.iso: a CD with APPS/HELLO.APP
```

Copy `HELLO.APP` into ASMOS: into a folder of `/APPS` to have it in the
Apps menu, or open it from a floppy or a CD (in the CD drive, ASMOS shows
it in Apps > CDROM).

`app.mk` does the rest: compiles with the right flags, links with
`libasmos.a`, packs with `mkapp.py`. The name, state and heap come from
`APP_META`. An app with its own `init`/`on_frame`/`destroy` instead (the
app API only) sets `NAME`, `STATE` (bytes) and `HEAP` (KB) in its
Makefile.

## Writing an app

`APP_META(name, state type, one instance only, heap KB, init, frame,
destroy)`: ASMOS gives each instance its own zeroed state (its first
member: the window) and a heap (`kmalloc` / `kfree`, 64 KB unless you ask
for more, in 16 KB steps). `frame` is called every frame, about 60 times
a second: games and animations go there.

From `asmos.h` you can use what ASMOS's own apps use:

- windows and controls: `app_window`, `ctl_button`, `ctl_edit`,
  `ctl_list`, `ctl_check`, `ctl_combo`, `ctl_tabs`, `ctl_tree`,
  `ctl_custom` (draw it yourself)... and `list_*`, `tree_*`, `edit_*`
- dialogs: `dlg_message`, `dlg_confirm`, `dlg_input`, `dlg_open_file`,
  `dlg_save_file`, `dlg_print`, `dlg_color` (a colour picker)
- menus: `window_add_menu`, `menu_add_item`, `menu_add_separator`
- drawing: `fill_rect`, `draw_line`, `draw_string`, `draw_pixels`,
  `text_width`... colours `BLACK` .. `WHITE`, fonts `FONT_SYSTEM`...
- strings and numbers: `strlen`, `strcpy`, `strcmp`, `snprintf`,
  `str_to_int`, `path_join`,
  `isqrt`, `rand_next`, `memcpy`, `memset`...
- time and sound: `time_rtc_local`, `time_weekday`..., `speaker_beep_async`,
  `audio_open` / `audio_play` (WAV, MIDI, CD tracks)
- settings: `app_prefs_get_int`, `app_prefs_set_int`...
- .BMP pictures: `bmp_open`, `bmp_read_all` (or `bmp_start` / `bmp_rows` /
  `bmp_end`, a few rows a frame), `bmp_save`; the screen's colours with
  `A->screen_palette` (`lib/bmp.h`)
- the keyboard, mouse and joystick: `kb.down[...]`, `mouse.x`, `joy.axis[...]`

The rest of the app API is `A->...` (files: `A->file_read`,
`A->list_dir`...; see `docs/DYNAMIC_APPS.md`). A function declared in
these headers is one you can call: the SDK checks that when it is made.

## Help pages

Text files in your HELP folder go inside the .APP. The first time the app
runs, ASMOS puts them in its Help app and gives your window Help > View
Help. `# ` starts a heading, `- ` a point, a blank line a paragraph. Name
them `01_Getting_started.TXT` and so on (no spaces).

## What it needs to run

`NEEDS = --min-cpu 486 --min-ram 4096` (KB): ASMOS won't start the app on
less, and says why.

## Limits

Code and data up to 256 KB each, state up to 64 KB, heap up to 4 MB. No
floating point (a 386 or a 486SX has no FPU): use fixed point. Apps run
in the same memory as ASMOS: a crash ends your app, not the system, but be
tidy.

## More

- `docs/DYNAMIC_APPS.md`: the app API, size by size, and the .APP format.
  How the SDK is made: its "Build" section.
- `docs/UI_TOOLKIT.md`: windows, controls, dialogs, menus. (Written for
  ASMOS's own code too: what isn't in `asmos.h` isn't for apps.)
