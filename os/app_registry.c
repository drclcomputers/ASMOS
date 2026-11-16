#include "os/app_registry.h"

extern app_descriptor asmdraw_app;
extern app_descriptor asmterm_app;
extern app_descriptor calculator_app;
extern app_descriptor clipview_app;
extern app_descriptor clock_app;
extern app_descriptor filef_app;
extern app_descriptor hexview_app;
extern app_descriptor audioplayer_app;
extern app_descriptor monitor_app;
extern app_descriptor puzzle_app;
extern app_descriptor calendar_app;
extern app_descriptor printq_app;
extern app_descriptor fdformat_app;
extern app_descriptor settings_app;
extern app_descriptor teditor_app;

const char *const APP_GROUP_NAMES[APP_GROUPS] = {"", "Accessories", "Games",
                                                 "Multimedia", "System Tools"};

/* in menu order within each group */
registered_app_t app_registry[] = {
    {&filef_app, "FileF", APP_TOP},
    {&settings_app, "Settings", APP_TOP},

    {&calculator_app, "Calculator", APP_ACCESSORIES},
    {&calendar_app, "Calendar", APP_ACCESSORIES},
    {&clock_app, "Clock", APP_ACCESSORIES},
    {&clipview_app, "ClipView", APP_ACCESSORIES},
    {&teditor_app, "TEditor", APP_ACCESSORIES},
    {&asmdraw_app, "ASMDraw", APP_ACCESSORIES},

    {&puzzle_app, "Puzzle", APP_GAMES},

    {&audioplayer_app, "Audio Player", APP_MULTIMEDIA},

    {&asmterm_app, "ASMTerm", APP_SYSTEM},
    {&monitor_app, "Monitor", APP_SYSTEM},
    {&hexview_app, "HexView", APP_SYSTEM},
    {&fdformat_app, "Format Disk", APP_SYSTEM},
    {&printq_app, "Print Queue", APP_SYSTEM},
};

int app_registry_count = sizeof(app_registry) / sizeof(app_registry[0]);
