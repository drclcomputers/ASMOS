#ifndef OS_API_H
#define OS_API_H

// Header for BUILT-IN apps (linked into the kernel). Dynamic .APP packages
// can't link against kernel symbols and use os/appapi.h instead — two headers
// on purpose.
// Apps include ONLY this header. It exposes everything an app legitimately needs while hiding kernel internals (ATA, PS/2 init, memory layout, etc.)
// if it's not in this header, apps don't touch it.

// Foundation Layer (Core types and utilities)
#include "lib/core.h"

// Memory & String Utilities
#include "lib/memory.h"
#include "lib/string.h"
#include "lib/math.h"

// Graphics & Visual
#include "lib/graphics.h"

// Device Control
#include "lib/time.h"
#include "drivers/serial.h"
#include "drivers/speaker.h"
#include "lib/cpu.h"

// Drivers support
#include "drivers/sb16.h"

// Input & Interaction
#include "drivers/keyboard.h"
#include "drivers/mouse.h"

// User Interface
#include "ui/ui.h"

// Storage & Filesystem
#include "fs/fs.h"
#include "fs/fdd_ctrl.h"

// OS Services
#include "os/clipboard.h"
#include "os/error.h"
#include "os/os.h"

// Shell / CLI / Assembler
#include "shell/cli.h"

extern bool window_is_focused(window *win);

#endif
