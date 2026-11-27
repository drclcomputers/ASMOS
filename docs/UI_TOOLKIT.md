# ASMOS UI toolkit

Win32-style retained controls. Create them once; the window manager draws
them, routes mouse/keyboard, keeps focus, and frees them with the window.

Built-in apps call the `ctl_*` / `list_*` / `edit_*` / `dlg_*` functions
directly (`ui/controls.h`, `ui/dialogs.h`). Dynamic `.APP`s get the same
functions through `api->ui` (`os/appapi.h`) — names drop the prefix:
`ctl_button` → `api->ui->button`, `list_add` → `api->ui->list_add`,
`dlg_message` → `api->ui->message`.

## Minimal app

```c
typedef struct { window *win; /* first */ control *name, *out; } st_t;

static void on_go(control *c, int ev) {
    st_t *s = c->user;                 /* = state passed to app_window */
    ctl_set_text(s->out, ctl_text(s->name));
}

static void init(void *state) {
    st_t *s = state;
    s->win  = app_window(s, "Demo", 60, 60, 200, 90, BLUE, AW_RESIZABLE);
    s->name = ctl_edit(s->win, 40, 6, 152, 12, "", 20, CS_STRETCH_W, NULL);
    s->out  = ctl_label(s->win, 6, 26, 186, 10, "", CS_STRETCH_W);
    control *b = ctl_button(s->win, 132, 54, 60, 14, "Go", 0, on_go);
    ctl_style_add(b, CS_DEFAULT | CS_ANCHOR_RIGHT | CS_ANCHOR_BOTTOM);
}
```

