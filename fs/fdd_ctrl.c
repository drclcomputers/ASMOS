#include "fs/fdd_ctrl.h"
#include "drivers/fdd.h"
#include "fs/fs.h"

bool fdd_eject(uint8_t drive_id) {
    if (drive_id != DRIVE_FDD0 && drive_id != DRIVE_FDD1)
        return false;
    if (!fs_drive_mounted(drive_id))
        return false;
    fdd_flush(drive_id == DRIVE_FDD0 ? 0 : 1); /* what waits in memory */
    fs_unmount(drive_id);
    fdd_forget_media(drive_id == DRIVE_FDD0 ? 0
                                            : 1); /* next disk: look again */
    return true;
}

bool fdd_insert(uint8_t drive_id) {
    if (drive_id != DRIVE_FDD0 && drive_id != DRIVE_FDD1)
        return false;
    if (fs_drive_mounted(drive_id))
        return false;
    /* not found at boot (or the controller was stuck): look again */
    uint8_t unit = drive_id == DRIVE_FDD0 ? 0 : 1;
    if (!fdd_is_present(unit) && !fdd_init(unit))
        return false;
    fdd_forget_media(unit); /* a new disk may be another size */
    return fs_mount_drive(drive_id);
}
