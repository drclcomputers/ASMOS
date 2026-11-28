// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "os/app_registry.h"

extern app_descriptor asmterm_app;
extern app_descriptor filef_app;
extern app_descriptor monitor_app;
extern app_descriptor printq_app;
extern app_descriptor fdformat_app;
extern app_descriptor settings_app;
extern app_descriptor teditor_app;
extern app_descriptor setup_app;
extern app_descriptor charmap_app;
extern app_descriptor joyconf_app;
extern app_descriptor mousecfg_app;
extern app_descriptor help_app;

const char *const APP_GROUP_NAMES[APP_GROUPS] = {"", "Accessories", "Games",
                                                 "Multimedia", "System Tools"};

registered_app_t app_registry[] = {
    {&filef_app, "FileF", APP_TOP, false, false},
    {&settings_app, "Settings", APP_TOP, false, false},
    {&help_app, "Help", APP_TOP, false, false},

    {&teditor_app, "TEditor", APP_ACCESSORIES, false, false},
    {&charmap_app, "Character Map", APP_ACCESSORIES, false, false},

    {&asmterm_app, "ASMTerm", APP_SYSTEM, false, false},
    {&monitor_app, "Monitor", APP_SYSTEM, false, false},
    {&fdformat_app, "Format Disk", APP_SYSTEM, false, false},
    {&printq_app, "Print Queue", APP_SYSTEM, false, true},
    {&joyconf_app, "Joystick", APP_SYSTEM, false, true},
    {&mousecfg_app, "Mouse", APP_SYSTEM, false, true},
    {&setup_app, "Setup", APP_SYSTEM, true, false},
};

int app_registry_count = sizeof(app_registry) / sizeof(app_registry[0]);
