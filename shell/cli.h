#ifndef CLI_H
#define CLI_H

#include "lib/types.h"

typedef enum {
    CMD_STATUS_OK = 0,
    CMD_STATUS_EXIT,
    CMD_STATUS_GUI,
    CMD_STATUS_CLEAR
} cmd_status_t;

void cli_run(void);
cmd_status_t cli_execute_command(const char *cmd, char *out_buffer, size_t max_len);

void cmd_help(char *out, size_t max);
void cmd_pwd(char *out, size_t max);
void cmd_ls(char *out, size_t max);
void cmd_cat(const char *filename, char *out, size_t max);
void cmd_touch(const char *filename, char *out, size_t max);
void cmd_rm(const char *args, char *out, size_t max);
void cmd_mkdir(const char *dirname, char *out, size_t max);
void cmd_rmdir(const char *dirname, char *out, size_t max);
void cmd_df(char *out, size_t max);
void cmd_mem(char *out, size_t max);
void cmd_mv(const char *args, char *out, size_t max);
void cmd_cp(const char *args, char *out, size_t max);
void cmd_write(const char *args, char *out, size_t max);
void cmd_echo(const char *text, char *out, size_t max);
void cmd_clock(char *out, size_t max);

#endif
