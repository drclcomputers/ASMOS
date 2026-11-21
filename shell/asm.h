#ifndef SHELL_ASM_H
#define SHELL_ASM_H

#include "lib/core.h"

/*
 * Assemble FAT16 source files or NUL-terminated strings into flat binaries.
 * Returns true on success; otherwise err_msg describes the error. Output is
 * written to out_buf and its length to out_len.
 */

#define ASM_OUT_MAX 16384

bool asm_assemble_file(const char *src_path, uint8_t *out_buf, int *out_len,
                       int buf_max, char *err_msg, int err_max);

bool asm_assemble_str(const char *src, uint8_t *out_buf, int *out_len,
                      int buf_max, char *err_msg, int err_max);

void cmd_asmasm(const char *args, char *out, size_t max);

#endif
