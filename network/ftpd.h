// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef FTPD_H
#define FTPD_H

#include "lib/core.h"

#define FTPD_SESSIONS 4

/* false: *err says why (the port is taken, no network, ...) */
bool ftpd_start(uint16_t port, bool read_only, const char *password,
                const char **err);
void ftpd_stop(void);
bool ftpd_running(void);
uint16_t ftpd_port(void);
bool ftpd_read_only(void);
int ftpd_clients(void);         /* connected now */
uint32_t ftpd_files_sent(void); /* and received: since started */
uint32_t ftpd_files_got(void);

/* one round of serving (the task does it; tests call it themselves) */
void ftpd_poll(void);

#endif
