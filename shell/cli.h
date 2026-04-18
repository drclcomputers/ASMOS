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

void cmd_run(const char *path, char *out, size_t max);
void cmd_asmasm(const char *args, char *out, size_t max);

/* Offsets (for use in ASM source):
    SC_PRINT    equ 0    ; void  print   (const char *str)
    SC_PUTCHAR  equ 4    ; void  putchar (char c)
    SC_READLINE equ 8    ; int   readline(char *buf, int max_chars)
    SC_GETCHAR  equ 12   ; int   getchar (void)   -- waits for keypress
    SC_ITOA     equ 16   ; void  itoa    (int val, char *buf_12bytes)
 */
#define ASM_NUL_SENTINEL ((char)0x01)
typedef struct {
    void (*sc_print)   (const char *str);
    void (*sc_putchar) (char c);
    int  (*sc_readline)(char *buf, int maxchars);
    int  (*sc_getchar) (void);
    void (*sc_itoa)    (int val, char *buf);
} bin_syscall_t;

#define TERM_BUF_LINES  256
#define TERM_BUF_LINE_W 128

void        term_buf_push     (const char *line);
void        term_buf_push_text(const char *text);
int         term_buf_count    (void);
const char *term_buf_get      (int i);
void        term_buf_clear    (void);
int         term_buf_read_all (char *dst, int max);
int         term_buf_read_new (char *dst, int max, int *cursor);
bool        term_buf_save     (const char *path);

#endif
