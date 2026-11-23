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
- Shell `mem` shows the kernel heap, the app pool and the paging state.
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
each, 0..63) and returns how many it has: 256 in 320x200 (the BIOS's mode
13h palette: 16..31 greys, then rings of hues, 248..255 black), 16 in the
other modes, where an index above 15 shows as `index & 15`. A game that
draws its own pictures maps its colours to the nearest ones once at start
(and with 16 can dither between two of them).

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
app with big buffers (ASMDraw's canvas). Default 64 KB.

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

FileF opens .PIC files in ASMDraw, WAV/MID in Audio Player, others in
HexView: the .APP of that name in /APPS (any folder), with open_request.

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
