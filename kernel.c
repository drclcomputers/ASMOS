// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "config/video_cfg.h"
#include "drivers/ata.h"
#include "drivers/joystick.h"
#include "drivers/lpt.h"
#include "drivers/pci.h"
#include "drivers/ps2.h"
#include "fs/fs.h"
#include "fs/install.h"
#include "lib/apm.h"
#include "lib/cpu.h"
#include "ui/uifont.h"

#include "drivers/gpu.h"
#include "drivers/ne2000.h"
#include "drivers/opl2.h"
#include "drivers/sb16.h"
#include "lib/realmode.h"

#include "network/dhcp.h"
#include "network/dns.h"
#include "network/net.h"

#include "os/app_registry.h"
#include "os/appload.h"
#include "os/error.h"
#include "os/meminfo.h"
#include "os/os.h"
#include "os/scheduler.h"
#include "shell/cli.h"

#include "config/config.h"
#include "config/runtime_config.h"

#include "drivers/serial.h"
#include "drivers/speaker.h"
#include "interrupts/idt.h"
#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/time.h"

#include "ui/ui.h"

extern uint8_t _heap_start, _low_heap_start, _kstack_bottom;
int g_screen_width = 640;
int g_screen_height = 400;
int g_video_mode = 1;
int g_backbuf_size = 640 * 400;

static void play_bootchime(void) {
    if (g_cfg.play_bootchime && g_cfg.sound_enabled) {
        if (opl2_detected()) {
            const uint8_t ch = 0;

            opl2_set_instrument(ch, &opl2_gm_patches[0]);

            opl2_note_on(ch, 60, 80);
            sleep_ms(150);
            opl2_note_off(ch);
            sleep_ms(50);

            opl2_note_on(ch, 64, 80);
            sleep_ms(150);
            opl2_note_off(ch);
            sleep_ms(50);

            opl2_note_on(ch, 67, 90);
            sleep_ms(200);
            opl2_note_off(ch);
        } else { /* no OPL2 (SB16 has no tone path here): PC speaker */
            speaker_beep(523, 120);
            speaker_beep(659, 120);
            speaker_beep(784, 180);
        }
    }
}

static void dbg_bar(int col, uint8_t color) {
    __asm__ volatile("movw $0x03C4, %%dx\n\t"
                     "movb $0x02,   %%al\n\t"
                     "outb %%al,    %%dx\n\t"
                     "incw %%dx\n\t"
                     "movb $0x0F,   %%al\n\t"
                     "outb %%al,    %%dx\n\t" ::
                         : "eax", "edx");

    if (video_mode_colors(g_video_mode) == 256) {
        /* (paging is off yet: a linear frame buffer is where it is; a
         * window shows the first 64 KB) */
        int pitch = g_vbe.mode ? g_vbe.pitch : g_screen_width;
        uint32_t end = g_vbe.mode && !g_vbe.lfb ? 65536 : 0xFFFFFFFFu;
        volatile uint8_t *fb =
            (volatile uint8_t *)(g_vbe.lfb ? g_vbe.lfb : 0xA0000);
        int x = col * 8;
        if (x + 8 > g_screen_width)
            return;
        for (int y = 0; y < g_screen_height; y++)
            for (int i = 0; i < 8; i++)
                if ((uint32_t)(y * pitch + x + i) < end)
                    fb[y * pitch + x + i] = color;
    } else {
        volatile uint8_t *fb = (volatile uint8_t *)0xA0000;
        int bx = col, stride = g_screen_width / 8;
        if (bx >= stride)
            return;
        for (int y = 0; y < g_screen_height; y++)
            fb[y * stride + bx] = color;
    }
}

unsigned int g_ram_bytes;
static uint32_t s_ram_top; /* end of RAM (or of what we use of it) */

// where the memory goes (os/meminfo.h)

void meminfo(meminfo_t *m) {
    memset(m, 0, sizeof(*m));
    uint32_t shadow = gpu_shadow_bytes(),
             app_heap = appload_kernel_heap_bytes();
    uint32_t used = heap_used();
    m->screen = (uint32_t)g_backbuf_size + shadow;
    m->apps = (pmm_total() - pmm_free_count()) * PMM_PAGE_SIZE + app_heap;
    m->heap = used > shadow + app_heap ? used - shadow - app_heap : 0;
    m->free = heap_remaining() + pmm_free_count() * PMM_PAGE_SIZE;
    m->reserved = 0x100000 - 0xA0000;
    /* the kernel: what it is made of, or (RAM known) the rest */
    uint32_t parts = ((uint32_t)&_heap_start - 0x100000) + /* code, data */
                     0x10000 + /* BIOS data, buffers */
                     (0xA0000 - (uint32_t)&_kstack_bottom) + /* stack */
                     0x10000;                                /* the top 64 KB */
    uint32_t others = m->reserved + m->screen + m->heap + m->apps + m->free;
    m->total = g_ram_bytes > others + parts ? g_ram_bytes : others + parts;
    m->kernel = m->total - others;
}

