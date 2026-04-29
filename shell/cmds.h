#ifndef SHELL_CLI_CMDS_H
#define SHELL_CLI_CMDS_H

#include "lib/core.h"
#include "shell/term_buf.h"

void cmd_help(char *out, size_t max);
void cmd_pwd(char *out, size_t max);
void cmd_ls(const char *path, char *out, size_t max);
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
void cmd_shutdown(const char *args, char *out, size_t max);
void cmd_restart(const char *args, char *out, size_t max);
void cmd_tee(const char *filename, char *out, size_t max);
void cmd_history(char *out, size_t max);
void cmd_cd(const char *path, char *out, size_t max);

#endif
