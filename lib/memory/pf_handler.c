#include "lib/memory/vmm.h"
#include "os/os.h"
#include "os/scheduler.h"
#include "lib/graphics.h"
#include "lib/string.h"
#include "lib/memory.h"

#define PF_PRESENT   (1u << 0)  // 0=not-present, 1=protection violation
#define PF_WRITE     (1u << 1)  // 0=read, 1=write
#define PF_USER      (1u << 2)  // 0=supervisor, 1=user mode
#define PF_RSVD      (1u << 3)  // reserved bit violation
#define PF_IFETCH    (1u << 4)  // instruction fetch (NX)

static void kill_faulting_app(uint32_t fault_addr, uint32_t err, uint32_t eip) {
    int slot = current_task;

    app_instance_t *victim = NULL;
    for (int i = 0; i < MAX_RUNNING_APPS; i++) {
        if (running_apps[i].running && running_apps[i].task_slot == slot) {
            victim = &running_apps[i];
            break;
        }
    }

    char msg[128];
    const char *app_name = (victim && victim->desc && victim->desc->name)
                           ? victim->desc->name
                           : "Unknown App";

    const char *kind;
    if (err & PF_RSVD)          kind = "reserved-bit violation";
    else if (err & PF_IFETCH)   kind = "instruction fetch violation";
    else if (err & PF_WRITE)    kind = kind = (err & PF_PRESENT) ? "write protection fault" : "write to unmapped page";
    else                        kind = (err & PF_PRESENT) ? "read protection fault" : "read from unmapped page";

    {
        int mi = 0;
        for (int i = 0; app_name[i] && mi < 60; i++)
            msg[mi++] = app_name[i];
        msg[mi++] = ':'; msg[mi++] = ' ';
        for (int i = 0; kind[i] && mi < 100; i++)
            msg[mi++] = kind[i];
        const char *at_str = "\nat 0x";
        for (int i = 0; at_str[i] && mi < 110; i++)
            msg[mi++] = at_str[i];
        for (int shift = 28; shift >= 0 && mi < 118; shift -= 4) {
            uint8_t nib = (fault_addr >> shift) & 0xF;
            msg[mi++] = nib < 10 ? ('0' + nib) : ('A' + nib - 10);
        }
        const char *eip_str = " (EIP 0x";
        for (int i = 0; eip_str[i] && mi < 122; i++)
            msg[mi++] = eip_str[i];
        for (int shift = 28; shift >= 0 && mi < 122; shift -= 4) {
            uint8_t nib = (eip >> shift) & 0xF;
            msg[mi++] = nib < 10 ? ('0' + nib) : ('A' + nib - 10);
        }
        if (mi < 126) msg[mi++] = ')';
        msg[mi] = '\0';
    }

    vmm_load_kernel();

    extern void modal_show(int type, const char *title, const char *msg,
                           void (*on_confirm)(void), void (*on_cancel)(void));
    modal_show(2, "Page Fault — App Terminated", msg, NULL, NULL);

    if (victim)
        os_quit_app(victim);

    task_yield();

    for (;;)
        __asm__ volatile("hlt");
}

void pf_handler_c(uint32_t eip, uint32_t error_code, uint32_t fault_addr) {
    if (current_task == 0) {
        extern int g_screen_width, g_screen_height;
        (void)eip; (void)error_code; (void)fault_addr;
        __asm__ volatile("cli");
        for (;;) __asm__ volatile("hlt");
    }

    kill_faulting_app(fault_addr, error_code, eip);
}
