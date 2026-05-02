═══════════════════════════════════════════════════════════════════════════
  ASMOS v0.9.5 - COMPLETE ARCHITECTURE DOCUMENTATION
═══════════════════════════════════════════════════════════════════════════

Last updated to reflect: config/config.h VER "0.9.6"
Covers: boot, kernel, HAL, memory, filesystem, scheduler, windowing,
        shell, assembler, runtime config, and all bundled applications.


1. BOOT PROCESS - FROM POWER-ON TO KERNEL
═══════════════════════════════════════════════════════════════════════════

STAGE 1: BIOS → BOOTLOADER (boot.asm)
--------------------------------------
Physical Address: 0x7C00 (31 KB mark in RAM)
Size Limit: 512 bytes (1 sector, ends with 0xAA55 signature)
CPU Mode: Real Mode (16-bit)
Layout: FAT16 BPB embedded in boot sector (bytes 3–61)

1. BIOS loads MBR from sector 0 to 0x7C00.
2. Bootloader executes:
   - INT 0x10 AH=0x00 AL=0x13  →  VGA mode 13h (320×200, 256 colours)
   - Zero segment registers: DS=ES=SS=0, SP=0x7C00
   - Hardcode boot drive to 0x80 (stored at [BOOT_DRIVE])

3. LBA Extension check (INT 0x13 AX=0x4100):
   - Verify BX returns 0xAA55; if not, print "No LBA support" and halt.

4. LBA Disk Load (Extended INT 0x13 AH=0x42 with DAP):
   - Loads SECTORS_TO_LOAD = 1000 sectors (512 KB) starting at LBA 1
   - Destination: segment 0x0800:0x0000  →  physical 0x8000
   - Reads in chunks of up to 127 sectors per call (BIOS limit)
   - Each chunk advances LBA counter and segment pointer by 32 per sector
     (32 paragraphs × 16 bytes = 512 bytes per sector)

5. Protected Mode Transition:
   a) CLI – disable interrupts
   b) LGDT [gdt_descriptor]:
      - Null descriptor        0x0000000000000000
      - Code segment  (0x08)   Base=0, Limit=4 GB, 32-bit, Execute/Read
      - Data segment  (0x10)   Base=0, Limit=4 GB, 32-bit, Read/Write
   c) Set CR0.PE (bit 0)
   d) Far jump to flush pipeline:  JMP 0x08:init_pm

6. Protected mode stub (init_pm, [bits 32]):
   - Load all segment registers with 0x10 (data selector)
   - ESP = 0x90000  (clean kernel stack)
   - JMP 0x8000     (kernel binary)

Memory Map After Boot:
┌──────────────────┬─────────────────────────────────────────────────────┐
│ 0x00000–0x003FF  │ Real Mode IVT (overwritten; no longer used)         │
│ 0x00400–0x004FF  │ BIOS Data Area                                      │
│ 0x00500–0x007FF  │ E820 memory map (count at 0x500, entries at 0x504)  │
│ 0x07C00–0x07DFF  │ Bootloader (512 bytes)                              │
│ 0x08000–0x8FFFF  │ Kernel image (1000 sectors = 512 KB ceiling)        │
│ 0x90000          │ Kernel stack base (grows downward)                  │
│ 0xA0000–0xBFFFF  │ VGA front buffer (mode 13h, 64 KB)                  │
│ 0xC0000–0xFFFFF  │ ROM / BIOS shadow                                   │
│ 0x100000         │ VGA back buffer  (BACKBUF, 64 KB)                   │
│ 0x164000         │ Heap start       (HEAP_START)                       │
│ 0x464000         │ Heap end default (HEAP_END, ~3 MB heap)             │
└──────────────────┴─────────────────────────────────────────────────────┘


STAGE 2: KERNEL LOADER (loader.asm)
-------------------------------------
Entry Point: 0x8000  (linked first via linker.ld)
CPU Mode: 32-bit Protected Mode

1. EBP = ESP = 0x90000   (re-establish stack pointer)
2. BSS zero-fill (rep stosb from _bss_start to _bss_end)
3. CALL kmain
4. JMP $  (safety hang if kmain ever returns)


2. KERNEL INITIALIZATION (kernel.c::kmain)
═══════════════════════════════════════════════════════════════════════════

void kmain(void) {
    alloc_init();          // Heap allocator init (HEAP_START..HEAP_END)
    detect_heap_end();     // Read E820 map to find actual RAM ceiling
    idt_init();            // Set up IDT, PIC, PIT timer (IRQ0 @ ~100 Hz)
    ps2_init();            // PS/2 keyboard + mouse

    sleep_s(1);
    boot_banner();         // "ASMOS Boot" splash in VGA mode 13h
    sleep_s(1);

    boot_check_heap();     // Validate allocator
    fs_mount();            // Mount FAT16 from sector 0 BPB
    boot_check_ata();      // Sector 0 read + 0xAA55 signature
    boot_check_fat();      // BPB sanity, cluster count, disk usage

    cfg_init_defaults();   // Populate g_cfg with built-in defaults
    cfg_load();            // Try to load /ASMOS.CFG; save if missing

    if (g_cfg.play_bootchime && g_cfg.sound_enabled)
        speaker_beep(523,120) + speaker_beep(659,120) + speaker_beep(784,180);

    sleep_s(2);

    if (!g_cfg.start_in_gui)
        cli_run();         // Blocking CLI shell

    // GUI path:
    desktop_fs_init();     // Create/mount /DESKTOP directory
    wm_init();             // Window manager
    desktop_init();        // Desktop window + icon layout + menus
    menubar_init();        // Global menubar
    if (g_cfg.sound_enabled) speaker_init();
    error_set_gui_mode(true);

    for each app in app_registry[]
        os_install_app(desc);   // Register 11 built-in apps

    scheduler_init();           // Task 0 = kernel task
    scheduler_kernel_task();    // Enter main event loop (never returns)
}

E820 Heap Detection (detect_heap_end):
- BIOS E820 memory map stored at 0x500 (count) + 0x504 (entries)
- Each entry: 20 bytes { uint64 base, uint64 length, uint32 type }
- Finds highest usable (type=1) range that covers HEAP_START
- Clamps to 32-bit; passes result to alloc_set_end()

