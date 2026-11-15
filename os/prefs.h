#ifndef OS_PREFS_H
#define OS_PREFS_H

/*
 * Per-app saved settings: one text file per app, /PREFS/<APP>.INI on HDA,
 * lines "key=value". Keys and values are short (keys < 24, values < 96
 * chars). Reads go to disk each time: call them in init, not per frame.
 *
 *   s->pen = app_prefs_get_int("ASMDRAW", "pen", BLACK);
 *   app_prefs_set_int("ASMDRAW", "pen", s->pen);   // e.g. in destroy
 *
 * Setting "" removes the key. Several changes at once: app_prefs_begin(app) ...
 * set ... app_prefs_end() writes the file once.
 */

#include "lib/core.h"

bool app_prefs_get(const char *app, const char *key, char *out, int sz);
int app_prefs_get_int(const char *app, const char *key, int def);
bool app_prefs_set(const char *app, const char *key, const char *value);
bool app_prefs_set_int(const char *app, const char *key, int value);

void app_prefs_begin(const char *app);
bool app_prefs_end(void);

#endif
