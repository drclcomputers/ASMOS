#include "fs/fs.h"
#include "io/ps2.h"

#include "drivers/gpu.h"
#include "drivers/ne2000.h"
#include "drivers/opl2.h"
#include "drivers/sb16.h"

#include "network/net.h"

#include "os/app_registry.h"
#include "os/error.h"
#include "os/os.h"
#include "os/scheduler.h"
#include "shell/cli.h"

#include "config/config.h"
#include "config/runtime_config.h"

#include "interrupts/idt.h"
#include "lib/device.h"
#include "lib/graphics.h"
#include "lib/memory.h"
#include "lib/time.h"

#include "ui/ui.h"

extern uint8_t _heap_start;
int g_screen_width = 640;
int g_screen_height = 400;
int g_video_mode = 1;
int g_backbuf_size = 640 * 400;

extern void wm_init(void);
extern void scheduler_init(void);
extern void desktop_on_frame(void);

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
        } else if (sb16_detected()) {
            speaker_beep(523, 120);
            speaker_beep(659, 120);
            speaker_beep(784, 180);
        } else {
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

static void detect_heap_range(void) {
    dbg_bar(0, 4);

    uint32_t kernel_end = (uint32_t)&_heap_start;
    if (kernel_end < HEAP_MIN_START)
        kernel_end = HEAP_MIN_START;

    kernel_end = (kernel_end + 15U) & ~15U;

    dbg_bar(1, 2);

    dbg_bar(2, 1);
    uint32_t total_ram = *(volatile uint32_t *)0x0500;

    dbg_bar(3, 6);

    uint32_t heap_top;
    if (total_ram == 0) {
        heap_top = HEAP_END_MAX;
    } else {
        heap_top = total_ram;
        if (heap_top > HEAP_END_MAX)
            heap_top = HEAP_END_MAX;
    }

    dbg_bar(4, 5);

    if (heap_top <= kernel_end + 0x10000U) {
        dbg_bar(5, 12);
        for (;;)
            __asm__ volatile("hlt");
    }

    dbg_bar(5, 3);

    alloc_set_range(kernel_end, heap_top);

    dbg_bar(6, 7);
}

static void boot_banner(void) {
    clear_screen(BLACK);
    draw_string(4, 2, "ASMOS Boot", WHITE, 2);
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
    idt_init();
    ps2_init();

    gpu_init();

    sleep_s(1);
    boot_banner();
    sleep_s(1);

    boot_check_heap();
    fs_mount();
    boot_check_ata();
    boot_check_fat();

    cfg_init_defaults();
    if (!cfg_load()) {
        cfg_save();
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

    if (ne2000_init()) {
        net_init();
        static const uint8_t my_ip[4] = {10, 0, 2, 15};
        static const uint8_t my_gw[4] = {10, 0, 2, 2};
        static const uint8_t my_nm[4] = {255, 255, 255, 0};
        net_set_ip(my_ip, my_gw, my_nm);
        arp_announce();
    }

    sleep_s(2);

    {
        volatile uint32_t d = 0x1000000;
        while (d--)
            __asm__ volatile("nop");
    }

    if (!g_cfg.start_in_gui)
        cli_run();

    desktop_fs_init();

    wm_init();
    desktop_init();
    menubar_init();

    error_set_gui_mode(true);

    for (int i = 0; i < app_registry_count; i++)
        os_install_app(app_registry[i].desc);

    scheduler_init();

    scheduler_kernel_task();
}
