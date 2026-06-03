#ifndef GOPHER_H
#define GOPHER_H

#include "lib/core.h"
#include "shell/cli.h"
#include "shell/term_buf.h"

void gopher_run(term_context_t *ctx, const uint8_t start_ip[4], uint16_t port,
                const char *selector);
void gopher_run_host(term_context_t *ctx, const char *hostname, uint16_t port,
                     const char *selector);
bool parse_ip_str(const char *str, uint8_t ip[4]);

void asmterm_set_gopher_mode(term_context_t *ctx, bool on);

#endif
