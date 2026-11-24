// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef FTP_H
#define FTP_H

#include "lib/core.h"

#define FTP_PORT 21

typedef struct {
    int ctl;  /* the control connection, -1 = none */
    int data; /* the data connection of a transfer, -1 = none */
    uint8_t ip[4];
    uint16_t port;
    bool busy;     /* a transfer: its last reply not read yet */
    int code;      /* the last reply: 0 = none came */
    char msg[128]; /* its last line, without the code */
    char lb[512];  /* the line being read */
    uint16_t ln;
} ftp_t;

/* each line of a reply (the code cut off); a multi-line one (FEAT) too */
typedef void (*ftp_line_cb)(const char *line, void *user);

void ftp_init(ftp_t *c);
/* connect and log in (TYPE I); false: c->code/msg say why (code 0: no
 * connection) */
bool ftp_open(ftp_t *c, const uint8_t ip[4], uint16_t port, const char *user,
              const char *pass);
bool ftp_alive(const ftp_t *c);
void ftp_close(ftp_t *c); /* QUIT, and closed */

/* one command, its reply: the code (0 = no reply, the connection is
 * dropped then) */
int ftp_cmd(ftp_t *c, const char *cmd);
int ftp_cmd_lines(ftp_t *c, const char *cmd, ftp_line_cb cb, void *user);

/* a transfer: PASV, the data connection, then cmd (RETR x, STOR x, LIST,
 * MLSD, APPE x); true: the data flows. rest: start at that byte (REST),
 * or 0 */
bool ftp_xfer_begin(ftp_t *c, const char *cmd, uint32_t rest);
/* some bytes (waits for them): 0 = the end, -1 = an error */
int ftp_xfer_read(ftp_t *c, uint8_t *buf, uint16_t n);
bool ftp_xfer_write(ftp_t *c, const uint8_t *buf, uint16_t n);
/* the data connection closed, the final reply read: true = 2xx. abort: cut
 * a download short (the connection is dropped if the server doesn't
 * answer) */
bool ftp_xfer_end(ftp_t *c, bool abort);

#endif