No `destroy` needed for the window: closing it ends the instance.
Full external example: `testapps/hello.c` (also the FDD tool's template).

## Windows

| call                                                     | notes                                                                                                                                                                                                                                                                                                                                             |
| -------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `app_window(state, title, x, y, w, h, bar_color, flags)` | `AW_RESIZABLE`, `AW_HIDDEN`, `AW_NO_ANIM`, `AW_FIXED_POS`. Face colour, min size = 2/3, close = end instance. The first window an app makes while starting opens where the user last closed it (/PREFS/WINDOWS.INI); x, y are only the first time. `AW_FIXED_POS`: always at x, y. Off the screen, or bigger than it: moved on and shrunk to fit. |
| `win_client_rect(w, &x, &y, &cw, &ch)`                   | client area = `w-2` × `h-17`                                                                                                                                                                                                                                                                                                                      |
| `ui_active_state()`                                      | state of the focused app window (for menu callbacks)                                                                                                                                                                                                                                                                                              |
| `w->on_close`                                            | return `true` if handled. To ask about unsaved work, use `wm_ask_save`                                                                                                                                                                                                                                                                            |

Coordinates are relative to the client area. Font: 5 px per char, 8 px per
line (`TH_CHAR_W`, `TH_LINE_H`). Usual heights: button/edit 12–14, label 10.

**Save before closing.** Turn it on once, then say when there are changes:

```c
static bool save_changes(window *w) {   /* what File > Save does */
    my_t *s = w->userdata;
    if (!s->path[0]) {
        save_as(s);      /* a Save As dialog: it closes when that saves */
        return false;
    }
    return save_to(s, s->path);   /* calls wm_set_unsaved(w, false) */
}
...
wm_ask_save(s->win, save_changes);   /* in init */
wm_set_unsaved(s->win, true);        /* on an edit */
wm_set_unsaved(s->win, false);       /* once saved, or a file loaded */
```

While there are changes the title shows a `*`. The close box, File >
Close (call `wm_close(win)`) and Ctrl+Q then ask _Save the changes to ...
before closing?_ with Save, Don't save, Cancel. Save calls `save_changes`;
the window closes once `wm_set_unsaved(w, false)` has been called and no
dialog is open (so a cancelled Save As leaves it open). Shut Down and
Restart name the windows not saved. TEditor and ASMDraw use it.

Every window gets these from the window manager, with no app code:

- **WindowShade:** double-click the title bar to roll the window up to it,
  and again to unroll. A shaded window keeps its size and contents, and
  takes no clicks or keys below the title. `wm_shade(w, on)`, `w->shaded`.
- `wm_zoom(w)` toggles a resizable window between its size and the whole
  desktop (`w->zoomed`); there is no title-bar button for it.

## Fonts (fonts/fonts.h)

The last argument of `draw_string` / `draw_char` / `draw_text_n` /
`text_width` picks the font:

| size              | font                                 | used for                       |
| ----------------- | ------------------------------------ | ------------------------------ |
| `FONT_MONO8` (1)  | 8×8 fixed                            | big text                       |
| `FONT_MONO` (2)   | 4×6 fixed, 5 px per char             | text fields, editors, terminal |
| `FONT_SYSTEM` (3) | bold, proportional, 8 rows (2 below) | window titles, the taskbar     |
| `FONT_SMALL` (4)  | proportional, 7 rows (2 below)       | controls, menus, icons, tips   |
| `FONT_TERM` (5)   | the Small glyphs in fixed 6 px cells | ASMTerm, the text shell        |

Proportional text: measure with `text_width(s, font)`, cut with
`text_fit(s, pixels, font)` (how many chars fit). Desktop code uses
`UI_FONT_BOLD` / `UI_FONT_SMALL` / `UI_FONT_TERM` (ui/uifont.h). Codes
128-255 come from the code page picked in Settings → Display → Characters
(fonts/fonts_ext.c, made by `tools/mkglyphs.py`). The ASCII glyphs are art in
`tools/mkfont.py`; edit them there and run
`python3 tools/mkfont.py > fonts/font_prop.c`.

## Controls

| create                                     | events                                                                                   | value / extras                                                                                 |
| ------------------------------------------ | ---------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------- |
| `ctl_label(w,x,y,cw,ch,text,style)`        | –                                                                                        | `LS_CENTER`, `LS_RIGHT`, `LS_BIG`, `CS_BORDER`                                                 |
| `ctl_button(w,x,y,cw,ch,text,id,h)`        | `EV_CLICK`                                                                               | `CS_DEFAULT`, `CS_CANCEL`, `CS_TOGGLE` (value 0/1), `CS_FLAT`                                  |
| `ctl_check(w,x,y,cw,text,checked,h)`       | `EV_CHANGE`                                                                              | value 0/1                                                                                      |
| `ctl_radio(w,x,y,cw,text,group,checked,h)` | `EV_CHANGE`                                                                              | one per `group`; arrows move                                                                   |
| `ctl_edit(w,x,y,cw,ch,text,max,style,h)`   | `EV_CHANGE`, `EV_SUBMIT` (Enter, single-line), `EV_KEY` (Up/Down/PgUp/PgDn, single-line) | see below                                                                                      |
| `ctl_list(w,x,y,cw,ch,style,h)`            | `EV_CHANGE` (selection), `EV_SUBMIT` (Enter/double-click)                                | `LBS_HEADER`, columns, type-ahead                                                              |
| `ctl_combo(w,x,y,cw,h)`                    | `EV_CHANGE`                                                                              | items via `list_*`; value = index                                                              |
| `ctl_vscroll/ctl_hscroll(w,x,y,len,h)`     | `EV_CHANGE`                                                                              | `ctl_set_range(c,min,max,page)`                                                                |
| `ctl_slider(w,x,y,cw,min,max,val,h)`       | `EV_CHANGE`                                                                              |                                                                                                |
| `ctl_progress(w,x,y,cw,ch,max)`            | –                                                                                        | `ctl_set_value`                                                                                |
| `ctl_group(w,x,y,cw,ch,title)`             | –                                                                                        | frame only                                                                                     |
| `ctl_tabs(w,x,y,cw,"A\|B\|C",h)`           | `EV_CHANGE`                                                                              | `ctl_on_page(ctl, tabs, page)` shows ctl only on that page                                     |
| `ctl_custom(w,x,y,cw,ch,paint,h,style)`    | `EV_MOUSEDOWN/MOVE/UP`, `EV_DBLCLICK`, `EV_KEY`, `EV_FOCUS/BLUR`                         | `paint(c,x,y,w,h)`; `c->mx/my` relative; `CS_FOCUSABLE` for keys; mouse is captured while held |
| `ctl_status(w,text)`                       | –                                                                                        | bottom bar, full width; keeps clear of the resize grip; `"left\tright"`                        |
| `ctl_tree(w,x,y,cw,ch,style,h)`            | `EV_CHANGE` (selection), `EV_SUBMIT`, `EV_EXPAND` (add children now)                     | `TS_FOLDERS` icons; lazy branches; see below                                                   |

Every control also gets `EV_MENU` on right-click / Menu key / Shift+F10
(see Context menus).

Common: `ctl_set_text / ctl_text / ctl_set_value / ctl_value / ctl_show /
ctl_enable / ctl_focus / ctl_move / ctl_set_colors(fg,bg; 0xFF = theme) /
ctl_style_add / ctl_set_handler(c,h,user) / ctl_by_id / ctl_destroy`.

Layout styles: `CS_ANCHOR_RIGHT`, `CS_ANCHOR_BOTTOM`, `CS_STRETCH_W`,
`CS_STRETCH_H` (relative to the client size at creation / last `ctl_move`).
`CS_NOFOCUS` keeps toolbar buttons out of the Tab order.

### Edit

Single-line or `ES_MULTILINE`; scrolls horizontally/vertically, mouse
select/drag, double-click word, Shift+arrows, Ctrl+arrows (word),
Home/End, Ctrl+Home/End, PgUp/PgDn, Ctrl+A/C/X/V, auto-indent.
Filters: `ES_NUMBER`, `ES_DECIMAL`, `ES_UPPER`, `ES_FILENAME`,
`ES_PASSWORD`, `ES_READONLY`. Arrow keys never insert characters.
Helpers: `edit_select`, `edit_select_all`, `edit_insert`, `edit_line_of`,
`edit_col_of`, `edit_line_count`, `edit_goto`, `edit_find`,
`edit_copy/cut/paste`.

### Tree

`tree_add(c, parent, text, data, can_expand)` (parent -1 = root) returns a
node id. Branches added with `can_expand` show [+]; the first time one
opens the handler gets `EV_EXPAND` and adds the children
(`tree_event_node(c)` says which); no children → the [+] goes away.
`tree_sel`, `tree_set_sel` (opens ancestors), `tree_expand(c,n,open)`,
`tree_is_expanded`, `tree_clear`, `tree_clear_children` (reload a
branch), `tree_parent / tree_first_child / tree_next_sibling`,
`tree_text / tree_set_text / tree_data`.
Paths: `tree_path(c, node, buf, sz)` → `"/A/B"` from node texts;
`tree_find_path(c, "/A/B")` opens (loading) branches down to it.
Keys: arrows (Right opens / goes in, Left closes / goes to parent),
Home/End, PgUp/PgDn, type-ahead, Enter = `EV_SUBMIT`. Collapsing a branch
that holds the selection selects the branch (`EV_CHANGE`).

### List / combo

`list_add`, `list_add_data(c,text,u32)`, `list_set`, `list_remove`,
`list_clear`, `list_count`, `list_get`, `list_data`, `list_sel`,
`list_set_sel`, `list_set_columns(c, widths, n)` (items split on `\t`).

## Dialogs

All are non-blocking: they return at once and call back later. The owner
window is blocked while its dialog is open. Callback text `NULL` = cancel.

| call                                                                  | callback                                                            |
| --------------------------------------------------------------------- | ------------------------------------------------------------------- |
| `dlg_message(owner, title, msg)`                                      | – (`\n` splits lines)                                               |
| `dlg_confirm(owner, title, msg, cb, user)`                            | `cb(user, bool yes)`                                                |
| `dlg_ask(owner, title, msg, buttons, n, cb, user)`                    | `cb(user, int choice)`: 2 or 3 buttons, 0 the default, Esc the last |
| `dlg_input(owner, title, prompt, initial, max, edit_style, cb, user)` | `cb(user, text)`                                                    |
| `dlg_input_check(…, check_label, bool *checked, cb, user)`            | same; `*checked` is written before `cb` (keep it in your state)     |
| `dlg_open_file(owner, title, dir, ext, cb, user)`                     | `cb(user, path)`                                                    |
| `dlg_save_file(owner, title, dir, name, ext, cb, user)`               | `cb(user, path)` (asks before overwriting)                          |
| `dlg_color(owner, title, rgb, preview, cb, user)`                     | `cb(user, rgb)`, NULL if cancelled (0..63 each)                     |

`ext` may list several types: `"PIC,BMP"`. Open shows all of them; Save
has a Type list (the first chosen at the start): its files are shown, a
name typed without an extension (or with one not of the list) gets its
extension, and choosing another type changes the typed one. The colour
picker (`ui/colorpick.c`) has a field of hues, a lighter/darker bar, the 16
basic colours and Red/Green/Blue as numbers; `preview` is a palette index
(a custom colour, 248..255) that shows the colour exactly while it is
chosen and is put back on Cancel, or -1.

Kernel code that must not create windows (fault/error handlers) uses
`dlg_post_message(title, msg)`: it is shown at the start of the next frame.

## Menus

```c
menu *m = window_add_menu(s->win, "File");     /* .APP: api->ui->add_menu */
menu_add_item(m, "Open...", menu_open);        /* .APP: api->ui->menu_item */
menu_add_separator(m);
```

Built-in commands are `void fn(void)` and find their instance with
`ui_active_state()`. In a .APP, `menu_item` takes `void fn(void *state)`
and the OS passes your state (the call is guarded like control handlers).

## Context menus

```c
case EV_MENU: {                        /* in a control handler */
    menu *m = popup_menu();            /* the one shared popup, emptied */
    menu_add_item(m, "Copy", menu_copy);
    menu_enable_last(m, has_sel);      /* grey out */
    menu_add_separator(m);
    menu_add_item_ud(m, "Rename", do_rename, s);
    popup_show_for(c);                 /* at c->mx/my; or popup_show(x,y) */
}
```

Right-click focuses the window and control first; lists and trees select
the row under the pointer. From the keyboard (`ctl_menu_from_key()`),
`c->mx/my` point at the selection: act on the selection, not the point.
Text boxes have a built-in Cut/Copy/Paste/Delete/Select All menu.
The popup takes the mouse and keyboard while open: arrows, Home/End,
first letter, Enter/Space, Esc; a click outside closes it (a right-click
there opens that place's menu). Labels are stored as pointers (literals or
static buffers). It closes by itself if its window closes.

## Tooltips

`ctl_set_tooltip(c, "Up one folder")` (copied). Shown after the pointer
rests on the control ~0.6 s; a click or key hides it until the pointer
leaves. Good for icon / one-letter toolbar buttons.

## Saved settings (os/prefs.h)

`app_prefs_get_int("ASMDRAW", "pen", BLACK)`, `app_prefs_set_int(...)`,
`app_prefs_get/set` for strings (keys < 24, values < 96 chars). One file
per app: `/PREFS/<APP>.INI`, lines `key=value`. Read in init, write when
a setting changes or in destroy; several at once:
`app_prefs_begin(app)` … sets … `app_prefs_end()` (one write).
.APP: `api->prefs_get / prefs_get_int / prefs_set / prefs_set_int(key…)`
(the app's own name is used).

## File operations (ui/fileops.h)

Shared by the desktop and FileF, so both behave the same and refresh
each other. Paths are absolute: `/HDA/X` (`/X` and `/DESKTOP/X` imply
HDA), `/FDD0/X`, `/HDB/X`. Operations return `NULL` or an error text:
`fo_create(dir,name,is_dir,hidden)`, `fo_rename(path,new_name)`,
`fo_delete`, `fo_move(src,dst_dir)` (drag & drop; across drives = copy +
delete), `fo_set_hidden`, `fo_clip(path,cut)` / `fo_paste(dst_dir)`.
`fo_open(owner,path)` opens by type (folder → FileF, .APP → run, text →
TEditor, .WAV → play, .PIC → ASMDraw, else HexView).
Dialog helpers: `fo_ask_new / fo_ask_rename / fo_ask_delete /
fo_show_info / fo_report`. Each change calls `fo_changed(dir)`, which
reloads the desktop and any FileF window (and folder tree) showing it.
`fo_list_dirs(dir, names, max, hidden)` feeds folder trees.
Trash: deleting on HDA moves to the hidden, protected `/TRASH`
(`fo_trash`, `fo_put_back`, `fo_empty_trash`, `fo_ask_empty_trash`,
`fo_trash_count`); `fo_ask_delete` does this without asking, and asks
before deleting for good (inside the Trash, other drives). Original
folders are kept in `/PREFS/TRASH.INI`. The desktop shows the Trash at the
bottom right (drop items on it); its FileF window offers Put Back.
Search: `fo_match(name, pattern)` (`*`/`?` = whole-name pattern, else
"contains"), `fo_find(root, pattern, hidden, cb, user)` walks subfolders
(shell `find`; typing in a FileF window filters its icons, Esc clears).

## Busy cursor and hung apps (os/watchdog.h)

When the main loop stalls for 0.6 s (a long job, or an app in a loop) the
timer interrupt shows an hourglass that follows the mouse and still reads
the keyboard: Win+Shift+Q / Ctrl+Shift+Q ends the stuck app, Ctrl+Alt+Win
/ Ctrl+Alt+Del stops it and opens Monitor (listed NOT RESPONDING: End Task
ends it, Switch To lets it run again). Call `wd_busy()` before a long job
to show the hourglass at once. App code (on_frame, handlers, menu
commands, painting; built-in apps and .APPs) runs guarded, so a crash or
a hang ends that app, not the OS.

## Overlay

`wm_set_overlay(fn, user)` draws `fn(user)` above every window each
frame (one at a time; FileF uses it for the dragged icon).
`wm_clear_overlay(user)` removes it.

## Lazy redraw (ui/window.h)

The back buffer is kept between frames, and a frame only draws what
changed:

- **Full redraw** when the scene changes: a window opens, closes, moves,
  resizes, shades or animates; on any click, button release or key (and
  two frames after); while a menu, context menu, combo list, tooltip or
  drag overlay is up; when the desktop icons or wallpaper change.
- **Otherwise** only windows whose content changed are drawn, plus any
  window lying on top of one that is. Toolkit controls are checksummed
  each frame, so `ctl_set_text` and friends need nothing extra.
- **Nothing at all** is drawn or sent to the VGA when nothing changed.

Custom-drawn content (`ctl_custom`, `on_draw`) cannot be checksummed, so
such a window is drawn every frame unless the app sets
`win->lazy_custom = true`. It then promises that the content only changes
inside control handlers and menu commands (those redraw automatically),
or else calls `wm_invalidate(win)`: from `on_frame` timers, background
tasks, or from the painter itself to animate. `.APP` programs have no
call for this yet, so their custom-drawn windows are drawn every frame.

With Settings → Speed → "Show windows while moving and resizing" off
(`g_cfg.outline_drag`), a move or resize only XORs a 2-pixel frame into
the back buffer (`w->band`, `w->bx..bh`); the window is applied and
redrawn once on release.

`wm_invalidate_all()` forces a full redraw (anything that draws on the
back buffer outside the frame must call it). The pointer is drawn with
save-under (`cursor_show` / `cursor_hide`).

## Screen saver (ui/saver.h)

After `g_cfg.saver_minutes` with no key or mouse movement the main loop
draws a saver instead of the desktop: Starfield, Mystic lines, Rain, or a
random one (`g_cfg.saver_effect`). Apps keep ticking. The input that
wakes it is swallowed. `saver_start()` starts it now;
`saver_paint(x, y, w, h, effect)` draws one frame into a rectangle (the
Settings preview).

## Screen size

`app_window` shrinks/moves a window that does not fit the screen before
it is first drawn; anchored/stretched controls follow. Design for
640×400 and use `CS_STRETCH_*` so 320×200 still works.

## Keyboard summary

Tab / Shift+Tab focus · Enter default button · Esc cancel button ·
Menu key or Shift+F10 context menu · Ctrl+Q close · Win+Shift+Q or
Ctrl+Shift+Q end the app at once · Ctrl+Alt+Win, Ctrl+Alt+Del or
Ctrl+Shift+Esc Monitor ·
Space presses focused button/check · unused keys reach the app's
`on_frame` (`kb.key_pressed` is cleared for keys a control consumed).

## Dynamic apps

- Check `api->size >= sizeof(os_api_t)` before using `api->ui`.
- Handlers run through the app guard: a crash inside one closes the app,
  not the OS.
- Menus: `api->ui->add_menu / menu_item / menu_separator` (only on your
  own windows).
- Newer members (context menus, tooltips, tree view, `edit_select_all`,
  `status`): check `api->ui->size >= sizeof(ui_api_t)` first; older
  kernels lack them. Same for `api->prefs_*`: `api->size`.
- Memory: each running instance has its own heap (64 KB + its state;
  `api->kmalloc/kfree` work there and everything is freed when it ends).
  With paging on, an app can reach only its own image and heap of the
  app pool, not other apps; NULL pointers and writes to kernel code
  (486+) fault and close just that app. Kernel data stays reachable
  (apps run in ring 0 and call the kernel directly).
- Context menu: on `EV_MENU`, `m = api->ui->popup_menu()`, add items with
  `menu_item(m, "Copy", fn)` (`fn(state)`), `menu_enable(m, on)` for the
  last one, `menu_separator(m)`, then `popup_show_for(c)`. Labels must stay
  valid while the menu is open (use literals). Text boxes have their own.
- `api->ui->set_tooltip(c, "text")`.
