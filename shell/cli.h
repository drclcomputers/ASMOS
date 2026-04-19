#ifndef SHELL_CLI_H
#define SHELL_CLI_H

#include "lib/core.h"

typedef enum {
    CMD_STATUS_OK = 0,
    CMD_STATUS_EXIT,
    CMD_STATUS_GUI,
    CMD_STATUS_CLEAR,
} cmd_status_t;

void cli_run(void);

cmd_status_t cli_execute_command(const char *cmd, char *out_buffer,
                                 size_t max_len);

#include "shell/term_buf.h"

void asmterm_input_push(char c);
void asmterm_input_push_enter(void);
int asmterm_output_read(char *dst, int max);
term_context_t *cli_asmterm_context(void);

#endif
