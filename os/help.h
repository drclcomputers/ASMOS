#ifndef OS_HELP_H
#define OS_HELP_H

/*
 * Help pages: /HELP/<App>/<page>.TXT, a folder per app (its name with
 * anything a file name can't have as '_': /HELP/Audio_Player). The Help
 * app (apps/help.c) shows them.
 *
 * The built-in apps' pages come with ASMOS (help/ in the source, put on
 * the disk by tools/mkdisk.py, back again when ASMOS is installed again).
 * An .APP can carry its own (tools/mkapp.py --help DIR): they are copied
 * to /HELP the first time it runs, and again only when the .APP file
 * changes (installed again, updated): pages deleted stay deleted until
 * then. An app with a /HELP folder gets a Help menu with View Help.
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