Runtime Configuration (config/runtime_config.h):
- File path:  /ASMOS.CFG  (packed struct, magic 0x4153434F)
- Fields:
    wallpaper_pattern           (0=solid, 1=checker, 2=stripes, 3=dots)
    wallpaper_main_color        (0–15 palette index)
    wallpaper_secondary_color
    timezone_offset             (signed int8, UTC±N hours)
    start_in_gui                (1 = skip CLI)
    play_bootchime              (0/1)
    sound_enabled               (0/1)
    filef_single_window         (0=multi-window, 1=single-window mode)
    reduce_motion               (0/1 — disables window animations)
- cfg_load() / cfg_save() use FAT16 directly
- Defaults: dots wallpaper, light-blue/light-cyan, UTC+0, start in GUI


3. INTERRUPT / TIMER SUBSYSTEM (interrupts/)
═══════════════════════════════════════════════════════════════════════════

IDT (interrupts/idt.c + interrupt.asm):
- 256-entry IDT; all vectors default to isr_spurious
- IRQ0 (vector 32) → isr_timer → C handler pit_tick_handler()
- PIC remapped: master IRQ0–7 → vectors 32–39
                slave  IRQ8–15 → vectors 40–47
- Only IRQ0 (timer) unmasked; all others masked (0xFE / 0xFF)

PIT (Programmable Interval Timer):
- Channel 0, mode 3 (square wave)
- Divisor: PIT_DIVISOR = 11931  →  ~100 Hz tick rate
- pit_ticks: volatile uint32_t incremented every tick
- pit_seconds: incremented every 100 ticks
- sleep_ms(ms): busy-wait on pit_ticks (HLT between polls)
- time_millis() = pit_ticks × 10

Task Switching (os/task_switch.asm):
- Cooperative scheduler uses a software context switch
- task_switch(old_esp_ptr, new_esp):
    PUSHFD, PUSH EBP/EBX/ESI/EDI
    Save ESP to *old_esp_ptr
    Load new_esp into ESP
    POP EDI/ESI/EBX/EBP, POPFD, RET
- task_trampoline: entry stub that calls task_trampoline_c(slot)


4. MEMORY MANAGEMENT
═══════════════════════════════════════════════════════════════════════════

4.1 HEAP ALLOCATOR (lib/memory/alloc.c)
----------------------------------------
Algorithm: Intrusive linked-list first-fit with coalescing

Block header (16 bytes, aligned to 4 bytes):
    uint32_t magic   (0xF4EEB10C = free, 0xA110CA7E = used)
    uint32_t size    (payload bytes, not including header)
    bool     free
    block_header_t *next

Range: HEAP_START (0x164000) to heap_end_addr (detected via E820,
       default HEAP_END = 0x464000  →  ~3 MB heap)

kmalloc(size):
  1. Round size up to 4-byte alignment
  2. Walk list from heap_start_ptr
  3. First free block with size >= requested:
     - Split if remaining >= HEADER_SIZE + 4
     - Mark used, return payload pointer

kfree(ptr):
  1. Locate header (ptr - HEADER_SIZE)
  2. Mark free
  3. Coalesce forward with adjacent free blocks
  4. Walk backward to find prev free block; coalesce again

Additional API:
- kzalloc(size)              zero-initialising malloc
- krealloc(ptr, new_size)    realloc (copy + free old)
- kmalloc_aligned(size, align)
- heap_used() / heap_remaining()  for Monitor app

Pool Allocator (pool_t):
- Fixed-block pool backed by a caller-supplied slab
- Free-list stored as a uint32_t stack (indices into slab)
- pool_alloc / pool_free  O(1) operations

libc Compatibility Layer (lib/compat/):
- malloc / free / calloc / realloc  → kmalloc wrappers
- FILE / fopen / fread / fwrite / fseek / ftell / fclose
  backed by FAT16 file handles
- printf / fprintf / sprintf / snprintf → vsprintf
- qsort (insertion sort), bsearch, atoi/atol/atof/strtol/strtod
- Allows TCC-compiled binaries and shell programs to use standard C idioms


4.2 TASK STACKS
---------------
Each scheduler task gets an 8 KB stack (TASK_STACK_SIZE) allocated
via kmalloc at task creation; freed asynchronously after task exit
(deferred via s_pending_free[] array, freed in scheduler_reap()).


5. HARDWARE ABSTRACTION LAYER
═══════════════════════════════════════════════════════════════════════════

5.1 PS/2 CONTROLLER (io/ps2.c, keyboard.c, mouse.c)
------------------------------------------------------
Ports: 0x60 data, 0x64 status/command

Polling (ps2_update, called every frame from scheduler_kernel_task):
  kb_update()   – clears per-frame flags
  mouse_update()– clears per-frame flags
  Loop while status bit 0 set:
    byte = IN 0x60
    bit 5 of status set → mouse_process_byte(byte)
    else               → kb_process_byte(byte)

Keyboard (kb_process_byte):
- Scancode set 1; extended prefix 0xE0 tracked (extended=true)
- Modifier tracking: shift, ctrl, alt, capslock
- Release: bit 7 set; key maps cleared
- lower[128] / upper[128] ASCII tables
- Populates kb.last_char, kb.last_scancode, kb.key_pressed
- Per-frame shortcut flags: ctrl_c, ctrl_x, ctrl_v, ctrl_a, ctrl_s,
  ctrl_z, ctrl_q, ctrl_n, ctrl_o, ctrl_f, ctrl_shift_c/v/z/s

Mouse (mouse_process_byte):
- 3-byte packets; byte 0 must have bit 3 set (sync guard)
- dx = byte1 − (flags & 0x10 ? 256 : 0)
- dy = −(byte2 − (flags & 0x20 ? 256 : 0))   (Y axis inverted)
- Clamps x ∈ [0, 319], y ∈ [0, 199]
- Detects left_clicked, right_clicked on rising edge


5.2 ATA PIO (fs/ata.c)
-----------------------
Primary channel: 0x1F0–0x1F7

