#ifndef SHELL_ASM_H
#define SHELL_ASM_H

#include "lib/core.h"

/*
 * x86 flat-binary assembler.
 *
 * Assemble a source file from FAT16 into a flat binary:
 *   bool asm_assemble_file(const char *src_path,
 *                          uint8_t *out_buf, int *out_len, int buf_max,
 *                          char *err_msg,    int  err_max);
 *
 * Returns true on success; false with a human-readable message in err_msg.
 * The output bytes are written to out_buf, *out_len is the byte count.
 *
 * Assemble from a NUL-terminated in-memory string (e.g. HELLO_ASM macro):
 *   bool asm_assemble_str(const char *src,
 *                         uint8_t *out_buf, int *out_len, int buf_max,
 *                         char *err_msg,    int  err_max);
 */

#define ASM_OUT_MAX 16384

bool asm_assemble_file(const char *src_path, uint8_t *out_buf, int *out_len,
                       int buf_max, char *err_msg, int err_max);

bool asm_assemble_str(const char *src, uint8_t *out_buf, int *out_len,
                      int buf_max, char *err_msg, int err_max);

void cmd_asmasm(const char *args, char *out, size_t max);

#endif
