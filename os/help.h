// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef OS_HELP_H
#define OS_HELP_H

/*
 * Help lives in /HELP/<App>/<page>.TXT; Built-in pages ship with ASMOS. .APP
 * pages are installed on first run and refreshed when the package changes; apps
 * with help get a View Help menu item.
 */

#include "os/os.h"

#define HELP_DIR "/HELP"

/* the folder for an app's name ("Audio Player" -> "Audio_Player") */
void help_folder_of(const char *app, char *out, int sz);
/* the app has pages */
bool help_has(const char *app);
/* after an app's init (os_launch_app): its own pages, its Help menu */
void help_attach(app_instance_t *a);
/* the name an app shows (the Apps menu's for a built-in one) */
const char *help_app_name(const app_descriptor *d);

/* the Help app, at a page or folder (a path in /HELP, or NULL: its start);
 * apps/help.c */
void help_open(const char *path);
/* the Help app, at an app's folder */
void help_open_app(const char *app);

#endif