ata_read_sector(drive_id, lba, buf):
  1. ata_wait_ready (BSY=0, no ERR)
  2. OUT 0x1F6: 0xE0|(drive_id<<4)|(lba>>24)&0xF  (LBA28)
  3. OUT 0x1F2: 1   (sector count)
  4. OUT 0x1F3/4/5: LBA bits 0–23
  5. OUT 0x1F7: 0x20 (READ PIO)
  6. ata_wait_drq
  7. INW 0x1F0 × 256  →  512-byte buffer

ata_write_sector: same sequence with cmd 0x30; after write send
  OUT 0x1F7: 0xE7 (FLUSH CACHE) + ata_wait_ready.

Multi-sector helpers: ata_read_sectors / ata_write_sectors (loop).
Supports DRIVE_HDA (0) and DRIVE_HDB (1) on primary channel.


5.3 VGA MODE 13h (lib/graphics/)
----------------------------------
Mode: 320×200, 8-bit palette (256 colours), linear framebuffer
Front buffer: 0xA0000 (VGA MMIO, 64 KB)
Back  buffer: 0x100000 (BACKBUF, 64 KB extended RAM)

Palette (16 named entries, defined in config.h + color.h):
  0=BLACK  1=BLUE  2=GREEN  3=CYAN  4=RED  5=MAGENTA
  6=BROWN  7=LIGHT_GRAY    8=DARK_GRAY     9=LIGHT_BLUE
  10=LIGHT_GREEN  11=LIGHT_CYAN  12=LIGHT_RED  13=LIGHT_MAGENTA
  14=YELLOW  15=WHITE

color.h utilities: color_blend, color_from_rgb, color_lighten,
  color_darken, color_invert, color_contrast_fg (luma-based)

Drawing primitives (primitive_graphics.c):
  draw_dot(x, y, color)        bounds-checked single pixel
  draw_line                    Bresenham integer algorithm
  draw_rect / fill_rect        outline / solid rectangle
  draw_char(x, y, c, color, size)
    size=1: font1_data  8×8 bitmap (1 byte/row, 8 rows, MSB first)
    size=2: font2_data  6×4 bitmap (1 byte/row, 6 rows, 4-bit wide)
  draw_string(x, y, str, color, size)  character spacing: 8 (size 1) / 5 (size 2)
  blit()  32-bit DWORD copy of 64 KB back→front buffer

Fonts: Two bitmap fonts embedded in fonts/fonts.c
  font1_data:  8×8,  95 printable ASCII chars (32–126)
  font2_data:  6×4,  95 printable ASCII chars (compact UI font)


5.4 PC SPEAKER (lib/device/speaker.c)
---------------------------------------
PIT channel 2 (port 0x42) + speaker gate (port 0x61 bits 0–1)

speaker_tone_start(freq_hz):
  divisor = 1193182 / freq  (clamped 1–65535)
  OUT 0x43: 0xB6   (channel 2, mode 3, binary)
  OUT 0x42: divisor low / high bytes
  port 0x61 |= 0x03   (gate speaker)

speaker_tone_stop():
  port 0x61 &= ~0x03

speaker_beep(freq, ms):  synchronous tone + sleep_ms
speaker_beep_async / speaker_update:  tick-based non-blocking variant

ASMusic app drives speaker_tone_start/stop each frame via PIT ticks.


6. FILESYSTEM (fs/fs.c)
═══════════════════════════════════════════════════════════════════════════

FAT16 layout on disk (BPB from boot sector):
  reserved_sectors = 1000   (boot + kernel sectors)
  fat_count        = 2
  sectors_per_fat  = 32
  root_entry_count = 512
  sectors_per_cluster = 4   (2 KB clusters)
  total_sectors_32 = 65536  (32 MB volume)

Computed at mount time:
  fat_lba   = 1000
  root_lba  = fat_lba + 2×32 = 1064
  data_lba  = root_lba + (512×32)/512 = 1096
  cluster_count = (65536 − 1096) / 4 = 16110

FAT entry values:
  0x0000  free
  0x0002–0xFFEF  next cluster in chain
  0xFFF8–0xFFFF  end of chain (EOC)
  0xFFF7  bad cluster
  0xFFF0–0xFFF6  reserved

Supports both FAT12 and FAT16 (fat_type_t, detected at mount time).
FAT12 handling covers floppy images; FAT16 for the main HDA volume.

Directory entry (32 bytes packed):
  name[8] + ext[3]  (uppercase, space-padded 8.3)
  attr, reserved, create_time_ms
  create_time, create_date, access_date
  cluster_hi (always 0 on FAT16), modify_time, modify_date
  cluster_lo, file_size

Special first-byte values:
  0x00  end-of-directory sentinel
  0xE5  deleted entry (free slot)

Key attribute bits: ATTR_DIRECTORY (0x10), ATTR_ARCHIVE (0x20),
  ATTR_VOLUME_ID (0x08), ATTR_READ_ONLY (0x01), ATTR_HIDDEN (0x02)

Multi-drive support:
  DRIVE_HDA (0) – primary HDD, main FAT16 volume
  DRIVE_HDB (1) – secondary HDD (optional)
  DRIVE_FDD0 (2) – floppy drive A
  DRIVE_FDD1 (3) – floppy drive B
  Each drive mounted independently via fs_mount_drive()
  VFS mount table maps logical paths /HDB, /FDD0, /FDD1

Path resolution (fs_resolve):
  Walks path components from dir_context.current_cluster
  Absolute paths (leading '/') reset to cluster 0 (root)
  Handles VFS cross-drive path segments transparently
  Returns parent_cluster + 11-byte 8.3 name for final component

High-level operations:
  fs_open / fs_create / fs_read / fs_write
  fs_seek / fs_tell / fs_close
  fs_find / fs_find_in_dir / fs_list_dir
  fs_rename / fs_delete / fs_rmdir / fs_rm_rf
  fs_mkdir / fs_copy_file / fs_copy_dir
  fs_move_file / fs_move_dir
  fs_set_hidden / fs_is_hidden
  fs_get_usage / fs_get_usage_drive
  fs_resolve_dir (path → drive_id + cluster)

Current directory: global fat_dir_ctx_t { drive_id, current_cluster, path[256] }
  fs_chdir resolves ".." via dotdot entries; normalises path string.

