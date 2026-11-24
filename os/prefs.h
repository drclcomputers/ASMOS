// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef OS_PREFS_H
#define OS_PREFS_H

/*
 * Per-app settings live in /PREFS/<APP>.INI as key=value lines (keys <24,
 * values <96 chars). Reads access disk, so load in init, not per frame.
 * Empty values remove keys; app_prefs_begin/end batches writes.
 */

#include "lib/core.h"

bool app_prefs_get(const char *app, const char *key, char *out, int sz);
int app_prefs_get_int(const char *app, const char *key, int def);
bool app_prefs_set(const char *app, const char *key, const char *value);
bool app_prefs_set_int(const char *app, const char *key, int value);

void app_prefs_begin(const char *app);
bool app_prefs_end(void);

#endif
