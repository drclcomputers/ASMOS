#ifndef FAT12_H
#define FAT12_H

#include "fs/fs.h"

typedef fat_vol_t fat12_fs_t;
typedef fat_file_t fat12_file_t;
typedef fat_dir_ctx_t fat12_dir_context_t;

#define fat12_mount fs_mount
#define fat12_mount_drive fs_mount_drive
#define fat12_select_drive fs_select_drive
#define fat12_current_drive fs_current_drive
#define fat12_drive_label fs_drive_label
#define fat12_drive_mounted fs_drive_mounted
#define fat12_get_usage fs_get_usage

#define fat12_make_83 fs_make_83
#define fat12_resolve fs_resolve
#define fat12_chdir fs_chdir
#define fat12_pwd fs_pwd

#define fat12_mkdir fs_mkdir
#define fat12_find_in_dir fs_find_in_dir
#define fat12_find fs_find
#define fat12_list_dir fs_list_dir
#define fat12_is_dir_empty fs_is_dir_empty
#define fat12_wipe_cluster fs_wipe_cluster

#define fat12_open fs_open
#define fat12_create fs_create
#define fat12_read fs_read
#define fat12_write fs_write
#define fat12_seek fs_seek
#define fat12_tell fs_tell
#define fat12_close fs_close

#define fat12_rename fs_rename
#define fat12_delete fs_delete
#define fat12_rmdir fs_rmdir
#define fat12_rm_rf fs_rm_rf

#define fat12_copy_file fs_copy_file
#define fat12_move_file fs_move_file
#define fat12_copy_dir fs_copy_dir
#define fat12_move_dir fs_move_dir

#define fat12_set_hidden fs_set_hidden
#define fat12_is_hidden fs_is_hidden

#endif
