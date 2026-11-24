// Copyright (c) 2026 @drclcomputers. All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#ifndef NETFS_H
#define NETFS_H

#include "fs/fs.h"

bool netfs_connect(const char *host, uint16_t port, const char *user,
                   const char *pass, const char *dir, const char **err);
void netfs_disconnect(void);
bool netfs_connected(void);
/* "ftp://user@host:port/dir" */
void netfs_describe(char *out, int n);
const char *netfs_last_error(void);

typedef bool (*netfs_cb)(dir_entry_t *e, const char *lname, void *user);

bool netfs_mount(fat_vol_t *v);
void netfs_unmount(void);
bool netfs_iterate(uint16_t dir, netfs_cb cb, void *user); /* no . and .. */
bool netfs_open(fat_file_t *f, uint16_t dir, const char *name83);
bool netfs_create(fat_file_t *f, uint16_t dir, const char *name);
int netfs_read(fat_file_t *f, void *buf, int len);
int netfs_write(fat_file_t *f, const void *buf, int len);
bool netfs_seek(fat_file_t *f, uint32_t off);
bool netfs_close(fat_file_t *f);
bool netfs_mkdir(uint16_t dir, const char *name, uint16_t *out);
bool netfs_delete(uint16_t dir, const char *name83);
bool netfs_rmdir(uint16_t dir, const char *name83, bool all);
bool netfs_rename(uint16_t dir, const char *name83, uint16_t to_dir,
                  const char *new_name);

#endif
