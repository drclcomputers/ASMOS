#include "config/video_cfg.h"
#include "drivers/ata.h"
#include "drivers/lpt.h"
#include "drivers/pci.h"
#include "drivers/ps2.h"
#include "fs/fs.h"
#include "lib/cpu.h"
#include "ui/uifont.h"

#include "drivers/gpu.h"
#include "drivers/ne2000.h"
#include "drivers/opl2.h"
#include "drivers/sb16.h"

#include "network/dns.h"
#include "network/net.h"

#include "os/app_registry.h"
#include "os/appload.h"
#include "os/error.h"
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

    if (g_video_mode == 0) {
        volatile uint8_t *fb = (volatile uint8_t *)0xA0000;
        int x = col * 8;
        if (x + 8 > g_screen_width)
            return;
        for (int y = 0; y < g_screen_height; y++)
            for (int i = 0; i < 8; i++)
                fb[y * g_screen_width + x + i] = color;
    } else {
        volatile uint8_t *fb = (volatile uint8_t *)0xA0000;
        int bx = col;
        if (bx >= 80)
            return;
        for (int y = 0; y < g_screen_height; y++)
            fb[y * 80 + bx] = color;
    }
}

unsigned int g_ram_bytes;
static uint32_t s_ram_top; /* end of RAM (or of what we use of it) */

static void detect_heap_range(void) {
    dbg_bar(0, 4);

    uint32_t heap_bot = (uint32_t)BACKBUF + (uint32_t)g_backbuf_size;

    uint32_t kernel_end = (uint32_t)&_heap_start;
    if (kernel_end > heap_bot)
        heap_bot = kernel_end;

    if (heap_bot < HEAP_MIN_START)
        heap_bot = HEAP_MIN_START;

    heap_bot = (heap_bot + 15U) & ~15U;

    dbg_bar(1, 2);
    dbg_bar(2, 1);

    uint32_t total_ram = *(volatile uint32_t *)0x0500;
    g_ram_bytes = total_ram;

    dbg_bar(3, 6);

    uint32_t heap_top;
    if (total_ram == 0 || total_ram <= heap_bot) {
        heap_top = heap_bot + 0x80000;
    } else {
        heap_top = total_ram - 0x10000;
    }

    if (heap_top > HEAP_END_MAX)
        heap_top = HEAP_END_MAX;

    dbg_bar(4, 5);

    if (heap_top <= heap_bot + 0x10000U) {
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
    if (pool_base > heap_bot + 0x40000) { /* keep >= 256 KB kernel heap */
        pmm_init(pool_base, (heap_top - pool_base) / PMM_PAGE_SIZE);
        heap_top = pool_base;
    }
    s_ram_top = total_ram > heap_top ? total_ram : heap_top;

    alloc_set_range(heap_bot, heap_top);

    /* what is left below 640 KB between the kernel and its stack */
    alloc_add_low((uint32_t)&_low_heap_start, (uint32_t)&_kstack_bottom);

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
    g_video_mode = *(volatile uint8_t *)0x0602;
    g_screen_width = *(volatile uint16_t *)0x0604;
    g_screen_height = *(volatile uint16_t *)0x0606;

    if (g_screen_width == 0 || g_screen_height == 0) {
        if (RESMODE) {
            g_video_mode = 1;
            g_screen_width = 640;
            g_screen_height = 400;
        } else {
            g_video_mode = 0;
            g_screen_width = 320;
            g_screen_height = 200;
        }
    }

    if (g_screen_width > 640)
        g_screen_width = 640;
    if (g_screen_height > 400)
        g_screen_height = 400;

    g_backbuf_size = g_screen_width * g_screen_height;
    if (g_backbuf_size == 0 || g_backbuf_size > 640 * 400)
        g_backbuf_size = 640 * 400;
}

void kmain(void) {
    resolution_set();

    detect_heap_range();
    cpu_init(); /* 386 safe: no CPUID unless present; sets up the FPU */
    idt_init();
    ps2_init();

    gpu_init();

    boot_banner();

    boot_check_heap();
    pci_init(); /* before disks/NIC: they look for their controllers here */
    boot_check_pci();
    lpt_init();
    boot_check_input();
    ata_init(*(volatile uint8_t *)0x0608); /* BIOS boot drive from stage 1 */
    video_cfg_init(*(volatile uint8_t *)0x0608); /* where the mode is kept */
    fs_mount();
    boot_check_ata();
    boot_check_fat();

    cfg_init_defaults();
    if (!cfg_load()) { /* first boot: light settings on a 386 */
        if (cpu_family() <= 3)
            cfg_set_low_end(true);
        cfg_save();
    }

    /* Paging: app memory protection (lib/vmm.h). Settings > System can
     * turn it off (no_paging) should a machine misbehave with it. */
    if (!g_cfg.no_paging)
        vmm_init(s_ram_top);
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
        net_set_ip(g_cfg.ip, g_cfg.gateway, g_cfg.netmask);
        dns_set_server(g_cfg.dns_server);
        arp_announce();
    }

    boot_check_network();

    if (!g_cfg.start_in_gui)
        cli_run();

    desktop_fs_init();

    wm_init();
    desktop_init();
    menubar_init();

    error_set_gui_mode(true);

    for (int i = 0; i < app_registry_count; i++)
        os_install_app(app_registry[i].desc);

    appload_scan_dir("/apps"); /* external .APP packages, if any */

    scheduler_init();

    scheduler_kernel_task();
}