const char *meminfo_kb(uint32_t bytes, char *out, int sz) {
    uint32_t kb = (bytes + 512) / 1024;
    if (kb >= 10240)
        snprintf(out, sz, "%u.%u MB", kb / 1024, kb % 1024 * 10 / 1024);
    else
        snprintf(out, sz, "%u KB", kb);
    return out;
}

static void detect_heap_range(void) {
    dbg_bar(0, 4);

    /* the kernel (code, data, zeroed data) is at 1 MB; the heap follows
     * (the back buffer is below 640 KB: linker.ld) */
    uint32_t heap_bot = (uint32_t)&_heap_start;

    if (heap_bot < HEAP_MIN_START)
        heap_bot = HEAP_MIN_START;

    heap_bot = (heap_bot + 15U) & ~15U;

    dbg_bar(1, 2);
    dbg_bar(2, 1);

    uint32_t total_ram = *(volatile uint32_t *)0x0500;
    g_ram_bytes = total_ram;

    dbg_bar(3, 6);

    uint32_t heap_top;
    if (total_ram == 0) /* (not known: assume 512 KB above the kernel) */
        heap_top = heap_bot + 0x80000;
    else if (total_ram > heap_bot + 0x10000)
        heap_top = total_ram - 0x10000;
    else /* nothing above the kernel: only the heap below 640 KB */
        heap_top = heap_bot;

    if (heap_top > HEAP_END_MAX)
        heap_top = HEAP_END_MAX;

    dbg_bar(4, 5);

    /* below 640 KB, after the back buffer (where the image was read before
     * stage 2 copied it to 1 MB), up to the stack: a second heap. With it,
     * a 2 MB machine runs even if little is left above the kernel. */
    uint32_t low =
        ((uint32_t)&_low_heap_start + (uint32_t)g_backbuf_size + 15U) & ~15U;
    uint32_t low_end = (uint32_t)&_kstack_bottom;
    uint32_t low_size = low_end > low ? low_end - low : 0;
    if (heap_top - heap_bot + low_size < 0x20000U) { /* under 128 KB in all */
        dbg_bar(5, 12);
        for (;;)
            __asm__ volatile("hlt");
    }

    dbg_bar(5, 3);

    /* Frame pool for apps (their code, heaps, page tables) at the top:
     * a quarter of the heap range, 128 KB .. 8 MB, page aligned. */
    uint32_t pool = (heap_top - heap_bot) / 4;
    if (heap_top - heap_bot < 0x100000) /* small machine: the minimum */
        pool = 0x20000;
    if (pool < 0x20000)
        pool = 0x20000;
    if (pool > PMM_MAX_FRAMES * PMM_PAGE_SIZE)
        pool = PMM_MAX_FRAMES * PMM_PAGE_SIZE;
    uint32_t pool_base = (heap_top - pool) & ~(PMM_PAGE_SIZE - 1);
    if (pool_base > heap_bot + 0x40000) {
        pmm_init(pool_base, (heap_top - pool_base) / PMM_PAGE_SIZE);
        heap_top = pool_base;
    }
    s_ram_top = total_ram > heap_top ? total_ram : heap_top;

    alloc_set_range(heap_bot, heap_top);
    alloc_add_low(low, low_end);

    dbg_bar(6, 7);
}

static void boot_banner(void) {
    clear_screen(BLACK);
    char title[32];
    sprintf(title, "ASMOS %s Boot", VER);
    draw_string(4, 2, title, WHITE, UI_FONT_BOLD);
    blit();
}

