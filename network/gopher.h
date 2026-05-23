#ifndef GOPHER_H
#define GOPHER_H

#include "lib/core.h"

void gopher_run(const uint8_t start_ip[4], uint16_t port, const char *selector);
void gopher_run_host(const char *hostname, uint16_t port, const char *selector);
bool parse_ip_str(const char *str, uint8_t ip[4]);

#endif
