#include "os/app_registry.h"

extern app_descriptor asmasm_app;
extern app_descriptor asmdraw_app;
extern app_descriptor asmterm_app;
extern app_descriptor asmusic_app;
extern app_descriptor calculator_app;
extern app_descriptor clock_app;
extern app_descriptor filef_app;
extern app_descriptor monitor_app;
extern app_descriptor settings_app;
extern app_descriptor teditor_app;

registered_app_t app_registry[] = {
    {&asmasm_app, "ASMembler"},      {&asmdraw_app, "ASMDraw"},
    {&asmterm_app, "ASMTerm"},       {&asmusic_app, "ASMusic"},
    {&calculator_app, "Calculator"}, {&clock_app, "Clock"},
    {&filef_app, "FileF"},           {&monitor_app, "Monitor"},
    {&settings_app, "Settings"},     {&teditor_app, "TEditor"},
};

int app_registry_count = sizeof(app_registry) / sizeof(app_registry[0]);
