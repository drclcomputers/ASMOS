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
for more, in 16 KB steps; `APP_HEAP_PLUS_SCREEN | 128` is 128 KB and one
screen's pixels more, for a canvas). `frame` is called every frame, about 60 times
a second: games and animations go there.

From `asmos.h` you can use what ASMOS's own apps use:

- windows and controls: `app_window`, `ctl_button`, `ctl_edit`,
  `ctl_list`, `ctl_check`, `ctl_combo`, `ctl_tabs`, `ctl_tree`,
  `ctl_custom` (draw it yourself)... and `list_*`, `tree_*`, `edit_*`
- dialogs: `dlg_message`, `dlg_confirm`, `dlg_ask` (2 or 3 buttons),
  `dlg_input`, `dlg_open_file`, `dlg_save_file`, `dlg_print`, `dlg_color`
  (a colour picker)
- "save the changes?" on closing, built in: `wm_ask_save(win, save)` once,
  then `wm_set_unsaved(win, true/false)`; File > Close calls
  `wm_close(win)` (docs/UI_TOOLKIT.md)
- menus: `window_add_menu`, `menu_add_item`, `menu_add_separator`,
  `menu_enable_last`; right-click ones: `popup_menu`, `popup_show_for`
- the clipboard: `clipboard_set_text`, `clipboard_set_image` (a picture),
  `clipboard_kind`, `clipboard_text`, `clipboard_image`, and the
  scrapbook's `clipboard_history_*` (`os/clipboard.h`)
- the mouse pointer and a selection box: `A->pointer(APP_POINTER_...)`,
  `selbox_*` (below)
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

## The pointer, and selections

`A->pointer(shape)` changes the mouse pointer for one frame: call it from
`frame` while it applies. It shows only while your window is in front, the
mouse over it, and no menu open. The shapes (`os/appapi.h`):

| `APP_POINTER_...` | looks like              | for                       |
| ----------------- | ----------------------- | ------------------------- |
| `ARROW`           | the arrow               | (the usual)               |
| `HAND`            | a hand                  | a link, something to push |
| `CROSS`           | a cross                 | aiming, drawing           |
| `TEXT`            | the I-beam              | text to type or pick      |
| `MOVE`            | four arrows             | something to drag around  |
| `SIZE_WE`         | left-right arrow        | a width to change         |
| `SIZE_NS`         | up-down arrow           | a height to change        |
| `SIZE_NWSE`       | top left - bottom right | a corner to drag          |
| `SIZE_NESW`       | top right - bottom left | the other corners         |

`lib/selbox.h` is a selection box like ASMDraw's: dragged out, moved by
its inside, resized by its edges and corners (Shift keeps its shape),
drawn as a dotted frame with 8 handles, and the right pointer for each
spot. It works in your own units (a picture's pixels, a grid's cells):

```c
typedef struct { window *win; control *view; selbox_t box; } my_t;

static void on_view(control *c, int ev) {   /* a ctl_custom */
    my_t *s = c->win->userdata;
    int x = c->mx, y = c->my;               /* the control's pixels */
    if (ev == EV_MOUSEDOWN)
        selbox_down(&s->box, x, y);
    else if (ev == EV_MOUSEMOVE && selbox_drag(&s->box, x, y, kb.shift))
        wm_invalidate(s->win);
    else if (ev == EV_MOUSEUP)
        selbox_up(&s->box);
}

static void paint(control *c, int x, int y, int w, int h) {
    my_t *s = c->user;
    fill_rect(x, y, w, h, WHITE);
    selbox_draw(&s->box, x, y, 1, x, y, w, h);  /* zoom 1, clipped */
}

static void frame(void *state) {             /* the pointer over it */
    my_t *s = state;
    control *c = s->view;
    int p = selbox_pointer_at(&s->box, mouse.x - c->ax, mouse.y - c->ay);
    if (p >= 0 && window_is_focused(s->win))
        A->pointer(p);
}
```

`selbox_down` returns what it grabbed: `SB_INSIDE` (moving), an edge or
corner (`SB_N`... `SB_SE`: resizing), or `SB_NONE` (a new box, the old one
gone: if it carried something, `selbox_hit` first says so). `box.on`
says if there is one; `selbox_set` and `selbox_clear` make one or drop it.

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
