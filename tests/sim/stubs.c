/* hardware stubs discovered at link time */
#include "lib/core.h"
bool midi_player_load(const uint8_t *d, uint32_t n) {
    (void)d;
    (void)n;
    return true;
}
static bool playing;
void midi_player_play(bool l) {
    (void)l;
    playing = true;
}
void midi_player_pause(void) { playing = false; }
void midi_player_stop(void) { playing = false; }
bool midi_player_is_playing(void) { return playing; }
void midi_player_set_loop(bool l) { (void)l; }
void mixer_write(uint8_t r, uint8_t v) {
    (void)r;
    (void)v;
}
int g_sim_hw;
bool ne2000_detected(void) { return g_sim_hw; }
void ne2000_get_mac(uint8_t m[6]) {
    for (int i = 0; i < 6; i++)
        m[i] = 0x52 + i;
}
bool opl2_detected(void) { return g_sim_hw; }
void opl2_set_master_volume(uint8_t v) { (void)v; }
bool sb16_detected(void) { return g_sim_hw; }
void sb16_set_volume(uint8_t l, uint8_t r) {
    (void)l;
    (void)r;
}
void speaker_beep(uint32_t f, uint32_t d) {
    (void)f;
    (void)d;
}
void speaker_update(void) {}

/* hardware detection (not simulated) */
#include "drivers/ata.h"
#include "drivers/pci.h"
bool cpu_has_fpu(void) { return true; }
int cpu_family(void) { return 5; }
void cpu_model_init(void) {}
const char *cpu_model_str(void) {
    return g_sim_hw ? "486-class CPU" : "Simulated CPU";
}
const char *ata_controller_desc(void) {
    return g_sim_hw ? "legacy ports" : "sim";
}
uint16_t g_ne_base = 0x300;
const char *ne2000_bus(void) { return "ISA"; }
uint16_t ne2000_io_base(void) { return 0x300; }
int ne2000_irq(void) { return g_sim_hw ? 3 : -1; }
uint16_t pci_bar_io(const pci_dev_t *d, int i) {
    (void)d;
    (void)i;
    return 0;
}
const char *pci_class_name(uint8_t c, uint8_t s) {
    (void)c;
    (void)s;
    return "?";
}
int pci_count(void) { return 0; }
const pci_dev_t *pci_get(int i) {
    (void)i;
    return NULL;
}
int pci_mechanism(void) { return 0; }
#include "drivers/mouse.h"
uint32_t gpu_fps(void) { return 60; }
mouse_kind_t mouse_kinds(void) { return MOUSE_PS2; }
int sermouse_port(void) { return 0; }
#include "drivers/serial.h"
uint16_t com_base(int n) { return g_sim_hw && n == 0 ? 0x3F8 : 0; }
#include "drivers/lpt.h"
#include "drivers/sb16.h"
/* fake card: the position runs on the timer */
extern volatile uint32_t pit_ticks;
static bool sbp, sbpause, sbloop;
static uint32_t sb_len, sb_rate, sb_t0, sb_off;
static uint32_t sb_now(void) {
    if (!sbp)
        return 0;
    uint32_t p = sb_off + (sbpause ? 0 : (pit_ticks - sb_t0) * sb_rate / 1000);
    if (p >= sb_len) {
        if (sbloop) {
            sb_off = 0;
            sb_t0 = pit_ticks;
            return 0;
        }
        sbp = false;
        return sb_len;
    }
    return p;
}
bool sb16_is_playing(void) {
    sb_now();
    return sbp;
}
void sb16_play_pcm(const uint8_t *d, uint32_t l, uint32_t r, bool s) {
    (void)d;
    (void)s;
    sbp = true;
    sbpause = false;
    sb_len = l;
    sb_rate = r;
    sb_t0 = pit_ticks;
    sb_off = 0;
}
void sb16_set_loop(bool l) { sbloop = l; }
void sb16_stop(void) { sbp = false; }
void sb16_pause(bool p) {
    if (!sbp)
        return;
    if (p && !sbpause) {
        sb_off = sb_now();
        sbpause = true;
    } else if (!p && sbpause) {
        sbpause = false;
        sb_t0 = pit_ticks;
    }
}
bool sb16_paused(void) { return sbp && sbpause; }
uint32_t sb16_position(void) { return sb_now(); }
void sb16_seek(uint32_t o) {
    sb_off = o;
    sb_t0 = pit_ticks;
}
int sb_dma(void) { return 1; }
int sb_dsp_version(void) { return 0x405; }
int sb_irq(void) { return 5; }
const char *sb_model_name(void) { return "Sound Blaster 16"; }
uint16_t sb_port(void) { return 0x220; }
/* watchdog needs (no real screen/mouse in the sim) */
void mouse_process_byte(unsigned char b) { (void)b; }
void sermouse_poll(void) {}
bool cpu_fpu_emulated(void) { return false; }
void speaker_beep_async(uint32_t f, uint32_t d) {
    (void)f;
    (void)d;
}
unsigned int g_ram_bytes = 2048 * 1024;
unsigned short sim_bda(unsigned int off) {
    (void)off;
    return 0;
}
void midi_player_update(void) {}
unsigned short apm_version(void) { return 0; }
int apm_present(void) { return 0; }
