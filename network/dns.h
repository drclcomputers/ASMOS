#ifndef DNS_H
#define DNS_H

#include "lib/core.h"

#define DNS_PORT 53
#define DNS_LOCAL 49153
#define DNS_TIMEOUT_MS 3000
#define DNS_RETRIES 3

bool dns_resolve(const char *hostname, uint8_t ip_out[4]);
void dns_set_server(const uint8_t ip[4]);

#endif
