#include "os/app_registry.h"

extern app_descriptor asmdraw_app;
extern app_descriptor asmterm_app;
extern app_descriptor asmusic_app;
extern app_descriptor aswav_app;
extern app_descriptor calculator_app;
extern app_descriptor clipview_app;
extern app_descriptor clock_app;
extern app_descriptor filef_app;
extern app_descriptor hexview_app;
extern app_descriptor monitor_app;
extern app_descriptor settings_app;
extern app_descriptor teditor_app;

registered_app_t app_registry[] = {
    {&asmdraw_app, "ASMDraw"},       {&asmterm_app, "ASMTerm"},
    {&asmusic_app, "ASMusic"},       {&aswav_app, "ASWAV"},
    {&calculator_app, "Calculator"}, {&clock_app, "Clock"},
    {&clipview_app, "ClipView"},     {&filef_app, "FileF"},
    {&hexview_app, "HexView"},       {&monitor_app, "Monitor"},
    {&settings_app, "Settings"},     {&teditor_app, "TEditor"},
};

int app_registry_count = sizeof(app_registry) / sizeof(app_registry[0]);
