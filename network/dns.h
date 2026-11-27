// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef DNS_H
#define DNS_H

#include "lib/core.h"

#define DNS_PORT 53
#define DNS_LOCAL 49153
#define DNS_TIMEOUT_MS 3000
#define DNS_RETRIES 3

/* waits for the answer (up to DNS_RETRIES x DNS_TIMEOUT_MS) */
bool dns_resolve(const char *hostname, uint8_t ip_out[4]);
int dns_start(const char *hostname);
int dns_poll(int lookup, uint8_t ip_out[4]);
void dns_cancel(int lookup);
void dns_set_server(const uint8_t ip[4]);
void dns_get_server(uint8_t ip[4]);

#endif