static void resolution_set(void) {
    video_limits_from_boot(); /* (also the VESA mode: g_vbe) */
    g_video_mode = *(volatile uint8_t *)0x0602;
    if (g_video_mode >= VIDEO_MODES)
        g_video_mode = VIDEO_640;
    g_screen_width = *(volatile uint16_t *)0x0604;
    g_screen_height = *(volatile uint16_t *)0x0606;

    if (g_screen_width == 0 || g_screen_height == 0) { /* (from the mode) */
        g_screen_width = video_size_w(video_mode_size(g_video_mode));
        g_screen_height = video_size_h(video_mode_size(g_video_mode));
    }

    if (g_screen_width > 800)
        g_screen_width = 800;
    if (g_screen_height > 600)
        g_screen_height = 600;

    g_backbuf_size = g_screen_width * g_screen_height;
    if (g_backbuf_size == 0 || g_backbuf_size > 800 * 600)
        g_backbuf_size = 800 * 600;
}

void kmain(void) {
    realmode_install(); /* (BIOS calls: the bank switch of a VESA mode) */
    resolution_set();

    detect_heap_range();
    cpu_init(); /* 386 safe: no CPUID unless present; sets up the FPU */
    idt_init();
    apm_init(); /* (stage 2 connected it: lib/apm.h) */
    ps2_init();

    gpu_init();

    boot_banner();

    boot_check_heap();
    pci_init(); /* before disks/NIC: they look for their controllers here */
    boot_check_pci();
    lpt_init();
    boot_check_input();
    /* what stage 2 left (boot/bootdefs.inc): the BIOS drive booted from,
     * and the volume on it (a partition's first sector; 0 on a floppy) */
    uint8_t boot_drive = *(volatile uint8_t *)0x0608;
    ata_init(boot_drive);
    video_cfg_init(boot_drive); /* where the mode is kept */
    fs_set_boot_volume(*(volatile uint32_t *)0x060C);
    fs_mount();
    /* started from the setup floppy: Setup runs (apps/setup.c) and the
     * hard disk is left alone until it says so */
    dir_entry_t setup_zip;
    g_setup_mode = boot_drive < 0x80 && fs_find(SETUP_ZIP, &setup_zip);
    boot_check_ata();
    boot_check_fat();

    cfg_init_defaults();
    if (!cfg_load()) { /* first boot: light settings on a 386 */
        if (cpu_family() <= 3)
            cfg_set_low_end(true);
        if (!g_setup_mode)
            cfg_save();
    }
    if (g_setup_mode)
        g_cfg.start_in_gui = true;
    joy_init(); /* the calibration is in g_cfg now */
    g_codepage = g_cfg.codepage < CP_COUNT ? g_cfg.codepage : CP_WESTERN;
    g_cfg.classic_fonts = 0; /* (gone from Settings: an old file had it) */

    /* Paging: app memory protection (lib/vmm.h). Settings > System can
     * turn it off (no_paging) should a machine misbehave with it. */
    if (!g_cfg.no_paging && vmm_init(s_ram_top))
        gpu_map(); /* a linear frame buffer is past the RAM: map it */
    if (g_cfg.fpu_emulate && cpu_has_fpu()) { /* Settings: test the emulator */
        cpu_emulate_fpu();
        idt_emulate_fpu();
    }

    if (g_cfg.sound_enabled) {
        speaker_init();
        sb16_init();
        opl2_init();
        sb16_unmute_fm();
    }

    boot_check_graphics();

    boot_check_sound();
    play_bootchime();

    if (g_cfg.networking_enabled && ne2000_init()) {
        net_init();
        dns_set_server(g_cfg.dns_server);
        if (g_cfg.net_dhcp) /* the address comes in a moment */
            dhcp_start();
        else
            net_set_ip(g_cfg.ip, g_cfg.gateway, g_cfg.netmask);
        arp_announce();
    }

    boot_check_network();

    if (!g_cfg.start_in_gui) {
        scheduler_init(); /* gopher and run are tasks in the text shell too */
        cli_run();
    }

    /* Settings > System: everything twice the size (drivers/gpu.h) */
    gpu_set_zoom(g_cfg.ui_zoom2 ? 2 : 1);
    mouse_apply(0, 0, false, false, false); /* the pointer on the smaller one */

    desktop_fs_init();

    wm_init();
    desktop_init();
    menubar_init();

    error_set_gui_mode(true);

    for (int i = 0; i < app_registry_count; i++)
        os_install_app(app_registry[i].desc);

    appload_scan_dir("/apps");

    if (g_setup_mode) {
        extern app_descriptor setup_app;
        os_launch_app(&setup_app);
    }

    scheduler_init();

    scheduler_kernel_task();
}