Protected paths: DESKTOP, HDB, FDD0, FDD1 — cannot be deleted/renamed.


7. FLOPPY DISK (fs/fdd.c)
═══════════════════════════════════════════════════════════════════════════

FDC (Floppy Disk Controller) at ports 0x3F0–0x3F7.
Geometry: 80 tracks × 2 heads × 18 sectors × 512 bytes = 1.44 MB.

DMA buffer at physical 0x7000 (fits in low 64 KB for ISA DMA).
Channel 2 DMA used for data transfer.

fdd_init(drive_id):
  - CMOS check for drive presence (register 0x10)
  - FDC reset via DOR port
  - SPECIFY command: SRT/HLT/HUT timings
  - Set 500 Kbps data rate (CCR register)
  - Recalibrate to track 0

fdd_read_sector / fdd_write_sector:
  - Motor start (300 ms spin-up delay)
  - Seek to cylinder
  - DMA setup (read: 0x56, write: 0x5A mode byte)
  - FDC READ/WRITE DATA command (9 bytes)
  - Poll for completion; check ST0/ST1/ST2 status bytes
  - Up to 3 retry attempts with recalibrate on failure


8. MULTITASKING SCHEDULER (os/scheduler.c)
═══════════════════════════════════════════════════════════════════════════

Model: cooperative, round-robin, up to MAX_TASKS=64 tasks

