#include "io/ps2.h"
#include "fs/fat16.h"
#include "ui/menubar.h"
#include "os/os.h"
#include "shell/cli.h"
#include "config/config.h"

extern app_descriptor finder_app;
extern app_descriptor terminal_app;

void kmain(void) {
    // hardware init
    ps2_init();
    fat16_mount();
    menubar_init();

    // Register applications
    os_register_app(&finder_app);
    os_register_app(&terminal_app);

    // Start directly in CLI or directly in the GUI
    if (!START_IN_GUI) cli_run();

    os_run();
}
