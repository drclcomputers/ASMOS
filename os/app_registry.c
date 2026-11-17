#include "os/app_registry.h"

extern app_descriptor asmterm_app;
extern app_descriptor filef_app;
extern app_descriptor monitor_app;
extern app_descriptor printq_app;
extern app_descriptor fdformat_app;
extern app_descriptor settings_app;
extern app_descriptor teditor_app;
extern app_descriptor setup_app;

const char *const APP_GROUP_NAMES[APP_GROUPS] = {"", "Accessories", "Games",
                                                 "Multimedia", "System Tools"};

registered_app_t app_registry[] = {
    {&filef_app, "FileF", APP_TOP, false},
    {&settings_app, "Settings", APP_TOP, false},

    {&teditor_app, "TEditor", APP_ACCESSORIES, false},

    {&asmterm_app, "ASMTerm", APP_SYSTEM, false},
    {&monitor_app, "Monitor", APP_SYSTEM, false},
    {&fdformat_app, "Format Disk", APP_SYSTEM, false},
    {&printq_app, "Print Queue", APP_SYSTEM, false},
    {&setup_app, "Setup", APP_SYSTEM, true},
};

int app_registry_count = sizeof(app_registry) / sizeof(app_registry[0]);
