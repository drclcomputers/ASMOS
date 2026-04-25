#ifndef FAT16_H
#define FAT16_H

#include "fs/fs.h"

typedef fat_vol_t fat16_fs_t;
typedef fat_file_t fat16_file_t;
typedef fat_dir_ctx_t fat16_dir_context_t;

#define fat16_mount fs_mount
#define fat16_mount_drive fs_mount_drive
#define fat16_select_drive fs_select_drive
#define fat16_current_drive fs_current_drive
#define fat16_drive_label fs_drive_label
#define fat16_drive_mounted fs_drive_mounted
#define fat16_get_usage fs_get_usage

#define fat16_make_83 fs_make_83
#define fat16_resolve fs_resolve
#define fat16_chdir fs_chdir
#define fat16_pwd fs_pwd

#define fat16_mkdir fs_mkdir
#define fat16_find_in_dir fs_find_in_dir
#define fat16_find fs_find
#define fat16_list_dir fs_list_dir
#define is_dir_empty fs_is_dir_empty
#define fat16_wipe_cluster fs_wipe_cluster

#define fat16_open fs_open
#define fat16_create fs_create
#define fat16_read fs_read
#define fat16_write fs_write
#define fat16_seek fs_seek
#define fat16_tell fs_tell
#define fat16_close fs_close

#define fat16_rename fs_rename
#define fat16_delete fs_delete
#define fat16_rmdir fs_rmdir
#define fat16_rm_rf fs_rm_rf

#define fat16_copy_file fs_copy_file
#define fat16_move_file fs_move_file
#define fat16_copy_dir fs_copy_dir
#define fat16_move_dir fs_move_dir

#define fat16_set_hidden fs_set_hidden
#define fat16_is_hidden fs_is_hidden

#endif