Task descriptor (task_t):
  uint32_t esp         (saved stack pointer)
  uint8_t *stack_base  (kmalloc'd 8 KB block)
  bool     alive
  void (*entry)(void *)
  void    *arg

Task 0: kernel task (scheduler_kernel_task), stack at 0x90000 (no heap alloc)
Tasks 1–63: app tasks (allocated by scheduler_add_task)

scheduler_add_task(entry, arg):
  1. Find free slot
  2. kmalloc 8 KB stack; write STACK_CANARY (0xDEADBEEF) at base
  3. Build initial stack frame (task_switch-compatible):
     [ edi=0 | esi=0 | ebx=0 | ebp=0 | eflags=0x202 ]  ← saved ESP
     [ &task_trampoline ] ← ret address after task_switch
     [ 0 ]               ← fake return for trampoline_c
     [ slot ]            ← argument to task_trampoline_c
  4. Set tasks[slot].esp, alive=true

task_yield():
  Checks canary before yielding (catches stack overflow)
  Round-robin search for next alive task
  Calls task_switch(&tasks[old].esp, tasks[next].esp)

Stack deallocation is deferred: dying tasks enqueue stack pointer into
  s_pending_free[]; scheduler_reap() (called from os_reap_dead_apps)
  calls kfree on all pending pointers.

Kernel Task Event Loop (scheduler_kernel_task):
  while (!gui_should_exit) {
    frame_start = time_millis();
    ps2_update();          // Input
    speaker_update();      // Async beep timeouts
    handle_global_shortcuts();  // Ctrl+Q = close focused window
    wm_sync_menubar();     // Sync focused window menus → global menubar
    menubar_layout();
    if (!modal_active()) {
        menubar_update();
        os_tick_apps();    // Call on_frame for every running app
        wm_update_all();   // Window drag/resize/click
    }
    os_reap_dead_apps();   // Destroy exited apps, free memory
    desktop_on_frame();    // Wallpaper + desktop icon logic
    wm_draw_all();         // Draw windows back-to-front
    menubar_draw();
    modal_update(); modal_draw();
    draw_cursor(mouse.x, mouse.y);
    blit();
    while (time_millis() - frame_start < FRAME_TIME_MS) task_yield();
  }

TARGET_FPS = 60, FRAME_TIME_MS = 16 ms


9. APPLICATION FRAMEWORK (os/os.c)
═══════════════════════════════════════════════════════════════════════════

App descriptor (app_descriptor):
    const char *name
    uint32_t    state_size
    void (*init)(void *state)
    void (*on_frame)(void *state)
    void (*destroy)(void *state)
    bool        single_instance

App instance (app_instance_t):
    app_descriptor *desc
    void           *state   (kmalloc'd state_size bytes)
    bool            running
    bool            wants_quit
    int             task_slot  (-1 = no dedicated task)

Registry: app_registry[] in os/app_registry.c lists 11 built-in apps.
  os_install_app registers them into installed_apps[MAX_INSTALLED_APPS=256].
  os_launch_app allocates state, calls init(), sets running=true.
  os_quit_app / os_quit_app_by_desc: set wants_quit flag.
  os_reap_dead_apps: calls destroy(), kfrees state, decrements count.

os_tick_apps: called every frame; rate-limited by FRAME_TIME_MS.
  Each app's on_frame is called at most once per 16 ms.

os_find_app(name): linear search through installed_apps by name string.
  Used by FileF and Desktop when double-clicking .APP shortcuts.

os_close_own_instance(win): finds the app instance whose state->win
  matches the given window pointer and calls os_quit_app on it.
  Handles the common pattern of apps embedding their window as first field.

Global alarm (g_alarm): set by Clock app; fires once per minute match,
  plays beeps and shows a modal dialog. Checked each frame in desktop_on_frame.


10. WINDOW MANAGER (ui/window.c)
═══════════════════════════════════════════════════════════════════════════

Window struct (partial):
  x, y, w, h, min_w, min_h, max_w, max_h
  title (char*), title_color, bar_color, content_color
  visible, minimized, dragging, resizing, resizable
  show_order (z-order integer)
  pinned_bottom (desktop background window flag)
  animate_open_close (bool — respects g_cfg.reduce_motion)
  anim_state: NONE / OPEN / CLOSE / MINIMIZE / RESTORE
  anim_no_of_frames (default: WIN_ANIM_DEFAULT_FRAMES = 20)
  on_close / on_minimize callbacks
  on_draw(win, userdata) custom content render hook
  widgets[MAX_WIN_WIDGETS=128], widget_count
  win_menus[MAX_WIN_MENUS=16], win_menu_count

Animations (draw_anim):
  OPEN/CLOSE:    lerp window rect from/to centre point
  MINIMIZE:      lerp window rect to/from taskbar button position
  RESTORE:       reverse of MINIMIZE
  All animations disabled when g_cfg.reduce_motion = 1

Stack: win_stack[MAX_WINDOWS=32], sorted by show_order ascending.
  Highest show_order = topmost window.

wm_focus(win): assigns show_order = win_count−1, compresses others,
  re-sorts stack. Sets focused_window global.

wm_sync_menubar(mb): copies menus from focused window into global menubar,
  preserving open state across focus changes.

wm_draw_all:
  For each window in stack order (back-to-front):
    If animating: draw_anim(win) and skip full draw
    window_draw(win):
      - Title bar (bar_color, 16 px high)
      - Close button [X] (RED, 10×10 at x+3, y+3)
      - Minimise button [_] (LIGHT_BLUE, 10×10 at x+16, y+3)
      - Title string (centred, truncated with "..." if needed)
      - Content fill (content_color below title bar)
      - Custom on_draw callback
      - Resize grip (8×8 bottom-right, if resizable)
      - Widget draw loop
  Taskbar: minimised windows drawn as labelled buttons at screen bottom.

wm_update_all:
  1. Taskbar click → restore minimised window (triggers RESTORE anim)
  2. Click in window area → wm_focus
  3. Delegate to window_update(focused_window):
     - Close button → WIN_ANIM_CLOSE (or immediate if reduce_motion)
     - Minimise button → WIN_ANIM_MINIMIZE (or immediate)
     - window_resize: drag resize grip
     - window_dragged: drag title bar
     - Widget update loop

Window coordinates:
  Content origin  = (win->x+1,  win->y + MENUBAR_H + 16)
  MENUBAR_H = 10, TITLEBAR_H = 16
  Effective content y = win->y + 26


11. MENUBAR (ui/menubar.c)
═══════════════════════════════════════════════════════════════════════════

Global menubar (g_menubar): populated by wm_sync_menubar each frame.
  Layout computed by menubar_layout: clock reserve (CLOCK_W) + title widths.

Clock area (leftmost ~42 px):
  Reads time_rtc_local() each frame draw; clicking launches Clock app.

Menu item: label + MenuAction (void(*)(void)) or MenuActionUD (void(*)(void*))
  with userdata pointer. Separators have label=NULL.

MAX_MENUS=8 per window, MAX_MENU_ITEMS=12 per menu, MENU_ITEM_H=10 px.

Click propagation flag: g_menubar_click_consumed prevents desktop /
  window layers from acting on the same click.


12. MODAL DIALOG (ui/modal.c)
═══════════════════════════════════════════════════════════════════════════

Singleton modal: only one dialog at a time.
Types: MODAL_INFO, MODAL_WARNING, MODAL_ERROR, MODAL_CONFIRM.

Dimensions: 220×110, centred on screen.
Layout: coloured title bar (16 px) + wrapped message (4 lines max)
  + OK button (or OK + Cancel for CONFIRM).
Title bar also features an [X] close button (calls on_cancel).

modal_active(): returns true → scheduler_kernel_task skips app ticks
  and window updates, keeping dialog exclusive.

modal_show(MODAL_ERROR):  plays descending triple beep via speaker.
modal_show(MODAL_WARNING): plays two-tone warning beep.


13. SHELL / CLI (shell/cli.c + cmds.c)
═══════════════════════════════════════════════════════════════════════════

cli_run(): blocking pre-GUI text shell.
  Renders directly to VGA back buffer using draw_char / blit.
  Polls ps2_update in a tight loop (no scheduler; runs before GUI).
  Scrolls by memmove of back buffer rows.

Commands (shell/cmds.c):
  File ops:    ls, cat, touch, rm [-r], write, cp, mv
  Directory:   pwd, cd, mkdir, rmdir
  System:      df, mem, sysinfo, clock, clear, help, gui, exit
  Terminal:    history, tee <file>  (writes term_buf to FAT16 file)
  Power:       shutdown [s], restart [s]  (optional delay in seconds)
  Assembler:   asm <src.asm> [out.bin]
  Binary run:  run <file.bin>

Terminal Buffer (shell/term_buf.c):
  256-line ring buffer (128 chars/line).
  term_buf_push / term_buf_push_text / term_buf_get / term_buf_save.
  All CLI and ASMTerm output is mirrored here for history/tee.

term_context_t: virtual I/O interface for running binaries.
  Callbacks: print, putchar, readline, getchar, userdata.
  CLI and ASMTerm each provide their own implementation.
  Allows the assembler and binrun to be context-agnostic.

cli_execute_command: shared by both CLI and ASMTerm on_frame,
  returning CMD_STATUS_OK / EXIT / GUI / CLEAR.


14. x86 ASSEMBLER (shell/asm/asm.c)
═══════════════════════════════════════════════════════════════════════════

A self-hosted two-pass flat-binary x86 assembler running inside ASMOS.
Source written in TEditor, assembled via `asm` CLI command or ASMTerm,
output .BIN file executed by `run`.

Input:  FAT16 source file (.ASM) or in-memory string
Output: flat binary up to ASM_OUT_MAX = 16384 bytes

Two-pass algorithm:
  Pass 0: emit code, record label addresses, note forward references
  Pass 1: re-emit code (with all labels now known), apply fixups

Label table: ASM_MAX_LABELS=128 entries  { name[64], addr, defined }
Fixup table: ASM_MAX_FIXUPS=128 entries
  Types: FIX_REL8 (short jump), FIX_REL32 (near jmp/call), FIX_ABS32 (dd/imm32)
  Applied after pass 1 by patching bytes in a_out[]

Tokeniser (a_tokenise):
  - Strips comments (;)
  - Handles quoted strings (single + double), escape sequences \n \t \0
  - Square-bracket expressions: memory operands [base+idx*scale+disp]
                                 directives [bits N] [org N] stripped
  - Comma/space delimiters; up to ASM_MAX_TOKENS=32 tokens per line

Expression evaluator (a_eval):
  - Supports +/- binary operators (right-to-left split at lowest-prec op)
  - Parenthesised sub-expressions
  - $ (current address), $$ (origin)
  - Numeric literals: decimal, 0x hex, trailing-h hex
  - Label references (forward-ref aware)

Register parser (a_reg): recognises all 8 × 32-bit, 16-bit, and 8-bit
  general-purpose registers (EAX–EDI, AX–DI, AL/AH–BL/BH).

Supported instruction set (subset of 32-bit IA-32):
  Data mov:   MOV, LEA, XCHG, MOVZX, MOVSX
  ALU:        ADD, OR, ADC, SBB, AND, SUB, XOR, CMP, TEST
              INC, DEC, NEG, NOT, MUL, IMUL, DIV, IDIV
  Shifts:     SHL/SAL, SHR, SAR, ROL, ROR, RCL, RCR
  Stack:      PUSH, POP, PUSHA/POPA, PUSHF/POPF, ENTER, LEAVE
  Jumps:      JMP (short/near), CALL (near/reg/mem), RET, RETF
              All Jcc mnemonics (JE, JNE, JL, JGE, JBE, JA, JS, …)
              LOOP, LOOPZ, LOOPNZ
  Flags:      CLC/STC/CMC, CLD/STD, CLI/STI, SAHF, LAHF
  String:     STOSB/STOSW/STOSD, MOVSB/MOVSW/MOVSD,
              SCASB/SCASD, LODSB/LODSD, CMPSB, REP/REPE/REPNE
  I/O:        IN, OUT
  System:     INT, NOP, HLT, IRET, WAIT, XLAT, CBW/CWDE, CDQ
  SETcc:      SETE, SETNE, SETL, SETGE, SETLE, SETG, SETB, SETAE, …
  Directives: BITS, ORG, DB, DW, DD, TIMES, EQU, label:

Binary calling convention (for `run`):
  Entry:  void binary(const bin_syscall_t *sc)   [esp+4] = sc pointer
  sc offsets: PRINT=0, PUTCHAR=4, READLINE=8, GETCHAR=12, ITOA=16
  Position-independent code requires the call/pop base-address trick:
    call .here
    .here: pop ebx
    sub  ebx, (.here - $$)   ; ebx = runtime base address


15. BINARY RUNNER (shell/binrun.c)
═══════════════════════════════════════════════════════════════════════════

cmd_run(ctx, path, out, max):
  1. fs_open → read file_size bytes into kmalloc'd buffer
  2. kmalloc a bin_task_ctx_t context
  3. scheduler_add_task(bin_task_entry, ctx)  → new cooperative task
  4. Returns immediately; binary runs concurrently

bin_task_entry (runs in new task):
  Calls  ((bin_fn_t)buf)(sc_table)
  On return: kfree(buf), scheduler_exit_current()

bin_syscall_t (function pointer table passed to binary):
  sc_print(str)          → term_ctx_print
  sc_putchar(char)       → term_ctx_putchar
  sc_readline(buf, max)  → yields until ENTER received from input queue
  sc_getchar()           → yields until character available
  sc_itoa(val, buf)      → int_to_str wrapper

Binary size limit: BINRUN_MAX_SIZE = 65536 bytes.


16. BUILT-IN APPLICATIONS
═══════════════════════════════════════════════════════════════════════════

All apps follow the app_descriptor pattern (init / on_frame / destroy).
State is a privately defined struct, zero-initialised by the OS.

16.1 ASMDraw (apps/asmdraw.c)
  Pixel art / paint application.
  Canvas: up to 320×200 8-bit palette pixels stored in
    uint8_t canvas[200][320] inside app state.
  Tools: Pencil, Eraser, Line (Bresenham), Rectangle, Fill (stack flood-fill).
  Brush sizes: 1, 3, 5 pixel radius squares.
  Palette: 16 colours displayed as a strip at the bottom.
  Save/Load: custom .PIC format  (4-byte header: width[2] + height[2], then raw pixels).
  Toolbar drawn at left edge (TOOLBAR_W=18 px); canvas offset by CANVAS_X_OFF.

16.2 ASMTerm (apps/asmterm.c)
  Terminal emulator with full shell + assembler + binary runner access.
  Output ring buffer: 128 lines × 80 chars.
  Input queue (INPUT_QUEUE_CAP=256) fed by keyboard, consumed by
    tctx_readline / tctx_getchar callbacks used by binrun.
  Binary waiting mode: shows "BIN>" prompt; input forwarded to running binary.
  History: 32 entries, up/down arrow navigation.
  Scrollable output with scrollbar; click-to-scroll also supported.
  Resizable window; visible_rows recalculated from window height each frame.
  Executes all cli_execute_command verbs including `asm` and `run`.

16.3 ASMusic (apps/asmusic.c)
  16-step × 12-note monophonic step sequencer.
  Notes: D4–A5 (12 fixed frequencies mapped to PC speaker Hz).
  BPM range: 60–180 in 21 steps; ticks_per_step = (6000/BPM)/4.
  Playback: pit_ticks drive tick_accum; each step fires speaker_tone_start
    for the lowest active note in that column; note_off after 60% of step.
  Demo pattern pre-loaded at init (Linkin Park "In the End" inspired riff).
  Keyboard shortcuts: Space=play/stop, C=clear, ←/→=BPM.

16.4 Calculator (apps/calculator.c)
  4-function calculator (+ − × ÷) with %, +/− sign toggle.
  State machine: accumulator + pending_op + fresh_operand flag.
  Custom double→string conversion (no libc printf needed).
  Display: 16-char right-aligned readout.
  Keyboard entry supported alongside mouse.
  Single-instance app.

16.5 Clock (apps/clock.c)
  Minimal clock window using widget labels.
  Reads time_rtc_local() every frame (no rate-limit; low cost).
  Displays HH:MM:SS and DD/MM/YYYY via update_label_text.
  Alarm menu: set/clear a global alarm (g_alarm); fires once per day
    at specified HH:MM, triggers beeps and a modal notification.
  Single-instance app.

16.6 FileF (apps/filef.c)
  Icon-based file manager; supports multiple simultaneous instances
  (or single-window mode controlled by g_cfg.filef_single_window).
  Virtual root ("/") shows all mounted drives as icons.
  Drives: HDA, HDB, FDD0, FDD1 — each accessible as /HDA, /HDB, etc.
  Navigation history: 32-entry back/forward stack; navbar buttons + Home/Desk.
  Icons: 20×20 px per entry, 44×30 px cells; up to 128 entries per directory.
  Scrollable content area with 4 px scrollbar.
  Sort modes: by Name, Date, Size (bubble sort on ff_item_t array).
  Double-click: open directory (new FileF instance or same window) or launch .APP.
  Drag-and-drop: move files between FileF windows or to Desktop.
  Drag-to-desktop: calls desktop_accept_drop (fs_move_file/dir).
  Clipboard: global g_clipboard (copy/cut/paste within and across windows).
  Modes: NORMAL, RENAME (inline text edit), CONFIRM (delete), NEWNAME,
         INFO (Get Info overlay with path copy button).
  Hidden files: shown/hidden via "Show Hidden" toggle; togglable via Edit menu.
  Keyboard shortcuts: Ctrl+C/X/V, Delete.
  Cascade position (FF_CASCADE=16 px) for each new window.

16.7 HexView (apps/hexview.c)
  Two-window hex viewer (address+hex | ASCII) for up to 64 KB files.
  Hex window: shows 16-byte rows with 4-digit address and colour-coded bytes.
  ASCII window: printable chars in LIGHT_GREEN; non-printable as '.'.
  Horizontal scrollbars on both panes; vertical scrollbar on hex pane.
  Arrow/PgUp/PgDn/Home/End/F5/F6 keyboard navigation.
  File open via in-window dialog with Ctrl+V paste support; data in kmalloc'd buffer.

16.8 Monitor (apps/monitor.c)
  System resource monitor with two tabs.
  Task List tab: lists all running apps; Switch To / End Task / Cancel buttons.
    Clicking "Switch To" focuses the selected app's window.
    Clicking "End Task" calls the app's on_close or os_quit_app.
  System tab: CPU model (CPUID), memory usage (heap_used/remaining),
    per-drive usage with colour-coded bar charts; uptime display.
  UPDATE_INTERVAL = 60 frames (~1 second refresh).
  Single-instance app.

16.9 Settings (apps/settings.c)
  Runtime configuration editor.
  Reflects and mutates g_cfg directly via button clicks.
  Settings: wallpaper pattern, colours A/B (0–15), timezone offset (±14),
    boot chime, sound enabled, start-in-GUI toggle,
    FileF single/multi window mode, reduce motion toggle.
  Dirty flag + "Save" button (turns RED when unsaved) calls cfg_save().
  Single-instance app.

16.10 TEditor (apps/teditor.c)
  Plain-text editor; 8 KB buffer (TEXT_CAP), up to 512 lines (MAX_LINES).
  Line index: line_start[MAX_LINES] rebuilt by np_reindex after every edit.
  Binary search np_line_of + np_col_of for O(log n) cursor queries.
  Cursor: character position (byte offset into text[]).
  Vertical + horizontal scrollbars; horizontal auto-scrolls to cursor.
  Highlight current line (LIGHT_CYAN band).
  Selection: Shift+arrow, Shift+Home/End, Ctrl+A; shown as LIGHT_GRAY band.
  Clipboard: Ctrl+C copy, Ctrl+X cut, Ctrl+V paste.
  File dialog: Open / Save / Save As with auto-.TXT extension.
  Keyboard: arrows, Home, End, PgUp/PgDn, Tab (4 spaces), Delete, Backspace.
  Multi-instance; unsaved-changes confirm dialog on close.
  Can be opened directly by FileF on double-click of .TXT files.
  Dirty marker '*' in status bar.


17. DESKTOP SYSTEM (ui/desktop.c + desktop_fs.c)
═══════════════════════════════════════════════════════════════════════════

Desktop Window:
  Pinned-bottom, covers full screen minus menubar and taskbar.
  show_order = −1 (always behind all real windows).
  Owns "Apps", "File", "Edit", "Action" global menus.
  File menu includes Rename for selected desktop items.

Wallpaper Rendering (draw_wallpaper_pattern):
  Pattern 0 – Solid:       clear_screen(main_color)
  Pattern 1 – Checkerboard: 10×10 tile grid alternating main/secondary
  Pattern 2 – Stripes:      horizontal 5-px stripes
  Pattern 3 – Dots:         solid fill + dot every 10 px

Power Animations (power_anim_run):
  Phase 1: scanline fade to black (8 passes, every 3rd row cleared per pass)
  Phase 2: animated progress bar with leading pulse edge, dot spinner,
           percentage counter, and scanline overlay (40 steps)
  Phase 3: 3-frame flash (DARK_GRAY/BLACK alternating)
  Phase 4: final hold with coloured message text
  Restart uses LIGHT_GREEN/GREEN; shutdown uses LIGHT_CYAN/CYAN.

Desktop FS (/DESKTOP directory):
  Created at first boot; populated with FILEF.APP + ASMDRAW.APP shortcuts.
  desktop_fs_reload(): reads FAT16 directory, builds desktop_item_t array.
    .APP files → DESKTOP_ITEM_APP (extension stripped for display)
    directories → DESKTOP_ITEM_DIR
    other files → DESKTOP_ITEM_FILE (PIC/TXT/CFG icons differentiated)
    Floppy drives → DESKTOP_ITEM_FDD (one per mounted FDD)
    DRIVE_HDB → DESKTOP_ITEM_HDD (if mounted)
  grid_slot(n): column-major layout (col = n / rows_avail).
  Icons persist position in memory only (no sidecar metadata file).

Rename dialog: inline text box, same validation as FileF newname dialog.

Double-click detection: TARGET_FPS ticks window (~1 second at 60 fps).

Desktop keyboard shortcuts (when desktop is focused):
  Ctrl+C / Ctrl+X / Ctrl+V / Delete — same operations as Edit menu.

Desktop drag-and-drop: icon positions updated in memory;
  fs_move_* called when dragging from FileF onto desktop area.


18. CURSOR (ui/cursor.c)
═══════════════════════════════════════════════════════════════════════════

4 cursor types (CHOSEN_CURSOR global):
  0 = Arrow      1 = Pointer (hand)
  2 = Crosshair  3 = Text (I-beam)

Each cursor is an 8-row × 9-column bitmap stored as two uint16_t[8] arrays:
  cursor_fill[type][row]   – WHITE pixels
  cursor_border[type][row] – BLACK pixels (drawn on top of fill)
Rendering: draw_dot per set bit, bounds-clamped, drawn last each frame.


19. ICON SYSTEM (ui/icons.c + icons.h)
═══════════════════════════════════════════════════════════════════════════

icon_view_t: reusable icon grid embedded in both desktop and FileF.
  MAX_ICONS_PER_VIEW=32 icons per view, 20×20 px each.
  Double-click threshold: TARGET_FPS ticks.
  Drag threshold: DRAG_THRESHOLD=4 px movement.
  icon_view_update: hit-test, click/double-click/drag state machine.

Per-extension file icons drawn inline:
  draw_folder_icon, draw_app_icon, draw_file_icon, draw_dotdot_icon,
  draw_pic_icon (.PIC), draw_txt_icon (.TXT), draw_cfg_icon (.CFG),
  draw_hdd_icon, draw_floppy_icon, draw_unknown_icon (fallback).
  All use 20×20 px (ICO_W × ICO_H) bounding boxes.
  Selected state: darker fill + no drop-shadow on label.


20. ERROR HANDLING (os/error.c)
═══════════════════════════════════════════════════════════════════════════

err_severity_t: ERR_INFO, ERR_WARNING, ERR_FATAL
err_code_t: ~20 codes covering ATA, FAT16, OOM, WM, app errors.

Boot mode (before error_set_gui_mode(true)):
  Text printed directly to VGA back buffer via boot_putchar.
  ERR_FATAL: print "System halted." then CLI + HLT forever.

GUI mode (after desktop init):
  ERR_FATAL / ERR_WARNING → modal_show(MODAL_ERROR/WARNING, …)
  ERR_FATAL additionally: busy-wait 0x8000000 NOPs, then CLI + HLT.

Boot checks (called from kmain before GUI starts):
  boot_check_heap:  kmalloc(16) + kfree + heap_remaining >= 1 KB
  boot_check_ata:   sector 0 read + 0xAA55 signature
  boot_check_fat:   BPB fields, cluster count in FAT16 range, <90% full

Assertion macros: ASSERT(cond, code, ctx) → ERR_FATAL if false
  ASSERT_NOT_NULL(ptr, ctx)
  WARN_IF(cond, code, ctx)


21. STRING / MATH / TIME LIBRARIES
═══════════════════════════════════════════════════════════════════════════

lib/string/string.c:
  Full reimplementation of standard C string functions:
  strlen, strcpy, strncpy, strcat, strncat, strcmp, strncmp,
  strchr, strrchr, strstr, strdup, strndup, strcasecmp, strncasecmp,
  strlcpy, strlcat, strtok_r, memchr, memmem
  Number ↔ string: uint32_to_str, str_to_uint32, int_to_str, str_to_int,
    uint32_to_hex_str, str_to_long, str_to_ulong, str_to_double
  vsprintf / sprintf: full format string (%c %s %d %i %u %x %X %o %p %%),
    supports flags (- 0 + space), width, precision, %l/%h length modifier.
  ctype: isalpha, isdigit, isspace, isupper, islower, isalnum, isprint,
    ispunct, isxdigit, iscntrl, toupper, tolower
  Path helpers: path_basename, path_dirname, path_join, path_ext
  strbuf_t: inline string builder with overflow tracking

lib/math/math.h + math.c:
  abs, max, min, clamp, lerp_i, sign, is_pow2, fabs, floor, ceil,
  div_ceil, align_up, align_down, map_range, wrap, abs_diff
  isqrt (Newton–Raphson), ipow, next_pow2, log2_floor
  XOR-shift PRNG: rand_seed / rand_next / rand_range

lib/time/time.c:
  CMOS RTC reader (registers 0x00/02/04/07/08/09, double-read for consistency)
  BCD→binary conversion
  time_rtc_local(): applies g_cfg.timezone_offset, handles day wrap
  FAT16 timestamp encoding: time_from_rtc() / date_from_rtc()
  Uptime: time_seconds() / time_millis() / uptime_hms()
  sleep_ms / sleep_s (pit_ticks busy-wait)

lib/cpu/cpu_metrics.c:
  cpu_model_init(): reads CPU brand string via CPUID leaves 0x80000002–4
  cpu_metrics_update(): idle-loop calibration for approximate CPU usage %
  cpu_model_str() / cpu_usage_percent()


22. KNOWN LIMITATIONS
═══════════════════════════════════════════════════════════════════════════

ARCHITECTURE:
  - All code runs in ring 0 (no memory protection or user mode)
  - No page tables / virtual memory / address space isolation
  - No DMA for ATA; all disk I/O is PIO (CPU-driven, blocking)
  - PS/2 is polled every frame, not interrupt-driven
  - Cooperative multitasking: a task that never yields blocks everything
  - No floating-point save/restore in task switch (apps must avoid FPU)

LIMITS (compile-time constants):
  MAX_WINDOWS           32
  MAX_WIN_WIDGETS       128
  MAX_WIN_MENUS         16 per window / 8 in global menubar
  MAX_MENU_ITEMS        12 per menu
  MAX_INSTALLED_APPS    256
  MAX_RUNNING_APPS      48
  MAX_TASKS             64
  FAT16 volume          32 MB (65536 sectors × 512 bytes)
  Heap                  ~3 MB default (E820-detected upper bound)
  TEditor               Max 1MB file (however, it may take a week to load :) )
  ASMDraw canvas        320×200 px (64 KB in app state)
  Assembler output      16 KB
  HexView file          64 KB
  Term buffer           256 lines × 128 chars
  FileF entries/dir     128
  Navigation history    32 entries per FileF instance

FILESYSTEM:
  - FAT16/FAT12 only (no FAT32, ext2, etc.)
  - 8.3 filenames only
  - No file locking; concurrent writes from multiple apps unsupported
  - No journaling; power loss during write may corrupt FAT

DISPLAY:
  - Fixed 320×200 resolution, 16-colour palette (mode 13h supports 256)
  - No hardware acceleration; all rendering is software
  - Back buffer at 1 MB requires PC to have extended memory (always true)
  - No sub-pixel rendering or anti-aliasing
