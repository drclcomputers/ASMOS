#ifndef CLI_H
#define CLI_H

#include "lib/core.h"

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

/* ─── Terminal ring buffer API ───────────────────────────────────────────────
 *
 * ASMTerm and other apps write lines into a global ring buffer so that any
 * code can read recently-printed output programmatically.
 *
 * Usage:
 *   term_buf_push("some output line");      // called by output consumers
 *   int n = term_buf_count();               // how many lines are stored
 *   const char *line = term_buf_get(i);     // get line i (0 = oldest)
 *   term_buf_clear();                       // clear the ring
 *
 * The buffer holds up to TERM_BUF_LINES lines of up to TERM_BUF_LINE_W chars.
 * When full, the oldest line is overwritten.
 *
 * term_buf_read_all(dst, max) copies all stored lines (newline-separated)
 * into dst, returning the number of bytes written.  Handy for piping output
 * into a file or another command.
 */

#define TERM_BUF_LINES   256
#define TERM_BUF_LINE_W  128

void term_buf_push(const char *line);

void term_buf_push_text(const char *text);

int  term_buf_count(void);

const char *term_buf_get(int i);

void term_buf_clear(void);

int  term_buf_read_all(char *dst, int max);

int  term_buf_read_new(char *dst, int max, int *cursor);

bool term_buf_save(const char *path);

#endif
