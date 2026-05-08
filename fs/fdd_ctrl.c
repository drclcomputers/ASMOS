#include "fs/fdd_ctrl.h"
#include "fs/fs.h"

bool fdd_eject(uint8_t drive_id) {
    if (drive_id != DRIVE_FDD0 && drive_id != DRIVE_FDD1)
        return false;
    if (!fs_drive_mounted(drive_id))
        return false;
    fs_unmount(drive_id);
    return true;
}

bool fdd_insert(uint8_t drive_id) {
    if (drive_id != DRIVE_FDD0 && drive_id != DRIVE_FDD1)
        return false;
    if (fs_drive_mounted(drive_id))
        return false;
    return fs_mount_drive(drive_id);
}
