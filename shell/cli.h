#ifndef CLI_H
#define CLI_H

#include "lib/types.h"

void cli_run(void);
bool cli_execute_command(const char *cmd);

void cmd_help(void);
void cmd_clear(void);
void cmd_ls(void);
void cmd_cat(const char *filename);
void cmd_gui(void);
void cmd_exit(void);

void cmd_touch(const char *filename);
void cmd_rm(const char *filename);
void cmd_mkdir(const char *dirname);
void cmd_rmdir(const char *dirname);
void cmd_df(void);
void cmd_mem(void);
void cmd_mv(const char *args);
void cmd_cp(const char *args);
void cmd_write(const char *args);
void cmd_echo(const char *text);
void cmd_clock();

#endif
