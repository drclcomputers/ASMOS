# ASMOS dynamic apps (.APP, format v2)

Install = copy `NAME.APP` to `/apps` (scanned at boot, or `app scan`) or
`/FDD0`. No kernel rebuild. Shell: `app run|scan|list|unload`.

## Format

See header comment in `os/appload.h`. v2 adds relocation table + BSS.
v1 packages still load (entry ABI `fn(state)`, no relocs/BSS).

v2 entry ABI: `void init/on_frame/destroy(const os_api_t *api, void *state)`.
Include `os/appapi.h` only; call kernel via `api->...`.
Entry offset 0 is valid. `destroy` absent = `ASMA_NO_ENTRY`.

## Build

With the SDK (`make sdk`: build/sdk/ASMOS-SDK.zip, for developers without
the source tree): `include $(SDK)/app.mk` from a short Makefile, see
docs/SDK.md (the SDK's README). `tools/mksdk.py` makes it: the headers the
bundled apps use (their kernel functions an app can't call taken out),
`libasmos.a` (the same objects the bundled apps link: extapps/shim.c and
shim_api.c, lib/string.c, math.c, mem.c), mkapp.py, mkiso.py, app.mk
(written by mksdk.py) and these docs. It checks that everything the
headers still declare links; to give apps another kernel function, wrap
its API call in extapps/shim_api.c. A test (`sdkapp`) builds
tests/sim/fixtures/sdkapp with the SDK alone and runs it.

By hand, with the source tree:

    i686-elf-gcc -m32 -march=i386 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -O2 -nostdlib -I. -c app.c -o app.o
    i686-elf-ld  -m elf_i386 -N -Ttext 0 -nostdlib --emit-relocs -o app.elf app.o
    python3 tools/mkapp.py --elf app.elf --name MyApp --out MYAPP.APP --state 64 --single --window-first

or `testapps/build_hello.sh`.

`--window-first`: state's first member is `window *`. Required for
single-instance focus, Task Monitor switch/end-task, window close.
Without it the state is opaque (never dereferenced by the OS).

## Help pages

`--help-dir DIR` packs the text files in DIR into the .APP (after the
relocations, with a trailer: page count, size, `ASHP`). The first time the
app runs, ASMOS copies them to `/HELP/<App name>/` (spaces in the name
become `_`) and the app's window gets **Help > View Help** (added to its
own Help menu if it has one). They are copied again only when the .APP
file changes (installed again, updated), so pages the user deleted stay
deleted. The bundled apps take theirs from `extapps/help/<name>/`.

A page: `# ` starts a heading, `- ` a point in a list, a blank line a new
paragraph; the lines of a paragraph run together. Name pages like
`01_Getting_started.TXT` (shown as "Getting started", in that order):
letters, digits and `. - _ # $ % & + = @ ! ~ ( ) [ ] { }`, no spaces.

## Loader guarantees

- Header/size/offset checks use 64-bit math (no overflow).
- state 4..64K, code <=256K, BSS <=256K, relocs <=32768, name printable.
- Image allocated once (code+BSS, BSS zeroed), relocated in place; in the
  app frame pool when it has room (page aligned), else the kernel heap.
- Each instance: own heap (state + 64 KB, 16 KB if the pool is short) for
  `api->kmalloc/kfree`; freed with the instance. `api->kfree` ignores
  pointers that are not from that heap, and the state itself.
- Name clash with built-in or loaded app -> rejected.
- Changed file on disk (size/mtime) -> hot-reload when idle; if running, old copy kept.
- Crash (#DE/#UD/#GP/#PF inside app code) -> app killed, window closed, modal with EIP offset. OS survives.
- Failed loads show a reason; `appload_last_error()`.

## Limits

- Ring 0. With paging on (default; Settings > System can turn it off) each
  instance runs in its own page directory: page 0 unmapped, other apps'
  pool memory unmapped, kernel code/constants read-only (486+; a 386
  ignores write protection in ring 0). Wild writes into kernel _data_
  (heap, globals) are still not caught: the API is called directly.
- Shell `mem` shows where the RAM goes (os/meminfo.h: the kernel, its
  heap, the screen, the apps, free, video memory and the BIOS), the kernel
  heap, the app pool and the paging state. Monitor and About OS show the
  same, video memory and the BIOS not counted as used.
- An infinite loop shows the hourglass after 0.6 s; Win/Ctrl+Shift+Q ends
  the app, Ctrl+Alt+Del / Ctrl+Alt+Win stops it and opens Monitor. Loops
  with interrupts disabled can't be stopped.
- No FPU (386, 486SX): x87 instructions are emulated (lib/fpu_emu.c, all
  387 instructions, same results as a coprocessor, much slower).
- Faults on kernel stack overflow are not recoverable.

## Speed (API size 4)

Check `api->size >= sizeof(os_api_t)` (or `offsetof` the member) first.

- Windows with custom drawing (`ui->custom` painters, `on_draw`) are
  redrawn every frame. If yours only changes in handlers, menu commands or
  dialogs, call `api->set_lazy(win, true)` once after creating it: it is
  then drawn only when it changes, after input, or after
  `api->invalidate(win)`. Content that changes in `on_frame` or on a timer
  needs `api->invalidate(win)` there, or the window shows old pixels until
  the next click or key.
- Fast drawing: `draw_pixels` (copy a w*h block, row pitch `stride`, clipped)
  beats `draw_dot` loops by far; `mem_copy`/`mem_set` are `rep movsd/stosd`.
  `text_width`, `font_height`, `draw_text_n` for layout.
- `speed_flags()` returns the user's Settings > Speed choices (`SPEED_*`):
  skip animations on `SPEED_NO_ANIM`, do less work on `SPEED_LOW_END`.
  `frame_ms()` is the frame time (16/33/50 ms) for frame-based timing.

## Submenus (API size 5)

```c
menu *game = api->ui->add_menu(win, "Game");
if (api->size >= sizeof(os_api_t)) {          /* a size-5 kernel */
    menu *lvl = api->submenu(game, "New Game");
    api->ui->menu_item(lvl, "Easy", m_easy);
    api->ui->menu_item(lvl, "Hard", m_hard);
}
```

The item shows an arrow; hovering or clicking it opens the submenu to the
side. One level only: `submenu` returns NULL inside a submenu, in a context
menu (`popup_menu`), or when the menu is full (16 items). Submenus are
freed with their window.

## Files and folders (API size 6)

Names are long (up to 63 characters, spaces allowed: `"My Save 1.sav"`),
paths up to 511 (`char path[APP_PATH_MAX + 1]`). Every call takes the long
or the 8.3 name, in any case.

```c
app_dirent_t e[32];
int n = api->list_dir("/FDD0/My Games", e, 32);   /* -1: no such folder */
for (int i = 0; i < n; i++)
    if (!(e[i].attr & APP_ATTR_DIR))
        api->ui->list_add(lst, e[i].name);       /* e[i].dos_name: 8.3 */
int all = api->list_dir("/FDD0/My Games", 0, 0); /* just count */
```

- `list_dir` gives entries in disk order (sort them yourself), without
  `.` and `..`; `name` is the long name if there is one.
- `file_info(path, &e)` one entry; `make_dir(path)`; `remove(path)` a file
  or an empty folder; `rename(path, "New Name.txt")` (a name, not a path).
  Protected items (`/DESKTOP`, `/TRASH`, drives) are refused.
- Changes made this way, and by `file_write`, show at once in FileF and on
  the desktop.

Example: `testapps/dirlist.c` (DirList: browse folders).

## API size 7: what the bundled apps needed

`active_state()`, `open_request(path, sz)` (a file FileF asks you to open:
call it in init and on_frame), `screen_size`, `window_focused`,
`focused_control`, `classic_fonts`, `file_read_at` / `file_append` (big
files in parts), `print(owner, &doc)` (the print dialog), and three
sub-tables: `time` (local time, calendar maths, the desktop alarm), `sound`
(beep, one WAV/MIDI at a time, volume), `clip` (the text clipboard).

## API size 9: the screen's colours

`screen_palette(rgb, max)` fills `rgb` with the screen's colours (6 bits
each, 0..63) and returns how many it has: 256 in the 256-colour modes (the
BIOS's mode 13h palette: 16..31 greys, then rings of hues; 248..255 the
custom colours of size 10), 16 in the others, where an index above 15 shows
as `index & 15`. A game that draws its own pictures maps its colours to the
nearest ones once at start (and with 16 can dither between two of them).

## API size 10: custom colours

`set_screen_color(i, r, g, b)` changes one of the 8 colours from
`APP_COLOR_CUSTOM` (248) to 255 (values 0..63). They start white and are
the same for every window (ASMDraw's custom colours are these), so a viewer
of other pictures leaves them out when it looks for the nearest colour. It
returns true if the screen shows them now (a 256-colour screen); on a
16-colour screen the colour is still kept, for a picture's file, but
indexes above 15 show as `index & 15`.

`palette256(rgb, max)` gives all 256 colours whatever the screen, to turn a
picture made on a 256-colour screen into its nearest 16.

## API size 11: .BMP pictures

`api->bmp` is the kernel's `lib/bmp.h` (where it is explained): reading
Windows and OS/2 .BMPs of 1 to 32 bits a pixel, RLE too, into indexes of
the screen's colours (the nearest, or dithered), and saving 4- or 8-bit
ones. In an app the same names call it (extapps/shim.h):

```c
bmp_info_t *bi = kmalloc(sizeof *bi); /* (800 bytes: not on the stack) */
const char *why = bmp_open("/HDA/SUNSET.BMP", bi);
uint8_t pal[256][3];
int n = A->screen_palette(pal, 256);
bmp_target_t t = {(const uint8_t (*)[3])pal, n > 248 ? 248 : n, 0, true};
uint8_t *pix = why ? NULL : kmalloc(bi->w * bi->h);
if (pix)
    why = bmp_read_all("/HDA/SUNSET.BMP", bi, &t, pix, bi->w, bi->h, bi->w);
```

For a big picture, `bmp_start` then `bmp_rows(r, n)` from `on_frame` (it
returns the rows done; `bmp_error(r)` says why it stopped), then
`bmp_end(r)`: BMP Viewer fills its window that way. The reader's memory
(`bmp_work_size`: about 40 KB for 24 bits) comes from the app's own heap.
`n` 248 leaves out the 8 custom colours, which other apps change.
`bmp_save(path, pix, w, h, stride, pal, 16 or 256)` writes one.

Also in API size 11, the UI table's size 4: `api->ui->color`, the colour
picker (`dlg_color` in an app; docs/UI_TOOLKIT.md).

## API size 12: pictures on the clipboard, the pointer

The clipboard (`api->clip`, os/clipboard.h; its new members need API
size 12) holds a picture as well as text:
`clipboard_set_image(w, h, pix, stride, pal)` copies one (a byte a pixel,
`pal` its 256 colours, 6 bits each; at most `CLIP_IMAGE_MAX`, 320 KB) and
`clipboard_image()` gives it back (`w`, `h`, `pix`, `pal`), or NULL. `clipboard_kind()` says which it holds:
`CLIP_TEXT`, `CLIP_IMAGE`, `CLIP_FILE` (FileF's) or `CLIP_EMPTY`. Map the
picture's colours to your own before using them: ASMDraw keeps an index
whose colour is the same, else takes the nearest.

The scrapbook is the clipboard's history, the last 20 things copied (and
at most 384 KB of them: a new big picture pushes the oldest picture out
first): `clipboard_history_count()`, `_kind(i)`, `_get_text(i)`,
`_image(i)` (0 the oldest), `_use(i)` (on the clipboard again),
`_remove(i)`, `_clear()`. ClipView shows it.

Pasted into a folder (FileF, the desktop), a picture becomes a new
PICTURE.BMP there.

`api->pointer(shape)` changes the mouse pointer for this frame:
`APP_POINTER_ARROW`, `HAND`, `CROSS`, `TEXT` (the I-beam), `MOVE` (four
arrows), `SIZE_WE`, `SIZE_NS`, `SIZE_NWSE`, `SIZE_NESW` (the resize
arrows). Call it from `on_frame` while it should show; it is taken only
from the app in front, with the mouse over its window, and not while a
menu is open. ASMDraw does it over its selection; `lib/selbox.h` (in the
apps' library) is that selection box, for any app: docs/SDK.md.

## API size 13: asking to save before closing

`api->ui` (its size 5) gains `ask_save`, `set_unsaved`, `close` and `ask`:
in an app `wm_ask_save(win, save)`, `wm_set_unsaved(win, bool)`,
`wm_close(win)` and `dlg_ask(...)` (docs/UI_TOOLKIT.md, "Save before
closing"). With it on and changes not saved, closing the window asks
Save / Don't save / Cancel; the title shows a `*`. File > Close should
call `wm_close(win)` (not `os_quit_app_by_desc`) so that it asks too.

## API size 14: the network, glyphs, the code page

`api->net` (network/net.h, network/dns.h; in an app the same names):
DNS and TCP, none of which waits, so an app's window stays alive while a
site answers.

```c
int h = dns_start("frogfind.com");   /* then each frame: */
uint8_t ip[4];
int r = dns_poll(h, ip);             /* 1 found, 0 still asking, -1 not */
int sock = tcp_open(ip, 80, 0);      /* then watch tcp_state(sock): */
                                     /* TCP_ESTABLISHED, or tcp_was_reset */
tcp_write(sock, req, n);             /* what fits now */
tcp_recv(sock, buf, sizeof buf);     /* what came; 0: nothing yet */
tcp_release(sock);                   /* closes in the background */
```

`net_ready()` says if there is a card and an address. An app reaches only
its own connections; any it leaves open are cut when it ends (even if it
crashed). `tcp_open` sends the SYN and returns; the ARP for a new address
may wait a moment.

`api->font_glyph(size, c, rows, &w)`: a character's glyph (rows, the left
column in bit 7) and ink width; the advance is w + 1 in the proportional
fonts. World View measures its pages with it, and draws headings twice as
big. `app_codepage()`: the code points of bytes 0x80..0xFF in the code page
in use, for showing Unicode text (or sending it: World View's forms go out
as UTF-8).

## An app of several files

`extapps/<name>/*.c`, linked into one `<name>.APP`: put `<name>` in
EXT_MULTI (makefile). World View (`extapps/worldview/`) is one.

## What an app needs: --min-cpu, --min-ram

`mkapp.py --min-cpu 486 --min-ram 4096` (KB) writes the least CPU and RAM
into the header (4 more bytes: 80 the CPU family, 82 the RAM in 64 KB
units). ASMOS checks them before loading anything and says what is
missing, for example "Outpost needs a 486 and 4 MB of RAM. This computer:
a 386, 2 MB." 256 KB less than asked still counts (some BIOSes keep it).
An older ASMOS skips the 4 bytes and starts the app anyway.

## Keys held down (games)

`api->kb` (`kb` in extapps/shim.h) also has `down[128]`: every key held
down now, by scan code (drivers/keyboard.h: `UP_ARROW`, `A_KEY`, `SPACE`...).
A game reads it each frame: `if (kb.down[UP_ARROW]) walk forward`. It stays
set while the key is held and clears when it comes up; the grey arrows and
the number pad share codes. `last_char` / `key_pressed` stay what they were:
one key press, for one frame. Check `window_is_focused(s->win)` first: the
keys are held for whichever window is in front.

## Joystick

`api->joy` (`joy` in extapps/shim.h; the kernel's `drivers/joystick.h`) is
the stick on the game port, refreshed once a frame before the apps run:

| field        | meaning                                                  |
| ------------ | -------------------------------------------------------- |
| `present`    | a stick answered; false when none is plugged in          |
| `axes`       | bit mask of the axes that answer (`0x03` a plain stick)  |
| `axis[0..3]` | X, Y, then Z, R: -100 to 100, 0 centred, calibrated      |
| `buttons`    | bit n: button n+1 held                                   |
| `pressed`    | bits that went down since the last frame (for one frame) |

```c
if (joy.present) {
    if (joy.axis[0] < -30) turn_left();
    if (joy.buttons & 1)   fire();
}
```

The values are already calibrated (Settings > Devices > Joystick: centre,
edges, dead zone, reversed axes), so a game only compares them. `joy` was
added to the end of the API: an app that must also run on an older kernel
checks that `api->size` reaches it.
Like the keyboard, it is the same for every window: read it only when
`window_is_focused(s->win)`.

## A bigger heap

`mkapp.py --heap 272` (KB; header flags bits 8-15, 16 KB units): for an
app with big buffers. Default 64 KB.

`--heap-screen` (header flag bit 2; in APP_META: `APP_HEAP_PLUS_SCREEN |
128`): that much and one screen's pixels more (width x height bytes), for a
canvas as big as the screen. ASMDraw asks for 128 KB and a screen: 191 KB
at 320x200, 597 KB at 800x600. An older ASMOS refuses the flag ("Unknown
.APP flags").

## Bundled apps (extapps/)

Calculator, Calendar, Clock, ClipView, ASMDraw, Puzzle, Audio Player and
HexView were built into the kernel; now they are .APPs, built by `make`
and put in /APPS on the disk image (extapps/MANIFEST, tools/mkhdimg.py).
Their code is the old built-in code: `extapps/shim.h` defines the kernel
calls they make on top of the API. To add one: `extapps/x.c` ending in

```c
APP_META("Name", x_state_t, single_instance, heap_kb, x_init, x_frame, x_destroy);
```

then `x` in EXT_APPS (makefile) and a line in extapps/MANIFEST.

FileF opens .PIC files in ASMDraw, WAV/MID in Audio Player, .BMP in BMP
Viewer, .HTM, .GIF and .JPG in World View, others in HexView: the .APP of
that name in /APPS (any folder), with open_request.

## The Apps menu is the /APPS folder

No installer: the menu mirrors the folder and follows it (rebuilt within a
second of any change).

Every folder in /APPS is a group, named as the folder (long names), or by
the first line of its `GROUP.TXT`; groups and items are sorted by name.

| In /APPS                                 | It appears in                              |
| ---------------------------------------- | ------------------------------------------ |
| `X.APP`, `X.LNK`                         | Apps, directly (under the groups)          |
| `Accessories/`                           | Accessories: Calculator.APP... TEditor.LNK |
| `Games/`, `Multimedia/`, `System Tools/` | those groups                               |
| `Acme Software/`                         | a group of its own                         |

The built-in apps (FileF, Settings, TEditor, ASMTerm, Monitor, Format
Disk) are in it as shortcuts, so any group can mix them with .APPs, and a
group is just a folder. If /APPS is deleted, it is made again at boot with
those shortcuts (the bundled .APPs come with the disk image). Print Queue
and Joystick have none: they open from Settings (Printers, Devices); `.LNK`
files and `app run` still find them.

A shortcut is a `.LNK` text file: line 1 the target, an app's name
(`TEditor`, or `Calculator`: an .APP in /APPS by its header name) or a path
(`/FDD0/GAME.APP`); line 2 (optional) the label. `write
"/APPS/Acme Software/Calc.LNK" Calculator` makes one; deleting it leaves
the app. Opening a .LNK in FileF runs its target.

The menu shows the name from the app's header. Files that aren't .APPs
are left out; deeper folders are not looked into. To remove an app or a
whole group, delete the file or the folder (`rm -r`).
